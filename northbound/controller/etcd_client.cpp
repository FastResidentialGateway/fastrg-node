#include <common.h>
#include "etcd_client.h"
#include "config_snapshot.h"
#include "kafka_producer.h"
#include "../../src/dbg.h"
#include "../../src/fastrg.h"
#include "../../src/etcd_integration.h"
#include <etcd/Client.hpp>
#include <etcd/Watcher.hpp>
#include <etcd/Response.hpp>
#include <etcd/v3/action_constants.hpp>
#include <etcd/v3/Transaction.hpp>
#include <json/json.h>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <iostream>
#include <cstring>
#include <regex>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <mutex>
#include <fstream>

class EtcdClientImpl {
private:
    std::unique_ptr<etcd::Client> client_;
    std::unique_ptr<etcd::Watcher> hsi_watcher_;
    std::unique_ptr<etcd::Watcher> user_count_watcher_;
    std::unique_ptr<etcd::Watcher> dns_record_watcher_;
    std::atomic<bool> watch_running_;
    std::atomic<bool> shutting_down_{false};   // set once during teardown; blocks new reconnect attempts
    std::atomic<bool> etcd_reachable_{false};  // last-known etcd reachability (watchdog/init)
    std::string node_uuid_;
    std::string etcd_endpoints_;

    // Offline config-write queue (point 7). Entries are CLI config writes received
    // via node gRPC while etcd is unreachable; flushed to etcd on reconnect.
    
    // Watch/reconcile events are delivered to the control-plane loop via
    // FastRG_t.etcd_event_q; the apply-side callbacks are no longer stored
    // here. sync_request_callback_ is the one exception — Step 4 of
    // sync_state_with_etcd() still invokes it directly.
    sync_request_callback_t sync_request_callback_;
    FastRG_t* fastrg_ccb;

    // Track self-initiated status modifications to avoid processing our own updates
    struct PendingModification {
        U16 ccb_id;
        int64_t revision;
        std::chrono::steady_clock::time_point timestamp;
    };
    std::vector<PendingModification> pending_modifications_;
    std::mutex pending_modifications_mutex_;

    // Reconnection related members
    std::atomic<bool> reconnect_running_;
    std::thread reconnect_thread_;
    std::mutex reconnect_mutex_;
    std::condition_variable reconnect_cv_;
    static constexpr int INITIAL_RECONNECT_DELAY_MS = 1000;      // 1 second
    static constexpr int MAX_RECONNECT_DELAY_MS = 30000;         // 30 seconds
    static constexpr int RECONNECT_DELAY_MULTIPLIER = 2;

    // Watchdog related members
    std::atomic<std::chrono::steady_clock::time_point> last_watch_activity_;
    std::thread watchdog_thread_;
    std::atomic<bool> watchdog_running_;
    std::mutex watchdog_mutex_;
    std::condition_variable watchdog_cv_;
    static constexpr int WATCHDOG_CHECK_INTERVAL_SEC = 60;      // Check every 60 seconds
    static constexpr int WATCH_TIMEOUT_SEC = 180;               // Reconnect if no activity for 3 minutes

public:
    EtcdClientImpl() : watch_running_(false),
                       sync_request_callback_(nullptr),
                       fastrg_ccb(nullptr),
                       reconnect_running_(false), watchdog_running_(false) {
        last_watch_activity_.store(std::chrono::steady_clock::now());
    }

    ~EtcdClientImpl() {
        watch_running_ = false;
        reconnect_running_ = false;
        watchdog_running_ = false;

        reconnect_cv_.notify_all();
        watchdog_cv_.notify_all();

        stop_watch();
    }

    void stop_reconnect_thread() {
        if (reconnect_running_) {
            reconnect_running_ = false;
            reconnect_cv_.notify_all();
        }
        if (reconnect_thread_.joinable()) {
            reconnect_thread_.join();
        }
    }

    void update_watch_activity() {
        last_watch_activity_.store(std::chrono::steady_clock::now());
    }

    // Signal the watchdog loop to exit without joining it. Safe to call from
    // any thread, including the watchdog thread itself.
    void signal_watchdog_stop() {
        watchdog_running_ = false;
        watchdog_cv_.notify_all();
    }

    // Signal and join the watchdog thread. Must NOT be called from the
    // watchdog thread itself (would self-join). Only stop_watch() uses this.
    void stop_watchdog() {
        signal_watchdog_stop();
        if (watchdog_thread_.joinable()) {
            watchdog_thread_.join();
        }
    }

