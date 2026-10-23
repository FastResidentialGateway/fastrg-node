#include <stddef.h>
#include <string.h>

#include <common.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "../src/dp.h"
#include "../src/dp_ipv6.h"
#include "../src/fastrg.h"
#include "../src/nd6/nd6.h"
#include "../src/pppd/ipv6_firewall.h"
#include "../src/pppd/nat.h"
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
    struct per_ccb_stats *old_lan_stats;
    struct per_ccb_stats *old_wan_stats;
    unsigned int available_before;
    unsigned int free_count_before;
    struct rte_mbuf *mbuf;

    printf("\nTesting send2cp function:\n");

    TEST_ASSERT(lcore_id != LCORE_ID_ANY, "send2cp runs on an EAL lcore", "unit test should run on EAL lcore 0");
    old_lan_stats = fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT];
    old_wan_stats = fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT];
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
}

/* Two lcores writing one TX queue corrupt its ring, so the layout must never
 * hand the same queue to two senders. Checked across every data-queue count
 * the build allows. */
static void test_get_tx_queue_id_for_sender(void)
{
    int clash_at = -1, gap_at = -1;

    printf("\nTesting get_tx_queue_id_for_sender:\n");

    TEST_ASSERT(get_tx_queue_count(2) == 5,
        "queue count is the data queues plus three", "expected 5 for N=2");

    for(U16 n=1; n<=MAX_DATA_QUEUES; n++) {
        for(U16 port=0; port<PORT_AMOUNT; port++) {
            /* owner[q] = the sender+index holding queue q, -1 while free.
             * Sized for the largest layout the build allows. */
            const U16 queue_max = get_tx_queue_count(MAX_DATA_QUEUES);
            int owner_sender[queue_max];
            int owner_index[queue_max];
            for(U16 q=0; q<queue_max; q++) {
                owner_sender[q] = -1;
                owner_index[q] = -1;
            }
            for(int sender=0; sender<=FASTRG_TX_SENDER_WAN_CTRL/* last one in enum */; sender++) {
                for(U16 index=0; index<MAX_DATA_QUEUES; index++) {
                    U16 q = get_tx_queue_id_for_sender((fastrg_tx_sender_t)sender, index, port, n);
                    if (q == FASTRG_TX_QUEUE_NONE)
                        continue;
                    if (q >= get_tx_queue_count(n)) {
                        gap_at = n;
                        continue;
                    }
                    if (owner_sender[q] != -1 &&
                        (owner_sender[q] != sender || owner_index[q] != (int)index))
                        clash_at = n;
                    owner_sender[q] = sender;
                    owner_index[q] = (int)index;
                }
            }
            /* Every queue the port was given has exactly one sender. */
            for(U16 q=0; q<get_tx_queue_count(n); q++)
                if (owner_sender[q] == -1)
                    gap_at = n;
        }
    }
    TEST_ASSERT(clash_at == -1, "no TX queue has two senders",
        "two senders collide at N=%d", clash_at);
    TEST_ASSERT(gap_at == -1, "every TX queue has a sender and stays in range",
        "layout leaves a hole or overruns at N=%d", gap_at);

    /* The pairing that matters: a reader never owns a queue on the port it
     * reads, so its replies there have to be handed over. */
    TEST_ASSERT(get_tx_queue_id_for_sender(FASTRG_TX_SENDER_LAN_DATA, 0, LAN_PORT, 2)
            == FASTRG_TX_QUEUE_NONE,
        "a LAN reader owns no LAN queue", "it must hand LAN-bound packets over");
    TEST_ASSERT(get_tx_queue_id_for_sender(FASTRG_TX_SENDER_WAN_DATA, 0, WAN_PORT, 2)
            == FASTRG_TX_QUEUE_NONE,
        "a WAN reader owns no WAN queue", "it must hand WAN-bound packets over");
    TEST_ASSERT(get_tx_queue_id_for_sender(FASTRG_TX_SENDER_WAN_DATA, 1, LAN_PORT, 2) == 2,
        "the second WAN reader owns LAN queue 2", "expected queue 2");

    /* Each poller owns a queue on both ports: replies on the port it polls,
     * forwarding on the other one. */
    TEST_ASSERT(get_tx_queue_id_for_sender(FASTRG_TX_SENDER_LAN_CTRL, 0, LAN_PORT, 2) == 3,
        "the LAN poller replies on LAN queue N+1", "expected queue 3");
    TEST_ASSERT(get_tx_queue_id_for_sender(FASTRG_TX_SENDER_LAN_CTRL, 0, WAN_PORT, 2) == 4,
        "the LAN poller forwards on WAN queue N+2", "expected queue 4");
    TEST_ASSERT(get_tx_queue_id_for_sender(FASTRG_TX_SENDER_WAN_CTRL, 0, WAN_PORT, 2) == 3,
        "the WAN poller replies on WAN queue N+1", "expected queue 3");
    TEST_ASSERT(get_tx_queue_id_for_sender(FASTRG_TX_SENDER_WAN_CTRL, 0, LAN_PORT, 2) == 4,
        "the WAN poller forwards on LAN queue N+2", "expected queue 4");
    TEST_ASSERT(get_tx_queue_id_for_sender(FASTRG_TX_SENDER_LAN_CTRL, 1, LAN_PORT, 2)
            == FASTRG_TX_QUEUE_NONE,
        "a poller has only index 0", "expected no queue for index 1");

    /* Start-up resolves the control queues once, from the LAN port, and uses
     * the answer for both ports. That holds only while the two ports place
     * those queues at the same index. */
    int self_at = -1, opposite_at = -1, ctrl_thread_at = -1;
    for(U16 n=1; n<=MAX_DATA_QUEUES; n++) {
        if (get_tx_queue_id_for_sender(FASTRG_TX_SENDER_LAN_CTRL, 0, LAN_PORT, n) !=
            get_tx_queue_id_for_sender(FASTRG_TX_SENDER_WAN_CTRL, 0, WAN_PORT, n))
            self_at = n;
        if (get_tx_queue_id_for_sender(FASTRG_TX_SENDER_WAN_CTRL, 0, LAN_PORT, n) !=
            get_tx_queue_id_for_sender(FASTRG_TX_SENDER_LAN_CTRL, 0, WAN_PORT, n))
            opposite_at = n;
        if (get_tx_queue_id_for_sender(FASTRG_TX_SENDER_CTRL_THREAD, 0, LAN_PORT, n) !=
            get_tx_queue_id_for_sender(FASTRG_TX_SENDER_CTRL_THREAD, 0, WAN_PORT, n))
            ctrl_thread_at = n;
    }
    TEST_ASSERT(self_at == -1, "a poller replies on the same queue id on either port",
        "the two ports disagree at N=%d", self_at);
    TEST_ASSERT(opposite_at == -1, "a poller forwards on the same queue id on either port",
        "the two ports disagree at N=%d", opposite_at);
    TEST_ASSERT(ctrl_thread_at == -1, "the control thread uses the same queue id on either port",
        "the two ports disagree at N=%d", ctrl_thread_at);
}


