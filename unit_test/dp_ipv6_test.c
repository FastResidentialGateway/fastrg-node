#include <stdio.h>
#include <string.h>

#include <common.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip6.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "../src/dp_ipv6.h"
#include "../src/nd6/nd6.h"
#include "../src/protocol.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;
static FastRG_t *g_fastrg_ccb;
static ppp_ccb_t test_ccb;

#define MOCK_BUF_LEN   2048
#define MOCK_HEADROOM  128
#define TEST_VLAN_ID   321
#define TEST_SESSION   0x1234

/* Mock mbuf: the private area must sit immediately behind the rte_mbuf, which
 * is where rte_mbuf_to_priv() looks for it. */
typedef struct mock_packet {
    struct rte_mbuf mbuf;
    mbuf_priv_t priv;
    U8 data[MOCK_BUF_LEN];
} __rte_cache_aligned mock_packet_t;

static const struct rte_ether_addr host_mac = {
    .addr_bytes = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50},
};
static const struct rte_ether_addr other_mac = {
    .addr_bytes = {0x02, 0x99, 0x99, 0x99, 0x99, 0x99},
};
static const struct rte_ether_addr bras_mac = {
    .addr_bytes = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee},
};

/* Delegated prefix 2001:db8:1234:5600::/56, LAN /64 is its first subnet. */
static const U8 lan_prefix[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x00,
};
static const U8 lan_host[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};
static const U8 second_subnet[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};
static const U8 remote_host[16] = {
    0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x11,
};
static const U8 off_prefix_host[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0x99, 0x99, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};
static const U8 link_local[16] = {
    0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};
static const U8 all_nodes[16] = {
    0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};
static const U8 loopback[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

static void set_gate(BOOL ipv6_enabled, BOOL ipv6cp_up, BOOL pd_ready)
{
    test_ccb.ipv6_enabled = ipv6_enabled;
    test_ccb.ipv6cp_up = ipv6cp_up;
    test_ccb.dhcp6_pd_ready = pd_ready;
    pppd_ipv6_dp_gate_update(&test_ccb);
}

static U8 *mock_reset(mock_packet_t *pkt)
{
    memset(pkt, 0, sizeof(*pkt));
    pkt->mbuf.buf_addr = pkt->data;
    pkt->mbuf.buf_len = MOCK_BUF_LEN;
    pkt->mbuf.data_off = MOCK_HEADROOM;
    pkt->priv.ccb_id = 0;
    return pkt->data + MOCK_HEADROOM;
}

/* Build eth / vlan / IPv6 as it arrives on the LAN port. The L4 payload is a
 * fixed byte pattern so header moves are easy to verify. */
static struct rte_ipv6_hdr *build_lan_frame(mock_packet_t *pkt,
    const struct rte_ether_addr *dst_mac, const U8 *src_ip, const U8 *dst_ip,
    U8 proto, U8 hop_limit, U16 payload_len)
{
    U8 *frame = mock_reset(pkt);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)frame;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(vlan + 1);
    U8 *payload = (U8 *)(ip6 + 1);

    rte_ether_addr_copy(&host_mac, &eth->src_addr);
    rte_ether_addr_copy(dst_mac, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(VLAN);
    vlan->tci_union.tci_value = rte_cpu_to_be_16(TEST_VLAN_ID);
    vlan->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IPV6);
    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(6) << 28);
    ip6->payload_len = rte_cpu_to_be_16(payload_len);
    ip6->proto = proto;
    ip6->hop_limits = hop_limit;
    memcpy(&ip6->src_addr, src_ip, 16);
    memcpy(&ip6->dst_addr, dst_ip, 16);
    for(U16 i=0; i<payload_len; i++)
        payload[i] = (U8)(i & 0xff);
    pkt->mbuf.pkt_len = IPV6_L2_LEN + sizeof(*ip6) + payload_len;
    pkt->mbuf.data_len = pkt->mbuf.pkt_len;
    pkt->priv.eth_hdr = eth;
    pkt->priv.vlan_hdr = vlan;
    return ip6;
}

