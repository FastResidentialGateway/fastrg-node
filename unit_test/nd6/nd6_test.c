#include <stdio.h>
#include <string.h>

#include <common.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip6.h>
#include <rte_rcu_qsbr.h>
#include <rte_timer.h>

#include "../../src/nd6/nd6.h"
#include "../../src/protocol.h"
#include "../test_helper.h"

static int test_count = 0;
static int pass_count = 0;
static FastRG_t *g_fastrg_ccb;
static ppp_ccb_t test_ccb;
/* QSBR the neighbor cache defers its key-slot reclaim on. This single-threaded
 * fixture is both the writer and the only registered reader, so it reports its
 * own quiescent states around the aging tests. */
static struct rte_rcu_qsbr *test_rcu;

static const struct rte_ether_addr host_mac = {
    .addr_bytes = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50},
};
static const U8 host_ip[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x00,
    0x02, 0x10, 0x20, 0xff, 0xfe, 0x30, 0x40, 0x50,
};
static const U8 lan_prefix[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x00,
};
static const U8 dns_servers[2][16] = {
    {0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x11},
    {0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x88},
};

typedef struct test_icmp6 {
    U8 type;
    U8 code;
    U16 checksum;
    U32 data;
} __rte_packed test_icmp6_t;

typedef struct test_neighbor {
    test_icmp6_t icmp;
    U8 target[16];
} __rte_packed test_neighbor_t;

static void reset_fixture(void)
{
    rte_timer_stop_sync(&test_ccb.ra_timer);
    nd6_table_reset(test_ccb.nd6_table);
    memset(test_ccb.hsi_ipv6_lan_prefix, 0,
        sizeof(test_ccb.hsi_ipv6_lan_prefix));
    memset(test_ccb.hsi_ipv6_dns, 0, sizeof(test_ccb.hsi_ipv6_dns));
    memcpy(test_ccb.hsi_ipv6_lan_prefix, lan_prefix, sizeof(lan_prefix));
    memcpy(test_ccb.hsi_ipv6_dns, dns_servers, sizeof(dns_servers));
    test_ccb.ipv6_enabled = TRUE;
    test_ccb.ipv6cp_up = TRUE;
    test_ccb.dhcp6_pd_ready = TRUE;
    nd6_test_tx_reset();
}

static U16 build_nd_packet(U8 *packet, U8 type, const U8 src_ip[16],
    const U8 target[16], U8 option_type, U8 option_length)
{
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)packet;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(vlan + 1);
    test_icmp6_t *icmp = (test_icmp6_t *)(ip6 + 1);
    U16 body_len = sizeof(*icmp);

    memset(packet, 0, ND6_PACKET_MAX_LEN);
    rte_ether_addr_copy(&host_mac, &eth->src_addr);
    eth->dst_addr.addr_bytes[0] = 0x33;
    eth->dst_addr.addr_bytes[1] = 0x33;
    eth->dst_addr.addr_bytes[5] = type == ND6_ICMP_RS ? 0x02 : 0x01;
    eth->ether_type = rte_cpu_to_be_16(VLAN);
    vlan->tci_union.tci_value = rte_cpu_to_be_16(321);
    vlan->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IPV6);
    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(6) << 28);
    ip6->proto = IPPROTO_ICMPV6;
    ip6->hop_limits = 255;
    memcpy(&ip6->src_addr, src_ip, 16);
    ((U8 *)&ip6->dst_addr)[0] = 0xff;
    ((U8 *)&ip6->dst_addr)[1] = 0x02;
    ((U8 *)&ip6->dst_addr)[15] = type == ND6_ICMP_RS ? 0x02 : 0x01;
    icmp->type = type;

    if (type == ND6_ICMP_NS || type == ND6_ICMP_NA) {
        test_neighbor_t *neighbor = (test_neighbor_t *)icmp;
        memcpy(neighbor->target, target, 16);
        body_len = sizeof(*neighbor);
    }
    if (option_type != 0) {
        U8 *option = (U8 *)icmp + body_len;
        option[0] = option_type;
        option[1] = option_length;
        memcpy(option + 2, host_mac.addr_bytes, RTE_ETHER_ADDR_LEN);
        body_len += 8;
    }
    ip6->payload_len = rte_cpu_to_be_16(body_len);
    icmp->checksum = rte_ipv6_udptcp_cksum(ip6, icmp);
    return sizeof(*eth) + sizeof(*vlan) + sizeof(*ip6) + body_len;
}

