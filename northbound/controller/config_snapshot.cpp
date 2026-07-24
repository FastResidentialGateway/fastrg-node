#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <json/json.h>

#include "config_snapshot.h"

#define SNAPSHOT_PATH_DEFAULT "/etc/fastrg/config_snapshot.json"
#define SNAPSHOT_UPDATED_BY "fastrg-node-offline"

/* Overridable for tests (CONFIG_SNAPSHOT_PATH); production uses the default. */
static const char *snapshot_path()
{
    const char *p = getenv("CONFIG_SNAPSHOT_PATH");
    return (p && p[0] != '\0') ? p : SNAPSHOT_PATH_DEFAULT;
}

namespace {

struct Entry {
    std::string value;    // full JSON value ("" = key recorded as deleted/absent)
    bool exists = false;  // false when the etcd key was seen deleted
    bool dirty = false;
    int64_t edited_at = 0;
    std::string summary;
};

// key: "<kind>/<user_id>"
std::map<std::string, Entry> g_entries;
std::mutex g_mutex;
bool g_initialized = false;

std::string map_key(snapshot_kind_t kind, const char *user_id)
{
    static const char *names[] = {"hsi", "dns", "count"};
    return std::string(names[kind]) + "/" + user_id;
}

bool kind_from_name(const std::string &name, snapshot_kind_t *out)
{
    if (name == "hsi") { *out = SNAPSHOT_KIND_HSI; return true; }
    if (name == "dns") { *out = SNAPSHOT_KIND_DNS; return true; }
    if (name == "count") { *out = SNAPSHOT_KIND_COUNT; return true; }
    return false;
}

std::string iso8601_now()
{
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

// Persist while holding g_mutex. Atomic replace, mirroring the pattern the
// retired config queue used.
void persist_locked()
{
    Json::Value arr(Json::arrayValue);
    for (const auto &it : g_entries) {
        Json::Value j;
        j["key"] = it.first;
        j["value"] = it.second.value;
        j["exists"] = it.second.exists;
        j["dirty"] = it.second.dirty;
        j["edited_at"] = (Json::Int64)it.second.edited_at;
        j["summary"] = it.second.summary;
        arr.append(j);
    }
    Json::Value root;
    root["version"] = 1;
    root["entries"] = arr;

    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    std::string data = Json::writeString(w, root);
    std::string tmp = std::string(snapshot_path()) + ".tmp";
    std::ofstream ofs(tmp, std::ios::trunc);
    if (!ofs)
        return;
    ofs << data;
    ofs.flush();
    ofs.close();
    std::rename(tmp.c_str(), snapshot_path());
}

// Extract metadata.resourceVersion as an integer
bool parse_rv(const std::string &value_json, long long *out)
{
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(value_json, root) || !root.isObject() ||
            !root.isMember("metadata") || !root["metadata"].isObject())
        return false;
    const Json::Value &rv = root["metadata"]["resourceVersion"];
    if (!rv.isString())
        return false;
    char *end = nullptr;
    long long v = strtoll(rv.asString().c_str(), &end, 10);
    if (!end || *end != '\0' || rv.asString().empty())
        return false;
    *out = v;
    return true;
}

// Strip the metadata member (top level) for comparison.
bool parse_without_metadata(const char *json, Json::Value *out)
{
    if (json == nullptr)
        return false;
    Json::Reader reader;
    if (!reader.parse(json, *out))
        return false;
    if (out->isObject())
        out->removeMember("metadata");
    return true;
}

} // namespace

extern "C" {

STATUS config_snapshot_init(void)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_entries.clear();
    g_initialized = true;

    std::ifstream ifs(snapshot_path());
    if (!ifs)
        return SUCCESS; // no file = empty snapshot

    std::stringstream buf;
    buf << ifs.rdbuf();
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(buf.str(), root) || !root.isMember("entries") ||
            !root["entries"].isArray())
        return ERROR; // corrupt file: start empty, report

    for (const auto &j : root["entries"]) {
        std::string key = j.get("key", "").asString();
        if (key.empty())
            continue;
        Entry e;
        e.value = j.get("value", "").asString();
        e.exists = j.get("exists", false).asBool();
        e.dirty = j.get("dirty", false).asBool();
        e.edited_at = j.get("edited_at", (Json::Int64)0).asInt64();
        e.summary = j.get("summary", "").asString();
        g_entries[key] = e;
    }
    return SUCCESS;
}

