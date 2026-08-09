#include <stdio.h>
#include <string.h>

#include <common.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip6.h>
#include <rte_lcore.h>
#include <rte_timer.h>
#include <rte_udp.h>

#include "../../src/dhcp6/dhcp6.h"
#include "../../src/protocol.h"
#include "../../src/pppd/header.h"
#include "../test_helper.h"

static int test_count = 0;
static int pass_count = 0;
static ppp_ccb_t test_ccb;
static FastRG_t *g_fastrg_ccb;
static BOOL timer_initialized = FALSE;

static const U8 server_duid[DHCP6_DUID_LL_LEN] = {
    0x00, 0x03, 0x00, 0x01, 0x9c, 0x69, 0xb4, 0x61, 0x16, 0xde,
};
static const U8 delegated_prefix[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const U8 dns_servers[2][16] = {
    {0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x11},
    {0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x88},
};

static void write_be16(U8 *p, U16 value)
{
    value = rte_cpu_to_be_16(value);
    memcpy(p, &value, sizeof(value));
}

static void write_be32(U8 *p, U32 value)
{
    value = rte_cpu_to_be_32(value);
    memcpy(p, &value, sizeof(value));
}

static U16 read_be16(const U8 *p)
{
    U16 value;

    memcpy(&value, p, sizeof(value));
    return rte_be_to_cpu_16(value);
}

static void append_option(U8 **cursor, U16 code, const U8 *value, U16 len)
{
    write_be16(*cursor, code);
    write_be16(*cursor + 2, len);
    if (len > 0)
        memcpy(*cursor + 4, value, len);
    *cursor += 4 + len;
}

static void build_client_duid(U8 duid[DHCP6_DUID_LL_LEN])
{
    write_be16(&duid[0], 3);
    write_be16(&duid[2], 1);
    memcpy(&duid[4], g_fastrg_ccb->nic_info.hsi_wan_src_mac.addr_bytes,
        RTE_ETHER_ADDR_LEN);
}

static void reset_ccb(void)
{
    if (timer_initialized)
        rte_timer_stop_sync(&test_ccb.dhcp6_timer);
    timer_initialized = TRUE;

    memset(&test_ccb, 0, sizeof(test_ccb));
    test_ccb.fastrg_ccb = g_fastrg_ccb;
    test_ccb.user_num = 7;
    test_ccb.session_id = rte_cpu_to_be_16(0x1234);
    rte_atomic16_init(&test_ccb.vlan_id);
    rte_atomic16_set(&test_ccb.vlan_id, 321);
    test_ccb.PPP_dst_mac.addr_bytes[0] = 0x02;
    test_ccb.PPP_dst_mac.addr_bytes[1] = 0xaa;
    test_ccb.PPP_dst_mac.addr_bytes[2] = 0xbb;
    test_ccb.PPP_dst_mac.addr_bytes[3] = 0xcc;
    test_ccb.PPP_dst_mac.addr_bytes[4] = 0xdd;
    test_ccb.PPP_dst_mac.addr_bytes[5] = 0xee;
    const U8 iid[8] = {0x9e, 0x69, 0xb4, 0xff, 0xfe, 0x61, 0x16, 0xdd};
    memcpy(test_ccb.ipv6cp_local_iid, iid, sizeof(iid));
    test_ccb.ipv6_enabled = TRUE;
    test_ccb.ipv6cp_up = TRUE;
    rte_timer_init(&test_ccb.dhcp6_timer);
}

static U8 *packet_dhcp(U8 *packet)
{
    return packet + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        sizeof(pppoe_header_t) + sizeof(ppp_payload_t) +
        sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_udp_hdr);
}

static const U8 *find_option(const U8 *dhcp, U16 len, U16 wanted,
    U16 *value_len)
{
    const U8 *cursor = dhcp + 4;
    U16 remaining = len - 4;

    while (remaining >= 4) {
        U16 code = read_be16(cursor);
        U16 opt_len = read_be16(cursor + 2);
        if (opt_len > remaining - 4)
            return NULL;
        if (code == wanted) {
            *value_len = opt_len;
            return cursor + 4;
        }
        cursor += 4 + opt_len;
        remaining -= 4 + opt_len;
    }
    return NULL;
}