/* Build eth / vlan / PPPoE / PPP 0x0057 / IPv6 as it arrives on the WAN port. */
static struct rte_ipv6_hdr *build_wan_frame(mock_packet_t *pkt,
    const U8 *src_ip, const U8 *dst_ip, U8 proto, U8 hop_limit,
    U16 payload_len)
{
    U8 *frame = mock_reset(pkt);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)frame;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    pppoe_header_t *pppoe = (pppoe_header_t *)(vlan + 1);
    U16 *ppp_proto = (U16 *)(pppoe + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(ppp_proto + 1);
    U8 *payload = (U8 *)(ip6 + 1);

    rte_ether_addr_copy(&bras_mac, &eth->src_addr);
    rte_ether_addr_copy(&g_fastrg_ccb->nic_info.hsi_wan_src_mac,
        &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(VLAN);
    vlan->tci_union.tci_value = rte_cpu_to_be_16(TEST_VLAN_ID);
    vlan->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);
    pppoe->ver_type = VER_TYPE;
    pppoe->code = 0;
    pppoe->session_id = rte_cpu_to_be_16(TEST_SESSION);
    pppoe->length = rte_cpu_to_be_16((U16)(sizeof(*ip6) + payload_len +
        sizeof(ppp_payload_t)));
    *ppp_proto = rte_cpu_to_be_16(PPP_IPV6_PROTOCOL);
    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(6) << 28);
    ip6->payload_len = rte_cpu_to_be_16(payload_len);
    ip6->proto = proto;
    ip6->hop_limits = hop_limit;
    memcpy(&ip6->src_addr, src_ip, 16);
    memcpy(&ip6->dst_addr, dst_ip, 16);
    for(U16 i=0; i<payload_len; i++)
        payload[i] = (U8)(i & 0xff);
    pkt->mbuf.pkt_len = IPV6_L2_LEN + IPV6_PPPOE_HDR_LEN + sizeof(*ip6) +
        payload_len;
    pkt->mbuf.data_len = pkt->mbuf.pkt_len;
    pkt->priv.eth_hdr = eth;
    pkt->priv.vlan_hdr = vlan;
    return ip6;
}

static ipv6_lan_verdict_t classify_lan(mock_packet_t *pkt)
{
    return ipv6_lan_classify(g_fastrg_ccb, &test_ccb, pkt->priv.eth_hdr,
        (struct rte_ipv6_hdr *)((char *)pkt->priv.eth_hdr + IPV6_L2_LEN),
        pkt->mbuf.pkt_len);
}

static ipv6_wan_verdict_t classify_wan(mock_packet_t *pkt)
{
    return ipv6_wan_classify(&test_ccb, (struct rte_ipv6_hdr *)
        ((char *)pkt->priv.eth_hdr + IPV6_L2_LEN + IPV6_PPPOE_HDR_LEN),
        pkt->mbuf.pkt_len);
}

