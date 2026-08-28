#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../kafka_producer_wal.h"

// Unit tests for the pure functions kafka_producer.cpp exports through
// kafka_producer_wal.h: the WAL's serialize/parse pair and the event
// payload builder. Neither needs a broker, a WAL file or etcd.
//
// Linking them pulls in the producer and the etcd client with it. etcd_client.o
// calls parse_user_id from its watcher-thread filter, whose real definition is
// in src/etcd_integration.c; that file is not part of this test, so stand in
// for it the same way test_kafka_retry.cpp does. Nothing here is exercised by
// the cases below.
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
    return (int)val - 1;
}
}

namespace ev = fastrg::events::v1;

static int g_total;
static int g_pass;

static void check(int cond, const char *name, const char *detail)
{
    g_total++;
    if (cond) {
        g_pass++;
        std::printf("  [PASS] %s\n", name);
    } else {
        std::printf("  [FAIL] %s: %s\n", name, detail);
    }
}

/* ------------------------------------------------------------------ */
/* WAL serialization                                                   */
/* ------------------------------------------------------------------ */

/* Build events whose payloads carry bytes a text format cannot hold as-is. */
static std::vector<KafkaWalEvent> sample_events(void)
{
    std::vector<KafkaWalEvent> v;
    KafkaWalEvent a;
    a.seq = 1;
    a.payload = std::string("\x00\xff", 2);   /* NUL and a high byte */
    a.persistent = true;
    v.push_back(a);
    KafkaWalEvent b;
    b.seq = 2;
    b.payload = "A";
    b.persistent = true;
    v.push_back(b);
    return v;
}

/* The on-disk shape is a file-compatibility contract: one compact JSON array,
 * one object per event, payload hex-encoded. */
static void test_kafka_wal_serialize_writes_the_file_format(void)
{
    std::string data = kafka_wal_serialize(sample_events());
    const char *want = "[{\"payload\":\"00ff\",\"seq\":1},{\"payload\":\"41\",\"seq\":2}]";

    check(data == want, "serialize emits the WAL's compact JSON array", data.c_str());
    check(kafka_wal_serialize(std::vector<KafkaWalEvent>()) == "[]",
        "serialize renders an empty WAL as an empty array", "expected []");
}

/* The file holds the durable subset and nothing else: an event buffered for
 * delivery but not marked durable must leave no trace on disk. This is the
 * rule that keeps a burst of state events off the disk entirely. */
static void test_kafka_wal_serialize_writes_only_durable_events(void)
{
    std::vector<KafkaWalEvent> v = sample_events();
    KafkaWalEvent transient;
    transient.seq = 3;
    transient.payload = "B";
    transient.persistent = false;
    v.push_back(transient);

    std::string data = kafka_wal_serialize(v);
    check(data == kafka_wal_serialize(sample_events()),
        "serialize leaves memory-only events out of the file", data.c_str());
    check(data.find("\"seq\":3") == std::string::npos,
        "a memory-only event's seq never reaches the file", data.c_str());

    std::vector<KafkaWalEvent> only_transient;
    only_transient.push_back(transient);
    check(kafka_wal_serialize(only_transient) == "[]",
        "a buffer holding nothing durable renders as an empty file", "expected []");
}

/* delivery_failed is in-memory retry state, not part of the file. */
static void test_kafka_wal_serialize_omits_delivery_state(void)
{
    std::vector<KafkaWalEvent> v = sample_events();
    v[0].delivery_failed = true;
    v[1].delivery_failed = true;

    std::string data = kafka_wal_serialize(v);
    check(data == kafka_wal_serialize(sample_events()),
        "serialize ignores the delivery_failed flag", data.c_str());
}

/* Every byte value must survive a trip through the file format. */
static void test_kafka_wal_parse_round_trips_serialize(void)
{
    std::vector<KafkaWalEvent> in;
    for (int i = 0; i < 8; i++) {
        KafkaWalEvent e;
        e.seq = 1000 + i;
        e.persistent = true;
        for (int b = 0; b < 256; b++)
            e.payload.push_back((char)((b + i) & 0xff));
        in.push_back(e);
    }

    std::vector<KafkaWalEvent> out;
    bool ok = kafka_wal_parse(kafka_wal_serialize(in), out);

    check(ok, "parse accepts what serialize produced", "returned false");
    check(out.size() == in.size(), "parse recovers every event", "wrong count");
    bool same = out.size() == in.size();
    for (size_t i = 0; same && i < in.size(); i++)
        same = out[i].seq == in[i].seq && out[i].payload == in[i].payload;
    check(same, "parse recovers seq and payload byte for byte", "content differs");
    check(out.size() == in.size() && !out[0].delivery_failed,
        "parse leaves the delivery_failed flag clear", "flag came back set");
    check(out.size() == in.size() && out[0].persistent,
        "parse marks what it read as durable", "persistent came back clear");
}

