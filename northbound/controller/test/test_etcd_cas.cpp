#include "../etcd_client.h"
#include "../config_snapshot.h"
#include "../../../src/fastrg.h"

#include <jsoncpp/json/json.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

// Offline-edit unit tests: config_snapshot_field_merge (pure JSON merge) and the
// config snapshot (rv stamping / dirty semantics / persistence). No etcd
// connection is required — the node no longer writes etcd; offline edits are
// applied to the local snapshot and reported over Kafka.
//
// Historical note: cases 1-5 covered the retired cas_put offline-queue flush
// primitive and were removed together with it (user-approved retirement).

// etcd_client.o references these from its watcher-thread self-event filter and
// its event hand-off; the real definitions live in src/etcd_integration.c and
// src/fastrg.c. This standalone test links etcd_client.o only for field_merge,
// so provide minimal stand-ins.
extern "C" {
int parse_user_id(const char *user_id_str, int max_count)
{
    (void)max_count;
    if (!user_id_str || user_id_str[0] == '\0')
        return -1;
    char *endptr;
    long val = strtol(user_id_str, &endptr, 10);
    if (endptr == user_id_str || *endptr != '\0')
        return -1;
    int ccb_id = (int)val - 1;
    return ccb_id < 0 ? -1 : ccb_id;
}

// No control-plane loop here: take ownership and drop the event, which is what
// the loop does once it has dispatched one.
STATUS fastrg_gen_etcd_event(FastRG_t *fastrg_ccb, etcd_event_t *ev)
{
    (void)fastrg_ccb;
    etcd_event_free(ev);
    return SUCCESS;
}

}

static int failures = 0;

template <typename Expected, typename Actual>
static void expect_equal(const std::string& assertion, const Expected& expected, const Actual& actual)
{
    if (expected == actual)
        return;

    std::cerr << "FAIL: " << assertion << ", expected=" << expected << ", actual=" << actual << std::endl;
    failures++;
}

static void expect_true(const std::string& assertion, bool actual)
{
    if (actual)
        return;

    std::cerr << "FAIL: " << assertion << ", expected=true, actual=false" << std::endl;
    failures++;
}

/* ---- config_snapshot_field_merge cases (pure JSON merge used by the offline
 * snapshot-edit path; no etcd connection required) ---- */

static const char *MERGE_SEED_HSI =
    "{\"config\":{\"user_id\":\"7\",\"vlan_id\":\"123\","
    "\"account_name\":\"cas-acct\",\"password\":\"cas-pw\","
    "\"dhcp_addr_pool\":\"192.168.9.2-192.168.9.10\","
    "\"dhcp_subnet\":\"255.255.255.0\",\"dhcp_gateway\":\"192.168.9.1\","
    "\"dns_proxy_enable\":true,\"tcp_conntrack_enable\":true,"
    "\"desire_status\":\"connect\","
    "\"port-mapping\":[{\"index\":\"0\",\"eport\":\"8080\","
    "\"dip\":\"192.168.9.5\",\"dport\":\"80\"}]},"
    "\"metadata\":{\"node\":\"cas-test-node\",\"resourceVersion\":\"3\","
    "\"updatedBy\":\"seed\",\"updatedAt\":\"2026-01-01T00:00:00Z\"}}";

static bool parse_json(const std::string& text, Json::Value& out)
{
    Json::Reader reader;
    return reader.parse(text, out);
}

static std::string run_merge(const std::string& assertion, const char *kind,
    const char *current, const char *value)
{
    char *out = NULL;
    STATUS s = config_snapshot_field_merge(kind, current, value, &out);
    expect_equal(assertion + " status", SUCCESS, s);
    std::string result = (s == SUCCESS && out) ? out : "";
    free(out);
    return result;
}

static void test_field_merge_preserves_untouched_fields()
{
    std::cout << "Case 6: field merge flips one flag, preserves the rest" << std::endl;
    Json::Value root;
    expect_true("case 6 result parses", parse_json(
        run_merge("case 6 dns_proxy", SNAPSHOT_FIELD_KIND_DNS_PROXY,
            MERGE_SEED_HSI, "false"), root));
    const Json::Value& cfg = root["config"];
    expect_equal("case 6 patched flag", false, cfg["dns_proxy_enable"].asBool());
    expect_equal("case 6 vlan preserved", std::string("123"), cfg["vlan_id"].asString());
    expect_equal("case 6 account preserved", std::string("cas-acct"), cfg["account_name"].asString());
    expect_equal("case 6 desire_status preserved", std::string("connect"), cfg["desire_status"].asString());
    expect_equal("case 6 tcp flag preserved", true, cfg["tcp_conntrack_enable"].asBool());
    expect_equal("case 6 mapping count preserved", 1u, cfg["port-mapping"].size());
    expect_equal("case 6 metadata preserved", std::string("seed"),
        root["metadata"]["updatedBy"].asString());

    Json::Value root2;
    expect_true("case 6 tcp result parses", parse_json(
        run_merge("case 6 tcp_conntrack", SNAPSHOT_FIELD_KIND_TCP_CONNTRACK,
            MERGE_SEED_HSI, "false"), root2));
    expect_equal("case 6 tcp patched", false,
        root2["config"]["tcp_conntrack_enable"].asBool());
    expect_equal("case 6 tcp leaves dns flag", true,
        root2["config"]["dns_proxy_enable"].asBool());

    // desire_status is a field kind as well (offline connect/disconnect).
    Json::Value root3;
    expect_true("case 6 desire result parses", parse_json(
        run_merge("case 6 desire", SNAPSHOT_FIELD_KIND_DESIRE,
            MERGE_SEED_HSI, "disconnect"), root3));
    expect_equal("case 6 desire patched", std::string("disconnect"),
        root3["config"]["desire_status"].asString());

    // HSI kinds require an existing config: absent key must fail.
    char *out = NULL;
    expect_equal("case 6 absent config fails", ERROR,
        config_snapshot_field_merge(SNAPSHOT_FIELD_KIND_DNS_PROXY, NULL, "true", &out));
    free(out);
}

