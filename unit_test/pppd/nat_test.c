#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <common.h>

#include <rte_atomic.h>
#include <rte_rcu_qsbr.h>

#include "../../src/fastrg.h"
#include "../../src/pppd/nat.h"
#include "../../src/protocol.h"
#include "../test_helper.h"

// Global test counters
static int test_count = 0;
static int pass_count = 0;

/* Mock subscriber ccb: owns the NAT pool, reverse hash and free-list.
 * Static (BSS) — ~20 MB, fine for the test binary. */
static ppp_ccb_t test_ccb;
static struct rte_rcu_qsbr *test_rcu;

/* One-time environment: QSBR var with this thread registered as reader 0,
 * then the per-subscriber hash + ring (requires rte_eal_init in test.c). */
static void nat_env_init_once(void)
{
    if (test_rcu != NULL)
        return;
    size_t sz = rte_rcu_qsbr_get_memsize(1);
    test_rcu = calloc(1, sz);
    assert(test_rcu != NULL && rte_rcu_qsbr_init(test_rcu, 1) == 0);
    rte_rcu_qsbr_thread_register(test_rcu, 0);
    rte_rcu_qsbr_thread_online(test_rcu, 0);

    memset(&test_ccb, 0, sizeof(test_ccb));
    rte_spinlock_init(&test_ccb.nat_insert_lock);
    assert(nat_table_init(&test_ccb, 999, test_rcu) == SUCCESS);
}

static void nat_env_reset(void)
{
    nat_env_init_once();
    nat_table_reset(&test_ccb);
    memset(test_ccb.port_fwd_table, 0, sizeof(test_ccb.port_fwd_table));
}

/* Report reader-0 quiescent and drain the RCU defer queue so deleted keys'
 * slots actually return to the free ring (what the data plane gets from
 * per-burst quiescent reporting). */
static void nat_quiesce_reclaim(void)
{
    unsigned int freed = 0, pending = 0, avail = 0;
    rte_rcu_qsbr_quiescent(test_rcu, 0);
    rte_hash_rcu_qsbr_dq_reclaim(test_ccb.nat_reverse_hash, &freed, &pending, &avail);
}

static void test_compute_initial_nat_port(void)
{
    printf("\nTesting compute_initial_nat_port:\n");
    printf("=================================\n\n");

    U32 src_ip = rte_cpu_to_be_32(0xC0A80001);
    U16 src_port = rte_cpu_to_be_16(12345);

    U16 p1 = compute_initial_nat_port(src_ip, src_port);
    U16 p2 = compute_initial_nat_port(src_ip, src_port);
    TEST_ASSERT(p1 == p2, "deterministic for same (src_ip, src_port)", NULL);

    U16 h = rte_be_to_cpu_16(p1);
    TEST_ASSERT(h >= SYS_MAX_PORT && h < TOTAL_SOCK_PORT,
        "port in [SYS_MAX_PORT, TOTAL_SOCK_PORT)",
        "got %u", h);
}

static void test_learning_basic(void)
{
    printf("\nTesting nat_learning_port_reuse basic:\n");
    printf("======================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 src_ip = rte_cpu_to_be_32(0xC0A80064), dst_ip = rte_cpu_to_be_32(0x08080808);
    U16 src_port = rte_cpu_to_be_16(40000), dst_port = rte_cpu_to_be_16(443);
    addr_table_t *entry = NULL;

    U16 nat_port = nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip,
        src_port, dst_port, &entry);
    TEST_ASSERT(nat_port != 0, "learning returns a nat_port", NULL);
    TEST_ASSERT(entry != NULL, "learning returns the entry via out param", NULL);
    TEST_ASSERT(entry->src_ip == src_ip && entry->src_port == src_port &&
        entry->dst_ip == dst_ip && entry->dst_port == dst_port &&
        entry->nat_port == nat_port,
        "entry fields match the learned flow", NULL);
    TEST_ASSERT(rte_atomic16_read(&entry->is_fill) == NAT_ENTRY_READY,
        "entry is READY after learning", NULL);
    TEST_ASSERT(rte_ring_count(test_ccb.nat_free_ring) == MAX_NAT_ENTRIES - 1,
        "exactly one pool slot consumed", NULL);
}

