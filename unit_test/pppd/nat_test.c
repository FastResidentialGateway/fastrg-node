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
    /* The test thread is reader 0 and holds no entry pointers here; report
     * quiescent so nat_table_reset() can drain leftover deferred frees from
     * the previous test (its drain loop would otherwise wait on us). */
    rte_rcu_qsbr_quiescent(test_rcu, 0);
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
    /* Wind back past the 1s coalescing threshold so the refresh must fire */
    nat_expire_set(e1->expire_slot,
        __atomic_load_n(e1->expire_slot, __ATOMIC_RELAXED) - 2 * fastrg_get_cycles_in_sec());
    U64 before = __atomic_load_n(e1->expire_slot, __ATOMIC_RELAXED);
    U16 p2 = nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip, src_port, dst_port, &e2);

    TEST_ASSERT(p1 == p2, "same flow gets the same nat_port", NULL);
    TEST_ASSERT(e1 == e2, "same flow maps to the same entry", NULL);
    TEST_ASSERT(__atomic_load_n(e2->expire_slot, __ATOMIC_RELAXED) > before,
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
    nat_expire_set(learned->expire_slot, 1); /* long past */

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
        nat_expire_set(entries[i]->expire_slot, 1); /* zombie: expired, never queried */
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

static void test_gc_tick_drains_deferred_slots(void)
{
    printf("\nTesting GC tick drains deferred slots after quiescence:\n");
    printf("=======================================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    addr_table_t *entry = NULL;

    nat_learning_port_reuse(&test_ccb, &eth,
        rte_cpu_to_be_32(0xC0A80110), rte_cpu_to_be_32(0x08080808),
        rte_cpu_to_be_16(57100), rte_cpu_to_be_16(443), &entry);
    nat_expire_set(entry->expire_slot, 1);

    TEST_ASSERT(nat_gc_scan_by_ccb(&test_ccb, MAX_NAT_ENTRIES) == 1,
        "first GC tick unlinks the expired mapping", NULL);
    TEST_ASSERT(rte_ring_count(test_ccb.nat_free_ring) == MAX_NAT_ENTRIES - 1,
        "slot stays deferred until the reader is quiescent", NULL);

    rte_rcu_qsbr_quiescent(test_rcu, 0);
    nat_gc_scan_by_ccb(&test_ccb, NAT_GC_SCAN_CHUNK);
    TEST_ASSERT(rte_ring_count(test_ccb.nat_free_ring) == MAX_NAT_ENTRIES,
        "next GC tick returns the quiescent slot to the free ring", NULL);
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

static void test_evict_clears_forward_key(void)
{
    printf("\nTesting evict unlinks the forward key too:\n");
    printf("==========================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 dst_ip = rte_cpu_to_be_32(0x08080808);
    U16 dst_port = rte_cpu_to_be_16(443);
    U32 src1 = rte_cpu_to_be_32(0xC0A8006E);
    U16 sp1 = rte_cpu_to_be_16(59000);
    addr_table_t *e1 = NULL, *e2 = NULL, *e3 = NULL;

    U16 p1 = nat_learning_port_reuse(&test_ccb, &eth, src1, dst_ip, sp1, dst_port, &e1);
    nat_expire_set(e1->expire_slot, 1); /* A expires */

    /* B collides with A's (nat_port, dst) key → eviction path, B takes A's
     * port.  A single source IP only covers ~64% of the port range with its
     * 65535 hash draws, so search across several source IPs — deterministic
     * (pure hash of constants), practically guaranteed to find one. */
    U32 src2 = 0;
    U16 sp2 = 0;
    for (U32 ip_off = 0; ip_off < 64 && sp2 == 0; ip_off++) {
        U32 cand = rte_cpu_to_be_32(0xC0A80200 + ip_off);
        for (U32 p = 1; p < TOTAL_SOCK_PORT; p++) {
            if (compute_initial_nat_port(cand, rte_cpu_to_be_16((U16)p)) == p1) {
                src2 = cand;
                sp2 = rte_cpu_to_be_16((U16)p);
                break;
            }
        }
    }
    TEST_ASSERT(sp2 != 0, "found colliding source for evict test", NULL);

    U16 p2 = nat_learning_port_reuse(&test_ccb, &eth, src2, dst_ip, sp2, dst_port, &e2);
    TEST_ASSERT(p2 == p1, "expired entry evicted, its port taken over", NULL);

    /* A's flow returns.  If eviction had leaked A's forward key, the fast
     * path would revive A's old entry and A would believe it still owns p1
     * — while inbound p1 now belongs to B (mis-bound NAT).  Correct dual
     * delete forces a forward miss → conflict walk → fresh port. */
    U16 p3 = nat_learning_port_reuse(&test_ccb, &eth, src1, dst_ip, sp1, dst_port, &e3);
    TEST_ASSERT(p3 != 0 && p3 != p1,
        "returning flow re-learns on a fresh port, not the stolen one",
        "p1=%u p3=%u", rte_be_to_cpu_16(p1), rte_be_to_cpu_16(p3));
    TEST_ASSERT(e3->src_ip == src1 && e3->src_port == sp1,
        "fresh entry belongs to the returning flow", NULL);

    addr_table_t *rb = nat_reverse_lookup(p1, dst_ip, dst_port, &test_ccb);
    TEST_ASSERT(rb != NULL && rb->src_ip == src2,
        "inbound on the contested port reaches B only", NULL);
    addr_table_t *ra = nat_reverse_lookup(p3, dst_ip, dst_port, &test_ccb);
    TEST_ASSERT(ra != NULL && ra->src_ip == src1,
        "inbound on the fresh port reaches A", NULL);
}

static void test_gc_clears_forward_key(void)
{
    printf("\nTesting GC unlinks the forward key too:\n");
    printf("=======================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 src_ip = rte_cpu_to_be_32(0xC0A80070), dst_ip = rte_cpu_to_be_32(0x08080808);
    U16 src_port = rte_cpu_to_be_16(60000), dst_port = rte_cpu_to_be_16(443);
    addr_table_t *e1 = NULL, *e2 = NULL;

    U16 p1 = nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip,
        src_port, dst_port, &e1);
    nat_expire_set(e1->expire_slot, 1); /* zombie */
    TEST_ASSERT(nat_gc_scan_by_ccb(&test_ccb, MAX_NAT_ENTRIES) == 1,
        "gc reclaimed the zombie", NULL);

    /* Relearn the same 5-tuple before RCU reclaim (harshest window).  A
     * leaked forward key would fast-path into the dead slot and skip
     * re-inserting the reverse key — inbound would blackhole.  Correct
     * dual delete forces a full re-learn that restores reverse reachability. */
    U16 p2 = nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip,
        src_port, dst_port, &e2);
    TEST_ASSERT(p2 != 0, "flow re-learns after gc", NULL);
    addr_table_t *rv = nat_reverse_lookup(p2, dst_ip, dst_port, &test_ccb);
    TEST_ASSERT(rv == e2 && rv->src_ip == src_ip,
        "re-learned mapping reachable from the WAN side again", NULL);

    nat_quiesce_reclaim();
    TEST_ASSERT(rte_ring_count(test_ccb.nat_free_ring) == MAX_NAT_ENTRIES - 1,
        "zombie slot recycled, exactly the live flow's slot in use", NULL);
}

static void test_expire_refresh_coalescing(void)
{
    printf("\nTesting NAT-level refresh write-coalescing:\n");
    printf("===========================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 src_ip = rte_cpu_to_be_32(0xC0A80071), dst_ip = rte_cpu_to_be_32(0x08080808);
    U16 src_port = rte_cpu_to_be_16(61000), dst_port = rte_cpu_to_be_16(443);
    addr_table_t *e1 = NULL;

    nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip, src_port, dst_port, &e1);
    U64 stored = __atomic_load_n(e1->expire_slot, __ATOMIC_RELAXED);

    /* Immediate re-learning: deadline moved by microseconds only → the
     * coalescing guard must skip the store */
    nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip, src_port, dst_port, NULL);
    TEST_ASSERT(__atomic_load_n(e1->expire_slot, __ATOMIC_RELAXED) == stored,
        "same-flow refresh within threshold coalesces (no store)", NULL);

    /* Drift the deadline back 2s → refresh must fire */
    nat_expire_set(e1->expire_slot, stored - 2 * fastrg_get_cycles_in_sec());
    nat_learning_port_reuse(&test_ccb, &eth, src_ip, dst_ip, src_port, dst_port, NULL);
    TEST_ASSERT(__atomic_load_n(e1->expire_slot, __ATOMIC_RELAXED) > stored - fastrg_get_cycles_in_sec(),
        "stale deadline refreshed past the threshold", NULL);
}

static void test_stats_counters(void)
{
    printf("\nTesting NAT health counters:\n");
    printf("============================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};

    TEST_ASSERT(test_ccb.nat_enospc == 0 && test_ccb.nat_gc_reclaimed == 0,
        "counters zeroed by reset", NULL);

    /* Every port reserved → learning fails and accounts one ENOSPC */
    for (U32 p = 0; p < PORT_FWD_TABLE_SIZE; p++)
        rte_atomic16_set(&test_ccb.port_fwd_table[p].is_active, 1);
    U16 nat_port = nat_learning_port_reuse(&test_ccb, &eth,
        rte_cpu_to_be_32(0xC0A80072), rte_cpu_to_be_32(0x08080808),
        rte_cpu_to_be_16(62000), rte_cpu_to_be_16(443), NULL);
    TEST_ASSERT(nat_port == 0 && test_ccb.nat_enospc == 1,
        "failed learning increments nat_enospc",
        "enospc=%" PRIu64, test_ccb.nat_enospc);
    memset(test_ccb.port_fwd_table, 0, sizeof(test_ccb.port_fwd_table));

    /* One zombie reclaimed by GC → accounted */
    addr_table_t *e1 = NULL;
    nat_learning_port_reuse(&test_ccb, &eth,
        rte_cpu_to_be_32(0xC0A80072), rte_cpu_to_be_32(0x08080808),
        rte_cpu_to_be_16(62000), rte_cpu_to_be_16(443), &e1);
    nat_expire_set(e1->expire_slot, 1);
    nat_gc_scan_by_ccb(&test_ccb, MAX_NAT_ENTRIES);
    TEST_ASSERT(test_ccb.nat_gc_reclaimed == 1,
        "gc reclaim increments nat_gc_reclaimed",
        "reclaimed=%" PRIu64, test_ccb.nat_gc_reclaimed);
}

static void test_pool_dry_enospc(void)
{
    printf("\nTesting pool-dry enospc (free ring exhausted):\n");
    printf("==============================================\n\n");

    nat_env_reset();
    struct rte_ether_hdr eth = {0};
    U32 dst_ip = rte_cpu_to_be_32(0x08080808);
    U16 dst_port = rte_cpu_to_be_16(443);
    addr_table_t *ea = NULL;

    /* One real flow A, then expire it — gives the emergency GC inside
     * nat_slot_alloc something to find. */
    U16 pa = nat_learning_port_reuse(&test_ccb, &eth,
        rte_cpu_to_be_32(0xC0A80080), dst_ip, rte_cpu_to_be_16(40100), dst_port, &ea);
    TEST_ASSERT(pa != 0, "flow A learned", NULL);
    nat_expire_set(ea->expire_slot, 1);

    /* Simulate exhaustion by live flows: drain every remaining free slot.
     * (Consuming them via 262k real learns proves nothing extra — per-learn
     * slot consumption is already asserted in test_learning_basic.) */
    void *obj;
    U32 drained = 0;
    while (rte_ring_dequeue(test_ccb.nat_free_ring, &obj) == 0)
        drained++;
    TEST_ASSERT(drained == MAX_NAT_ENTRIES - 1,
        "drained all remaining free slots", "drained=%u", drained);

    /* New flow B: slot alloc fails, emergency GC fires (deletes A's keys)
     * but the slot can only come back through the RCU defer queue — which
     * this thread hasn't quiesced through yet — so learning must fail and
     * account one enospc. */
    U64 enospc_before = test_ccb.nat_enospc;
    U16 pb = nat_learning_port_reuse(&test_ccb, &eth,
        rte_cpu_to_be_32(0xC0A80081), dst_ip, rte_cpu_to_be_16(40101), dst_port, NULL);
    TEST_ASSERT(pb == 0, "learning fails when the pool is dry", NULL);
    TEST_ASSERT(test_ccb.nat_enospc == enospc_before + 1,
        "pool-dry failure increments nat_enospc",
        "enospc=%" PRIu64, test_ccb.nat_enospc);
    TEST_ASSERT(test_ccb.nat_gc_reclaimed >= 1,
        "emergency GC ran and reclaimed the expired flow's keys", NULL);
    TEST_ASSERT(nat_reverse_lookup(pa, dst_ip, dst_port, &test_ccb) == NULL,
        "expired flow A unlinked by the emergency GC", NULL);

    /* Recovery: once readers quiesce and the defer queue drains, A's slot
     * returns and the same flow B learns successfully. */
    nat_quiesce_reclaim();
    TEST_ASSERT(rte_ring_count(test_ccb.nat_free_ring) == 1,
        "A's slot back in the free ring after RCU reclaim", NULL);
    pb = nat_learning_port_reuse(&test_ccb, &eth,
        rte_cpu_to_be_32(0xC0A80081), dst_ip, rte_cpu_to_be_16(40101), dst_port, NULL);
    TEST_ASSERT(pb != 0, "flow B learns after a slot is reclaimed", NULL);
    TEST_ASSERT(test_ccb.nat_enospc == enospc_before + 1,
        "successful retry adds no enospc", NULL);
}

/* Small hand-built fixture for the hash-ENOSPC paths.  The real hashes
 * (262144 entries, cuckoo, multi-writer) have no deterministic fill-to-fail
 * point, so build tiny single-writer hashes (key slots = entries exactly)
 * around the same nat_learning_port_reuse code path.  No RCU attached:
 * slot recycling isn't under test here, key-add failure handling is. */
static ppp_ccb_t small_ccb;
#define SMALL_POOL_SLOTS 64

static STATUS small_env_init(U32 reverse_entries, U32 forward_entries)
{
    static int small_gen; /* unique DPDK object names per call */
    char name[RTE_RING_NAMESIZE];

    memset(&small_ccb, 0, sizeof(small_ccb));
    rte_spinlock_init(&small_ccb.nat_insert_lock);
    small_gen++;

    struct rte_hash_parameters params = {
        .key_len = sizeof(nat_reverse_key_t),
        .hash_func = rte_hash_crc,
        .hash_func_init_val = 0,
        .socket_id = (int)rte_socket_id(),
    };
    snprintf(name, sizeof(name), "nat_small_rev_%d", small_gen);
    params.name = name;
    params.entries = reverse_entries;
    small_ccb.nat_reverse_hash = rte_hash_create(&params);

    snprintf(name, sizeof(name), "nat_small_fwd_%d", small_gen);
    params.name = name;
    params.entries = forward_entries;
    params.key_len = sizeof(nat_forward_key_t);
    small_ccb.nat_forward_hash = rte_hash_create(&params);

    snprintf(name, sizeof(name), "nat_small_free_%d", small_gen);
    small_ccb.nat_free_ring = rte_ring_create(name, SMALL_POOL_SLOTS,
        (int)rte_socket_id(), RING_F_EXACT_SZ);

    if (small_ccb.nat_reverse_hash == NULL || small_ccb.nat_forward_hash == NULL ||
        small_ccb.nat_free_ring == NULL)
        return ERROR;

    for (U32 i = 0; i < SMALL_POOL_SLOTS; i++) {
        small_ccb.addr_table[i].expire_slot = &small_ccb.nat_expire_at[i];
        rte_ring_enqueue(small_ccb.nat_free_ring, (void *)(uintptr_t)i);
    }
    return SUCCESS;
}

static void small_env_destroy(void)
{
    nat_table_destroy(&small_ccb);
}

/* Fill the small fixture with n distinct-destination flows (same source →
 * same nat_port reused, one pool slot + one key in each hash per flow). */
static U32 small_fill_flows(struct rte_ether_hdr *eth, U32 n)
{
    U32 ok = 0;
    for (U32 i = 0; i < n; i++) {
        if (nat_learning_port_reuse(&small_ccb, eth,
                rte_cpu_to_be_32(0x0A000001), rte_cpu_to_be_32(0x08080000 + i),
                rte_cpu_to_be_16(40200), rte_cpu_to_be_16(443), NULL) != 0)
            ok++;
    }
    return ok;
}

static void test_reverse_hash_full_enospc(void)
{
    printf("\nTesting reverse-hash-full enospc:\n");
    printf("=================================\n\n");

    struct rte_ether_hdr eth = {0};
    TEST_ASSERT(small_env_init(8, SMALL_POOL_SLOTS) == SUCCESS,
        "small fixture created (reverse=8)", NULL);

    TEST_ASSERT(small_fill_flows(&eth, 8) == 8,
        "8 flows fill the reverse hash exactly", NULL);
    U32 ring_before = rte_ring_count(small_ccb.nat_free_ring);

    /* 9th flow: every candidate port's reverse add hits ENOSPC; the
     * never-published slot must go straight back to the ring each time,
     * and after the full port wrap the flow fails with one enospc. */
    U16 p = nat_learning_port_reuse(&small_ccb, &eth,
        rte_cpu_to_be_32(0x0A000001), rte_cpu_to_be_32(0x08080000 + 8),
        rte_cpu_to_be_16(40200), rte_cpu_to_be_16(443), NULL);
    TEST_ASSERT(p == 0, "learning fails when the reverse hash is full", NULL);
    TEST_ASSERT(small_ccb.nat_enospc == 1,
        "reverse-hash-full failure increments nat_enospc",
        "enospc=%" PRIu64, small_ccb.nat_enospc);
    TEST_ASSERT(rte_ring_count(small_ccb.nat_free_ring) == ring_before,
        "no slot leaked across the failed port walk",
        "before=%u after=%u", ring_before, rte_ring_count(small_ccb.nat_free_ring));
    TEST_ASSERT(rte_hash_count(small_ccb.nat_forward_hash) == 8,
        "no forward-key pollution from the failed flow",
        "count=%u", rte_hash_count(small_ccb.nat_forward_hash));

    small_env_destroy();
}

static void test_forward_hash_full_enospc(void)
{
    printf("\nTesting forward-hash-full enospc:\n");
    printf("=================================\n\n");

    struct rte_ether_hdr eth = {0};
    TEST_ASSERT(small_env_init(1024, 8) == SUCCESS,
        "small fixture created (forward=8)", NULL);

    TEST_ASSERT(small_fill_flows(&eth, 8) == 8,
        "8 flows fill the forward hash exactly", NULL);
    U32 ring_before = rte_ring_count(small_ccb.nat_free_ring);

    /* 9th flow: reverse add succeeds (room left), forward add hits ENOSPC →
     * the reverse key must be unpublished and the flow fails immediately
     * (a different port wouldn't help — the forward key has no port in it). */
    U16 p = nat_learning_port_reuse(&small_ccb, &eth,
        rte_cpu_to_be_32(0x0A000001), rte_cpu_to_be_32(0x08080000 + 8),
        rte_cpu_to_be_16(40200), rte_cpu_to_be_16(443), NULL);
    TEST_ASSERT(p == 0, "learning fails when the forward hash is full", NULL);
    TEST_ASSERT(small_ccb.nat_enospc == 1,
        "forward-hash-full failure increments nat_enospc",
        "enospc=%" PRIu64, small_ccb.nat_enospc);
    TEST_ASSERT(rte_hash_count(small_ccb.nat_reverse_hash) == 8,
        "reverse key of the failed flow was unpublished",
        "count=%u", rte_hash_count(small_ccb.nat_reverse_hash));
    /* Without the reverse key, the WAN side must not reach the dead slot */
    U16 pa = compute_initial_nat_port(rte_cpu_to_be_32(0x0A000001), rte_cpu_to_be_16(40200));
    TEST_ASSERT(nat_reverse_lookup(pa, rte_cpu_to_be_32(0x08080000 + 8),
        rte_cpu_to_be_16(443), &small_ccb) == NULL,
        "failed flow unreachable from the WAN side", NULL);
    /* The slot travels through deferred reclaim in production (RCU dq owns
     * it after the reverse key was published) — it must NOT be pushed
     * straight back to the free ring. */
    TEST_ASSERT(rte_ring_count(small_ccb.nat_free_ring) == ring_before - 1,
        "slot of the failed flow goes to deferred reclaim, not the ring",
        "before=%u after=%u", ring_before, rte_ring_count(small_ccb.nat_free_ring));

    small_env_destroy();
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
    test_gc_tick_drains_deferred_slots();
    test_udp_tcp_icmp_wrappers();
    test_evict_clears_forward_key();
    test_gc_clears_forward_key();
    test_expire_refresh_coalescing();
    test_stats_counters();
    test_pool_dry_enospc();
    test_reverse_hash_full_enospc();
    test_forward_hash_full_enospc();

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