static U16 build_expected_ra(U8 *packet)
{
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)packet;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(vlan + 1);
    U8 *ra = (U8 *)(ip6 + 1);
    U8 *cursor;
    U8 gateway[16];
    U16 payload_len = 16 + 32 + 40 + 8;
    U32 value;

    memset(packet, 0, ND6_PACKET_MAX_LEN);
    eth->dst_addr = (struct rte_ether_addr){{0x33, 0x33, 0, 0, 0, 1}};
    rte_ether_addr_copy(&g_fastrg_ccb->nic_info.hsi_lan_mac,
        &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(VLAN);
    vlan->tci_union.tci_value = rte_cpu_to_be_16(321);
    vlan->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IPV6);
    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(6) << 28);
    ip6->payload_len = rte_cpu_to_be_16(payload_len);
    ip6->proto = IPPROTO_ICMPV6;
    ip6->hop_limits = 255;
    nd6_gateway_link_local(&g_fastrg_ccb->nic_info.hsi_lan_mac, gateway);
    memcpy(&ip6->src_addr, gateway, 16);
    ((U8 *)&ip6->dst_addr)[0] = 0xff;
    ((U8 *)&ip6->dst_addr)[1] = 0x02;
    ((U8 *)&ip6->dst_addr)[15] = 1;

    ra[0] = ND6_ICMP_RA;
    ra[4] = 64;
    *(U16 *)&ra[6] = rte_cpu_to_be_16(1800);
    cursor = ra + 16;
    cursor[0] = ND6_OPT_PREFIX_INFO;
    cursor[1] = 4;
    cursor[2] = 64;
    cursor[3] = 0xc0;
    value = rte_cpu_to_be_32(86400);
    memcpy(cursor + 4, &value, sizeof(value));
    value = rte_cpu_to_be_32(43200);
    memcpy(cursor + 8, &value, sizeof(value));
    memcpy(cursor + 16, lan_prefix, 16);
    cursor += 32;
    cursor[0] = ND6_OPT_RDNSS;
    cursor[1] = 5;
    value = rte_cpu_to_be_32(1800);
    memcpy(cursor + 4, &value, sizeof(value));
    memcpy(cursor + 8, dns_servers, sizeof(dns_servers));
    cursor += 40;
    cursor[0] = ND6_OPT_SLLA;
    cursor[1] = 1;
    memcpy(cursor + 2, g_fastrg_ccb->nic_info.hsi_lan_mac.addr_bytes,
        RTE_ETHER_ADDR_LEN);
    *(U16 *)&ra[2] = rte_ipv6_udptcp_cksum(ip6, ra);
    return sizeof(*eth) + sizeof(*vlan) + sizeof(*ip6) + payload_len;
}

static const U8 *find_ra_option(const U8 *packet, U8 wanted,
    U16 *option_len)
{
    const struct rte_ether_hdr *eth = (const struct rte_ether_hdr *)packet;
    const vlan_header_t *vlan = (const vlan_header_t *)(eth + 1);
    const struct rte_ipv6_hdr *ip6 = (const struct rte_ipv6_hdr *)(vlan + 1);
    const U8 *option = (const U8 *)(ip6 + 1) + 16;
    U16 remaining = rte_be_to_cpu_16(ip6->payload_len) - 16;

    while (remaining >= 2 && option[1] != 0) {
        U16 len = (U16)option[1] * 8;
        if (len > remaining)
            return NULL;
        if (option[0] == wanted) {
            *option_len = len;
            return option;
        }
        option += len;
        remaining -= len;
    }
    return NULL;
}