static void test_lan_classification(void)
{
    mock_packet_t pkt;
    struct rte_ipv6_hdr *ip6;
    U8 *icmp6;
    BOOL gates_closed = TRUE;

    printf("\nTesting IPv6 LAN ingress classification:\n");

    set_gate(TRUE, TRUE, TRUE);
    build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac, lan_host,
        remote_host, IPPROTO_TCP, 64, 20);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_FORWARD,
        "T1 subscriber traffic to the internet is forwarded", NULL);

    for(U8 gate=0; gate<3; gate++) {
        set_gate(gate != 0, gate != 1, gate != 2);
        gates_closed = gates_closed && classify_lan(&pkt) == IPV6_LAN_DROP &&
            pppd_ipv6_dp_gate_open(&test_ccb) == FALSE;
    }
    set_gate(TRUE, TRUE, TRUE);
    TEST_ASSERT(gates_closed && pppd_ipv6_dp_gate_open(&test_ccb) == TRUE,
        "T1 each readiness flag closes the forwarding gate", NULL);

    build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac,
        off_prefix_host, remote_host, IPPROTO_TCP, 64, 20);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_DROP,
        "T1 source outside the LAN prefix is dropped", NULL);

    build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac, link_local,
        remote_host, IPPROTO_TCP, 64, 20);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_DROP,
        "T1 link-local source is dropped", NULL);

    build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac, lan_host,
        all_nodes, IPPROTO_UDP, 64, 20);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_DROP,
        "T1 multicast destination is dropped", NULL);

    build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac, lan_host,
        link_local, IPPROTO_UDP, 64, 20);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_DROP,
        "T1 link-local destination is dropped", NULL);

    build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac, lan_host,
        lan_prefix, IPPROTO_TCP, 64, 20);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_DROP,
        "T1 on-link destination is not hairpinned", NULL);

    build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac, lan_host,
        loopback, IPPROTO_TCP, 64, 20);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_DROP,
        "T1 destination without a routable high half is dropped", NULL);

    build_lan_frame(&pkt, &other_mac, lan_host, remote_host, IPPROTO_TCP,
        64, 20);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_PASSTHROUGH,
        "T1 frame for another MAC passes through untouched", NULL);

    ip6 = build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac,
        lan_host, remote_host, IPPROTO_TCP, 64, 20);
    ip6->payload_len = rte_cpu_to_be_16(21);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_DROP,
        "T1 payload length disagreeing with the frame is dropped", NULL);

    ip6 = build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac,
        lan_host, remote_host, IPPROTO_TCP, 64, 20);
    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(4) << 28);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_DROP,
        "T1 non-6 version field is dropped", NULL);

    BOOL nd_to_cp = TRUE;
    U8 nd_types[3] = {ND6_ICMP_RS, ND6_ICMP_NS, ND6_ICMP_NA};
    for(U8 i=0; i<3; i++) {
        ip6 = build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac,
            link_local, all_nodes, IPPROTO_ICMPV6, 255, 24);
        icmp6 = (U8 *)(ip6 + 1);
        icmp6[0] = nd_types[i];
        nd_to_cp = nd_to_cp && classify_lan(&pkt) == IPV6_LAN_TO_CP;
    }
    TEST_ASSERT(nd_to_cp,
        "T1 RS, NS and NA still reach the control plane", NULL);
}

static void test_lan_to_wan_transform(void)
{
    mock_packet_t pkt;
    struct rte_ipv6_hdr *ip6;
    U32 orig_len;

    printf("\nTesting IPv6 LAN to WAN encapsulation:\n");

    set_gate(TRUE, TRUE, TRUE);
    ip6 = build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac, lan_host,
        remote_host, IPPROTO_UDP, 64, 32);
    orig_len = pkt.mbuf.pkt_len;
    ipv6_lan_to_wan_encap(g_fastrg_ccb, &test_ccb, &pkt.mbuf, 0);

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(&pkt.mbuf,
        struct rte_ether_hdr *);
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    pppoe_header_t *pppoe = (pppoe_header_t *)(vlan + 1);
    U16 *ppp_proto = (U16 *)(pppoe + 1);
    struct rte_ipv6_hdr *out_ip6 = (struct rte_ipv6_hdr *)(ppp_proto + 1);

    TEST_ASSERT(pkt.mbuf.pkt_len == orig_len + IPV6_PPPOE_HDR_LEN &&
            pkt.mbuf.data_len == pkt.mbuf.pkt_len &&
            pkt.mbuf.data_off == MOCK_HEADROOM - IPV6_PPPOE_HDR_LEN,
        "T2 frame grows by the 8 PPPoE bytes", NULL);
    TEST_ASSERT(out_ip6 == ip6 && out_ip6->hop_limits == 63,
        "T2 hop limit is decremented and the IPv6 packet stays in place", NULL);
    TEST_ASSERT(rte_is_same_ether_addr(&eth->src_addr,
                &g_fastrg_ccb->nic_info.hsi_wan_src_mac) &&
            rte_is_same_ether_addr(&eth->dst_addr, &test_ccb.PPP_dst_mac) &&
            eth->ether_type == rte_cpu_to_be_16(VLAN),
        "T2 Ethernet header is rewritten for the WAN side", NULL);
    TEST_ASSERT(vlan->tci_union.tci_value == rte_cpu_to_be_16(TEST_VLAN_ID) &&
            vlan->next_proto == rte_cpu_to_be_16(ETH_P_PPP_SES),
        "T2 VLAN tag is kept and marked as a PPPoE session", NULL);
    TEST_ASSERT(pppoe->ver_type == VER_TYPE && pppoe->code == 0 &&
            pppoe->session_id == test_ccb.session_id &&
            pppoe->length == rte_cpu_to_be_16((U16)(orig_len - IPV6_L2_LEN +
                sizeof(ppp_payload_t))) &&
            *ppp_proto == rte_cpu_to_be_16(PPP_IPV6_PROTOCOL),
        "T2 PPPoE header carries the IPv6 PPP protocol", NULL);

    build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac, lan_host,
        remote_host, IPPROTO_UDP, 1, 32);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_DROP,
        "T2 hop limit 1 is dropped instead of forwarded", NULL);

    build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac, lan_host,
        remote_host, IPPROTO_UDP, 0, 32);
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_DROP,
        "T2 hop limit 0 is dropped", NULL);
}