/* The owner folds handed-over packets into the burst it was already sending,
 * so it must respect the batch it is given. */
static void test_check_pkt_from_other_lcore(void)
{
    struct rte_ring *ring = rte_ring_lookup("tx_merge_test");
    struct rte_mbuf *batch[4];
    struct rte_mbuf slots[4];
    U16 len;

    printf("\nTesting check_pkt_from_other_lcore:\n");
    if (ring == NULL)
        ring = rte_ring_create("tx_merge_test", 8, (int)rte_socket_id(),
            RING_F_SC_DEQ | RING_F_EXACT_SZ);
    TEST_ASSERT(ring != NULL, "handover fixture ring", "rte_ring_create failed");
    if (ring == NULL)
        return;

    memset(batch, 0, sizeof(batch));
    TEST_ASSERT(check_pkt_from_other_lcore(NULL, batch, 1, 4) == 1,
        "no ring leaves the batch alone", "batch length changed");
    TEST_ASSERT(check_pkt_from_other_lcore(ring, batch, 1, 4) == 1,
        "an empty ring leaves the batch alone", "batch length changed");

    for(int i=0; i<4; i++)
        rte_ring_enqueue(ring, &slots[i]);

    /* Two already in the batch, capacity four: exactly two may come over. */
    len = check_pkt_from_other_lcore(ring, batch, 2, 4);
    TEST_ASSERT(len == 4, "the batch fills to capacity", "expected 4, got %u", len);
    TEST_ASSERT(batch[2] == &slots[0] && batch[3] == &slots[1],
        "handed-over packets keep their order", "wrong packets merged");
    TEST_ASSERT(rte_ring_count(ring) == 2,
        "what does not fit stays on the ring for the next pass",
        "ring should still hold 2");

    /* A full batch takes nothing, and nothing is lost. */
    len = check_pkt_from_other_lcore(ring, batch, 4, 4);
    TEST_ASSERT(len == 4, "a full batch takes nothing", "batch length changed");
    TEST_ASSERT(rte_ring_count(ring) == 2, "a full batch leaves the ring untouched",
        "ring lost entries");

    while (rte_ring_dequeue(ring, (void **)&batch[0]) == 0)
        ;
}

