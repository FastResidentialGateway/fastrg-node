#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <inttypes.h>

#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include "../src/config.h"
#include "../src/fastrg.h"
#include "../src/init.h"
#include "../src/pppd/pppd.h"
#include "../src/dhcpd/dhcpd.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

void test_parse_config_valid(FastRG_t *fastrg_ccb)
{
    printf("\nTesting parse_config function for valid config:\n");
    printf("=========================================\n\n");

    struct fastrg_config cfg = {0};
    /* parse_config writes user_count into the shared mock ccb; save the
     * capacity too so this case can prove a legacy field does not touch it. */
    U16 saved_user_count = fastrg_ccb->user_count;
    U16 saved_max_user_count = fastrg_ccb->max_user_count;

    /* Create a temporary config file */
    const char *test_config = "/tmp/test_fastrg.conf";
    FILE *fp = fopen(test_config, "w");
    TEST_ASSERT(fp != NULL, "Mock config file", "Create test config file failed");

    fprintf(fp, "MaxUserCount = 100;\n");
    fprintf(fp, "InitUserCount = 10;\n");
    fprintf(fp, "Loglvl = \"INFO\";\n");
    fprintf(fp, "HeartbeatInterval = 60;\n");
    fclose(fp);

    STATUS ret = parse_config(test_config, fastrg_ccb, &cfg);
    TEST_ASSERT(ret == SUCCESS, "Check parse_config return value", 
        "parse_config returns ERROR");
    TEST_ASSERT(fastrg_ccb->max_user_count == saved_max_user_count,
        "legacy MaxUserCount is ignored", "max_user_count changed from %u to %u",
        saved_max_user_count, fastrg_ccb->max_user_count);
    /* user_count is configured after admin set the subscriber count */
    TEST_ASSERT(fastrg_ccb->user_count == 0,
        "user_count should boots at 0", "user_count != 0");
    TEST_ASSERT(cfg.heartbeat_interval == 60, "check heartbeat interval", 
        "HeartbeatInterval != 60");

    fastrg_ccb->user_count = saved_user_count;
    fastrg_ccb->max_user_count = saved_max_user_count;
    unlink(test_config);
    printf("✓ Test passed\n");
}

void test_parse_config_invalid(FastRG_t *fastrg_ccb)
{
    printf("\nTesting parse_config function for invalid config:\n");
    printf("=========================================\n\n");

    struct fastrg_config cfg = {0};
    U16 saved_user_count = fastrg_ccb->user_count;
    U16 saved_max_user_count = fastrg_ccb->max_user_count;

    const char *test_config = "/tmp/test_fastrg_invalid.conf";
    FILE *fp = fopen(test_config, "w");
    TEST_ASSERT(fp != NULL, "Mock config file", 
        "Create test config file failed");

    fprintf(fp, "MaxUserCount = 0;\n");  /* Legacy field, ignored */
    fprintf(fp, "InitUserCount = 0;\n");  /* Malformed config, should be ignored */
    fclose(fp);

    STATUS ret = parse_config(test_config, fastrg_ccb, &cfg);
    TEST_ASSERT(ret == SUCCESS, "legacy MaxUserCount does not reject config",
        "parse_config returns ERROR for ignored MaxUserCount");
    TEST_ASSERT(fastrg_ccb->max_user_count == saved_max_user_count,
        "ignored MaxUserCount leaves capacity unchanged", "max_user_count changed from %u to %u",
        saved_max_user_count, fastrg_ccb->max_user_count);

    fastrg_ccb->user_count = saved_user_count;
    fastrg_ccb->max_user_count = saved_max_user_count;
    unlink(test_config);

    printf("✓ Test passed\n");
}