static void test_packet_too_big(void)
{
    mock_packet_t pkt;
    struct rte_ipv6_hdr *ip6;
    U8 reply[IPV6_PTB_MAX_LEN];
    U8 gateway[16];
    U16 reply_len, expected_quote;
    U32 mtu_field;

    printf("\nTesting IPv6 oversize handling:\n");

    set_gate(TRUE, TRUE, TRUE);
    build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac, lan_host,
        remote_host, IPPROTO_TCP, 64,
        (U16)(IPV6_PPPOE_MTU - sizeof(struct rte_ipv6_hdr)));
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_FORWARD,
        "T3 a packet exactly at the PPPoE MTU is forwarded", NULL);

    ip6 = build_lan_frame(&pkt, &g_fastrg_ccb->nic_info.hsi_lan_mac, lan_host,
        remote_host, IPPROTO_TCP, 64,
        (U16)(IPV6_PPPOE_MTU - sizeof(struct rte_ipv6_hdr) + 1));
    TEST_ASSERT(classify_lan(&pkt) == IPV6_LAN_TOO_BIG,
        "T3 one byte over the PPPoE MTU asks for Packet Too Big", NULL);

    reply_len = ipv6_build_packet_too_big(&test_ccb, pkt.priv.eth_hdr,
        pkt.priv.vlan_hdr, ip6, (U16)(pkt.mbuf.pkt_len - IPV6_L2_LEN), reply);

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)reply;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *reply_ip6 = (struct rte_ipv6_hdr *)(vlan + 1);
    U8 *icmp6 = (U8 *)(reply_ip6 + 1);

    expected_quote = (U16)(IPV6_MIN_MTU - sizeof(struct rte_ipv6_hdr) -
        ICMP6_PTB_HDR_LEN);
    memcpy(&mtu_field, icmp6 + 4, sizeof(mtu_field));
    nd6_gateway_link_local(&g_fastrg_ccb->nic_info.hsi_lan_mac, gateway);

    TEST_ASSERT(reply_len == IPV6_L2_LEN + sizeof(*reply_ip6) +
                ICMP6_PTB_HDR_LEN + expected_quote &&
            reply_len == IPV6_L2_LEN + IPV6_MIN_MTU,
        "T3 the error never exceeds the minimum IPv6 MTU", NULL);
    TEST_ASSERT(icmp6[0] == ICMP6_PACKET_TOO_BIG && icmp6[1] == 0 &&
            rte_be_to_cpu_32(mtu_field) == IPV6_PPPOE_MTU,
        "T3 type 2 code 0 reporting MTU 1492", NULL);
    TEST_ASSERT(memcmp((U8 *)&reply_ip6->src_addr, gateway, 16) == 0 &&
            memcmp((U8 *)&reply_ip6->dst_addr, lan_host, 16) == 0 &&
            reply_ip6->proto == IPPROTO_ICMPV6,
        "T3 sent from the gateway link-local back to the sender", NULL);
    TEST_ASSERT(rte_is_same_ether_addr(&eth->dst_addr, &host_mac) &&
            rte_is_same_ether_addr(&eth->src_addr,
                &g_fastrg_ccb->nic_info.hsi_lan_mac) &&
            vlan->tci_union.tci_value == rte_cpu_to_be_16(TEST_VLAN_ID) &&
            vlan->next_proto == rte_cpu_to_be_16(FRAME_TYPE_IPV6),
        "T3 L2 header is addressed back to the LAN host", NULL);
    TEST_ASSERT(memcmp(icmp6 + ICMP6_PTB_HDR_LEN, ip6, expected_quote) == 0,
        "T3 the original packet is quoted up to the truncation point", NULL);
    TEST_ASSERT(rte_ipv6_udptcp_cksum_verify(reply_ip6, icmp6) == 0,
        "T3 ICMPv6 checksum is valid", NULL);
}