static void test_learning_same_flow_refresh(void)
{
    printf("\nTesting same-flow refresh:\n");
    printf("==========================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 src_ip = rte_cpu_to_be_32(0xC0A80065), dst_ip = rte_cpu_to_be_32(0x01010101);
    U16 src_port = rte_cpu_to_be_16(50000), dst_port = rte_cpu_to_be_16(80);
    addr_table_t *e1 = NULL, *e2 = NULL;

    U16 p1 = nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip, src_port, dst_port, &e1);
    rte_atomic64_set(&e1->expire_at, (S64)(rte_atomic64_read(&e1->expire_at) - 1000));
    U64 before = (U64)rte_atomic64_read(&e1->expire_at);
    U16 p2 = nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip, src_port, dst_port, &e2);

    TEST_ASSERT(p1 == p2, "same flow gets the same nat_port", NULL);
    TEST_ASSERT(e1 == e2, "same flow maps to the same entry", NULL);
    TEST_ASSERT((U64)rte_atomic64_read(&e2->expire_at) > before,
        "expire_at refreshed on same-flow hit", NULL);
    TEST_ASSERT(rte_ring_count(test_ccb.nat_free_ring) == MAX_NAT_ENTRIES - 1,
        "no extra slot consumed on refresh", NULL);
}

