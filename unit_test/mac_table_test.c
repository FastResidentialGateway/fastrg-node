#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include <common.h>

#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include <rte_arp.h>
#include <rte_ethdev.h>

#include "../src/mac_table.h"
#include "../src/protocol.h"
#include "../src/fastrg.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

static struct rte_ether_addr mac_a = {.addr_bytes = {0x02, 0x00, 0x00, 0x00, 0x00, 0xAA}};
static struct rte_ether_addr mac_b = {.addr_bytes = {0x02, 0x00, 0x00, 0x00, 0x00, 0xBB}};

/* Dedicated mbuf pool — direct_pool[LAN_PORT] may already be a small pool
 * created by an earlier suite, so don't share it. */
static struct rte_mempool *mac_test_pool;
static U16 mock_tx_result;
static struct rte_mbuf *mock_tx_mbuf;

static U16 mock_tx_burst(void *txq, struct rte_mbuf **tx_pkts, U16 nb_pkts)
{
    (void)txq;

    if (nb_pkts > 0)
        mock_tx_mbuf = tx_pkts[0];
    return mock_tx_result;
}

/* Allocate an mbuf with an all-zero ether header, so arp_pending_drain has
 * a dst_addr field to write into. */
static struct rte_mbuf *make_test_mbuf(void)
{
    struct rte_mbuf *m = rte_pktmbuf_alloc(mac_test_pool);
    if (m == NULL)
        return NULL;
    char *p = rte_pktmbuf_append(m, sizeof(struct rte_ether_hdr));
    if (p == NULL) {
        rte_pktmbuf_free(m);
        return NULL;
    }
    memset(p, 0, sizeof(struct rte_ether_hdr));
    return m;
}

static void test_mac_learn_lookup(void)
{
    printf("\nTesting mac_table_learn / mac_table_lookup:\n");
    printf("=========================================\n\n");

    mac_table_t *table = mac_table_alloc(0);
    TEST_ASSERT(table != NULL, "mac_table_alloc returns a table", "");
    if (table == NULL)
        return;

    U32 ip = htonl(0x0A010203);
    struct rte_ether_addr out;
    TEST_ASSERT(mac_table_lookup(table, ip, &out) == ERROR,
        "fresh table has no entry", "");
    TEST_ASSERT(mac_table_lookup(NULL, ip, &out) == ERROR,
        "lookup on NULL table returns ERROR", "");

    mac_table_learn(NULL, ip, &mac_a); /* must not crash */
    mac_table_learn(table, ip, &mac_a);
    TEST_ASSERT(mac_table_lookup(table, ip, &out) == SUCCESS,
        "learned entry found", "");
    TEST_ASSERT(rte_is_same_ether_addr(&out, &mac_a),
        "learned MAC matches", "");

    /* neighbor IPs untouched */
    TEST_ASSERT(mac_table_lookup(table, htonl(0x0A010204), &out) == ERROR,
        "adjacent IP not learned", "");
    TEST_ASSERT(mac_table_lookup(table, htonl(0x0A010202), &out) == ERROR,
        "other adjacent IP not learned", "");

    /* re-learn same MAC (skip-write fast path) — entry stays valid */
    mac_table_learn(table, ip, &mac_a);
    TEST_ASSERT(mac_table_lookup(table, ip, &out) == SUCCESS &&
        rte_is_same_ether_addr(&out, &mac_a),
        "re-learn with same MAC keeps entry intact", "");

    /* re-learn different MAC (host moved) — atomic data-word overwrite */
    mac_table_learn(table, ip, &mac_b);
    TEST_ASSERT(mac_table_lookup(table, ip, &out) == SUCCESS &&
        rte_is_same_ether_addr(&out, &mac_b),
        "re-learn with new MAC updates entry", "");

    mac_table_free(table);
    mac_table_free(NULL); /* must not crash */
}