void test_compute_max_user_count(FastRG_t *fastrg_ccb)
{
    const uint64_t reserve = 512ULL * 1024ULL * 1024ULL;
    const uint64_t per_subscriber = 175ULL * 1024ULL * 1024ULL;
    U16 saved_max_user_count = fastrg_ccb->max_user_count;

    printf("\nTesting hugepage-based subscriber capacity:\n");
    printf("===========================================\n\n");

    /* Pin the per-subscriber cost so the three assertions below keep testing
     * the arithmetic they were written for, now that the real path measures it. */
    fastrg_set_subscriber_cost_for_test(per_subscriber);

    fastrg_set_hugepage_free_bytes_for_test(0);
    fastrg_compute_max_user_count(fastrg_ccb);
    TEST_ASSERT(fastrg_ccb->max_user_count == MIN_USER_COUNT,
        "tiny hugepage budget clamps to minimum", "max_user_count=%u", fastrg_ccb->max_user_count);

    fastrg_set_hugepage_free_bytes_for_test(reserve + 10 * per_subscriber + per_subscriber - 1);
    fastrg_compute_max_user_count(fastrg_ccb);
    TEST_ASSERT(fastrg_ccb->max_user_count == 10,
        "middle hugepage budget uses integer division", "max_user_count=%u", fastrg_ccb->max_user_count);

    fastrg_set_hugepage_free_bytes_for_test(reserve + ((uint64_t)MAX_USER_COUNT + 1) * per_subscriber);
    fastrg_compute_max_user_count(fastrg_ccb);
    TEST_ASSERT(fastrg_ccb->max_user_count == MAX_USER_COUNT,
        "large hugepage budget clamps to maximum", "max_user_count=%u", fastrg_ccb->max_user_count);

    fastrg_set_subscriber_cost_for_test(0);
    fastrg_ccb->max_user_count = saved_max_user_count;
    printf("✓ Test passed\n");
}

/* The real path: measure a subscriber, hand back everything it borrowed, and
 * size the pools by the count rather than by a rounded-up one. */
static void test_measured_subscriber_capacity(FastRG_t *fastrg_ccb)
{
    U16 saved_max_user_count = fastrg_ccb->max_user_count;
    uint64_t saved_cost = fastrg_ccb->subscriber_cost_bytes;
    struct rte_malloc_socket_stats stats;
    uint64_t before = 0, after = 0;

    printf("\nTesting measured subscriber capacity:\n");
    printf("=====================================\n\n");

    for(int socket_id=0; socket_id<RTE_MAX_NUMA_NODES; socket_id++)
        if (rte_malloc_get_socket_stats(socket_id, &stats) == 0)
            before += stats.heap_freesz_bytes;

    /* No injected cost: this runs the probe for real on the test heap. */
    fastrg_set_subscriber_cost_for_test(0);
    fastrg_set_hugepage_free_bytes_for_test(8ULL * 1024 * 1024 * 1024);
    STATUS ret = fastrg_compute_max_user_count(fastrg_ccb);

    for(int socket_id=0; socket_id<RTE_MAX_NUMA_NODES; socket_id++)
        if (rte_malloc_get_socket_stats(socket_id, &stats) == 0)
            after += stats.heap_freesz_bytes;

    TEST_ASSERT(ret == SUCCESS, "measuring subscriber cost succeeds", NULL);
    /* The inline arrays inside one subscriber already run to megabytes, so a
     * plausible measurement cannot come out under 5 MiB. */
    TEST_ASSERT(fastrg_ccb->subscriber_cost_bytes > 5ULL * 1024 * 1024,
        "probe measures a positive subscriber cost",
        "cost=%" PRIu64, fastrg_ccb->subscriber_cost_bytes);
    TEST_ASSERT(before == after,
        "probe leaves the heap where it found it",
        "before=%" PRIu64 " after=%" PRIu64, before, after);

    fastrg_set_hugepage_free_bytes_for_test(0);
    fastrg_ccb->subscriber_cost_bytes = saved_cost;
    fastrg_ccb->max_user_count = saved_max_user_count;
    printf("✓ Test passed\n");
}

/* The pools hold exactly the number of subscribers the capacity says, not the
 * next power of two: at ~24 MiB an object, rounding up wastes hundreds of MiB. */