static void test_ra_bytes(void)
{
    U8 actual[ND6_PACKET_MAX_LEN];
    U8 expected[ND6_PACKET_MAX_LEN];
    U16 actual_len = 0;
    U16 expected_len;
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)actual;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(vlan + 1);

    reset_fixture();
    expected_len = build_expected_ra(expected);
    STATUS status = nd6_build_ra(&test_ccb, actual, &actual_len);
    TEST_ASSERT(status == SUCCESS && actual_len == expected_len &&
            memcmp(actual, expected, actual_len) == 0 &&
            rte_ipv6_udptcp_cksum_verify(ip6, ip6 + 1) == 0,
        "T1 RA full frame bytes", NULL);
}

static void test_rs_to_ra(void)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U8 zero[16] = {0};
    U16 len;
    U32 tx_count;

    reset_fixture();
    len = build_nd_packet(packet, ND6_ICMP_RS, host_ip, zero,
        ND6_OPT_SLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_test_get_last_tx(NULL, &tx_count);
    BOOL valid_replied = tx_count == 1;

    nd6_test_tx_reset();
    ((struct rte_ipv6_hdr *)((vlan_header_t *)
        ((struct rte_ether_hdr *)packet + 1) + 1))->hop_limits = 64;
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_test_get_last_tx(NULL, &tx_count);
    BOOL bad_hop_dropped = tx_count == 0;

    len = build_nd_packet(packet, ND6_ICMP_RS, host_ip, zero,
        ND6_OPT_SLLA, 1);
    packet[len - 1] ^= 1;
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_test_get_last_tx(NULL, &tx_count);
    TEST_ASSERT(valid_replied && bad_hop_dropped && tx_count == 0,
        "T2 RS reply and validation", NULL);
}

static void test_ns_to_na(void)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U8 gateway[16];
    U8 other[16] = {0x20, 1};
    U16 len, tx_len;
    U32 tx_count;

    reset_fixture();
    nd6_gateway_link_local(&g_fastrg_ccb->nic_info.hsi_lan_mac, gateway);
    len = build_nd_packet(packet, ND6_ICMP_NS, host_ip, gateway,
        ND6_OPT_SLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    const U8 *tx = nd6_test_get_last_tx(&tx_len, &tx_count);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)tx;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(vlan + 1);
    test_neighbor_t *na = (test_neighbor_t *)(ip6 + 1);
    BOOL valid_na = tx_count == 1 && tx_len > 0 &&
        vlan->tci_union.tci_value == rte_cpu_to_be_16(321) &&
        na->icmp.type == ND6_ICMP_NA &&
        rte_be_to_cpu_32(na->icmp.data) == UINT32_C(0xe0000000) &&
        ((U8 *)(na + 1))[0] == ND6_OPT_TLLA &&
        memcmp((U8 *)(na + 1) + 2,
            g_fastrg_ccb->nic_info.hsi_lan_mac.addr_bytes, 6) == 0 &&
        rte_ipv6_udptcp_cksum_verify(ip6, na) == 0;

    nd6_test_tx_reset();
    len = build_nd_packet(packet, ND6_ICMP_NS, host_ip, other,
        ND6_OPT_SLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_test_get_last_tx(NULL, &tx_count);
    TEST_ASSERT(valid_na && tx_count == 0,
        "T3 gateway NS to NA only", NULL);
}

