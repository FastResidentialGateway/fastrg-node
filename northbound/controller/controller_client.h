#ifndef _CONTROLLER_CLIENT_H_
#define _CONTROLLER_CLIENT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONTROLLER_SUCCESS = 0,
    CONTROLLER_ERROR = -1,
    CONTROLLER_REGISTER_FAILED = -2,
    CONTROLLER_HEARTBEAT_FAILED = -3
} controller_status_t;

/* Initialize the controller client */
int controller_client_init(const char* server_address);

/* Register node with the controller */
controller_status_t controller_register_node(const char* node_uuid, const char* ip, const char* version, const char* location, uint32_t grpc_port);

/* Send heartbeat to the controller */
controller_status_t controller_send_heartbeat(const char* node_uuid, long uptime_timestamp, const char* ip);

/* Report graceful shutdown to the controller (marks node inactive, keeps it visible) */
controller_status_t controller_report_shutdown(const char* node_uuid);

/* Cleanup the controller client */
void controller_client_cleanup(void);

/* Get local IP address used for connection to server */
int get_local_ip_for_server(const char* server_address, unsigned char* local_ip, size_t ip_buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_CLIENT_H_ */
