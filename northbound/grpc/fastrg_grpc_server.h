#ifndef FastRG_GRPC_SERVER_H
#define FastRG_GRPC_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

void *fastrg_grpc_server_run(void *arg);
void fastrg_grpc_server_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // FastRG_GRPC_SERVER_H
