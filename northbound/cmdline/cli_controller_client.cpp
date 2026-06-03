#include "cli_controller_client.h"

#include "controller.grpc.pb.h"
#include "controller.pb.h"

#include <grpcpp/grpcpp.h>
#include <curl/curl.h>
#include <json/json.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using controller::ConfigService;

namespace {

std::string g_grpc_addr;
std::string g_rest_base;
std::string g_node_uuid;
std::string g_token;
std::unique_ptr<ConfigService::Stub> g_stub;

constexpr int RPC_DEADLINE_SEC = 3;

void ensure_stub() {
    if (!g_stub && !g_grpc_addr.empty()) {
        auto ch = grpc::CreateChannel(g_grpc_addr, grpc::InsecureChannelCredentials());
        g_stub = ConfigService::NewStub(ch);
    }
}

void set_ctx(grpc::ClientContext& ctx) {
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(RPC_DEADLINE_SEC));
    if (!g_token.empty())
        ctx.AddMetadata("authorization", "Bearer " + g_token);
}

// Map a gRPC status to the CLI fallback decision.
cli_ctrl_status_t map_status(const grpc::Status& s) {
    if (s.ok())
        return CLI_CTRL_OK;
    switch (s.error_code()) {
        case grpc::StatusCode::UNAUTHENTICATED:
            return CLI_CTRL_AUTH;
        case grpc::StatusCode::UNAVAILABLE:
        case grpc::StatusCode::DEADLINE_EXCEEDED:
            return CLI_CTRL_UNAVAIL;
        case grpc::StatusCode::INVALID_ARGUMENT:
        case grpc::StatusCode::FAILED_PRECONDITION:
        case grpc::StatusCode::ALREADY_EXISTS:
        case grpc::StatusCode::NOT_FOUND:
            std::cerr << "controller rejected request: " << s.error_message() << std::endl;
            return CLI_CTRL_INVALID;
        default:
            std::cerr << "controller error (" << s.error_code() << "): "
                      << s.error_message() << std::endl;
            return CLI_CTRL_ERR;
    }
}

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

extern "C" {

void cli_controller_configure(const char* grpc_addr, const char* rest_base, const char* node_uuid) {
    g_grpc_addr = grpc_addr ? grpc_addr : "";
    g_rest_base = rest_base ? rest_base : "";
    g_node_uuid = node_uuid ? node_uuid : "";
    g_stub.reset();
}

int cli_controller_configured(void) {
    return (!g_grpc_addr.empty() && !g_node_uuid.empty()) ? 1 : 0;
}

const char* cli_controller_node_uuid(void) {
    return g_node_uuid.c_str();
}

int cli_controller_has_token(void) {
    return g_token.empty() ? 0 : 1;
}

cli_ctrl_status_t cli_controller_login(const char* username, const char* password) {
    if (g_rest_base.empty()) {
        std::cerr << "No controller REST URL configured (use --rest)" << std::endl;
        return CLI_CTRL_ERR;
    }
    CURL* curl = curl_easy_init();
    if (!curl)
        return CLI_CTRL_ERR;

    Json::Value body;
    body["username"] = username ? username : "";
    body["password"] = password ? password : "";
    Json::StreamWriterBuilder wb; wb["indentation"] = "";
    std::string payload = Json::writeString(wb, body);

    std::string url = g_rest_base + "/api/login";
    std::string resp;
    long http_code = 0;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        std::cerr << "login: cannot reach controller REST (" << curl_easy_strerror(rc) << ")" << std::endl;
        return CLI_CTRL_UNAVAIL;
    }
    if (http_code == 401) {
        std::cerr << "login: invalid username or password" << std::endl;
        return CLI_CTRL_AUTH;
    }
    if (http_code != 200) {
        std::cerr << "login: controller returned HTTP " << http_code << std::endl;
        return CLI_CTRL_ERR;
    }

    Json::Value root; Json::CharReaderBuilder rb; std::string errs;
    std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
    if (!reader->parse(resp.c_str(), resp.c_str() + resp.size(), &root, &errs) ||
            !root.isMember("token")) {
        std::cerr << "login: unexpected response from controller" << std::endl;
        return CLI_CTRL_ERR;
    }
    g_token = root["token"].asString();
    return CLI_CTRL_OK;
}

// Build a controller::HSIConfig from CLI args (desire_status intentionally unset).
static void fill_hsi(controller::HSIConfig* cfg, unsigned int user_id, unsigned int vlan_id,
    const char* account, const char* password, const char* pool_start, const char* pool_end,
    const char* subnet, const char* gateway) {
    cfg->set_user_id(std::to_string(user_id));
    cfg->set_vlan_id(std::to_string(vlan_id));
    cfg->set_account_name(account ? account : "");
    cfg->set_password(password ? password : "");
    cfg->set_dhcp_addr_pool(std::string(pool_start ? pool_start : "") + "-" + (pool_end ? pool_end : ""));
    cfg->set_dhcp_subnet(subnet ? subnet : "");
    cfg->set_dhcp_gateway(gateway ? gateway : "");
    cfg->set_dns_proxy_enable(true);
    cfg->set_tcp_conntrack_enable(true);
}