void config_snapshot_cleanup(void)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_initialized)
        persist_locked();
    g_entries.clear();
    g_initialized = false;
}

void config_snapshot_watch_update(snapshot_kind_t kind, const char *user_id,
    const char *value_json)
{
    if (!user_id)
        return;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_initialized)
        return;
    Entry &e = g_entries[map_key(kind, user_id)];
    e.value = value_json ? value_json : "";
    e.exists = (value_json != nullptr);
    e.dirty = false;
    e.summary.clear();
    persist_locked();
}

char *config_snapshot_get(snapshot_kind_t kind, const char *user_id)
{
    if (!user_id)
        return nullptr;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_entries.find(map_key(kind, user_id));
    if (it == g_entries.end() || !it->second.exists)
        return nullptr;
    return strdup(it->second.value.c_str());
}

STATUS config_snapshot_offline_edit(snapshot_kind_t kind, const char *user_id,
    const char *new_value_json, const char *edit_summary)
{
    if (!user_id || !new_value_json)
        return ERROR;

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(new_value_json, root) || !root.isObject())
        return ERROR;

    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_initialized)
        return ERROR;

    Entry &e = g_entries[map_key(kind, user_id)];

    // Resource version from the current snapshot entry. A tombstone (offline
    // delete) keeps its pre-delete value: a recreate continues that rv
    // chain instead of resetting to "1" — this will ensure the config always 
    // win the etcd key if it still exists.
    long long cur = 0;
    std::string rv;
    if (!e.exists && e.value.empty())
        rv = "1";
    else if (!parse_rv(e.value, &cur))
        rv = "2";
    else
        rv = std::to_string(cur + 1);

    Json::Value meta = root.isMember("metadata") && root["metadata"].isObject()
        ? root["metadata"] : Json::Value(Json::objectValue);
    meta["resourceVersion"] = rv;
    meta["updatedAt"] = iso8601_now();
    meta["updatedBy"] = SNAPSHOT_UPDATED_BY;
    root["metadata"] = meta;

    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    e.value = Json::writeString(w, root);
    e.exists = true;
    e.dirty = true;
    e.edited_at = (int64_t)std::time(nullptr);
    if (edit_summary && edit_summary[0] != '\0') {
        if (!e.summary.empty())
            e.summary += "; ";
        e.summary += edit_summary;
    }
    persist_locked();
    return SUCCESS;
}

STATUS config_snapshot_offline_delete(snapshot_kind_t kind, const char *user_id,
    const char *edit_summary)
{
    if (!user_id)
        return ERROR;

    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_initialized)
        return ERROR;

    auto it = g_entries.find(map_key(kind, user_id));
    if (it == g_entries.end() || !it->second.exists)
        return SUCCESS; // already absent: nothing to propose

    Entry &e = it->second;
    // Tombstone: keep e.value so foreach_dirty can still read the last-known
    // rv for the proposal; config_snapshot_get guards on exists so the value
    // never leaks as live config.
    e.exists = false;
    e.dirty = true;
    e.edited_at = (int64_t)std::time(nullptr);
    if (edit_summary && edit_summary[0] != '\0') {
        if (!e.summary.empty())
            e.summary += "; ";
        e.summary += edit_summary;
    }
    persist_locked();
    return SUCCESS;
}

BOOL config_snapshot_content_equal(const char *json_a, const char *json_b)
{
    Json::Value a, b;
    bool ok_a = parse_without_metadata(json_a, &a);
    bool ok_b = parse_without_metadata(json_b, &b);
    if (!ok_a || !ok_b)
        return (ok_a == ok_b) ? TRUE : FALSE; // both absent/unparsable = equal
    return (a == b) ? TRUE : FALSE;
}

static void foreach_impl(snapshot_dirty_cb_t cb, void *user_data, bool dirty_only);

void config_snapshot_foreach(snapshot_dirty_cb_t cb, void *user_data)
{
    foreach_impl(cb, user_data, false);
}

void config_snapshot_foreach_dirty(snapshot_dirty_cb_t cb, void *user_data)
{
    foreach_impl(cb, user_data, true);
}

