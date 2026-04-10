#include <iostream>
#include <unistd.h>
#include <cstring>
#include "../etcd_client.h"

// Global flag to control callback failure for testing
static bool simulate_callback_failure = false;
static int callback_count = 0;

STATUS hsi_config_callback(const char *node_id, const char *user_id, 
    const hsi_config_t *config, etcd_action_type_t action, 
    int64_t revision, void *user_data) {
    callback_count++;

    const char* action_str;
    switch (action) {
        case HSI_ACTION_CREATE: action_str = "CREATE"; break;
        case HSI_ACTION_UPDATE: action_str = "UPDATE"; break; 
        case HSI_ACTION_DELETE: action_str = "DELETE"; break;
        default: action_str = "UNKNOWN"; break;
    }

    std::cout << "=== HSI Config Event ===" << std::endl;
    std::cout << "Action: " << action_str << std::endl;
    std::cout << "Node ID: " << node_id << std::endl;
    std::cout << "User ID: " << user_id << std::endl;

    if (config) {
        std::cout << "Config Details:" << std::endl;
        std::cout << "  VLAN ID: " << config->vlan_id << std::endl;
        std::cout << "  Account: " << config->account_name << std::endl;
        std::cout << "  DHCP Pool: " << config->dhcp_addr_pool << std::endl;
        std::cout << "  DHCP Subnet: " << config->dhcp_subnet << std::endl;
        std::cout << "  DHCP Gateway: " << config->dhcp_gateway << std::endl;
    } else {
        std::cout << "Config: (null - deleted)" << std::endl;
    }

    // Simulate failure for testing fallback error mechanism
    if (simulate_callback_failure) {
        std::cout << "!!! SIMULATING CALLBACK FAILURE !!!" << std::endl;
        std::cout << "========================" << std::endl;
        return ERROR;
    }

    std::cout << "========================" << std::endl;
    return SUCCESS;
}

STATUS pppoe_command_callback(const char* node_id, const pppoe_command_t* command, void* user_data) {
    callback_count++;

    std::cout << "=== PPPoE Command Event ===" << std::endl;
    std::cout << "Node ID: " << node_id << std::endl;
    std::cout << "Action: " << command->action << std::endl;
    std::cout << "User ID: " << command->user_id << std::endl;
    std::cout << "VLAN: " << command->vlan << std::endl;
    std::cout << "Account: " << command->account << std::endl;
    std::cout << "Timestamp: " << command->timestamp << std::endl;

    // Simulate failure for testing fallback error mechanism
    if (simulate_callback_failure) {
        std::cout << "!!! SIMULATING CALLBACK FAILURE !!!" << std::endl;
        std::cout << "===========================" << std::endl;
        return ERROR;
    }

    std::cout << "===========================" << std::endl;
    return SUCCESS;
}

STATUS user_count_changed_callback(const char* node_id,
    const user_count_config_t* config, etcd_action_type_t action,
    int64_t revision, void* user_data) {
    callback_count++;

    const char* action_str;
    switch (action) {
        case HSI_ACTION_CREATE: action_str = "CREATE"; break;
        case HSI_ACTION_UPDATE: action_str = "UPDATE"; break; 
        case HSI_ACTION_DELETE: action_str = "DELETE"; break;
        default: action_str = "UNKNOWN"; break;
    }

    std::cout << "=== User Count Change Event ===" << std::endl;
    std::cout << "Action: " << action_str << std::endl;
    std::cout << "Node ID: " << node_id << std::endl;
    std::cout << "New User Count: " << config->user_count << std::endl;

    // Simulate failure for testing fallback error mechanism
    if (simulate_callback_failure) {
        std::cout << "!!! SIMULATING CALLBACK FAILURE !!!" << std::endl;
        std::cout << "==============================" << std::endl;
        return ERROR;
    }

    std::cout << "==============================" << std::endl;
    return SUCCESS;
}

void sync_request_callback(const char* node_id, void* user_data) {
    std::cout << "=== Sync Request Event ===" << std::endl;
    std::cout << "Node ID: " << node_id << std::endl;
    std::cout << "==========================" << std::endl;
}

