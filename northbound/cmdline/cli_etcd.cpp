#include "cli_etcd.h"

#include <etcd/Client.hpp>
#include <etcd/Response.hpp>
#include <etcd/v3/action_constants.hpp>
#include <json/json.h>

#include <ctime>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

namespace {

std::string g_endpoints;
std::string g_node_uuid;
std::unique_ptr<etcd::Client> g_client;

constexpr int CAS_MAX_RETRIES = 5;

etcd::Client* client() {
    if (!g_client && !g_endpoints.empty())
        g_client = std::make_unique<etcd::Client>(g_endpoints);
    return g_client.get();
}

std::string iso_now() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string to_json(const Json::Value& v) {
    Json::StreamWriterBuilder wb; wb["indentation"] = "";
    return Json::writeString(wb, v);
}

/*
 * CAS read-modify-write on a key. mutate(root, exists) edits root in place and
 * returns true to proceed (false to abort). When the key is absent, root starts
 * empty and exists=false. Returns CLI_ETCD_OK / UNAVAIL / ERR.
 */
cli_etcd_status_t cas(const std::string& key,
    const std::function<bool(Json::Value&, bool)>& mutate) {
    etcd::Client* c = client();
    if (!c) return CLI_ETCD_UNAVAIL;

    int backoff_ms = 50;
    for (int attempt = 0; attempt < CAS_MAX_RETRIES; attempt++) {
        std::string cur;
        int64_t rev = 0;
        bool exists = false;
        try {
            auto resp = c->get(key).get();
            if (resp.error_code() == 0) {
                cur = resp.value().as_string();
                rev = resp.value().modified_index();
                exists = true;
            } else if (resp.error_code() != etcdv3::ERROR_KEY_NOT_FOUND) {
                return CLI_ETCD_UNAVAIL;   /* connection / server error */
            }
        } catch (const std::exception&) {
            return CLI_ETCD_UNAVAIL;
        }

        Json::Value root;
        if (exists) {
            Json::CharReaderBuilder rb; std::string errs;
            std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
            if (!reader->parse(cur.c_str(), cur.c_str() + cur.size(), &root, &errs))
                return CLI_ETCD_ERR;
        }
        if (!mutate(root, exists))
            return CLI_ETCD_ERR;
        std::string payload = to_json(root);

        try {
            auto w = exists ? c->modify_if(key, payload, rev).get()
                            : c->add(key, payload).get();
            if (w.error_code() == 0)
                return CLI_ETCD_OK;
            if (w.error_code() == etcdv3::ERROR_COMPARE_FAILED ||
                w.error_code() == etcdv3::ERROR_KEY_ALREADY_EXISTS) {
                struct timespec ts { backoff_ms / 1000, (long)(backoff_ms % 1000) * 1000000L };
                nanosleep(&ts, nullptr);
                backoff_ms *= 2;
                continue;   /* retry */
            }
            return CLI_ETCD_ERR;
        } catch (const std::exception&) {
            return CLI_ETCD_UNAVAIL;
        }
    }
    return CLI_ETCD_ERR;   /* retries exhausted */
}

std::string hsi_key(unsigned int user_id) {
    return "configs/" + g_node_uuid + "/hsi/" + std::to_string(user_id);
}
std::string dns_key(unsigned int user_id) {
    return "configs/" + g_node_uuid + "/" + std::to_string(user_id) + "/dns";
}

void stamp_meta(Json::Value& root) {
    Json::Value& meta = root["metadata"];
    meta["node"] = g_node_uuid;
    if (!meta.isMember("resourceVersion")) meta["resourceVersion"] = "";
    meta["updatedBy"] = "fastrg-cli";
    meta["updatedAt"] = iso_now();
}

}  // namespace