static void test_neighbor_learning_and_dad(void)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U8 gateway[16];
    U8 zero[16] = {0};
    U8 dad_target[16] = {0x20, 1, 0x0d, 0xb8, 0, 0, 0, 1};
    struct rte_ether_addr found;
    U16 len;
    U32 tx_count;

    reset_fixture();
    nd6_gateway_link_local(&g_fastrg_ccb->nic_info.hsi_lan_mac, gateway);
    len = build_nd_packet(packet, ND6_ICMP_NS, host_ip, gateway,
        ND6_OPT_SLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    BOOL ns_learned = nd6_table_lookup(test_ccb.nd6_table, host_ip,
        &found) == SUCCESS && rte_is_same_ether_addr(&found, &host_mac);

    nd6_table_reset(test_ccb.nd6_table);
    len = build_nd_packet(packet, ND6_ICMP_NA, host_ip, host_ip,
        ND6_OPT_TLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    BOOL na_learned = nd6_table_lookup(test_ccb.nd6_table, host_ip,
        &found) == SUCCESS && rte_is_same_ether_addr(&found, &host_mac);

    nd6_table_reset(test_ccb.nd6_table);
    nd6_test_tx_reset();
    len = build_nd_packet(packet, ND6_ICMP_NS, zero, dad_target,
        ND6_OPT_SLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_test_get_last_tx(NULL, &tx_count);
    TEST_ASSERT(ns_learned && na_learned && tx_count == 0 &&
            nd6_table_lookup(test_ccb.nd6_table, zero, &found) == ERROR,
        "T4 NS and NA learning; DAD ignored", NULL);
}

static void test_cache_generation(void)
{
    struct rte_ether_addr found;

    reset_fixture();
    nd6_table_learn(test_ccb.nd6_table, host_ip, &host_mac);
    BOOL learned = nd6_table_lookup(test_ccb.nd6_table, host_ip,
        &found) == SUCCESS;
    nd6_table_reset(test_ccb.nd6_table);
    BOOL invalidated = nd6_table_lookup(test_ccb.nd6_table, host_ip,
        &found) == ERROR;
    nd6_table_learn(test_ccb.nd6_table, host_ip, &host_mac);
    BOOL relearned = nd6_table_lookup(test_ccb.nd6_table, host_ip,
        &found) == SUCCESS && rte_is_same_ether_addr(&found, &host_mac);
    TEST_ASSERT(learned && invalidated && relearned,
        "T5 neighbor cache generation reset", NULL);
}

static void test_three_gates(void)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U8 gateway[16];
    struct rte_ether_addr found;
    U16 len;
    U32 tx_count;
    BOOL all_dropped = TRUE;

    reset_fixture();
    nd6_gateway_link_local(&g_fastrg_ccb->nic_info.hsi_lan_mac, gateway);
    len = build_nd_packet(packet, ND6_ICMP_NS, host_ip, gateway,
        ND6_OPT_SLLA, 1);
    for (U8 gate = 0; gate < 3; gate++) {
        reset_fixture();
        if (gate == 0)
            test_ccb.ipv6_enabled = FALSE;
        else if (gate == 1)
            test_ccb.ipv6cp_up = FALSE;
        else
            test_ccb.dhcp6_pd_ready = FALSE;
        nd6_lan_input(g_fastrg_ccb, 0, packet, len);
        nd6_test_get_last_tx(NULL, &tx_count);
        all_dropped = all_dropped && tx_count == 0 &&
            nd6_table_lookup(test_ccb.nd6_table, host_ip, &found) == ERROR;
        rte_timer_reset(&test_ccb.ra_timer, 1000000, PERIODICAL, 0,
            nd6_ra_timer_cb, &test_ccb);
        nd6_ra_timer_cb(&test_ccb.ra_timer, &test_ccb);
        all_dropped = all_dropped && rte_timer_pending(&test_ccb.ra_timer) == 0;
    }
    TEST_ASSERT(all_dropped, "T6 all three readiness gates", NULL);
}

static void test_rdnss_boundaries(void)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U16 packet_len, option_len;

    reset_fixture();
    memset(test_ccb.hsi_ipv6_dns, 0, sizeof(test_ccb.hsi_ipv6_dns));
    nd6_build_ra(&test_ccb, packet, &packet_len);
    BOOL none = find_ra_option(packet, ND6_OPT_RDNSS, &option_len) == NULL;
    memcpy(test_ccb.hsi_ipv6_dns[1], dns_servers[1], 16);
    nd6_build_ra(&test_ccb, packet, &packet_len);
    const U8 *rdnss = find_ra_option(packet, ND6_OPT_RDNSS, &option_len);
    TEST_ASSERT(none && rdnss != NULL && option_len == 24 &&
            memcmp(rdnss + 8, dns_servers[1], 16) == 0,
        "T7 zero and single RDNSS entries", NULL);
}

