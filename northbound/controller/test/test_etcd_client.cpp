#include <iostream>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include "../etcd_client.h"

// etcd_client.o calls these from its watcher-thread self-event filter; the real
// definitions live in src/etcd_integration.c. This standalone test links only
// etcd_client.o, so provide minimal stand-ins.
extern "C" {
int parse_user_id(const char *user_id_str, int max_count) {
    (void)max_count;
    if (!user_id_str || user_id_str[0] == '\0')
        return -1;
    char *endptr;
    long val = strtol(user_id_str, &endptr, 10);
    if (endptr == user_id_str || *endptr != '\0')
        return -1;
    int ccb_id = (int)val - 1;
    return ccb_id < 0 ? -1 : ccb_id;
}
BOOL etcd_is_self_event(etcd_action_type_t action, U16 ccb_id, int64_t revision) {
    (void)action; (void)ccb_id; (void)revision;
    return FALSE;  // no pending-events tracking in the standalone test
}
}

// Global flag to control callback failure for testing
static bool simulate_callback_failure = false;
static int callback_count = 0;

void sync_request_callback(const char* node_id, void* user_data) {
    std::cout << "=== Sync Request Event ===" << std::endl;
    std::cout << "Node ID: " << node_id << std::endl;
    std::cout << "==========================" << std::endl;
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
    status = etcd_client_start_watch(test_node_uuid, sync_request_callback);

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
