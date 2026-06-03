#include <common.h>
#include "etcd_client.h"
#include "../../src/dbg.h"
#include "../../src/fastrg.h"
#include "../../src/etcd_integration.h"
#include <etcd/Client.hpp>
#include <etcd/Watcher.hpp>
#include <etcd/Response.hpp>
#include <etcd/v3/action_constants.hpp>
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

/* Persisted offline config-write queue (point 7). Path mirrors CONFIG_DIR_PATH. */
#define CONFIG_QUEUE_PATH "/etc/fastrg/config_queue.json"

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
    struct QueueEntry {
        std::string op;        // "put" | "delete"
        std::string kind;      // "config" (HSI/count edit) | "desire" (desire_status change)
        std::string key;       // full etcd key
        std::string value;     // "config": JSON value; "desire": the status string
        int64_t     ts_ms;     // enqueue time (ms since epoch), for timestamp merge
        bool        preserve_desire;  // config put: keep etcd's existing desire_status
    };
    std::vector<QueueEntry> config_queue_;
    std::mutex config_queue_mutex_;
    
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
                    // Flush any CLI writes that were queued while etcd was down,
                    // BEFORE reconciling, so local (CLI-originated) intent is pushed
                    // to etcd first and then reconciled back.
                    flush_config_queue();
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Watchdog: etcd reachable, syncing state with etcd...");
                    sync_state_with_etcd();
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
                return ETCD_ERROR;
            }
            etcd_reachable_ = true;
            return ETCD_SUCCESS;
        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                "Exception during etcd initialization: %s", e.what());
            return ETCD_ERROR;
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

        return create_watchers();
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

            // Watch DNS static records: configs/{nodeId}/{subscriberId}/dns
            std::string dns_prefix = "configs/" + node_uuid_ + "/";
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

            // Only signal the watchdog to stop; never join it here. Joining is
            // done by stop_watch() (shutdown) or start_watchdog() (next cycle).
            // This keeps trigger_reconnect() safe to call from the watchdog
            // thread itself without self-joining.
            signal_watchdog_stop();

            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                "watch dog signalled, triggering etcd reconnection...");

            // Stop current watchers
            if (hsi_watcher_) {
                hsi_watcher_->Cancel();
            }
            if (user_count_watcher_) {
                user_count_watcher_->Cancel();
            }
            if (dns_record_watcher_) {
                dns_record_watcher_->Cancel();
            }

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
                    
                    // Sync state with etcd after reconnection
                    sync_state_with_etcd();
                    
                    // Recreate watchers
                    if (create_watchers() == ETCD_SUCCESS) {
                        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Watchers recreated successfully after reconnection");
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
                    std::string key = user_count_response.key(i);
                    std::string value = user_count_response.value(i).as_string();
                    std::regex user_count_regex("user_counts/([^/]+)/");
                    std::smatch matches;
                    if (!std::regex_match(key, matches, user_count_regex) || matches.size() != 2) {
                        continue;
                    }
                    etcd_event_t *ev = alloc_etcd_event(ETCD_EVENT_USER_COUNT);
                    if (!ev)
                        continue;
                    ev->action = HSI_ACTION_UPDATE;
                    ev->revision = user_count_response.index();
                    ev->from_reconcile = TRUE;
                    std::strncpy(ev->node_id, matches[1].str().c_str(), sizeof(ev->node_id) - 1);
                    if (!parse_user_count_config(value, &ev->event_data.user_count)) {
                        etcd_event_free(ev);
                        continue;
                    }
                    enqueue_etcd_event(ev);
                }
            }

            // Step 3b: HSI configs — enqueue for the control-plane loop, and
            // collect the ccb_ids present in etcd for the sweep below.
            int hsi_total = 0;
            std::vector<int> present_ccb_ids;
            if (hsi_response.error_code() == 0) {
                for (size_t i = 0; i < hsi_response.keys().size(); ++i) {
                    std::string key = hsi_response.key(i);
                    std::string value = hsi_response.value(i).as_string();
                    std::regex hsi_regex("configs/([^/]+)/hsi/(.+)");
                    std::smatch matches;
                    if (!std::regex_match(key, matches, hsi_regex) || matches.size() != 3)
                        continue;
                    std::string node_id = matches[1].str();
                    std::string user_id = matches[2].str();
                    hsi_total++;

                    int ccb_id = parse_user_id(user_id.c_str(), 0);
                    if (ccb_id >= 0)
                        present_ccb_ids.push_back(ccb_id);

                    etcd_event_t *ev = alloc_etcd_event(ETCD_EVENT_HSI);
                    if (!ev)
                        continue;
                    ev->action = HSI_ACTION_UPDATE;
                    ev->revision = hsi_response.index();
                    ev->from_reconcile = TRUE;
                    std::strncpy(ev->node_id, node_id.c_str(), sizeof(ev->node_id) - 1);
                    std::strncpy(ev->user_id, user_id.c_str(), sizeof(ev->user_id) - 1);
                    bool is_enabled = false;
                    if (!parse_hsi_config(value, &ev->event_data.hsi.config, &is_enabled)) {
                        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                            "Sync: failed to parse HSI config for user %s", user_id.c_str());
                        etcd_event_free(ev);
                        continue;
                    }
                    ev->event_data.hsi.desire_connect = is_enabled ? TRUE : FALSE;
                    enqueue_etcd_event(ev);
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
            // Key format: configs/{nodeId}/{userId}/dns  value: JSON array of records
            {
                std::string dns_base_prefix = "configs/" + node_uuid_ + "/";
                std::regex dns_key_regex("configs/([^/]+)/([^/]+)/dns");
                auto dns_response = client_->ls(dns_base_prefix).get();
                if (dns_response.error_code() == 0) {
                    int dns_total = 0;
                    for (size_t i = 0; i < dns_response.keys().size(); ++i) {
                        std::string key = dns_response.key(i);
                        std::smatch dns_matches;
                        if (!std::regex_match(key, dns_matches, dns_key_regex) || dns_matches.size() != 3)
                            continue;
                        std::string dns_node_id = dns_matches[1].str();
                        std::string dns_user_id = dns_matches[2].str();
                        std::string dns_value = dns_response.value(i).as_string();

                        try {
                            Json::Value records;
                            Json::Reader reader;
                            if (!reader.parse(dns_value, records) || !records.isArray())
                                continue;
                            for (const Json::Value& entry : records) {
                                if (!entry.isMember("domain") || !entry.isMember("ip"))
                                    continue;
                                etcd_event_t *ev = alloc_etcd_event(ETCD_EVENT_DNS_RECORD);
                                if (!ev)
                                    continue;
                                ev->action = HSI_ACTION_CREATE;
                                ev->revision = dns_response.index();
                                ev->from_reconcile = TRUE;
                                std::strncpy(ev->node_id, dns_node_id.c_str(), sizeof(ev->node_id) - 1);
                                std::strncpy(ev->user_id, dns_user_id.c_str(), sizeof(ev->user_id) - 1);
                                std::strncpy(ev->event_data.dns_record.domain,
                                    entry["domain"].asString().c_str(),
                                    sizeof(ev->event_data.dns_record.domain) - 1);
                                std::strncpy(ev->event_data.dns_record.ip,
                                    entry["ip"].asString().c_str(),
                                    sizeof(ev->event_data.dns_record.ip) - 1);
                                ev->event_data.dns_record.ttl =
                                    entry.isMember("ttl") ? entry["ttl"].asUInt() : 3600;
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

    // Write fallback error event to etcd for failed processing
    etcd_status_t write_fallback_error(const std::string& event_type,
        const std::string& key,
        const std::string& node_id,
        const std::string& user_id,
        etcd_error_reason_t reason,
        const std::string& error_detail,
        const std::string& original_value = "") {

        if (!client_) {
            return ETCD_ERROR;
        }

        try {
            // Build fallback error key: failed_events/{event_type}/{node_id}/{user_id}/{timestamp}
            std::time_t now = std::time(nullptr);
            std::stringstream ss;
            ss << "failed_events/" << event_type << "/" << node_id << "/" 
               << user_id << "/" << now;
            std::string fail_key = ss.str();

            // Build JSON payload
            Json::Value root;
            root["event_type"] = event_type;
            root["original_key"] = key;
            root["node_id"] = node_id;
            root["user_id"] = user_id;
            root["error_reason_code"] = static_cast<int>(reason);
            root["error_reason_name"] = get_error_reason_name(reason);
            root["error_detail"] = error_detail;
            root["timestamp"] = static_cast<Json::Int64>(now);

            // Record the instance's local UTC offset, e.g. "UTC+8". localtime()'s
            // shared static buffer is fine here — we only read the (effectively
            // constant) timezone offset, not the time itself.
            char tz_raw[8] = {0};
            struct tm *local_tm = std::localtime(&now);
            if (local_tm)
                std::strftime(tz_raw, sizeof(tz_raw), "%z", local_tm);  // e.g. "+0800"
            std::string tz_str = "UTC";
            if (std::strlen(tz_raw) >= 5) {
                int hh = (tz_raw[1] - '0') * 10 + (tz_raw[2] - '0');
                int mm = (tz_raw[3] - '0') * 10 + (tz_raw[4] - '0');
                tz_str += tz_raw[0];                       // '+' or '-'
                tz_str += std::to_string(hh);
                if (mm != 0) {
                    tz_str += ':';
                    if (mm < 10)
                        tz_str += '0';
                    tz_str += std::to_string(mm);
                }
            }
            root["timezone"] = tz_str;

            // Include original value if available (for DELETE events with prev_kv)
            if (!original_value.empty()) {
                root["original_value"] = original_value;
            }

            // Convert to string
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";  // Compact JSON
            std::string payload = Json::writeString(writer, root);

            // Write to etcd
            auto response_task = client_->set(fail_key, payload);
            auto response = response_task.get();

            if (response.error_code() == 0) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Wrote fallback error to: %s", fail_key.c_str());
                return ETCD_SUCCESS;
            } else {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to write fallback error: %s", response.error_message().c_str());
                return ETCD_ERROR;
            }
        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Exception writing fallback error: %s", e.what());
            return ETCD_ERROR;
        }
    }

    // Generic Compare-And-Swap put keyed on ModRevision.
    // See docs/contracts/cas-convention.md. Later slices (config writes,
    // desire_status updates, offline-queue flush) build on this primitive.
    etcd_status_t cas_put(const std::string& key, etcd_mutate_fn_t mutate_fn,
        void* user_data, int64_t* out_revision) {
        if (!client_ || key.empty() || !mutate_fn) return ETCD_ERROR;

        constexpr int CAS_MAX_RETRIES = 5;
        constexpr int CAS_BACKOFF_INIT_MS = 50;
        constexpr int CAS_BACKOFF_MULT = 2;
        int backoff_ms = CAS_BACKOFF_INIT_MS;

        for (int attempt = 0; attempt < CAS_MAX_RETRIES; attempt++) {
            try {
                // 1. Read current value + revision
                std::string current;
                int64_t mod_revision = 0;
                bool exists = false;

                auto get_resp = client_->get(key).get();
                if (get_resp.error_code() == 0) {
                    current = get_resp.value().as_string();
                    mod_revision = get_resp.value().modified_index();
                    exists = true;
                } else if (get_resp.error_code() == etcdv3::ERROR_KEY_NOT_FOUND) {
                    exists = false;   // key absent → create-if-absent below
                } else {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                        "cas_put: get failed for %s: %s", key.c_str(),
                        get_resp.error_message().c_str());
                    return ETCD_ERROR;
                }

                // 2. Produce new value via caller's mutate function
                char* out_value = NULL;
                if (mutate_fn(exists ? current.c_str() : NULL, &out_value, user_data) != SUCCESS
                        || out_value == NULL) {
                    free(out_value);
                    return ETCD_ERROR;
                }
                std::string new_value(out_value);
                free(out_value);

                // 3. Conditional write: CAS on revision, or create-if-absent
                etcd::Response write_resp = exists
                    ? client_->modify_if(key, new_value, mod_revision).get()
                    : client_->add(key, new_value).get();

                if (write_resp.error_code() == 0) {
                    if (out_revision) *out_revision = write_resp.index();
                    return ETCD_SUCCESS;
                }

                // 4. Conflict (compare failed / key already exists) → backoff + retry
                if (write_resp.error_code() == etcdv3::ERROR_COMPARE_FAILED ||
                    write_resp.error_code() == etcdv3::ERROR_KEY_ALREADY_EXISTS) {
                    FastRG_LOG(DBG, fastrg_ccb->fp, NULL, NULL,
                        "cas_put: conflict on %s (attempt %d/%d), retrying",
                        key.c_str(), attempt + 1, CAS_MAX_RETRIES);
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                    backoff_ms *= CAS_BACKOFF_MULT;
                    continue;
                }

                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                    "cas_put: write failed for %s: %s", key.c_str(),
                    write_resp.error_message().c_str());
                return ETCD_ERROR;
            } catch (const std::exception& e) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                    "cas_put: exception for %s: %s", key.c_str(), e.what());
                return ETCD_ERROR;
            }
        }

        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
            "cas_put: exhausted %d retries for %s", CAS_MAX_RETRIES, key.c_str());
        return ETCD_CAS_CONFLICT;
    }

    // Build the HSIConfigWithMetadata JSON string for an hsi_config_t. Shared by
    // put_hsi_config and the offline queue so both serialize identically.
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
        // enableStatus removed: observed PPPoE status is reported via Kafka, not etcd.

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

    // Put HSI config into etcd under configs/{nodeId}/hsi/{userId}
    etcd_status_t put_hsi_config(const char* node_id, const char* user_id,
        const hsi_config_t* config, const char* updated_by,
        int64_t* revision) {
        if (!client_ || !node_id || !user_id || !config) return ETCD_ERROR;

        try {
            std::stringstream ss;
            ss << "configs/" << node_id << "/hsi/" << user_id;
            std::string key = ss.str();

            std::string payload = build_hsi_config_json(node_id, config, updated_by);

            auto response_task = client_->set(key, payload);
            auto response = response_task.get();
            if (response.error_code() == 0) {
                if (revision)
                    *revision = response.index();
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Wrote HSI config to: %s (revision: %lld)",
                    key.c_str(), (long long)response.index());
                return ETCD_SUCCESS;
            } else {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to put HSI config: %s", response.error_message().c_str());
                return ETCD_ERROR;
            }
        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Exception putting HSI config: %s", e.what());
            return ETCD_ERROR;
        }
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

    // Parse an ISO8601 "YYYY-MM-DDTHH:MM:SSZ" timestamp to ms since epoch (0 on failure).
    static int64_t iso8601_to_ms(const std::string& s) {
        int y, mo, d, h, mi, sec;
        if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &sec) != 6)
            return 0;
        std::tm tm{};
        tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
        tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = sec;
        return (int64_t)timegm(&tm) * 1000;
    }

    // Merge context + callback for flushing a queued PUT via cas_put().
    struct FlushCtx {
        const std::string* value;   // config kind: JSON value; desire kind: status string
        int64_t ts_ms;              // queued enqueue time
        bool preserve_desire;       // config put: keep etcd's existing config.desire_status
        bool desire_only;           // desire kind: update only config.desire_status, keep config
        bool skipped;               // set when the queued entry should be dropped (etcd wins / N/A)
    };

    // Timestamp merge for flushing a queued write.
    //  - config kind: object-level merge; queued value wins unless etcd holds a
    //    newer write. For config edits etcd's desire_status is always preserved so
    //    a config edit never clobbers PPPoE intent.
    //  - desire kind (desire_only): keep etcd's whole config and update ONLY
    //    config.desire_status to the queued value, so a queued connect/disconnect
    //    never clobbers a concurrent config edit (symmetric to the config case).
    static STATUS flush_merge_fn(const char* current_json, char** out_value, void* user_data) {
        FlushCtx* ctx = (FlushCtx*)user_data;

        if (ctx->desire_only) {
            // Need an existing config to attach the desire_status to.
            if (current_json == NULL) { ctx->skipped = true; return ERROR; }
            Json::Value cur; Json::Reader reader;
            if (!reader.parse(current_json, cur) || !cur.isMember("config")) {
                ctx->skipped = true; return ERROR;
            }
            cur["config"]["desire_status"] = *ctx->value;   // value holds the status string
            Json::StreamWriterBuilder w; w["indentation"] = "";
            std::string s = Json::writeString(w, cur);
            *out_value = strdup(s.c_str());
            return *out_value ? SUCCESS : ERROR;
        }

        if (current_json == NULL) {
            *out_value = strdup(ctx->value->c_str());
            return *out_value ? SUCCESS : ERROR;
        }
        Json::Value cur; Json::Reader reader;
        if (!reader.parse(current_json, cur)) {
            *out_value = strdup(ctx->value->c_str());   // unparseable → overwrite
            return *out_value ? SUCCESS : ERROR;
        }
        int64_t etcd_ts = 0;
        if (cur.isMember("metadata") && cur["metadata"].isMember("updatedAt"))
            etcd_ts = iso8601_to_ms(cur["metadata"]["updatedAt"].asString());
        if (etcd_ts > ctx->ts_ms) {
            ctx->skipped = true;   // etcd newer → it wins; abort the CAS
            return ERROR;
        }
        Json::Value newv; Json::Reader r2;
        if (!r2.parse(*ctx->value, newv)) { ctx->skipped = true; return ERROR; }
        if (ctx->preserve_desire && cur.isMember("config") &&
                cur["config"].isMember("desire_status") && newv.isMember("config")) {
            newv["config"]["desire_status"] = cur["config"]["desire_status"];
        }
        Json::StreamWriterBuilder w; w["indentation"] = "";
        std::string s = Json::writeString(w, newv);
        *out_value = strdup(s.c_str());
        return *out_value ? SUCCESS : ERROR;
    }

    void persist_queue_locked() {
        Json::Value arr(Json::arrayValue);
        for (const auto& e : config_queue_) {
            Json::Value j;
            j["op"] = e.op;
            j["kind"] = e.kind;
            j["key"] = e.key;
            j["value"] = e.value;
            j["ts_ms"] = (Json::Int64)e.ts_ms;
            j["preserve_desire"] = e.preserve_desire;
            arr.append(j);
        }
        Json::StreamWriterBuilder w; w["indentation"] = "";
        std::string data = Json::writeString(w, arr);
        std::string tmp = std::string(CONFIG_QUEUE_PATH) + ".tmp";
        std::ofstream ofs(tmp, std::ios::trunc);
        if (!ofs) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to open queue temp file %s", tmp.c_str());
            return;
        }
        ofs << data;
        ofs.flush();
        ofs.close();
        if (std::rename(tmp.c_str(), CONFIG_QUEUE_PATH) != 0)
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to rename queue file to %s", CONFIG_QUEUE_PATH);
    }

    etcd_status_t load_config_queue() {
        std::lock_guard<std::mutex> lk(config_queue_mutex_);
        config_queue_.clear();
        std::ifstream ifs(CONFIG_QUEUE_PATH);
        if (!ifs) return ETCD_SUCCESS;   // no file = empty queue
        std::stringstream buf; buf << ifs.rdbuf();
        std::string data = buf.str();
        if (data.empty()) return ETCD_SUCCESS;
        Json::Value arr; Json::Reader reader;
        if (!reader.parse(data, arr) || !arr.isArray()) {
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Corrupt config queue file, ignoring");
            return ETCD_ERROR;
        }
        for (const auto& j : arr) {
            QueueEntry e;
            e.op = j.get("op", "").asString();
            e.kind = j.get("kind", "config").asString();
            e.key = j.get("key", "").asString();
            e.value = j.get("value", "").asString();
            e.ts_ms = j.get("ts_ms", (Json::Int64)0).asInt64();
            e.preserve_desire = j.get("preserve_desire", false).asBool();
            if (!e.op.empty() && !e.key.empty())
                config_queue_.push_back(e);
        }
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Loaded %zu queued config write(s) from disk", config_queue_.size());
        return ETCD_SUCCESS;
    }

    void enqueue_locked(const QueueEntry& e) {
        config_queue_.push_back(e);
        persist_queue_locked();
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Queued offline config write: op=%s key=%s (pending=%zu)",
            e.op.c_str(), e.key.c_str(), config_queue_.size());
    }

    etcd_status_t queue_hsi_put(const char* node_id, const char* user_id,
        const hsi_config_t* config, const char* updated_by) {
        if (!node_id || !user_id || !config) return ETCD_ERROR;
        QueueEntry e;
        e.op = "put";
        e.kind = "config";
        e.key = "configs/" + std::string(node_id) + "/hsi/" + std::string(user_id);
        e.value = build_hsi_config_json(node_id, config, updated_by);
        e.ts_ms = now_ms();
        e.preserve_desire = true;   // config edit must not clobber etcd desire_status
        std::lock_guard<std::mutex> lk(config_queue_mutex_);
        enqueue_locked(e);
        return ETCD_SUCCESS;
    }

    etcd_status_t queue_hsi_delete(const char* node_id, const char* user_id) {
        if (!node_id || !user_id) return ETCD_ERROR;
        QueueEntry e;
        e.op = "delete";
        e.kind = "config";
        e.key = "configs/" + std::string(node_id) + "/hsi/" + std::string(user_id);
        e.ts_ms = now_ms();
        e.preserve_desire = false;
        std::lock_guard<std::mutex> lk(config_queue_mutex_);
        enqueue_locked(e);
        return ETCD_SUCCESS;
    }

    etcd_status_t queue_desire_status(const char* node_id, const char* user_id,
        const char* desire_status) {
        if (!node_id || !user_id || !desire_status) return ETCD_ERROR;
        QueueEntry e;
        e.op = "put";
        e.kind = "desire";   // flush updates only config.desire_status, preserving config
        e.key = "configs/" + std::string(node_id) + "/hsi/" + std::string(user_id);
        e.value = desire_status;   // the status string
        e.ts_ms = now_ms();
        e.preserve_desire = false;
        std::lock_guard<std::mutex> lk(config_queue_mutex_);
        enqueue_locked(e);
        return ETCD_SUCCESS;
    }

    etcd_status_t queue_subscriber_count(const char* node_id,
        const char* count_str, const char* updated_by) {
        if (!node_id || !count_str) return ETCD_ERROR;
        Json::Value root;
        root["subscriber_count"] = std::string(count_str);
        Json::Value meta;
        meta["node"] = std::string(node_id);
        meta["resourceVersion"] = "";
        meta["updatedBy"] = updated_by ? std::string(updated_by) : std::string("");
        std::time_t now = std::time(nullptr); std::tm tm{}; gmtime_r(&now, &tm);
        std::ostringstream out; out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        meta["updatedAt"] = out.str();
        root["metadata"] = meta;
        Json::StreamWriterBuilder w; w["indentation"] = "";

        QueueEntry e;
        e.op = "put";
        e.kind = "config";
        e.key = "user_counts/" + std::string(node_id) + "/";
        e.value = Json::writeString(w, root);
        e.ts_ms = now_ms();
        e.preserve_desire = false;
        std::lock_guard<std::mutex> lk(config_queue_mutex_);
        enqueue_locked(e);
        return ETCD_SUCCESS;
    }

    int queue_pending() {
        std::lock_guard<std::mutex> lk(config_queue_mutex_);
        return (int)config_queue_.size();
    }

    // Flush queued writes to etcd. Called by the watchdog when etcd is reachable.
    // etcd_reachable_ is already true here, so the gRPC server rejects (does not
    // enqueue) concurrent writes — holding the queue lock for the flush is safe.
    void flush_config_queue() {
        std::lock_guard<std::mutex> lk(config_queue_mutex_);
        if (config_queue_.empty()) return;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Flushing %zu queued config write(s) to etcd", config_queue_.size());

        std::vector<QueueEntry> remaining;
        for (auto& e : config_queue_) {
            bool resolved = false;

            // For HSI keys, recover ccb_id to drive self-event filtering so the
            // node's own watcher does not re-process this flush write.
            int ccb_id = -1;
            std::smatch m;
            std::regex re("configs/[^/]+/hsi/(.+)");
            if (std::regex_match(e.key, m, re) && m.size() == 2)
                ccb_id = parse_user_id(m[1].str().c_str(), 0);

            if (e.op == "put") {
                FlushCtx ctx{ &e.value, e.ts_ms, e.preserve_desire, (e.kind == "desire"), false };
                if (ccb_id >= 0) etcd_mark_pending_event(HSI_ACTION_UPDATE, (U16)ccb_id);
                int64_t rev = 0;
                etcd_status_t s = cas_put(e.key, &EtcdClientImpl::flush_merge_fn, &ctx, &rev);
                if (s == ETCD_SUCCESS) {
                    if (ccb_id >= 0) etcd_confirm_pending_event(HSI_ACTION_UPDATE, (U16)ccb_id, rev);
                    resolved = true;
                } else if (ctx.skipped) {
                    if (ccb_id >= 0) etcd_remove_event(HSI_ACTION_UPDATE, (U16)ccb_id);
                    resolved = true;   // etcd newer → drop queued write
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                        "Queued write for %s superseded by newer etcd value, dropping", e.key.c_str());
                } else {
                    if (ccb_id >= 0) etcd_remove_event(HSI_ACTION_UPDATE, (U16)ccb_id);
                    FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                        "Failed to flush queued write for %s, will retry", e.key.c_str());
                }
            } else if (e.op == "delete") {
                try {
                    auto resp = client_->get(e.key).get();
                    if (resp.error_code() == etcdv3::ERROR_KEY_NOT_FOUND) {
                        resolved = true;   // already gone
                    } else if (resp.error_code() == 0) {
                        int64_t etcd_ts = 0;
                        Json::Value cur; Json::Reader r;
                        if (r.parse(resp.value().as_string(), cur) &&
                                cur.isMember("metadata") && cur["metadata"].isMember("updatedAt"))
                            etcd_ts = iso8601_to_ms(cur["metadata"]["updatedAt"].asString());
                        if (etcd_ts > e.ts_ms) {
                            resolved = true;  // re-created/updated after our delete → keep etcd
                        } else {
                            if (ccb_id >= 0) etcd_mark_pending_event(HSI_ACTION_DELETE, (U16)ccb_id);
                            auto del = client_->rm(e.key).get();
                            if (del.error_code() == 0) {
                                if (ccb_id >= 0) etcd_confirm_pending_event(HSI_ACTION_DELETE, (U16)ccb_id, del.index());
                                resolved = true;
                            } else if (ccb_id >= 0) {
                                etcd_remove_event(HSI_ACTION_DELETE, (U16)ccb_id);
                            }
                        }
                    }
                } catch (const std::exception& ex) {
                    FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                        "Exception flushing delete %s: %s", e.key.c_str(), ex.what());
                }
            }

            if (!resolved) remaining.push_back(e);
        }
        config_queue_.swap(remaining);
        persist_queue_locked();
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Config queue flush complete, %zu entr(ies) still pending", config_queue_.size());
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

    etcd_status_t put_subscriber_count(const char* node_id, 
        const char* subscriber_count_str, const char* updated_by) {
        if (!client_ || !node_id || !subscriber_count_str) return ETCD_ERROR;

        try {
            std::stringstream ss;
            ss << "user_counts/" << node_id << "/";
            std::string key = ss.str();

            Json::Value root;
            root["subscriber_count"] = std::string(subscriber_count_str);
            Json::Value meta;
            meta["node"] = std::string(node_id);
            meta["resourceVersion"] = "";
            meta["updatedBy"] = updated_by ? std::string(updated_by) : std::string("");

            // ISO8601-ish timestamp
            std::time_t now = std::time(nullptr);
            std::tm tm{};
            gmtime_r(&now, &tm);
            std::ostringstream out;
            out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            meta["updatedAt"] = out.str();

            root["metadata"] = meta;

            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            std::string payload = Json::writeString(writer, root);

            auto response_task = client_->set(key, payload);
            auto response = response_task.get();
            if (response.error_code() == 0) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Wrote subscriber count to: %s", key.c_str());
                return ETCD_SUCCESS;
            } else {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to put subscriber count: %s", response.error_message().c_str());
                return ETCD_ERROR;
            }
        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Exception putting subscriber count: %s", e.what());
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

    // Convert error reason enum to readable string
    static std::string get_error_reason_name(etcd_error_reason_t reason) {
        switch (reason) {
            case ERROR_REASON_CALLBACK_FAILED:
                return "CALLBACK_FAILED";
            case ERROR_REASON_PARSE_FAILED:
                return "PARSE_FAILED";
            case ERROR_REASON_INVALID_FORMAT:
                return "INVALID_FORMAT";
            case ERROR_REASON_MISSING_FIELD:
                return "MISSING_FIELD";
            case ERROR_REASON_RESOURCE_UNAVAILABLE:
                return "RESOURCE_UNAVAILABLE";
            case ERROR_REASON_TIMEOUT:
                return "TIMEOUT";
            case ERROR_REASON_UNKNOWN:
            default:
                return "UNKNOWN";
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

                    // Extract user_id from key: user_counts/{nodeId}/{userId}
                    std::regex user_count_regex("user_counts/([^/]+)/");
                    std::smatch matches;

                    if (!std::regex_match(key, matches, user_count_regex) || matches.size() != 2) {
                        std::cerr << "Invalid user count key format during load: " << key << std::endl;
                        continue;
                    }

                    std::string node_id = matches[1].str();

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

                // Extract user_id from key: configs/{nodeId}/hsi/{userId}
                std::regex hsi_regex("configs/([^/]+)/hsi/(.+)");
                std::smatch matches;

                if (!std::regex_match(key, matches, hsi_regex) || matches.size() != 3) {
                    std::cerr << "Invalid HSI config key format during load: " << key << std::endl;
                    continue;
                }

                std::string node_id = matches[1].str();
                std::string user_id = matches[2].str();

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
            // key: configs/{nodeId}/{userId}/dns  value: JSON array of records
            if (dns_record_callback) {
                std::string dns_base_prefix = "configs/" + std::string(node_uuid) + "/";
                std::regex dns_key_regex("configs/([^/]+)/([^/]+)/dns");
                auto dns_response = client_->ls(dns_base_prefix).get();
                if (dns_response.error_code() == 0) {
                    int dns_count = 0;
                    for (size_t i = 0; i < dns_response.keys().size(); ++i) {
                        std::string key = dns_response.key(i);
                        std::smatch dns_matches;
                        if (!std::regex_match(key, dns_matches, dns_key_regex) || dns_matches.size() != 3)
                            continue;

                        std::string dns_node_id = dns_matches[1].str();
                        std::string dns_user_id = dns_matches[2].str();
                        std::string dns_value   = dns_response.value(i).as_string();
                        int64_t dns_revision    = dns_response.index();

                        try {
                            Json::Value records;
                            Json::Reader reader;
                            if (!reader.parse(dns_value, records) || !records.isArray()) {
                                std::cerr << "Failed to parse DNS records JSON array during load: "
                                          << key << std::endl;
                                continue;
                            }
                            for (const Json::Value& entry : records) {
                                if (!entry.isMember("domain") || !entry.isMember("ip"))
                                    continue;
                                dns_record_config_t dns_rec;
                                memset(&dns_rec, 0, sizeof(dns_rec));
                                strncpy(dns_rec.domain, entry["domain"].asString().c_str(),
                                    sizeof(dns_rec.domain) - 1);
                                strncpy(dns_rec.ip, entry["ip"].asString().c_str(),
                                    sizeof(dns_rec.ip) - 1);
                                dns_rec.ttl = entry.isMember("ttl") ? entry["ttl"].asUInt() : 3600;

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

private:
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

        // Self-event filtering runs on the watcher thread so the slow retry in
        // etcd_is_self_event() never blocks the control-plane loop.
        etcd_action_type_t self_key =
            (action == HSI_ACTION_DELETE) ? HSI_ACTION_DELETE : HSI_ACTION_UPDATE;
        int ccb_id = parse_user_id(user_id.c_str(), 0);
        if (ccb_id >= 0 && etcd_is_self_event(self_key, (U16)ccb_id, revision)) {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                "Ignoring self-initiated HSI event for user %s (revision %ld)",
                user_id.c_str(), (long)revision);
            return SUCCESS;
        }

        etcd_event_t *ev = alloc_etcd_event(ETCD_EVENT_HSI);
        if (!ev)
            return ERROR;
        ev->action = action;
        ev->revision = revision;
        ev->from_reconcile = FALSE;
        std::strncpy(ev->node_id, node_id.c_str(), sizeof(ev->node_id) - 1);
        std::strncpy(ev->user_id, user_id.c_str(), sizeof(ev->user_id) - 1);

        if (action != HSI_ACTION_DELETE) {
            bool is_enabled = false;
            if (!parse_hsi_config(value, &ev->event_data.hsi.config, &is_enabled)) {
                // Parse errors are reported via Kafka (slice 11).
                std::cerr << "Failed to parse HSI config: " << value << std::endl;
                etcd_event_free(ev);
                return ERROR;
            }
            ev->event_data.hsi.desire_connect = is_enabled ? TRUE : FALSE;
            // Keep the raw JSON so the control-plane loop can record it in
            // failed_events/ if the apply fails.
            ev->event_data.hsi.raw_value = dup_string(value);
        } else {
            // DELETE: the new kv carries no value; the deleted config is in
            // prev_kv. Keep it for the failed_events/ record on apply failure.
            try {
                if (!event.prev_kv().key().empty())
                    ev->event_data.hsi.raw_value = dup_string(event.prev_kv().as_string());
            } catch (...) {
                // prev_kv unavailable — leave raw_value NULL
            }
        }
        enqueue_etcd_event(ev);
        return SUCCESS;
    }

    // Return SUCCESS if this event is PUT and it must be removed after processed
    STATUS process_user_count_change_event(const etcd::Event& event) {
        std::cout << "Processing user count change event: " << event.kv().key() << std::endl;

        std::string key = event.kv().key();
        std::string value = event.kv().as_string();
        int64_t revision = event.kv().modified_index();

        std::regex user_count_regex("user_counts/([^/]+)/");
        std::smatch matches;
        if (!std::regex_match(key, matches, user_count_regex) || matches.size() != 2) {
            // Errors are reported via Kafka (slice 11); failed_events/ is removed.
            std::cerr << "Invalid user count config key format: " << key << std::endl;
            return ERROR;
        }
        std::string node_id = matches[1].str();

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

        etcd_event_t *ev = alloc_etcd_event(ETCD_EVENT_USER_COUNT);
        if (!ev)
            return ERROR;
        ev->action = action;
        ev->revision = revision;
        ev->from_reconcile = FALSE;
        std::strncpy(ev->node_id, node_id.c_str(), sizeof(ev->node_id) - 1);

        if (action != HSI_ACTION_DELETE) {
            if (!parse_user_count_config(value, &ev->event_data.user_count)) {
                // Parse errors are reported via Kafka (slice 11).
                std::cerr << "Failed to parse user count config: " << value << std::endl;
                etcd_event_free(ev);
                return ERROR;
            }
        }
        enqueue_etcd_event(ev);
        return SUCCESS;
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

    // DNS record event processing
    STATUS process_dns_record_event(const etcd::Event& event) {
        std::string key = event.kv().key();

        // Only process keys matching configs/{nodeId}/{subscriberId}/dns (no domain suffix)
        std::regex dns_regex("configs/([^/]+)/([^/]+)/dns");
        std::smatch matches;
        if (!std::regex_match(key, matches, dns_regex) || matches.size() != 3)
            return ERROR; // Not a DNS record key, skip silently

        std::string node_id = matches[1].str();
        std::string user_id = matches[2].str();

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
                try {
                    Json::Value records;
                    Json::Reader reader;
                    if (!reader.parse(value, records) || !records.isArray()) {
                        std::cerr << "Failed to parse DNS records JSON array: " << value << std::endl;
                        return ERROR;
                    }
                    for (const Json::Value& entry : records) {
                        if (!entry.isMember("domain") || !entry.isMember("ip"))
                            continue;
                        etcd_event_t *ev = alloc_etcd_event(ETCD_EVENT_DNS_RECORD);
                        if (!ev)
                            continue;
                        ev->action = action;
                        ev->revision = revision;
                        ev->from_reconcile = FALSE;
                        std::strncpy(ev->node_id, node_id.c_str(), sizeof(ev->node_id) - 1);
                        std::strncpy(ev->user_id, user_id.c_str(), sizeof(ev->user_id) - 1);
                        std::strncpy(ev->event_data.dns_record.domain,
                            entry["domain"].asString().c_str(),
                            sizeof(ev->event_data.dns_record.domain) - 1);
                        std::strncpy(ev->event_data.dns_record.ip,
                            entry["ip"].asString().c_str(),
                            sizeof(ev->event_data.dns_record.ip) - 1);
                        ev->event_data.dns_record.ttl =
                            entry.isMember("ttl") ? entry["ttl"].asUInt() : 3600;
                        enqueue_etcd_event(ev);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Exception parsing DNS records: " << e.what() << std::endl;
                    return ERROR;
                }
                break;
            }
            case etcd::Event::EventType::DELETE_: {
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
                    Json::Reader reader;
                    if (!reader.parse(prev_value, records) || !records.isArray())
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

    // Put DNS record to etcd — all records for a subscriber share one key
public:
    etcd_status_t put_dns_record(const char* node_id, const char* user_id,
        const dns_record_config_t* record) {
        if (!client_ || !node_id || !user_id || !record) return ETCD_ERROR;

        try {
            std::string key = std::string("configs/") + node_id + "/" + user_id + "/dns";
            std::string domain_str(record->domain);

            // Read existing records
            Json::Value records(Json::arrayValue);
            auto get_resp = client_->get(key).get();
            if (get_resp.error_code() == 0) {
                Json::Reader reader;
                Json::Value existing;
                if (reader.parse(get_resp.value().as_string(), existing) && existing.isArray())
                    records = existing;
            }

            // Update existing entry or append new one
            bool found = false;
            for (Json::Value& entry : records) {
                if (entry.isMember("domain") && entry["domain"].asString() == domain_str) {
                    entry["ip"] = std::string(record->ip);
                    entry["ttl"] = record->ttl;
                    found = true;
                    break;
                }
            }
            if (!found) {
                Json::Value entry;
                entry["domain"] = domain_str;
                entry["ip"] = std::string(record->ip);
                entry["ttl"] = record->ttl;
                records.append(entry);
            }

            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            std::string payload = Json::writeString(writer, records);

            auto response = client_->set(key, payload).get();
            if (response.error_code() == 0) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                    "Wrote DNS records to: %s", key.c_str());
                return ETCD_SUCCESS;
            } else {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                    "Failed to put DNS record: %s", response.error_message().c_str());
                return ETCD_ERROR;
            }
        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                "Exception putting DNS record: %s", e.what());
            return ETCD_ERROR;
        }
    }

    // Load all DNS static records for a specific subscriber from etcd
    etcd_status_t load_dns_records(const char *node_uuid, const char *user_id,
        dns_record_callback_t dns_record_callback, void *user_data) {

        if (!client_ || !node_uuid || !user_id || !dns_record_callback)
            return ETCD_ERROR;

        try {
            std::string key = std::string("configs/") + node_uuid + "/" + user_id + "/dns";

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
            Json::Value records;
            Json::Reader reader;
            if (!reader.parse(value, records) || !records.isArray()) {
                FastRG_LOG(WARN, fastrg_ccb ? fastrg_ccb->fp : nullptr, NULL, NULL,
                    "load_dns_records: failed to parse JSON array for key %s", key.c_str());
                return ETCD_ERROR;
            }

            int loaded = 0;
            int64_t revision = response.index();
            for (const Json::Value& entry : records) {
                if (!entry.isMember("domain") || !entry.isMember("ip"))
                    continue;
                dns_record_config_t rec;
                memset(&rec, 0, sizeof(rec));
                strncpy(rec.domain, entry["domain"].asString().c_str(), sizeof(rec.domain) - 1);
                strncpy(rec.ip, entry["ip"].asString().c_str(), sizeof(rec.ip) - 1);
                rec.ttl = entry.isMember("ttl") ? entry["ttl"].asUInt() : 3600;

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

    // Delete a specific DNS record from the combined subscriber DNS key
    etcd_status_t delete_dns_record(const char* node_id, const char* user_id,
        const char* domain) {
        if (!client_ || !node_id || !user_id || !domain) return ETCD_ERROR;

        try {
            std::string key = std::string("configs/") + node_id + "/" + user_id + "/dns";
            std::string domain_str(domain);

            auto get_resp = client_->get(key).get();
            if (get_resp.error_code() == 100)
                return ETCD_SUCCESS; // Key not found — nothing to delete
            if (get_resp.error_code() != 0) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                    "Failed to get DNS records for delete: %s",
                    get_resp.error_message().c_str());
                return ETCD_ERROR;
            }

            // Rebuild array without the removed domain
            Json::Value remaining(Json::arrayValue);
            Json::Reader reader;
            Json::Value existing;
            if (reader.parse(get_resp.value().as_string(), existing) && existing.isArray()) {
                for (const Json::Value& entry : existing) {
                    if (entry.isMember("domain") && entry["domain"].asString() == domain_str)
                        continue;
                    remaining.append(entry);
                }
            }

            if (remaining.empty()) {
                // No records left — remove the key entirely
                auto rm_resp = client_->rm(key).get();
                if (rm_resp.error_code() == 0 || rm_resp.error_code() == 100) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                        "Deleted DNS key (no remaining records): %s", key.c_str());
                    return ETCD_SUCCESS;
                }
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                    "Failed to delete DNS key: %s", rm_resp.error_message().c_str());
                return ETCD_ERROR;
            }

            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            std::string payload = Json::writeString(writer, remaining);
            auto set_resp = client_->set(key, payload).get();
            if (set_resp.error_code() == 0) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                    "Removed domain %s from DNS records at: %s", domain, key.c_str());
                return ETCD_SUCCESS;
            }
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                "Failed to update DNS records after delete: %s",
                set_resp.error_message().c_str());
            return ETCD_ERROR;
        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                "Exception deleting DNS record: %s", e.what());
            return ETCD_ERROR;
        }
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