static void test_malformed_packets(void)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U8 gateway[16];
    struct rte_ether_addr found;
    U16 len;
    U32 tx_count;
    BOOL dropped = TRUE;

    reset_fixture();
    nd6_gateway_link_local(&g_fastrg_ccb->nic_info.hsi_lan_mac, gateway);
    len = build_nd_packet(packet, ND6_ICMP_NS, host_ip, gateway,
        ND6_OPT_SLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len - 1);

    len = build_nd_packet(packet, ND6_ICMP_NS, host_ip, gateway,
        ND6_OPT_SLLA, 0);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);

    len = build_nd_packet(packet, ND6_ICMP_NS, host_ip, gateway,
        ND6_OPT_SLLA, 1);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)packet;
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)
        ((vlan_header_t *)(eth + 1) + 1);
    ip6->payload_len = rte_cpu_to_be_16(rte_be_to_cpu_16(ip6->payload_len) + 8);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_test_get_last_tx(NULL, &tx_count);
    dropped = dropped && tx_count == 0 &&
        nd6_table_lookup(test_ccb.nd6_table, host_ip, &found) == ERROR;
    TEST_ASSERT(dropped, "T8 malformed ND options and lengths", NULL);
}

static void test_timer_callback(void)
{
    U32 tx_count;

    reset_fixture();
    rte_timer_reset(&test_ccb.ra_timer, 1000000, PERIODICAL, 0,
        nd6_ra_timer_cb, &test_ccb);
    nd6_ra_timer_cb(&test_ccb.ra_timer, &test_ccb);
    nd6_test_get_last_tx(NULL, &tx_count);
    BOOL sent = tx_count == 1 && rte_timer_pending(&test_ccb.ra_timer) != 0;
    test_ccb.dhcp6_pd_ready = FALSE;
    nd6_ra_timer_cb(&test_ccb.ra_timer, &test_ccb);
    TEST_ASSERT(sent && rte_timer_pending(&test_ccb.ra_timer) == 0,
        "T9 periodic RA timer callback", NULL);
}