static void test_field_merge_port_mappings()
{
    std::cout << "Case 7: field merge upserts/removes port mappings" << std::endl;
    Json::Value root;
    expect_true("case 7 upsert parses", parse_json(
        run_merge("case 7 upsert", SNAPSHOT_FIELD_KIND_SNAT_UPSERT, MERGE_SEED_HSI,
            "{\"index\":\"0\",\"eport\":\"9090\",\"dip\":\"192.168.9.6\",\"dport\":\"90\"}"),
        root));
    const Json::Value& pm = root["config"]["port-mapping"];
    expect_equal("case 7 mapping count", 2u, pm.size());
    expect_equal("case 7 existing kept", std::string("8080"), pm[0u]["eport"].asString());
    expect_equal("case 7 appended", std::string("9090"), pm[1u]["eport"].asString());
    expect_equal("case 7 reindexed", std::string("1"), pm[1u]["index"].asString());
    expect_equal("case 7 vlan preserved", std::string("123"),
        root["config"]["vlan_id"].asString());

    Json::Value root2;
    expect_true("case 7 remove parses", parse_json(
        run_merge("case 7 remove", SNAPSHOT_FIELD_KIND_SNAT_REMOVE,
            MERGE_SEED_HSI, "8080"), root2));
    expect_equal("case 7 removed", 0u, root2["config"]["port-mapping"].size());

    Json::Value root3;
    expect_true("case 7 idempotent remove parses", parse_json(
        run_merge("case 7 idempotent remove", SNAPSHOT_FIELD_KIND_SNAT_REMOVE,
            MERGE_SEED_HSI, "7070"), root3));
    expect_equal("case 7 idempotent keeps entry", 1u,
        root3["config"]["port-mapping"].size());
}

static void test_field_merge_dns_records()
{
    std::cout << "Case 8: field merge edits the DNS records envelope" << std::endl;
    // The DNS key is an envelope, not a bare array.
    const char *seed =
        "{\"records\":[{\"domain\":\"a.example\",\"ip\":\"10.0.0.1\",\"ttl\":3600}],"
        "\"metadata\":{\"node\":\"cas-test-node\",\"resourceVersion\":\"2\","
        "\"updatedBy\":\"seed\",\"updatedAt\":\"2026-01-01T00:00:00Z\"}}";

    // dns_add creates the envelope when the key is absent.
    Json::Value created;
    expect_true("case 8 create parses", parse_json(
        run_merge("case 8 create", SNAPSHOT_FIELD_KIND_DNS_ADD, NULL,
            "{\"domain\":\"b.example\",\"ip\":\"10.0.0.2\",\"ttl\":600}"), created));
    expect_true("case 8 created is envelope", created.isObject() &&
        created["records"].isArray());
    expect_equal("case 8 created size", 1u, created["records"].size());

    // dns_add appends to an existing envelope; same-domain updates in place.
    Json::Value appended;
    expect_true("case 8 append parses", parse_json(
        run_merge("case 8 append", SNAPSHOT_FIELD_KIND_DNS_ADD, seed,
            "{\"domain\":\"b.example\",\"ip\":\"10.0.0.2\",\"ttl\":600}"), appended));
    expect_equal("case 8 appended size", 2u, appended["records"].size());
    Json::Value updated;
    expect_true("case 8 update parses", parse_json(
        run_merge("case 8 update", SNAPSHOT_FIELD_KIND_DNS_ADD, seed,
            "{\"domain\":\"a.example\",\"ip\":\"10.0.0.9\",\"ttl\":60}"), updated));
    expect_equal("case 8 updated size", 1u, updated["records"].size());
    expect_equal("case 8 updated ip", std::string("10.0.0.9"),
        updated["records"][0u]["ip"].asString());

    // dns_del removes by domain; absent key fails (nothing to delete).
    Json::Value removed;
    expect_true("case 8 remove parses", parse_json(
        run_merge("case 8 remove", SNAPSHOT_FIELD_KIND_DNS_DEL, seed, "a.example"),
        removed));
    expect_equal("case 8 removed size", 0u, removed["records"].size());
    char *out = NULL;
    expect_equal("case 8 absent key fails", ERROR,
        config_snapshot_field_merge(SNAPSHOT_FIELD_KIND_DNS_DEL, NULL, "a.example", &out));
    free(out);
}

/* ---- config snapshot cases (rv stamping / dirty semantics / persistence);
 * CONFIG_SNAPSHOT_PATH points at a temp file set up in main() ---- */

static void test_snapshot_rv_stamping()
{
    std::cout << "Case 9: snapshot offline edit stamps rv" << std::endl;
    // Absent entry → rv "1".
    expect_equal("case 9 first edit", SUCCESS,
        config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "7",
            "{\"config\":{\"vlan_id\":\"123\"}}", "first"));
    char *v = config_snapshot_get(SNAPSHOT_KIND_HSI, "7");
    Json::Value root;
    expect_true("case 9 value parses", v != NULL && parse_json(v, root));
    expect_equal("case 9 rv is 1", std::string("1"),
        root["metadata"]["resourceVersion"].asString());
    expect_equal("case 9 updatedBy", std::string("fastrg-node-offline"),
        root["metadata"]["updatedBy"].asString());
    free(v);

    // Existing entry → rv+1.
    expect_equal("case 9 second edit", SUCCESS,
        config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "7",
            "{\"config\":{\"vlan_id\":\"124\"}}", "second"));
    v = config_snapshot_get(SNAPSHOT_KIND_HSI, "7");
    expect_true("case 9 second parses", v != NULL && parse_json(v, root));
    expect_equal("case 9 rv is 2", std::string("2"),
        root["metadata"]["resourceVersion"].asString());
    free(v);

    // Watched value with an unparsable rv → next edit stamps "2".
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "8",
        "{\"config\":{\"vlan_id\":\"200\"},\"metadata\":{\"resourceVersion\":\"\"}}");
    expect_equal("case 9 edit on bad rv", SUCCESS,
        config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "8",
            "{\"config\":{\"vlan_id\":\"201\"}}", "bad-rv-base"));
    v = config_snapshot_get(SNAPSHOT_KIND_HSI, "8");
    expect_true("case 9 bad-rv parses", v != NULL && parse_json(v, root));
    expect_equal("case 9 rv falls back to 2", std::string("2"),
        root["metadata"]["resourceVersion"].asString());
    free(v);
}

