#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../controller_client.h"

int main() {
    printf("Testing controller client...\n");

    // Initialize controller client
    const char* server_address = "127.0.0.1:50052";
    if (controller_client_init(server_address) != 0) {
        printf("Failed to initialize controller client\n");
        return -1;
    }
    printf("Controller client initialized successfully\n");

    // Get local IP
    char local_ip[16];
    if (get_local_ip_for_server(server_address, local_ip, sizeof(local_ip)) == 0) {
        printf("Local IP for server connection: %s\n", local_ip);
    } else {
        printf("Failed to get local IP\n");
    }

    // Test node registration (will fail without server, but tests the call)
    const char* node_uuid = "test-uuid-12345";
    const char* version = "1.0.0-test";
    controller_status_t status = controller_register_node(node_uuid, local_ip, version, "");

    switch (status) {
        case CONTROLLER_SUCCESS:
            printf("Node registration successful\n");
            break;
        case CONTROLLER_REGISTER_FAILED:
            printf("Node registration failed (server rejected)\n");
            break;
        case CONTROLLER_ERROR:
            printf("Node registration error (connection issue - expected without server)\n");
            break;
        default:
            printf("Unknown status: %d\n", status);
    }

    // Test heartbeat (will also fail without server)
    status = controller_send_heartbeat(node_uuid, 1697223600, local_ip);
    switch (status) {
        case CONTROLLER_SUCCESS:
            printf("Heartbeat successful\n");
            break;
        case CONTROLLER_HEARTBEAT_FAILED:
            printf("Heartbeat failed (server rejected)\n");
            break;
        case CONTROLLER_ERROR:
            printf("Heartbeat error (connection issue - expected without server)\n");
            break;
        default:
            printf("Unknown status: %d\n", status);
    }

    // Cleanup
    controller_client_cleanup();
    printf("Controller client cleaned up\n");

    printf("Test completed successfully\n");
    return 0;
}