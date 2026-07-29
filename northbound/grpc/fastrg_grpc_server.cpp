#include <chrono>
#include <iostream>
#include <mutex>
#include <grpc++/grpc++.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include "fastrg_node_grpc.h"
#include "../../src/fastrg.h"

static std::mutex server_mutex;
static grpc::Server *running_server;
static bool shutdown_requested;

#ifdef __cplusplus
extern "C" {
#endif

void *fastrg_grpc_server_run(void *arg)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)arg;

    std::string unix_sock_path(fastrg_ccb->unix_sock_path);
    std::string ip_address(fastrg_ccb->node_grpc_ip_port);
    std::cout << "grpc server starting..." << std::endl;
    grpc::ServerBuilder builder;

    grpc::EnableDefaultHealthCheckService(true);
    std::shared_ptr<grpc::ServerCredentials> cred = grpc::InsecureServerCredentials();
    builder.AddListeningPort(unix_sock_path, cred);
    builder.AddListeningPort(ip_address, cred);
    FastRGNodeServiceImpl fastrg_service(fastrg_ccb);
    builder.RegisterService(&fastrg_service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::cerr << "grpc server failed to start" << std::endl;
        return NULL;
    }

    std::cout << "grpc server listening on " << unix_sock_path << " and " << ip_address << std::endl;
    {
        std::lock_guard<std::mutex> lock(server_mutex);
        running_server = server.get();
        if (shutdown_requested)
            running_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(3));
    }
    server->Wait();

    {
        std::lock_guard<std::mutex> lock(server_mutex);
        if (running_server == server.get())
            running_server = NULL;
    }
    return NULL;
}

void fastrg_grpc_server_shutdown(void)
{
    std::lock_guard<std::mutex> lock(server_mutex);
    shutdown_requested = true;
    if (running_server != NULL)
        running_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(3));
}

#ifdef __cplusplus
}
#endif