struct DirtyProbe {
    int count = 0;
    std::string last_user;
    std::string last_summary;
    std::string last_rv;
    bool last_tombstone = false;   // value_json == NULL (offline delete)
};

static void dirty_probe_cb(snapshot_kind_t kind, const char *user_id,
    const char *value_json, const char *resource_version, int64_t edited_at,
    const char *edit_summary, uint64_t edit_seq, void *user_data)
{
    (void)kind; (void)edited_at; (void)edit_seq;
    DirtyProbe *p = (DirtyProbe *)user_data;
    p->count++;
    p->last_user = user_id;
    p->last_summary = edit_summary ? edit_summary : "";
    p->last_rv = resource_version ? resource_version : "";
    p->last_tombstone = (value_json == NULL);
}

/* Compare-and-clear support: config_snapshot_clear_dirty takes the edit
 * sequence the caller observed via the foreach callback. This helper scans
 * the dirty set for one entry and captures its current seq/value/tombstone
 * state, so tests can clear with the up-to-date seq (or probe dirtiness). */
struct DirtyEntryProbe {
    snapshot_kind_t kind = SNAPSHOT_KIND_HSI;
    const char *uid = "";
    bool found = false;
    bool tombstone = false;
    uint64_t seq = 0;
    std::string value;
};

static void dirty_entry_probe_cb(snapshot_kind_t kind, const char *user_id,
    const char *value_json, const char *resource_version, int64_t edited_at,
    const char *edit_summary, uint64_t edit_seq, void *user_data)
{
    (void)resource_version; (void)edited_at; (void)edit_summary;
    DirtyEntryProbe *p = (DirtyEntryProbe *)user_data;
    if (kind != p->kind || std::string(user_id) != p->uid)
        return;
    p->found = true;
    p->tombstone = (value_json == NULL);
    p->seq = edit_seq;
    p->value = value_json ? value_json : "";
}

static DirtyEntryProbe probe_dirty_entry(snapshot_kind_t kind, const char *uid)
{
    DirtyEntryProbe p;
    p.kind = kind;
    p.uid = uid;
    config_snapshot_foreach_dirty(dirty_entry_probe_cb, &p);
    return p;
}

static uint64_t dirty_seq_of(snapshot_kind_t kind, const char *uid)
{
    return probe_dirty_entry(kind, uid).seq;
}

static void test_snapshot_dirty_semantics()
{
    std::cout << "Case 10: offline edits set dirty; watch updates defer to dirty entries" << std::endl;
    DirtyProbe p;
    config_snapshot_foreach_dirty(dirty_probe_cb, &p);
    int base = p.count;

    config_snapshot_offline_edit(SNAPSHOT_KIND_DNS, "5",
        "{\"records\":[{\"domain\":\"x.example\",\"ip\":\"10.0.0.5\",\"ttl\":60}]}",
        "dns add x.example");
    p = DirtyProbe();
    config_snapshot_foreach_dirty(dirty_probe_cb, &p);
    expect_equal("case 10 dirty count grows", base + 1, p.count);

    // A watch update on a dirty entry is skipped — the mirror must never
    // overwrite an offline edit that has not been reported yet.
    config_snapshot_watch_update(SNAPSHOT_KIND_DNS, "5",
        "{\"records\":[],\"metadata\":{\"resourceVersion\":\"9\"}}");
    p = DirtyProbe();
    config_snapshot_foreach_dirty(dirty_probe_cb, &p);
    expect_equal("case 10 watch defers to the dirty entry", base + 1, p.count);

    // Once the report flow clears dirty, the next watch update lands.
    config_snapshot_clear_dirty(SNAPSHOT_KIND_DNS, "5",
        dirty_seq_of(SNAPSHOT_KIND_DNS, "5"));
    config_snapshot_watch_update(SNAPSHOT_KIND_DNS, "5",
        "{\"records\":[],\"metadata\":{\"resourceVersion\":\"9\"}}");
    p = DirtyProbe();
    config_snapshot_foreach_dirty(dirty_probe_cb, &p);
    expect_equal("case 10 mirror lands after the report clears dirty", base, p.count);
    char *v10 = config_snapshot_get(SNAPSHOT_KIND_DNS, "5");
    Json::Value root10;
    expect_true("case 10 mirrored value parses", v10 != NULL && parse_json(v10, root10));
    expect_equal("case 10 mirrored rv", std::string("9"),
        root10["metadata"]["resourceVersion"].asString());
    free(v10);

    // Summaries accumulate across edits and reset with clear_dirty.
    config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "9",
        "{\"config\":{\"vlan_id\":\"1\"}}", "edit-a");
    config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "9",
        "{\"config\":{\"vlan_id\":\"2\"}}", "edit-b");
    p = DirtyProbe();
    config_snapshot_foreach_dirty(dirty_probe_cb, &p);
    expect_equal("case 10 summary accumulates", std::string("edit-a; edit-b"),
        p.last_summary);
    expect_equal("case 10 dirty rv matches", std::string("2"), p.last_rv);
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "9",
        dirty_seq_of(SNAPSHOT_KIND_HSI, "9"));
    p = DirtyProbe();
    config_snapshot_foreach_dirty(dirty_probe_cb, &p);
    expect_equal("case 10 clear_dirty removes entry", base, p.count);
}

static void test_snapshot_persistence()
{
    std::cout << "Case 11: snapshot persists across reinit" << std::endl;
    config_snapshot_offline_edit(SNAPSHOT_KIND_COUNT, "0",
        "{\"subscriber_count\":\"5\"}", "subscriber_count=5");
    config_snapshot_cleanup();
    expect_equal("case 11 reinit", SUCCESS, config_snapshot_init());
    char *v = config_snapshot_get(SNAPSHOT_KIND_COUNT, "0");
    Json::Value root;
    expect_true("case 11 persisted value parses", v != NULL && parse_json(v, root));
    expect_equal("case 11 persisted count", std::string("5"),
        root["subscriber_count"].asString());
    free(v);
    DirtyProbe p;
    config_snapshot_foreach_dirty(dirty_probe_cb, &p);
    expect_true("case 11 dirty survives restart", p.count >= 1);
}