static U16 build_expected_client_packet(U8 msg_type, U8 *packet)
{
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)packet;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    pppoe_header_t *pppoe = (pppoe_header_t *)(vlan + 1);
    ppp_payload_t *ppp = (ppp_payload_t *)(pppoe + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(ppp + 1);
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip6 + 1);
    U8 *dhcp = (U8 *)(udp + 1);
    U8 *cursor = dhcp;
    U8 client_duid[DHCP6_DUID_LL_LEN];
    U8 ia_pd[12];

    memset(packet, 0, DHCP6_PACKET_MAX_LEN);
    rte_ether_addr_copy(&g_fastrg_ccb->nic_info.hsi_wan_src_mac,
        &eth->src_addr);
    rte_ether_addr_copy(&test_ccb.PPP_dst_mac, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(VLAN);
    vlan->tci_union.tci_value = rte_cpu_to_be_16(321);
    vlan->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);
    pppoe->ver_type = VER_TYPE;
    pppoe->session_id = test_ccb.session_id;
    ppp->ppp_protocol = rte_cpu_to_be_16(PPP_IPV6_PROTOCOL);

    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(6) << 28);
    ip6->proto = IPPROTO_UDP;
    ip6->hop_limits = 1;
    U8 *src = (U8 *)&ip6->src_addr;
    U8 *dst = (U8 *)&ip6->dst_addr;
    src[0] = 0xfe;
    src[1] = 0x80;
    memcpy(src + 8, test_ccb.ipv6cp_local_iid, 8);
    dst[0] = 0xff;
    dst[1] = 0x02;
    dst[13] = 0x01;
    dst[15] = 0x02;

    udp->src_port = rte_cpu_to_be_16(DHCP6_CLIENT_PORT);
    udp->dst_port = rte_cpu_to_be_16(DHCP6_SERVER_PORT);
    *cursor++ = msg_type;
    memcpy(cursor, test_ccb.dhcp6_xid, 3);
    cursor += 3;
    build_client_duid(client_duid);
    append_option(&cursor, DHCP6_OPT_CLIENTID, client_duid,
        sizeof(client_duid));
    if (msg_type != DHCP6_SOLICIT)
        append_option(&cursor, DHCP6_OPT_SERVERID, server_duid,
            sizeof(server_duid));
    write_be32(&ia_pd[0], test_ccb.user_num);
    write_be32(&ia_pd[4], 0);
    write_be32(&ia_pd[8], 0);
    append_option(&cursor, DHCP6_OPT_IA_PD, ia_pd, sizeof(ia_pd));

    U16 dhcp_len = (U16)(cursor - dhcp);
    U16 udp_len = sizeof(*udp) + dhcp_len;
    ip6->payload_len = rte_cpu_to_be_16(udp_len);
    udp->dgram_len = rte_cpu_to_be_16(udp_len);
    udp->dgram_cksum = 0;
    udp->dgram_cksum = rte_ipv6_udptcp_cksum(ip6, udp);
    U16 pppoe_len = sizeof(*ppp) + sizeof(*ip6) + udp_len;
    pppoe->length = rte_cpu_to_be_16(pppoe_len);
    return sizeof(*eth) + sizeof(*vlan) + sizeof(*pppoe) + pppoe_len;
}

