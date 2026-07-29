#include <stdlib.h>
#include <string.h>

#include <common.h>

#include <rte_lcore.h>
#include <rte_rcu_qsbr.h>

#include "../src/fastrg.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

static BOOL stats_equal(const struct per_ccb_stats *actual, const struct per_ccb_stats *expected)
{
    return actual->rx_packets == expected->rx_packets &&
        actual->rx_bytes == expected->rx_bytes &&
        actual->tx_packets == expected->tx_packets &&
        actual->tx_bytes == expected->tx_bytes &&
        actual->dropped_packets == expected->dropped_packets &&
        actual->dropped_bytes == expected->dropped_bytes;
}

static void test_subscriber_stats_reuse_moves_unknown_slot(void)
{
    FastRG_t test_ccb = {0};
    struct rte_rcu_qsbr *stats_rcu = NULL;
    const struct per_ccb_stats user_stats = {
        .rx_packets = 11,
        .rx_bytes = 12,
        .tx_packets = 13,
        .tx_bytes = 14,
        .dropped_packets = 15,
        .dropped_bytes = 16,
    };
    const struct per_ccb_stats unknown_stats = {
        .rx_packets = 21,
        .rx_bytes = 22,
        .tx_packets = 23,
        .tx_bytes = 24,
        .dropped_packets = 25,
        .dropped_bytes = 26,
    };
    const struct per_ccb_stats zero_stats = {0};
    size_t rcu_size = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    unsigned int lcore_id;
    unsigned int main_lcore = rte_get_main_lcore();

    TEST_ASSERT(posix_memalign((void **)&stats_rcu, RTE_CACHE_LINE_SIZE, rcu_size) == 0,
        "allocate subscriber stats resize RCU fixture", "allocation failed");
    memset(stats_rcu, 0, rcu_size);
    TEST_ASSERT(rte_rcu_qsbr_init(stats_rcu, RTE_MAX_LCORE) == 0,
        "initialize subscriber stats resize RCU fixture", "initialization failed");

    test_ccb.user_count = 1;
    test_ccb.per_subscriber_stats_len = 2;
    test_ccb.per_subscriber_stats_rcu = stats_rcu;
    rte_atomic16_init(&test_ccb.per_subscriber_stats_updating);

    RTE_LCORE_FOREACH(lcore_id) {
        for (int port_id = 0; port_id < PORT_AMOUNT; port_id++) {
            test_ccb.per_subscriber_stats[lcore_id][port_id] =
                calloc(test_ccb.per_subscriber_stats_len + 1, sizeof(struct per_ccb_stats));
            TEST_ASSERT(test_ccb.per_subscriber_stats[lcore_id][port_id] != NULL,
                "allocate subscriber stats reuse fixture row", "lcore %u port %d allocation failed",
                lcore_id, port_id);
        }
    }

    test_ccb.per_subscriber_stats[main_lcore][WAN_PORT][0] = user_stats;
    test_ccb.per_subscriber_stats[main_lcore][WAN_PORT][test_ccb.user_count] = unknown_stats;

    TEST_ASSERT(fastrg_add_subscriber_stats(&test_ccb, 1) == SUCCESS,
        "resize subscriber stats despite reusable capacity", "resize failed");
    TEST_ASSERT(stats_equal(&test_ccb.per_subscriber_stats[main_lcore][WAN_PORT][0], &user_stats),
        "subscriber stats resize preserves existing user counters", "user 0 counters changed");
    TEST_ASSERT(stats_equal(&test_ccb.per_subscriber_stats[main_lcore][WAN_PORT][1], &zero_stats),
        "subscriber stats resize leaves new user counters zero", "new user inherited unknown counters");
    TEST_ASSERT(stats_equal(&test_ccb.per_subscriber_stats[main_lcore][WAN_PORT][2], &unknown_stats),
        "subscriber stats resize moves unknown counters to new tail slot", "unknown counters were not moved");

    RTE_LCORE_FOREACH(lcore_id) {
        for (int port_id = 0; port_id < PORT_AMOUNT; port_id++)
            free(test_ccb.per_subscriber_stats[lcore_id][port_id]);
    }
    free(stats_rcu);
}