void etcd_client_write_fallback_error(const char *event_type, const char *key,
    const char *node_id, const char *user_id, etcd_error_reason_t reason,
    const char *error_detail, const char *original_value) {
    if (!g_etcd_client)
        return;
    g_etcd_client->write_fallback_error(
        event_type ? event_type : "", key ? key : "",
        node_id ? node_id : "", user_id ? user_id : "",
        reason, error_detail ? error_detail : "",
        original_value ? original_value : "");
}

int etcd_client_is_initialized(void) {
    return (g_etcd_client != nullptr) ? 1 : 0;
}

etcd_status_t etcd_client_cas_put(const char* key, etcd_mutate_fn_t mutate_fn,
    void* user_data, int64_t* out_revision) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->cas_put(std::string(key), mutate_fn, user_data, out_revision);
}

int etcd_client_is_connected(void) {
    return (g_etcd_client && g_etcd_client->is_connected()) ? 1 : 0;
}

etcd_status_t etcd_client_queue_load(void) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->load_config_queue();
}

etcd_status_t etcd_client_queue_hsi_put(const char* node_id, const char* user_id,
    const hsi_config_t* config, const char* updated_by) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->queue_hsi_put(node_id, user_id, config, updated_by);
}

etcd_status_t etcd_client_queue_hsi_delete(const char* node_id, const char* user_id) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->queue_hsi_delete(node_id, user_id);
}