static U16 build_server_response(U8 *packet, U8 msg_type, const U8 xid[3],
    BOOL include_server, BOOL client_matches, U8 prefix_plen, BOOL no_prefix,
    U16 dns_len, BOOL malformed)
{
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)packet;
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip6 + 1);
    U8 *dhcp = (U8 *)(udp + 1);
    U8 *cursor = dhcp;
    U8 client_duid[DHCP6_DUID_LL_LEN];
    U8 ia_pd[64] = {0};
    U8 *ia_cursor = ia_pd + 12;
    U8 dns_value[48] = {0};
    U8 *last_option = NULL;

    memset(packet, 0, DHCP6_PACKET_MAX_LEN);
    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(6) << 28);
    ip6->proto = IPPROTO_UDP;
    ip6->hop_limits = 64;
    U8 *src = (U8 *)&ip6->src_addr;
    U8 *dst = (U8 *)&ip6->dst_addr;
    src[0] = 0xfe;
    src[1] = 0x80;
    memcpy(src + 8, test_ccb.ipv6cp_peer_iid, 8);
    dst[0] = 0xfe;
    dst[1] = 0x80;
    memcpy(dst + 8, test_ccb.ipv6cp_local_iid, 8);
    udp->src_port = rte_cpu_to_be_16(DHCP6_SERVER_PORT);
    udp->dst_port = rte_cpu_to_be_16(DHCP6_CLIENT_PORT);

    *cursor++ = msg_type;
    memcpy(cursor, xid, 3);
    cursor += 3;
    if (include_server)
        append_option(&cursor, DHCP6_OPT_SERVERID, server_duid,
            sizeof(server_duid));
    build_client_duid(client_duid);
    if (!client_matches)
        client_duid[9] ^= 0xff;
    append_option(&cursor, DHCP6_OPT_CLIENTID, client_duid,
        sizeof(client_duid));

    write_be32(&ia_pd[0], test_ccb.user_num);
    write_be32(&ia_pd[4], DHCP6_DEFAULT_T1);
    write_be32(&ia_pd[8], 34560);
    if (no_prefix) {
        U8 status[2];
        write_be16(status, DHCP6_STATUS_NO_PREFIX);
        append_option(&ia_cursor, DHCP6_OPT_STATUS, status, sizeof(status));
    } else {
        U8 ia_prefix[25] = {0};
        write_be32(&ia_prefix[0], 43200);
        write_be32(&ia_prefix[4], 86400);
        ia_prefix[8] = prefix_plen;
        memcpy(&ia_prefix[9], delegated_prefix, 16);
        append_option(&ia_cursor, DHCP6_OPT_IAPREFIX, ia_prefix,
            sizeof(ia_prefix));
    }
    last_option = cursor;
    append_option(&cursor, DHCP6_OPT_IA_PD, ia_pd,
        (U16)(ia_cursor - ia_pd));
    if (dns_len > 0) {
        memcpy(dns_value, dns_servers, sizeof(dns_servers));
        last_option = cursor;
        append_option(&cursor, DHCP6_OPT_DNS_SERVERS, dns_value, dns_len);
    }
    if (malformed)
        write_be16(last_option + 2, UINT16_MAX);

    U16 dhcp_len = (U16)(cursor - dhcp);
    U16 udp_len = sizeof(*udp) + dhcp_len;
    ip6->payload_len = rte_cpu_to_be_16(udp_len);
    udp->dgram_len = rte_cpu_to_be_16(udp_len);
    udp->dgram_cksum = 0;
    udp->dgram_cksum = rte_ipv6_udptcp_cksum(ip6, udp);
    return sizeof(*ip6) + udp_len;
}