/* Distributor worker fixture: one subscriber with live NAT, IPv6 firewall and
 * neighbor tables, swapped into ppp_ccb[0] for the worker tests. */
#define WORKER_NAT_NAME_ID  997
#define WORKER_FW_NAME_ID   60005
#define WORKER_ND6_NAME_ID  60006
#define WORKER_VLAN_ID      3
#define WORKER_SESSION_ID   0x2345
#define WORKER_LAN_SPORT    40000
#define WORKER_REMOTE_PORT  53

static ppp_ccb_t worker_ccb;
static struct rte_mempool *worker_pool;
static struct per_ccb_stats worker_stats[PORT_AMOUNT][1];

static const struct rte_ether_addr worker_host_mac = {
    .addr_bytes = {0x02, 0x00, 0x00, 0x00, 0x00, 0x10},
};
static const struct rte_ether_addr worker_bras_mac = {
    .addr_bytes = {0x02, 0x00, 0x00, 0x00, 0x00, 0x20},
};
static const U8 worker_lan_host6[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00,
};
static const U8 worker_remote6[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x53,
};
#define WORKER_LAN_HOST4    RTE_BE32(0xC0A80264)
#define WORKER_REMOTE4      RTE_BE32(0x08080808)
#define WORKER_HSI_IPV4     RTE_BE32(0x64400001)

/* Build eth / vlan / IPv4 / TCP or UDP as a distributor RX lcore hands it to a
 * worker: no PPPoE header, priv pointing at the L2 headers. */
static struct rte_mbuf *build_worker_ipv4_frame(U8 proto, U32 src_ip, U16 src_port,
    U32 dst_ip, U16 dst_port)
{
    U16 l4_len = proto == IPPROTO_TCP ? sizeof(struct rte_tcp_hdr) : sizeof(struct rte_udp_hdr);
    U16 len = (U16)(sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        sizeof(struct rte_ipv4_hdr) + l4_len);
    struct rte_mbuf *m = rte_pktmbuf_alloc(worker_pool);
    struct rte_ether_hdr *eth;
    vlan_header_t *vlan;
    struct rte_ipv4_hdr *ip;
    mbuf_priv_t *priv;

    if (m == NULL)
        return NULL;
    eth = (struct rte_ether_hdr *)rte_pktmbuf_append(m, len);
    memset(eth, 0, len);
    vlan = (vlan_header_t *)(eth + 1);
    ip = (struct rte_ipv4_hdr *)(vlan + 1);
    rte_ether_addr_copy(&worker_host_mac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(VLAN);
    vlan->tci_union.tci_value = rte_cpu_to_be_16(WORKER_VLAN_ID);
    vlan->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IP);
    ip->version_ihl = RTE_IPV4_VHL_DEF;
    ip->total_length = rte_cpu_to_be_16((U16)(sizeof(*ip) + l4_len));
    ip->time_to_live = 64;
    ip->next_proto_id = proto;
    ip->src_addr = src_ip;
    ip->dst_addr = dst_ip;
    if (proto == IPPROTO_TCP) {
        struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip + 1);
        tcp->src_port = rte_cpu_to_be_16(src_port);
        tcp->dst_port = rte_cpu_to_be_16(dst_port);
        tcp->data_off = 0x50;
        tcp->tcp_flags = RTE_TCP_SYN_FLAG;
    } else {
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
        udp->src_port = rte_cpu_to_be_16(src_port);
        udp->dst_port = rte_cpu_to_be_16(dst_port);
        udp->dgram_len = rte_cpu_to_be_16(sizeof(*udp));
    }
    priv = rte_mbuf_to_priv(m);
    priv->ccb_id = 0;
    priv->eth_hdr = eth;
    priv->vlan_hdr = vlan;
    return m;
}