static void test_ccb_pools_sized_exactly(FastRG_t *fastrg_ccb)
{
    FastRG_t probe = {0};

    printf("\nTesting ccb mempool sizing:\n");
    printf("===========================\n\n");

    probe.loglvl = (U8)-1;
    probe.user_count = 3;
    probe.max_user_count = 3;   /* not a power of two, so rounding would show */
    probe.vlan_userid_map = calloc(MAX_VLAN_ID, sizeof(*probe.vlan_userid_map));
    TEST_ASSERT(probe.vlan_userid_map != NULL, "vlan map allocated for the sizing probe", NULL);
    if (probe.vlan_userid_map == NULL)
        return;

    /* Tearing a subscriber down reaches into its DHCP block to release the DNS
     * proxy state, so the teardown below needs one to exist per subscriber. */
    probe.dhcp_ccb = calloc(probe.max_user_count, sizeof(*probe.dhcp_ccb));
    TEST_ASSERT(probe.dhcp_ccb != NULL, "dhcp ccb array allocated for the sizing probe", NULL);
    if (probe.dhcp_ccb == NULL) {
        free(probe.vlan_userid_map);
        return;
    }
    for(U16 i=0; i<probe.max_user_count; i++) {
        probe.dhcp_ccb[i] = calloc(1, sizeof(dhcp_ccb_t));
        if (probe.dhcp_ccb[i] == NULL) {
            TEST_ASSERT(0, "dhcp ccb allocated for the sizing probe", "slot=%u", i);
            for(U16 j=0; j<i; j++)
                free(probe.dhcp_ccb[j]);
            free(probe.dhcp_ccb);
            free(probe.vlan_userid_map);
            return;
        }
    }

    TEST_ASSERT(pppd_init(&probe) == SUCCESS, "pppd_init succeeds for the sizing probe", NULL);
    if (probe.ppp_ccb_mp != NULL) {
        TEST_ASSERT(probe.ppp_ccb_mp->size == probe.max_user_count,
            "ccb mempool is sized exactly max_user_count",
            "size=%u max_user_count=%u", probe.ppp_ccb_mp->size, probe.max_user_count);
        TEST_ASSERT(probe.ppp_ccb_mp->elt_size == sizeof(ppp_ccb_t),
            "ccb mempool object size matches the struct",
            "elt_size=%u sizeof=%zu", probe.ppp_ccb_mp->elt_size, sizeof(ppp_ccb_t));
    }

    pppd_cleanup_ccb(&probe);
    for(U16 i=0; i<probe.max_user_count; i++)
        free(probe.dhcp_ccb[i]);
    free(probe.dhcp_ccb);
    free(probe.vlan_userid_map);
    (void)fastrg_ccb;
    printf("✓ Test passed\n");
}

/* The per-subscriber charge has to cover what the pool really takes, checked
 * against DPDK's own answer rather than a second copy of the arithmetic. */
static void test_pool_cost_covers_objects_and_ring(void)
{
    printf("\nTesting mempool cost accounting:\n");
    printf("================================\n\n");

    const uint32_t elt = 256;
    const uint32_t counts[] = {64, 4096, 4097};

    for(unsigned i=0; i<sizeof(counts)/sizeof(counts[0]); i++) {
        uint32_t n = counts[i];
        uint64_t span = 0;
        uint64_t charged;

        TEST_ASSERT(fastrg_get_mempool_span_bytes(elt, n, &span) == SUCCESS,
            "span is answerable", "n=%u", n);
        charged = span + 16ULL * n + fastrg_get_mempool_fixed_usage_bytes(elt);
        struct rte_mempool *mp = rte_mempool_create_empty("fastrg_ut_cost", n, elt,
            0, 0, (int)rte_socket_id(), 0);
        ssize_t actual = -1;

        if (mp != NULL && rte_mempool_set_ops_byname(mp, "ring_mp_mc", NULL) == 0) {
            size_t min_chunk = 0, align = 0, page_size = 0;
            uint32_t pg_shift = 0;

            if (rte_mempool_get_page_size(mp, &page_size) == 0 && page_size > 0)
                pg_shift = (uint32_t)rte_bsf32((uint32_t)page_size);
            actual = rte_mempool_ops_calc_mem_size(mp, n, pg_shift, &min_chunk, &align);
        }
        if (mp != NULL)
            rte_mempool_free(mp);

        TEST_ASSERT(actual >= 0 && charged >= (uint64_t)actual +
                rte_ring_get_memsize(rte_align32pow2(n + 1)),
            "charged cost is an upper bound on what the pool takes",
            "n=%u charged=%" PRIu64 " objs=%zd ring=%" PRIu64,
            n, charged, actual, (uint64_t)rte_ring_get_memsize(rte_align32pow2(n + 1)));
    }
    uint64_t empty_span = 1;

    TEST_ASSERT(fastrg_get_mempool_span_bytes(elt, 0, &empty_span) == SUCCESS &&
        empty_span == 0, "an empty pool spans nothing", NULL);
    printf("✓ Test passed\n");
}