extern "C" {

void cli_etcd_configure(const char* endpoints, const char* node_uuid) {
    g_endpoints = endpoints ? endpoints : "";
    g_node_uuid = node_uuid ? node_uuid : "";
    g_client.reset();
}

int cli_etcd_configured(void) {
    return (!g_endpoints.empty() && !g_node_uuid.empty()) ? 1 : 0;
}

cli_etcd_status_t cli_etcd_apply_hsi(U16 user_id, U16 vlan_id, const char* account,
    const char* password, const char* pool_start, const char* pool_end,
    const char* subnet, const char* gateway) {
    return cas(hsi_key(user_id), [&](Json::Value& root, bool exists) {
        Json::Value& cfg = root["config"];
        cfg["user_id"] = std::to_string(user_id);
        cfg["vlan_id"] = std::to_string(vlan_id);
        cfg["account_name"] = account ? account : "";
        cfg["password"] = password ? password : "";
        cfg["dhcp_addr_pool"] = std::string(pool_start ? pool_start : "") + "-" +
                                (pool_end ? pool_end : "");
        cfg["dhcp_subnet"] = subnet ? subnet : "";
        cfg["dhcp_gateway"] = gateway ? gateway : "";
        if (!cfg.isMember("dns_proxy_enable")) cfg["dns_proxy_enable"] = true;
        if (!cfg.isMember("tcp_conntrack_enable")) cfg["tcp_conntrack_enable"] = true;
        /* preserve desire_status (managed by connect/disconnect); default disconnect */
        if (!exists || !cfg.isMember("desire_status"))
            cfg["desire_status"] = "disconnect";
        stamp_meta(root);
        return true;
    });
}

cli_etcd_status_t cli_etcd_remove_hsi(U16 user_id) {
    etcd::Client* c = client();
    if (!c) return CLI_ETCD_UNAVAIL;
    try {
        auto resp = c->rm(hsi_key(user_id)).get();
        if (resp.error_code() == 0 || resp.error_code() == etcdv3::ERROR_KEY_NOT_FOUND)
            return CLI_ETCD_OK;
        return CLI_ETCD_UNAVAIL;
    } catch (const std::exception&) {
        return CLI_ETCD_UNAVAIL;
    }
}

cli_etcd_status_t cli_etcd_set_subscriber_count(int count) {
    return cas("user_counts/" + g_node_uuid + "/", [&](Json::Value& root, bool) {
        root["subscriber_count"] = std::to_string(count);
        stamp_meta(root);
        return true;
    });
}

cli_etcd_status_t cli_etcd_set_desire(U16 user_id, const char* desire_status) {
    return cas(hsi_key(user_id), [&](Json::Value& root, bool exists) {
        if (!exists || !root.isMember("config")) return false;   /* no config to set intent on */
        root["config"]["desire_status"] = desire_status ? desire_status : "disconnect";
        stamp_meta(root);
        return true;
    });
}

cli_etcd_status_t cli_etcd_set_dns_proxy(U16 user_id, int enable) {
    return cas(hsi_key(user_id), [&](Json::Value& root, bool exists) {
        if (!exists || !root.isMember("config")) return false;
        root["config"]["dns_proxy_enable"] = (enable != 0);
        stamp_meta(root);
        return true;
    });
}

cli_etcd_status_t cli_etcd_set_tcp_conntrack(U16 user_id, int enable) {
    return cas(hsi_key(user_id), [&](Json::Value& root, bool exists) {
        if (!exists || !root.isMember("config")) return false;
        root["config"]["tcp_conntrack_enable"] = (enable != 0);
        stamp_meta(root);
        return true;
    });
}

cli_etcd_status_t cli_etcd_snat_set(U16 user_id, U16 eport, const char* dip, U16 iport) {
    return cas(hsi_key(user_id), [&](Json::Value& root, bool exists) {
        if (!exists || !root.isMember("config")) return false;
        Json::Value& cfg = root["config"];
        Json::Value kept(Json::arrayValue);
        if (cfg.isMember("port-mapping") && cfg["port-mapping"].isArray())
            for (const auto& e : cfg["port-mapping"])
                if (e["eport"].asString() != std::to_string(eport))
                    kept.append(e);
        Json::Value entry;
        entry["index"] = std::to_string(kept.size());
        entry["eport"] = std::to_string(eport);
        entry["dip"] = dip ? dip : "";
        entry["dport"] = std::to_string(iport);
        kept.append(entry);
        cfg["port-mapping"] = kept;
        stamp_meta(root);
        return true;
    });
}

cli_etcd_status_t cli_etcd_snat_unset(U16 user_id, U16 eport) {
    return cas(hsi_key(user_id), [&](Json::Value& root, bool exists) {
        if (!exists || !root.isMember("config")) return false;
        Json::Value& cfg = root["config"];
        Json::Value kept(Json::arrayValue);
        if (cfg.isMember("port-mapping") && cfg["port-mapping"].isArray())
            for (const auto& e : cfg["port-mapping"])
                if (e["eport"].asString() != std::to_string(eport))
                    kept.append(e);
        cfg["port-mapping"] = kept;
        stamp_meta(root);
        return true;
    });
}

cli_etcd_status_t cli_etcd_add_dns(U16 user_id, const char* domain, const char* ip, U32 ttl) {
    return cas(dns_key(user_id), [&](Json::Value& root, bool) {
        Json::Value arr = root.isArray() ? root : Json::Value(Json::arrayValue);
        Json::Value out(Json::arrayValue);
        for (const auto& e : arr)
            if (e["domain"].asString() != (domain ? domain : ""))
                out.append(e);
        Json::Value rec;
        rec["domain"] = domain ? domain : "";
        rec["ip"] = ip ? ip : "";
        rec["ttl"] = ttl;
        out.append(rec);
        root = out;   /* DNS key is a bare JSON array */
        return true;
    });
}

cli_etcd_status_t cli_etcd_del_dns(U16 user_id, const char* domain) {
    return cas(dns_key(user_id), [&](Json::Value& root, bool exists) {
        if (!exists || !root.isArray()) return true;   /* nothing to remove */
        Json::Value out(Json::arrayValue);
        for (const auto& e : root)
            if (e["domain"].asString() != (domain ? domain : ""))
                out.append(e);
        root = out;
        return true;
    });
}

}  // extern "C"
