#ifndef FastRG_GRPC_SERVER_H
#define FastRG_GRPC_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

void *fastrg_grpc_server_run(void *arg);
/* Blocks until the server thread reports whether it managed to bind its
 * listening addresses. Returns 0 when the server is listening, -1 when the
 * bind failed. Call it exactly once per server thread launch. */
int fastrg_grpc_server_wait_ready(void);
void fastrg_grpc_server_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // FastRG_GRPC_SERVER_H