static void test_wan_classification(void)
{
    mock_packet_t pkt;
    struct rte_ipv6_hdr *ip6;
    struct rte_udp_hdr *udp;

    printf("\nTesting IPv6 WAN ingress classification:\n");

    set_gate(TRUE, TRUE, TRUE);
    build_wan_frame(&pkt, remote_host, lan_host, IPPROTO_TCP, 64, 20);
    TEST_ASSERT(classify_wan(&pkt) == IPV6_WAN_FORWARD,
        "T4 traffic for the LAN prefix is forwarded", NULL);

    build_wan_frame(&pkt, remote_host, second_subnet, IPPROTO_TCP, 64, 20);
    TEST_ASSERT(classify_wan(&pkt) == IPV6_WAN_DROP,
        "T4 another subnet of the delegated prefix is dropped", NULL);

    ip6 = build_wan_frame(&pkt, remote_host, off_prefix_host, IPPROTO_UDP,
        64, 20);
    udp = (struct rte_udp_hdr *)(ip6 + 1);
    udp->dst_port = rte_cpu_to_be_16(DHCP6_CLIENT_PORT);
    TEST_ASSERT(classify_wan(&pkt) == IPV6_WAN_TO_CP,
        "T4 DHCPv6 replies go to the control plane", NULL);

    build_wan_frame(&pkt, remote_host, link_local, IPPROTO_ICMPV6, 255, 24);
    TEST_ASSERT(classify_wan(&pkt) == IPV6_WAN_TO_CP,
        "T4 ICMPv6 to our WAN link-local goes to the control plane", NULL);

    build_wan_frame(&pkt, remote_host, all_nodes, IPPROTO_ICMPV6, 255, 24);
    TEST_ASSERT(classify_wan(&pkt) == IPV6_WAN_TO_CP,
        "T4 ICMPv6 to a multicast group goes to the control plane", NULL);

    build_wan_frame(&pkt, remote_host, off_prefix_host, IPPROTO_TCP, 64, 20);
    TEST_ASSERT(classify_wan(&pkt) == IPV6_WAN_DROP,
        "T4 traffic for an address we do not serve is dropped", NULL);

    ip6 = build_wan_frame(&pkt, remote_host, lan_host, IPPROTO_TCP, 64, 20);
    ip6->payload_len = rte_cpu_to_be_16(19);
    TEST_ASSERT(classify_wan(&pkt) == IPV6_WAN_DROP,
        "T4 inner length disagreeing with the frame is dropped", NULL);

    /* With any readiness flag down the RX loops skip classification entirely
     * and hand the untouched frame to the control plane. */
    set_gate(TRUE, TRUE, FALSE);
    TEST_ASSERT(pppd_ipv6_dp_gate_open(&test_ccb) == FALSE,
        "T4 a closed gate keeps WAN IPv6 on the control-plane path", NULL);
    set_gate(TRUE, TRUE, TRUE);
}