/* A truncated or hand-edited file must cost only the damaged entries. */
static void test_kafka_wal_parse_skips_bad_entries(void)
{
    std::string data =
        "["
        "{\"payload\":\"41\",\"seq\":1},"        /* good */
        "{\"seq\":2},"                            /* no payload */
        "{\"payload\":\"42\"},"                   /* no seq */
        "{\"payload\":\"4\",\"seq\":3},"          /* odd-length hex */
        "{\"payload\":\"zz\",\"seq\":4},"         /* not hex */
        "{\"payload\":\"43\",\"seq\":\"five\"},"  /* seq is not a number */
        "{\"payload\":68,\"seq\":6},"             /* payload is not a string */
        "{\"payload\":\"44\",\"seq\":7}"          /* good */
        "]";

    std::vector<KafkaWalEvent> out;
    bool ok = kafka_wal_parse(data, out);

    check(ok, "parse still reports a damaged array as usable", "returned false");
    check(out.size() == 2, "parse keeps only the well-formed entries", "wrong count");
    check(out.size() == 2 && out[0].seq == 1 && out[0].payload == "A" &&
          out[1].seq == 7 && out[1].payload == "D",
        "parse keeps the surviving entries intact and in order", "wrong content");
}

/* Anything that is not a JSON array is a corrupt WAL, reported as such. */
static void test_kafka_wal_parse_rejects_corrupt_input(void)
{
    std::vector<KafkaWalEvent> out;

    check(!kafka_wal_parse("this is not json", out),
        "parse rejects text that is not JSON", "returned true");
    check(!kafka_wal_parse("{\"seq\":1,\"payload\":\"41\"}", out),
        "parse rejects a JSON object at the top level", "returned true");
    check(!kafka_wal_parse("[{\"payload\":\"41\",\"seq\":1}", out),
        "parse rejects a truncated array", "returned true");
    check(out.empty(), "parse leaves nothing behind after rejecting input",
        "output not empty");
}

/* No file and an empty file are both simply an empty WAL. */
static void test_kafka_wal_parse_accepts_empty_input(void)
{
    std::vector<KafkaWalEvent> out = sample_events();

    check(kafka_wal_parse("", out), "parse treats empty text as an empty WAL",
        "returned false");
    check(out.empty(), "parse clears the caller's vector", "output not empty");
    check(kafka_wal_parse("[]", out), "parse accepts an empty array", "returned false");
    check(out.empty(), "an empty array yields no events", "output not empty");
}

/* Cost smoke: one WAL write at these sizes is what the batching turns a burst
 * of that many events into, so the per-write cost is the thing to keep an eye
 * on. Correctness is asserted; the timings are printed for the record. */
static void test_kafka_wal_serialize_at_scale(void)
{
    const size_t sizes[] = {1000, 100000};

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t n = sizes[s];
        std::vector<KafkaWalEvent> in;
        in.reserve(n);
        for (size_t i = 0; i < n; i++) {
            KafkaWalEvent e;
            e.seq = (int64_t)i + 1;
            e.persistent = true;
            e.payload.assign(64, (char)(i & 0xff));   /* a typical NodeEvent size */
            in.push_back(std::move(e));
        }

        auto t0 = std::chrono::steady_clock::now();
        std::string data = kafka_wal_serialize(in);
        auto t1 = std::chrono::steady_clock::now();

        std::vector<KafkaWalEvent> out;
        bool ok = kafka_wal_parse(data, out);
        auto t2 = std::chrono::steady_clock::now();

        double ser_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double par_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        std::printf("  [INFO] %zu events: serialize %.1f ms, parse %.1f ms, %zu bytes\n",
            n, ser_ms, par_ms, data.size());

        char name[128];
        std::snprintf(name, sizeof(name),
            "serialize round-trips %zu events", n);
        check(ok && out.size() == n && out[n - 1].seq == (int64_t)n &&
              out[n - 1].payload == in[n - 1].payload,
            name, "scale round-trip lost or corrupted events");
    }
}

/* ------------------------------------------------------------------ */
/* Event payload builder                                               */
/* ------------------------------------------------------------------ */

/* The node names the config version it applied; the controller records that
 * number rather than inferring one from etcd's current value. */
static void test_config_apply_carries_the_applied_revision(void)
{
    ev::ConfigApplyResult c;
    kafka_build_config_apply_result(&c, "update", TRUE, NULL, NULL, "48", FALSE, 4812);

    check(c.applied_mod_revision() == 4812,
        "a successful apply reports the revision it applied",
        std::to_string(c.applied_mod_revision()).c_str());
    check(c.action() == "update" && c.success() && !c.republished() &&
          c.applied_resource_version() == "48",
        "the other apply fields keep their values", c.DebugString().c_str());
}

/* A failure has to be pinned to a version too, otherwise the controller cannot
 * tell which config value the node choked on. */