static void test_t1_solicit_bytes(void)
{
    U8 actual[DHCP6_PACKET_MAX_LEN];
    U8 expected[DHCP6_PACKET_MAX_LEN];
    U16 actual_len = 0;

    printf("\nT1: Solicit bytes match the BRAS contract:\n");
    reset_ccb();
    test_ccb.dhcp6_xid[0] = 0x01;
    test_ccb.dhcp6_xid[1] = 0x02;
    test_ccb.dhcp6_xid[2] = 0x03;
    U16 expected_len = build_expected_client_packet(DHCP6_SOLICIT, expected);
    STATUS ret = dhcp6_build_packet(&test_ccb, DHCP6_SOLICIT, actual,
        &actual_len);

    TEST_ASSERT(ret == SUCCESS && actual_len == expected_len,
        "Solicit builder returns the expected full-frame length", "got %u/%u",
        actual_len, expected_len);
    TEST_ASSERT(memcmp(actual, expected, expected_len) == 0,
        "Solicit full-frame bytes match the BRAS fixture", NULL);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(actual +
        sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        sizeof(pppoe_header_t) + sizeof(ppp_payload_t));
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip6 + 1);
    TEST_ASSERT(udp->dgram_cksum != 0 &&
            rte_ipv6_udptcp_cksum_verify(ip6, udp) == 0,
        "Solicit carries a valid non-zero IPv6 UDP checksum", NULL);
    U16 dhcp_len = rte_be_to_cpu_16(udp->dgram_len) - sizeof(*udp);
    U16 opt_len = 0;
    TEST_ASSERT(find_option((U8 *)(udp + 1), dhcp_len,
            DHCP6_OPT_SERVERID, &opt_len) == NULL,
        "Solicit does not contain SERVERID", NULL);
}

static void test_t2_request_renew_serverid(void)
{
    U8 packet[DHCP6_PACKET_MAX_LEN];
    U8 expected[DHCP6_PACKET_MAX_LEN];
    U16 len;

    printf("\nT2: Request and Renew echo SERVERID:\n");
    reset_ccb();
    memcpy(test_ccb.dhcp6_xid, (U8[]){0x10, 0x20, 0x30}, 3);
    memcpy(test_ccb.dhcp6_server_duid, server_duid, sizeof(server_duid));
    test_ccb.dhcp6_server_duid_len = sizeof(server_duid);

    U16 expected_len = build_expected_client_packet(DHCP6_REQUEST, expected);
    TEST_ASSERT(dhcp6_build_packet(&test_ccb, DHCP6_REQUEST, packet, &len) ==
            SUCCESS && len == expected_len &&
            memcmp(packet, expected, len) == 0,
        "Request full-frame bytes include the BRAS SERVERID", NULL);
    U8 *dhcp = packet_dhcp(packet);
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(dhcp - sizeof(*udp));
    U16 dhcp_len = rte_be_to_cpu_16(udp->dgram_len) - sizeof(*udp);
    U16 opt_len = 0;
    const U8 *value = find_option(dhcp, dhcp_len, DHCP6_OPT_SERVERID,
        &opt_len);
    TEST_ASSERT(value != NULL && opt_len == sizeof(server_duid) &&
            memcmp(value, server_duid, sizeof(server_duid)) == 0,
        "Request SERVERID equals the Advertise DUID", NULL);

    TEST_ASSERT(dhcp6_build_packet(&test_ccb, DHCP6_RENEW, packet, &len) ==
            SUCCESS && packet_dhcp(packet)[0] == DHCP6_RENEW,
        "Renew uses message type 5", NULL);
    dhcp = packet_dhcp(packet);
    udp = (struct rte_udp_hdr *)(dhcp - sizeof(*udp));
    dhcp_len = rte_be_to_cpu_16(udp->dgram_len) - sizeof(*udp);
    value = find_option(dhcp, dhcp_len, DHCP6_OPT_SERVERID, &opt_len);
    TEST_ASSERT(value != NULL && opt_len == sizeof(server_duid) &&
            memcmp(value, server_duid, sizeof(server_duid)) == 0,
        "Renew echoes the same SERVERID", NULL);
}