cli_ctrl_status_t cli_controller_apply_hsi(unsigned int user_id, unsigned int vlan_id,
    const char* account, const char* password, const char* pool_start, const char* pool_end,
    const char* subnet, const char* gateway) {
    ensure_stub();
    if (!g_stub) return CLI_CTRL_UNAVAIL;

    // Try create first; on ALREADY_EXISTS, update.
    {
        grpc::ClientContext ctx; set_ctx(ctx);
        controller::CreateHSIConfigRequest req;
        req.set_node_id(g_node_uuid);
        fill_hsi(req.mutable_config(), user_id, vlan_id, account, password,
            pool_start, pool_end, subnet, gateway);
        controller::HSIConfigResponse reply;
        grpc::Status s = g_stub->CreateHSIConfig(&ctx, req, &reply);
        if (s.ok())
            return CLI_CTRL_OK;
        if (s.error_code() != grpc::StatusCode::ALREADY_EXISTS)
            return map_status(s);
    }
    grpc::ClientContext ctx; set_ctx(ctx);
    controller::UpdateHSIConfigRequest req;
    req.set_node_id(g_node_uuid);
    req.set_user_id(std::to_string(user_id));
    fill_hsi(req.mutable_config(), user_id, vlan_id, account, password,
        pool_start, pool_end, subnet, gateway);
    controller::HSIConfigResponse reply;
    return map_status(g_stub->UpdateHSIConfig(&ctx, req, &reply));
}

cli_ctrl_status_t cli_controller_remove_hsi(unsigned int user_id) {
    ensure_stub();
    if (!g_stub) return CLI_CTRL_UNAVAIL;
    grpc::ClientContext ctx; set_ctx(ctx);
    controller::DeleteHSIConfigRequest req;
    req.set_node_id(g_node_uuid);
    req.set_user_id(std::to_string(user_id));
    google::protobuf::Empty reply;
    return map_status(g_stub->DeleteHSIConfig(&ctx, req, &reply));
}

cli_ctrl_status_t cli_controller_set_subscriber_count(int count) {
    ensure_stub();
    if (!g_stub) return CLI_CTRL_UNAVAIL;
    grpc::ClientContext ctx; set_ctx(ctx);
    controller::SetSubscriberCountRequest req;
    req.set_node_id(g_node_uuid);
    req.set_subscriber_count(count);
    google::protobuf::Empty reply;
    return map_status(g_stub->SetSubscriberCount(&ctx, req, &reply));
}

cli_ctrl_status_t cli_controller_dial(unsigned int user_id) {
    ensure_stub();
    if (!g_stub) return CLI_CTRL_UNAVAIL;
    grpc::ClientContext ctx; set_ctx(ctx);
    controller::PPPoEActionRequest req;
    req.set_node_id(g_node_uuid);
    req.set_user_id(std::to_string(user_id));
    google::protobuf::Empty reply;
    return map_status(g_stub->DialPPPoE(&ctx, req, &reply));
}

cli_ctrl_status_t cli_controller_hangup(unsigned int user_id) {
    ensure_stub();
    if (!g_stub) return CLI_CTRL_UNAVAIL;
    grpc::ClientContext ctx; set_ctx(ctx);
    controller::PPPoEActionRequest req;
    req.set_node_id(g_node_uuid);
    req.set_user_id(std::to_string(user_id));
    google::protobuf::Empty reply;
    return map_status(g_stub->HangupPPPoE(&ctx, req, &reply));
}

cli_ctrl_status_t cli_controller_add_dns(unsigned int user_id, const char* domain,
    const char* ip, unsigned int ttl) {
    ensure_stub();
    if (!g_stub) return CLI_CTRL_UNAVAIL;
    grpc::ClientContext ctx; set_ctx(ctx);
    controller::DNSRecordRequest req;
    req.set_node_id(g_node_uuid);
    req.set_user_id(std::to_string(user_id));
    controller::DNSRecord* rec = req.mutable_record();
    rec->set_domain(domain ? domain : "");
    rec->set_ip(ip ? ip : "");
    rec->set_ttl(ttl);
    google::protobuf::Empty reply;
    return map_status(g_stub->AddOrUpdateDNSRecord(&ctx, req, &reply));
}

cli_ctrl_status_t cli_controller_del_dns(unsigned int user_id, const char* domain) {
    ensure_stub();
    if (!g_stub) return CLI_CTRL_UNAVAIL;
    grpc::ClientContext ctx; set_ctx(ctx);
    controller::DeleteDNSRecordRequest req;
    req.set_node_id(g_node_uuid);
    req.set_user_id(std::to_string(user_id));
    req.set_domain(domain ? domain : "");
    google::protobuf::Empty reply;
    return map_status(g_stub->DeleteDNSRecord(&ctx, req, &reply));
}

cli_ctrl_status_t cli_controller_get_hsi(unsigned int user_id, char* out_buf, size_t out_len) {
    ensure_stub();
    if (!g_stub) return CLI_CTRL_UNAVAIL;
    grpc::ClientContext ctx; set_ctx(ctx);
    controller::GetHSIConfigRequest req;
    req.set_node_id(g_node_uuid);
    req.set_user_id(std::to_string(user_id));
    controller::HSIConfigResponse reply;
    grpc::Status s = g_stub->GetHSIConfig(&ctx, req, &reply);
    cli_ctrl_status_t rc = map_status(s);
    if (rc == CLI_CTRL_OK && out_buf && out_len > 0) {
        const controller::HSIConfig& c = reply.config();
        snprintf(out_buf, out_len,
            "user_id=%s vlan=%s account=%s pool=%s subnet=%s gateway=%s "
            "dns_proxy=%d tcp_conntrack=%d desire_status=%s",
            c.user_id().c_str(), c.vlan_id().c_str(), c.account_name().c_str(),
            c.dhcp_addr_pool().c_str(), c.dhcp_subnet().c_str(), c.dhcp_gateway().c_str(),
            c.dns_proxy_enable() ? 1 : 0, c.tcp_conntrack_enable() ? 1 : 0,
            c.desire_status().c_str());
    }
    return rc;
}

}  // extern "C"
