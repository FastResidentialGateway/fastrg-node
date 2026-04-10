#include <grpc++/grpc++.h>
#include "fastrg_node.grpc.pb.h"
#include "../../src/fastrg.h"

using namespace std;
using namespace fastrgnodeservice;

class FastRGNodeServiceImpl final : public fastrgnodeservice::FastrgService::Service
{
    public:
    explicit FastRGNodeServiceImpl(FastRG_t* ctx) : fastrg_ccb(ctx) {}

    ::grpc::Status ApplyConfig(::grpc::ServerContext* context, const ::fastrgnodeservice::ConfigRequest* request, ::fastrgnodeservice::ConfigReply* response) override;
    ::grpc::Status RemoveConfig(::grpc::ServerContext* context, const ::fastrgnodeservice::ConfigRequest* request, ::fastrgnodeservice::ConfigReply* response) override;
    ::grpc::Status SetSubscriberCount(::grpc::ServerContext* context, const ::fastrgnodeservice::SetSubscriberCountRequest* request, ::fastrgnodeservice::SetSubscriberCountReply* response) override;
    ::grpc::Status ConnectHsi(::grpc::ServerContext* context, const ::fastrgnodeservice::HsiRequest* request, ::fastrgnodeservice::HsiReply* response) override;
    ::grpc::Status DisconnectHsi(::grpc::ServerContext* context, const ::fastrgnodeservice::HsiRequest* request, ::fastrgnodeservice::HsiReply* response) override;
    ::grpc::Status DhcpServerStart(::grpc::ServerContext* context, const ::fastrgnodeservice::DhcpServerRequest* request, ::fastrgnodeservice::DhcpServerReply* response) override;
    ::grpc::Status DhcpServerStop(::grpc::ServerContext* context, const ::fastrgnodeservice::DhcpServerRequest* request, ::fastrgnodeservice::DhcpServerReply* response) override;
    ::grpc::Status SetSnatConfig(::grpc::ServerContext* context, const ::fastrgnodeservice::SnatConfigRequest* request, ::fastrgnodeservice::SnatConfigReply* response) override;
    ::grpc::Status RemoveSnatConfig(::grpc::ServerContext* context, const ::fastrgnodeservice::SnatConfigRequest* request, ::fastrgnodeservice::SnatConfigReply* response) override;
    ::grpc::Status GetFastrgSystemInfo(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::FastrgSystemInfo* response) override;
    ::grpc::Status GetFastrgSystemStats(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::FastrgSystemStatsInfo* response) override;
    ::grpc::Status GetFastrgSystemXStats(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::FastrgSystemXStatsInfo* response) override;
    ::grpc::Status GetFastrgHsiInfo(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::FastrgHsiInfo* response) override;
    ::grpc::Status GetFastrgDhcpInfo(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::FastrgDhcpInfo* response) override;
    ::grpc::Status GetPortFwdInfo(::grpc::ServerContext* context, const ::fastrgnodeservice::PortFwdInfoRequest* request, ::fastrgnodeservice::PortFwdInfoReply* response) override;
    ::grpc::Status GetArpTable(::grpc::ServerContext* context, const ::fastrgnodeservice::ArpTableRequest* request, ::fastrgnodeservice::ArpTableReply* response) override;
    ::grpc::Status GetNodeStatus(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::NodeStatus* response) override;
    ::grpc::Status AddDnsRecord(::grpc::ServerContext* context, const ::fastrgnodeservice::DnsRecordRequest* request, ::fastrgnodeservice::DnsRecordReply* response) override;
    ::grpc::Status RemoveDnsRecord(::grpc::ServerContext* context, const ::fastrgnodeservice::DnsRecordRequest* request, ::fastrgnodeservice::DnsRecordReply* response) override;
    ::grpc::Status GetDnsCache(::grpc::ServerContext* context, const ::fastrgnodeservice::DnsCacheRequest* request, ::fastrgnodeservice::DnsCacheReply* response) override;
    ::grpc::Status GetDnsStaticRecords(::grpc::ServerContext* context, const ::fastrgnodeservice::DnsStaticRequest* request, ::fastrgnodeservice::DnsStaticReply* response) override;
    ::grpc::Status FlushDnsCache(::grpc::ServerContext* context, const ::fastrgnodeservice::DnsCacheFlushRequest* request, ::fastrgnodeservice::DnsCacheFlushReply* response) override;

    private:
    FastRG_t* fastrg_ccb;
};