static void test_arp_pending_queue(void)
{
    printf("\nTesting arp_pending enqueue / drain / flush:\n");
    printf("=========================================\n\n");

    struct rte_mempool *mp = NULL;
    arp_pending_queue_t q = {0};

    TEST_ASSERT(arp_pending_init_pool(NULL) == ERROR, "init_pool rejects NULL", "");
    TEST_ASSERT(arp_pending_init_pool(&mp) == SUCCESS && mp != NULL,
        "arp_pending_init_pool succeeds", "");
    TEST_ASSERT(arp_pending_init_queue(NULL, 0) == ERROR, "init_queue rejects NULL", "");
    TEST_ASSERT(arp_pending_init_queue(&q, 0) == SUCCESS && q.ring != NULL,
        "arp_pending_init_queue succeeds", "");

    U32 ip_a = htonl(0x0A010203);
    U32 ip_b = htonl(0x0A010204);

    TEST_ASSERT(arp_pending_enqueue(NULL, &q, NULL, ip_a, 0) == ERROR,
        "enqueue rejects NULL mempool", "");
    TEST_ASSERT(arp_pending_enqueue(mp, NULL, NULL, ip_a, 0) == ERROR,
        "enqueue rejects NULL queue", "");

    /* queue 2 packets waiting on ip_a, 1 on ip_b */
    struct rte_mbuf *m1 = make_test_mbuf(), *m2 = make_test_mbuf(), *m3 = make_test_mbuf();
    TEST_ASSERT(m1 != NULL && m2 != NULL && m3 != NULL, "test mbufs allocated", "");
    TEST_ASSERT(arp_pending_enqueue(mp, &q, m1, ip_a, 0) == SUCCESS &&
                arp_pending_enqueue(mp, &q, m2, ip_a, 0) == SUCCESS &&
                arp_pending_enqueue(mp, &q, m3, ip_b, 0) == SUCCESS,
        "three packets enqueued", "");
    TEST_ASSERT(rte_ring_count(q.ring) == 3, "ring holds 3 entries",
        "got %u", rte_ring_count(q.ring));

    /* resolve ip_a → both its packets drain with dst MAC filled; ip_b stays */
    struct rte_mbuf *tx_pkts[8] = {0};
    U16 tx_queues[8] = {0};
    U16 tx_count = 0;
    arp_pending_drain(mp, &q, ip_a, &mac_a, tx_pkts, tx_queues, &tx_count, 8);
    TEST_ASSERT(tx_count == 2, "drain releases both ip_a packets",
        "expected 2, got %u", tx_count);
    BOOL macs_ok = TRUE;
    for (U16 i = 0; i < tx_count; i++) {
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(tx_pkts[i], struct rte_ether_hdr *);
        if (!rte_is_same_ether_addr(&eth->dst_addr, &mac_a))
            macs_ok = FALSE;
        rte_pktmbuf_free(tx_pkts[i]);
    }
    TEST_ASSERT(macs_ok == TRUE, "drained packets got resolved dst MAC", "");
    TEST_ASSERT(rte_ring_count(q.ring) == 1, "unmatched ip_b entry re-enqueued",
        "got %u", rte_ring_count(q.ring));

    /* tx_max cap: matching entries beyond the cap are re-enqueued, not lost */
    struct rte_mbuf *m4 = make_test_mbuf(), *m5 = make_test_mbuf();
    arp_pending_enqueue(mp, &q, m4, ip_a, 0);
    arp_pending_enqueue(mp, &q, m5, ip_a, 0);
    tx_count = 0;
    memset(tx_pkts, 0, sizeof(tx_pkts));
    arp_pending_drain(mp, &q, ip_a, &mac_a, tx_pkts, tx_queues, &tx_count, 1);
    TEST_ASSERT(tx_count == 1, "drain honors tx_max cap", "got %u", tx_count);
    TEST_ASSERT(rte_ring_count(q.ring) == 2,
        "capped match re-enqueued alongside ip_b entry", "got %u", rte_ring_count(q.ring));
    rte_pktmbuf_free(tx_pkts[0]);

    /* flush drops everything and returns pool slots */
    unsigned avail_before_fill = rte_mempool_avail_count(mp);
    arp_pending_flush(mp, &q);
    TEST_ASSERT(rte_ring_count(q.ring) == 0, "flush empties the ring",
        "got %u", rte_ring_count(q.ring));
    TEST_ASSERT(rte_mempool_avail_count(mp) == avail_before_fill + 2,
        "flush returns mempool slots", "avail %u vs %u",
        rte_mempool_avail_count(mp), avail_before_fill + 2);
    arp_pending_flush(mp, &q); /* empty flush is a no-op */
    arp_pending_flush(mp, NULL); /* NULL queue must not crash */

    /* ring-full behavior: oldest entry is dropped, newest wins */
    unsigned cap = rte_ring_get_capacity(q.ring);
    BOOL fill_ok = TRUE;
    for (unsigned i = 0; i < cap; i++) {
        struct rte_mbuf *m = make_test_mbuf();
        if (m == NULL || arp_pending_enqueue(mp, &q, m, htonl(0x0A010000 + i), 0) != SUCCESS)
            fill_ok = FALSE;
    }
    TEST_ASSERT(fill_ok == TRUE, "ring filled to capacity", "cap=%u", cap);
    TEST_ASSERT(rte_ring_count(q.ring) == cap, "ring at capacity",
        "got %u", rte_ring_count(q.ring));

    struct rte_mbuf *m_new = make_test_mbuf();
    TEST_ASSERT(arp_pending_enqueue(mp, &q, m_new, ip_b, 0) == SUCCESS,
        "enqueue into full ring still succeeds (drop-oldest)", "");
    TEST_ASSERT(rte_ring_count(q.ring) == cap, "ring still at capacity",
        "got %u", rte_ring_count(q.ring));
    /* oldest entry (ip 10.1.0.0) was dropped: draining it yields nothing */
    tx_count = 0;
    arp_pending_drain(mp, &q, htonl(0x0A010000), &mac_a, tx_pkts, tx_queues, &tx_count, 8);
    TEST_ASSERT(tx_count == 0, "oldest entry was dropped on overflow",
        "got %u", tx_count);
    /* the newest entry survived */
    tx_count = 0;
    arp_pending_drain(mp, &q, ip_b, &mac_b, tx_pkts, tx_queues, &tx_count, 8);
    TEST_ASSERT(tx_count == 1, "newest entry survived overflow", "got %u", tx_count);
    rte_pktmbuf_free(tx_pkts[0]);

    /* cleanup_queue flushes remaining entries and frees the ring */
    arp_pending_cleanup_queue(&q, mp);
    TEST_ASSERT(q.ring == NULL, "cleanup_queue frees the ring", "");
    TEST_ASSERT(rte_mempool_in_use_count(mp) == 0,
        "all mempool slots returned after cleanup", "in_use=%u", rte_mempool_in_use_count(mp));

    arp_pending_cleanup_pool(&mp);
    TEST_ASSERT(mp == NULL, "cleanup_pool nulls the pointer", "");
    arp_pending_cleanup_pool(&mp); /* double cleanup is a no-op */
    arp_pending_cleanup_pool(NULL);
}

