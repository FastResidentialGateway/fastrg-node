#include <stddef.h>
#include <string.h>

#include <common.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_udp.h>

#include "../src/dp.h"
#include "../src/fastrg.h"
#include "../src/protocol.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

struct mock_l2_packet {
    struct rte_mbuf mbuf;
    mbuf_priv_t priv;
    U8 data[sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t)];
} __rte_cache_aligned;

static void test_compute_flow_tag(void)
{
    U32 src_ip = rte_cpu_to_be_32(0xC0A80164);
    U32 dst_ip = rte_cpu_to_be_32(0x08080808);
    U16 src_port = rte_cpu_to_be_16(12345);
    U16 dst_port = rte_cpu_to_be_16(443);
    U8 proto = IPPROTO_TCP;
    U32 tag = compute_flow_tag(src_ip, dst_ip, src_port, dst_port, proto);

    printf("\nTesting compute_flow_tag function:\n");
    TEST_ASSERT(tag == compute_flow_tag(src_ip, dst_ip, src_port, dst_port, proto),
        "flow tag is deterministic", "same 5-tuple should produce the same tag");
    TEST_ASSERT(tag != compute_flow_tag(src_ip ^ rte_cpu_to_be_32(1), dst_ip, src_port, dst_port, proto),
        "flow tag changes with src_ip", "src_ip change should produce a different tag");
    TEST_ASSERT(tag != compute_flow_tag(src_ip, dst_ip ^ rte_cpu_to_be_32(1), src_port, dst_port, proto),
        "flow tag changes with dst_ip", "dst_ip change should produce a different tag");
    TEST_ASSERT(tag != compute_flow_tag(src_ip, dst_ip, src_port ^ rte_cpu_to_be_16(1), dst_port, proto),
        "flow tag changes with src_port", "src_port change should produce a different tag");
    TEST_ASSERT(tag != compute_flow_tag(src_ip, dst_ip, src_port, dst_port ^ rte_cpu_to_be_16(1), proto),
        "flow tag changes with dst_port", "dst_port change should produce a different tag");
    TEST_ASSERT(tag != compute_flow_tag(src_ip, dst_ip, src_port, dst_port, IPPROTO_UDP),
        "flow tag changes with protocol", "protocol change should produce a different tag");
    TEST_ASSERT(tag != compute_flow_tag(dst_ip, src_ip, dst_port, src_port, proto),
        "flow tag is directional", "reversed direction should produce a different tag");
}

static void set_mock_vlan(struct mock_l2_packet *packet, U16 vlan_id)
{
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)packet->data;
    vlan_header_t *vlan_hdr = (vlan_header_t *)(eth_hdr + 1);

    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);
    vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(vlan_id);
}

