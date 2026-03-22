#include <common.h>
#include "etcd_client.h"
#include "../../src/dbg.h"
#include "../../src/fastrg.h"
#include <etcd/Client.hpp>
#include <etcd/Watcher.hpp>
#include <etcd/Response.hpp>
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

class EtcdClientImpl {
private:
    std::unique_ptr<etcd::Client> client_;
    std::unique_ptr<etcd::Watcher> hsi_watcher_;
    std::unique_ptr<etcd::Watcher> command_watcher_;
    std::unique_ptr<etcd::Watcher> user_count_watcher_;
    std::atomic<bool> watch_running_;
    std::string node_uuid_;
    std::string etcd_endpoints_;
    
    hsi_config_callback_t hsi_callback_;
    pppoe_command_callback_t command_callback_;
    user_count_changed_callback_t user_count_callback_;
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
    EtcdClientImpl() : watch_running_(false), hsi_callback_(nullptr), 
                       command_callback_(nullptr), user_count_callback_(nullptr),
                       sync_request_callback_(nullptr), fastrg_ccb(nullptr), 
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

    void stop_watchdog() {
        if (watchdog_running_) {
            watchdog_running_ = false;
            watchdog_cv_.notify_all();
            if (watchdog_thread_.joinable()) {
                watchdog_thread_.join();
            }
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

                // Check if watch is active
                auto now = std::chrono::steady_clock::now();
                auto last_activity = last_watch_activity_.load();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity).count();

                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Watchdog check: %ld seconds since last watch activity", elapsed);

                if (elapsed > WATCH_TIMEOUT_SEC) {
                    FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Watchdog detected inactive watchers (%ld s), testing connection...", elapsed);

                    // Test if etcd is actually reachable
                    if (test_etcd_connection()) {
                        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Etcd is reachable but watchers are inactive");
                        update_watch_activity();
                    } else {
                        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Etcd is not reachable, watchers expected to be inactive, try to reconnect...");
                        // Set flag to exit watchdog to avoid joining in self thread
                        watchdog_running_ = false;

                        // Use thread to trigger reconnection to avoid deadlock
                        std::thread([this]() {
                            // Set short delay to ensure watchdog fully exits
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            trigger_reconnect();
                        }).detach();
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
                    return ETCD_SUCCESS;
                }
                return ETCD_ERROR;
            }
            return ETCD_SUCCESS;
        } catch (const std::exception& e) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                "Exception during etcd initialization: %s", e.what());
            return ETCD_ERROR;
        }
    }

    etcd_status_t start_watch(const char* node_uuid,
        hsi_config_callback_t hsi_callback,
        pppoe_command_callback_t command_callback,
        user_count_changed_callback_t user_count_callback,
        sync_request_callback_t sync_request_callback) {

        if (!client_) {
            return ETCD_ERROR;
        }

        node_uuid_ = node_uuid;
        hsi_callback_ = hsi_callback;
        command_callback_ = command_callback;
        user_count_callback_ = user_count_callback;
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
            if (command_watcher_) {
                command_watcher_->Cancel();
                command_watcher_.reset();
            }

            if (user_count_watcher_) {
                user_count_watcher_->Cancel();
                user_count_watcher_.reset();
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

            // Watch commands: commands/{nodeId}/
            std::string command_prefix = "commands/" + node_uuid_ + "/";

            // Create command watcher with callback
            command_watcher_ = std::make_unique<etcd::Watcher>(
                *client_, 
                command_prefix,
                [this](etcd::Response response) {
                    if (!watch_running_) return;
                    
                    update_watch_activity(); // update activity time
                    
                    if (response.error_code() == 0) {
                        for(const auto& event : response.events()) {
                            if (process_command_event(event) == SUCCESS) {
                                // Delete command after processing
                                delete_command(event.kv().key().c_str());
                            }
                        }
                    } else {
                        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Command watch error: %s", response.error_message().c_str());
                        // Trigger reconnection on watch error
                        trigger_reconnect();
                    }
                },
                [this](bool connected) {
                    update_watch_activity(); // update activity time
                    
                    // Connection status callback
                    if (!connected && watch_running_) {
                        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Command watcher disconnected, triggering reconnect...");
                        trigger_reconnect();
                    }
                },
                true  // recursive
            );

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
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        
        // Only start reconnect thread if not already running
        if (!reconnect_running_) {
            reconnect_running_ = true;
            
            // stop watchdog (if still running)
            // Note: if this function is called from the watchdog thread, watchdog_running_ should already be set to false
            if (watchdog_running_) {
                stop_watchdog();
            } else if (watchdog_thread_.joinable()) {
                // Watchdog has been marked to stop but the thread has not joined yet, wait for it
                watchdog_thread_.join();
            }

            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
                "watch dog stopped, triggering etcd reconnection...");
            
            // Stop current watchers
            if (hsi_watcher_) {
                hsi_watcher_->Cancel();
            }
            if (command_watcher_) {
                command_watcher_->Cancel();
            }
            
            if (user_count_watcher_) {
                user_count_watcher_->Cancel();
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

    // Synchronize state with etcd after reconnection
    void sync_state_with_etcd() {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Starting state synchronization with etcd...");
        
        if (!hsi_callback_ || !user_count_callback_) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot sync state: callbacks not set");
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

            // Step 3: Load existing configs from etcd (this updates local state if etcd has data)
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Reloading existing configs from etcd for node: %s", node_uuid_.c_str());
            etcd_status_t status = load_existing_configs(
                node_uuid_.c_str(),
                hsi_callback_,
                command_callback_,
                user_count_callback_,
                fastrg_ccb
            );
            
            if (status == ETCD_SUCCESS) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Successfully reloaded configs from etcd");
            } else {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to reload configs from etcd, status: %d", status);
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
        watch_running_ = false;
        
        // Stop watchdog first
        stop_watchdog();
        
        // Stop reconnection thread first
        stop_reconnect_thread();

        if (hsi_watcher_) {
            hsi_watcher_->Cancel();
        }
        if (command_watcher_) {
            command_watcher_->Cancel();
        }

        if (user_count_watcher_) {
            user_count_watcher_->Cancel();
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

    // Put HSI config into etcd under configs/{nodeId}/hsi/{userId}
    etcd_status_t put_hsi_config(const char* node_id, const char* user_id, 
        const hsi_config_t* config, hsi_enable_status_t enable_status, const char* updated_by) {
        if (!client_ || !node_id || !user_id || !config) return ETCD_ERROR;

        try {
            std::stringstream ss;
            ss << "configs/" << node_id << "/hsi/" << user_id;
            std::string key = ss.str();

            Json::Value root;
            Json::Value cfg;
            cfg["user_id"] = std::string(config->user_id);
            cfg["vlan_id"] = std::string(config->vlan_id);
            cfg["account_name"] = std::string(config->account_name);
            cfg["password"] = std::string(config->password);
            cfg["dhcp_addr_pool"] = std::string(config->dhcp_addr_pool);
            cfg["dhcp_subnet"] = std::string(config->dhcp_subnet);
            cfg["dhcp_gateway"] = std::string(config->dhcp_gateway);

            root["config"] = cfg;
            Json::Value meta;
            meta["node"] = std::string(node_id);
            meta["resourceVersion"] = "";
            meta["updatedBy"] = updated_by ? std::string(updated_by) : std::string("");
            std::string status_str;
            switch (enable_status) {
                case ENABLE_STATUS_ENABLED:
                    status_str = "enabled";
                    break;
                case ENABLE_STATUS_ENABLING:
                    status_str = "enabling";
                    break;
                case ENABLE_STATUS_DISABLING:
                    status_str = "disabling";
                    break;
                case ENABLE_STATUS_DISABLED:
                    status_str = "disabled";
                    break;
                default:
                    status_str = "unknown";
                    break;
            }
            meta["enableStatus"] = status_str;

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
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Wrote HSI config to: %s", key.c_str());
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

    etcd_status_t get_hsi_config_status(const std::string& node_id, 
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
                std::cerr << "Failed to parse HSI config JSON for get_status" << std::endl;
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

            // Ensure null termination
            output->config.user_id[sizeof(output->config.user_id) - 1] = '\0';
            output->config.vlan_id[sizeof(output->config.vlan_id) - 1] = '\0';
            output->config.account_name[sizeof(output->config.account_name) - 1] = '\0';
            output->config.password[sizeof(output->config.password) - 1] = '\0';
            output->config.dhcp_addr_pool[sizeof(output->config.dhcp_addr_pool) - 1] = '\0';
            output->config.dhcp_subnet[sizeof(output->config.dhcp_subnet) - 1] = '\0';
            output->config.dhcp_gateway[sizeof(output->config.dhcp_gateway) - 1] = '\0';

            // Parse metadata section
            if (root.isMember("metadata")) {
                Json::Value metadata = root["metadata"];
                
                // Parse enableStatus
                if (metadata["enableStatus"].isString()) {
                    std::string status = metadata["enableStatus"].asString();
                    if (status == "enabled") {
                        output->enable_status = ENABLE_STATUS_ENABLED;
                    } else if (status == "enabling") {
                        output->enable_status = ENABLE_STATUS_ENABLING;
                    } else if (status == "disabling") {
                        output->enable_status = ENABLE_STATUS_DISABLING;
                    } else if (status == "disabled") {
                        output->enable_status = ENABLE_STATUS_DISABLED;
                    } else {
                        output->enable_status = ENABLE_STATUS_DISABLED; // default
                    }
                } else {
                    output->enable_status = ENABLE_STATUS_DISABLED;
                }

                // Parse other metadata fields
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
                output->enable_status = ENABLE_STATUS_DISABLED;
                output->updated_by[0] = '\0';
                output->updated_at[0] = '\0';
                output->resource_version[0] = '\0';
            }

            std::cout << "Successfully retrieved HSI config status for user: " 
                << user_id << " (enableStatus: " 
                << (output->enable_status == ENABLE_STATUS_ENABLED ? "enabled" :
                    output->enable_status == ENABLE_STATUS_ENABLING ? "enabling" :
                    output->enable_status == ENABLE_STATUS_DISABLING ? "disabling" : "disabled")
                << ")" << std::endl;

            return ETCD_SUCCESS;

        } catch (const std::exception& e) {
            std::cerr << "Exception getting HSI config status: " << e.what() << std::endl;
            return ETCD_ERROR;
        }
    }

    etcd_status_t modify_hsi_config_status(const std::string& node_id, 
        const std::string& user_id, hsi_enable_status_t enable_status, int64_t* revision) {
        if (!client_) {
            return ETCD_ERROR;
        }

        try {
            std::string key = "configs/" + node_id + "/hsi/" + user_id;

            // Get current config
            auto get_response = client_->get(key).get();

            if (get_response.error_code() != 0) {
                std::cerr << "Failed to get HSI config for modify with key: " 
                    << key << " - " << get_response.error_message() << std::endl;
                return ETCD_ERROR;
            }

            std::string value = get_response.value().as_string();

            // Parse JSON
            Json::Value root;
            Json::Reader reader;

            if (!reader.parse(value, root)) {
                std::cerr << "Failed to parse HSI config JSON for modify" << std::endl;
                return ETCD_CONFIG_PARSE_FAILED;
            }

            // Modify the enableStatus field in metadata
            if (!root.isMember("metadata")) {
                root["metadata"] = Json::Value(Json::objectValue);
            }

            // Provide a textual enableStatus for intermediate states
            std::string status_str;
            switch (enable_status) {
                case ENABLE_STATUS_ENABLED:
                    status_str = "enabled";
                    break;
                case ENABLE_STATUS_ENABLING:
                    status_str = "enabling";
                    break;
                case ENABLE_STATUS_DISABLING:
                    status_str = "disabling";
                    break;
                case ENABLE_STATUS_DISABLED:
                    status_str = "disabled";
                    break;
                default:
                    status_str = "unknown";
                    break;
            }
            root["metadata"]["enableStatus"] = status_str;

            // Update the updatedAt timestamp
            std::time_t now = std::time(nullptr);
            std::tm tm{};
            gmtime_r(&now, &tm);
            std::ostringstream out;
            out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            root["metadata"]["updatedAt"] = out.str();
            root["metadata"]["updatedBy"] = "etcd_client_modify";

            // Convert back to JSON string
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            std::string modified_payload = Json::writeString(writer, root);

            // Write back to etcd
            auto set_response = client_->set(key, modified_payload).get();

            if (set_response.error_code() != 0) {
                std::cerr << "Failed to update HSI config status: " << set_response.error_message() << std::endl;
                return ETCD_ERROR;
            }

            // Mark this modification as pending to avoid processing in watcher
            *revision = set_response.index();

            std::cout << "Set HSI enableStatus='" << status_str << "' for user: " << user_id
                      << " (revision: " << *revision << ")" << std::endl;

            return ETCD_SUCCESS;

        } catch (const std::exception& e) {
            std::cerr << "Exception modifying HSI config status: " << e.what() << std::endl;
            return ETCD_ERROR;
        }
    }

    etcd_status_t load_existing_configs(const char* node_uuid,
        hsi_config_callback_t hsi_callback,
        pppoe_command_callback_t command_callback,
        user_count_changed_callback_t user_count_callback,
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

                    // Invoke callback with UPDATE action for existing configs
                    STATUS ret = hsi_callback(node_id.c_str(), user_id.c_str(), &config, 
                        HSI_ACTION_UPDATE, revision, user_data);
                    if (ret == SUCCESS) {
                        count++;
                        std::cout << "Loaded existing HSI config for user: " << user_id << std::endl;
                        if (is_enabled) {
                            std::cout << "The loaded HSI config for user: " << user_id << " is enabled." << std::endl;
                            pppoe_command_t command = { 0 };
                            std::strncpy(command.action, "dial", sizeof(command.action)-1);
                            std::strncpy(command.user_id, config.user_id, sizeof(command.user_id)-1);
                            std::strncpy(command.vlan, config.vlan_id, sizeof(command.vlan)-1);
                            std::strncpy(command.account, config.account_name, sizeof(command.account)-1);
                            std::strncpy(command.password, config.password, sizeof(command.password)-1);
                            command.timestamp = std::time(nullptr);
                            command_callback(node_id.c_str(), &command, user_data);
                        } else {
                            std::cout << "The loaded HSI config for user: " << user_id << " is disabled." << std::endl;
                        }
                    } else {
                        std::cerr << "Failed to load HSI config for user: " << user_id << std::endl;
                    }
                } else {
                    std::cerr << "Failed to parse existing HSI config for user: " << user_id << std::endl;
                }
            }

            std::cout << "Loaded " << count << " existing HSI config(s) for node: " << node_uuid << std::endl;

            return ETCD_SUCCESS;

        } catch (const std::exception& e) {
            std::cerr << "Exception loading existing configs: " << e.what() << std::endl;
            return ETCD_ERROR;
        }
    }

private:
    STATUS process_hsi_event(const etcd::Event& event) {
        if (!hsi_callback_) return ERROR;

        std::cout << "Processing HSI event: " << event.kv().key() << std::endl;

        std::string key = event.kv().key();
        std::string value = event.kv().as_string();
        int64_t revision = event.kv().modified_index();

        // Extract user_id from key: configs/{nodeId}/hsi/{userId}
        std::regex hsi_regex("configs/([^/]+)/hsi/(.+)");
        std::smatch matches;

        if (!std::regex_match(key, matches, hsi_regex) || matches.size() != 3) {
            std::cerr << "Invalid HSI config key format: " << key << std::endl;
            write_fallback_error("hsi_config", key, node_uuid_, "",
                ERROR_REASON_INVALID_FORMAT, 
                "Key does not match expected format: configs/{nodeId}/hsi/{userId}",
                value);
            return ERROR;
        }

        std::string node_id = matches[1].str();
        std::string user_id = matches[2].str();

        etcd_action_type_t action;
        switch (event.event_type()) {
            case etcd::Event::EventType::PUT:
                // Distinguish between CREATE and UPDATE by checking if prev_kv exists
                try {
                    if (event.prev_kv().key().empty() || event.prev_kv().key().length() == 0) {
                        action = HSI_ACTION_CREATE;  // No previous value = new key
                    } else {
                        action = HSI_ACTION_UPDATE;  // Has previous value = update
                    }
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

        if (action == HSI_ACTION_DELETE) {
            // For delete events, try to get prev_value if available
            std::string prev_value;
            try {
                if (event.prev_kv().key().length() > 0) {
                    prev_value = event.prev_kv().as_string();
                }
            } catch (...) {
                // prev_kv not available
            }

            // Call callback with nullptr config and revision
            STATUS ret = hsi_callback_(node_id.c_str(), user_id.c_str(), nullptr, action, revision, fastrg_ccb);

            // If callback failed, write fallback error
            if (ret != SUCCESS) {
                std::cerr << "HSI DELETE callback failed for: " << node_id << "/" << user_id << std::endl;
                write_fallback_error("hsi_config", key, node_id, user_id,
                    ERROR_REASON_CALLBACK_FAILED,
                    "HSI DELETE callback returned error",
                    prev_value);
            }

            return ret;
        }

        // Parse HSI config from JSON
        hsi_config_t config;
        bool is_enabled = false;
        if (parse_hsi_config(value, &config, &is_enabled)) {
            STATUS ret = hsi_callback_(node_id.c_str(), user_id.c_str(), &config, 
                action, revision, fastrg_ccb);

            // If callback failed, write fallback error
            if (ret != SUCCESS) {
                std::cerr << "HSI callback failed for: " << node_id << "/" << user_id << std::endl;
                write_fallback_error("hsi_config", key, node_id, user_id,
                    ERROR_REASON_CALLBACK_FAILED, "HSI callback returned error", value);
            }

            return ret;
        } else {
            std::cerr << "Failed to parse HSI config: " << value << std::endl;
            write_fallback_error("hsi_config", key, node_id, user_id,
                ERROR_REASON_PARSE_FAILED,
                "JSON parsing failed or missing required fields",
                value);
            return ERROR;
        }
    }

    // Return SUCCESS if this event is PUT and it must be removed after processed
    STATUS process_command_event(const etcd::Event& event) {
        std::string key = event.kv().key();
        std::string value = event.kv().as_string();

        if (!command_callback_) return ERROR;

        std::cout << "Processing command event: " << key << std::endl;

        // Only process PUT events for commands
        if (event.event_type() != etcd::Event::EventType::PUT) {
            std::cout << "Ignoring non-PUT command event: " << key << std::endl;
            return ERROR;
        }

        // Extract node_id from key: commands/{nodeId}/pppoe_{action}_{userId}
        std::regex command_regex("commands/([^/]+)/pppoe_(dial|hangup)_(.+)");
        std::smatch matches;

        if (!std::regex_match(key, matches, command_regex) || matches.size() != 4) {
            // Not a PPPoE command, ignore and write fallback error
            std::cerr << "Invalid command key format: " << key << std::endl;
            write_fallback_error("pppoe_command", key, node_uuid_, "",
                ERROR_REASON_INVALID_FORMAT,
                "Key does not match expected format: commands/{nodeId}/pppoe_{action}_{userId}",
                value);
            return SUCCESS;
        }

        std::string node_id = matches[1].str();
        std::string action = matches[2].str();
        std::string user_id = matches[3].str();

        // Parse command from JSON
        pppoe_command_t command;
        if (parse_pppoe_command(value, &command)) {
            STATUS ret = command_callback_(node_id.c_str(), &command, fastrg_ccb);
            if (ret != SUCCESS) {
                // Callback failed, write fallback error and keep command for retry
                std::cerr << "Command callback failed for: " << node_id << "/" << user_id << std::endl;
                write_fallback_error("pppoe_command", key, node_id, user_id,
                    ERROR_REASON_CALLBACK_FAILED,
                    "PPPoE command callback returned error",
                    value);
            }
            return SUCCESS;
        } else {
            std::cerr << "Failed to parse PPPoE command: " << value << std::endl;
            write_fallback_error("pppoe_command", key, node_id, user_id,
                ERROR_REASON_PARSE_FAILED,
                "JSON parsing failed or missing required fields", value);
            return SUCCESS;
        }
    }

    STATUS process_user_count_change_event(const etcd::Event& event) {
        if (!user_count_callback_) return ERROR;

        std::cout << "Processing user count change event: " << event.kv().key() << std::endl;

        std::string key = event.kv().key();
        std::string value = event.kv().as_string();
        int64_t revision = event.kv().modified_index();

        // Extract node_id from key: user_counts/{nodeId}/
        std::regex user_count_regex("user_counts/([^/]+)/");
        std::smatch matches;

        if (!std::regex_match(key, matches, user_count_regex) || matches.size() != 2) {
            std::cerr << "Invalid user count config key format: " << key << std::endl;
            write_fallback_error("user_count_config", key, node_uuid_, "",
                ERROR_REASON_INVALID_FORMAT,
                "Key does not match expected format: user_counts/{nodeId}/",
                value);
            return ERROR;
        }

        std::string node_id = matches[1].str();

        etcd_action_type_t action;
        switch (event.event_type()) {
            case etcd::Event::EventType::PUT:
                // Distinguish between CREATE and UPDATE by checking if prev_kv exists
                try {
                    if (event.prev_kv().key().empty() || event.prev_kv().key().length() == 0) {
                        action = HSI_ACTION_CREATE;  // No previous value = new key
                    } else {
                        action = HSI_ACTION_UPDATE;  // Has previous value = update
                    }
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

        if (action == HSI_ACTION_DELETE) {
            // For delete events, try to get prev_value if available
            std::string prev_value;
            try {
                if (event.prev_kv().key().length() > 0) {
                    prev_value = event.prev_kv().as_string();
                }
            } catch (...) {
                // prev_kv not available
            }

            // Call callback with nullptr config and revision
            STATUS ret = user_count_callback_(node_id.c_str(), nullptr, action, revision, fastrg_ccb);

            // If callback failed, write fallback error
            if (ret != SUCCESS) {
                std::cerr << "User count DELETE callback failed for node: " << node_id << std::endl;
                write_fallback_error("user_count_config", key, node_id, "",
                    ERROR_REASON_CALLBACK_FAILED,
                    "User count DELETE callback returned error",
                    prev_value);
            }

            return ret;
        }

        // Parse user count config from JSON
        user_count_config_t config;
        if (parse_user_count_config(value, &config)) {
            STATUS ret = user_count_callback_(node_id.c_str(), &config, action, revision, fastrg_ccb);

            // If callback failed, write fallback error
            if (ret != SUCCESS) {
                std::cerr << "User count callback failed for node: " << node_id << std::endl;
                write_fallback_error("user_count_config", key, node_id, "",
                    ERROR_REASON_CALLBACK_FAILED,
                    "User count callback returned error",
                    value);
            }

            return ret;
        } else {
            std::cerr << "Failed to parse user count config: " << value << std::endl;
            write_fallback_error("user_count_config", key, node_id, "",
                ERROR_REASON_PARSE_FAILED,
                "JSON parsing failed or missing required fields",
                value);
            return ERROR;
        }
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

            if (root.isMember("metadata") && is_enabled) {
                Json::Value metadata = root["metadata"];
                if (metadata["enableStatus"].isString()) {
                    std::string status = metadata["enableStatus"].asString();
                    *is_enabled = (status == "enabled" || status == "enabling");
                } else {
                    *is_enabled = false;
                }
            }

            return true;

        } catch (const std::exception& e) {
            std::cerr << "Exception parsing HSI config: " << e.what() << std::endl;
            return false;
        }
    }

    bool parse_pppoe_command(const std::string& json_str, pppoe_command_t* command) {
        try {
            Json::Value root;
            Json::Reader reader;

            if (!reader.parse(json_str, root)) {
                return false;
            }

            // Extract command fields
            strncpy(command->action, root.get("action", "").asString().c_str(), 
                   sizeof(command->action) - 1);
            strncpy(command->user_id, root.get("user_id", "").asString().c_str(), 
                   sizeof(command->user_id) - 1);
            strncpy(command->vlan, root.get("vlan", "").asString().c_str(), 
                   sizeof(command->vlan) - 1);
            strncpy(command->account, root.get("account", "").asString().c_str(), 
                   sizeof(command->account) - 1);
            strncpy(command->password, root.get("password", "").asString().c_str(), 
                   sizeof(command->password) - 1);
            command->timestamp = root.get("timestamp", 0).asInt64();

            // Ensure null termination
            command->action[sizeof(command->action) - 1] = '\0';
            command->user_id[sizeof(command->user_id) - 1] = '\0';
            command->vlan[sizeof(command->vlan) - 1] = '\0';
            command->account[sizeof(command->account) - 1] = '\0';
            command->password[sizeof(command->password) - 1] = '\0';

            return true;

        } catch (const std::exception& e) {
            std::cerr << "Exception parsing PPPoE command: " << e.what() << std::endl;
            return false;
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
    hsi_config_callback_t hsi_callback,
    pppoe_command_callback_t command_callback,
    user_count_changed_callback_t user_count_callback,
    sync_request_callback_t sync_request_callback) {

    if (!g_etcd_client) {
        return ETCD_ERROR;
    }
    return g_etcd_client->start_watch(node_uuid, hsi_callback, 
        command_callback, user_count_callback, sync_request_callback);
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

etcd_status_t etcd_client_put_hsi_config(const char* node_id, const char* user_id, 
    const hsi_config_t* config, hsi_enable_status_t enable_status, const char* updated_by) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->put_hsi_config(node_id, user_id, config, enable_status, updated_by);
}

etcd_status_t etcd_client_delete_hsi_config(const char* node_id, const char* user_id, 
    int64_t* revision) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->delete_hsi_config(node_id, user_id, revision);
}

etcd_status_t etcd_client_modify_hsi_config_status(const char* node_id, 
    const char* user_id, hsi_enable_status_t enable_status, int64_t* revision) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->modify_hsi_config_status(std::string(node_id), 
        std::string(user_id), enable_status, revision);
}

etcd_status_t etcd_client_get_hsi_config_status(const char* node_id, 
    const char* user_id, hsi_config_full_t* output) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->get_hsi_config_status(std::string(node_id), 
        std::string(user_id), output);
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
    pppoe_command_callback_t command_callback,
    user_count_changed_callback_t user_count_callback,
    void* user_data) {
    if (!g_etcd_client) return ETCD_ERROR;
    return g_etcd_client->load_existing_configs(node_uuid, hsi_callback, 
        command_callback, user_count_callback, user_data);
}

void etcd_client_cleanup(void) {
    if (g_etcd_client) {
        g_etcd_client.reset();
    }
}

} // extern "C"