/* Build an IPv6 UDP frame as a distributor RX lcore hands it to a worker: from
 * the LAN as eth / vlan / IPv6, from the WAN still PPPoE-encapsulated. */
static struct rte_mbuf *build_worker_ipv6_frame(BOOL from_wan, const U8 *src_ip,
    U16 src_port, const U8 *dst_ip, U16 dst_port)
{
    U16 pppoe_len = from_wan ? IPV6_PPPOE_HDR_LEN : 0;
    U16 len = (U16)(IPV6_L2_LEN + pppoe_len + sizeof(struct rte_ipv6_hdr) +
        sizeof(struct rte_udp_hdr));
    struct rte_mbuf *m = rte_pktmbuf_alloc(worker_pool);
    struct rte_ether_hdr *eth;
    vlan_header_t *vlan;
    struct rte_ipv6_hdr *ip6;
    struct rte_udp_hdr *udp;
    mbuf_priv_t *priv;

    if (m == NULL)
        return NULL;
    eth = (struct rte_ether_hdr *)rte_pktmbuf_append(m, len);
    memset(eth, 0, len);
    vlan = (vlan_header_t *)(eth + 1);
    ip6 = (struct rte_ipv6_hdr *)((U8 *)eth + IPV6_L2_LEN + pppoe_len);
    udp = (struct rte_udp_hdr *)(ip6 + 1);
    eth->ether_type = rte_cpu_to_be_16(VLAN);
    vlan->tci_union.tci_value = rte_cpu_to_be_16(WORKER_VLAN_ID);
    if (from_wan) {
        pppoe_header_t *pppoe = (pppoe_header_t *)(vlan + 1);
        U16 *ppp_proto = (U16 *)(pppoe + 1);

        rte_ether_addr_copy(&worker_bras_mac, &eth->src_addr);
        vlan->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);
        pppoe->ver_type = VER_TYPE;
        pppoe->session_id = rte_cpu_to_be_16(WORKER_SESSION_ID);
        pppoe->length = rte_cpu_to_be_16((U16)(sizeof(*ip6) + sizeof(*udp) +
            sizeof(ppp_payload_t)));
        *ppp_proto = rte_cpu_to_be_16(PPP_IPV6_PROTOCOL);
    } else {
        rte_ether_addr_copy(&worker_host_mac, &eth->src_addr);
        vlan->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IPV6);
    }
    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(6) << 28);
    ip6->payload_len = rte_cpu_to_be_16(sizeof(*udp));
    ip6->proto = IPPROTO_UDP;
    ip6->hop_limits = 64;
    memcpy(&ip6->src_addr, src_ip, 16);
    memcpy(&ip6->dst_addr, dst_ip, 16);
    udp->src_port = rte_cpu_to_be_16(src_port);
    udp->dst_port = rte_cpu_to_be_16(dst_port);
    udp->dgram_len = rte_cpu_to_be_16(sizeof(*udp));
    priv = rte_mbuf_to_priv(m);
    priv->ccb_id = 0;
    priv->eth_hdr = eth;
    priv->vlan_hdr = vlan;
    return m;
}