static void test_parse_l2_hdr(FastRG_t *fastrg_ccb)
{
    rte_atomic16_t vlan_map[MAX_VLAN_ID];
    struct mock_l2_packet packet;
    rte_atomic16_t *old_vlan_map = fastrg_ccb->vlan_userid_map;
    U16 old_user_count = fastrg_ccb->user_count;
    dhcp_ccb_t *dhcp_ccb = fastrg_ccb->dhcp_ccb[0];
    U32 old_server_ip = dhcp_ccb->dhcp_server_ip;
    U32 old_subnet_mask = dhcp_ccb->subnet_mask;
    U32 server_ip = rte_cpu_to_be_32(0xC0A80101);
    U32 subnet_mask = rte_cpu_to_be_32(0xFFFFFF00);
    struct rte_ether_hdr *eth_hdr;
    vlan_header_t *vlan_hdr;

    printf("\nTesting parse_l2_hdr function:\n");

    memset(vlan_map, 0, sizeof(vlan_map));
    memset(&packet, 0, sizeof(packet));
    packet.mbuf.buf_addr = packet.data;
    packet.mbuf.buf_len = sizeof(packet.data);
    packet.mbuf.data_off = 0;
    packet.mbuf.pkt_len = sizeof(packet.data);
    packet.mbuf.data_len = sizeof(packet.data);
    eth_hdr = (struct rte_ether_hdr *)packet.data;
    vlan_hdr = (vlan_header_t *)(eth_hdr + 1);

    TEST_ASSERT((void *)rte_mbuf_to_priv(&packet.mbuf) == (void *)&packet.priv,
        "mock mbuf private area layout", "private area should immediately follow rte_mbuf");

    fastrg_ccb->vlan_userid_map = vlan_map;
    fastrg_ccb->user_count = 1;
    dhcp_ccb->dhcp_server_ip = server_ip;
    dhcp_ccb->subnet_mask = subnet_mask;

    eth_hdr->ether_type = rte_cpu_to_be_16(FRAME_TYPE_IP);
    TEST_ASSERT(parse_l2_hdr(fastrg_ccb, &packet.mbuf, LAN_PORT) == ERROR,
        "non-VLAN frame is rejected", "ether_type other than VLAN should return ERROR");

    set_mock_vlan(&packet, MIN_VLAN_ID - 1);
    TEST_ASSERT(parse_l2_hdr(fastrg_ccb, &packet.mbuf, LAN_PORT) == ERROR,
        "VLAN below minimum is rejected", "VLAN %u should return ERROR", MIN_VLAN_ID - 1);

    rte_atomic16_set(&vlan_map[MIN_VLAN_ID - 1], 0);
    set_mock_vlan(&packet, MIN_VLAN_ID);
    TEST_ASSERT(parse_l2_hdr(fastrg_ccb, &packet.mbuf, LAN_PORT) == SUCCESS,
        "minimum VLAN is accepted", "VLAN %u should return SUCCESS", MIN_VLAN_ID);

    rte_atomic16_set(&vlan_map[MAX_VLAN_ID - 1], 0);
    set_mock_vlan(&packet, MAX_VLAN_ID);
    TEST_ASSERT(parse_l2_hdr(fastrg_ccb, &packet.mbuf, WAN_PORT) == SUCCESS,
        "maximum VLAN is accepted", "VLAN %u should return SUCCESS", MAX_VLAN_ID);

    set_mock_vlan(&packet, MAX_VLAN_ID + 1);
    TEST_ASSERT(parse_l2_hdr(fastrg_ccb, &packet.mbuf, WAN_PORT) == ERROR,
        "VLAN above maximum is rejected", "VLAN %u should return ERROR", MAX_VLAN_ID + 1);

    rte_atomic16_set(&vlan_map[MIN_VLAN_ID - 1], fastrg_ccb->user_count);
    set_mock_vlan(&packet, MIN_VLAN_ID);
    TEST_ASSERT(parse_l2_hdr(fastrg_ccb, &packet.mbuf, LAN_PORT) == ERROR,
        "out-of-range subscriber is rejected", "ccb_id equal to user_count should return ERROR");

    rte_atomic16_set(&vlan_map[MIN_VLAN_ID - 1], 0);
    memset(&packet.priv, 0, sizeof(packet.priv));
    TEST_ASSERT(parse_l2_hdr(fastrg_ccb, &packet.mbuf, LAN_PORT) == SUCCESS,
        "valid VLAN frame is accepted", "valid frame should return SUCCESS");
    TEST_ASSERT(packet.priv.ccb_id == 0, "parse_l2_hdr stores ccb_id", "expected ccb_id 0");
    TEST_ASSERT(packet.priv.dhcp_server_ip == server_ip,
        "parse_l2_hdr stores DHCP server IP", "DHCP server IP should match subscriber state");
    TEST_ASSERT(packet.priv.dhcp_subnet_mask == subnet_mask,
        "parse_l2_hdr stores DHCP subnet mask", "subnet mask should match subscriber state");
    TEST_ASSERT(packet.priv.eth_hdr == eth_hdr,
        "parse_l2_hdr stores Ethernet header", "Ethernet header pointer should reference packet data");
    TEST_ASSERT(packet.priv.vlan_hdr == vlan_hdr,
        "parse_l2_hdr stores VLAN header", "VLAN header pointer should reference packet data");

    fastrg_ccb->vlan_userid_map = old_vlan_map;
    fastrg_ccb->user_count = old_user_count;
    dhcp_ccb->dhcp_server_ip = old_server_ip;
    dhcp_ccb->subnet_mask = old_subnet_mask;
}