etcd_status_t etcd_client_queue_subscriber_count(const char* node_id,
    const char* subscriber_count_str, const char* updated_by) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->queue_subscriber_count(node_id, subscriber_count_str, updated_by);
}

etcd_status_t etcd_client_queue_desire_status(const char* node_id, const char* user_id,
    const char* desire_status) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->queue_desire_status(node_id, user_id, desire_status);
}

int etcd_client_queue_pending(void) {
    if (!g_etcd_client) return 0;
    return g_etcd_client->queue_pending();
}

etcd_status_t etcd_client_put_hsi_config(const char* node_id, const char* user_id,
    const hsi_config_t* config, const char* updated_by,
    int64_t* revision) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->put_hsi_config(node_id, user_id, config, updated_by, revision);
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

etcd_status_t etcd_client_put_subscriber_count(const char* node_id, 
    const char* subscriber_count_str, const char* updated_by) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->put_subscriber_count(node_id, subscriber_count_str, updated_by);
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

etcd_status_t etcd_client_put_dns_record(const char* node_id, const char* user_id,
    const dns_record_config_t* record) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->put_dns_record(node_id, user_id, record);
}

etcd_status_t etcd_client_delete_dns_record(const char* node_id, const char* user_id,
    const char* domain) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->delete_dns_record(node_id, user_id, domain);
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