/* One subscriber must be charged a slice of a page, not a whole one; a slab
 * charge is also an upper bound, so the check above cannot catch it. */
static void test_span_shares_pages_across_subscribers(void)
{
    printf("\nTesting mempool page sharing:\n");
    printf("=============================\n\n");

    /* Small enough that several fit in a page under any page size in use. */
    const uint32_t elt = 1000;
    struct rte_mempool *mp = rte_mempool_create_empty("fastrg_ut_share", 1, elt,
        0, 0, (int)rte_socket_id(), 0);
    size_t page_size = 0;
    uint64_t span = 0;
    struct rte_mempool_objsz sz;
    uint64_t objs_per_page;

    if (mp != NULL && rte_mempool_set_ops_byname(mp, "ring_mp_mc", NULL) == 0)
        (void)rte_mempool_get_page_size(mp, &page_size);
    if (mp != NULL)
        rte_mempool_free(mp);

    rte_mempool_calc_obj_size(elt, 0, &sz);
    objs_per_page = page_size > 0 ? (uint64_t)page_size / sz.total_size : 0;

    TEST_ASSERT(objs_per_page >= 2,
        "several objects of this size fit in one page",
        "page_size=%zu total_elt=%u", page_size, sz.total_size);
    TEST_ASSERT(fastrg_get_mempool_span_bytes(elt, 1, &span) == SUCCESS && span < page_size,
        "one subscriber is charged a slice of a page, not a whole one",
        "span=%" PRIu64 " page_size=%zu", span, page_size);
    printf("✓ Test passed\n");
}