static void test_config_apply_carries_the_revision_on_failure(void)
{
    ev::ConfigApplyResult c;
    kafka_build_config_apply_result(&c, "create", FALSE, "apply_failed",
        "node failed to apply HSI config", "56", FALSE, 5601);

    check(c.applied_mod_revision() == 5601,
        "a failed apply reports the revision it failed on",
        std::to_string(c.applied_mod_revision()).c_str());
    check(!c.success() && c.error_code() == "apply_failed",
        "a failed apply still carries its error", c.DebugString().c_str());
}

/* A republish confirmation is the case the revision was added for: it says
 * "this exact version is what I am running". */
static void test_republish_confirmation_carries_the_revision(void)
{
    ev::ConfigApplyResult c;
    kafka_build_config_apply_result(&c, "update", TRUE, NULL, NULL, "48", TRUE, 4812);

    check(c.republished() && c.applied_mod_revision() == 4812,
        "a republish confirmation names the version it confirms",
        c.DebugString().c_str());
}

/* 0 is the "not reported" value, and no etcd key ever has ModRevision 0, so
 * the two can never be confused. */
static void test_unknown_revision_is_zero(void)
{
    ev::ConfigApplyResult c;
    kafka_build_config_apply_result(&c, "delete", TRUE, NULL, NULL, NULL, FALSE, 0);

    check(c.applied_mod_revision() == 0,
        "an unknown revision reports as 0", c.DebugString().c_str());
    check(c.applied_resource_version().empty(),
        "a NULL resource version leaves the field empty", c.DebugString().c_str());
}

/* etcd revisions outgrow what a double can hold exactly, so the value has to
 * stay a 64-bit integer end to end. */
static void test_large_revision_is_not_truncated(void)
{
    const int64_t big = 9007199254740993LL;   /* 2^53 + 1 */
    ev::ConfigApplyResult c;
    kafka_build_config_apply_result(&c, "update", TRUE, NULL, NULL, "1", FALSE, big);

    check(c.applied_mod_revision() == big,
        "a revision past 2^53 survives intact",
        std::to_string(c.applied_mod_revision()).c_str());
}

/* The builder writes every field, so a message reused for a second report
 * cannot leak the first one's revision. */
static void test_builder_leaves_nothing_from_an_earlier_call(void)
{
    ev::ConfigApplyResult c;
    kafka_build_config_apply_result(&c, "update", FALSE, "apply_failed", "boom", "48", TRUE, 4812);
    kafka_build_config_apply_result(&c, "update", TRUE, NULL, NULL, NULL, FALSE, 5601);

    check(c.applied_mod_revision() == 5601 && c.error_code().empty() &&
          c.error_message().empty() && c.applied_resource_version().empty() &&
          !c.republished() && c.success(),
        "a reused message keeps nothing from the previous call", c.DebugString().c_str());
}

/* The field has to exist on the wire, not just in this process's message
 * class: a stale generated pb would still compile against an older contract. */
static void test_revision_survives_the_wire(void)
{
    ev::ConfigApplyResult c;
    kafka_build_config_apply_result(&c, "update", TRUE, NULL, NULL, "48", TRUE, 4812);

    std::string bytes;
    check(c.SerializeToString(&bytes), "the payload serializes", "SerializeToString failed");

    ev::ConfigApplyResult back;
    check(back.ParseFromString(bytes), "the payload parses back", "ParseFromString failed");
    check(back.applied_mod_revision() == 4812 && back.republished() &&
          back.applied_resource_version() == "48",
        "the revision comes back off the wire", back.DebugString().c_str());
}

int main(void)
{
    std::printf("Kafka producer unit tests\n");
    std::printf("=========================\n");

    std::printf("-- WAL serialization --\n");
    test_kafka_wal_serialize_writes_the_file_format();
    test_kafka_wal_serialize_writes_only_durable_events();
    test_kafka_wal_serialize_omits_delivery_state();
    test_kafka_wal_parse_round_trips_serialize();
    test_kafka_wal_parse_skips_bad_entries();
    test_kafka_wal_parse_rejects_corrupt_input();
    test_kafka_wal_parse_accepts_empty_input();
    test_kafka_wal_serialize_at_scale();

    std::printf("-- Event payload builder --\n");
    test_config_apply_carries_the_applied_revision();
    test_config_apply_carries_the_revision_on_failure();
    test_republish_confirmation_carries_the_revision();
    test_unknown_revision_is_zero();
    test_large_revision_is_not_truncated();
    test_builder_leaves_nothing_from_an_earlier_call();
    test_revision_survives_the_wire();

    std::printf("-------------------------\n");
    std::printf("Total: %d  Pass: %d  Fail: %d\n", g_total, g_pass, g_total - g_pass);
    if (g_pass != g_total) {
        std::printf("Kafka producer unit tests FAILED\n");
        return 1;
    }
    std::printf("All Kafka producer unit tests passed\n");
    return 0;
}