static void test_offlink_learn_gate(void)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U8 gateway[16];
    U8 offlink_ip[16] = {0x20, 0x01, 0x0d, 0xb8, 0x99, 0x99, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    U8 multicast_ip[16] = {0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x99};
    U8 link_local_ip[16] = {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    struct rte_ether_addr found;
    U16 len;
    U32 tx_count;

    reset_fixture();
    nd6_gateway_link_local(&g_fastrg_ccb->nic_info.hsi_lan_mac, gateway);
    len = build_nd_packet(packet, ND6_ICMP_NS, offlink_ip, gateway,
        ND6_OPT_SLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_test_get_last_tx(NULL, &tx_count);
    BOOL offlink_ignored = tx_count == 0 &&
        nd6_table_lookup(test_ccb.nd6_table, offlink_ip, &found) == ERROR;

    len = build_nd_packet(packet, ND6_ICMP_NA, multicast_ip, multicast_ip,
        ND6_OPT_TLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    BOOL multicast_ignored = nd6_table_lookup(test_ccb.nd6_table,
        multicast_ip, &found) == ERROR;

    nd6_test_tx_reset();
    len = build_nd_packet(packet, ND6_ICMP_NS, link_local_ip, gateway,
        ND6_OPT_SLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_test_get_last_tx(NULL, &tx_count);
    TEST_ASSERT(offlink_ignored && multicast_ignored && tx_count == 1 &&
            nd6_table_lookup(test_ccb.nd6_table, link_local_ip,
                &found) == SUCCESS &&
            rte_is_same_ether_addr(&found, &host_mac),
        "T10 off-link and multicast sources are never learned", NULL);
}

static void test_rs_ra_rate_limit(void)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U8 zero[16] = {0};
    U16 len;
    U32 tx_count;

    reset_fixture();
    test_ccb.last_rs_ra_cycles = 0;
    len = build_nd_packet(packet, ND6_ICMP_RS, host_ip, zero,
        ND6_OPT_SLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_test_get_last_tx(NULL, &tx_count);
    BOOL first_replied = tx_count == 1;

    nd6_test_tx_reset();
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_test_get_last_tx(NULL, &tx_count);
    BOOL second_suppressed = tx_count == 0;

    test_ccb.last_rs_ra_cycles = 0;
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_test_get_last_tx(NULL, &tx_count);
    TEST_ASSERT(first_replied && second_suppressed && tx_count == 1,
        "T11 RS flood collapses to one RA per window", NULL);
}

static void test_dad_defense(void)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U8 gateway[16];
    U8 zero[16] = {0};
    struct rte_ether_addr found;
    U16 len, tx_len;
    U32 tx_count;

    reset_fixture();
    nd6_gateway_link_local(&g_fastrg_ccb->nic_info.hsi_lan_mac, gateway);
    len = build_nd_packet(packet, ND6_ICMP_NS, zero, gateway, 0, 0);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    const U8 *tx = nd6_test_get_last_tx(&tx_len, &tx_count);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)tx;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(vlan + 1);
    test_neighbor_t *na = (test_neighbor_t *)(ip6 + 1);
    U8 *dst_ip = (U8 *)&ip6->dst_addr;
    BOOL defended = tx_count == 1 && tx_len > 0 &&
        eth->dst_addr.addr_bytes[0] == 0x33 &&
        eth->dst_addr.addr_bytes[1] == 0x33 &&
        eth->dst_addr.addr_bytes[5] == 0x01 &&
        dst_ip[0] == 0xff && dst_ip[1] == 0x02 && dst_ip[15] == 0x01 &&
        na->icmp.type == ND6_ICMP_NA &&
        rte_be_to_cpu_32(na->icmp.data) == UINT32_C(0xa0000000) &&
        memcmp(na->target, gateway, 16) == 0 &&
        rte_ipv6_udptcp_cksum_verify(ip6, na) == 0;
    TEST_ASSERT(defended &&
            nd6_table_lookup(test_ccb.nd6_table, zero, &found) == ERROR,
        "T12 gateway DAD probe answered with unsolicited NA", NULL);
}

static void test_neighbor_aging(void)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    struct rte_ether_addr found;
    const U8 *tx;
    U16 len, tx_len;
    U32 tx_count;
    U64 fresh, aged;

    reset_fixture();
    nd6_table_learn(test_ccb.nd6_table, host_ip, &host_mac);
    fresh = fastrg_get_cur_cycles();
    aged = fresh + (U64)(ND6_NEIGHBOR_TTL_SEC + 1) * fastrg_get_cycles_in_sec();

    /* A sweep at learn time neither probes nor deletes. */
    nd6_age_scan_table(&test_ccb, fresh);
    nd6_test_get_last_tx(NULL, &tx_count);
    BOOL fresh_kept = tx_count == 0 &&
        nd6_table_lookup(test_ccb.nd6_table, host_ip, &found) == SUCCESS;

    /* Idle past the TTL: one unicast NS probe, entry still present. */
    nd6_age_scan_table(&test_ccb, aged);
    tx = nd6_test_get_last_tx(&tx_len, &tx_count);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)tx;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(vlan + 1);
    test_neighbor_t *ns = (test_neighbor_t *)(ip6 + 1);
    U8 gateway[16];

    nd6_gateway_link_local(&g_fastrg_ccb->nic_info.hsi_lan_mac, gateway);
    BOOL probed = tx_count == 1 && tx_len > 0 &&
        rte_is_same_ether_addr(&eth->dst_addr, &host_mac) &&
        ns->icmp.type == ND6_ICMP_NS && ns->icmp.code == 0 &&
        memcmp(ns->target, host_ip, 16) == 0 &&
        memcmp((U8 *)&ip6->dst_addr, host_ip, 16) == 0 &&
        memcmp((U8 *)&ip6->src_addr, gateway, 16) == 0 &&
        ip6->hop_limits == 255 &&
        ((U8 *)(ns + 1))[0] == ND6_OPT_SLLA &&
        ((U8 *)(ns + 1))[1] == 1 &&
        memcmp((U8 *)(ns + 1) + 2,
            g_fastrg_ccb->nic_info.hsi_lan_mac.addr_bytes, 6) == 0 &&
        rte_ipv6_udptcp_cksum_verify(ip6, ns) == 0 &&
        nd6_table_lookup(test_ccb.nd6_table, host_ip, &found) == SUCCESS;

    /* An answering NA refreshes the entry and clears the outstanding probe,
     * so the next sweep probes again instead of deleting. */
    nd6_test_tx_reset();
    len = build_nd_packet(packet, ND6_ICMP_NA, host_ip, host_ip,
        ND6_OPT_TLLA, 1);
    nd6_lan_input(g_fastrg_ccb, 0, packet, len);
    nd6_age_scan_table(&test_ccb, aged);
    nd6_test_get_last_tx(NULL, &tx_count);
    BOOL refreshed = tx_count == 1 &&
        nd6_table_lookup(test_ccb.nd6_table, host_ip, &found) == SUCCESS;

    /* That probe goes unanswered: the entry is deleted. */
    nd6_age_scan_table(&test_ccb, aged);
    TEST_ASSERT(fresh_kept && probed && refreshed &&
            nd6_table_lookup(test_ccb.nd6_table, host_ip, &found) == ERROR,
        "T13 idle neighbors are probed once then reclaimed", NULL);
}