static void test_learning_port_reuse_different_dst(void)
{
    printf("\nTesting true port reuse (same port, different dst):\n");
    printf("===================================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 src_ip = rte_cpu_to_be_32(0xC0A80066);
    U16 src_port = rte_cpu_to_be_16(51000);
    addr_table_t *ea = NULL, *eb = NULL;

    /* Same source → same initial candidate port; different destinations →
     * different rev keys → both flows keep the SAME nat_port. */
    U16 pa = nat_learning_port_reuse(&test_ccb, &eth, src_ip,
        rte_cpu_to_be_32(0x08080808), src_port, rte_cpu_to_be_16(443), &ea);
    U16 pb = nat_learning_port_reuse(&test_ccb, &eth, src_ip,
        rte_cpu_to_be_32(0x08080404), src_port, rte_cpu_to_be_16(443), &eb);

    TEST_ASSERT(pa != 0 && pb != 0, "both learnings succeed", NULL);
    TEST_ASSERT(pa == pb, "same nat_port reused toward different destinations", NULL);
    TEST_ASSERT(ea != eb, "distinct entries for the two flows", NULL);
}

static void test_learning_conflict(void)
{
    printf("\nTesting conflict (same key, different source):\n");
    printf("==============================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 dst_ip = rte_cpu_to_be_32(0x08080808);
    U16 dst_port = rte_cpu_to_be_16(443);
    U32 src1 = rte_cpu_to_be_32(0xC0A80067);
    U16 sp1 = rte_cpu_to_be_16(52000);
    addr_table_t *e1 = NULL, *e2 = NULL;

    U16 p1 = nat_learning_port_reuse(&test_ccb, &eth, src1, dst_ip, sp1, dst_port, &e1);

    /* Find a different source that hashes to the same initial candidate port
     * → forces the (nat_port, dst) conflict path. */
    U32 src2 = rte_cpu_to_be_32(0xC0A80068);
    U16 sp2 = 0;
    for (U32 p = 1; p < TOTAL_SOCK_PORT; p++) {
        if (compute_initial_nat_port(src2, rte_cpu_to_be_16((U16)p)) == p1) {
            sp2 = rte_cpu_to_be_16((U16)p);
            break;
        }
    }
    TEST_ASSERT(sp2 != 0, "found colliding source port for conflict test", NULL);

    U16 p2 = nat_learning_port_reuse(&test_ccb, &eth, src2, dst_ip, sp2, dst_port, &e2);
    TEST_ASSERT(p2 != 0 && p2 != p1,
        "conflicting source gets a different nat_port",
        "p1=%u p2=%u", rte_be_to_cpu_16(p1), rte_be_to_cpu_16(p2));
    TEST_ASSERT(e1->src_ip == src1 && e2->src_ip == src2,
        "both flows keep their own entries", NULL);
}

static void test_reverse_lookup(void)
{
    printf("\nTesting nat_reverse_lookup:\n");
    printf("===========================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 src_ip = rte_cpu_to_be_32(0xC0A80069), dst_ip = rte_cpu_to_be_32(0x08080808);
    U16 src_port = rte_cpu_to_be_16(53000), dst_port = rte_cpu_to_be_16(443);
    addr_table_t *learned = NULL;

    U16 nat_port = nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip,
        src_port, dst_port, &learned);

    /* Hit: WAN reply (dst_port of pkt = nat_port, remote = dst of flow) */
    addr_table_t *hit = nat_reverse_lookup(nat_port, dst_ip, dst_port, &test_ccb);
    TEST_ASSERT(hit == learned, "reverse lookup returns the learned entry", NULL);
    TEST_ASSERT(hit->src_ip == src_ip && hit->src_port == src_port,
        "reverse entry holds original LAN source", NULL);

    /* Miss: unknown key returns NULL (no table walk anymore) */
    addr_table_t *miss = nat_reverse_lookup(rte_cpu_to_be_16(1234),
        rte_cpu_to_be_32(0x0A0A0A0A), rte_cpu_to_be_16(9999), &test_ccb);
    TEST_ASSERT(miss == NULL, "unknown key misses with NULL", NULL);
}

static void test_reverse_expired_is_miss(void)
{
    printf("\nTesting expired mapping is a reverse miss:\n");
    printf("==========================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 src_ip = rte_cpu_to_be_32(0xC0A8006A), dst_ip = rte_cpu_to_be_32(0x08080808);
    U16 src_port = rte_cpu_to_be_16(54000), dst_port = rte_cpu_to_be_16(443);
    addr_table_t *learned = NULL;

    U16 nat_port = nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip,
        src_port, dst_port, &learned);
    rte_atomic64_set(&learned->expire_at, 1); /* long past */

    addr_table_t *hit = nat_reverse_lookup(nat_port, dst_ip, dst_port, &test_ccb);
    TEST_ASSERT(hit == NULL, "expired mapping treated as miss (WAN untrusted)", NULL);

    /* Key was deleted; after a grace period the slot returns to the pool */
    nat_quiesce_reclaim();
    TEST_ASSERT(rte_ring_count(test_ccb.nat_free_ring) == MAX_NAT_ENTRIES,
        "slot recycled to free ring after RCU reclaim", NULL);
    TEST_ASSERT(rte_atomic16_read(&learned->is_fill) == NAT_ENTRY_FREE,
        "entry marked FREE by reclaim callback", NULL);
}

static void test_port_fwd_reserved_skipped(void)
{
    printf("\nTesting port-forward reserved port is skipped:\n");
    printf("==============================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 src_ip = rte_cpu_to_be_32(0xC0A8006B), dst_ip = rte_cpu_to_be_32(0x08080808);
    U16 src_port = rte_cpu_to_be_16(55000), dst_port = rte_cpu_to_be_16(443);

    U16 initial = compute_initial_nat_port(src_ip, src_port);
    port_fwd_add(test_ccb.port_fwd_table, rte_be_to_cpu_16(initial),
        rte_cpu_to_be_32(0xC0A80002), 8080);

    addr_table_t *entry = NULL;
    U16 nat_port = nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip,
        src_port, dst_port, &entry);
    TEST_ASSERT(nat_port != 0 && nat_port != initial,
        "reserved candidate skipped, next port allocated", NULL);

    TEST_ASSERT(port_fwd_remove(test_ccb.port_fwd_table,
        rte_be_to_cpu_16(initial)) == SUCCESS, "port_fwd_remove", NULL);
}

static void test_all_ports_reserved_returns_zero(void)
{
    printf("\nTesting all NAT ports reserved → learning fails fast:\n");
    printf("=====================================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    for (U32 p = 0; p < PORT_FWD_TABLE_SIZE; p++)
        rte_atomic16_set(&test_ccb.port_fwd_table[p].is_active, 1);

    U16 nat_port = nat_learning_port_reuse(&test_ccb, &eth,
        rte_cpu_to_be_32(0xC0A8006C), rte_cpu_to_be_32(0x08080808),
        rte_cpu_to_be_16(56000), rte_cpu_to_be_16(443), NULL);
    TEST_ASSERT(nat_port == 0, "learning returns 0 when every port is reserved", NULL);
}

static void test_gc_scan_reclaims_zombies(void)
{
    printf("\nTesting nat_gc_scan_by_ccb reclaims expired zombies:\n");
    printf("====================================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 dst_ip = rte_cpu_to_be_32(0x08080808);
    U16 dst_port = rte_cpu_to_be_16(443);
    addr_table_t *entries[5] = {0};
    U16 ports[5];

    for (int i = 0; i < 5; i++) {
        ports[i] = nat_learning_port_reuse(&test_ccb, &eth,
            rte_cpu_to_be_32(0xC0A80100 + i), dst_ip,
            rte_cpu_to_be_16(57000 + i), dst_port, &entries[i]);
        rte_atomic64_set(&entries[i]->expire_at, 1); /* zombie: expired, never queried */
    }

    U32 reclaimed = nat_gc_scan_by_ccb(&test_ccb, MAX_NAT_ENTRIES);
    TEST_ASSERT(reclaimed == 5, "gc scan deleted all 5 expired keys",
        "reclaimed=%u", reclaimed);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT(nat_reverse_lookup(ports[i], dst_ip, dst_port, &test_ccb) == NULL,
            "zombie no longer reachable via reverse lookup", NULL);
    }
    nat_quiesce_reclaim();
    TEST_ASSERT(rte_ring_count(test_ccb.nat_free_ring) == MAX_NAT_ENTRIES,
        "all zombie slots back in the free ring", NULL);
}

static void test_udp_tcp_icmp_wrappers(void)
{
    printf("\nTesting protocol learning wrappers:\n");
    printf("===================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    struct rte_ipv4_hdr ip = {0};
    ip.version_ihl = 0x45;
    ip.src_addr = rte_cpu_to_be_32(0xC0A8006D);
    ip.dst_addr = rte_cpu_to_be_32(0x08080808);
    ip.total_length = rte_cpu_to_be_16(40);

    struct rte_udp_hdr udp = {0};
    udp.src_port = rte_cpu_to_be_16(58000);
    udp.dst_port = rte_cpu_to_be_16(53);
    U16 up = nat_udp_learning(&test_ccb, &eth, &ip, &udp);
    TEST_ASSERT(up != 0, "UDP wrapper learns", NULL);
    TEST_ASSERT(nat_reverse_lookup(up, ip.dst_addr, udp.dst_port, &test_ccb) != NULL,
        "UDP mapping reachable via reverse lookup", NULL);

    struct rte_tcp_hdr tcp = {0};
    tcp.src_port = rte_cpu_to_be_16(58001);
    tcp.dst_port = rte_cpu_to_be_16(443);
    tcp.data_off = 0x50;
    tcp.tcp_flags = RTE_TCP_SYN_FLAG;
    U16 tp = nat_tcp_learning(&test_ccb, &eth, &ip, &tcp);
    TEST_ASSERT(tp != 0, "TCP wrapper learns", NULL);
    addr_table_t *te = nat_reverse_lookup(tp, ip.dst_addr, tcp.dst_port, &test_ccb);
    TEST_ASSERT(te != NULL && te->tcp_state == TCP_CONNTRACK_SYN_SENT,
        "TCP conntrack ran on the learned entry (SYN → SYN_SENT)",
        "state=%d", te ? te->tcp_state : -1);

    struct rte_icmp_hdr icmp = {0};
    icmp.icmp_type = 8; /* echo request */
    icmp.icmp_ident = rte_cpu_to_be_16(0xBEEF);
    U16 ipn = nat_icmp_learning(&test_ccb, &eth, &ip, &icmp);
    TEST_ASSERT(ipn != 0, "ICMP wrapper learns", NULL);
}

void test_nat(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║              NAT MODULE UNIT TESTS                       ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_compute_initial_nat_port();
    test_learning_basic();
    test_learning_same_flow_refresh();
    test_learning_port_reuse_different_dst();
    test_learning_conflict();
    test_reverse_lookup();
    test_reverse_expired_is_miss();
    test_port_fwd_reserved_skipped();
    test_all_ports_reserved_returns_zero();
    test_gc_scan_reclaims_zombies();
    test_udp_tcp_icmp_wrappers();

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

    if (pass_count == test_count) {
        printf("\n✓ All tests passed!\n");
    } else {
        printf("\n✗ Some tests failed!\n");
    }

    *total_tests += test_count;
    *total_pass += pass_count;
}