static void test_t3_advertise_validation(void)
{
    U8 packet[DHCP6_PACKET_MAX_LEN];
    U16 len;

    printf("\nT3: Advertise validation and state transition:\n");
    reset_ccb();
    memcpy(test_ccb.dhcp6_xid, (U8[]){0xaa, 0xbb, 0xcc}, 3);
    test_ccb.dhcp6_state = DHCP6_S_SOLICITING;
    len = build_server_response(packet, DHCP6_ADVERTISE,
        test_ccb.dhcp6_xid, TRUE, TRUE, DHCP6_PD_PLEN, FALSE, 32, FALSE);
    dhcp6_wan_input(&test_ccb, packet, len);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_REQUESTING &&
            test_ccb.dhcp6_server_duid_len == sizeof(server_duid) &&
            memcmp(test_ccb.dhcp6_server_duid, server_duid,
                sizeof(server_duid)) == 0,
        "valid Advertise enters REQUESTING and stores SERVERID", NULL);

    reset_ccb();
    memcpy(test_ccb.dhcp6_xid, (U8[]){0xaa, 0xbb, 0xcc}, 3);
    test_ccb.dhcp6_state = DHCP6_S_SOLICITING;
    U8 wrong_xid[3] = {0xaa, 0xbb, 0xcd};
    len = build_server_response(packet, DHCP6_ADVERTISE, wrong_xid,
        TRUE, TRUE, DHCP6_PD_PLEN, FALSE, 32, FALSE);
    dhcp6_wan_input(&test_ccb, packet, len);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_SOLICITING,
        "Advertise with a different xid is ignored", NULL);

    len = build_server_response(packet, DHCP6_ADVERTISE,
        test_ccb.dhcp6_xid, TRUE, FALSE, DHCP6_PD_PLEN, FALSE, 32, FALSE);
    dhcp6_wan_input(&test_ccb, packet, len);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_SOLICITING,
        "Advertise with a different CLIENTID is ignored", NULL);

    len = build_server_response(packet, DHCP6_ADVERTISE,
        test_ccb.dhcp6_xid, FALSE, TRUE, DHCP6_PD_PLEN, FALSE, 32, FALSE);
    dhcp6_wan_input(&test_ccb, packet, len);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_SOLICITING,
        "Advertise without SERVERID is ignored", NULL);
}

static void prepare_reply_state(U8 state)
{
    reset_ccb();
    memcpy(test_ccb.dhcp6_xid, (U8[]){0x44, 0x55, 0x66}, 3);
    memcpy(test_ccb.dhcp6_server_duid, server_duid, sizeof(server_duid));
    test_ccb.dhcp6_server_duid_len = sizeof(server_duid);
    test_ccb.dhcp6_state = state;
}

static void test_t4_reply_binding_and_t1(void)
{
    U8 packet[DHCP6_PACKET_MAX_LEN];

    printf("\nT4: Reply commits the delegated lease:\n");
    prepare_reply_state(DHCP6_S_REQUESTING);
    U16 len = build_server_response(packet, DHCP6_REPLY,
        test_ccb.dhcp6_xid, TRUE, TRUE, DHCP6_PD_PLEN, FALSE, 32, FALSE);
    dhcp6_wan_input(&test_ccb, packet, len);

    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_BOUND &&
            test_ccb.dhcp6_pd_ready == TRUE,
        "valid Reply enters BOUND and publishes readiness", NULL);
    TEST_ASSERT(test_ccb.hsi_ipv6_pd_plen == DHCP6_PD_PLEN &&
            memcmp(test_ccb.hsi_ipv6_pd_prefix, delegated_prefix, 16) == 0,
        "Reply stores the /56 delegated prefix", NULL);
    TEST_ASSERT(memcmp(test_ccb.hsi_ipv6_lan_prefix, delegated_prefix, 8) == 0 &&
            memcmp(test_ccb.hsi_ipv6_dns, dns_servers, sizeof(dns_servers)) == 0,
        "Reply derives the first /64 and stores both DNS servers", NULL);
    TEST_ASSERT(test_ccb.dhcp6_t1 == DHCP6_DEFAULT_T1,
        "Reply stores BRAS T1=21600", "got %u", test_ccb.dhcp6_t1);

    dhcp6_timer_cb(&test_ccb.dhcp6_timer, &test_ccb);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_RENEWING,
        "T1 timer callback enters RENEWING", NULL);
}