static void set_worker_gates(BOOL ipv4_open, BOOL ipv6_open)
{
    rte_atomic16_set(&worker_ccb.dp_start_bool, ipv4_open ? 1 : 0);
    rte_atomic16_set(&worker_ccb.ipv6_dp_bool, ipv6_open ? 1 : 0);
}

static void test_dp_lan_dist_worker_process_pkt(FastRG_t *fastrg_ccb)
{
    static const U8 l4_protos[] = { IPPROTO_TCP, IPPROTO_UDP };
    struct rte_mbuf *wan_pkt[4];
    struct rte_mbuf *m;
    U16 n;
    int32_t rev_before, fwd_before, fw_before;
    U64 drop_before;
    unsigned int avail_before;

    printf("\nTesting dp_lan_dist_worker_process_pkt:\n");

    /* IPv4 gate closed: no NAT entry is learned and the packet is dropped. */
    set_worker_gates(FALSE, TRUE);
    for (U32 i = 0; i < RTE_DIM(l4_protos); i++) {
        rev_before = rte_hash_count(worker_ccb.nat_reverse_hash);
        fwd_before = rte_hash_count(worker_ccb.nat_forward_hash);
        drop_before = worker_stats[LAN_PORT][0].dropped_packets;
        avail_before = rte_mempool_avail_count(worker_pool);
        m = build_worker_ipv4_frame(l4_protos[i], WORKER_LAN_HOST4,
            (U16)(WORKER_LAN_SPORT + i), WORKER_REMOTE4, WORKER_REMOTE_PORT);
        TEST_ASSERT(m != NULL, "LAN worker IPv4 frame allocation", NULL);
        if (m == NULL)
            return;
        n = 0;
        dp_lan_dist_worker_process_pkt(fastrg_ccb, m, 1, wan_pkt, &n);
        TEST_ASSERT(n == 0 && rte_mempool_avail_count(worker_pool) == avail_before,
            "closed IPv4 gate drops the LAN packet", "proto %u: n=%u", l4_protos[i], n);
        TEST_ASSERT(worker_stats[LAN_PORT][0].dropped_packets == drop_before + 1,
            "closed IPv4 gate counts a LAN drop", "proto %u", l4_protos[i]);
        TEST_ASSERT(rte_hash_count(worker_ccb.nat_reverse_hash) == rev_before &&
                rte_hash_count(worker_ccb.nat_forward_hash) == fwd_before,
            "closed IPv4 gate learns no NAT entry", "proto %u", l4_protos[i]);
    }

    /* IPv4 gate open: the same kind of packet is NAT-learned and encapsulated. */
    set_worker_gates(TRUE, FALSE);
    rev_before = rte_hash_count(worker_ccb.nat_reverse_hash);
    fwd_before = rte_hash_count(worker_ccb.nat_forward_hash);
    m = build_worker_ipv4_frame(IPPROTO_UDP, WORKER_LAN_HOST4, WORKER_LAN_SPORT,
        WORKER_REMOTE4, WORKER_REMOTE_PORT);
    TEST_ASSERT(m != NULL, "LAN worker IPv4 frame allocation", NULL);
    if (m == NULL)
        return;
    n = 0;
    dp_lan_dist_worker_process_pkt(fastrg_ccb, m, 1, wan_pkt, &n);
    TEST_ASSERT(n == 1 && wan_pkt[0] == m, "open IPv4 gate queues the packet for WAN TX",
        "n=%u", n);
    TEST_ASSERT(rte_hash_count(worker_ccb.nat_reverse_hash) == rev_before + 1 &&
            rte_hash_count(worker_ccb.nat_forward_hash) == fwd_before + 1,
        "open IPv4 gate learns one NAT entry", NULL);
    TEST_ASSERT(rte_pktmbuf_mtod(m, struct rte_ether_hdr *)->ether_type ==
            rte_cpu_to_be_16(VLAN) &&
            ((vlan_header_t *)(rte_pktmbuf_mtod(m, struct rte_ether_hdr *) + 1))->next_proto ==
            rte_cpu_to_be_16(ETH_P_PPP_SES),
        "open IPv4 gate PPPoE-encapsulates the packet", NULL);
    if (n > 0)
        rte_pktmbuf_free(wan_pkt[0]);

    /* IPv6 gate closed: no firewall session is opened and the packet is dropped. */
    set_worker_gates(TRUE, FALSE);
    fw_before = rte_hash_count(worker_ccb.ipv6_firewall_hash);
    drop_before = worker_stats[LAN_PORT][0].dropped_packets;
    avail_before = rte_mempool_avail_count(worker_pool);
    m = build_worker_ipv6_frame(FALSE, worker_lan_host6, WORKER_LAN_SPORT,
        worker_remote6, WORKER_REMOTE_PORT);
    TEST_ASSERT(m != NULL, "LAN worker IPv6 frame allocation", NULL);
    if (m == NULL)
        return;
    n = 0;
    dp_lan_dist_worker_process_pkt(fastrg_ccb, m, 1, wan_pkt, &n);
    TEST_ASSERT(n == 0 && rte_mempool_avail_count(worker_pool) == avail_before &&
            worker_stats[LAN_PORT][0].dropped_packets == drop_before + 1,
        "closed IPv6 gate drops the LAN packet", "n=%u", n);
    TEST_ASSERT(rte_hash_count(worker_ccb.ipv6_firewall_hash) == fw_before,
        "closed IPv6 gate opens no firewall session", NULL);

    /* IPv6 gate open: the packet opens a session and is encapsulated. */
    set_worker_gates(FALSE, TRUE);
    m = build_worker_ipv6_frame(FALSE, worker_lan_host6, WORKER_LAN_SPORT,
        worker_remote6, WORKER_REMOTE_PORT);
    TEST_ASSERT(m != NULL, "LAN worker IPv6 frame allocation", NULL);
    if (m == NULL)
        return;
    n = 0;
    dp_lan_dist_worker_process_pkt(fastrg_ccb, m, 1, wan_pkt, &n);
    TEST_ASSERT(n == 1 && wan_pkt[0] == m, "open IPv6 gate queues the packet for WAN TX",
        "n=%u", n);
    TEST_ASSERT(rte_hash_count(worker_ccb.ipv6_firewall_hash) == fw_before + 1,
        "open IPv6 gate opens one firewall session", NULL);
    if (n > 0)
        rte_pktmbuf_free(wan_pkt[0]);
}

