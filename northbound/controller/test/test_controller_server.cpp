#include <iostream>
#include <memory>
#include <string>
#include <grpcpp/grpcpp.h>
#include "../proto/controller.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using controller::NodeManagement;
using controller::NodeRegisterRequest;
using controller::NodeRegisterReply;
using controller::NodeHeartbeat;
using controller::NodeShutdownRequest;
using google::protobuf::Empty;

class NodeManagementServiceImpl final : public NodeManagement::Service {
public:
    Status RegisterNode(ServerContext* context, const NodeRegisterRequest* request,
                        NodeRegisterReply* reply) override {
        std::cout << "Node registration request received:" << std::endl;
        std::cout << "  UUID: " << request->node_uuid() << std::endl;
        std::cout << "  IP: " << request->ip() << std::endl;
        std::cout << "  Version: " << request->version() << std::endl;
        
        reply->set_success(true);
        reply->set_message("Node registered successfully");
        
        return Status::OK;
    }
    
    Status Heartbeat(ServerContext* context, const NodeHeartbeat* request,
                     Empty* reply) override {
        std::cout << "Heartbeat received from node " << request->node_uuid() 
                  << " at " << request->ip() 
                  << " (uptime: " << request->uptime_timestamp() << ")" << std::endl;
        
        return Status::OK;
    }

    Status ReportShutdown(ServerContext* context, const NodeShutdownRequest* request,
                          Empty* reply) override {
        std::cout << "Shutdown reported by node " << request->node_uuid() << std::endl;

        return Status::OK;
    }
};

int main() {
    std::string server_address("0.0.0.0:50052");
    NodeManagementServiceImpl service;
    
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Controller server listening on " << server_address << std::endl;
    
    server->Wait();
    
    return 0;
}