static void test_t5_lan_prefix_derivation(void)
{
    U8 lan[16];
    U8 prefix60[16] = {
        0x20, 0x01, 0x0d, 0xb8, 0xaa, 0xbb, 0xcc, 0xdf,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };

    printf("\nT5: delegated prefixes derive the first LAN /64:\n");
    dhcp6_derive_lan_prefix(delegated_prefix, 56, lan);
    TEST_ASSERT(memcmp(lan, delegated_prefix, 8) == 0 &&
            memcmp(lan + 8, (U8[8]){0}, 8) == 0,
        "/56 derives the expected first /64", NULL);
    dhcp6_derive_lan_prefix(prefix60, 60, lan);
    TEST_ASSERT(memcmp(lan, prefix60, 7) == 0 && lan[7] == 0xd0 &&
            memcmp(lan + 8, (U8[8]){0}, 8) == 0,
        "/60 preserves its high nibble and clears the remaining host bits",
        NULL);
}

static void test_t6_defensive_parsing(void)
{
    U8 packet[DHCP6_PACKET_MAX_LEN];
    U16 len;

    printf("\nT6: malformed and unusable leases are rejected safely:\n");
    prepare_reply_state(DHCP6_S_REQUESTING);
    len = build_server_response(packet, DHCP6_REPLY, test_ccb.dhcp6_xid,
        TRUE, TRUE, 64, FALSE, 32, FALSE);
    dhcp6_wan_input(&test_ccb, packet, len);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_REQUESTING &&
            test_ccb.dhcp6_pd_ready == FALSE,
        "IAPREFIX with plen other than 56 is rejected", NULL);

    len = build_server_response(packet, DHCP6_REPLY, test_ccb.dhcp6_xid,
        TRUE, TRUE, DHCP6_PD_PLEN, FALSE, 32, TRUE);
    dhcp6_wan_input(&test_ccb, packet, len);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_REQUESTING,
        "overflowing option length is rejected without changing state", NULL);

    len = build_server_response(packet, DHCP6_REPLY, test_ccb.dhcp6_xid,
        TRUE, TRUE, DHCP6_PD_PLEN, FALSE, 17, FALSE);
    dhcp6_wan_input(&test_ccb, packet, len);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_REQUESTING,
        "DNS option whose length is not a multiple of 16 is rejected", NULL);

    test_ccb.hsi_ipv6_pd_prefix[0] = 0xff;
    len = build_server_response(packet, DHCP6_REPLY, test_ccb.dhcp6_xid,
        TRUE, TRUE, DHCP6_PD_PLEN, TRUE, 32, FALSE);
    dhcp6_wan_input(&test_ccb, packet, len);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_SOLICITING &&
            test_ccb.dhcp6_pd_ready == FALSE &&
            test_ccb.hsi_ipv6_pd_prefix[0] == 0,
        "NoPrefixAvail clears lease data and schedules a slow Solicit retry",
        NULL);
}

static void test_t7_renew_serverid_validation(void)
{
    U8 packet[DHCP6_PACKET_MAX_LEN];

    printf("\nT7: Renew Reply must come from the selected server:\n");
    prepare_reply_state(DHCP6_S_RENEWING);
    U16 len = build_server_response(packet, DHCP6_REPLY,
        test_ccb.dhcp6_xid, TRUE, TRUE, DHCP6_PD_PLEN, FALSE, 32, FALSE);
    U8 *dhcp = (U8 *)((struct rte_udp_hdr *)
        ((struct rte_ipv6_hdr *)packet + 1) + 1);
    U16 opt_len = 0;
    U8 *value = (U8 *)find_option(dhcp,
        rte_be_to_cpu_16(((struct rte_udp_hdr *)
            ((struct rte_ipv6_hdr *)packet + 1))->dgram_len) -
            sizeof(struct rte_udp_hdr), DHCP6_OPT_SERVERID, &opt_len);
    value[opt_len - 1] ^= 0xff;
    dhcp6_wan_input(&test_ccb, packet, len);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_RENEWING &&
            test_ccb.dhcp6_pd_ready == FALSE,
        "Renew Reply with a different SERVERID is ignored", NULL);
}