static void test_dp_wan_dist_worker_process_pkt(FastRG_t *fastrg_ccb)
{
    struct rte_mbuf *tx_pkt[4];
    struct rte_mbuf *m;
    struct rte_ipv4_hdr *ip;
    struct rte_udp_hdr *udp;
    U16 nat_port;
    U16 n;
    U64 drop_before;
    unsigned int avail_before;

    printf("\nTesting dp_wan_dist_worker_process_pkt:\n");

    /* A LAN flow learned on the product path, so its reply hits the NAT table. */
    m = build_worker_ipv4_frame(IPPROTO_UDP, WORKER_LAN_HOST4, WORKER_LAN_SPORT,
        WORKER_REMOTE4, WORKER_REMOTE_PORT);
    TEST_ASSERT(m != NULL, "WAN worker learning frame allocation", NULL);
    if (m == NULL)
        return;
    ip = (struct rte_ipv4_hdr *)(((mbuf_priv_t *)rte_mbuf_to_priv(m))->vlan_hdr + 1);
    nat_port = nat_udp_learning(&worker_ccb, ((mbuf_priv_t *)rte_mbuf_to_priv(m))->eth_hdr,
        ip, (struct rte_udp_hdr *)(ip + 1));
    rte_pktmbuf_free(m);
    TEST_ASSERT(nat_port != 0, "WAN worker fixture learns a NAT mapping", NULL);

    /* IPv4 gate closed: the reply is dropped even though the mapping exists. */
    set_worker_gates(FALSE, TRUE);
    drop_before = worker_stats[WAN_PORT][0].dropped_packets;
    avail_before = rte_mempool_avail_count(worker_pool);
    m = build_worker_ipv4_frame(IPPROTO_UDP, WORKER_REMOTE4, WORKER_REMOTE_PORT,
        WORKER_HSI_IPV4, rte_be_to_cpu_16(nat_port));
    TEST_ASSERT(m != NULL, "WAN worker IPv4 frame allocation", NULL);
    if (m == NULL)
        return;
    n = 0;
    dp_wan_dist_worker_process_pkt(fastrg_ccb, m, 1, tx_pkt, &n);
    TEST_ASSERT(n == 0 && rte_mempool_avail_count(worker_pool) == avail_before,
        "closed IPv4 gate drops the WAN reply", "n=%u", n);
    TEST_ASSERT(worker_stats[WAN_PORT][0].dropped_packets == drop_before + 1,
        "closed IPv4 gate counts a WAN drop", NULL);

    /* IPv4 gate open: the same reply is translated back to the LAN host. */
    set_worker_gates(TRUE, FALSE);
    m = build_worker_ipv4_frame(IPPROTO_UDP, WORKER_REMOTE4, WORKER_REMOTE_PORT,
        WORKER_HSI_IPV4, rte_be_to_cpu_16(nat_port));
    TEST_ASSERT(m != NULL, "WAN worker IPv4 frame allocation", NULL);
    if (m == NULL)
        return;
    n = 0;
    dp_wan_dist_worker_process_pkt(fastrg_ccb, m, 1, tx_pkt, &n);
    TEST_ASSERT(n == 1 && tx_pkt[0] == m, "open IPv4 gate queues the reply for LAN TX",
        "n=%u", n);
    ip = (struct rte_ipv4_hdr *)(((mbuf_priv_t *)rte_mbuf_to_priv(m))->vlan_hdr + 1);
    udp = (struct rte_udp_hdr *)(ip + 1);
    TEST_ASSERT(ip->dst_addr == WORKER_LAN_HOST4 &&
            udp->dst_port == rte_cpu_to_be_16(WORKER_LAN_SPORT),
        "open IPv4 gate rewrites the reply to the LAN host", NULL);
    if (n > 0)
        rte_pktmbuf_free(tx_pkt[0]);

    /* An open IPv6 session and a known LAN neighbor for the IPv6 reply. */
    m = build_worker_ipv6_frame(FALSE, worker_lan_host6, WORKER_LAN_SPORT,
        worker_remote6, WORKER_REMOTE_PORT);
    TEST_ASSERT(m != NULL, "WAN worker IPv6 learning frame allocation", NULL);
    if (m == NULL)
        return;
    ipv6_firewall_learn(&worker_ccb, (struct rte_ipv6_hdr *)
        ((U8 *)((mbuf_priv_t *)rte_mbuf_to_priv(m))->eth_hdr + IPV6_L2_LEN));
    rte_pktmbuf_free(m);
    nd6_table_learn(worker_ccb.nd6_table, worker_lan_host6, &worker_host_mac);

    /* IPv6 gate closed: the reply is dropped even though the session exists. */
    set_worker_gates(TRUE, FALSE);
    drop_before = worker_stats[WAN_PORT][0].dropped_packets;
    avail_before = rte_mempool_avail_count(worker_pool);
    m = build_worker_ipv6_frame(TRUE, worker_remote6, WORKER_REMOTE_PORT,
        worker_lan_host6, WORKER_LAN_SPORT);
    TEST_ASSERT(m != NULL, "WAN worker IPv6 frame allocation", NULL);
    if (m == NULL)
        return;
    n = 0;
    dp_wan_dist_worker_process_pkt(fastrg_ccb, m, 1, tx_pkt, &n);
    TEST_ASSERT(n == 0 && rte_mempool_avail_count(worker_pool) == avail_before &&
            worker_stats[WAN_PORT][0].dropped_packets == drop_before + 1,
        "closed IPv6 gate drops the WAN reply", "n=%u", n);

    /* IPv6 gate open: the same reply is forwarded to the LAN host. */
    set_worker_gates(FALSE, TRUE);
    m = build_worker_ipv6_frame(TRUE, worker_remote6, WORKER_REMOTE_PORT,
        worker_lan_host6, WORKER_LAN_SPORT);
    TEST_ASSERT(m != NULL, "WAN worker IPv6 frame allocation", NULL);
    if (m == NULL)
        return;
    n = 0;
    dp_wan_dist_worker_process_pkt(fastrg_ccb, m, 1, tx_pkt, &n);
    TEST_ASSERT(n == 1 && tx_pkt[0] == m, "open IPv6 gate queues the reply for LAN TX",
        "n=%u", n);
    if (n > 0)
        rte_pktmbuf_free(tx_pkt[0]);
}