static void test_encode_arp_request(FastRG_t *fastrg_ccb)
{
    printf("\nTesting encode_arp_request:\n");
    printf("=========================================\n\n");

    U8 buf[64];
    memset(buf, 0xEE, sizeof(buf)); /* poison to catch over-writes */
    U32 src_ip    = htonl(0x0A010101);
    U32 target_ip = htonl(0x0A010203);
    const struct rte_ether_addr *src_mac = &fastrg_ccb->nic_info.hsi_lan_mac;

    U16 len = encode_arp_request(buf, src_mac, src_ip, target_ip, 3);
    /* eth(14) + vlan(4) + arp(28) = 46 */
    TEST_ASSERT(len == 46, "frame length is 46 (eth+vlan+arp)", "got %u", len);

    U8 bcast[RTE_ETHER_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT(memcmp(buf, bcast, RTE_ETHER_ADDR_LEN) == 0,
        "dst MAC is broadcast", "");
    TEST_ASSERT(memcmp(buf + 6, src_mac->addr_bytes, RTE_ETHER_ADDR_LEN) == 0,
        "src MAC copied", "");
    TEST_ASSERT(*(U16 *)(buf + 12) == htons(VLAN),
        "ether_type is 802.1Q", "got 0x%04x", ntohs(*(U16 *)(buf + 12)));
    TEST_ASSERT(*(U16 *)(buf + 14) == htons(0x0003),
        "vlan TCI carries vlan 3", "got 0x%04x", ntohs(*(U16 *)(buf + 14)));
    TEST_ASSERT(*(U16 *)(buf + 16) == htons(FRAME_TYPE_ARP),
        "vlan next_proto is ARP", "got 0x%04x", ntohs(*(U16 *)(buf + 16)));

    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(buf + 18);
    TEST_ASSERT(arp->arp_hardware == htons(RTE_ARP_HRD_ETHER),
        "hardware type is ethernet", "got 0x%04x", ntohs(arp->arp_hardware));
    TEST_ASSERT(arp->arp_protocol == htons(FRAME_TYPE_IP),
        "protocol type is IPv4", "got 0x%04x", ntohs(arp->arp_protocol));
    TEST_ASSERT(arp->arp_hlen == RTE_ETHER_ADDR_LEN && arp->arp_plen == 4,
        "hlen/plen are 6/4", "got %u/%u", arp->arp_hlen, arp->arp_plen);
    TEST_ASSERT(arp->arp_opcode == htons(RTE_ARP_OP_REQUEST),
        "opcode is REQUEST", "got 0x%04x", ntohs(arp->arp_opcode));
    TEST_ASSERT(memcmp(arp->arp_data.arp_sha.addr_bytes, src_mac->addr_bytes,
        RTE_ETHER_ADDR_LEN) == 0, "sender MAC is ours", "");
    TEST_ASSERT(arp->arp_data.arp_sip == src_ip, "sender IP is gateway IP", "");
    U8 zero_mac[RTE_ETHER_ADDR_LEN] = {0};
    TEST_ASSERT(memcmp(arp->arp_data.arp_tha.addr_bytes, zero_mac,
        RTE_ETHER_ADDR_LEN) == 0, "target MAC is zeroed", "");
    TEST_ASSERT(arp->arp_data.arp_tip == target_ip, "target IP is resolve target", "");
    TEST_ASSERT(buf[46] == 0xEE, "encoder writes exactly 46 bytes", "");
}

static void test_send_arp_request_tx_result(FastRG_t *fastrg_ccb)
{
    printf("\nTesting send_arp_request TX result handling:\n");
    printf("=========================================\n\n");

    struct rte_eth_fp_ops saved_ops = rte_eth_fp_ops[LAN_PORT];
    struct rte_mempool *saved_pool = direct_pool[LAN_PORT];
    void *tx_queue_data[] = {(void *)1};
    RTE_ATOMIC(void *) tx_callbacks[] = {NULL};

    direct_pool[LAN_PORT] = mac_test_pool;
    /* Unit tests never init a real port, so the rte_eth_tx_burst inline
     * wrapper would dereference NULL per-queue tables — hook a fake tx
     * queue handle and an empty callback slot before swapping in the mock. */
    rte_eth_fp_ops[LAN_PORT].txq.data = tx_queue_data;
    rte_eth_fp_ops[LAN_PORT].txq.clbk = tx_callbacks;
    rte_eth_fp_ops[LAN_PORT].tx_pkt_burst = mock_tx_burst;

    unsigned avail_before = rte_mempool_avail_count(mac_test_pool);
    mock_tx_result = 0;
    mock_tx_mbuf = NULL;
    STATUS status = send_arp_request(&fastrg_ccb->nic_info.hsi_lan_mac,
        htonl(0x0A010101), htonl(0x0A010203), 3, 0, direct_pool[LAN_PORT]);
    TEST_ASSERT(status == ERROR, "TX rejection returns ERROR", "got %d", status);
    TEST_ASSERT(mock_tx_mbuf != NULL, "TX rejection path reaches tx_burst", "");
    TEST_ASSERT(rte_mempool_avail_count(mac_test_pool) == avail_before,
        "TX rejection frees the unsent mbuf", "avail %u vs %u",
        rte_mempool_avail_count(mac_test_pool), avail_before);

    mock_tx_result = 1;
    mock_tx_mbuf = NULL;
    status = send_arp_request(&fastrg_ccb->nic_info.hsi_lan_mac,
        htonl(0x0A010101), htonl(0x0A010203), 3, 0, direct_pool[LAN_PORT]);
    TEST_ASSERT(status == SUCCESS, "successful TX returns SUCCESS", "got %d", status);
    TEST_ASSERT(mock_tx_mbuf != NULL, "successful TX hands mbuf to tx_burst", "");
    if (mock_tx_mbuf != NULL)
        rte_pktmbuf_free(mock_tx_mbuf);
    TEST_ASSERT(rte_mempool_avail_count(mac_test_pool) == avail_before,
        "successful TX test returns captured mbuf", "avail %u vs %u",
        rte_mempool_avail_count(mac_test_pool), avail_before);

    rte_eth_fp_ops[LAN_PORT] = saved_ops;
    direct_pool[LAN_PORT] = saved_pool;
}


/* ---- fixed-max refactor cases (moved from the task-scoped file into the
 * module test file per repo naming convention; additive only) ---- */

static void test_mac_table_hash_generation_reset(void)
{
    printf("\nTesting mac_table generation reset:\n");
    printf("=========================================\n\n");

    mac_table_t *t = mac_table_alloc(1);
    TEST_ASSERT(t != NULL, "mac_table_alloc returns a table handle", "");
    if (t == NULL)
        return;

    U32 ip1 = htonl(0x0A010203), ip2 = htonl(0x0A010204);
    struct rte_ether_addr out;

    mac_table_learn(t, ip1, &mac_a);
    mac_table_learn(t, ip2, &mac_b);

    mac_table_reset(t);
    TEST_ASSERT(mac_table_lookup(t, ip1, &out) == ERROR,
        "reset invalidates learned entry (generation bump)", "");
    TEST_ASSERT(mac_table_lookup(t, ip2, &out) == ERROR,
        "reset invalidates every learned entry", "");

    /* stale keys are excluded from iterate */
    U32 iter = 0, iter_ip = 0;
    TEST_ASSERT(mac_table_iterate(t, &iter, &iter_ip, &out) < 0,
        "iterate skips stale-generation entries after reset", "");

    /* re-learn after reset revalidates per key */
    mac_table_learn(t, ip1, &mac_b);
    TEST_ASSERT(mac_table_lookup(t, ip1, &out) == SUCCESS &&
        rte_is_same_ether_addr(&out, &mac_b),
        "re-learn after reset revalidates the key", "");
    TEST_ASSERT(mac_table_lookup(t, ip2, &out) == ERROR,
        "un-relearned key stays invalid after reset", "");

    iter = 0;
    int found = 0;
    while (mac_table_iterate(t, &iter, &iter_ip, &out) >= 0)
        found++;
    TEST_ASSERT(found == 1 && iter_ip == ip1,
        "iterate returns exactly the current-generation entry", "found %d", found);

    mac_table_reset(NULL); /* must not crash */
    mac_table_free(t);
}

static void test_mac_table_hash_capacity(void)
{
    printf("\nTesting mac_table 64K capacity-full behavior:\n");
    printf("=========================================\n\n");

    mac_table_t *t = mac_table_alloc(2);
    TEST_ASSERT(t != NULL, "mac_table_alloc returns a table handle", "");
    if (t == NULL)
        return;

    /* Fill with 64K distinct IPs (10.0.0.0 + i). rte_hash guarantees the
     * configured entry count, so all must succeed. */
    for(U32 i=0; i<MAC_TABLE_MAX_ENTRIES; i++) {
        U32 ip = rte_cpu_to_be_32(0x0A000000u + i);
        mac_table_learn(t, ip, &mac_a);
    }
    U64 fails_at_capacity = __atomic_load_n(&t->learn_fail, __ATOMIC_RELAXED);
    TEST_ASSERT(fails_at_capacity == 0,
        "all 64K entries fit without learn failures", "learn_fail=%lu",
        (unsigned long)fails_at_capacity);

    struct rte_ether_addr out;
    TEST_ASSERT(mac_table_lookup(t, rte_cpu_to_be_32(0x0A000000u), &out) == SUCCESS,
        "first entry still present at full capacity", "");
    TEST_ASSERT(mac_table_lookup(t,
        rte_cpu_to_be_32(0x0A000000u + MAC_TABLE_MAX_ENTRIES - 1), &out) == SUCCESS,
        "last entry present at full capacity", "");

    /* One more distinct IP must be dropped (counted, no crash), and an
     * overwrite of an existing key must still work at full capacity. */
    mac_table_learn(t, rte_cpu_to_be_32(0x0B000000u), &mac_b);
    TEST_ASSERT(__atomic_load_n(&t->learn_fail, __ATOMIC_RELAXED) > 0,
        "learn beyond capacity is dropped and counted", "");
    mac_table_learn(t, rte_cpu_to_be_32(0x0A000000u), &mac_b);
    TEST_ASSERT(mac_table_lookup(t, rte_cpu_to_be_32(0x0A000000u), &out) == SUCCESS &&
        rte_is_same_ether_addr(&out, &mac_b),
        "existing-key overwrite still works at full capacity", "");

    mac_table_free(t);
}

void test_mac_table(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           MAC Table Unit Tests                             ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    if (mac_test_pool == NULL)
        mac_test_pool = rte_pktmbuf_pool_create("mac_tbl_pool", 256, 0, 0,
            RTE_MBUF_DEFAULT_BUF_SIZE, (int)rte_socket_id());

    test_mac_learn_lookup();
    test_mac_table_hash_generation_reset();
    test_mac_table_hash_capacity();
    test_arp_pending_queue();
    test_encode_arp_request(fastrg_ccb);
    test_send_arp_request_tx_result(fastrg_ccb);

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