static void test_stale_generation_reclaim(void)
{
    U8 addr[16];
    struct rte_ether_addr found;
    U64 now;
    BOOL filled = TRUE;

    reset_fixture();
    now = fastrg_get_cur_cycles();
    /* Hand back key slots deferred by earlier sweeps so the fill below starts
     * from a genuinely empty table. */
    for(U8 i=0; i<3; i++) {
        rte_rcu_qsbr_quiescent(test_rcu, 0);
        nd6_age_scan_table(&test_ccb, now);
    }

    memcpy(addr, lan_prefix, 16);
    for(U32 i=0; i<ND6_TABLE_ENTRIES; i++) {
        addr[14] = (U8)(i >> 8);
        addr[15] = (U8)i;
        nd6_table_learn(test_ccb.nd6_table, addr, &host_mac);
    }
    for(U32 i=0; i<ND6_TABLE_ENTRIES; i++) {
        addr[14] = (U8)(i >> 8);
        addr[15] = (U8)i;
        filled = filled &&
            nd6_table_lookup(test_ccb.nd6_table, addr, &found) == SUCCESS;
    }

    /* One more address has nowhere to go while the table is full. */
    addr[13] = 0x01;
    nd6_table_learn(test_ccb.nd6_table, addr, &host_mac);
    BOOL rejected = nd6_table_lookup(test_ccb.nd6_table, addr, &found) == ERROR;

    /* A reset leaves every key behind at the previous generation; the sweep
     * deletes them and the deferred reclaim makes the slots allocatable
     * again once the reader has crossed a grace period. */
    nd6_table_reset(test_ccb.nd6_table);
    rte_rcu_qsbr_quiescent(test_rcu, 0);
    nd6_age_scan_table(&test_ccb, now);
    rte_rcu_qsbr_quiescent(test_rcu, 0);
    nd6_table_learn(test_ccb.nd6_table, addr, &host_mac);
    TEST_ASSERT(filled && rejected &&
            nd6_table_lookup(test_ccb.nd6_table, addr, &found) == SUCCESS &&
            rte_is_same_ether_addr(&found, &host_mac),
        "T14 stale generation entries are swept and their slots reused", NULL);
}

