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

// etcd_client.o references these from its watcher-thread self-event filter;
// the real definitions live in src/etcd_integration.c. This standalone test
// links etcd_client.o only for field_merge, so provide minimal stand-ins.
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
    const char *edit_summary, void *user_data)
{
    (void)kind; (void)edited_at;
    DirtyProbe *p = (DirtyProbe *)user_data;
    p->count++;
    p->last_user = user_id;
    p->last_summary = edit_summary ? edit_summary : "";
    p->last_rv = resource_version ? resource_version : "";
    p->last_tombstone = (value_json == NULL);
}

static void test_snapshot_dirty_semantics()
{
    std::cout << "Case 10: offline edits set dirty; watch updates clear it" << std::endl;
    DirtyProbe p;
    config_snapshot_foreach_dirty(dirty_probe_cb, &p);
    int base = p.count;

    config_snapshot_offline_edit(SNAPSHOT_KIND_DNS, "5",
        "{\"records\":[{\"domain\":\"x.example\",\"ip\":\"10.0.0.5\",\"ttl\":60}]}",
        "dns add x.example");
    p = DirtyProbe();
    config_snapshot_foreach_dirty(dirty_probe_cb, &p);
    expect_equal("case 10 dirty count grows", base + 1, p.count);

    // A stale-refresh (watch update) clears dirty — a merely-stale snapshot
    // never becomes a proposal
    config_snapshot_watch_update(SNAPSHOT_KIND_DNS, "5",
        "{\"records\":[],\"metadata\":{\"resourceVersion\":\"9\"}}");
    p = DirtyProbe();
    config_snapshot_foreach_dirty(dirty_probe_cb, &p);
    expect_equal("case 10 watch clears dirty", base, p.count);

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
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "9");
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
        const char *rv, int64_t, const char *summary, void *ud) {
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

    // A real etcd delete arriving on the watch clears the tombstone — the
    // controller deleted it too, so the proposal is moot.
    config_snapshot_watch_update(SNAPSHOT_KIND_HSI, "42", NULL);
    FindTomb ft2;
    config_snapshot_foreach_dirty(find_cb, &ft2);
    expect_true("case 13 watch delete clears the tombstone", !ft2.found);

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
        const char *, int64_t, const char *, void *ud) {
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
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "52");
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

    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "60");
    config_snapshot_clear_dirty(SNAPSHOT_KIND_HSI, "61");
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

    config_snapshot_cleanup();
    std::remove(path);

    if (failures != 0) {
        std::cerr << failures << " offline-edit assertion(s) failed" << std::endl;
        return 1;
    }

    std::cout << "All etcd CAS put tests passed" << std::endl;
    return 0;
}