static void test_remove_subscriber_stats_updates_length(void)
{
    FastRG_t test_ccb = {0};
    struct rte_rcu_qsbr *stats_rcu = NULL;
    size_t rcu_size = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    unsigned int lcore_id;

    TEST_ASSERT(posix_memalign((void **)&stats_rcu, RTE_CACHE_LINE_SIZE, rcu_size) == 0,
        "allocate subscriber stats remove RCU fixture", "allocation failed");
    memset(stats_rcu, 0, rcu_size);
    TEST_ASSERT(rte_rcu_qsbr_init(stats_rcu, RTE_MAX_LCORE) == 0,
        "initialize subscriber stats remove RCU fixture", "initialization failed");

    test_ccb.user_count = 2;
    test_ccb.per_subscriber_stats_len = 2;
    test_ccb.per_subscriber_stats_rcu = stats_rcu;
    rte_atomic16_init(&test_ccb.per_subscriber_stats_updating);

    RTE_LCORE_FOREACH(lcore_id) {
        for (int port_id = 0; port_id < PORT_AMOUNT; port_id++) {
            test_ccb.per_subscriber_stats[lcore_id][port_id] =
                calloc(test_ccb.per_subscriber_stats_len + 1, sizeof(struct per_ccb_stats));
            TEST_ASSERT(test_ccb.per_subscriber_stats[lcore_id][port_id] != NULL,
                "allocate subscriber stats remove fixture row", "lcore %u port %d allocation failed",
                lcore_id, port_id);
        }
    }

    TEST_ASSERT(fastrg_remove_subscriber_stats(&test_ccb, 1, test_ccb.user_count) == SUCCESS,
        "remove subscriber stats fixture entry", "remove failed");
    TEST_ASSERT(test_ccb.per_subscriber_stats_len == 1,
        "subscriber stats remove updates allocated length", "expected 1, got %u",
        test_ccb.per_subscriber_stats_len);

    RTE_LCORE_FOREACH(lcore_id) {
        for (int port_id = 0; port_id < PORT_AMOUNT; port_id++)
            free(test_ccb.per_subscriber_stats[lcore_id][port_id]);
    }
    free(stats_rcu);
}

static void test_pppoes_stats_remove_then_add_reallocates(void)
{
    FastRG_t test_ccb = {0};
    struct rte_rcu_qsbr *stats_rcu = NULL;
    struct pppoes_lcore_stats *reduced_stats = NULL;
    size_t rcu_size = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    unsigned int lcore_id;
    unsigned int main_lcore = rte_get_main_lcore();

    TEST_ASSERT(posix_memalign((void **)&stats_rcu, RTE_CACHE_LINE_SIZE, rcu_size) == 0,
        "allocate pppoes stats resize RCU fixture", "allocation failed");
    memset(stats_rcu, 0, rcu_size);
    TEST_ASSERT(rte_rcu_qsbr_init(stats_rcu, RTE_MAX_LCORE) == 0,
        "initialize pppoes stats resize RCU fixture", "initialization failed");

    test_ccb.user_count = 2;
    test_ccb.pppoes_stats_len = 2;
    test_ccb.ppp_ccb_rcu = stats_rcu;
    rte_atomic16_init(&test_ccb.pppoes_stats_updating);

    RTE_LCORE_FOREACH(lcore_id) {
        test_ccb.pppoes_stats[lcore_id] =
            calloc(test_ccb.pppoes_stats_len, sizeof(struct pppoes_lcore_stats));
        TEST_ASSERT(test_ccb.pppoes_stats[lcore_id] != NULL,
            "allocate pppoes stats resize fixture row", "lcore %u allocation failed", lcore_id);
    }

    TEST_ASSERT(fastrg_remove_pppoes_stats(&test_ccb, 1, test_ccb.user_count) == SUCCESS,
        "remove pppoes stats fixture entry", "remove failed");
    TEST_ASSERT(test_ccb.pppoes_stats_len == 1,
        "pppoes stats remove updates allocated length", "expected 1, got %u",
        test_ccb.pppoes_stats_len);

    reduced_stats = test_ccb.pppoes_stats[main_lcore];
    test_ccb.user_count = 1;
    TEST_ASSERT(fastrg_add_pppoes_stats(&test_ccb, 1) == SUCCESS,
        "add pppoes stats after remove", "add failed");
    TEST_ASSERT(test_ccb.pppoes_stats_len == 2,
        "pppoes stats add restores allocated length", "expected 2, got %u",
        test_ccb.pppoes_stats_len);
    TEST_ASSERT(test_ccb.pppoes_stats[main_lcore] != reduced_stats,
        "pppoes stats add reallocates reduced row", "add reused the reduced row");

    RTE_LCORE_FOREACH(lcore_id)
        free(test_ccb.pppoes_stats[lcore_id]);
    free(stats_rcu);
}

void test_fastrg(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║                 fastrg Module Unit Tests                  ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    test_subscriber_stats_reuse_moves_unknown_slot();
    test_remove_subscriber_stats_updates_length();
    test_pppoes_stats_remove_then_add_reallocates();

    printf("\nfastrg tests: %d passed, %d failed\n", pass_count, test_count - pass_count);
    *total_tests += test_count;
    *total_pass += pass_count;
}
