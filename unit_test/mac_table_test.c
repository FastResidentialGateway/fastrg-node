#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include <common.h>

#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include <rte_arp.h>

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

static void test_ip_to_mac_idx(void)
{
    printf("\nTesting ip_to_mac_idx:\n");
    printf("=========================================\n\n");

    U32 idx = 0;
    /* 10.1.2.3 → B=1 C=2 D=3 → 1*255*255 + 2*255 + 3 */
    TEST_ASSERT(ip_to_mac_idx(htonl(0x0A010203), &idx) == SUCCESS,
        "10.1.2.3 maps successfully", "");
    TEST_ASSERT(idx == (U32)(1 * 255 * 255 + 2 * 255 + 3),
        "10.1.2.3 index formula", "expected %u, got %u", 1 * 255 * 255 + 2 * 255 + 3, idx);

    /* boundary: octets 254 are the last valid values */
    TEST_ASSERT(ip_to_mac_idx(htonl(0x0AFEFEFE), &idx) == SUCCESS,
        "10.254.254.254 maps successfully", "");
    TEST_ASSERT(idx == MAC_TABLE_SIZE - 1,
        "10.254.254.254 is the last index", "expected %u, got %u", MAC_TABLE_SIZE - 1, idx);
    TEST_ASSERT(ip_to_mac_idx(htonl(0x0A000000), &idx) == SUCCESS && idx == 0,
        "10.0.0.0 is index 0", "got %u", idx);

    /* octet 255 anywhere in B/C/D is rejected */
    TEST_ASSERT(ip_to_mac_idx(htonl(0x0AFF0001), &idx) == ERROR,
        "octet B=255 rejected", "");
    TEST_ASSERT(ip_to_mac_idx(htonl(0x0A00FF01), &idx) == ERROR,
        "octet C=255 rejected", "");
    TEST_ASSERT(ip_to_mac_idx(htonl(0x0A0000FF), &idx) == ERROR,
        "octet D=255 rejected (broadcast)", "");

    /* first octet is the /8 network part — not range-checked */
    TEST_ASSERT(ip_to_mac_idx(htonl(0xFF010203), &idx) == SUCCESS,
        "first octet 255 is allowed (network part)", "");
}