static void test_snapshot_content_equal()
{
    std::cout << "Case 12: content_equal excludes metadata" << std::endl;
    const char *a = "{\"config\":{\"vlan_id\":\"3\"},"
        "\"metadata\":{\"resourceVersion\":\"4\",\"updatedAt\":\"2026-01-01T00:00:00Z\"}}";
    const char *b = "{ \"metadata\": {\"resourceVersion\": \"9\"}, "
        "\"config\": { \"vlan_id\": \"3\" } }";
    const char *c = "{\"config\":{\"vlan_id\":\"4\"},\"metadata\":{}}";
    expect_equal("case 12 same payload, different metadata → equal", (BOOL)TRUE,
        config_snapshot_content_equal(a, b));
    expect_equal("case 12 different payload → not equal", (BOOL)FALSE,
        config_snapshot_content_equal(a, c));
    expect_equal("case 12 both NULL → equal", (BOOL)TRUE,
        config_snapshot_content_equal(NULL, NULL));
    expect_equal("case 12 one NULL → not equal", (BOOL)FALSE,
        config_snapshot_content_equal(a, NULL));
}

static void test_snapshot_offline_delete()
{
    std::cout << "Case 13: offline delete surfaces a tombstone" << std::endl;

    // Seed a live entry (rv stamped to "1" as a fresh key), then delete it
    // offline. The reconnect scan must surface it as a tombstone: dirty, with
    // a NULL value_json and the key's last-known rv carried on the proposal.
    config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "42",
        "{\"config\":{\"vlan_id\":\"7\"}}", "apply config");
    expect_equal("case 13 delete an existing key", SUCCESS,
        config_snapshot_offline_delete(SNAPSHOT_KIND_HSI, "42", "remove config"));

    // config_snapshot_get must not leak the tombstoned value as live config.
    char *g = config_snapshot_get(SNAPSHOT_KIND_HSI, "42");
    expect_true("case 13 get returns NULL for a tombstone", g == NULL);
    free(g);

    // Find the tombstone among any other dirty entries by scanning for user 42.
    struct FindTomb {
        bool found = false; bool tombstone = false; std::string rv, summary;
    } ft;
    auto find_cb = [](snapshot_kind_t, const char *uid, const char *value_json,
        const char *rv, int64_t, const char *summary, uint64_t, void *ud) {
        FindTomb *f = (FindTomb *)ud;
        if (std::string(uid) == "42") {
            f->found = true;
            f->tombstone = (value_json == NULL);
            f->rv = rv ? rv : "";
            f->summary = summary ? summary : "";
        }
    };
    config_snapshot_foreach_dirty(find_cb, &ft);
    expect_true("case 13 tombstone is dirty", ft.found);
    expect_true("case 13 value_json is NULL", ft.tombstone);
    expect_equal("case 13 carries last-known rv", std::string("1"), ft.rv);
    expect_equal("case 13 summary accumulates delete", std::string("apply config; remove config"),
        ft.summary);

    // An etcd delete arriving on the watch is skipped while the tombstone is
    // still dirty — the delete proposal has not been reported yet, and the
    // mirror must never swallow an unreported offline edit.
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "42", NULL);
    FindTomb ft2;
    config_snapshot_foreach_dirty(find_cb, &ft2);
    expect_true("case 13 watch delete defers to the dirty tombstone",
        ft2.found && ft2.tombstone);

    // Report pass, moot branch: the etcd key is already gone (the controller
    // deleted it too), so the delete proposal is moot — the report clears the
    // flag with the seq it observed and sends nothing. After that the
    // tombstone is clean and the dirty scan no longer surfaces it.
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "42",
        dirty_seq_of(SNAPSHOT_KIND_HSI, "42"));
    FindTomb ft2b;
    config_snapshot_foreach_dirty(find_cb, &ft2b);
    expect_true("case 13 moot proposal clears via the report pass", !ft2b.found);

    // Deleting an already-absent key is an idempotent no-op (no new proposal).
    expect_equal("case 13 delete absent key is a no-op", SUCCESS,
        config_snapshot_offline_delete(SNAPSHOT_KIND_HSI, "43", "remove config"));
    FindTomb ft3;
    config_snapshot_foreach_dirty(find_cb, &ft3);
    expect_true("case 13 absent-key delete adds nothing", !ft3.found);
}

static void test_snapshot_boot_apply_skips_deleted()
{
    std::cout << "Case 14: boot apply skips deleted entries (no resurrection)" << std::endl;

    // Three entries: one live, one cleanly deleted by a watch DELETE event
    // (controller-driven), one offline-delete tombstone. The boot-time apply
    // path (config_snapshot_foreach, non-dirty) must only see the live one —
    // a deleted config must never resurrect on a degraded boot.
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "50",
        "{\"config\":{\"vlan_id\":\"10\"},\"metadata\":{\"resourceVersion\":\"4\"}}");
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "51",
        "{\"config\":{\"vlan_id\":\"11\"},\"metadata\":{\"resourceVersion\":\"5\"}}");
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "51", NULL);   // controller delete
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "52",
        "{\"config\":{\"vlan_id\":\"12\"},\"metadata\":{\"resourceVersion\":\"6\"}}");
    config_snapshot_offline_delete(SNAPSHOT_KIND_HSI, "52", "remove config");

    struct SeenUsers {
        bool u50 = false, u51 = false, u52 = false;
    } seen;
    auto seen_cb = [](snapshot_kind_t, const char *uid, const char *value_json,
        const char *, int64_t, const char *, uint64_t, void *ud) {
        SeenUsers *s = (SeenUsers *)ud;
        std::string u(uid);
        if (u == "50") s->u50 = (value_json != NULL);
        if (u == "51") s->u51 = true;
        if (u == "52") s->u52 = true;
    };
    config_snapshot_foreach(seen_cb, &seen);
    expect_true("case 14 live entry visible with value", seen.u50);
    expect_true("case 14 watch-deleted entry skipped", !seen.u51);
    expect_true("case 14 tombstoned entry skipped", !seen.u52);

    // Clean up the tombstone's dirty flag so later cases see a known state.
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "52",
        dirty_seq_of(SNAPSHOT_KIND_HSI, "52"));
}