static void test_ns_builder(void)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U8 solicited[16];
    U8 gateway[16];
    struct rte_ether_addr solicited_mac;
    U16 packet_len = 0;
    STATUS status;

    reset_fixture();
    nd6_solicited_node_addr(host_ip, solicited);
    solicited_mac.addr_bytes[0] = 0x33;
    solicited_mac.addr_bytes[1] = 0x33;
    memcpy(&solicited_mac.addr_bytes[2], &solicited[12], 4);
    nd6_gateway_link_local(&g_fastrg_ccb->nic_info.hsi_lan_mac, gateway);
    status = nd6_build_ns(&test_ccb, host_ip, solicited, &solicited_mac,
        packet, &packet_len);

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)packet;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(vlan + 1);
    test_neighbor_t *ns = (test_neighbor_t *)(ip6 + 1);
    U8 *dst_ip = (U8 *)&ip6->dst_addr;

    TEST_ASSERT(status == SUCCESS &&
            packet_len == sizeof(*eth) + sizeof(*vlan) + sizeof(*ip6) + 32 &&
            solicited[0] == 0xff && solicited[1] == 0x02 &&
            solicited[11] == 0x01 && solicited[12] == 0xff &&
            memcmp(&solicited[13], &host_ip[13], 3) == 0 &&
            eth->dst_addr.addr_bytes[0] == 0x33 &&
            eth->dst_addr.addr_bytes[1] == 0x33 &&
            memcmp(&eth->dst_addr.addr_bytes[2], &solicited[12], 4) == 0 &&
            vlan->next_proto == rte_cpu_to_be_16(FRAME_TYPE_IPV6) &&
            memcmp(dst_ip, solicited, 16) == 0 &&
            memcmp((U8 *)&ip6->src_addr, gateway, 16) == 0 &&
            ns->icmp.type == ND6_ICMP_NS &&
            memcmp(ns->target, host_ip, 16) == 0 &&
            rte_ipv6_udptcp_cksum_verify(ip6, ns) == 0,
        "T15 solicited-node NS frame format", NULL);
}

void test_nd6(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    ppp_ccb_t *original_ccb = fastrg_ccb->ppp_ccb[0];

    g_fastrg_ccb = fastrg_ccb;
    memset(&test_ccb, 0, sizeof(test_ccb));
    test_ccb.fastrg_ccb = fastrg_ccb;
    test_ccb.user_num = 1;
    rte_atomic16_init(&test_ccb.vlan_id);
    rte_atomic16_set(&test_ccb.vlan_id, 321);
    rte_timer_init(&test_ccb.ra_timer);
    if (test_rcu == NULL) {
        size_t rcu_size = rte_rcu_qsbr_get_memsize(1);

        test_rcu = calloc(1, rcu_size);
        assert(test_rcu != NULL && rte_rcu_qsbr_init(test_rcu, 1) == 0);
        rte_rcu_qsbr_thread_register(test_rcu, 0);
        rte_rcu_qsbr_thread_online(test_rcu, 0);
    }
    test_ccb.nd6_table = nd6_table_alloc(60000, test_rcu);
    if (test_ccb.nd6_table == NULL) {
        fprintf(stderr, "ND6 fixture allocation failed\n");
        assert(test_ccb.nd6_table != NULL);
    }
    fastrg_ccb->ppp_ccb[0] = &test_ccb;

    test_ra_bytes();
    test_rs_to_ra();
    test_ns_to_na();
    test_neighbor_learning_and_dad();
    test_cache_generation();
    test_three_gates();
    test_rdnss_boundaries();
    test_malformed_packets();
    test_timer_callback();
    test_offlink_learn_gate();
    test_rs_ra_rate_limit();
    test_dad_defense();
    test_neighbor_aging();
    test_stale_generation_reclaim();
    test_ns_builder();

    rte_timer_stop_sync(&test_ccb.ra_timer);
    nd6_table_free(test_ccb.nd6_table);
    fastrg_ccb->ppp_ccb[0] = original_ccb;
    *total_tests += test_count;
    *total_pass += pass_count;
}