static void test_wan_to_lan_transform(void)
{
    mock_packet_t pkt;
    struct rte_ipv6_hdr *ip6;
    U32 orig_len;
    U8 payload_copy[32];

    printf("\nTesting IPv6 WAN to LAN decapsulation:\n");

    set_gate(TRUE, TRUE, TRUE);
    nd6_table_reset(test_ccb.nd6_table);
    nd6_table_learn(test_ccb.nd6_table, lan_host, &host_mac);

    ip6 = build_wan_frame(&pkt, remote_host, lan_host, IPPROTO_UDP, 64, 32);
    memcpy(payload_copy, (U8 *)(ip6 + 1), sizeof(payload_copy));
    orig_len = pkt.mbuf.pkt_len;
    ipv6_forward_result_t result = ipv6_wan_to_lan_forward(g_fastrg_ccb,
        &test_ccb, &pkt.mbuf, 0);

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(&pkt.mbuf,
        struct rte_ether_hdr *);
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *out_ip6 = (struct rte_ipv6_hdr *)(vlan + 1);

    TEST_ASSERT(result == IPV6_FWD_OK &&
            pkt.mbuf.pkt_len == orig_len - IPV6_PPPOE_HDR_LEN &&
            pkt.mbuf.data_len == pkt.mbuf.pkt_len &&
            pkt.mbuf.data_off == MOCK_HEADROOM + IPV6_PPPOE_HDR_LEN,
        "T5 the 8 PPPoE bytes are stripped", NULL);
    TEST_ASSERT(out_ip6 == ip6 && out_ip6->hop_limits == 63 &&
            memcmp((U8 *)(out_ip6 + 1), payload_copy,
                sizeof(payload_copy)) == 0,
        "T5 hop limit is decremented and the payload is untouched", NULL);
    TEST_ASSERT(rte_is_same_ether_addr(&eth->src_addr,
                &g_fastrg_ccb->nic_info.hsi_lan_mac) &&
            rte_is_same_ether_addr(&eth->dst_addr, &host_mac) &&
            vlan->tci_union.tci_value == rte_cpu_to_be_16(TEST_VLAN_ID) &&
            vlan->next_proto == rte_cpu_to_be_16(FRAME_TYPE_IPV6),
        "T5 L2 header is rewritten for the resolved LAN host", NULL);
    TEST_ASSERT(pkt.priv.eth_hdr == eth && pkt.priv.vlan_hdr == vlan,
        "T5 cached header pointers follow the move", NULL);

    /* An address the control plane never learned: the frame must come back
     * exactly as it arrived so the miss escalation can carry it. */
    U8 unknown[16];
    memcpy(unknown, lan_host, 16);
    unknown[15] = 0x11;
    build_wan_frame(&pkt, remote_host, unknown, IPPROTO_UDP, 64, 32);
    orig_len = pkt.mbuf.pkt_len;
    result = ipv6_wan_to_lan_forward(g_fastrg_ccb, &test_ccb, &pkt.mbuf, 0);
    TEST_ASSERT(result == IPV6_FWD_NEIGHBOR_MISS &&
            pkt.mbuf.pkt_len == orig_len &&
            pkt.mbuf.data_off == MOCK_HEADROOM,
        "T5 an unresolved destination reports a miss and leaves the frame alone",
        NULL);

    ip6 = build_wan_frame(&pkt, remote_host, lan_host, IPPROTO_UDP, 1, 32);
    result = ipv6_wan_to_lan_forward(g_fastrg_ccb, &test_ccb, &pkt.mbuf, 0);
    TEST_ASSERT(result == IPV6_FWD_HOP_LIMIT && ip6->hop_limits == 1,
        "T5 hop limit 1 is dropped without touching the packet", NULL);
}