static void test_snapshot_delete_recreate_rv_chain()
{
    std::cout << "Case 15: delete->recreate continues the rv chain" << std::endl;

    // Build a chain: create (rv 1) -> edit (rv 2) -> offline delete
    // (tombstone keeps last-known rv 2) -> recreate. The recreate must stamp
    // rv 3 (chain continues), NOT reset to 1 — a reset-to-1 proposal would
    // always lose against the still-existing etcd key.
    config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "60",
        "{\"config\":{\"vlan_id\":\"20\"}}", "create");
    config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "60",
        "{\"config\":{\"vlan_id\":\"21\"}}", "edit");
    config_snapshot_offline_delete(SNAPSHOT_KIND_HSI, "60", "remove config");
    config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "60",
        "{\"config\":{\"vlan_id\":\"22\"}}", "recreate");

    char *v = config_snapshot_get(SNAPSHOT_KIND_HSI, "60");
    Json::Value root;
    expect_true("case 15 recreate is live again", v != NULL && parse_json(v, root));
    expect_equal("case 15 recreate continues rv chain", std::string("3"),
        root["metadata"]["resourceVersion"].asString());
    free(v);

    // A key deleted by the CONTROLLER (watch NULL: no kept value) then
    // recreated offline is a genuinely new key: rv resets to 1.
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "61",
        "{\"config\":{\"vlan_id\":\"30\"},\"metadata\":{\"resourceVersion\":\"9\"}}");
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "61", NULL);
    config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "61",
        "{\"config\":{\"vlan_id\":\"31\"}}", "recreate after controller delete");
    v = config_snapshot_get(SNAPSHOT_KIND_HSI, "61");
    Json::Value root2;
    expect_true("case 15 controller-deleted recreate parses", v != NULL && parse_json(v, root2));
    expect_equal("case 15 controller-deleted recreate resets rv", std::string("1"),
        root2["metadata"]["resourceVersion"].asString());
    free(v);

    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "60",
        dirty_seq_of(SNAPSHOT_KIND_HSI, "60"));
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "61",
        dirty_seq_of(SNAPSHOT_KIND_HSI, "61"));
}

/* ---- compare-and-clear cases (dirty-flag read-send-clear race): the report
 * path copies the dirty set out of the lock, does slow I/O per entry, then
 * clears dirty. An offline edit landing inside that window must survive the
 * clear. The race is reproduced deterministically by editing the entry from
 * inside the foreach_dirty callback — same interleaving as a gRPC-thread
 * edit racing the watchdog-thread report, no timing involved. ---- */

static void test_snapshot_clear_dirty_matching_seq()
{
    std::cout << "Case 16: clear_dirty with the observed seq clears (no concurrent edit)" << std::endl;

    config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "70",
        "{\"config\":{\"vlan_id\":\"40\"}}", "edit-70");
    DirtyEntryProbe before = probe_dirty_entry(SNAPSHOT_KIND_HSI, "70");
    expect_true("case 16 entry is dirty after edit", before.found);

    // No edit happened since the foreach copy: the seq matches and the clear
    // goes through, exactly like the pre-seq behaviour.
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "70", before.seq);
    DirtyEntryProbe after = probe_dirty_entry(SNAPSHOT_KIND_HSI, "70");
    expect_true("case 16 matching seq clears dirty", !after.found);
}

// Simulates the racing edit: runs inside the foreach_dirty callback for the
// target entry, i.e. after the dirty set was copied but before the report
// path clears the flag.
struct RaceCtx {
    const char *uid = "";
    bool hit = false;
    bool delete_in_window = false;   // in-window edit is an offline_delete
};

static void race_edit_cb(snapshot_kind_t kind, const char *user_id,
    const char *value_json, const char *resource_version, int64_t edited_at,
    const char *edit_summary, uint64_t edit_seq, void *user_data)
{
    (void)value_json; (void)resource_version; (void)edited_at; (void)edit_summary;
    RaceCtx *r = (RaceCtx *)user_data;
    if (kind != SNAPSHOT_KIND_HSI || std::string(user_id) != r->uid)
        return;
    r->hit = true;
    // The in-window edit (what the gRPC thread would do while the watchdog
    // thread is mid-report on the copied value)...
    if (r->delete_in_window)
        config_snapshot_offline_delete(SNAPSHOT_KIND_HSI, r->uid, "in-window delete");
    else
        config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, r->uid,
            "{\"config\":{\"vlan_id\":\"51\"}}", "in-window edit");
    // ...then the report path finishes and clears with the seq it observed
    // before the edit. The stale seq must NOT clear the new edit's dirty flag.
    config_snapshot_clear_dirty(kind, user_id, edit_seq);
}

static void test_snapshot_clear_dirty_edit_race()
{
    std::cout << "Case 17: edit inside the report window survives a stale clear" << std::endl;

    config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "71",
        "{\"config\":{\"vlan_id\":\"50\"}}", "edit-71");

    RaceCtx r;
    r.uid = "71";
    config_snapshot_foreach_dirty(race_edit_cb, &r);
    expect_true("case 17 race callback ran", r.hit);

    // The stale-seq clear must have been a no-op: still dirty, and the next
    // scan (the next report tick) sees the in-window value.
    DirtyEntryProbe p = probe_dirty_entry(SNAPSHOT_KIND_HSI, "71");
    expect_true("case 17 entry stays dirty", p.found);
    Json::Value root;
    expect_true("case 17 next scan value parses", parse_json(p.value, root));
    expect_equal("case 17 next scan sees the new value", std::string("51"),
        root["config"]["vlan_id"].asString());

    // With the up-to-date seq the clear works again (window closed).
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "71", p.seq);
    expect_true("case 17 fresh seq clears dirty",
        !probe_dirty_entry(SNAPSHOT_KIND_HSI, "71").found);
}