static void test_is_iptv_pkt_need_drop(FastRG_t *fastrg_ccb)
{
    U8 packet[sizeof(vlan_header_t) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr)] = {0};
    vlan_header_t *vlan_hdr = (vlan_header_t *)packet;
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(vlan_hdr + 1);
    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);

    printf("\nTesting is_iptv_pkt_need_drop function:\n");

    vlan_hdr->next_proto = rte_cpu_to_be_16(FRAME_TYPE_ARP);
    TEST_ASSERT(is_iptv_pkt_need_drop(fastrg_ccb, vlan_hdr) == TRUE,
        "non-IP IPTV packet is dropped", "non-IP next_proto should return TRUE");

    vlan_hdr->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IP);
    ip_hdr->next_proto_id = IPPROTO_IGMP;
    TEST_ASSERT(is_iptv_pkt_need_drop(fastrg_ccb, vlan_hdr) == FALSE,
        "IGMP packet is passed", "IGMP should return FALSE");

    ip_hdr->next_proto_id = IPPROTO_UDP;
    vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(MULTICAST_TAG);
    TEST_ASSERT(is_iptv_pkt_need_drop(fastrg_ccb, vlan_hdr) == FALSE,
        "multicast-tagged UDP packet is passed", "VLAN 4001 UDP should return FALSE");

    vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(MIN_VLAN_ID);
    ip_hdr->dst_addr = rte_cpu_to_be_32(0x0A010203);
    TEST_ASSERT(is_iptv_pkt_need_drop(fastrg_ccb, vlan_hdr) == FALSE,
        "10/8 VOD UDP packet is passed", "10.x.x.x UDP should return FALSE");

    ip_hdr->dst_addr = rte_cpu_to_be_32(0xC0A80164);
    ip_hdr->total_length = rte_cpu_to_be_16(256);
    udp_hdr->dst_port = rte_cpu_to_be_16(DHCP_CLIENT_PORT);
    TEST_ASSERT(is_iptv_pkt_need_drop(fastrg_ccb, vlan_hdr) == FALSE,
        "DHCP UDP packet with large payload is passed",
        "network-order total_length 256 should compare greater than IP+UDP headers");

    udp_hdr->dst_port = rte_cpu_to_be_16(12345);
    TEST_ASSERT(is_iptv_pkt_need_drop(fastrg_ccb, vlan_hdr) == TRUE,
        "ordinary UDP packet is dropped", "non-multicast, non-VOD, non-DHCP UDP should return TRUE");

    ip_hdr->next_proto_id = IPPROTO_TCP;
    TEST_ASSERT(is_iptv_pkt_need_drop(fastrg_ccb, vlan_hdr) == TRUE,
        "TCP IPTV packet is dropped", "TCP should return TRUE");
}

static struct rte_mbuf *alloc_test_mbuf(struct rte_mempool *pool, U16 ccb_id, U32 pkt_len)
{
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(pool);

    TEST_ASSERT(mbuf != NULL, "send2cp test mbuf allocation", "rte_pktmbuf_alloc should succeed");
    mbuf->pkt_len = pkt_len;
    mbuf->data_len = pkt_len;
    ((mbuf_priv_t *)rte_mbuf_to_priv(mbuf))->ccb_id = ccb_id;
    return mbuf;
}