static void test_mac_learn_lookup(void)
{
    printf("\nTesting mac_table_learn / mac_table_lookup:\n");
    printf("=========================================\n\n");

    mac_table_entry_t *table = mac_table_alloc();
    TEST_ASSERT(table != NULL, "mac_table_alloc returns a table", "");
    if (table == NULL)
        return;

    U32 ip = htonl(0x0A010203);
    TEST_ASSERT(mac_table_lookup(table, ip) == NULL,
        "fresh table has no entry (zero-initialised)", "");
    TEST_ASSERT(mac_table_lookup(NULL, ip) == NULL,
        "lookup on NULL table returns NULL", "");

    mac_table_learn(NULL, ip, &mac_a); /* must not crash */
    mac_table_learn(table, ip, &mac_a);
    mac_table_entry_t *e = mac_table_lookup(table, ip);
    TEST_ASSERT(e != NULL, "learned entry found", "");
    TEST_ASSERT(e != NULL && rte_is_same_ether_addr(&e->mac, &mac_a),
        "learned MAC matches", "");

    /* neighbor indices untouched */
    TEST_ASSERT(mac_table_lookup(table, htonl(0x0A010204)) == NULL,
        "adjacent IP not learned", "");
    TEST_ASSERT(mac_table_lookup(table, htonl(0x0A010202)) == NULL,
        "other adjacent IP not learned", "");

    /* re-learn same MAC (PR #69 skip-write path) — entry stays valid */
    mac_table_learn(table, ip, &mac_a);
    e = mac_table_lookup(table, ip);
    TEST_ASSERT(e != NULL && rte_is_same_ether_addr(&e->mac, &mac_a),
        "re-learn with same MAC keeps entry intact", "");

    /* re-learn different MAC (host moved) — entry updated */
    mac_table_learn(table, ip, &mac_b);
    e = mac_table_lookup(table, ip);
    TEST_ASSERT(e != NULL && rte_is_same_ether_addr(&e->mac, &mac_b),
        "re-learn with new MAC updates entry", "");

    /* out-of-range IP: learn is a no-op, lookup returns NULL */
    mac_table_learn(table, htonl(0x0A0000FF), &mac_a);
    TEST_ASSERT(mac_table_lookup(table, htonl(0x0A0000FF)) == NULL,
        "broadcast-octet IP never learned or found", "");

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

    TEST_ASSERT(arp_pending_enqueue(NULL, &q, NULL, ip_a) == ERROR,
        "enqueue rejects NULL mempool", "");
    TEST_ASSERT(arp_pending_enqueue(mp, NULL, NULL, ip_a) == ERROR,
        "enqueue rejects NULL queue", "");

    /* queue 2 packets waiting on ip_a, 1 on ip_b */
    struct rte_mbuf *m1 = make_test_mbuf(), *m2 = make_test_mbuf(), *m3 = make_test_mbuf();
    TEST_ASSERT(m1 != NULL && m2 != NULL && m3 != NULL, "test mbufs allocated", "");
    TEST_ASSERT(arp_pending_enqueue(mp, &q, m1, ip_a) == SUCCESS &&
                arp_pending_enqueue(mp, &q, m2, ip_a) == SUCCESS &&
                arp_pending_enqueue(mp, &q, m3, ip_b) == SUCCESS,
        "three packets enqueued", "");
    TEST_ASSERT(rte_ring_count(q.ring) == 3, "ring holds 3 entries",
        "got %u", rte_ring_count(q.ring));

    /* resolve ip_a → both its packets drain with dst MAC filled; ip_b stays */
    struct rte_mbuf *tx_pkts[8] = {0};
    U16 tx_count = 0;
    arp_pending_drain(mp, &q, ip_a, &mac_a, tx_pkts, &tx_count, 8);
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
    arp_pending_enqueue(mp, &q, m4, ip_a);
    arp_pending_enqueue(mp, &q, m5, ip_a);
    tx_count = 0;
    memset(tx_pkts, 0, sizeof(tx_pkts));
    arp_pending_drain(mp, &q, ip_a, &mac_a, tx_pkts, &tx_count, 1);
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
        if (m == NULL || arp_pending_enqueue(mp, &q, m, htonl(0x0A010000 + i)) != SUCCESS)
            fill_ok = FALSE;
    }
    TEST_ASSERT(fill_ok == TRUE, "ring filled to capacity", "cap=%u", cap);
    TEST_ASSERT(rte_ring_count(q.ring) == cap, "ring at capacity",
        "got %u", rte_ring_count(q.ring));

    struct rte_mbuf *m_new = make_test_mbuf();
    TEST_ASSERT(arp_pending_enqueue(mp, &q, m_new, ip_b) == SUCCESS,
        "enqueue into full ring still succeeds (drop-oldest)", "");
    TEST_ASSERT(rte_ring_count(q.ring) == cap, "ring still at capacity",
        "got %u", rte_ring_count(q.ring));
    /* oldest entry (ip 10.1.0.0) was dropped: draining it yields nothing */
    tx_count = 0;
    arp_pending_drain(mp, &q, htonl(0x0A010000), &mac_a, tx_pkts, &tx_count, 8);
    TEST_ASSERT(tx_count == 0, "oldest entry was dropped on overflow",
        "got %u", tx_count);
    /* the newest entry survived */
    tx_count = 0;
    arp_pending_drain(mp, &q, ip_b, &mac_b, tx_pkts, &tx_count, 8);
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

    test_ip_to_mac_idx();
    test_mac_learn_lookup();
    test_arp_pending_queue();
    test_encode_arp_request(fastrg_ccb);

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