/* Builds the worker fixture, runs both worker tests, then restores the mock. */
static void run_dist_worker_tests(FastRG_t *fastrg_ccb)
{
    unsigned int lcore_id = rte_lcore_id();
    ppp_ccb_t *old_ccb = fastrg_ccb->ppp_ccb[0];
    struct per_ccb_stats *old_lan_stats = fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT];
    struct per_ccb_stats *old_wan_stats = fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT];
    struct pppoes_lcore_stats *old_pppoes = fastrg_ccb->pppoes_stats[lcore_id];
    BOOL ready;

    memset(&worker_ccb, 0, sizeof(worker_ccb));
    memset(worker_stats, 0, sizeof(worker_stats));
    worker_ccb.fastrg_ccb = fastrg_ccb;
    worker_ccb.session_id = rte_cpu_to_be_16(WORKER_SESSION_ID);
    worker_ccb.hsi_ipv4 = WORKER_HSI_IPV4;
    rte_ether_addr_copy(&worker_bras_mac, &worker_ccb.PPP_dst_mac);
    rte_atomic16_init(&worker_ccb.dp_start_bool);
    rte_atomic16_init(&worker_ccb.ipv6_dp_bool);
    rte_atomic16_init(&worker_ccb.vlan_id);
    rte_atomic16_set(&worker_ccb.vlan_id, WORKER_VLAN_ID);
    rte_spinlock_init(&worker_ccb.nat_insert_lock);
    rte_spinlock_init(&worker_ccb.ipv6_firewall_insert_lock);

    worker_pool = rte_pktmbuf_pool_create("dp_test_worker_pool", 32, 0, sizeof(mbuf_priv_t),
        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    worker_ccb.nd6_table = nd6_table_alloc(WORKER_ND6_NAME_ID, NULL);
    ready = worker_pool != NULL && worker_ccb.nd6_table != NULL &&
        nat_table_init(&worker_ccb, WORKER_NAT_NAME_ID, fastrg_ccb->ppp_ccb_rcu) == SUCCESS &&
        ipv6_firewall_table_init(&worker_ccb, WORKER_FW_NAME_ID, fastrg_ccb->ppp_ccb_rcu) == SUCCESS;
    TEST_ASSERT(ready, "distributor worker fixture", "pool, NAT, firewall or nd6 setup failed");

    if (ready) {
        fastrg_ccb->ppp_ccb[0] = &worker_ccb;
        fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT] = worker_stats[LAN_PORT];
        fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT] = worker_stats[WAN_PORT];
        fastrg_ccb->pppoes_stats[lcore_id] = NULL;

        test_dp_lan_dist_worker_process_pkt(fastrg_ccb);
        test_dp_wan_dist_worker_process_pkt(fastrg_ccb);

        fastrg_ccb->ppp_ccb[0] = old_ccb;
        fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT] = old_lan_stats;
        fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT] = old_wan_stats;
        fastrg_ccb->pppoes_stats[lcore_id] = old_pppoes;
    }

    ipv6_firewall_table_destroy(&worker_ccb);
    nat_table_destroy(&worker_ccb);
    if (worker_ccb.nd6_table != NULL)
        nd6_table_free(worker_ccb.nd6_table);
    if (worker_pool != NULL)
        rte_mempool_free(worker_pool);
    worker_pool = NULL;
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
    test_get_tx_queue_id_for_sender();
    test_check_pkt_from_other_lcore();
    run_dist_worker_tests(fastrg_ccb);

    printf("\nDP Pure Logic Test Summary: %d/%d passed\n", pass_count, test_count);
    *total_tests += test_count;
    *total_pass += pass_count;
}