static void foreach_impl(snapshot_dirty_cb_t cb, void *user_data, bool dirty_only)
{
    if (!cb)
        return;
    // Copy the dirty set out so the callback can call back into the snapshot
    // (e.g. clear_dirty) without deadlocking.
    struct DirtyItem {
        snapshot_kind_t kind;
        std::string user_id;
        std::string value;
        std::string rv;
        int64_t edited_at;
        std::string summary;
        bool tombstone;   // offline delete: value_json passed as NULL
    };
    std::vector<DirtyItem> items;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (const auto &it : g_entries) {
            bool tombstone = !it.second.exists;
            if (dirty_only) {
                // reconnect sync: include dirty tombstones (delete proposals);
                // skip clean entries.
                if (!it.second.dirty)
                    continue;
            } else {
                // boot apply: only live entries; a delete must not re-apply.
                if (tombstone)
                    continue;
            }
            auto slash = it.first.find('/');
            snapshot_kind_t kind;
            if (slash == std::string::npos ||
                    !kind_from_name(it.first.substr(0, slash), &kind))
                continue;
            // A tombstone keeps its pre-delete value, so parse_rv still yields
            // the key's last-known rv to carry on the proposal.
            long long rv_num = 0;
            std::string rv = parse_rv(it.second.value, &rv_num)
                ? std::to_string(rv_num) : std::string("");
            items.push_back({kind, it.first.substr(slash + 1), it.second.value,
                rv, it.second.edited_at, it.second.summary, tombstone});
        }
    }
    for (const auto &i : items)
        cb(i.kind, i.user_id.c_str(), i.tombstone ? nullptr : i.value.c_str(),
            i.rv.c_str(), i.edited_at, i.summary.c_str(), user_data);
}

void config_snapshot_clear_dirty(snapshot_kind_t kind, const char *user_id)
{
    if (!user_id)
        return;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_entries.find(map_key(kind, user_id));
    if (it == g_entries.end())
        return;
    it->second.dirty = false;
    it->second.summary.clear();
    persist_locked();
}

// Field-level merge for the offline-edit kinds that touch a single aspect of
// an HSI config or a DNS record envelope. Pure function of
// (kind, current_json, value); the offline-edit path runs it on the
// snapshot's current value before config_snapshot_offline_edit stamps and
// stores the result.
static STATUS field_merge_impl(const char *kind, const char *current_json,
    const char *value, char **out_json)
{
    if (!kind || !value || !out_json) return ERROR;
    *out_json = NULL;

    Json::Reader reader;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";

    // DNS record kinds operate on the per-subscriber enveloped record key
    // ({"records": [...], "metadata": {...}}).
    if (strcmp(kind, SNAPSHOT_FIELD_KIND_DNS_ADD) == 0 ||
            strcmp(kind, SNAPSHOT_FIELD_KIND_DNS_DEL) == 0) {
        Json::Value env(Json::objectValue);
        if (current_json != NULL) {
            if (!reader.parse(current_json, env) || !env.isObject())
                env = Json::Value(Json::objectValue);   // unparseable → rebuild
        }
        Json::Value arr = env.isMember("records") && env["records"].isArray()
            ? env["records"] : Json::Value(Json::arrayValue);

        if (strcmp(kind, SNAPSHOT_FIELD_KIND_DNS_ADD) == 0) {
            Json::Value rec;
            if (!reader.parse(value, rec) || !rec.isMember("domain"))
                return ERROR;
            bool updated = false;
            for (auto& entry : arr) {
                if (entry.get("domain", "").asString() ==
                        rec["domain"].asString()) {
                    entry = rec;
                    updated = true;
                    break;
                }
            }
            if (!updated)
                arr.append(rec);
        } else {
            if (current_json == NULL)
                return ERROR;   // nothing to delete from
            Json::Value filtered(Json::arrayValue);
            for (const auto& entry : arr) {
                if (entry.get("domain", "").asString() != value)
                    filtered.append(entry);
            }
            arr = filtered;
        }
        env["records"] = arr;
        std::string s = Json::writeString(writer, env);
        *out_json = strdup(s.c_str());
        return *out_json ? SUCCESS : ERROR;
    }

    // HSI field kinds require an existing config object.
    if (current_json == NULL)
        return ERROR;
    Json::Value cur;
    if (!reader.parse(current_json, cur) || !cur.isMember("config"))
        return ERROR;
    Json::Value& cfg = cur["config"];

    if (strcmp(kind, SNAPSHOT_FIELD_KIND_DNS_PROXY) == 0) {
        cfg["dns_proxy_enable"] = (strcmp(value, "true") == 0);
    } else if (strcmp(kind, SNAPSHOT_FIELD_KIND_DESIRE) == 0) {
        cfg["desire_status"] = value;
    } else if (strcmp(kind, SNAPSHOT_FIELD_KIND_TCP_CONNTRACK) == 0) {
        cfg["tcp_conntrack_enable"] = (strcmp(value, "true") == 0);
    } else if (strcmp(kind, SNAPSHOT_FIELD_KIND_SNAT_UPSERT) == 0) {
        Json::Value entry;
        if (!reader.parse(value, entry) || !entry.isMember("eport"))
            return ERROR;
        Json::Value pms = cfg.isMember("port-mapping") &&
            cfg["port-mapping"].isArray() ? cfg["port-mapping"]
                                          : Json::Value(Json::arrayValue);
        bool updated = false;
        for (auto& pm : pms) {
            if (pm.get("eport", "").asString() ==
                    entry["eport"].asString()) {
                pm = entry;
                updated = true;
                break;
            }
        }
        if (!updated)
            pms.append(entry);
        for (Json::ArrayIndex i = 0; i < pms.size(); i++)
            pms[i]["index"] = std::to_string(i);
        cfg["port-mapping"] = pms;
    } else if (strcmp(kind, SNAPSHOT_FIELD_KIND_SNAT_REMOVE) == 0) {
        Json::Value filtered(Json::arrayValue);
        if (cfg.isMember("port-mapping") && cfg["port-mapping"].isArray()) {
            for (const auto& pm : cfg["port-mapping"]) {
                if (pm.get("eport", "").asString() != value)
                    filtered.append(pm);
            }
        }
        for (Json::ArrayIndex i = 0; i < filtered.size(); i++)
            filtered[i]["index"] = std::to_string(i);
        cfg["port-mapping"] = filtered;   // idempotent when absent
    } else {
        return ERROR;   // unknown kind
    }

    std::string s = Json::writeString(writer, cur);
    *out_json = strdup(s.c_str());
    return *out_json ? SUCCESS : ERROR;
}