static void test_snapshot_clear_dirty_delete_race()
{
    std::cout << "Case 18: delete inside the report window survives a stale clear" << std::endl;

    config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "72",
        "{\"config\":{\"vlan_id\":\"60\"}}", "edit-72");

    RaceCtx r;
    r.uid = "72";
    r.delete_in_window = true;
    config_snapshot_foreach_dirty(race_edit_cb, &r);
    expect_true("case 18 race callback ran", r.hit);

    // The offline delete landed in the window: the stale clear must not eat
    // it, and the next scan surfaces the tombstone (delete proposal).
    DirtyEntryProbe p = probe_dirty_entry(SNAPSHOT_KIND_HSI, "72");
    expect_true("case 18 entry stays dirty", p.found);
    expect_true("case 18 next scan sees the tombstone", p.tombstone);

    // With the up-to-date seq the tombstone's flag clears normally.
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "72", p.seq);
    expect_true("case 18 fresh seq clears dirty",
        !probe_dirty_entry(SNAPSHOT_KIND_HSI, "72").found);
}

/* ---- persist-failure cases: CONFIG_SNAPSHOT_PATH is re-read on every
 * persist, so pointing it into a nonexistent directory makes the .tmp open
 * fail deterministically (a read-only directory would not stop root).
 * Teardown restores the good path and clears the dirty flag it created. ---- */

static void test_snapshot_persist_failure_and_recovery(const char *good_path)
{
    std::cout << "Case 19: persist failure flips persist_ok, is sticky, and recovers" << std::endl;

    expect_equal("case 19 baseline persist_ok", (BOOL)TRUE, config_snapshot_persist_ok());

    // Break persistence: the snapshot's parent directory does not exist.
    setenv("CONFIG_SNAPSHOT_PATH",
        "/tmp/fastrg_test_snapshot_missing_dir/snapshot.json", 1);

    // The API keeps its semantics: the edit is applied in memory and reports
    // SUCCESS even though the disk write failed (durability is out-of-band).
    expect_equal("case 19 edit returns SUCCESS despite persist failure", SUCCESS,
        config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "80",
            "{\"config\":{\"vlan_id\":\"80\"}}", "edit-80"));
    expect_equal("case 19 persist_ok FALSE after failure", (BOOL)FALSE,
        config_snapshot_persist_ok());

    // The in-memory effect survives: get sees the new value, dirty is set.
    char *v = config_snapshot_get(SNAPSHOT_KIND_HSI, "80");
    Json::Value root;
    expect_true("case 19 memory value present", v != NULL && parse_json(v, root));
    expect_equal("case 19 memory value applied", std::string("80"),
        root["config"]["vlan_id"].asString());
    free(v);
    expect_true("case 19 entry marked dirty",
        probe_dirty_entry(SNAPSHOT_KIND_HSI, "80").found);

    // Sticky failure: repeated mutations while persist keeps failing must not
    // crash and keep persist_ok FALSE (the transition reporting itself only
    // fires on ok->fail, which stderr shows but the test cannot assert).
    for (int i = 0; i < 5; i++) {
        expect_equal("case 19 repeated edit while failing", SUCCESS,
            config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "80",
                "{\"config\":{\"vlan_id\":\"81\"}}", "edit-80-again"));
    }
    expect_equal("case 19 delete while failing", SUCCESS,
        config_snapshot_offline_delete(SNAPSHOT_KIND_HSI, "80", "delete-80"));
    expect_equal("case 19 persist_ok stays FALSE while failing", (BOOL)FALSE,
        config_snapshot_persist_ok());

    // Recovery: restore the writable path; the next mutation persists again.
    setenv("CONFIG_SNAPSHOT_PATH", good_path, 1);
    expect_equal("case 19 recovery edit", SUCCESS,
        config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "80",
            "{\"config\":{\"vlan_id\":\"82\"}}", "recover-80"));
    expect_equal("case 19 persist_ok TRUE after recovery", (BOOL)TRUE,
        config_snapshot_persist_ok());

    // Leave a clean dirty set for anything running after this case.
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "80",
        dirty_seq_of(SNAPSHOT_KIND_HSI, "80"));
}

/* ---- watch-update dirty-skip cases: the etcd mirror
 * (config_snapshot_watch_update) must never overwrite an entry holding an
 * unreported offline edit — boot load and reconcile both funnel through it,
 * so the skip inside the function covers every mirror entry point. ---- */