static void test_t8_silent_gate(void)
{
    U8 packet[DHCP6_PACKET_MAX_LEN];

    printf("\nT8: disabled or unopened IPv6 stays silent:\n");
    prepare_reply_state(DHCP6_S_REQUESTING);
    U16 len = build_server_response(packet, DHCP6_REPLY,
        test_ccb.dhcp6_xid, TRUE, TRUE, DHCP6_PD_PLEN, FALSE, 32, FALSE);
    test_ccb.ipv6_enabled = FALSE;
    dhcp6_wan_input(&test_ccb, packet, len);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_REQUESTING &&
            test_ccb.dhcp6_pd_ready == FALSE,
        "ipv6_enabled=false ignores inbound DHCPv6", NULL);
    test_ccb.ipv6_enabled = TRUE;
    test_ccb.ipv6cp_up = FALSE;
    dhcp6_wan_input(&test_ccb, packet, len);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_REQUESTING &&
            test_ccb.dhcp6_pd_ready == FALSE,
        "IPV6CP not opened ignores inbound DHCPv6", NULL);
}

static void test_t9_stop_idempotence(void)
{
    printf("\nT9: stop clears every delegated field and is idempotent:\n");
    reset_ccb();
    test_ccb.dhcp6_state = DHCP6_S_BOUND;
    test_ccb.dhcp6_pd_ready = TRUE;
    test_ccb.dhcp6_t1 = DHCP6_DEFAULT_T1;
    test_ccb.dhcp6_server_duid_len = sizeof(server_duid);
    memset(test_ccb.dhcp6_server_duid, 0xaa,
        sizeof(test_ccb.dhcp6_server_duid));
    memset(test_ccb.hsi_ipv6_pd_prefix, 0xaa,
        sizeof(test_ccb.hsi_ipv6_pd_prefix));
    memset(test_ccb.hsi_ipv6_lan_prefix, 0xbb,
        sizeof(test_ccb.hsi_ipv6_lan_prefix));
    memset(test_ccb.hsi_ipv6_dns, 0xcc, sizeof(test_ccb.hsi_ipv6_dns));
    test_ccb.hsi_ipv6_pd_plen = DHCP6_PD_PLEN;

    dhcp6_pd_stop(&test_ccb);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_IDLE &&
            test_ccb.dhcp6_pd_ready == FALSE && test_ccb.dhcp6_t1 == 0 &&
            test_ccb.dhcp6_server_duid_len == 0 &&
            test_ccb.hsi_ipv6_pd_plen == 0,
        "stop returns the client to an empty IDLE state", NULL);
    TEST_ASSERT(memcmp(test_ccb.hsi_ipv6_pd_prefix, (U8[16]){0}, 16) == 0 &&
            memcmp(test_ccb.hsi_ipv6_lan_prefix, (U8[16]){0}, 16) == 0 &&
            memcmp(test_ccb.hsi_ipv6_dns, (U8[32]){0}, 32) == 0,
        "stop clears delegated prefix, LAN prefix, and DNS", NULL);
    dhcp6_pd_stop(&test_ccb);
    TEST_ASSERT(test_ccb.dhcp6_state == DHCP6_S_IDLE &&
            test_ccb.dhcp6_pd_ready == FALSE,
        "a second stop is harmless", NULL);
}

void test_dhcp6(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           DHCPv6-PD Client Unit Tests                     ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;
    g_fastrg_ccb = fastrg_ccb;
    fastrg_ccb->lcore.ctrl_thread = rte_lcore_id();
    rte_timer_subsystem_init();

    test_t1_solicit_bytes();
    test_t2_request_renew_serverid();
    test_t3_advertise_validation();
    test_t4_reply_binding_and_t1();
    test_t5_lan_prefix_derivation();
    test_t6_defensive_parsing();
    test_t7_renew_serverid_validation();
    test_t8_silent_gate();
    test_t9_stop_idempotence();

    rte_timer_stop_sync(&test_ccb.dhcp6_timer);
    printf("\nDHCPv6-PD tests: %d passed, %d failed\n", pass_count,
        test_count - pass_count);
    *total_tests += test_count;
    *total_pass += pass_count;
}