STATUS config_snapshot_field_merge(const char *kind, const char *current_json,
    const char *value, char **out_json)
{
    return field_merge_impl(kind, current_json, value, out_json);
}

struct SnapshotApplyCtx {
    const char *node_id;
    hsi_config_callback_t hsi_cb;
    user_count_changed_callback_t count_cb;
    void *user_data;
    bool count_pass;
};

static void snapshot_apply_cb(snapshot_kind_t kind, const char *user_id,
    const char *value_json, const char *resource_version, int64_t edited_at,
    const char *edit_summary, void *user_data)
{
    (void)resource_version; (void)edited_at; (void)edit_summary;
    SnapshotApplyCtx *ctx = (SnapshotApplyCtx *)user_data;

    if (ctx->count_pass) {
        if (kind != SNAPSHOT_KIND_COUNT || !ctx->count_cb)
            return;
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(value_json, root) || !root.isObject() ||
                !root.isMember("subscriber_count"))
            return;
        user_count_config_t cfg;
        cfg.user_count = atoi(root["subscriber_count"].asString().c_str());
        if (cfg.user_count > 0)
            ctx->count_cb(ctx->node_id, &cfg, HSI_ACTION_UPDATE, 0, ctx->user_data);
        return;
    }

    if (kind != SNAPSHOT_KIND_HSI || !ctx->hsi_cb)
        return;
    hsi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (etcd_client_parse_hsi_config(value_json, &cfg, NULL) != SUCCESS)
        return;
    ctx->hsi_cb(ctx->node_id, user_id, &cfg, HSI_ACTION_UPDATE, 0, ctx->user_data);
    hsi_config_free_port_mappings(&cfg);
}

void config_snapshot_apply_all(const char *node_id,
    hsi_config_callback_t hsi_callback,
    user_count_changed_callback_t user_count_callback,
    void *user_data)
{
    if (!node_id)
        return;
    SnapshotApplyCtx ctx{node_id, hsi_callback, user_count_callback, user_data, true};
    config_snapshot_foreach(snapshot_apply_cb, &ctx);   // count first
    ctx.count_pass = false;
    config_snapshot_foreach(snapshot_apply_cb, &ctx);   // then HSI configs
}

} // extern "C"