static void test_flow_tag(void)
{
    U16 src_port = rte_cpu_to_be_16(45000);
    U16 dst_port = rte_cpu_to_be_16(443);
    U32 tag = compute_flow_tag6(lan_host, remote_host, src_port, dst_port,
        IPPROTO_TCP);

    printf("\nTesting compute_flow_tag6 function:\n");

    TEST_ASSERT(tag == compute_flow_tag6(lan_host, remote_host, src_port,
            dst_port, IPPROTO_TCP),
        "T6 the same 5-tuple always yields the same tag", NULL);
    TEST_ASSERT(tag != compute_flow_tag6(remote_host, lan_host, dst_port,
            src_port, IPPROTO_TCP),
        "T6 the reverse direction yields a different tag", NULL);
    TEST_ASSERT(tag != compute_flow_tag6(lan_host, second_subnet, src_port,
            dst_port, IPPROTO_TCP),
        "T6 a different destination yields a different tag", NULL);
    TEST_ASSERT(tag != compute_flow_tag6(lan_host, remote_host,
            rte_cpu_to_be_16(45001), dst_port, IPPROTO_TCP),
        "T6 a different source port yields a different tag", NULL);
    TEST_ASSERT(tag != compute_flow_tag6(lan_host, remote_host, src_port,
            dst_port, IPPROTO_UDP),
        "T6 a different protocol yields a different tag", NULL);

    struct rte_ipv6_hdr ip6 = {
        .payload_len = rte_cpu_to_be_16(4),
        .proto = IPPROTO_ICMPV6,
    };
    U32 unused_tag = 0;
    TEST_ASSERT(ipv6_flow_tag(&ip6, &unused_tag) == FALSE,
        "T6 non-TCP/UDP packets carry no flow tag", NULL);
}

void test_dp_ipv6(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    ppp_ccb_t *original_ccb = fastrg_ccb->ppp_ccb[0];
    unsigned int lcore_id = rte_lcore_id();
    struct per_ccb_stats *saved_lan = fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT];
    struct per_ccb_stats *saved_wan = fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT];
    struct pppoes_lcore_stats *saved_pppoes = fastrg_ccb->pppoes_stats[lcore_id];

    g_fastrg_ccb = fastrg_ccb;
    memset(&test_ccb, 0, sizeof(test_ccb));
    test_ccb.fastrg_ccb = fastrg_ccb;
    test_ccb.user_num = 1;
    test_ccb.session_id = rte_cpu_to_be_16(TEST_SESSION);
    rte_atomic16_init(&test_ccb.vlan_id);
    rte_atomic16_set(&test_ccb.vlan_id, TEST_VLAN_ID);
    rte_atomic16_init(&test_ccb.ipv6_dp_bool);
    rte_ether_addr_copy(&bras_mac, &test_ccb.PPP_dst_mac);
    memcpy(test_ccb.hsi_ipv6_lan_prefix, lan_prefix, sizeof(lan_prefix));
    test_ccb.nd6_table = nd6_table_alloc(60001, NULL);
    if (test_ccb.nd6_table == NULL) {
        fprintf(stderr, "IPv6 data-plane fixture allocation failed\n");
        assert(test_ccb.nd6_table != NULL);
    }
    fastrg_ccb->ppp_ccb[0] = &test_ccb;
    /* The transforms bump per-lcore counters; this mock has no rows, so the
     * getters must return NULL instead of an uninitialized pointer. */
    fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT] = NULL;
    fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT] = NULL;
    fastrg_ccb->pppoes_stats[lcore_id] = NULL;

    test_lan_classification();
    test_lan_to_wan_transform();
    test_packet_too_big();
    test_wan_classification();
    test_wan_to_lan_transform();
    test_flow_tag();

    fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT] = saved_lan;
    fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT] = saved_wan;
    fastrg_ccb->pppoes_stats[lcore_id] = saved_pppoes;
    nd6_table_free(test_ccb.nd6_table);
    fastrg_ccb->ppp_ccb[0] = original_ccb;
    *total_tests += test_count;
    *total_pass += pass_count;
}