static void test_send2cp(FastRG_t *fastrg_ccb)
{
    struct rte_ring *old_cp_q = fastrg_ccb->cp_q;
    struct rte_ring *old_free_mail_ring = fastrg_ccb->free_mail_ring;
    struct rte_ring *cp_q;
    struct rte_ring *free_mail_ring;
    struct rte_mempool *pool;
    tFastRG_MBX mail_slot = {0};
    tFastRG_MBX fillers[3] = {0};
    tFastRG_MBX *received = NULL;
    void *object = NULL;
    unsigned int lcore_id = rte_lcore_id();
    BOOL old_persistent;
    struct per_ccb_stats *old_lan_stats;
    struct per_ccb_stats *old_wan_stats;
    unsigned int available_before;
    unsigned int free_count_before;
    struct rte_mbuf *mbuf;

    printf("\nTesting send2cp function:\n");

    TEST_ASSERT(lcore_id != LCORE_ID_ANY, "send2cp runs on an EAL lcore", "unit test should run on EAL lcore 0");
    old_persistent = fastrg_rcu_persistent[lcore_id];
    old_lan_stats = fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT];
    old_wan_stats = fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT];
    fastrg_rcu_persistent[lcore_id] = TRUE;
    fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT] = NULL;
    fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT] = NULL;

    cp_q = rte_ring_create("dp_test_cp_q", 4, rte_socket_id(), 0);
    free_mail_ring = rte_ring_create("dp_test_free_mail", 4, rte_socket_id(), 0);
    pool = rte_pktmbuf_pool_create("dp_test_mbuf_pool", 16, 0, sizeof(mbuf_priv_t),
        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    TEST_ASSERT(cp_q != NULL && free_mail_ring != NULL && pool != NULL,
        "send2cp test DPDK objects", "rings and mbuf pool should be created");

    fastrg_ccb->cp_q = cp_q;
    fastrg_ccb->free_mail_ring = free_mail_ring;
    rte_ring_enqueue(free_mail_ring, &mail_slot);

    available_before = rte_mempool_avail_count(pool);
    mbuf = alloc_test_mbuf(pool, 0, 128);
    send2cp(fastrg_ccb, mbuf, EV_DP_DNS, WAN_PORT);
    TEST_ASSERT(rte_ring_dequeue(cp_q, (void **)&received) == 0 && received == &mail_slot,
        "send2cp enqueues the mail slot", "cp_q should contain the pre-allocated slot");
    TEST_ASSERT(received->mbuf == mbuf, "send2cp stores mbuf pointer", "mbuf pointer should be unchanged");
    TEST_ASSERT(received->type == EV_DP_DNS, "send2cp stores event type", "event type should be EV_DP_DNS");
    TEST_ASSERT(received->len == 128, "send2cp stores packet length", "packet length should be 128");
    TEST_ASSERT(received->ccb_id == 0, "send2cp stores ccb_id", "ccb_id should be 0");
    TEST_ASSERT(received->port_id == WAN_PORT, "send2cp stores port_id", "port_id should be WAN_PORT");
    TEST_ASSERT(rte_mempool_avail_count(pool) == available_before - 1,
        "successful send2cp transfers mbuf ownership", "mbuf should remain allocated for control plane");
    rte_pktmbuf_free(received->mbuf);
    rte_ring_enqueue(free_mail_ring, received);

    for (U32 i = 0; i < RTE_DIM(fillers); i++)
        rte_ring_enqueue(cp_q, &fillers[i]);
    TEST_ASSERT(rte_ring_full(cp_q), "send2cp cp_q fixture is full", "capacity-three ring should be full");
    available_before = rte_mempool_avail_count(pool);
    free_count_before = rte_ring_count(free_mail_ring);
    mbuf = alloc_test_mbuf(pool, 0, 256);
    send2cp(fastrg_ccb, mbuf, EV_DP_DHCP, LAN_PORT);
    TEST_ASSERT(rte_ring_count(free_mail_ring) == free_count_before,
        "full cp_q returns mail slot", "free_mail_ring count should remain unchanged");
    TEST_ASSERT(rte_mempool_avail_count(pool) == available_before,
        "full cp_q frees mbuf", "dropped mbuf should return to its mempool");
    while (rte_ring_dequeue(cp_q, &object) == 0) {
    }

    rte_ring_dequeue(free_mail_ring, &object);
    TEST_ASSERT(rte_ring_empty(free_mail_ring),
        "send2cp free_mail_ring fixture is empty", "mail slot pool should be empty");
    available_before = rte_mempool_avail_count(pool);
    mbuf = alloc_test_mbuf(pool, 0, 64);
    send2cp(fastrg_ccb, mbuf, EV_DP_PPPoE, WAN_PORT);
    TEST_ASSERT(rte_ring_empty(cp_q), "empty free_mail_ring bypasses cp_q", "cp_q should remain empty");
    TEST_ASSERT(rte_mempool_avail_count(pool) == available_before,
        "empty free_mail_ring frees mbuf", "fallback path should return mbuf to its mempool");

    rte_mempool_free(pool);
    rte_ring_free(free_mail_ring);
    rte_ring_free(cp_q);
    fastrg_ccb->cp_q = old_cp_q;
    fastrg_ccb->free_mail_ring = old_free_mail_ring;
    fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT] = old_lan_stats;
    fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT] = old_wan_stats;
    fastrg_rcu_persistent[lcore_id] = old_persistent;
}

void test_dp(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╭────────────────────────────────────────────────────────────╮\n");
    printf("│           DP Pure Logic Unit Tests                         │\n");
    printf("╰────────────────────────────────────────────────────────────╯\n");

    test_count = 0;
    pass_count = 0;

    test_compute_flow_tag();
    test_parse_l2_hdr(fastrg_ccb);
    test_is_iptv_pkt_need_drop(fastrg_ccb);
    test_send2cp(fastrg_ccb);

    printf("\nDP Pure Logic Test Summary: %d/%d passed\n", pass_count, test_count);
    *total_tests += test_count;
    *total_pass += pass_count;
}
