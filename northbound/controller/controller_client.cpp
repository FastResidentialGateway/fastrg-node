#include "controller_client.h"
#include <grpcpp/grpcpp.h>
#include "proto/controller.grpc.pb.h"
#include <memory>
#include <string>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <sstream>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using controller::NodeManagement;
using controller::NodeRegisterRequest;
using controller::NodeRegisterReply;
using controller::NodeHeartbeat;

class ControllerClient {
public:
    ControllerClient(std::shared_ptr<Channel> channel)
        : stub_(NodeManagement::NewStub(channel)) {}

    controller_status_t RegisterNode(const std::string& node_uuid, const std::string& ip, const std::string& version) {
        // Store registration info for potential re-registration
        last_node_uuid_ = node_uuid;
        last_ip_ = ip;
        last_version_ = version;
        NodeRegisterRequest request;
        request.set_node_uuid(node_uuid);
        request.set_ip(ip);
        request.set_version(version);

        NodeRegisterReply reply;
        ClientContext context;

        Status status = stub_->RegisterNode(&context, request, &reply);

        if (status.ok()) {
            if (reply.success()) {
                return CONTROLLER_SUCCESS;
            } else {
                std::cerr << "Register failed: " << reply.message() << std::endl;
                return CONTROLLER_REGISTER_FAILED;
            }
        } else {
            std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
            return CONTROLLER_ERROR;
        }
    }

    controller_status_t SendHeartbeat(const std::string& node_uuid, int64_t uptime_timestamp, const std::string& ip) {
        NodeHeartbeat heartbeat;
        heartbeat.set_node_uuid(node_uuid);
        heartbeat.set_uptime_timestamp(uptime_timestamp);
        heartbeat.set_ip(ip);
        
        // Read OS pretty name from /etc/os-release (e.g. "Ubuntu 26.04 LTS")
        std::string host_os = "Unknown OS";
        std::ifstream os_release("/etc/os-release");
        if (os_release.is_open()) {
            std::string line;
            while (std::getline(os_release, line)) {
                if (line.rfind("PRETTY_NAME=", 0) == 0) {
                    host_os = line.substr(12);
                    if (!host_os.empty() && host_os.front() == '"')
                        host_os = host_os.substr(1, host_os.size() - 2);
                    break;
                }
            }
        }
        heartbeat.set_host_os(host_os);

        google::protobuf::Empty reply;
        ClientContext context;

        Status status = stub_->Heartbeat(&context, heartbeat, &reply);

        if (status.ok()) {
            return CONTROLLER_SUCCESS;
        } else {
            std::cerr << "Heartbeat RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;

            // Check if error message contains "node not registered"
            std::string error_msg = status.error_message();
            if (error_msg.find("node not registered") != std::string::npos) {
                std::cout << "Node not registered, attempting to re-register..." << std::endl;

                // Attempt to re-register using stored credentials
                if (!last_node_uuid_.empty() && !last_ip_.empty() && !last_version_.empty()) {
                    controller_status_t reg_status = RegisterNode(last_node_uuid_, last_ip_, last_version_);
                    if (reg_status == CONTROLLER_SUCCESS) {
                        std::cout << "Re-registration successful, retrying heartbeat..." << std::endl;
                        // Retry heartbeat after successful registration
                        ClientContext retry_context;
                        Status retry_status = stub_->Heartbeat(&retry_context, heartbeat, &reply);
                        if (retry_status.ok()) {
                            return CONTROLLER_SUCCESS;
                        }
                    }
                }
            }

            return CONTROLLER_HEARTBEAT_FAILED;
        }
    }

    controller_status_t UnregisterNode(const std::string& node_uuid, const std::string& ip, const std::string& version) {
        NodeRegisterRequest request;
        request.set_node_uuid(node_uuid);
        request.set_ip(ip);
        request.set_version(version);

        google::protobuf::Empty reply;
        ClientContext context;

        Status status = stub_->UnregisterNode(&context, request, &reply);

        if (status.ok()) {
            return CONTROLLER_SUCCESS;
        } else {
            std::cerr << "Unregister RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
            return CONTROLLER_ERROR;
        }
    }

private:
    std::unique_ptr<NodeManagement::Stub> stub_;
    // Store last registration info for potential re-registration
    std::string last_node_uuid_;
    std::string last_ip_;
    std::string last_version_;
};

static std::unique_ptr<ControllerClient> g_client = nullptr;

extern "C" {

int controller_client_init(const char* server_address) {
    try {
        auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
        g_client = std::make_unique<ControllerClient>(channel);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize controller client: " << e.what() << std::endl;
        return -1;
    }
}

controller_status_t controller_register_node(const char* node_uuid, const char* ip, const char* version) {
    if (!g_client) {
        return CONTROLLER_ERROR;
    }
    return g_client->RegisterNode(std::string(node_uuid), std::string(ip), std::string(version));
}

controller_status_t controller_unregister_node(const char* node_uuid, const char* ip, const char* version) {
    if (!g_client) {
        return CONTROLLER_ERROR;
    }
    return g_client->UnregisterNode(std::string(node_uuid), std::string(ip), std::string(version));
}

controller_status_t controller_send_heartbeat(const char* node_uuid, long uptime_timestamp, const char* ip) {
    if (!g_client) {
        return CONTROLLER_ERROR;
    }
    return g_client->SendHeartbeat(std::string(node_uuid), static_cast<int64_t>(uptime_timestamp), std::string(ip));
}

void controller_client_cleanup(void) {
    if (!g_client) {
        return;
    }
    g_client.reset();
}

int get_local_ip_for_server(const char* server_address, unsigned char* local_ip, size_t ip_buffer_size) {
    // Parse server address to extract IP and port
    std::string server_str(server_address);
    size_t colon_pos = server_str.find_last_of(':');
    if (colon_pos == std::string::npos) {
        return -1;
    }

    std::string server_ip = server_str.substr(0, colon_pos);
    int server_port = std::stoi(server_str.substr(colon_pos + 1));

    // Create a socket and connect to the server to get local IP
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return -1;
    }

    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(sock, (struct sockaddr*)&local_addr, &addr_len) < 0) {
        close(sock);
        return -1;
    }

    const char* ip_str = inet_ntoa(local_addr.sin_addr);
    if (strlen(ip_str) >= ip_buffer_size) {
        close(sock);
        return -1;
    }

    strcpy((char *)local_ip, ip_str);
    close(sock);
    return 0;
}

} // extern "C"