    bool test_etcd_connection() {
        if (!client_) {
            return false;
        }

        try {
            // Simple connection test with etcd-cpp-api
            auto response = client_->get("_watchdog_test_" + node_uuid_).get();

            // Any response (including key not found) means connection is OK
            if (response.error_code() == 0 || response.error_code() == 100) {
                return true;
            }

            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                "Etcd connection test failed: %s", response.error_message().c_str());
            return false;

        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                "Etcd connection test exception: %s", e.what());
            return false;
        }
    }

    void start_watchdog() {
        if (watchdog_running_) {
            return; // Already running
        }

        watchdog_running_ = true;
        update_watch_activity(); // Reset timer

        if (watchdog_thread_.joinable()) {
            watchdog_thread_.join();
        }

        watchdog_thread_ = std::thread([this]() {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Watchdog thread started");

            while (watchdog_running_ && watch_running_) {
                // Wait for check interval or stop signal
                {
                    std::unique_lock<std::mutex> lock(watchdog_mutex_);
                    if (watchdog_cv_.wait_for(lock, std::chrono::seconds(WATCHDOG_CHECK_INTERVAL_SEC),
                        [this]() { return !watchdog_running_ || !watch_running_; })) {
                        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Watchdog thread exiting due to stop signal");
                        break; // Stopped
                    }
                }

                if (!watchdog_running_ || !watch_running_) {
                    break;
                }

                // Test etcd reachability every tick. When reachable, reconcile local state
                // with etcd so we don't have to wait for a reconnect to recover from missed events.
                if (test_etcd_connection()) {
                    etcd_reachable_ = true;
                    // Report offline snapshot edits made while etcd was down,
                    // BEFORE reconciling, so the controller can arbitrate them
                    // before the reconcile refreshes the snapshot. When any
                    // report is still pending (transient read failure), skip
                    // the reconcile this tick — its mirror writes would clear
                    // the dirty flag and silently swallow the proposal; the
                    // next tick retries both.
                    if (kafka_report_offline_edits() == TRUE) {
                        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Watchdog: etcd reachable, syncing state with etcd...");
                        sync_state_with_etcd();
                    } else {
                        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                            "Watchdog: offline-edit report incomplete; deferring state sync to the next tick");
                    }
                    update_watch_activity();
                } else {
                    etcd_reachable_ = false;
                    // Not reachable — preserve existing tolerance: only trigger reconnect when
                    // watchers have also been inactive longer than WATCH_TIMEOUT_SEC.
                    auto now = std::chrono::steady_clock::now();
                    auto last_activity = last_watch_activity_.load();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity).count();

                    FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Watchdog: etcd not reachable, %ld s since last watch activity", elapsed);

                    if (elapsed > WATCH_TIMEOUT_SEC) {
                        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Watchdog: triggering reconnect");
                        // trigger_reconnect() only signals the watchdog (never joins
                        // it), so calling it directly from this thread is deadlock-free
                        // and needs no detached helper thread.
                        watchdog_running_ = false;
                        trigger_reconnect();
                    }
                }
            }

            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Watchdog thread exiting");
        });
    }

    etcd_status_t init(const char* etcd_endpoints, void* user_data) {
        try {
            fastrg_ccb = (FastRG_t *)user_data;
            etcd_endpoints_ = etcd_endpoints;  // Store endpoints for reconnection
            client_ = std::make_unique<etcd::Client>(etcd_endpoints);

            // Test connection by getting a simple key
            auto response_task = client_->get("test_connection");
            auto response = response_task.get(); // Get the actual response

            // Check if the operation was successful (connection works)
            if (response.error_code() != 0) {
                // Connection failed, but this might be expected if key doesn't exist
                // For connection test, we just need to ensure we can communicate
                if (response.error_code() == 100) { // Key not found is OK for connection test
                    etcd_reachable_ = true;
                    return ETCD_SUCCESS;
                }
                // etcd unreachable at startup: not fatal — the node boots from
                // the persisted snapshot. The client object is
                // constructed; the watchdog/reconnect machinery brings the
                // connection up when etcd returns.
                FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                    "etcd unreachable at init (%s); continuing on the local snapshot",
                    response.error_message().c_str());
                etcd_reachable_ = false;
                return ETCD_SUCCESS;
            }
            etcd_reachable_ = true;
            return ETCD_SUCCESS;
        } catch (const std::exception& e) {
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                "Exception during etcd initialization (%s); continuing on the local snapshot",
                e.what());
            etcd_reachable_ = false;
            return (client_ != nullptr) ? ETCD_SUCCESS : ETCD_ERROR;
        }
    }

    etcd_status_t start_watch(const char* node_uuid,
        sync_request_callback_t sync_request_callback) {

        if (!client_) {
            return ETCD_ERROR;
        }

        node_uuid_ = node_uuid;
        sync_request_callback_ = sync_request_callback;
        watch_running_ = true;

        // Probe etcd before creating any watcher: an etcd::Watcher built
        // against an unreachable endpoint constructs "successfully" but its
        // internal thread blocks in waitForResponse() forever and Cancel()
        // cannot stop it (etcd-cpp-apiv3 limitation) — hanging shutdown and
        // reconnection. Watchers must only ever be created right after a
        // successful probe (reconnect_loop already does the same).
        etcd_status_t s = ETCD_WATCH_FAILED;
        try {
            auto response = client_->get("test_connection").get();
            if (response.error_code() == 0 || response.error_code() == 100)
                s = create_watchers();
        } catch (const std::exception& e) {
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                "Etcd probe failed at startup: %s", e.what());
        }
        if (s != ETCD_SUCCESS) {
            // etcd unreachable at startup: not fatal — the node operates from
            // the persisted snapshot. Start the watchdog and signal a reconnect 
            // so the watchers come up once etcd returns.
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                "Etcd watchers could not start (etcd unreachable?); "
                "continuing on the local snapshot and retrying in the background");
            etcd_reachable_ = false;
            // Only spawn the reconnect loop; create_watchers() starts the
            // watchdog itself once the retry succeeds (same as the normal
            // flow — starting the watchdog here and immediately signalling it
            // from trigger_reconnect() deadlocks the thread bookkeeping).
            trigger_reconnect();
            return ETCD_SUCCESS;
        }
        return ETCD_SUCCESS;
    }

    // Create or recreate watchers - separated for reconnection support
    etcd_status_t create_watchers() {
        try {
            // Cancel existing watchers if any
            if (hsi_watcher_) {
                hsi_watcher_->Cancel();
                hsi_watcher_.reset();
            }

            if (user_count_watcher_) {
                user_count_watcher_->Cancel();
                user_count_watcher_.reset();
            }

            if (dns_record_watcher_) {
                dns_record_watcher_->Cancel();
                dns_record_watcher_.reset();
            }

            // Watch HSI configs: configs/{nodeId}/hsi/
            std::string hsi_prefix = "configs/" + node_uuid_ + "/hsi/";

            // Create HSI watcher with callback
            hsi_watcher_ = std::make_unique<etcd::Watcher>(
                *client_, 
                hsi_prefix,
                [this](etcd::Response response) {
                    if (!watch_running_) return;
                    
                    update_watch_activity(); // update activity time
                    
                    if (response.error_code() == 0) {
                        for(const auto& event : response.events()) {
                            process_hsi_event(event);
                        }
                    } else {
                        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "HSI watch error: %s", response.error_message().c_str());
                        // Trigger reconnection on watch error
                        trigger_reconnect();
                    }
                },
                [this](bool connected) {
                    update_watch_activity(); // update activity time
                    
                    // Connection status callback
                    if (!connected && watch_running_) {
                        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "HSI watcher disconnected, triggering reconnect...");
                        trigger_reconnect();
                    }
                },
                true  // recursive
            );

            // PPPoE dial/hangup is no longer driven by a commands/ key; it is
            // driven by config.desire_status on the HSI config watcher above.

            std::string user_count_prefix = "user_counts/" + node_uuid_ + "/";
            user_count_watcher_ = std::make_unique<etcd::Watcher>(
                *client_,
                user_count_prefix,
                [this](etcd::Response response) {
                    if (!watch_running_) return;

                    update_watch_activity(); // update activity time

                    if (response.error_code() == 0) {
                        for (const auto& event : response.events()) {
                            process_user_count_change_event(event);
                        }
                    } else {
                        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                            "User count watch error: %s", response.error_message().c_str());
                        // Trigger reconnection on watch error
                        trigger_reconnect();
                    }
                },
                [this](bool connected) {
                    update_watch_activity(); // update activity time
                    
                    // Connection status callback
                    if (!connected && watch_running_) {
                        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, 
                            "User count watcher disconnected, triggering reconnect...");
                        trigger_reconnect();
                    }
                },
                true  // recursive
            );

            // Watch DNS static records: configs/{nodeId}/dns/{subscriberId}
            std::string dns_prefix = "configs/" + node_uuid_ + "/dns/";
            dns_record_watcher_ = std::make_unique<etcd::Watcher>(
                *client_,
                dns_prefix,
                [this](etcd::Response response) {
                    if (!watch_running_) return;
                    update_watch_activity();
                    if (response.error_code() == 0) {
                        for (const auto& event : response.events()) {
                            process_dns_record_event(event);
                        }
                    } else {
                        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                            "DNS record watch error: %s", response.error_message().c_str());
                        trigger_reconnect();
                    }
                },
                [this](bool connected) {
                    update_watch_activity();
                    if (!connected && watch_running_) {
                        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                            "DNS record watcher disconnected, triggering reconnect...");
                        trigger_reconnect();
                    }
                },
                true  // recursive
            );

            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Etcd watchers started successfully");
            
            // start watchdog
            start_watchdog();
            
            return ETCD_SUCCESS;

        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Exception during watch setup: %s", e.what());
            return ETCD_WATCH_FAILED;
        }
    }

    // Trigger reconnection in a separate thread
    void trigger_reconnect() {
        // Never start a reconnect while the client is shutting down — the
        // resulting thread would outlive teardown and use freed memory.
        if (shutting_down_) {
            return;
        }

        std::lock_guard<std::mutex> lock(reconnect_mutex_);

        // Re-check under the lock: stop_watch() sets shutting_down_ and then
        // takes this same lock as a barrier, so once it has, any reconnect
        // started here is guaranteed visible to stop_reconnect_thread().
        if (shutting_down_) {
            return;
        }

        // Only start reconnect thread if not already running
        if (!reconnect_running_) {
            reconnect_running_ = true;

            // Entering reconnect means etcd is unreachable NOW. The watchdog
            // (the only other writer of this flag) is stopped below and stays
            // down until reconnection succeeds, so without this store the flag
            // would hold a stale `true` for the whole outage — keeping the
            // node gRPC SDN guard closed and the offline config queue
            // unreachable exactly when it is needed.
            etcd_reachable_ = false;

            // Only signal the watchdog to stop; never join it here. Joining is
            // done by stop_watch() (shutdown) or start_watchdog() (next cycle).
            // This keeps trigger_reconnect() safe to call from the watchdog
            // thread itself without self-joining.
            signal_watchdog_stop();

            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                "watch dog signalled, triggering etcd reconnection...");

            // Do NOT cancel the watchers here: trigger_reconnect() is often
            // invoked from a watcher's own disconnect callback (the watcher's
            // internal thread), and Watcher::Cancel() joins that thread —
            // self-join, std::system_error "Resource deadlock avoided".
            // create_watchers() cancels and resets the stale watchers instead,
            // and it only ever runs on the main or reconnect thread. Until
            // then the stale watchers are harmless: their callbacks hit the
            // reconnect_running_ guard above and no-op.

            // Start reconnection thread
            if (reconnect_thread_.joinable()) {
                reconnect_thread_.join();
            }
            reconnect_thread_ = std::thread(&EtcdClientImpl::reconnect_loop, this);
        }
    }

    // Reconnection loop with exponential backoff
    void reconnect_loop() {
        int delay_ms = INITIAL_RECONNECT_DELAY_MS;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
            "Etcd reconnection thread started: reconnect_running_ = %d, watch_running_ = %d", 
            reconnect_running_.load(), watch_running_.load());
        
        while (reconnect_running_ && watch_running_) {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
                "Attempting etcd reconnection in %d ms...", delay_ms);
            
            // Wait with the ability to be interrupted
            {
                std::unique_lock<std::mutex> lock(reconnect_mutex_);
                if (reconnect_cv_.wait_for(lock, std::chrono::milliseconds(delay_ms),
                    [this]() { return !reconnect_running_ || !watch_running_; })) {
                    // Condition became true, exit loop
                    break;
                }
            }
            
            if (!watch_running_ || !reconnect_running_) {
                break;
            }

            // Try to reconnect the client
            try {
                // Recreate the client
                client_ = std::make_unique<etcd::Client>(etcd_endpoints_);
                
                // Test connection
                auto response = client_->get("test_connection").get();
                
                // If we get here without exception, connection works
                // (error_code 100 = key not found is OK)
                if (response.error_code() == 0 || response.error_code() == 100) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Etcd reconnection successful, syncing state...");
                    
                    // Check again if we should still continue
                    if (!watch_running_ || !reconnect_running_) {
                        break;
                    }

                    // Same report-then-sync order as the watchdog tick: report
                    // dirty offline edits for controller arbitration first, so
                    // the reconcile below can refresh the snapshot afterwards.
                    // On a partial report (transient read failure) skip the
                    // reconcile — the mirror would swallow the pending
                    // proposal; the watchdog tick retries both within 60s.
                    if (kafka_report_offline_edits() == TRUE) {
                        // Sync state with etcd after reconnection
                        sync_state_with_etcd();
                    } else {
                        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                            "Reconnect: offline-edit report incomplete; deferring state sync to the watchdog");
                    }
                    
                    // Recreate watchers
                    if (create_watchers() == ETCD_SUCCESS) {
                        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Watchers recreated successfully after reconnection");
                        // Mark reachable immediately instead of waiting up to a
                        // full watchdog tick, so the SDN guard closes again as
                        // soon as etcd is actually back.
                        etcd_reachable_ = true;
                        reconnect_running_ = false;
                        break;
                    } else {
                        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to recreate watchers after reconnection");
                    }
                }
            } catch (const std::exception& e) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Etcd reconnection failed: %s", e.what());
            }
            
            // Exponential backoff
            delay_ms = std::min(delay_ms * RECONNECT_DELAY_MULTIPLIER, MAX_RECONNECT_DELAY_MS);
        }
        
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Reconnection loop exiting...");
        reconnect_running_ = false;
        reconnect_cv_.notify_all();
    }

    // Allocate a zeroed etcd_event_t of the given kind.
    static etcd_event_t *alloc_etcd_event(etcd_event_kind_t kind) {
        etcd_event_t *ev = (etcd_event_t *)calloc(1, sizeof(etcd_event_t));
        if (ev)
            ev->kind = kind;
        return ev;
    }

    // Heap copy of a std::string (caller owns; NULL on OOM).
    static char *dup_string(const std::string& s) {
        char *p = (char *)malloc(s.size() + 1);
        if (p)
            memcpy(p, s.c_str(), s.size() + 1);
        return p;
    }

    // Hand an etcd_event_t to the control-plane loop. Takes ownership: on
    // success the loop frees it; on failure (ring full / unavailable) it is
    // freed here. Returns true if enqueued.
    bool enqueue_etcd_event(etcd_event_t *ev) {
        if (!ev)
            return false;
        if (!fastrg_ccb || !fastrg_ccb->etcd_event_q ||
                rte_ring_enqueue(fastrg_ccb->etcd_event_q, ev) != 0) {
            FastRG_LOG(WARN, fastrg_ccb ? fastrg_ccb->fp : NULL, NULL, NULL,
                "etcd_event_q unavailable/full, dropping event (kind=%d)", ev->kind);
            etcd_event_free(ev);
            return false;
        }
        return true;
    }

    // Synchronize state with etcd after reconnection
    void sync_state_with_etcd() {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Starting state synchronization with etcd...");
        
        if (!client_) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot sync state: etcd client not initialized");
            return;
        }

        try {
            // Step 1: Check if subscriber count exists in etcd
            bool need_write_subscriber_count = false;
            U16 etcd_subscriber_count = 0;
            etcd_status_t sc_status = get_subscriber_count(node_uuid_.c_str(), &etcd_subscriber_count);
            
            if (sc_status == ETCD_KEY_NOT_FOUND) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Subscriber count not found in etcd, will request local sync");
                need_write_subscriber_count = true;
            } else if (sc_status == ETCD_SUCCESS) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Found subscriber count in etcd: %u", etcd_subscriber_count);
                // etcd has subscriber count, it will be loaded by load_existing_configs
            } else {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Error checking subscriber count in etcd");
            }

            // Step 2: Check if HSI configs exist in etcd
            bool need_write_hsi_configs = false;
            std::string hsi_prefix = "configs/" + node_uuid_ + "/hsi/";
            auto hsi_response = client_->ls(hsi_prefix).get();
            
            if (hsi_response.error_code() == 100) {
                // Key not found - no HSI configs in etcd
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "No HSI configs found in etcd, will request local sync");
                need_write_hsi_configs = true;
            } else if (hsi_response.error_code() == 0) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Found %zu HSI config(s) in etcd", hsi_response.keys().size());
                // etcd has HSI configs, they will be loaded by load_existing_configs
            } else {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Error checking HSI configs in etcd: %s", hsi_response.error_message().c_str());
            }

            // Step 3: Reconcile configs from etcd, skipping entries whose local state already matches.
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                "Reconciling configs from etcd for node: %s", node_uuid_.c_str());

            // Step 3a: user_counts — callback has its own dedupe (skips when new_count == current_count).
            std::string user_count_prefix = "user_counts/" + node_uuid_ + "/";
            auto user_count_response = client_->ls(user_count_prefix).get();
            if (user_count_response.error_code() == 0) {
                for (size_t i = 0; i < user_count_response.keys().size(); ++i) {
                    std::string value = user_count_response.value(i).as_string();
                    std::string count_node;
                    if (!extract_node_from_count_key(user_count_response.key(i), &count_node))
                        continue;
                    ingest_user_count_value(count_node, &value, HSI_ACTION_UPDATE,
                        user_count_response.index(), TRUE);
                }
            }

            // Step 3b: HSI configs — enqueue for the control-plane loop, and
            // collect the ccb_ids present in etcd for the sweep below.
            int hsi_total = 0;
            std::vector<int> present_ccb_ids;
            if (hsi_response.error_code() == 0) {
                for (size_t i = 0; i < hsi_response.keys().size(); ++i) {
                    std::string value = hsi_response.value(i).as_string();
                    std::string node_id, user_id;
                    if (!extract_ids_from_hsi_key(hsi_response.key(i), &node_id, &user_id))
                        continue;
                    hsi_total++;

                    int ccb_id = parse_user_id(user_id.c_str(), 0);
                    if (ccb_id >= 0)
                        present_ccb_ids.push_back(ccb_id);

                    ingest_hsi_value(node_id, user_id, &value, HSI_ACTION_UPDATE,
                        hsi_response.index(), TRUE);
                }
            }
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                "Sync: enqueued %d HSI config(s) for reconcile", hsi_total);

            // Step 3b-sweep: hand the loop the set of ccb_ids present in etcd so
            // it can remove subscribers active locally but no longer in etcd.
            if (hsi_response.error_code() == 0) {
                etcd_event_t *sweep = alloc_etcd_event(ETCD_EVENT_HSI_SWEEP);
                if (sweep) {
                    sweep->from_reconcile = TRUE;
                    sweep->event_data.sweep.count = (int)present_ccb_ids.size();
                    if (sweep->event_data.sweep.count > 0) {
                        sweep->event_data.sweep.present_ccb_ids =
                            (int *)malloc(sizeof(int) * sweep->event_data.sweep.count);
                        if (!sweep->event_data.sweep.present_ccb_ids) {
                            etcd_event_free(sweep);
                            sweep = nullptr;
                        }
                    }
                    if (sweep) {
                        for (int i = 0; i < sweep->event_data.sweep.count; ++i)
                            sweep->event_data.sweep.present_ccb_ids[i] = present_ccb_ids[(size_t)i];
                        enqueue_etcd_event(sweep);
                    }
                }
            }

            // Step 3c: DNS static records — enqueue for the control-plane loop.
            // Key format: configs/{nodeId}/dns/{userId}  value: JSON array of records
            {
                std::string dns_base_prefix = "configs/" + node_uuid_ + "/dns/";
                auto dns_response = client_->ls(dns_base_prefix).get();
                if (dns_response.error_code() == 0) {
                    int dns_total = 0;
                    for (size_t i = 0; i < dns_response.keys().size(); ++i) {
                        std::string key = dns_response.key(i);
                        std::string dns_node_id, dns_user_id;
                        if (!extract_ids_from_dns_key(key, &dns_node_id, &dns_user_id))
                            continue;
                        std::string dns_value = dns_response.value(i).as_string();

                        config_snapshot_watch_update(SNAPSHOT_KIND_DNS,
                            dns_user_id.c_str(), dns_value.c_str());

                        try {
                            Json::Value records;
                            if (!parse_dns_records_envelope(dns_value, &records))
                                continue;
                            for (const Json::Value& entry : records) {
                                dns_record_config_t rec;
                                if (!parse_dns_record_from_json(entry, &rec))
                                    continue;
                                etcd_event_t *ev = alloc_etcd_event(ETCD_EVENT_DNS_RECORD);
                                if (!ev)
                                    continue;
                                ev->action = HSI_ACTION_CREATE;
                                ev->revision = dns_response.index();
                                ev->from_reconcile = TRUE;
                                std::strncpy(ev->node_id, dns_node_id.c_str(), sizeof(ev->node_id) - 1);
                                std::strncpy(ev->user_id, dns_user_id.c_str(), sizeof(ev->user_id) - 1);
                                ev->event_data.dns_record = rec;
                                enqueue_etcd_event(ev);
                                dns_total++;
                            }
                        } catch (const std::exception& e) {
                            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                                "Sync: failed to parse DNS records for key %s: %s",
                                key.c_str(), e.what());
                        }
                    }
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                        "Sync: enqueued %d DNS record(s) for reconcile", dns_total);
                }
            }

            // Step 4: If etcd doesn't have data, request upper layer to write local data to etcd
            if ((need_write_subscriber_count || need_write_hsi_configs) && sync_request_callback_) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Requesting upper layer to sync local state to etcd...");
                sync_request_callback_(node_uuid_.c_str(), fastrg_ccb);
            }
            
        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Exception during state sync: %s", e.what());
        }
        
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "State synchronization completed");
    }

    void stop_watch() {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Stopping watch...");

        // Mark teardown first so any in-flight watch/connection callback that
        // calls trigger_reconnect() becomes a no-op and cannot spawn a thread
        // that would outlive cleanup.
        shutting_down_ = true;
        watch_running_ = false;

        // Barrier: after we have held reconnect_mutex_ with shutting_down_ set,
        // every trigger_reconnect() has either already finished (its thread is
        // visible to stop_reconnect_thread() below) or will observe
        // shutting_down_ and bail out. Lock is released immediately — never
        // held across a join — to stay deadlock-free.
        {
            std::lock_guard<std::mutex> lock(reconnect_mutex_);
        }

        // Stop the reconnect thread before the watchdog: the reconnect loop is
        // the only thing that can recreate the watchdog, so once it is joined
        // the watchdog can be joined without racing a restart.
        stop_reconnect_thread();

        // Now signal and join the watchdog.
        stop_watchdog();

        // Cancel and destroy every watcher. reset() blocks until the watcher's
        // in-flight callback returns, so after this no watch callback can run.
        if (hsi_watcher_) {
            hsi_watcher_->Cancel();
            hsi_watcher_.reset();
        }
        if (user_count_watcher_) {
            user_count_watcher_->Cancel();
            user_count_watcher_.reset();
        }
        if (dns_record_watcher_) {
            dns_record_watcher_->Cancel();
            dns_record_watcher_.reset();
        }

        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Etcd watchers stopped");
    }
    
    etcd_status_t delete_command(const char* command_key) {
        if (!client_) {
            return ETCD_ERROR;
        }

        try {
            auto response_task = client_->rm(command_key);
            auto response = response_task.get();

            if (response.error_code() == 0) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Deleted processed command: %s", command_key);
                return ETCD_SUCCESS;
            } else {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to delete command: %s", response.error_message().c_str());
                return ETCD_ERROR;
            }
        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Exception deleting command: %s", e.what());
            return ETCD_ERROR;
        }
    }


    // Build the HSIConfigWithMetadata JSON string for an hsi_config_t. Shared
    // by the offline-edit snapshot path so serialization stays identical to
    // the controller's schema. Metadata fields are placeholders; the snapshot
    // layer re-stamps them.
    std::string build_hsi_config_json(const char* node_id, const hsi_config_t* config,
        const char* updated_by) {
        Json::Value root;
        Json::Value cfg;
        cfg["user_id"] = std::string(config->user_id);
        cfg["vlan_id"] = std::string(config->vlan_id);
        cfg["account_name"] = std::string(config->account_name);
        cfg["password"] = std::string(config->password);
        cfg["dhcp_addr_pool"] = std::string(config->dhcp_addr_pool);
        cfg["dhcp_subnet"] = std::string(config->dhcp_subnet);
        cfg["dhcp_gateway"] = std::string(config->dhcp_gateway);
        cfg["dns_proxy_enable"] = (config->dns_proxy_enable == TRUE);
        cfg["tcp_conntrack_enable"] = (config->tcp_conntrack_enable == TRUE);
        // PPPoE desired state; default to disconnect when unset.
        cfg["desire_status"] = (config->desire_status[0] != '\0')
            ? std::string(config->desire_status) : std::string(DESIRE_STATUS_DISCONNECT);

        if (config->port_mapping_count > 0 && config->port_mappings != NULL) {
            Json::Value pm_array(Json::arrayValue);
            for (int i = 0; i < config->port_mapping_count; i++) {
                Json::Value entry;
                entry["index"] = std::to_string(i);
                entry["eport"] = std::to_string(config->port_mappings[i].eport);
                entry["dip"] = std::string(config->port_mappings[i].dip);
                entry["dport"] = std::to_string(config->port_mappings[i].dport);
                pm_array.append(entry);
            }
            cfg["port-mapping"] = pm_array;
        }

        root["config"] = cfg;
        Json::Value meta;
        meta["node"] = std::string(node_id);
        meta["resourceVersion"] = "";
        meta["updatedBy"] = updated_by ? std::string(updated_by) : std::string("");

        std::time_t now = std::time(nullptr);
        std::tm tm{};
        gmtime_r(&now, &tm);
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        meta["updatedAt"] = out.str();

        root["metadata"] = meta;

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        return Json::writeString(writer, root);
    }

    etcd_status_t delete_hsi_config(const char* node_id, const char* user_id, 
        int64_t* revision) {
        if (!client_ || !node_id || !user_id) return ETCD_ERROR;

        try {
            std::stringstream ss;
            ss << "configs/" << node_id << "/hsi/" << user_id;
            std::string key = ss.str();

            auto response_task = client_->rm(key);
            auto response = response_task.get();

            if (response.error_code() == 0) {
                // Capture the revision if output parameter provided
                if (revision) {
                    *revision = response.index();
                }
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Deleted HSI config: %s (revision: %lld)", key.c_str(), response.index());
                return ETCD_SUCCESS;
            } else {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to delete HSI config: %s", response.error_message().c_str());
                return ETCD_ERROR;
            }
        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Exception deleting HSI config: %s", e.what());
            return ETCD_ERROR;
        }
    }

    // ---- Offline config write queue (point 7) ----------------------------

    bool is_connected() const { return etcd_reachable_.load(); }

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }


    // ------------------ Offline-edit reconnect sync ------------------
    // For every dirty snapshot entry: read the etcd current value; when the
    // content differs (metadata excluded) report the full snapshot as a
    // ConfigOfflineEdit over Kafka for controller arbitration. The node never
    // writes etcd here. etcd read errors leave the entry dirty so the next
    // reconnect retries.
    // Raw single-key read for the offline-edit reporter: SUCCESS + malloc'd
    // value, KEY_NOT_FOUND (out stays NULL), or ETCD_ERROR on a transient
    // failure (caller keeps the entry dirty and retries later).
    etcd_status_t get_raw_value(const char *key, char **out_value) {
        if (!client_)
            return ETCD_ERROR;
        try {
            auto resp = client_->get(key).get();
            if (resp.error_code() == 0) {
                *out_value = strdup(resp.value().as_string().c_str());
                return *out_value ? ETCD_SUCCESS : ETCD_ERROR;
            }
            if (resp.error_code() == etcdv3::ERROR_KEY_NOT_FOUND)
                return ETCD_KEY_NOT_FOUND;
            return ETCD_ERROR;
        } catch (const std::exception&) {
            return ETCD_ERROR;
        }
    }

    etcd_status_t get_hsi_config(const std::string& node_id,
        const std::string& user_id, hsi_config_full_t* output)
    {
        if (!client_ || !output) {
            return ETCD_ERROR;
        }

        try {
            std::string key = "configs/" + node_id + "/hsi/" + user_id;

            // Get current config from etcd
            auto get_response = client_->get(key).get();

            if (get_response.error_code() != 0) {
                if (get_response.error_code() == 100) {
                    // Key not found
                    std::cerr << "HSI config not found for key: " << key << std::endl;
                    return ETCD_KEY_NOT_FOUND;
                }
                std::cerr << "Failed to get HSI config with key: " << key 
                    << " - " << get_response.error_message() << std::endl;
                return ETCD_ERROR;
            }

            std::string value = get_response.value().as_string();

            // Parse JSON
            Json::Value root;
            Json::Reader reader;

            if (!reader.parse(value, root)) {
                std::cerr << "Failed to parse HSI config JSON for get_hsi_config" << std::endl;
                return ETCD_CONFIG_PARSE_FAILED;
            }

            // Parse config section
            Json::Value config_obj;
            if (root.isMember("config")) {
                config_obj = root["config"];
            } else {
                config_obj = root; // Old format fallback
            }

            // Fill hsi_config_t part
            std::strncpy(output->config.user_id, 
                config_obj.get("user_id", "").asString().c_str(), 
                sizeof(output->config.user_id) - 1);
            std::strncpy(output->config.vlan_id, 
                config_obj.get("vlan_id", "").asString().c_str(), 
                sizeof(output->config.vlan_id) - 1);
            std::strncpy(output->config.account_name, 
                config_obj.get("account_name", "").asString().c_str(), 
                sizeof(output->config.account_name) - 1);
            std::strncpy(output->config.password, 
                config_obj.get("password", "").asString().c_str(), 
                sizeof(output->config.password) - 1);
            std::strncpy(output->config.dhcp_addr_pool, 
                config_obj.get("dhcp_addr_pool", "").asString().c_str(), 
                sizeof(output->config.dhcp_addr_pool) - 1);
            std::strncpy(output->config.dhcp_subnet, 
                config_obj.get("dhcp_subnet", "").asString().c_str(), 
                sizeof(output->config.dhcp_subnet) - 1);
            std::strncpy(output->config.dhcp_gateway, 
                config_obj.get("dhcp_gateway", "").asString().c_str(), 
                sizeof(output->config.dhcp_gateway) - 1);

            // dns_proxy_enable defaults to TRUE when absent in etcd
            output->config.dns_proxy_enable = TRUE;
            if (config_obj.isMember("dns_proxy_enable")) {
                const Json::Value& v = config_obj["dns_proxy_enable"];
                if (v.isBool())
                    output->config.dns_proxy_enable = v.asBool() ? TRUE : FALSE;
                else if (v.isString())
                    output->config.dns_proxy_enable = (v.asString() == "false") ? FALSE : TRUE;
                else if (v.isIntegral())
                    output->config.dns_proxy_enable = v.asInt() != 0 ? TRUE : FALSE;
            }

            // tcp_conntrack_enable defaults to TRUE when absent in etcd
            output->config.tcp_conntrack_enable = TRUE;
            if (config_obj.isMember("tcp_conntrack_enable")) {
                const Json::Value& v = config_obj["tcp_conntrack_enable"];
                if (v.isBool())
                    output->config.tcp_conntrack_enable = v.asBool() ? TRUE : FALSE;
                else if (v.isString())
                    output->config.tcp_conntrack_enable = (v.asString() == "false") ? FALSE : TRUE;
                else if (v.isIntegral())
                    output->config.tcp_conntrack_enable = v.asInt() != 0 ? TRUE : FALSE;
            }

            // desire_status: "connect"/"disconnect"; empty/absent treated as disconnect.
            memset(output->config.desire_status, 0, sizeof(output->config.desire_status));
            if (config_obj.isMember("desire_status") && config_obj["desire_status"].isString()) {
                std::strncpy(output->config.desire_status,
                    config_obj["desire_status"].asString().c_str(),
                    sizeof(output->config.desire_status) - 1);
            }

            // Parse port-mapping array (dynamic allocation — caller must call hsi_config_free_port_mappings)
            output->config.port_mappings = NULL;
            output->config.port_mapping_count = 0;
            if (config_obj.isMember("port-mapping") && config_obj["port-mapping"].isArray()) {
                const Json::Value& pm_array = config_obj["port-mapping"];
                int total = (int)pm_array.size();
                if (total > 0) {
                    output->config.port_mappings = (port_mapping_t *)malloc(total * sizeof(port_mapping_t));
                    if (output->config.port_mappings) {
                        for (int i = 0; i < total; i++) {
                            const Json::Value& entry = pm_array[i];
                            if (!entry.isMember("eport") || !entry.isMember("dip") || !entry.isMember("dport"))
                                continue;
                            port_mapping_t *pm = &output->config.port_mappings[output->config.port_mapping_count];
                            pm->eport = (U16)std::stoi(entry["eport"].asString());
                            std::strncpy(pm->dip, entry["dip"].asString().c_str(), sizeof(pm->dip) - 1);
                            pm->dip[sizeof(pm->dip) - 1] = '\0';
                            pm->dport = (U16)std::stoi(entry["dport"].asString());
                            output->config.port_mapping_count++;
                        }
                    }
                }
            }

            // Ensure null termination
            output->config.user_id[sizeof(output->config.user_id) - 1] = '\0';
            output->config.vlan_id[sizeof(output->config.vlan_id) - 1] = '\0';
            output->config.account_name[sizeof(output->config.account_name) - 1] = '\0';
            output->config.password[sizeof(output->config.password) - 1] = '\0';
            output->config.dhcp_addr_pool[sizeof(output->config.dhcp_addr_pool) - 1] = '\0';
            output->config.dhcp_subnet[sizeof(output->config.dhcp_subnet) - 1] = '\0';
            output->config.dhcp_gateway[sizeof(output->config.dhcp_gateway) - 1] = '\0';

            // Parse metadata section (enableStatus removed; PPPoE intent lives in
            // config.desire_status, observed status is reported via Kafka).
            if (root.isMember("metadata")) {
                Json::Value metadata = root["metadata"];

                std::strncpy(output->updated_by,
                    metadata.get("updatedBy", "").asString().c_str(),
                    sizeof(output->updated_by) - 1);
                output->updated_by[sizeof(output->updated_by) - 1] = '\0';

                std::strncpy(output->updated_at,
                    metadata.get("updatedAt", "").asString().c_str(),
                    sizeof(output->updated_at) - 1);
                output->updated_at[sizeof(output->updated_at) - 1] = '\0';

                std::strncpy(output->resource_version,
                    metadata.get("resourceVersion", "").asString().c_str(),
                    sizeof(output->resource_version) - 1);
                output->resource_version[sizeof(output->resource_version) - 1] = '\0';
            } else {
                // No metadata section, set defaults
                output->updated_by[0] = '\0';
                output->updated_at[0] = '\0';
                output->resource_version[0] = '\0';
            }

            std::cout << "Successfully retrieved HSI config for user: "
                << user_id << " (desire_status: " << output->config.desire_status << ")" << std::endl;

            return ETCD_SUCCESS;

        } catch (const std::exception& e) {
            std::cerr << "Exception getting HSI config: " << e.what() << std::endl;
            return ETCD_ERROR;
        }
    }

    etcd_status_t get_subscriber_count(const char* node_id, U16* subscriber_count) {
        if (!client_ || !node_id || !subscriber_count) {
            return ETCD_ERROR;
        }

        try {
            std::string key = "user_counts/" + std::string(node_id) + "/";

            // Get current subscriber count from etcd
            auto get_response = client_->get(key).get();

            if (get_response.error_code() != 0) {
                if (get_response.error_code() == 100) {
                    // Key not found
                    std::cerr << "Subscriber count not found for key: " << key << std::endl;
                    return ETCD_KEY_NOT_FOUND;
                }
                std::cerr << "Failed to get subscriber count with key: " << key 
                    << " - " << get_response.error_message() << std::endl;
                return ETCD_ERROR;
            }

            std::string value = get_response.value().as_string();

            // Parse JSON
            Json::Value root;
            Json::Reader reader;

            if (!reader.parse(value, root)) {
                std::cerr << "Failed to parse subscriber count JSON" << std::endl;
                return ETCD_CONFIG_PARSE_FAILED;
            }

            // Parse subscriber_count field
            if (!root.isMember("subscriber_count")) {
                std::cerr << "Missing 'subscriber_count' field" << std::endl;
                return ETCD_CONFIG_PARSE_FAILED;
            }

            std::string count_str = root["subscriber_count"].asString();
            *subscriber_count = static_cast<U16>(std::stoi(count_str));

            std::cout << "Successfully retrieved subscriber count: " << *subscriber_count << std::endl;
            return ETCD_SUCCESS;

        } catch (const std::exception& e) {
            std::cerr << "Exception getting subscriber count: " << e.what() << std::endl;
            return ETCD_ERROR;
        }
    }


    etcd_status_t load_existing_configs(const char* node_uuid,
        hsi_config_callback_t hsi_callback,
        user_count_changed_callback_t user_count_callback,
        dns_record_callback_t dns_record_callback,
        void* user_data) {
        if (!client_ || !node_uuid || !hsi_callback) {
            return ETCD_ERROR;
        }

        try {
            std::string user_count_prefix = "user_counts/" + std::string(node_uuid) + "/";
            auto user_count_response = client_->ls(user_count_prefix).get();
            if (user_count_response.error_code() == 0) {
                for(size_t i=0; i<user_count_response.keys().size(); ++i) {
                    std::string key = user_count_response.key(i);
                    std::string value = user_count_response.value(i).as_string();
                    std::string node_id;
                    if (!extract_node_from_count_key(key, &node_id)) {
                        std::cerr << "Invalid user count key format during load: " << key << std::endl;
                        continue;
                    }

                    user_count_config_t config;
                    if (parse_user_count_config(value, &config)) {
                        int64_t revision = user_count_response.index();
                        // Invoke user count changed callback
                        user_count_callback(node_id.c_str(), &config, HSI_ACTION_UPDATE, 
                            revision, user_data);
                        std::cout << "Loaded existing user count: " << 
                            config.user_count << std::endl;
                    }
                }
            } else if (user_count_response.error_code() != 100) {
                std::cerr << "Failed to load existing user counts: " 
                          << user_count_response.error_message() << std::endl;
            }

            std::string hsi_prefix = "configs/" + std::string(node_uuid) + "/hsi/";

            // Get all keys under the prefix
            auto response = client_->ls(hsi_prefix).get();

            if (response.error_code() != 0) {
                // If error is "key not found", it's OK (no existing configs)
                if (response.error_code() == 100) {
                    std::cout << "No existing HSI configs found for node: " << node_uuid << std::endl;
                    return ETCD_SUCCESS;
                }
                std::cerr << "Failed to load existing configs: " << response.error_message() << std::endl;
                return ETCD_ERROR;
            }

            // Process each key found
            int count = 0;
            for(size_t i=0; i<response.keys().size(); ++i) {
                std::string key = response.key(i);
                std::string value = response.value(i).as_string();

                std::string node_id, user_id;
                if (!extract_ids_from_hsi_key(key, &node_id, &user_id)) {
                    std::cerr << "Invalid HSI config key format during load: " << key << std::endl;
                    continue;
                }

                config_snapshot_watch_update(SNAPSHOT_KIND_HSI, user_id.c_str(),
                    value.c_str());

                // Parse HSI config from JSON
                hsi_config_t config;
                bool is_enabled = false;
                if (parse_hsi_config(value, &config, &is_enabled)) {
                    // Get the revision from response
                    int64_t revision = response.index();

                    // Invoke callback with UPDATE action for existing configs. The
                    // callback applies the config and reconciles PPPoE toward
                    // config.desire_status (dial when "connect"), so the node
                    // re-establishes sessions on restart from desire_status alone.
                    STATUS ret = hsi_callback(node_id.c_str(), user_id.c_str(), &config,
                        HSI_ACTION_UPDATE, revision, user_data);
                    if (ret == SUCCESS) {
                        count++;
                        std::cout << "Loaded existing HSI config for user: " << user_id
                            << " (desire_status: " << config.desire_status << ")" << std::endl;
                    } else {
                        std::cerr << "Failed to load HSI config for user: " << user_id << std::endl;
                    }
                } else {
                    std::cerr << "Failed to parse existing HSI config for user: " << user_id << std::endl;
                }
                hsi_config_free_port_mappings(&config);
            }

            std::cout << "Loaded " << count << " existing HSI config(s) for node: " << node_uuid << std::endl;

            // Load existing DNS static records:
            // key: configs/{nodeId}/dns/{userId}  value: JSON array of records
            if (dns_record_callback) {
                std::string dns_base_prefix = "configs/" + std::string(node_uuid) + "/dns/";
                auto dns_response = client_->ls(dns_base_prefix).get();
                if (dns_response.error_code() == 0) {
                    int dns_count = 0;
                    for (size_t i = 0; i < dns_response.keys().size(); ++i) {
                        std::string key = dns_response.key(i);
                        std::string dns_node_id, dns_user_id;
                        if (!extract_ids_from_dns_key(key, &dns_node_id, &dns_user_id))
                            continue;

                        std::string dns_value = dns_response.value(i).as_string();
                        int64_t dns_revision  = dns_response.index();

                        config_snapshot_watch_update(SNAPSHOT_KIND_DNS,
                            dns_user_id.c_str(), dns_value.c_str());

                        try {
                            Json::Value records;
                            if (!parse_dns_records_envelope(dns_value, &records)) {
                                std::cerr << "Failed to parse DNS records envelope during load: "
                                          << key << std::endl;
                                continue;
                            }
                            for (const Json::Value& entry : records) {
                                dns_record_config_t dns_rec;
                                if (!parse_dns_record_from_json(entry, &dns_rec))
                                    continue;

                                STATUS dns_ret = dns_record_callback(dns_node_id.c_str(),
                                    dns_user_id.c_str(), &dns_rec,
                                    HSI_ACTION_CREATE, dns_revision, user_data);
                                if (dns_ret == SUCCESS) {
                                    dns_count++;
                                    std::cout << "Loaded DNS record: " << dns_rec.domain
                                              << " for user " << dns_user_id << std::endl;
                                }
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "Exception parsing DNS records during load: "
                                      << e.what() << std::endl;
                        }
                    }
                    std::cout << "Loaded " << dns_count << " DNS static record(s) for node: "
                              << node_uuid << std::endl;
                } else if (dns_response.error_code() != 100) {
                    std::cerr << "Error querying DNS records during load: "
                              << dns_response.error_message() << std::endl;
                }
            }

            return ETCD_SUCCESS;

        } catch (const std::exception& e) {
            std::cerr << "Exception loading existing configs: " << e.what() << std::endl;
            return ETCD_ERROR;
        }
    }

    bool parse_hsi_config(const std::string& json_str, hsi_config_t* config, bool* is_enabled) {
        try {
            Json::Value root;
            Json::Reader reader;

            if (!reader.parse(json_str, root)) {
                std::cerr << "Failed to parse HSI config JSON: " << json_str << std::endl;
                return false;
            }

            // Check if this is the new format with metadata
            Json::Value config_obj;
            if (root.isMember("config")) {
                config_obj = root["config"];
            } else {
                config_obj = root; // Old format
            }

            // Extract HSI config fields
            strncpy(config->user_id, config_obj.get("user_id", "").asString().c_str(), 
                   sizeof(config->user_id) - 1);
            strncpy(config->vlan_id, config_obj.get("vlan_id", "").asString().c_str(), 
                   sizeof(config->vlan_id) - 1);
            strncpy(config->account_name, config_obj.get("account_name", "").asString().c_str(), 
                   sizeof(config->account_name) - 1);
            strncpy(config->password, config_obj.get("password", "").asString().c_str(), 
                   sizeof(config->password) - 1);
            strncpy(config->dhcp_addr_pool, config_obj.get("dhcp_addr_pool", "").asString().c_str(), 
                   sizeof(config->dhcp_addr_pool) - 1);
            strncpy(config->dhcp_subnet, config_obj.get("dhcp_subnet", "").asString().c_str(), 
                   sizeof(config->dhcp_subnet) - 1);
            strncpy(config->dhcp_gateway, config_obj.get("dhcp_gateway", "").asString().c_str(), 
                   sizeof(config->dhcp_gateway) - 1);

            // Ensure null termination
            config->user_id[sizeof(config->user_id) - 1] = '\0';
            config->vlan_id[sizeof(config->vlan_id) - 1] = '\0';
            config->account_name[sizeof(config->account_name) - 1] = '\0';
            config->password[sizeof(config->password) - 1] = '\0';
            config->dhcp_addr_pool[sizeof(config->dhcp_addr_pool) - 1] = '\0';
            config->dhcp_subnet[sizeof(config->dhcp_subnet) - 1] = '\0';
            config->dhcp_gateway[sizeof(config->dhcp_gateway) - 1] = '\0';

            // dns_proxy_enable defaults to TRUE when the field is absent in etcd
            config->dns_proxy_enable = TRUE;
            if (config_obj.isMember("dns_proxy_enable")) {
                const Json::Value& v = config_obj["dns_proxy_enable"];
                if (v.isBool())
                    config->dns_proxy_enable = v.asBool() ? TRUE : FALSE;
                else if (v.isString())
                    config->dns_proxy_enable = (v.asString() == "false") ? FALSE : TRUE;
                else if (v.isIntegral())
                    config->dns_proxy_enable = v.asInt() != 0 ? TRUE : FALSE;
            }

            // tcp_conntrack_enable defaults to TRUE when the field is absent in etcd
            config->tcp_conntrack_enable = TRUE;
            if (config_obj.isMember("tcp_conntrack_enable")) {
                const Json::Value& v = config_obj["tcp_conntrack_enable"];
                if (v.isBool())
                    config->tcp_conntrack_enable = v.asBool() ? TRUE : FALSE;
                else if (v.isString())
                    config->tcp_conntrack_enable = (v.asString() == "false") ? FALSE : TRUE;
                else if (v.isIntegral())
                    config->tcp_conntrack_enable = v.asInt() != 0 ? TRUE : FALSE;
            }

            // desire_status: "connect"/"disconnect"; empty/absent treated as disconnect.
            memset(config->desire_status, 0, sizeof(config->desire_status));
            if (config_obj.isMember("desire_status") && config_obj["desire_status"].isString()) {
                strncpy(config->desire_status, config_obj["desire_status"].asString().c_str(),
                    sizeof(config->desire_status) - 1);
            }

            // Parse port-mapping array (dynamic allocation — caller must call hsi_config_free_port_mappings)
            config->port_mappings = NULL;
            config->port_mapping_count = 0;
            if (config_obj.isMember("port-mapping") && config_obj["port-mapping"].isArray()) {
                const Json::Value& pm_array = config_obj["port-mapping"];
                int total = (int)pm_array.size();
                if (total > 0) {
                    config->port_mappings = (port_mapping_t *)malloc(total * sizeof(port_mapping_t));
                    if (config->port_mappings) {
                        for (int i = 0; i < total; i++) {
                            const Json::Value& entry = pm_array[i];
                            if (!entry.isMember("eport") || !entry.isMember("dip") || !entry.isMember("dport")) {
                                std::cerr << "Skipping port-mapping entry " << i << ": missing required fields" << std::endl;
                                continue;
                            }
                            port_mapping_t *pm = &config->port_mappings[config->port_mapping_count];
                            pm->eport = (U16)std::stoi(entry["eport"].asString());
                            strncpy(pm->dip, entry["dip"].asString().c_str(), sizeof(pm->dip) - 1);
                            pm->dip[sizeof(pm->dip) - 1] = '\0';
                            pm->dport = (U16)std::stoi(entry["dport"].asString());
                            config->port_mapping_count++;
                        }
                    }
                }
            }

            // "is_enabled" now means "the desired state is connect", derived from
            // config.desire_status (the only source of truth for PPPoE intent).
            if (is_enabled) {
                *is_enabled = (strcmp(config->desire_status, DESIRE_STATUS_CONNECT) == 0);
            }

            return true;

        } catch (const std::exception& e) {
            std::cerr << "Exception parsing HSI config: " << e.what() << std::endl;
            return false;
        }
    }

    etcd_status_t load_dns_records(const char *node_uuid, const char *user_id,
        dns_record_callback_t dns_record_callback, void *user_data) {

        if (!client_ || !node_uuid || !user_id || !dns_record_callback)
            return ETCD_ERROR;

        try {
            std::string key = std::string("configs/") + node_uuid + "/dns/" + user_id;

            auto response = client_->get(key).get();
            if (response.error_code() != 0) {
                if (response.error_code() == 100)
                    return ETCD_SUCCESS; // No records — not an error
                FastRG_LOG(ERR, fastrg_ccb ? fastrg_ccb->fp : nullptr, NULL, NULL,
                    "load_dns_records: get failed for key %s: %s",
                    key.c_str(), response.error_message().c_str());
                return ETCD_ERROR;
            }

            std::string value = response.value().as_string();
            config_snapshot_watch_update(SNAPSHOT_KIND_DNS, user_id, value.c_str());

            Json::Value records;
            if (!parse_dns_records_envelope(value, &records)) {
                FastRG_LOG(WARN, fastrg_ccb ? fastrg_ccb->fp : nullptr, NULL, NULL,
                    "load_dns_records: failed to parse records envelope for key %s", key.c_str());
                return ETCD_ERROR;
            }

            int loaded = 0;
            int64_t revision = response.index();
            for (const Json::Value& entry : records) {
                dns_record_config_t rec;
                if (!parse_dns_record_from_json(entry, &rec))
                    continue;

                /* load_dns_records runs on the control-plane thread (PPPoE
                 * session establishment), the same thread that applies queued
                 * etcd events, so calling the callback directly is race-free. */
                if (dns_record_callback(node_uuid, user_id, &rec,
                        HSI_ACTION_CREATE, revision, user_data) == SUCCESS)
                    loaded++;
            }

            FastRG_LOG(INFO, fastrg_ccb ? fastrg_ccb->fp : nullptr, NULL, NULL,
                "load_dns_records: loaded %d DNS record(s) for user %s",
                loaded, user_id);
            return ETCD_SUCCESS;

        } catch (const std::exception &e) {
            FastRG_LOG(ERR, fastrg_ccb ? fastrg_ccb->fp : nullptr, NULL, NULL,
                "load_dns_records: exception: %s", e.what());
            return ETCD_ERROR;
        }
    }

private:
    // configs/{nodeId}/hsi/{userId}
    static bool extract_ids_from_hsi_key(const std::string &key, std::string *node_id,
        std::string *user_id) {
        static const std::regex re("configs/([^/]+)/hsi/(.+)");
        std::smatch m;
        if (!std::regex_match(key, m, re) || m.size() != 3)
            return false;
        *node_id = m[1].str();
        *user_id = m[2].str();
        return true;
    }

    // configs/{nodeId}/dns/{subscriberId}
    static bool extract_ids_from_dns_key(const std::string &key, std::string *node_id,
        std::string *user_id) {
        static const std::regex re("configs/([^/]+)/dns/([^/]+)");
        std::smatch m;
        if (!std::regex_match(key, m, re) || m.size() != 3)
            return false;
        *node_id = m[1].str();
        *user_id = m[2].str();
        return true;
    }

    // user_counts/{nodeId}/
    static bool extract_node_from_count_key(const std::string &key, std::string *node_id) {
        static const std::regex re("user_counts/([^/]+)/");
        std::smatch m;
        if (!std::regex_match(key, m, re) || m.size() != 2)
            return false;
        *node_id = m[1].str();
        return true;
    }

    // DNS records are stored as {"records":[...],"metadata":{...}}
    static bool parse_dns_records_envelope(const std::string &value,
        Json::Value *records_out) {
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(value, root) || !root.isObject() ||
                !root.isMember("records") || !root["records"].isArray())
            return false;
        *records_out = root["records"];
        return true;
    }

    // One envelope entry {"domain","ip","ttl"} -> dns_record_config_t
    // (ttl defaults to 3600 when absent).
    static bool parse_dns_record_from_json(const Json::Value &entry,
        dns_record_config_t *rec) {
        if (!entry.isMember("domain") || !entry.isMember("ip"))
            return false;
        memset(rec, 0, sizeof(*rec));
        strncpy(rec->domain, entry["domain"].asString().c_str(), sizeof(rec->domain) - 1);
        strncpy(rec->ip, entry["ip"].asString().c_str(), sizeof(rec->ip) - 1);
        rec->ttl = entry.isMember("ttl") ? entry["ttl"].asUInt() : 3600;
        return true;
    }

    // Mirror + parse + enqueue one HSI config value for the control-plane
    // loop. Shared by the watch handler (live action, from_reconcile FALSE)
    // and the reconcile pass (UPDATE, from_reconcile TRUE); the boot load
    // delivers through direct callbacks instead and does not use this.
    // value == nullptr records a key deletion.
    STATUS ingest_hsi_value(const std::string &node_id, const std::string &user_id,
        const std::string *value, etcd_action_type_t action, int64_t revision,
        BOOL from_reconcile) {
        config_snapshot_watch_update(SNAPSHOT_KIND_HSI, user_id.c_str(),
            value ? value->c_str() : NULL);
        etcd_event_t *ev = alloc_etcd_event(ETCD_EVENT_HSI);
        if (!ev)
            return ERROR;
        ev->action = action;
        ev->revision = revision;
        ev->from_reconcile = from_reconcile;
        std::strncpy(ev->node_id, node_id.c_str(), sizeof(ev->node_id) - 1);
        std::strncpy(ev->user_id, user_id.c_str(), sizeof(ev->user_id) - 1);
        if (value) {
            bool is_enabled = false;
            if (!parse_hsi_config(*value, &ev->event_data.hsi.config, &is_enabled)) {
                std::cerr << "Failed to parse HSI config for user " << user_id << std::endl;
                etcd_event_free(ev);
                return ERROR;
            }
            ev->event_data.hsi.desire_connect = is_enabled ? TRUE : FALSE;
            // Carry metadata.resourceVersion for the apply-result report.
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(*value, root) && root.isMember("metadata") &&
                    root["metadata"]["resourceVersion"].isString()) {
                std::strncpy(ev->event_data.hsi.resource_version,
                    root["metadata"]["resourceVersion"].asString().c_str(),
                    sizeof(ev->event_data.hsi.resource_version) - 1);
            }
        }
        return enqueue_etcd_event(ev) ? SUCCESS : ERROR;
    }

    // Same pipeline for the per-node subscriber-count key.
    STATUS ingest_user_count_value(const std::string &node_id,
        const std::string *value, etcd_action_type_t action, int64_t revision,
        BOOL from_reconcile) {
        // Node-level key: the snapshot's count entry uses the fixed
        // placeholder user_id "0" (no subscriber dimension).
        config_snapshot_watch_update(SNAPSHOT_KIND_COUNT, "0",
            value ? value->c_str() : NULL);
        etcd_event_t *ev = alloc_etcd_event(ETCD_EVENT_USER_COUNT);
        if (!ev)
            return ERROR;
        ev->action = action;
        ev->revision = revision;
        ev->from_reconcile = from_reconcile;
        std::strncpy(ev->node_id, node_id.c_str(), sizeof(ev->node_id) - 1);
        if (value) {
            if (!parse_user_count_config(*value, &ev->event_data.user_count)) {
                std::cerr << "Failed to parse user count config: " << *value << std::endl;
                etcd_event_free(ev);
                return ERROR;
            }
        }
        return enqueue_etcd_event(ev) ? SUCCESS : ERROR;
    }

    // Watcher thread: parse, self-event-filter, and enqueue for the control loop.
    STATUS process_hsi_event(const etcd::Event& event) {
        std::cout << "Processing HSI event: " << event.kv().key() << std::endl;

        std::string key = event.kv().key();
        std::string value = event.kv().as_string();
        int64_t revision = event.kv().modified_index();

        // Extract user_id from key: configs/{nodeId}/hsi/{userId}
        std::regex hsi_regex("configs/([^/]+)/hsi/(.+)");
        std::smatch matches;
        if (!std::regex_match(key, matches, hsi_regex) || matches.size() != 3) {
            // Errors are reported to the controller via Kafka (slice 11); the etcd
            // failed_events/ namespace is removed.
            std::cerr << "Invalid HSI config key format: " << key << std::endl;
            return ERROR;
        }
        std::string node_id = matches[1].str();
        std::string user_id = matches[2].str();

        etcd_action_type_t action;
        switch (event.event_type()) {
            case etcd::Event::EventType::PUT:
                // Distinguish between CREATE and UPDATE by checking if prev_kv exists
                try {
                    action = (event.prev_kv().key().empty()) ? HSI_ACTION_CREATE : HSI_ACTION_UPDATE;
                } catch (...) {
                    // If prev_kv() throws or is not available, treat as CREATE
                    action = HSI_ACTION_CREATE;
                }
                break;
            case etcd::Event::EventType::DELETE_:
                action = HSI_ACTION_DELETE;
                break;
            default:
                return ERROR; // Ignore other event types
        }

        return ingest_hsi_value(node_id, user_id,
            (action == HSI_ACTION_DELETE) ? nullptr : &value,
            action, revision, FALSE);
    }

    // Return SUCCESS if this event is PUT and it must be removed after processed
    STATUS process_user_count_change_event(const etcd::Event& event) {
        std::cout << "Processing user count change event: " << event.kv().key() << std::endl;

        std::string key = event.kv().key();
        std::string value = event.kv().as_string();
        int64_t revision = event.kv().modified_index();

        std::string node_id;
        if (!extract_node_from_count_key(key, &node_id)) {
            std::cerr << "Invalid user count config key format: " << key << std::endl;
            return ERROR;
        }

        etcd_action_type_t action;
        switch (event.event_type()) {
            case etcd::Event::EventType::PUT:
                try {
                    action = (event.prev_kv().key().empty()) ? HSI_ACTION_CREATE : HSI_ACTION_UPDATE;
                } catch (...) {
                    action = HSI_ACTION_CREATE;
                }
                break;
            case etcd::Event::EventType::DELETE_:
                action = HSI_ACTION_DELETE;
                break;
            default:
                return ERROR;
        }

        return ingest_user_count_value(node_id,
            (action == HSI_ACTION_DELETE) ? nullptr : &value,
            action, revision, FALSE);
    }

    bool parse_user_count_config(const std::string& json_str, user_count_config_t* config) {
        try {
            Json::Value root;
            Json::Reader reader;

            if (!reader.parse(json_str, root)) {
                std::cerr << "Failed to parse user count config JSON: " << json_str << std::endl;
                return false;
            }

            // Extract subscriber_count field
            if (!root.isMember("subscriber_count")) {
                std::cerr << "Missing 'subscriber_count' field in user count config" << std::endl;
                return false;
            }

            config->user_count = std::stoi(root.get("subscriber_count", "").asString());

            // Validate user_count
            if (config->user_count <= 0) {
                std::cerr << "Invalid subscriber_count value: " << config->user_count << std::endl;
                return false;
            }

            std::cout << "Parsed user count config: subscriber_count=" << config->user_count << std::endl;
            return true;

        } catch (const std::exception& e) {
            std::cerr << "Exception parsing user count config: " << e.what() << std::endl;
            return false;
        }
    }

    // Watcher thread: parse, mirror, and enqueue for the control loop (same
    // family as process_hsi_event / process_user_count_change_event above).
    STATUS process_dns_record_event(const etcd::Event& event) {
        std::string key = event.kv().key();

        std::string node_id, user_id;
        if (!extract_ids_from_dns_key(key, &node_id, &user_id))
            return ERROR; // Not a DNS record key, skip silently

        if (node_id != node_uuid_) return ERROR; // Not for us

        int64_t revision = event.kv().modified_index();

        switch (event.event_type()) {
            case etcd::Event::EventType::PUT: {
                etcd_action_type_t action;
                try {
                    action = (event.prev_kv().key().empty()) ? HSI_ACTION_CREATE : HSI_ACTION_UPDATE;
                } catch (...) {
                    action = HSI_ACTION_CREATE;
                }
                std::string value = event.kv().as_string();
                config_snapshot_watch_update(SNAPSHOT_KIND_DNS, user_id.c_str(),
                    value.c_str());
                try {
                    Json::Value records;
                    if (!parse_dns_records_envelope(value, &records)) {
                        std::cerr << "Invalid DNS records envelope: " << key << std::endl;
                        return ERROR;
                    }
                    for (const Json::Value& entry : records) {
                        dns_record_config_t rec;
                        if (!parse_dns_record_from_json(entry, &rec))
                            continue;
                        etcd_event_t *ev = alloc_etcd_event(ETCD_EVENT_DNS_RECORD);
                        if (!ev)
                            continue;
                        ev->action = action;
                        ev->revision = revision;
                        ev->from_reconcile = FALSE;
                        std::strncpy(ev->node_id, node_id.c_str(), sizeof(ev->node_id) - 1);
                        std::strncpy(ev->user_id, user_id.c_str(), sizeof(ev->user_id) - 1);
                        ev->event_data.dns_record = rec;
                        enqueue_etcd_event(ev);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Exception parsing DNS records: " << e.what() << std::endl;
                    return ERROR;
                }
                break;
            }
            case etcd::Event::EventType::DELETE_: {
                config_snapshot_watch_update(SNAPSHOT_KIND_DNS, user_id.c_str(), NULL);

                // Parse prev_kv to emit per-record DELETE events
                std::string prev_value;
                try {
                    if (!event.prev_kv().key().empty())
                        prev_value = event.prev_kv().as_string();
                } catch (...) {}

                if (prev_value.empty())
                    return SUCCESS; // No prev_kv — cannot determine deleted records

                try {
                    Json::Value records;
                    if (!parse_dns_records_envelope(prev_value, &records))
                        return ERROR;
                    for (const Json::Value& entry : records) {
                        if (!entry.isMember("domain"))
                            continue;
                        etcd_event_t *ev = alloc_etcd_event(ETCD_EVENT_DNS_RECORD);
                        if (!ev)
                            continue;
                        ev->action = HSI_ACTION_DELETE;
                        ev->revision = revision;
                        ev->from_reconcile = FALSE;
                        std::strncpy(ev->node_id, node_id.c_str(), sizeof(ev->node_id) - 1);
                        std::strncpy(ev->user_id, user_id.c_str(), sizeof(ev->user_id) - 1);
                        std::strncpy(ev->event_data.dns_record.domain,
                            entry["domain"].asString().c_str(),
                            sizeof(ev->event_data.dns_record.domain) - 1);
                        enqueue_etcd_event(ev);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Exception parsing DNS records for delete: " << e.what() << std::endl;
                    return ERROR;
                }
                break;
            }
            default:
                return ERROR;
        }
        return SUCCESS;
    }

};

// Global instance
static std::unique_ptr<EtcdClientImpl> g_etcd_client = nullptr;

extern "C" {

etcd_status_t etcd_client_init(const char* etcd_endpoints, void* user_data) {
    try {
        g_etcd_client = std::make_unique<EtcdClientImpl>();
        return g_etcd_client->init(etcd_endpoints, user_data);
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize etcd client: " << e.what() << std::endl;
        return ETCD_ERROR;
    }
}

etcd_status_t etcd_client_start_watch(const char* node_uuid,
    sync_request_callback_t sync_request_callback) {

    if (!g_etcd_client) {
        return ETCD_ERROR;
    }
    return g_etcd_client->start_watch(node_uuid, sync_request_callback);
}

void etcd_client_stop_watch(void) {
    if (g_etcd_client) {
        g_etcd_client->stop_watch();
    }
}

etcd_status_t etcd_client_delete_command(const char* command_key) {
    if (!g_etcd_client) {
        return ETCD_ERROR;
    }
    return g_etcd_client->delete_command(command_key);
}


int etcd_client_is_initialized(void) {
    return (g_etcd_client != nullptr) ? 1 : 0;
}


int etcd_client_is_connected(void) {
    return (g_etcd_client && g_etcd_client->is_connected()) ? 1 : 0;
}


// Boot-time fallback: apply the persisted snapshot as the operating base when
// etcd is unreachable at startup. The subscriber count is applied first so HSI 
// configs land within range.
char *etcd_client_render_hsi_config(const char* node_id, const hsi_config_t* config) {
    if (!g_etcd_client || !node_id || !config) return NULL;
    std::string s = g_etcd_client->build_hsi_config_json(node_id, config,
        "fastrg-node-offline");
    return strdup(s.c_str());
}

STATUS etcd_client_parse_hsi_config(const char *value_json, hsi_config_t *out_config,
    BOOL *out_is_enabled) {
    if (!g_etcd_client || !value_json || !out_config)
        return ERROR;
    bool is_enabled = false;
    if (!g_etcd_client->parse_hsi_config(value_json, out_config, &is_enabled))
        return ERROR;
    if (out_is_enabled)
        *out_is_enabled = is_enabled == true? TRUE : FALSE;
    return SUCCESS;
}

etcd_status_t etcd_client_get_value(const char *key, char **out_value) {
    if (!g_etcd_client || !key || !out_value)
        return ETCD_ERROR;
    *out_value = NULL;
    return g_etcd_client->get_raw_value(key, out_value);
}

etcd_status_t etcd_client_get_hsi_config(const char* node_id,
    const char* user_id, hsi_config_full_t* output) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->get_hsi_config(std::string(node_id),
        std::string(user_id), output);
}

etcd_status_t etcd_client_delete_hsi_config(const char* node_id, const char* user_id,
    int64_t* revision) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->delete_hsi_config(node_id, user_id, revision);
}

etcd_status_t etcd_client_get_subscriber_count(const char* node_id,
    U16 *subscriber_count) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->get_subscriber_count(node_id, subscriber_count);
}

etcd_status_t etcd_client_load_existing_configs(const char* node_uuid,
    hsi_config_callback_t hsi_callback,
    user_count_changed_callback_t user_count_callback,
    dns_record_callback_t dns_record_callback,
    void* user_data) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->load_existing_configs(node_uuid, hsi_callback,
        user_count_callback, dns_record_callback, user_data);
}

void etcd_client_cleanup(void) {
    if (g_etcd_client) {
        g_etcd_client.reset();
    }
}



etcd_status_t etcd_client_load_dns_records(const char *node_uuid,
    const char *user_id,
    dns_record_callback_t dns_record_callback,
    void *user_data) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->load_dns_records(node_uuid, user_id,
        dns_record_callback, user_data);
}

} // extern "C"
