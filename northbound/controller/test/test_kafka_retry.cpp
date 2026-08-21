#include <cstdio>
#include <cstddef>
#include <cstdlib>

#include "../kafka_retry_policy.h"

// The policy methods live in kafka_producer.cpp, so linking this test pulls in
// the producer and the etcd client with it. etcd_client.o calls parse_user_id
// from its watcher-thread filter, whose real definition is in
// src/etcd_integration.c; that file is not part of this test, so stand in for
// it the same way test_etcd_cas.cpp does. Nothing here is exercised by the
// cases below.
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

/* Only the flagged positions come back, and they stay in arrival order. */
static void test_select_picks_failed_entries(void)
{
    const unsigned char flags[] = {0, 1, 0, 1, 1};
    std::size_t hits[5];
    std::size_t n = KafkaRetryPolicy::select(flags, 5, hits, 5);

    check(n == 3, "select returns one index per failed entry", "wrong count");
    check(n == 3 && hits[0] == 1 && hits[1] == 3 && hits[2] == 4,
        "select keeps the entries in order", "wrong indices");
}

/* Nothing flagged must produce no work at all. */
static void test_select_ignores_delivered_entries(void)
{
    const unsigned char flags[] = {0, 0, 0};
    std::size_t hits[3];

    check(KafkaRetryPolicy::select(flags, 3, hits, 3) == 0,
        "select returns nothing when every delivery succeeded", "expected 0");
}

/* An empty buffer is not an error; it simply has nothing to retry. */
static void test_select_handles_empty_input(void)
{
    const unsigned char flags[1] = {0};
    std::size_t hits[1];

    check(KafkaRetryPolicy::select(flags, 0, hits, 1) == 0,
        "select on an empty buffer returns nothing", "expected 0");
    check(KafkaRetryPolicy::select(nullptr, 3, hits, 1) == 0,
        "select rejects a null flag array", "expected 0");
    check(KafkaRetryPolicy::select(flags, 1, nullptr, 1) == 0,
        "select rejects a null output array", "expected 0");
}

/* The caller's buffer is the hard limit; a full one must not be overrun. */
static void test_select_respects_output_capacity(void)
{
    const unsigned char flags[] = {1, 1, 1, 1};
    std::size_t hits[2];
    std::size_t n = KafkaRetryPolicy::select(flags, 4, hits, 2);

    check(n == 2, "select stops at the output capacity", "wrote past capacity");
    check(n == 2 && hits[0] == 0 && hits[1] == 1,
        "select fills the capacity with the oldest entries", "wrong indices");
}

/* The sweep is owed once the interval has elapsed, and not before. */
static void test_is_due_honours_the_interval(void)
{
    check(KafkaRetryPolicy::is_due(100, 100, 30) == 0,
        "no sweep is due immediately after one ran", "fired too early");
    check(KafkaRetryPolicy::is_due(129, 100, 30) == 0,
        "no sweep is due one second short of the interval", "fired too early");
    check(KafkaRetryPolicy::is_due(130, 100, 30) != 0,
        "a sweep is due exactly on the interval", "did not fire");
    check(KafkaRetryPolicy::is_due(1000, 100, 30) != 0,
        "a sweep is due long after the interval", "did not fire");
}

/* A non-positive interval means the caller wants every pass to sweep. */
static void test_is_due_with_no_interval(void)
{
    check(KafkaRetryPolicy::is_due(100, 100, 0) != 0,
        "a zero interval makes every pass due", "did not fire");
    check(KafkaRetryPolicy::is_due(100, 100, -5) != 0,
        "a negative interval makes every pass due", "did not fire");
}

int main(void)
{
    std::printf("Kafka retry policy tests\n");
    std::printf("========================\n");

    test_select_picks_failed_entries();
    test_select_ignores_delivered_entries();
    test_select_handles_empty_input();
    test_select_respects_output_capacity();
    test_is_due_honours_the_interval();
    test_is_due_with_no_interval();

    std::printf("------------------------\n");
    std::printf("Total: %d  Pass: %d  Fail: %d\n", g_total, g_pass, g_total - g_pass);
    if (g_pass != g_total) {
        std::printf("Kafka retry policy tests FAILED\n");
        return 1;
    }
    std::printf("All Kafka retry policy tests passed\n");
    return 0;
}
