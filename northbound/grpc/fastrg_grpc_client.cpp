#include <iostream>
#include <iomanip>
#include <map>
#include <inttypes.h>
#include <grpc++/grpc++.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include "fastrg_node_grpc.h"
#include "../../src/fastrg.h"

#ifdef __cplusplus
extern "C" {
#endif

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using fastrgnodeservice::FastrgService;
using fastrgnodeservice::HsiRequest;
using fastrgnodeservice::HsiReply;

class FastRGNodeClient {
    public:
        FastRGNodeClient(std::shared_ptr<Channel> channel):stub_(FastrgService::NewStub(channel)) {}
    std::unique_ptr<FastrgService::Stub> stub_;
};

std::unique_ptr<FastRGNodeClient> fastrg_client;

void fastrg_grpc_client_connect(char *server_address) {
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    fastrg_client = std::make_unique<FastRGNodeClient>(channel);
}

void fastrg_grpc_apply_config(U16 user_id, U16 vlan_id, char *pppoe_account, 
    char *pppoe_password, char *dhcp_pool_start, char *dhcp_pool_end, 
    char *dhcp_subnet_mask, char *dhcp_gateway) {
    std::cout << "grpc client config" << std::endl;
    ConfigRequest request;
    ConfigReply reply;
    request.set_user_id(user_id);
    request.set_vlan_id(vlan_id);
    request.set_pppoe_account(pppoe_account);
    request.set_pppoe_password(pppoe_password);
    request.set_dhcp_pool_start(dhcp_pool_start);
    request.set_dhcp_pool_end(dhcp_pool_end);
    request.set_dhcp_subnet_mask(dhcp_subnet_mask);
    request.set_dhcp_gateway(dhcp_gateway);
    ClientContext context;
    Status status = fastrg_client->stub_->ApplyConfig(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client config ok" << std::endl;
    } else {
        std::cout << "grpc client config failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
    return;
}

void fastrg_grpc_remove_config(U16 user_id) {
    std::cout << "grpc client remove config" << std::endl;
    ConfigRequest request;
    ConfigReply reply;
    request.set_user_id(user_id);
    ClientContext context;
    Status status = fastrg_client->stub_->RemoveConfig(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client remove config ok" << std::endl;
    } else {
        std::cout << "grpc client remove config failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
    return;
}

void fastrg_grpc_set_subscriber(U16 subscriber_count) {
    std::cout << "grpc client set subscriber count" << std::endl;
    SetSubscriberCountRequest request;
    SetSubscriberCountReply reply;
    request.set_subscriber_count(subscriber_count);
    ClientContext context;
    Status status = fastrg_client->stub_->SetSubscriberCount(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client set subscriber count ok" << std::endl;
    } else {
        std::cout << "grpc client set subscriber count failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
    return;
}

void fastrg_grpc_hsi_connect(U16 user_id) {
    std::cout << "grpc client hsi connect" << std::endl;
    HsiRequest request;
    HsiReply reply;
    request.set_user_id(user_id);
    ClientContext context;
    Status status = fastrg_client->stub_->ConnectHsi(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client hsi connect ok" << std::endl;
    } else {
        std::cout << "grpc client hsi connect failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
    return;
}

void fastrg_grpc_hsi_disconnect(U16 user_id, bool force) {
    std::cout << "grpc client hsi disconnect" << std::endl;
    HsiRequest request;
    HsiReply reply;
    request.set_user_id(user_id);
    request.set_force(force);
    ClientContext context;
    Status status = fastrg_client->stub_->DisconnectHsi(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client hsi disconnect ok" << std::endl;
    } else {
        std::cout << "grpc client hsi disconnect failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
    return;
}

void fastrg_grpc_dhcp_server_start(U8 user_id) {
    std::cout << "grpc client dhcp server start" << std::endl;
    DhcpServerRequest request;
    DhcpServerReply reply;
    request.set_user_id(user_id);
    ClientContext context;
    Status status = fastrg_client->stub_->DhcpServerStart(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client dhcp server start ok" << std::endl;
    } else {
        std::cout << "grpc client dhcp server start failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
    return;
}

void fastrg_grpc_dhcp_server_stop(U8 user_id) {
    std::cout << "grpc client dhcp server stop" << std::endl;
    DhcpServerRequest request;
    DhcpServerReply reply;
    request.set_user_id(user_id);
    ClientContext context;
    Status status = fastrg_client->stub_->DhcpServerStop(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client dhcp server stop ok" << std::endl;
    } else {
        std::cout << "grpc client dhcp server stop failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
    return;
}

void fastrg_grpc_hsi_snat_set(U16 user_id, U16 eport, char *dip, U16 iport) {
    std::cout << "grpc client hsi snat set" << std::endl;
    SnatConfigRequest request;
    SnatConfigReply reply;
    request.set_user_id(user_id);
    request.set_eport(eport);
    request.set_dip(dip);
    request.set_iport(iport);
    ClientContext context;
    Status status = fastrg_client->stub_->SetSnatConfig(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client hsi snat set ok" << std::endl;
    } else {
        std::cout << "grpc client hsi snat set failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
    return;
}

void fastrg_grpc_hsi_snat_unset(U16 user_id, U16 eport) {
    std::cout << "grpc client hsi snat unset" << std::endl;
    SnatConfigRequest request;
    SnatConfigReply reply;
    request.set_user_id(user_id);
    request.set_eport(eport);
    ClientContext context;
    Status status = fastrg_client->stub_->RemoveSnatConfig(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client hsi snat unset ok" << std::endl;
    } else {
        std::cout << "grpc client hsi snat unset failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
    return;
}

void fastrg_grpc_get_port_fwd_info(U16 user_id) {
    std::cout << "grpc client getting port forwarding info" << std::endl;
    PortFwdInfoRequest request;
    PortFwdInfoReply reply;
    request.set_user_id(user_id);
    ClientContext context;
    Status status = fastrg_client->stub_->GetPortFwdInfo(&context, request, &reply);
    if (status.ok()) {
        std::cout << "Port Forwarding entries for User " << reply.user_id() << ":" << std::endl;
        if (reply.entries_size() == 0) {
            std::cout << "  (no entries)" << std::endl;
        } else {
            std::cout << "  " << std::left
                      << std::setw(20) << "External DPort"
                      << std::setw(22) << "Internal Dest IP"
                      << std::setw(20) << "Internal DPort"
                      << std::setw(12) << "Hit Count"
                      << std::endl;
            std::cout << "  " << std::string(74, '-') << std::endl;
            for(int i=0; i<reply.entries_size(); i++) {
                const PortFwdEntry& entry = reply.entries(i);
                std::cout << "  " << std::left
                          << std::setw(20) << entry.eport()
                          << std::setw(22) << entry.dip()
                          << std::setw(20) << entry.iport()
                          << std::setw(12) << entry.hit_count()
                          << std::endl;
            }
        }
    } else {
        std::cout << "grpc client get port fwd info failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_get_arp_table(U16 user_id, U32 max_count) {
    ArpTableRequest request;
    ArpTableReply reply;
    request.set_user_id(user_id);
    request.set_max_count(max_count);
    ClientContext context;
    Status status = fastrg_client->stub_->GetArpTable(&context, request, &reply);
    if (status.ok()) {
        std::cout << "ARP Table for User " << reply.user_id()
                  << " (showing " << reply.entries_size()
                  << " of " << reply.total_count() << " entries):" << std::endl;
        if (reply.entries_size() == 0) {
            std::cout << "  (no entries)" << std::endl;
        } else {
            std::cout << "  " << std::left
                      << std::setw(15) << "Entry ID"
                      << std::setw(20) << "IP Address"
                      << std::setw(20) << "MAC Address"
                      << std::endl;
            std::cout << "  " << std::string(55, '-') << std::endl;
            for(int i=0; i<reply.entries_size(); i++) {
                const ArpTableEntry& entry = reply.entries(i);
                std::cout << "  " << std::left
                          << std::setw(15) << entry.entry_id()
                          << std::setw(20) << entry.ip()
                          << std::setw(20) << entry.mac()
                          << std::endl;
            }
        }
    } else {
        std::cout << "grpc client get arp table failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_get_system_info() {
    std::cout << "grpc client getting FastRG system and node info" << std::endl;
    google::protobuf::Empty request;
    FastrgSystemInfo reply_fastrg_system;
    NodeStatus reply_node_status;
    ClientContext context_fastrg_system, context_node_status;
    Status status = fastrg_client->stub_->GetFastrgSystemInfo(&context_fastrg_system, request, &reply_fastrg_system);
    if (status.ok()) {
        std::cout << "grpc client get FastRG system info ok" << std::endl;
        std::cout << "  FastRG version: " << reply_fastrg_system.base_info().fastrg_version() << std::endl;
        std::cout << "  Build date: " << reply_fastrg_system.base_info().build_date() << std::endl;
        std::cout << "  DPDK version: " << reply_fastrg_system.base_info().dpdk_version() << std::endl;
        std::cout << "  DPDK EAL args: " << reply_fastrg_system.base_info().dpdk_eal_args() << std::endl;
        std::cout << "  Number of subscribers: " << reply_fastrg_system.base_info().num_users() << std::endl;
    } else {
        std::cout << "grpc client get info failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }

    context_node_status.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    status = fastrg_client->stub_->GetNodeStatus(&context_node_status, request, &reply_node_status);
    if (status.ok()) {
        std::cout << "grpc client get node status ok" << std::endl;
        std::cout << "  Node OS version: " << reply_node_status.node_os_version() << std::endl;
        std::cout << "  Node uptime (seconds): " << reply_node_status.node_uptime() << std::endl;
        std::cout << "  Node IP address: " << reply_node_status.node_ip_info() << std::endl;
        std::cout << "  Health status: " << (reply_node_status.healthy() ? "Healthy" : "Unhealthy") << std::endl;
    } else {
        std::cout << "grpc client get node status failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_get_system_stats() {
    std::cout << "grpc client getting FastRG system stats" << std::endl;
    google::protobuf::Empty request;
    FastrgSystemStatsInfo reply;
    ClientContext context;
    Status status = fastrg_client->stub_->GetFastrgSystemStats(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client get FastRG system stats ok" << std::endl;
        std::cout << "  NICs: " << std::endl;
        for(int i=0; i<reply.nics_size() && i<reply.stats_size(); i++) {
            const NicDriverInfo& nic_info = reply.nics(i);
            std::cout << "    NIC " << i << ":" << std::endl;
            std::cout << "      Driver name: " << nic_info.driver_name() << std::endl;
            std::cout << "      PCI address: " << nic_info.pci_addr() << std::endl;
            std::cout << "      MAC address: ";
            std::string mac_bin = nic_info.mac_addr();
            const uint8_t* mac_bytes = reinterpret_cast<const uint8_t*>(mac_bin.data());
            for(size_t j=0; j<mac_bin.size(); j++)
                printf("%02x%c", mac_bytes[j], (j == mac_bin.size()-1 ? '\n' : ':'));
            const Statistics& stats = reply.stats(i);
            std::cout << "      Rx packets: " << stats.rx_packets() << std::endl;
            std::cout << "      Tx packets: " << stats.tx_packets() << std::endl;
            std::cout << "      Rx bytes: " << stats.rx_bytes() << std::endl;
            std::cout << "      Tx bytes: " << stats.tx_bytes() << std::endl;
            std::cout << "      Rx errors: " << stats.rx_errors() << std::endl;
            std::cout << "      Tx errors: " << stats.tx_errors() << std::endl;
            std::cout << "      Rx dropped: " << stats.rx_dropped() << std::endl;
            for(int j=0; j<stats.per_user_stats_size()-1; j++) {
                const PerUserStatistics& per_user_stats = stats.per_user_stats(j);
                std::cout << "        User ID: " << per_user_stats.user_id() << std::endl;
                std::cout << "          Rx packets: " << per_user_stats.rx_packets() << std::endl;
                std::cout << "          Tx packets: " << per_user_stats.tx_packets() << std::endl;
                std::cout << "          Rx bytes: " << per_user_stats.rx_bytes() << std::endl;
                std::cout << "          Tx bytes: " << per_user_stats.tx_bytes() << std::endl;
                std::cout << "          Dropped packets: " << per_user_stats.dropped_packets() << std::endl;
                std::cout << "          Dropped bytes: " << per_user_stats.dropped_bytes() << std::endl;
            }
            const PerUserStatistics& per_user_stats = stats.per_user_stats(stats.per_user_stats_size()-1);
            std::cout << "        Unknown user: " << std::endl;
            std::cout << "          Rx packets: " << per_user_stats.rx_packets() << std::endl;
            std::cout << "          Tx packets: " << per_user_stats.tx_packets() << std::endl;
            std::cout << "          Rx bytes: " << per_user_stats.rx_bytes() << std::endl;
            std::cout << "          Tx bytes: " << per_user_stats.tx_bytes() << std::endl;
            std::cout << "          Dropped packets: " << per_user_stats.dropped_packets() << std::endl;
            std::cout << "          Dropped bytes: " << per_user_stats.dropped_bytes() << std::endl;
        }
        if (reply.lcore_usage_size() > 0) {
            std::map<uint32_t, std::pair<uint64_t, uint64_t>> prev;
            for (int i = 0; i < reply.lcore_usage_size(); i++) {
                const LcoreUsage& lu = reply.lcore_usage(i);
                prev[lu.lcore_id()] = {lu.busy_cycles(), lu.total_cycles()};
            }
            sleep(1);
            FastrgSystemStatsInfo reply2;
            ClientContext context2;
            Status status2 = fastrg_client->stub_->GetFastrgSystemStats(&context2, request, &reply2);
            if (status2.ok() && reply2.lcore_usage_size() > 0) {
                std::cout << "  Lcore Usage (1s sample):" << std::endl;
                for (int i = 0; i < reply2.lcore_usage_size(); i++) {
                    const LcoreUsage& lu = reply2.lcore_usage(i);
                    auto it = prev.find(lu.lcore_id());
                    double busyness = 0.0;
                    if (it != prev.end()) {
                        uint64_t d_busy = lu.busy_cycles() - it->second.first;
                        uint64_t d_total = lu.total_cycles() - it->second.second;
                        if (d_total > 0)
                            busyness = (double)d_busy * 100.0 / (double)d_total;
                    }
                    printf("    lcore %2u [%-16s]   busy: %6.2f%%\n",
                        lu.lcore_id(), lu.role().c_str(), busyness);
                }
                reply = std::move(reply2);
            }
        }
        if (reply.heap_stats_size() > 0) {
            for (int i = 0; i < reply.heap_stats_size(); i++) {
                const HeapStats& hs = reply.heap_stats(i);
                printf("  Heap (socket %u):\n", hs.socket_id());
                printf("    Total: %lu MB  Used: %lu MB  Free: %lu MB  Largest free block: %lu MB\n",
                    hs.total_bytes() / (1024 * 1024),
                    hs.used_bytes() / (1024 * 1024),
                    hs.free_bytes() / (1024 * 1024),
                    hs.largest_free_blk() / (1024 * 1024));
            }
        }
        if (reply.mempool_stats_size() > 0) {
            std::cout << "  Mempools:" << std::endl;
            for (int i = 0; i < reply.mempool_stats_size(); i++) {
                const MempoolStats& ms = reply.mempool_stats(i);
                printf("    %-30s  size: %5u  avail: %5u  in_use: %5u\n",
                    ms.name().c_str(), ms.size(), ms.avail_count(), ms.in_use_count());
            }
        }
        if (reply.hugepage_pinned_bytes() > 0) {
            printf("  Hugepage pinned: %lu MB\n",
                reply.hugepage_pinned_bytes() / (1024 * 1024));
        }
    } else {
        std::cout << "grpc client get system stats failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_get_system_xstats() {
    std::cout << "grpc client getting FastRG system xstats" << std::endl;
    google::protobuf::Empty request;
    FastrgSystemXStatsInfo reply;
    ClientContext context;
    Status status = fastrg_client->stub_->GetFastrgSystemXStats(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client get FastRG system xstats ok" << std::endl;
        for(int i=0; i<reply.nic_xstats_size(); i++) {
            const NicXStats& nic_xstats = reply.nic_xstats(i);
            std::cout << "  Port " << nic_xstats.port_id() << " xstats:" << std::endl;
            for(int j=0; j<nic_xstats.xstats_size(); j++) {
                const XStat& xstat = nic_xstats.xstats(j);
                printf("    %-48s: %" PRIu64 "\n", xstat.name().c_str(), xstat.value());
            }
        }
    } else {
        std::cout << "grpc client get system xstats failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_get_hsi_info() {
    std::cout << "grpc client getting hsi info" << std::endl;
    google::protobuf::Empty request;
    FastrgHsiInfo reply;
    ClientContext context;
    Status status = fastrg_client->stub_->GetFastrgHsiInfo(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client get hsi info ok" << std::endl;
        for(int i=0; i<reply.hsi_infos_size(); i++) {
            const HsiInfo& hsi_info = reply.hsi_infos(i);
            std::cout << "  HSI " << i << ":" << std::endl;
            std::cout << "    User ID: " << hsi_info.user_id() << std::endl;
            std::cout << "    VLAN ID: " << hsi_info.vlan_id() << std::endl;
            std::cout << "    Status: " << hsi_info.status() << std::endl;
            std::cout << "    Account: " << hsi_info.account() << std::endl;
            std::cout << "    Password: " << hsi_info.password() << std::endl;
            std::cout << "    Session ID: " << hsi_info.session_id() << std::endl;
            std::cout << "    IP address: " << hsi_info.ip_addr() << std::endl;
            std::cout << "    Gateway: " << hsi_info.gateway() << std::endl;
            std::cout << "    DNS servers: ";
            for(int j=0; j<hsi_info.dnss_size(); j++) {
                std::cout << hsi_info.dnss(j);
                if (j < hsi_info.dnss_size() - 1)
                    std::cout << ", ";
            }
            std::cout << std::endl;
        }
    } else {
        std::cout << "grpc client get hsi info failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
}

int fastrg_grpc_get_hsi_user(U16 user_id, char *out_buf, U32 out_len) {
    if (!out_buf || out_len == 0)
        return -1;
    google::protobuf::Empty request;
    FastrgHsiInfo reply;
    ClientContext context;
    Status status = fastrg_client->stub_->GetFastrgHsiInfo(&context, request, &reply);
    if (!status.ok())
        return -1;
    for (int i = 0; i < reply.hsi_infos_size(); i++) {
        const HsiInfo& h = reply.hsi_infos(i);
        if (h.user_id() != user_id)
            continue;
        snprintf(out_buf, out_len,
            "user_id=%u vlan=%u account=%s status=%s ip=%s gateway=%s",
            h.user_id(), h.vlan_id(), h.account().c_str(), h.status().c_str(),
            h.ip_addr().c_str(), h.gateway().c_str());
        return 0;
    }
    snprintf(out_buf, out_len, "(no running state for user %u)", user_id);
    return 0;
}

void fastrg_grpc_get_dhcp_info() {
    std::cout << "grpc client getting dhcp info" << std::endl;
    google::protobuf::Empty request;
    FastrgDhcpInfo reply;
    ClientContext context;
    Status status = fastrg_client->stub_->GetFastrgDhcpInfo(&context, request, &reply);
    if (status.ok()) {
        std::cout << "grpc client get dhcp info ok" << std::endl;
        for(int i=0; i<reply.dhcp_infos_size(); i++) {
            const DhcpInfo& dhcp_info = reply.dhcp_infos(i);
            std::cout << "  DHCP " << i << ":" << std::endl;
            std::cout << "    User ID: " << dhcp_info.user_id() << std::endl;
            std::cout << "    Status: " << dhcp_info.status() << std::endl;
            std::cout << "    IP Range: " << dhcp_info.ip_range() << std::endl;
            std::cout << "    Subnet Mask: " << dhcp_info.subnet_mask() << std::endl;
            std::cout << "    Gateway: " << dhcp_info.gateway() << std::endl;
            std::cout << "    In-use IPs: ";
            for(int j=0; j<dhcp_info.inuse_ips_size(); j++) {
                std::cout << dhcp_info.inuse_ips(j);
                if (j < dhcp_info.inuse_ips_size() - 1)
                    std::cout << ", ";
            }
            std::cout << std::endl;
            std::cout << "    DNS servers: ";
            for(int j=0; j<dhcp_info.dnss_size(); j++) {
                std::cout << dhcp_info.dnss(j);
                if (j < dhcp_info.dnss_size() - 1)
                    std::cout << ", ";
            }
            std::cout << std::endl;
        }
    } else {
        std::cout << "grpc client get dhcp info failed: " << std::endl;
        std::cout << "  Error code: " << status.error_code() << std::endl;
        std::cout << "  Error message: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_add_dns_record(U16 user_id, char *domain, char *ip, U32 ttl) {
    DnsRecordRequest request;
    DnsRecordReply reply;
    request.set_user_id(user_id);
    request.set_domain(domain);
    request.set_ip(ip);
    request.set_ttl(ttl);
    ClientContext context;
    Status status = fastrg_client->stub_->AddDnsRecord(&context, request, &reply);
    if (status.ok()) {
        std::cout << "DNS record added: " << domain << " -> " << ip << " (TTL: " << ttl << ")" << std::endl;
    } else {
        std::cout << "Failed to add DNS record: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_remove_dns_record(U16 user_id, char *domain) {
    DnsRecordRequest request;
    DnsRecordReply reply;
    request.set_user_id(user_id);
    request.set_domain(domain);
    ClientContext context;
    Status status = fastrg_client->stub_->RemoveDnsRecord(&context, request, &reply);
    if (status.ok()) {
        std::cout << "DNS record removed: " << domain << std::endl;
    } else {
        std::cout << "Failed to remove DNS record: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_get_dns_cache(U16 user_id) {
    DnsCacheRequest request;
    DnsCacheReply reply;
    request.set_user_id(user_id);
    ClientContext context;
    Status status = fastrg_client->stub_->GetDnsCache(&context, request, &reply);
    if (status.ok()) {
        std::cout << "DNS Cache for User " << reply.user_id()
                  << " (" << reply.total_entries() << " entries):" << std::endl;
        if (reply.entries_size() == 0) {
            std::cout << "  (empty)" << std::endl;
        } else {
            std::cout << "  " << std::left
                      << std::setw(40) << "Domain"
                      << std::setw(8) << "Type"
                      << std::setw(10) << "TTL"
                      << std::setw(14) << "Remaining"
                      << std::setw(10) << "Hits"
                      << std::endl;
            std::cout << "  " << std::string(82, '-') << std::endl;
            for(int i=0; i<reply.entries_size(); i++) {
                const DnsCacheEntry& entry = reply.entries(i);
                std::cout << "  " << std::left
                          << std::setw(40) << entry.domain()
                          << std::setw(8) << entry.qtype()
                          << std::setw(10) << entry.ttl()
                          << std::setw(14) << entry.remaining_ttl()
                          << std::setw(10) << entry.hit_count()
                          << std::endl;
            }
        }
    } else {
        std::cout << "Failed to get DNS cache: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_get_dns_static(U16 user_id) {
    DnsStaticRequest request;
    DnsStaticReply reply;
    request.set_user_id(user_id);
    ClientContext context;
    Status status = fastrg_client->stub_->GetDnsStaticRecords(&context, request, &reply);
    if (status.ok()) {
        std::cout << "DNS Static Records for User " << reply.user_id()
                  << " (" << reply.total_entries() << " entries):" << std::endl;
        if (reply.entries_size() == 0) {
            std::cout << "  (empty)" << std::endl;
        } else {
            std::cout << "  " << std::left
                      << std::setw(40) << "Domain"
                      << std::setw(20) << "IP"
                      << std::setw(10) << "TTL"
                      << std::endl;
            std::cout << "  " << std::string(70, '-') << std::endl;
            for(int i=0; i<reply.entries_size(); i++) {
                const DnsStaticEntry& entry = reply.entries(i);
                std::cout << "  " << std::left
                          << std::setw(40) << entry.domain()
                          << std::setw(20) << entry.ip()
                          << std::setw(10) << entry.ttl()
                          << std::endl;
            }
        }
    } else {
        std::cout << "Failed to get DNS static records: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_flush_dns_cache(U16 user_id) {
    DnsCacheFlushRequest request;
    DnsCacheFlushReply reply;
    request.set_user_id(user_id);
    ClientContext context;
    Status status = fastrg_client->stub_->FlushDnsCache(&context, request, &reply);
    if (status.ok()) {
        std::cout << "DNS cache flushed: " << reply.flushed_count() << " entries removed" << std::endl;
    } else {
        std::cout << "Failed to flush DNS cache: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_set_dns_proxy(U16 user_id, bool enable) {
    SetDnsProxyRequest request;
    SetDnsProxyReply reply;
    request.set_user_id(user_id);
    request.set_enable(enable);
    ClientContext context;
    Status status = fastrg_client->stub_->SetDnsProxy(&context, request, &reply);
    if (status.ok()) {
        std::cout << "dns_proxy " << (enable ? "enabled" : "disabled")
                  << " for user " << user_id << std::endl;
    } else {
        std::cout << "Failed to set dns_proxy: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_set_tcp_conntrack(U16 user_id, bool enable) {
    SetTcpConntrackRequest request;
    SetTcpConntrackReply reply;
    request.set_user_id(user_id);
    request.set_enable(enable);
    ClientContext context;
    Status status = fastrg_client->stub_->SetTcpConntrack(&context, request, &reply);
    if (status.ok()) {
        std::cout << "tcp_conntrack " << (enable ? "enabled" : "disabled")
                  << " for user " << user_id << std::endl;
    } else {
        std::cout << "Failed to set tcp_conntrack: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_pdump_start(U16 direction, U16 subscriber, const char *filter, U32 size_mb) {
    fastrgnodeservice::PdumpRequest request;
    fastrgnodeservice::PdumpReply reply;
    request.set_direction(direction);
    request.set_subscriber(subscriber);
    if (filter && filter[0])
        request.set_filter(filter);
    request.set_size_limit_mb(size_mb);
    ClientContext context;
    Status status = fastrg_client->stub_->PdumpStart(&context, request, &reply);
    if (status.ok()) {
        std::cout << "pdump capture started: " << reply.pcap_file() << std::endl;
    } else {
        std::cout << "Failed to start pdump capture: " << status.error_message() << std::endl;
    }
}

void fastrg_grpc_pdump_stop(U16 direction, U16 subscriber) {
    fastrgnodeservice::PdumpRequest request;
    fastrgnodeservice::PdumpReply reply;
    request.set_direction(direction);
    request.set_subscriber(subscriber);
    ClientContext context;
    Status status = fastrg_client->stub_->PdumpStop(&context, request, &reply);
    if (status.ok()) {
        std::cout << "pdump capture stopped" << std::endl;
    } else {
        std::cout << "Failed to stop pdump capture: " << status.error_message() << std::endl;
    }
}

#ifdef __cplusplus
}
#endif