/* The reported size info has to describe the pools that actually get created. */
static void test_domain_size_info_mirrors_real_pools(FastRG_t *fastrg_ccb)
{
    ccb_memory_info_t dhcp_size_info = {0};

    printf("\nTesting domain size info mirrors the real pools:\n");
    printf("===========================================\n\n");

    dhcp_get_subscriber_real_size(&dhcp_size_info);
    TEST_ASSERT(dhcp_size_info.n_pools == 2, "dhcp reports both of its pools",
        "n_pools=%d", dhcp_size_info.n_pools);
    TEST_ASSERT(dhcp_size_info.pools[0].elt_size == sizeof(dhcp_ccb_t) &&
            dhcp_size_info.pools[0].objs_per_sub == 1,
        "dhcp ccb pool entry matches the real pool",
        "elt=%u objs=%u", dhcp_size_info.pools[0].elt_size, dhcp_size_info.pools[0].objs_per_sub);
    TEST_ASSERT(dhcp_size_info.pools[1].elt_size == sizeof(dhcp_ccb_per_lan_user_t),
        "dhcp lease pool entry matches the real pool",
        "elt=%u objs=%u", dhcp_size_info.pools[1].elt_size, dhcp_size_info.pools[1].objs_per_sub);
    /* The lease block dominates every other term, so its pointer array must be
     * charged too. */
    TEST_ASSERT(dhcp_size_info.plain_bytes_per_sub >=
            (uint64_t)dhcp_size_info.pools[1].objs_per_sub * sizeof(dhcp_ccb_per_lan_user_t *),
        "dhcp plain bytes cover the lease pointer array",
        "plain=%" PRIu64, dhcp_size_info.plain_bytes_per_sub);

    /* Build the real pools and compare against them, rather than against a
     * second copy of the constants the reported size info was built from. */
    FastRG_t dhcp_probe = {0};

    dhcp_probe.loglvl = (U8)-1;
    dhcp_probe.max_user_count = 3;  /* not a power of two, so rounding would show */
    TEST_ASSERT(dhcp_init(&dhcp_probe) == SUCCESS,
        "dhcp_init succeeds for the sizing probe", NULL);
    if (dhcp_probe.dhcp_ccb_mp != NULL) {
        struct rte_mempool *per_lan_pool =
            DHCPD_GET_CCB(&dhcp_probe, 0)->dhcp_per_lan_user_mempool;

        TEST_ASSERT(dhcp_probe.dhcp_ccb_mp->size == dhcp_probe.max_user_count,
            "dhcp ccb mempool is sized exactly max_user_count",
            "size=%u max_user_count=%u", dhcp_probe.dhcp_ccb_mp->size,
            dhcp_probe.max_user_count);
        TEST_ASSERT(dhcp_size_info.pools[0].elt_size == dhcp_probe.dhcp_ccb_mp->elt_size,
            "dhcp ccb entry matches the pool that gets created",
            "info=%u pool=%u", dhcp_size_info.pools[0].elt_size,
            dhcp_probe.dhcp_ccb_mp->elt_size);
        TEST_ASSERT(per_lan_pool != NULL, "lease pool exists on the probe ccb", NULL);
        if (per_lan_pool != NULL) {
            TEST_ASSERT((uint64_t)dhcp_size_info.pools[1].objs_per_sub *
                    dhcp_probe.max_user_count == per_lan_pool->size,
                "lease count per subscriber matches the pool that gets created",
                "info=%u x %u pool=%u", dhcp_size_info.pools[1].objs_per_sub,
                dhcp_probe.max_user_count, per_lan_pool->size);
            TEST_ASSERT(dhcp_size_info.pools[1].elt_size == per_lan_pool->elt_size,
                "lease entry size matches the pool that gets created",
                "info=%u pool=%u", dhcp_size_info.pools[1].elt_size,
                per_lan_pool->elt_size);
        }
    }
    dhcpd_cleanup_ccb(&dhcp_probe);

    ccb_memory_info_t ppp_size_info = {0};
    STATUS ret = pppd_get_subscriber_real_size(fastrg_ccb, &ppp_size_info);

    TEST_ASSERT(ret == SUCCESS, "pppd reports its size info", NULL);
    if (ret == SUCCESS) {
        TEST_ASSERT(ppp_size_info.n_pools == 1 &&
                ppp_size_info.pools[0].elt_size == sizeof(ppp_ccb_t) &&
                ppp_size_info.pools[0].objs_per_sub == 1,
            "ppp ccb pool entry matches the real pool",
            "n_pools=%d elt=%u objs=%u", ppp_size_info.n_pools,
            ppp_size_info.pools[0].elt_size, ppp_size_info.pools[0].objs_per_sub);
        TEST_ASSERT(ppp_size_info.plain_bytes_per_sub > 5ULL * 1024 * 1024,
            "ppp plain bytes carry the measured element cost",
            "plain=%" PRIu64, ppp_size_info.plain_bytes_per_sub);
    }
    printf("✓ Test passed\n");
}

void test_config(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║            Configuration Module Unit Tests                ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    test_parse_config_valid(fastrg_ccb);
    test_parse_config_invalid(fastrg_ccb);
    test_compute_max_user_count(fastrg_ccb);
    test_measured_subscriber_capacity(fastrg_ccb);
    test_pool_cost_covers_objects_and_ring();
    test_span_shares_pages_across_subscribers();
    test_domain_size_info_mirrors_real_pools(fastrg_ccb);
    test_ccb_pools_sized_exactly(fastrg_ccb);

    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║  Test Summary                                              ║\n");
    printf("╠════════════════════════════════════════════════════════════╣\n");
    printf("║  Total Tests:  %3d                                         ║\n", test_count);
    printf("║  Passed:       %3d                                         ║\n", pass_count);
    printf("║  Failed:       %3d                                         ║\n", test_count - pass_count);
    printf("║  Success Rate: %3d%%                                        ║\n", 
           test_count > 0 ? (pass_count * 100 / test_count) : 0);
    printf("╚════════════════════════════════════════════════════════════╝\n");

    *total_tests += test_count;
    *total_pass += pass_count;
}