static void test_snapshot_watch_update_dirty_skip()
{
    std::cout << "Case 20: watch update skips dirty entries until the report clears them" << std::endl;

    // (a) Dirty live entry: the whole mirror write is skipped —
    // value/exists/dirty/summary/edit_seq all untouched.
    config_snapshot_offline_edit(SNAPSHOT_KIND_HSI, "90",
        "{\"config\":{\"vlan_id\":\"90\"}}", "edit-90");
    DirtyEntryProbe before = probe_dirty_entry(SNAPSHOT_KIND_HSI, "90");
    expect_true("case 20 entry is dirty after the edit", before.found);

    struct SummaryProbe {
        std::string summary;
        bool found = false;
    } sp_before, sp_after;
    auto summary_cb = [](snapshot_kind_t kind, const char *uid, const char *,
        const char *, int64_t, const char *summary, uint64_t, void *ud) {
        SummaryProbe *s = (SummaryProbe *)ud;
        if (kind == SNAPSHOT_KIND_HSI && std::string(uid) == "90") {
            s->found = true;
            s->summary = summary ? summary : "";
        }
    };
    config_snapshot_foreach_dirty(summary_cb, &sp_before);

    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "90",
        "{\"config\":{\"vlan_id\":\"900\"},\"metadata\":{\"resourceVersion\":\"9\"}}");
    DirtyEntryProbe after = probe_dirty_entry(SNAPSHOT_KIND_HSI, "90");
    expect_true("case 20 entry stays dirty after the watch", after.found);
    expect_equal("case 20 edit_seq unchanged", before.seq, after.seq);
    expect_equal("case 20 proposal value unchanged", before.value, after.value);
    config_snapshot_foreach_dirty(summary_cb, &sp_after);
    expect_true("case 20 summary probe found the entry", sp_before.found && sp_after.found);
    expect_equal("case 20 summary unchanged", sp_before.summary, sp_after.summary);
    char *v = config_snapshot_get(SNAPSHOT_KIND_HSI, "90");
    Json::Value root;
    expect_true("case 20 offline value still readable", v != NULL && parse_json(v, root));
    expect_equal("case 20 offline value kept", std::string("90"),
        root["config"]["vlan_id"].asString());
    free(v);

    // (b) Clean entry: watch updates land exactly as before and never set
    // dirty.
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "91",
        "{\"config\":{\"vlan_id\":\"91\"},\"metadata\":{\"resourceVersion\":\"3\"}}");
    v = config_snapshot_get(SNAPSHOT_KIND_HSI, "91");
    expect_true("case 20 clean mirror lands", v != NULL && parse_json(v, root));
    expect_equal("case 20 clean mirrored value", std::string("91"),
        root["config"]["vlan_id"].asString());
    free(v);
    expect_true("case 20 clean entry stays clean",
        !probe_dirty_entry(SNAPSHOT_KIND_HSI, "91").found);
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "91",
        "{\"config\":{\"vlan_id\":\"910\"},\"metadata\":{\"resourceVersion\":\"4\"}}");
    v = config_snapshot_get(SNAPSHOT_KIND_HSI, "91");
    expect_true("case 20 clean re-mirror lands", v != NULL && parse_json(v, root));
    expect_equal("case 20 clean re-mirrored value", std::string("910"),
        root["config"]["vlan_id"].asString());
    free(v);

    // (c) Dirty tombstone (offline delete): watch updates — value or delete —
    // are skipped the same way.
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "92",
        "{\"config\":{\"vlan_id\":\"92\"},\"metadata\":{\"resourceVersion\":\"5\"}}");
    expect_equal("case 20 offline delete", SUCCESS,
        config_snapshot_offline_delete(SNAPSHOT_KIND_HSI, "92", "delete-92"));
    DirtyEntryProbe tomb = probe_dirty_entry(SNAPSHOT_KIND_HSI, "92");
    expect_true("case 20 tombstone is dirty", tomb.found && tomb.tombstone);

    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "92",
        "{\"config\":{\"vlan_id\":\"920\"},\"metadata\":{\"resourceVersion\":\"6\"}}");
    DirtyEntryProbe tomb2 = probe_dirty_entry(SNAPSHOT_KIND_HSI, "92");
    expect_true("case 20 tombstone survives a watch value", tomb2.found && tomb2.tombstone);
    expect_equal("case 20 tombstone edit_seq unchanged", tomb.seq, tomb2.seq);
    v = config_snapshot_get(SNAPSHOT_KIND_HSI, "92");
    expect_true("case 20 tombstone still hides the value", v == NULL);
    free(v);

    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "92", NULL);
    DirtyEntryProbe tomb3 = probe_dirty_entry(SNAPSHOT_KIND_HSI, "92");
    expect_true("case 20 tombstone survives a watch delete", tomb3.found && tomb3.tombstone);

    // (d) Convergence: the report flow (foreach_dirty + compare-and-clear with
    // the observed seq) clears dirty, then the next watch update lands.
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "90", after.seq);
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "90",
        "{\"config\":{\"vlan_id\":\"900\"},\"metadata\":{\"resourceVersion\":\"9\"}}");
    expect_true("case 20 reported entry is clean",
        !probe_dirty_entry(SNAPSHOT_KIND_HSI, "90").found);
    v = config_snapshot_get(SNAPSHOT_KIND_HSI, "90");
    expect_true("case 20 mirror lands after the report", v != NULL && parse_json(v, root));
    expect_equal("case 20 mirrored value after the report", std::string("900"),
        root["config"]["vlan_id"].asString());
    free(v);

    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "92", tomb3.seq);
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "92",
        "{\"config\":{\"vlan_id\":\"920\"},\"metadata\":{\"resourceVersion\":\"6\"}}");
    expect_true("case 20 reported tombstone is clean",
        !probe_dirty_entry(SNAPSHOT_KIND_HSI, "92").found);
    v = config_snapshot_get(SNAPSHOT_KIND_HSI, "92");
    expect_true("case 20 tombstone mirror lands after the report",
        v != NULL && parse_json(v, root));
    expect_equal("case 20 recreated value after the report", std::string("920"),
        root["config"]["vlan_id"].asString());
    free(v);
}

static void test_hsi_ipv6_parse_defaults()
{
    std::cout << "Case 21: HSI IPv6 parsing keeps legacy defaults" << std::endl;
    hsi_config_t config = { 0 };
    BOOL is_enabled = TRUE;

    expect_equal("case 21 legacy HSI parses", SUCCESS,
        etcd_client_parse_hsi_config(MERGE_SEED_HSI, &config, &is_enabled));
    expect_equal("case 21 ipv6 defaults false", FALSE, config.ipv6_enable);
    expect_equal("case 21 dns proxy defaults true", TRUE, config.dns_proxy_enable);
    expect_equal("case 21 conntrack defaults true", TRUE, config.tcp_conntrack_enable);
    hsi_config_free_port_mappings(&config);

    const char *missing_flags = "{\"config\":{\"user_id\":\"8\"}}";
    config = hsi_config_t{ 0 };
    expect_equal("case 21 missing flags HSI parses", SUCCESS,
        etcd_client_parse_hsi_config(missing_flags, &config, NULL));
    expect_equal("case 21 missing ipv6 defaults false", FALSE, config.ipv6_enable);
    expect_equal("case 21 missing dns proxy defaults true", TRUE, config.dns_proxy_enable);
    expect_equal("case 21 missing conntrack defaults true", TRUE, config.tcp_conntrack_enable);
}

static void expect_ipv6_parse(const std::string& assertion, const char *json, BOOL expected)
{
    hsi_config_t config = { 0 };
    expect_equal(assertion + " parses", SUCCESS,
        etcd_client_parse_hsi_config(json, &config, NULL));
    expect_equal(assertion + " value", expected, config.ipv6_enable);
    hsi_config_free_port_mappings(&config);
}