STATUS dns_record_callback(const char *node_id, const char *user_id,
    const dns_record_config_t *record, etcd_action_type_t action,
    int64_t revision, void *user_data) {
    callback_count++;
    std::cout << "=== DNS Record Event ===" << std::endl;
    std::cout << "Node ID: " << node_id << std::endl;
    std::cout << "User ID: " << user_id << std::endl;
    if (record) {
        std::cout << "Domain: " << record->domain << std::endl;
        std::cout << "Action: " << (action == HSI_ACTION_DELETE ? "DELETE" : "UNKNOWN") << std::endl;
    }
    std::cout << "========================" << std::endl;

    return SUCCESS;
}

int main() {
    std::cout << "Starting etcd client test..." << std::endl;
    std::cout << "\nTest Features:" << std::endl;
    std::cout << "1. Normal event processing" << std::endl;
    std::cout << "2. Fallback error mechanism when callback fails" << std::endl;
    std::cout << "3. Watch failed_events/ namespace to see error records" << std::endl;
    std::cout << "\nCommands:" << std::endl;
    std::cout << "  Type 'fail' to toggle callback failure simulation" << std::endl;
    std::cout << "  Type 'stat' to show statistics" << std::endl;
    std::cout << "  Type 'quit' to exit\n" << std::endl;

    // Initialize etcd client
    const char* etcd_endpoints = "http://127.0.0.1:2379";
    etcd_status_t status = etcd_client_init(etcd_endpoints, nullptr);

    if (status != ETCD_SUCCESS) {
        std::cerr << "Failed to initialize etcd client" << std::endl;
        return -1;
    }

    std::cout << "Etcd client initialized successfully" << std::endl;

    // Start watching for test node
    const char* test_node_uuid = "test-node-12345";
    status = etcd_client_start_watch(test_node_uuid, hsi_config_callback, 
        pppoe_command_callback, user_count_changed_callback, 
        sync_request_callback, dns_record_callback);

    if (status != ETCD_SUCCESS) {
        std::cerr << "Failed to start etcd watching" << std::endl;
        etcd_client_cleanup();
        return -1;
    }

    std::cout << "Etcd watching started for node: " << test_node_uuid << std::endl;
    std::cout << "\nTo test fallback error mechanism:" << std::endl;
    std::cout << "1. Type 'fail' to enable callback failure simulation" << std::endl;
    std::cout << "2. Write a test config: etcdctl put configs/test-node-12345/hsi/user001 '{\"user_id\":\"user001\",\"vlan_id\":\"100\",\"account_name\":\"test@isp.com\",\"password\":\"pass123\",\"dhcp_addr_pool\":\"192.168.1.100-192.168.1.200\",\"dhcp_subnet\":\"192.168.1.0/24\",\"dhcp_gateway\":\"192.168.1.1\"}'" << std::endl;
    std::cout << "3. Check failed_events: etcdctl get failed_events/ --prefix" << std::endl;
    std::cout << "\nWaiting for events...\n" << std::endl;

    // Interactive command loop
    char input[256];
    while (true) {
        std::cout << "> ";
        if (fgets(input, sizeof(input), stdin) != nullptr) {
            // Remove newline
            input[strcspn(input, "\n")] = 0;

            if (strcmp(input, "fail") == 0) {
                simulate_callback_failure = !simulate_callback_failure;
                std::cout << "Callback failure simulation: " 
                         << (simulate_callback_failure ? "ENABLED" : "DISABLED") << std::endl;
            } else if (strcmp(input, "stat") == 0) {
                std::cout << "=== Statistics ===" << std::endl;
                std::cout << "Total callbacks processed: " << callback_count << std::endl;
                std::cout << "Failure simulation: " 
                         << (simulate_callback_failure ? "ENABLED" : "DISABLED") << std::endl;
                std::cout << "==================" << std::endl;
            } else if (strcmp(input, "quit") == 0) {
                std::cout << "Exiting..." << std::endl;
                break;
            } else if (strlen(input) > 0) {
                std::cout << "Unknown command. Available: fail, stat, quit" << std::endl;
            }
        }
        usleep(100000); // 100ms to allow event processing
    }

    // Cleanup
    etcd_client_cleanup();
    std::cout << "Test completed. Total callbacks: " << callback_count << std::endl;
    return 0;
}