static void test_hsi_ipv6_parse_explicit_values()
{
    std::cout << "Case 22: HSI IPv6 parsing accepts bool, string, and integer values" << std::endl;
    expect_ipv6_parse("case 22 bool true", "{\"config\":{\"ipv6_enable\":true}}", TRUE);
    expect_ipv6_parse("case 22 bool false", "{\"config\":{\"ipv6_enable\":false}}", FALSE);
    expect_ipv6_parse("case 22 string true", "{\"config\":{\"ipv6_enable\":\"true\"}}", TRUE);
    expect_ipv6_parse("case 22 string false", "{\"config\":{\"ipv6_enable\":\"false\"}}", FALSE);
    expect_ipv6_parse("case 22 integer one", "{\"config\":{\"ipv6_enable\":1}}", TRUE);
    expect_ipv6_parse("case 22 integer zero", "{\"config\":{\"ipv6_enable\":0}}", FALSE);
}

static void expect_ipv6_round_trip(const std::string& assertion, BOOL expected)
{
    hsi_config_t source = { 0 };
    source.dns_proxy_enable = TRUE;
    source.tcp_conntrack_enable = TRUE;
    source.ipv6_enable = expected;

    char *rendered = etcd_client_render_hsi_config("cas-test-node", &source);
    expect_true(assertion + " renders", rendered != NULL);
    if (!rendered)
        return;

    Json::Value root;
    expect_true(assertion + " rendered JSON parses", parse_json(rendered, root));
    expect_true(assertion + " rendered field exists",
        root["config"].isMember("ipv6_enable"));
    expect_equal(assertion + " rendered value", expected == TRUE,
        root["config"]["ipv6_enable"].asBool());

    hsi_config_t parsed = { 0 };
    expect_equal(assertion + " parses back", SUCCESS,
        etcd_client_parse_hsi_config(rendered, &parsed, NULL));
    expect_equal(assertion + " round-trip value", expected, parsed.ipv6_enable);
    hsi_config_free_port_mappings(&parsed);
    free(rendered);
}

static void test_hsi_ipv6_render_round_trip()
{
    std::cout << "Case 23: HSI IPv6 render round-trip preserves both values" << std::endl;
    expect_ipv6_round_trip("case 23 true", TRUE);
    expect_ipv6_round_trip("case 23 false", FALSE);
}

static void test_field_merge_ipv6()
{
    std::cout << "Case 24: field merge flips ipv6_enable" << std::endl;

    // The seed carries no ipv6_enable at all (the legacy shape): the merge must
    // create the field rather than fail, so an offline toggle works on configs
    // written before IPv6 existed.
    std::string enabled = run_merge("case 24 ipv6 on", SNAPSHOT_FIELD_KIND_IPV6,
        MERGE_SEED_HSI, "true");
    Json::Value root;
    expect_true("case 24 enable parses", parse_json(enabled, root));
    const Json::Value& cfg = root["config"];
    expect_equal("case 24 ipv6 enabled", true, cfg["ipv6_enable"].asBool());
    expect_equal("case 24 dns flag preserved", true, cfg["dns_proxy_enable"].asBool());
    expect_equal("case 24 tcp flag preserved", true, cfg["tcp_conntrack_enable"].asBool());
    expect_equal("case 24 vlan preserved", std::string("123"), cfg["vlan_id"].asString());
    expect_equal("case 24 desire_status preserved", std::string("connect"),
        cfg["desire_status"].asString());
    expect_equal("case 24 mapping count preserved", 1u, cfg["port-mapping"].size());
    expect_equal("case 24 metadata preserved", std::string("seed"),
        root["metadata"]["updatedBy"].asString());

    // Toggling back off must write false, not drop the field.
    Json::Value root2;
    expect_true("case 24 disable parses", parse_json(
        run_merge("case 24 ipv6 off", SNAPSHOT_FIELD_KIND_IPV6,
            enabled.c_str(), "false"), root2));
    expect_true("case 24 disabled field exists",
        root2["config"].isMember("ipv6_enable"));
    expect_equal("case 24 ipv6 disabled", false,
        root2["config"]["ipv6_enable"].asBool());

    // HSI kinds require an existing config: absent key must fail.
    char *out = NULL;
    expect_equal("case 24 absent config fails", ERROR,
        config_snapshot_field_merge(SNAPSHOT_FIELD_KIND_IPV6, NULL, "true", &out));
    free(out);
}

int main()
{
    // Point the snapshot at a scratch file so the test never touches the
    // node's real /etc/fastrg/config_snapshot.json.
    char path[] = "/tmp/fastrg_test_snapshot_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);
    setenv("CONFIG_SNAPSHOT_PATH", path, 1);
    std::remove(path);   // start with no file

    if (config_snapshot_init() != SUCCESS) {
        std::cerr << "FAIL: config_snapshot_init" << std::endl;
        return 1;
    }

    test_field_merge_preserves_untouched_fields();
    test_field_merge_port_mappings();
    test_field_merge_dns_records();
    test_snapshot_rv_stamping();
    test_snapshot_dirty_semantics();
    test_snapshot_persistence();
    test_snapshot_content_equal();
    test_snapshot_offline_delete();
    test_snapshot_boot_apply_skips_deleted();
    test_snapshot_delete_recreate_rv_chain();
    test_snapshot_clear_dirty_matching_seq();
    test_snapshot_clear_dirty_edit_race();
    test_snapshot_clear_dirty_delete_race();
    test_snapshot_persist_failure_and_recovery(path);
    test_snapshot_watch_update_dirty_skip();
    test_hsi_ipv6_parse_defaults();
    test_hsi_ipv6_parse_explicit_values();
    test_hsi_ipv6_render_round_trip();
    test_field_merge_ipv6();

    config_snapshot_cleanup();
    std::remove(path);

    if (failures != 0) {
        std::cerr << failures << " offline-edit assertion(s) failed" << std::endl;
        return 1;
    }

    std::cout << "All etcd CAS put tests passed" << std::endl;
    return 0;
}
