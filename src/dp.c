#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_arp.h>
#include <rte_ip.h>
#include <rte_icmp.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_timer.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <rte_memcpy.h>
#include <rte_atomic.h>
#include <rte_ip_frag.h>

#include "fastrg.h"
#include "protocol.h"
#include "pppd/nat.h"
#include "init.h"
#include "dp_codec.h"
#include "dp_flow.h"
#include "dhcpd/dhcpd.h"
#include "dnsd/dnsd.h"
#include "mac_table.h"
#include "dbg.h"
#include "dp.h"
#include "trace.h"
#include "utils.h"

#define RX_RING_SIZE 128

#define TX_RING_SIZE 512

#define BURST_SIZE 32

#define	IPV4_MTU_DEFAULT	RTE_ETHER_MTU
#define	IPV6_MTU_DEFAULT	RTE_ETHER_MTU

extern struct rte_mempool 		*direct_pool[PORT_AMOUNT], *indirect_pool[PORT_AMOUNT];
extern struct rte_ring 			*cp_q, *free_mail_ring;
static U16 						nb_rxd = RX_RING_SIZE;
static U16 						nb_txd = TX_RING_SIZE;

static struct rte_eth_conf port_conf_default = {
    /* https://github.com/DPDK/dpdk/commit/1bb4a528c41f4af4847bd3d58cc2b2b9f1ec9a27#diff-71b61db11e3ee1ca6bb272a90e3c1aa0e8c90071b1a38387fd541687314b1843
     * From this commit, mtu field is only for jumbo frame
     **/
    //.rxmode = { .mtu = RTE_ETHER_MAX_JUMBO_FRAME_LEN - RTE_ETHER_HDR_LEN - RTE_ETHER_CRC_LEN, }, 
    .txmode = { .offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | 
                            RTE_ETH_TX_OFFLOAD_UDP_CKSUM | 
                            /*RTE_ETH_TX_OFFLOAD_MT_LOCKFREE |*/
                            RTE_ETH_TX_OFFLOAD_TCP_CKSUM, },
    .intr_conf = {
        .lsc = 1, /**< link status interrupt feature enabled */ },
};
static int lsi_event_callback(U16 port_id, enum rte_eth_event_type type, void *param);

STATUS PORT_INIT(FastRG_t *fastrg_ccb, U16 port)
{
    struct rte_eth_conf port_conf = port_conf_default;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_rxconf rxq_conf;
    struct rte_eth_txconf *txconf;
    /* Non-ICE PMDs do not support rte_flow RSS; use a single queue. */
    const U16 rx_rings = (fastrg_ccb->nic_info.vendor_id == NIC_VENDOR_ICE) ?
        fastrg_calc_queue_count(rte_lcore_count()) : 1;
    const U16 tx_rings = rx_rings;   /* symmetric TX queues */
    int retval;
    U16 q;

    if (fastrg_ccb->nic_info.vendor_id > NIC_VENDOR_VMXNET3)
        port_conf.intr_conf.lsc = 0;
    if (!rte_eth_dev_is_valid_port(port)) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Invalid port: %u", port);
        return ERROR;
    }
    int ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Error during getting device (port %u) info: %s\n", port, strerror(-ret));
        return ERROR;
    }
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    /* Enable multi-queue RSS so plain IP traffic distributes via RETA.
     * PPPoE flows are handled by explicit rte_flow rules (which take
     * priority), and the RETA will be reprogrammed after port start to
     * only use queues 1..N-1, excluding the PPPoE control queue 0. */
    if (rx_rings > 1) {
        uint64_t rss_hf =/* RTE_ETH_RSS_IP |*/ RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP;
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        port_conf.rx_adv_conf.rss_conf.rss_key = NULL; /* use default key */
        port_conf.rx_adv_conf.rss_conf.rss_hf =
            rss_hf & dev_info.flow_type_rss_offloads;
    }

    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot configure port %d: %s", port, rte_strerror(rte_errno));
        return ERROR;
    }
    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval < 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot adjust number of descriptors: err=%s, port=%u\n", strerror(-retval), port);
        return ERROR;
    }

    retval = rte_eth_dev_callback_register(port, RTE_ETH_EVENT_INTR_LSC, (rte_eth_dev_cb_fn)lsi_event_callback, fastrg_ccb);
    if (retval < 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot register lsc callback: err=%s, port=%u\n", strerror(-retval), port);
        return ERROR;
    }

    rxq_conf = dev_info.default_rxconf;
    rxq_conf.offloads = port_conf.rxmode.offloads;
    /* Allocate and set up RX queues per Ethernet port. */
    for(q=0; q<rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd, rte_eth_dev_socket_id(port), &rxq_conf, direct_pool[port]);
        if (retval < 0) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot setup RX queue: err=%s, port=%u\n", strerror(-retval), port);
            goto err_unregister_callback;
        }
    }

    txconf = &dev_info.default_txconf;
    txconf->offloads = port_conf.txmode.offloads;
    /* Allocate and set up TX queues per Ethernet port. */
    for(q=0; q<tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd, rte_eth_dev_socket_id(port), txconf);
        if (retval < 0) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot setup TX queue: err=%s, port=%u\n", strerror(-retval), port);
            goto err_unregister_callback;
        }
    }

    /* Start the Ethernet port. */
    retval = rte_eth_dev_start(port);
    if (retval < 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot start port %d: %s", port, strerror(-retval));
        goto err_unregister_callback;
    }

    retval = rte_eth_promiscuous_enable(port);
    if (retval < 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot enable promiscuous mode for port %u: %s", port, strerror(-retval));
        goto err_dev_stop;
    }

    return SUCCESS;

err_dev_stop:
    rte_eth_dev_stop(port);

err_unregister_callback:
    rte_eth_dev_callback_unregister(port, RTE_ETH_EVENT_INTR_LSC, 
        (rte_eth_dev_cb_fn)lsi_event_callback, fastrg_ccb);
    return ERROR;
}

static inline STATUS parse_l2_hdr(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    U8 port_id)
{
    mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(single_pkt);
    struct rte_ether_hdr *eth_hdr;
    vlan_header_t *vlan_header;
    U16 ccb_id;
    U16 vlan_id;

    eth_hdr = rte_pktmbuf_mtod(single_pkt, struct rte_ether_hdr *);
    mbuf_priv->eth_hdr = eth_hdr;
    if (unlikely(eth_hdr->ether_type != rte_cpu_to_be_16(VLAN)))
        return ERROR;

    vlan_header = (vlan_header_t *)(rte_pktmbuf_mtod(single_pkt, unsigned char *) + sizeof(struct rte_ether_hdr));
    mbuf_priv->vlan_hdr = vlan_header;

    vlan_id = rte_be_to_cpu_16(vlan_header->tci_union.tci_value) & 0xFFF;
    if (unlikely(vlan_id < MIN_VLAN_ID || vlan_id > MAX_VLAN_ID))
        return ERROR;

    ccb_id = rte_atomic16_read(&fastrg_ccb->vlan_userid_map[vlan_id - 1]);
    if (unlikely(ccb_id > fastrg_ccb->user_count - 1))
        return ERROR;

    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);

    mbuf_priv->ccb_id = ccb_id;
    mbuf_priv->dhcp_server_ip = dhcp_ccb->dhcp_server_ip;
    mbuf_priv->dhcp_subnet_mask = dhcp_ccb->subnet_mask;

    return SUCCESS;
}

#define VOD_IP_PREFIX_HOST 10  // 10.0.0.0/24 in host order
#define VOD_IP_MASK 0x000000FF
static inline BOOL is_iptv_pkt_need_drop(FastRG_t *fastrg_ccb, vlan_header_t *vlan_hdr)
{
    /* We need to detect IGMP and multicast msg here */
    if (vlan_hdr->next_proto == rte_cpu_to_be_16(FRAME_TYPE_IP)) {
        struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(vlan_hdr + 1);
        if (ip_hdr->next_proto_id == PROTO_TYPE_UDP) { // use 4001 vlan tag to detect IPTV and VOD packet
            U16 vlan_id = rte_be_to_cpu_16(vlan_hdr->tci_union.tci_value) & 0xFFF;
            struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
            // VOD pkt dst ip is always 10.x.x.x, we compare it in network order
            if (likely(vlan_id == MULTICAST_TAG || 
                    ((ip_hdr->dst_addr) & VOD_IP_MASK) == VOD_IP_PREFIX_HOST)) {
                return FALSE;
            } else if (ip_hdr->total_length > rte_cpu_to_be_16(
                    sizeof(struct rte_udp_hdr) + sizeof(struct rte_ipv4_hdr)) && 
                    udp_hdr->dst_port == rte_be_to_cpu_16(DHCP_CLIENT_PORT)) {
                return FALSE;
            } else {
                return TRUE;
            }
        }
        if (ip_hdr->next_proto_id == IPPROTO_IGMP)
            return FALSE;
    }
    return TRUE;
}

/**
 * send2cp - Forward DNS/DHCP/PPPoE packet to control plane via cp_q ring.
 *
 * Stores the mbuf pointer directly in the pre-allocated tFastRG_MBX slot
 * (zero-copy). The control plane is responsible for freeing the mbuf after
 * processing.
 */
static inline void send2cp(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt,
    fastrg_event_type_t evt_type, U8 port_id)
{
    tFastRG_MBX *slot = NULL;
    U16 ccb_id = ((mbuf_priv_t *)rte_mbuf_to_priv(single_pkt))->ccb_id;

    if (rte_ring_dequeue(free_mail_ring, (void **)&slot) == 0) {
        slot->mbuf = single_pkt;
        slot->type = evt_type;
        slot->len = single_pkt->pkt_len;
        slot->ccb_id = ccb_id;
        slot->port_id = port_id;
        /* cp_q is full: return slot to free_mail_ring */
        if (rte_ring_enqueue(cp_q, slot) != 0) {
            rte_ring_enqueue(free_mail_ring, slot);
            drop_packet(fastrg_ccb, single_pkt, port_id, ccb_id);
        } else {
            count_rx_packet(fastrg_ccb, single_pkt, port_id, ccb_id);
            /* mbuf ownership transferred to control plane — do NOT free here */
        }
    } else {
        drop_packet(fastrg_ccb, single_pkt, port_id, ccb_id);
    }
}

/**
 * wan_ctrl_rx - WAN port queue 0 handler.
 *
 * Handles PPPoE control (discovery + LCP/IPCP/PAP) → send2cp,
 * IPTV forwarding, and ICMP echo-reply decapsulation + NAT reverse.
 * Runs on a dedicated lcore (wan_ctrl_thread).
 */
int wan_ctrl_rx(void *arg)
{
    struct rte_mbuf      *single_pkt;
    uint64_t             total_tx = 0;
    struct rte_ether_hdr *eth_hdr, tmp_eth_hdr;
    vlan_header_t        *vlan_header, tmp_vlan_header;
    struct rte_ipv4_hdr  *ip_hdr;
    struct rte_icmp_hdr  *icmphdr;
    struct rte_mbuf      *pkt[BURST_SIZE];
    U16                  nb_rx;
    ppp_payload_t        *ppp_payload;
    U16                  ccb_id;
    U16                  pppoe_len = sizeof(pppoe_header_t) + sizeof(ppp_payload_t);
    FastRG_t             *fastrg_ccb = (FastRG_t *)arg;
    const U16            rx_q = 0;  /* always queue 0 */
    const U16            tx_q = 0;  /* TX on LAN queue 0 */

    rte_thread_t thread_id = rte_thread_self();
    rte_thread_set_name(thread_id, "fastrg_wan_ctrl");

    while(rte_atomic16_read(&start_flag) == 0)
        rte_pause();

    while(likely(rte_atomic16_read(&stop_flag) == 0)) {
        nb_rx = rte_eth_rx_burst(WAN_PORT, rx_q, pkt, BURST_SIZE);
        for(int i=0; i<nb_rx; i++) {
            single_pkt = pkt[i];
            rte_prefetch0(rte_pktmbuf_mtod(single_pkt, void *));
            if (unlikely(parse_l2_hdr(fastrg_ccb, single_pkt, WAN_PORT) == ERROR)) {
                drop_packet(fastrg_ccb, single_pkt, WAN_PORT, fastrg_ccb->user_count);
                continue;
            }
            mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(single_pkt);
            vlan_header = mbuf_priv->vlan_hdr;
            eth_hdr = mbuf_priv->eth_hdr;
            ccb_id = mbuf_priv->ccb_id;

            /* Usually if a packet is not a PPPoE packet, it should be an IPTV packet. */
            if (unlikely(vlan_header->next_proto != rte_cpu_to_be_16(ETH_P_PPP_SES) && 
                    vlan_header->next_proto != rte_cpu_to_be_16(ETH_P_PPP_DIS))) {
                if (is_iptv_pkt_need_drop(fastrg_ccb, vlan_header) == TRUE) {
                    drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                } else {
                    count_rx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    pkt[total_tx++] = single_pkt;
                    count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                }
                continue;
            }

            ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
            ppp_payload = ((ppp_payload_t *)((char *)eth_hdr + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t)));
            if (unlikely(vlan_header->next_proto == rte_cpu_to_be_16(ETH_P_PPP_DIS) || 
                    (ppp_payload->ppp_protocol == rte_cpu_to_be_16(LCP_PROTOCOL) || 
                    ppp_payload->ppp_protocol == rte_cpu_to_be_16(PAP_PROTOCOL) || 
                    ppp_payload->ppp_protocol == rte_cpu_to_be_16(IPCP_PROTOCOL)))) {
                /* Check whether ppp_bool is enabled */
                if (unlikely(rte_atomic16_read(&ppp_ccb->ppp_bool) == 0)) {
                    drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    continue;
                }
                send2cp(fastrg_ccb, single_pkt, EV_DP_PPPoE, WAN_PORT);
                continue;
            }

            /* PPPoE session data on queue 0 (fallback rule): strip PPPoE, handle ICMP */
            if (unlikely(rte_atomic16_read(&ppp_ccb->dp_start_bool) == (BIT16)0)) {
                drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                continue;
            }

            vlan_header->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IP);
            rte_memcpy(&tmp_eth_hdr, eth_hdr, sizeof(struct rte_ether_hdr));
            rte_memcpy(&tmp_vlan_header, vlan_header, sizeof(vlan_header_t));
            rte_memcpy((char *)eth_hdr+pppoe_len, &tmp_eth_hdr, sizeof(struct rte_ether_hdr));
            rte_memcpy((char *)vlan_header+pppoe_len, &tmp_vlan_header, sizeof(vlan_header_t));
            single_pkt->data_off += pppoe_len;
            single_pkt->pkt_len -= pppoe_len;
            single_pkt->data_len -= pppoe_len;
            eth_hdr = (struct rte_ether_hdr *)((char *)eth_hdr + pppoe_len);
            vlan_header = (vlan_header_t *)(eth_hdr + 1);
            mbuf_priv->vlan_hdr = vlan_header;
            mbuf_priv->eth_hdr = eth_hdr;

            ip_hdr = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(single_pkt, 
                unsigned char *) + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t));
            single_pkt->l2_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t);
            single_pkt->l3_len = sizeof(struct rte_ipv4_hdr);
            if (ip_hdr->next_proto_id == PROTO_TYPE_ICMP) {
                icmphdr = (struct rte_icmp_hdr *)(rte_pktmbuf_mtod(single_pkt, 
                    unsigned char *) + sizeof(struct rte_ether_hdr) + 
                    sizeof(vlan_header_t) + sizeof(struct rte_ipv4_hdr));
                if (icmphdr->icmp_type != ICMP_ECHO_REPLY) {
                    drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    continue;
                }
                addr_table_t *entry = nat_reverse_lookup(icmphdr->icmp_ident, 
                    ip_hdr->src_addr, ICMP_ECHO_REQUEST, ppp_ccb->addr_table);
                if (entry == NULL) {
                    drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    continue;
                }
                int32_t icmp_cksum_diff = (int32_t)icmphdr->icmp_ident - (int32_t)entry->src_port;
                U32 icmp_new_cksum;

                rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, 
                    &eth_hdr->src_addr);
                rte_ether_addr_copy(&entry->mac_addr, &eth_hdr->dst_addr);
                ip_hdr->dst_addr = entry->src_ip;
                icmphdr->icmp_ident = entry->src_port;

                if (((icmp_new_cksum = (U32)icmp_cksum_diff + (U32)icmphdr->icmp_cksum) >> 16) != 0)
                    icmp_new_cksum = (icmp_new_cksum & 0xFFFF) + (icmp_new_cksum >> 16);
                icmphdr->icmp_cksum = (U16)icmp_new_cksum;
                ip_hdr->hdr_checksum = 0;
                ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
                pkt[total_tx++] = single_pkt;
                count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                count_rx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                increase_pppoes_rx_count(ppp_ccb, single_pkt->pkt_len);
            } else {
                /* Non-ICMP session data that landed on queue 0 (shouldn't happen normally) */
                drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
            }
        }
        if (likely(total_tx > 0)) {
            U16 nb_tx = rte_eth_tx_burst(LAN_PORT, tx_q, pkt, total_tx);
            if (unlikely(nb_tx < total_tx)) {
                for(U16 buf=nb_tx; buf<total_tx; buf++) {
                    mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(pkt[buf]);
                    U16 ccb_id = mbuf_priv->ccb_id;
                    drop_packet(fastrg_ccb, pkt[buf], LAN_PORT, ccb_id);
                }
            }
            total_tx = 0;
        }
    }
    return 0;
}

/**
 * wan_data_rx - WAN port queues 1..N handler (one thread per RSS queue).
 *
 * All packets here are PPPoE session with inner TCP/UDP (from RSS).
 * Strips PPPoE header, performs NAT reverse (decaps), and TX to LAN.
 */
int wan_data_rx(void *arg)
{
    dp_rx_arg_t          *rx_arg = (dp_rx_arg_t *)arg;
    FastRG_t             *fastrg_ccb = rx_arg->fastrg_ccb;
    const U16            rx_q = rx_arg->rx_queue_id;
    const U16            tx_q = rx_arg->tx_queue_id;
    struct rte_mbuf      *single_pkt;
    uint64_t             total_tx = 0;
    struct rte_ether_hdr *eth_hdr, tmp_eth_hdr;
    vlan_header_t        *vlan_header, tmp_vlan_header;
    struct rte_ipv4_hdr  *ip_hdr;
    struct rte_mbuf      *pkt[BURST_SIZE];
    U16                  nb_rx;
    U16                  ccb_id;
    U16                  pppoe_len = sizeof(pppoe_header_t) + sizeof(ppp_payload_t);
    int                  pkt_num;
    char                 thread_name[32];

    snprintf(thread_name, sizeof(thread_name), "fastrg_wan_data_%u", rx_q);
    rte_thread_t thread_id = rte_thread_self();
    rte_thread_set_name(thread_id, thread_name);

    while(rte_atomic16_read(&start_flag) == 0)
        rte_pause();

    while(likely(rte_atomic16_read(&stop_flag) == 0)) {
        nb_rx = rte_eth_rx_burst(WAN_PORT, rx_q, pkt, BURST_SIZE);
        for(int i=0; i<nb_rx; i++) {
            single_pkt = pkt[i];
            rte_prefetch0(rte_pktmbuf_mtod(single_pkt, void *));
            if (unlikely(parse_l2_hdr(fastrg_ccb, single_pkt, WAN_PORT) == ERROR)) {
                drop_packet(fastrg_ccb, single_pkt, WAN_PORT, fastrg_ccb->user_count);
                continue;
            }
            mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(single_pkt);
            vlan_header = mbuf_priv->vlan_hdr;
            eth_hdr = mbuf_priv->eth_hdr;
            ccb_id = mbuf_priv->ccb_id;

            ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
            if (unlikely(rte_atomic16_read(&ppp_ccb->dp_start_bool) == (BIT16)0)) {
                drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                continue;
            }

            /* Strip PPPoE header */
            vlan_header->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IP);
            rte_memcpy(&tmp_eth_hdr, eth_hdr, sizeof(struct rte_ether_hdr));
            rte_memcpy(&tmp_vlan_header, vlan_header, sizeof(vlan_header_t));
            rte_memcpy((char *)eth_hdr+pppoe_len, &tmp_eth_hdr, sizeof(struct rte_ether_hdr));
            rte_memcpy((char *)vlan_header+pppoe_len, &tmp_vlan_header, sizeof(vlan_header_t));
            single_pkt->data_off += pppoe_len;
            single_pkt->pkt_len -= pppoe_len;
            single_pkt->data_len -= pppoe_len;
            eth_hdr = (struct rte_ether_hdr *)((char *)eth_hdr + pppoe_len);
            vlan_header = (vlan_header_t *)(eth_hdr + 1);
            mbuf_priv->vlan_hdr = vlan_header;
            mbuf_priv->eth_hdr = eth_hdr;

            /* NAT reverse + decapsulation */
            ip_hdr = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(single_pkt, 
                unsigned char *) + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t));
            ip_hdr->hdr_checksum = 0;
            single_pkt->l2_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t);
            single_pkt->l3_len = sizeof(struct rte_ipv4_hdr);
            if (ip_hdr->next_proto_id == PROTO_TYPE_UDP) {
                struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
                if (udp_hdr->src_port == rte_cpu_to_be_16(DNS_PORT)) {
                    /* Forward DNS response to control plane for proxy handling */
                    dhcp_ccb_t *dns_dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
                    if (dns_dhcp_ccb != NULL && dns_dhcp_ccb->dns_state.dns_proxy_enabled) {
                        send2cp(fastrg_ccb, single_pkt, EV_DP_DNS, WAN_PORT);
                        continue;
                    }
                }
                pkt_num = decaps_udp(fastrg_ccb, single_pkt, eth_hdr,
                    vlan_header, ip_hdr, ccb_id, tx_q);
            } else if (ip_hdr->next_proto_id == PROTO_TYPE_TCP) {
                pkt_num = decaps_tcp(fastrg_ccb, single_pkt, eth_hdr, 
                    vlan_header, ip_hdr, ccb_id, tx_q);
            } else {
                drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                continue;
            }
            for(int j=0; j<pkt_num; j++) {
                pkt[total_tx++] = single_pkt;
                single_pkt = single_pkt->next;
            }
        }
        if (likely(total_tx > 0)) {
            U16 nb_tx = rte_eth_tx_burst(LAN_PORT, tx_q, pkt, total_tx);
            if (unlikely(nb_tx < total_tx)) {
                for(U16 buf=nb_tx; buf<total_tx; buf++) {
                    mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(pkt[buf]);
                    U16 ccb_id = mbuf_priv->ccb_id;
                    drop_packet(fastrg_ccb, pkt[buf], LAN_PORT, ccb_id);
                }
            }
            total_tx = 0;
        }
    }
    return 0;
}

/**
 * lan_ctrl_rx - LAN port queue 0 handler.
 *
 * Handles non-TCP/UDP traffic that arrives on the LAN default queue 0
 * (everything not matching the RSS flow rules eth/vlan/ipv4/tcp|udp):
 *   - ARP to gateway IP → reply directly, others → TX to WAN
 *   - PPPoE pass-through → forward to WAN
 *   - IP ICMP to gateway → echo reply on LAN
 *   - IP ICMP to WAN → NAT + PPPoE encap + TX to WAN
 *   - IGMP → forward to WAN
 *   - Unknown / other → drop
 */
int lan_ctrl_rx(void *arg)
{
    FastRG_t             *fastrg_ccb = (FastRG_t *)arg;
    const U16            rx_q = 0;  /* always queue 0 */
    const U16            tx_q = 0;  /* TX queue on both WAN and LAN */
    struct rte_mbuf      *single_pkt;
    uint64_t             total_wan_tx = 0;
    struct rte_ether_hdr *eth_hdr;
    vlan_header_t        *vlan_header;
    struct rte_ipv4_hdr  *ip_hdr;
    struct rte_icmp_hdr  *icmphdr;
    struct rte_mbuf      *wan_pkt[BURST_SIZE]; /* batch for WAN TX */
    char                 *cur;
    pppoe_header_t       *pppoe_header;
    U16                  nb_rx, ccb_id;
    U16                  pppoe_len = sizeof(pppoe_header_t) + sizeof(ppp_payload_t);

    rte_thread_t thread_id = rte_thread_self();
    rte_thread_set_name(thread_id, "fastrg_lan_ctrl");

    while(rte_atomic16_read(&start_flag) == 0)
        rte_pause();

    struct rte_mbuf *rx_pkt[BURST_SIZE];
    while(likely(rte_atomic16_read(&stop_flag) == 0)) {
        nb_rx = rte_eth_rx_burst(LAN_PORT, rx_q, rx_pkt, BURST_SIZE);
        for(int i=0; i<nb_rx; i++) {
            single_pkt = rx_pkt[i];
            rte_prefetch0(rte_pktmbuf_mtod(single_pkt, void *));
            if (unlikely(parse_l2_hdr(fastrg_ccb, single_pkt, LAN_PORT) == ERROR)) {
                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, fastrg_ccb->user_count);
                continue;
            }
            mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(single_pkt);
            vlan_header = mbuf_priv->vlan_hdr;
            eth_hdr = mbuf_priv->eth_hdr;
            ccb_id = mbuf_priv->ccb_id;
            U32 dhcp_server_ip = mbuf_priv->dhcp_server_ip;
            U32 subnet_mask = mbuf_priv->dhcp_subnet_mask;

            /* ---- ARP ---- */
            if (unlikely(vlan_header->next_proto == rte_cpu_to_be_16(FRAME_TYPE_ARP))) {
                struct rte_arp_hdr *arphdr = (struct rte_arp_hdr *)(rte_pktmbuf_mtod(single_pkt,
                    unsigned char *) + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t));

                /* Learn MAC from any ARP source (sender HW/IP always valid) */
                ppp_ccb_t *ppp_ccb_arp = PPPD_GET_CCB(fastrg_ccb, ccb_id);
                if (is_ip_in_range(arphdr->arp_data.arp_sip, dhcp_server_ip, subnet_mask)) {
                    mac_table_learn(ppp_ccb_arp->mac_table,
                        arphdr->arp_data.arp_sip, &arphdr->arp_data.arp_sha);
                }

                /* ARP request to gateway IP → reply directly */
                if (arphdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST) &&
                        arphdr->arp_data.arp_tip == dhcp_server_ip) {
                    rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
                    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);
                    rte_ether_addr_copy(&arphdr->arp_data.arp_sha, &arphdr->arp_data.arp_tha);
                    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &arphdr->arp_data.arp_sha);
                    arphdr->arp_data.arp_tip = arphdr->arp_data.arp_sip;
                    arphdr->arp_data.arp_sip = dhcp_server_ip;
                    arphdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
                    count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    rte_eth_tx_burst(LAN_PORT, tx_q, &single_pkt, 1);
                } else if (arphdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REPLY) &&
                        arphdr->arp_data.arp_tip == dhcp_server_ip) {
                    /* ARP reply to us → drain pending queue */
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    struct rte_mbuf *drain_pkts[ARP_PENDING_QUEUE_SIZE];
                    U16 drain_count = 0;
                    arp_pending_drain(fastrg_ccb->arp_pending_mp,
                        &ppp_ccb_arp->arp_pq, arphdr->arp_data.arp_sip,
                        &arphdr->arp_data.arp_sha,
                        drain_pkts, &drain_count, ARP_PENDING_QUEUE_SIZE);
                    if (drain_count > 0) {
                        U16 nb_tx = rte_eth_tx_burst(LAN_PORT, tx_q,
                            drain_pkts, drain_count);
                        for(U16 d=0; d<nb_tx; d++)
                            count_tx_packet(fastrg_ccb, drain_pkts[d], LAN_PORT, ccb_id);
                        for(U16 d=nb_tx; d<drain_count; d++)
                            drop_packet(fastrg_ccb, drain_pkts[d], LAN_PORT, ccb_id);
                    }
                    rte_pktmbuf_free(single_pkt);
                } else {
                    /* ARP to other → forward to WAN */
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    rte_eth_tx_burst(WAN_PORT, tx_q, &single_pkt, 1);
                }
                continue;
            }

            /* ---- PPPoE pass-through ---- */
            if (unlikely(vlan_header->next_proto == rte_cpu_to_be_16(ETH_P_PPP_DIS) || 
                    (vlan_header->next_proto == rte_cpu_to_be_16(ETH_P_PPP_SES)))) {
                #ifdef TEST_MODE
                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                continue;
                #else
                count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                wan_pkt[total_wan_tx++] = single_pkt;
                #endif
                continue;
            }

            /* ---- IPv4 ---- */
            if (likely(vlan_header->next_proto == rte_cpu_to_be_16(FRAME_TYPE_IP))) {
                ip_hdr = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(single_pkt, unsigned char *) + 
                    sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t));
                ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
                /* Learn MAC from IP packet (before headers are modified) */
                if (is_ip_in_range(ip_hdr->src_addr, dhcp_server_ip, subnet_mask)) {
                    mac_table_learn(ppp_ccb->mac_table,
                        ip_hdr->src_addr, &eth_hdr->src_addr);
                }

                /* Gateway subnet: only ICMP expected on queue 0 */
                if (unlikely(is_ip_in_range(ip_hdr->dst_addr, dhcp_server_ip, subnet_mask))) {
                    if (ip_hdr->next_proto_id == PROTO_TYPE_ICMP) {
                        icmphdr = (struct rte_icmp_hdr *)(rte_pktmbuf_mtod(single_pkt, unsigned char *) + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(struct rte_ipv4_hdr));
                        if (ip_hdr->dst_addr == dhcp_server_ip) {
                            if (icmphdr->icmp_type != ICMP_ECHO_REQUEST) {
                                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                                continue;
                            }
                            rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
                            rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);
                            ip_hdr->dst_addr = ip_hdr->src_addr;
                            ip_hdr->src_addr = dhcp_server_ip;
                            icmphdr->icmp_type = 0;
                            U32 cksum = ~icmphdr->icmp_cksum & 0xffff;
                            cksum += ~rte_cpu_to_be_16(8 << 8) & 0xffff;
                            cksum += rte_cpu_to_be_16(0 << 8);
                            cksum = (cksum & 0xffff) + (cksum >> 16);
                            cksum = (cksum & 0xffff) + (cksum >> 16);
                            icmphdr->icmp_cksum = ~cksum;
                            rte_eth_tx_burst(LAN_PORT, tx_q, &single_pkt, 1);
                        } else {
                            /* ICMP to other host in subnet → forward on LAN */
                            rte_eth_tx_burst(LAN_PORT, tx_q, &single_pkt, 1);
                        }
                    } else {
                        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    }
                    continue;
                }

                /* ---- WAN-bound traffic ---- */
                single_pkt->l2_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t) + sizeof(ppp_payload_t);
                single_pkt->l3_len = sizeof(struct rte_ipv4_hdr);

                if (ip_hdr->next_proto_id == PROTO_TYPE_ICMP) {
                    if (unlikely(!rte_is_same_ether_addr(&eth_hdr->dst_addr, &fastrg_ccb->nic_info.hsi_lan_mac))) {
                        count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                        wan_pkt[total_wan_tx++] = single_pkt;
                        continue;
                    }
                    icmphdr = (struct rte_icmp_hdr *)(rte_pktmbuf_mtod(single_pkt, unsigned char *) + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(struct rte_ipv4_hdr));
                    if (unlikely(rte_atomic16_read(&ppp_ccb->dp_start_bool) == (BIT16)0)) {
                        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        continue;
                    }
                    /* NAT + PPPoE encapsulate ICMP */
                    U16 new_port_id;
                    U32 icmp_new_cksum;
                    U16 ori_ident = icmphdr->icmp_ident;

                    new_port_id = nat_icmp_learning(eth_hdr, ip_hdr, icmphdr, 
                        ppp_ccb->addr_table, ppp_ccb->port_fwd_table);
                    if (unlikely(new_port_id == 0)) {
                        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        continue;
                    }
                    ip_hdr->src_addr = ppp_ccb->hsi_ipv4;
                    icmphdr->icmp_ident = new_port_id;
                    ip_hdr->hdr_checksum = 0;
                    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

                    if (((icmp_new_cksum = icmphdr->icmp_cksum + ori_ident - new_port_id) >> 16) != 0)
                        icmp_new_cksum = (icmp_new_cksum & 0xFFFF) + (icmp_new_cksum >> 16);
                    icmphdr->icmp_cksum = (U16)icmp_new_cksum;

                    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
                    rte_ether_addr_copy(&ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);

                    vlan_header->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);
                    cur = (char *)eth_hdr - pppoe_len;
                    rte_memcpy(cur, eth_hdr, sizeof(struct rte_ether_hdr));
                    rte_memcpy(cur+sizeof(struct rte_ether_hdr), vlan_header, sizeof(vlan_header_t));
                    pppoe_header = (pppoe_header_t *)(cur + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t));
                    pppoe_header->ver_type = VER_TYPE;
                    pppoe_header->code = 0;
                    pppoe_header->session_id = ppp_ccb->session_id;
                    pppoe_header->length = rte_cpu_to_be_16((single_pkt->pkt_len) - 
                        (sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t)) + sizeof(ppp_payload_t));
                    *((U16 *)(cur + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + 
                        sizeof(pppoe_header_t))) = rte_cpu_to_be_16(PPP_IP_PROTOCOL);
                    single_pkt->data_off -= pppoe_len;
                    single_pkt->pkt_len += pppoe_len;
                    single_pkt->data_len += pppoe_len;
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    increase_pppoes_tx_count(ppp_ccb, single_pkt->pkt_len);
                    wan_pkt[total_wan_tx++] = single_pkt;
                } else if (ip_hdr->next_proto_id == IPPROTO_IGMP) {
                    #ifdef TEST_MODE
                    drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    continue;
                    #else
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    wan_pkt[total_wan_tx++] = single_pkt;
                    #endif
                } else {
                    FastRG_LOG(DBG, fastrg_ccb->fp, NULL, NULL, "unknown L4 packet with protocol id %x recv on LAN ctrl queue", ip_hdr->next_proto_id);
                    drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                }
            } else {
                FastRG_LOG(DBG, fastrg_ccb->fp, NULL, NULL, "unknown ether type %x recv on LAN ctrl queue", rte_be_to_cpu_16(vlan_header->next_proto));
                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                continue;
            }
        }
        if (likely(total_wan_tx > 0)) {
            U16 nb_tx = rte_eth_tx_burst(WAN_PORT, tx_q, wan_pkt, total_wan_tx);
            if (unlikely(nb_tx < total_wan_tx)) {
                for(U16 buf=nb_tx; buf<total_wan_tx; buf++) {
                    mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(wan_pkt[buf]);
                    U16 ccb_id = mbuf_priv->ccb_id;
                    drop_packet(fastrg_ccb, wan_pkt[buf], WAN_PORT, ccb_id);
                }
            }
            total_wan_tx = 0;
        }
    }
    return 0;
}

/**
 * lan_data_rx - LAN port queues 1..N handler (one thread per RSS queue).
 *
 * Handles IPv4 TCP/UDP traffic distributed by RSS flow rules:
 *   - TCP: NAT + PPPoE encapsulation → TX to WAN
 *   - UDP: DHCP → handle locally; others → NAT + PPPoE encap → TX to WAN
 */
int lan_data_rx(void *arg)
{
    dp_rx_arg_t          *rx_arg = (dp_rx_arg_t *)arg;
    FastRG_t             *fastrg_ccb = rx_arg->fastrg_ccb;
    const U16            rx_q = rx_arg->rx_queue_id;
    const U16            tx_q = rx_arg->tx_queue_id; /* TX queue on both WAN and LAN */
    struct rte_mbuf      *single_pkt;
    uint64_t             total_wan_tx = 0;
    struct rte_ether_hdr *eth_hdr;
    vlan_header_t        *vlan_header;
    struct rte_ipv4_hdr  *ip_hdr;
    struct rte_mbuf      *wan_pkt[BURST_SIZE]; /* batch for WAN TX */
    U16                  nb_rx, ccb_id;
    char                 thread_name[32];
    int                  pkt_num;

    snprintf(thread_name, sizeof(thread_name), "fastrg_lan_%u", rx_q);
    rte_thread_t thread_id = rte_thread_self();
    rte_thread_set_name(thread_id, thread_name);

    while(rte_atomic16_read(&start_flag) == 0)
        rte_pause();

    struct rte_mbuf *rx_pkt[BURST_SIZE];
    while(likely(rte_atomic16_read(&stop_flag) == 0)) {
        nb_rx = rte_eth_rx_burst(LAN_PORT, rx_q, rx_pkt, BURST_SIZE);
        for(int i=0; i<nb_rx; i++) {
            single_pkt = rx_pkt[i];
            rte_prefetch0(rte_pktmbuf_mtod(single_pkt, void *));
            if (unlikely(parse_l2_hdr(fastrg_ccb, single_pkt, LAN_PORT) == ERROR)) {
                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, fastrg_ccb->user_count);
                continue;
            }
            mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(single_pkt);
            vlan_header = mbuf_priv->vlan_hdr;
            eth_hdr = mbuf_priv->eth_hdr;
            ccb_id = mbuf_priv->ccb_id;
            U32 dhcp_server_ip = mbuf_priv->dhcp_server_ip;
            U32 subnet_mask = mbuf_priv->dhcp_subnet_mask;

            /* Only IPv4 TCP/UDP expected from RSS flow rules */
            if (unlikely(vlan_header->next_proto != rte_cpu_to_be_16(FRAME_TYPE_IP))) {
                FastRG_LOG(DBG, fastrg_ccb->fp, NULL, NULL, "unexpected ether type %x on LAN data queue %u", rte_be_to_cpu_16(vlan_header->next_proto), rx_q);
                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                continue;
            }

            ip_hdr = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(single_pkt, unsigned char *) + 
                sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t));

            /* Gateway subnet traffic: DHCP (UDP port 67) and DNS (port 53) */
            if (unlikely(is_ip_in_range(ip_hdr->dst_addr, dhcp_server_ip, subnet_mask))) {
                if (ip_hdr->next_proto_id == PROTO_TYPE_UDP) {
                    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
                    if (udp_hdr->dst_port == rte_be_to_cpu_16(DHCP_SERVER_PORT)) {
                        /* Forward DHCP to control plane */
                        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
                        if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 0) {
                            drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                            continue;
                        }
                        send2cp(fastrg_ccb, single_pkt, EV_DP_DHCP, LAN_PORT);
                        continue;
                    }
                    if (udp_hdr->dst_port == rte_cpu_to_be_16(DNS_PORT)) {
                        /* Forward DNS (UDP) to control plane */
                        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
                        if (dhcp_ccb->dns_state.dns_proxy_enabled) {
                            send2cp(fastrg_ccb, single_pkt, EV_DP_DNS, LAN_PORT);
                            continue;
                        }
                    }
                } else if (ip_hdr->next_proto_id == PROTO_TYPE_TCP) {
                    struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
                    if (tcp_hdr->dst_port == rte_cpu_to_be_16(DNS_PORT)) {
                        /* Forward DNS (TCP) to control plane */
                        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
                        if (dhcp_ccb->dns_state.dns_proxy_enabled) {
                            send2cp(fastrg_ccb, single_pkt, EV_DP_DNS, LAN_PORT);
                            continue;
                        }
                    }
                }
                /* Other gateway subnet traffic -> drop */
                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                continue;
            }

            /* ---- WAN-bound traffic ---- */
            single_pkt->l2_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t) + sizeof(ppp_payload_t);
            single_pkt->l3_len = sizeof(struct rte_ipv4_hdr);

            ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);

            /* Learn MAC from all WAN-bound LAN traffic */
            if (is_ip_in_range(ip_hdr->src_addr, dhcp_server_ip, subnet_mask)) {
                mac_table_learn(ppp_ccb->mac_table,
                    ip_hdr->src_addr, &eth_hdr->src_addr);
            }

            if (ip_hdr->next_proto_id == PROTO_TYPE_TCP) {
                if (unlikely(!rte_is_same_ether_addr(&eth_hdr->dst_addr, &fastrg_ccb->nic_info.hsi_lan_mac))) {
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    wan_pkt[total_wan_tx++] = single_pkt;
                    continue;
                }
                if (unlikely(rte_atomic16_read(&ppp_ccb->dp_start_bool) == (BIT16)0)) {
                    drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    continue;
                }
                /* NAT + PPPoE encap TCP (inline uplink) */
                ip_hdr = (struct rte_ipv4_hdr *)rte_pktmbuf_adj(single_pkt, 
                    (U16)(sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t)));
                pkt_num = encaps_tcp(fastrg_ccb, &single_pkt, eth_hdr, 
                    vlan_header, ip_hdr, ccb_id, tx_q);
                for(int j=0; j<pkt_num; j++) {
                    wan_pkt[total_wan_tx++] = single_pkt;
                    single_pkt = single_pkt->next;
                }
            } else if (ip_hdr->next_proto_id == PROTO_TYPE_UDP) {
                if (unlikely(RTE_IS_IPV4_MCAST(rte_be_to_cpu_32(ip_hdr->dst_addr)))) {
                    drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    continue;
                }
                struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
                if (unlikely(udp_hdr->dst_port == rte_be_to_cpu_16(DHCP_SERVER_PORT))) {
                    /* Forward DHCP to control plane */
                    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
                    if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 0) {
                        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        continue;
                    }
                    send2cp(fastrg_ccb, single_pkt, EV_DP_DHCP, LAN_PORT);
                    continue;
                }
                if (unlikely(!rte_is_same_ether_addr(&eth_hdr->dst_addr, &fastrg_ccb->nic_info.hsi_lan_mac))) {
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    wan_pkt[total_wan_tx++] = single_pkt;
                    continue;
                }
                if (unlikely(rte_atomic16_read(&ppp_ccb->dp_start_bool) == (BIT16)0)) {
                    drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    continue;
                }
                /* NAT + PPPoE encap UDP (inline uplink) */
                ip_hdr = (struct rte_ipv4_hdr *)rte_pktmbuf_adj(single_pkt, 
                    (U16)(sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t)));
                pkt_num = encaps_udp(fastrg_ccb, &single_pkt, eth_hdr, 
                    vlan_header, ip_hdr, ccb_id, tx_q);
                for(int j=0; j<pkt_num; j++) {
                    wan_pkt[total_wan_tx++] = single_pkt;
                    single_pkt = single_pkt->next;
                }
            } else {
                FastRG_LOG(DBG, fastrg_ccb->fp, NULL, NULL, "unexpected L4 protocol %x on LAN data queue %u", ip_hdr->next_proto_id, rx_q);
                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
            }
        }
        if (likely(total_wan_tx > 0)) {
            U16 nb_tx = rte_eth_tx_burst(WAN_PORT, tx_q, wan_pkt, total_wan_tx);
            if (unlikely(nb_tx < total_wan_tx)) {
                for(U16 buf=nb_tx; buf<total_wan_tx; buf++) {
                    mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(wan_pkt[buf]);
                    U16 ccb_id = mbuf_priv->ccb_id;
                    drop_packet(fastrg_ccb, wan_pkt[buf], WAN_PORT, ccb_id);
                }
            }
            total_wan_tx = 0;
        }
    }
    return 0;
}

/**
 * wan_combined_rx - WAN port single-queue handler for non-ICE PMDs.
 *
 * Merges wan_ctrl_rx (PPPoE control, IPTV, ICMP) and wan_data_rx
 * (PPPoE session TCP/UDP decap) into one thread polling queue 0.
 */
int wan_combined_rx(void *arg)
{
    FastRG_t             *fastrg_ccb = (FastRG_t *)arg;
    const U16            rx_q = 0;
    const U16            tx_q = 0;
    struct rte_mbuf      *single_pkt;
    uint64_t             total_tx = 0;
    struct rte_ether_hdr *eth_hdr, tmp_eth_hdr;
    vlan_header_t        *vlan_header, tmp_vlan_header;
    struct rte_ipv4_hdr  *ip_hdr;
    struct rte_icmp_hdr  *icmphdr;
    struct rte_mbuf      *pkt[BURST_SIZE];
    U16                  nb_rx;
    ppp_payload_t        *ppp_payload;
    U16                  ccb_id;
    U16                  pppoe_len = sizeof(pppoe_header_t) + sizeof(ppp_payload_t);
    int                  pkt_num;

    rte_thread_t thread_id = rte_thread_self();
    rte_thread_set_name(thread_id, "fastrg_wan_comb");

    while(rte_atomic16_read(&start_flag) == 0)
        rte_pause();

    while(likely(rte_atomic16_read(&stop_flag) == 0)) {
        nb_rx = rte_eth_rx_burst(WAN_PORT, rx_q, pkt, BURST_SIZE);
        for(int i=0; i<nb_rx; i++) {
            single_pkt = pkt[i];
            rte_prefetch0(rte_pktmbuf_mtod(single_pkt, void *));
            if (unlikely(parse_l2_hdr(fastrg_ccb, single_pkt, WAN_PORT) == ERROR)) {
                drop_packet(fastrg_ccb, single_pkt, WAN_PORT, fastrg_ccb->user_count);
                continue;
            }
            mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(single_pkt);
            vlan_header = mbuf_priv->vlan_hdr;
            eth_hdr = mbuf_priv->eth_hdr;
            ccb_id = mbuf_priv->ccb_id;

            /* Non-PPPoE → IPTV / multicast handling */
            if (unlikely(vlan_header->next_proto != rte_cpu_to_be_16(ETH_P_PPP_SES) &&
                    vlan_header->next_proto != rte_cpu_to_be_16(ETH_P_PPP_DIS))) {
                if (is_iptv_pkt_need_drop(fastrg_ccb, vlan_header) == TRUE) {
                    drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                } else {
                    count_rx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    pkt[total_tx++] = single_pkt;
                    count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                }
                continue;
            }

            ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
            ppp_payload = ((ppp_payload_t *)((char *)eth_hdr + sizeof(struct rte_ether_hdr) +
                sizeof(vlan_header_t) + sizeof(pppoe_header_t)));

            /* PPPoE Discovery or control protocols → send to CP */
            if (unlikely(vlan_header->next_proto == rte_cpu_to_be_16(ETH_P_PPP_DIS) ||
                    (ppp_payload->ppp_protocol == rte_cpu_to_be_16(LCP_PROTOCOL) ||
                    ppp_payload->ppp_protocol == rte_cpu_to_be_16(PAP_PROTOCOL) ||
                    ppp_payload->ppp_protocol == rte_cpu_to_be_16(IPCP_PROTOCOL)))) {
                if (unlikely(rte_atomic16_read(&ppp_ccb->ppp_bool) == 0)) {
                    drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    continue;
                }
                send2cp(fastrg_ccb, single_pkt, EV_DP_PPPoE, WAN_PORT);
                continue;
            }

            /* PPPoE session data → strip PPPoE header */
            if (unlikely(rte_atomic16_read(&ppp_ccb->dp_start_bool) == (BIT16)0)) {
                drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                continue;
            }

            vlan_header->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IP);
            rte_memcpy(&tmp_eth_hdr, eth_hdr, sizeof(struct rte_ether_hdr));
            rte_memcpy(&tmp_vlan_header, vlan_header, sizeof(vlan_header_t));
            rte_memcpy((char *)eth_hdr+pppoe_len, &tmp_eth_hdr, sizeof(struct rte_ether_hdr));
            rte_memcpy((char *)vlan_header+pppoe_len, &tmp_vlan_header, sizeof(vlan_header_t));
            single_pkt->data_off += pppoe_len;
            single_pkt->pkt_len -= pppoe_len;
            single_pkt->data_len -= pppoe_len;
            eth_hdr = (struct rte_ether_hdr *)((char *)eth_hdr + pppoe_len);
            vlan_header = (vlan_header_t *)(eth_hdr + 1);
            mbuf_priv->vlan_hdr = vlan_header;
            mbuf_priv->eth_hdr = eth_hdr;

            ip_hdr = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(single_pkt,
                unsigned char *) + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t));
            single_pkt->l2_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t);
            single_pkt->l3_len = sizeof(struct rte_ipv4_hdr);

            if (ip_hdr->next_proto_id == PROTO_TYPE_ICMP) {
                /* ICMP echo reply → NAT reverse + TX to LAN */
                icmphdr = (struct rte_icmp_hdr *)(rte_pktmbuf_mtod(single_pkt,
                    unsigned char *) + sizeof(struct rte_ether_hdr) +
                    sizeof(vlan_header_t) + sizeof(struct rte_ipv4_hdr));
                if (icmphdr->icmp_type != ICMP_ECHO_REPLY) {
                    drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    continue;
                }
                addr_table_t *entry = nat_reverse_lookup(icmphdr->icmp_ident,
                    ip_hdr->src_addr, ICMP_ECHO_REQUEST, ppp_ccb->addr_table);
                if (entry == NULL) {
                    drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    continue;
                }
                int32_t icmp_cksum_diff = (int32_t)icmphdr->icmp_ident - (int32_t)entry->src_port;
                U32 icmp_new_cksum;

                rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac,
                    &eth_hdr->src_addr);
                rte_ether_addr_copy(&entry->mac_addr, &eth_hdr->dst_addr);
                ip_hdr->dst_addr = entry->src_ip;
                icmphdr->icmp_ident = entry->src_port;

                if (((icmp_new_cksum = (U32)icmp_cksum_diff + (U32)icmphdr->icmp_cksum) >> 16) != 0)
                    icmp_new_cksum = (icmp_new_cksum & 0xFFFF) + (icmp_new_cksum >> 16);
                icmphdr->icmp_cksum = (U16)icmp_new_cksum;
                ip_hdr->hdr_checksum = 0;
                ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
                pkt[total_tx++] = single_pkt;
                count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                count_rx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                increase_pppoes_rx_count(ppp_ccb, single_pkt->pkt_len);
            } else if (ip_hdr->next_proto_id == PROTO_TYPE_UDP) {
                /* UDP → check DNS proxy first, then NAT reverse decap + TX to LAN */
                struct rte_udp_hdr *udp_hdr_wan = (struct rte_udp_hdr *)(ip_hdr + 1);
                if (udp_hdr_wan->src_port == rte_cpu_to_be_16(DNS_PORT)) {
                    /* Forward DNS response to control plane for proxy handling */
                    dhcp_ccb_t *dns_dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
                    if (dns_dhcp_ccb != NULL && dns_dhcp_ccb->dns_state.dns_proxy_enabled) {
                        send2cp(fastrg_ccb, single_pkt, EV_DP_DNS, WAN_PORT);
                        continue;
                    }
                }
                ip_hdr->hdr_checksum = 0;
                pkt_num = decaps_udp(fastrg_ccb, single_pkt, eth_hdr,
                    vlan_header, ip_hdr, ccb_id, tx_q);
                for(int j=0; j<pkt_num; j++) {
                    pkt[total_tx++] = single_pkt;
                    single_pkt = single_pkt->next;
                }
            } else if (ip_hdr->next_proto_id == PROTO_TYPE_TCP) {
                /* TCP → NAT reverse decap + TX to LAN */
                ip_hdr->hdr_checksum = 0;
                pkt_num = decaps_tcp(fastrg_ccb, single_pkt, eth_hdr,
                    vlan_header, ip_hdr, ccb_id, tx_q);
                for(int j=0; j<pkt_num; j++) {
                    pkt[total_tx++] = single_pkt;
                    single_pkt = single_pkt->next;
                }
            } else {
                drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
            }
        }
        if (likely(total_tx > 0)) {
            U16 nb_tx = rte_eth_tx_burst(LAN_PORT, tx_q, pkt, total_tx);
            if (unlikely(nb_tx < total_tx)) {
                for(U16 buf=nb_tx; buf<total_tx; buf++) {
                    mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(pkt[buf]);
                    U16 ccb_id = mbuf_priv->ccb_id;
                    drop_packet(fastrg_ccb, pkt[buf], LAN_PORT, ccb_id);
                }
            }
            total_tx = 0;
        }
    }
    return 0;
}

/**
 * lan_combined_rx - LAN port single-queue handler for non-ICE PMDs.
 *
 * Merges lan_ctrl_rx (ARP, PPPoE passthrough, ICMP, IGMP) and
 * lan_data_rx (TCP/UDP NAT + PPPoE encap, DHCP) into one thread
 * polling queue 0.
 */
int lan_combined_rx(void *arg)
{
    FastRG_t             *fastrg_ccb = (FastRG_t *)arg;
    const U16            rx_q = 0;
    const U16            tx_q = 0;
    struct rte_mbuf      *single_pkt;
    uint64_t             total_wan_tx = 0;
    struct rte_ether_hdr *eth_hdr;
    vlan_header_t        *vlan_header;
    struct rte_ipv4_hdr  *ip_hdr;
    struct rte_icmp_hdr  *icmphdr;
    struct rte_mbuf      *wan_pkt[BURST_SIZE];
    char                 *cur;
    pppoe_header_t       *pppoe_header;
    U16                  nb_rx, ccb_id;
    U16                  pppoe_len = sizeof(pppoe_header_t) + sizeof(ppp_payload_t);
    int                  pkt_num;

    rte_thread_t thread_id = rte_thread_self();
    rte_thread_set_name(thread_id, "fastrg_lan_comb");

    while(rte_atomic16_read(&start_flag) == 0)
        rte_pause();

    struct rte_mbuf *rx_pkt[BURST_SIZE];
    while(likely(rte_atomic16_read(&stop_flag) == 0)) {
        nb_rx = rte_eth_rx_burst(LAN_PORT, rx_q, rx_pkt, BURST_SIZE);
        for(int i=0; i<nb_rx; i++) {
            single_pkt = rx_pkt[i];
            rte_prefetch0(rte_pktmbuf_mtod(single_pkt, void *));
            if (unlikely(parse_l2_hdr(fastrg_ccb, single_pkt, LAN_PORT) == ERROR)) {
                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, fastrg_ccb->user_count);
                continue;
            }
            mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(single_pkt);
            vlan_header = mbuf_priv->vlan_hdr;
            eth_hdr = mbuf_priv->eth_hdr;
            ccb_id = mbuf_priv->ccb_id;
            U32 dhcp_server_ip = mbuf_priv->dhcp_server_ip;
            U32 subnet_mask = mbuf_priv->dhcp_subnet_mask;

            /* ---- ARP ---- */
            if (unlikely(vlan_header->next_proto == rte_cpu_to_be_16(FRAME_TYPE_ARP))) {
                struct rte_arp_hdr *arphdr = (struct rte_arp_hdr *)(rte_pktmbuf_mtod(single_pkt,
                    unsigned char *) + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t));

                /* Learn MAC from any ARP source */
                ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
                if (is_ip_in_range(arphdr->arp_data.arp_sip, dhcp_server_ip, subnet_mask)) {
                    mac_table_learn(ppp_ccb->mac_table,
                        arphdr->arp_data.arp_sip, &arphdr->arp_data.arp_sha);
                }

                if (arphdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST) &&
                        arphdr->arp_data.arp_tip == dhcp_server_ip) {
                    rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
                    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);
                    rte_ether_addr_copy(&arphdr->arp_data.arp_sha, &arphdr->arp_data.arp_tha);
                    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &arphdr->arp_data.arp_sha);
                    arphdr->arp_data.arp_tip = arphdr->arp_data.arp_sip;
                    arphdr->arp_data.arp_sip = dhcp_server_ip;
                    arphdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
                    count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    rte_eth_tx_burst(LAN_PORT, tx_q, &single_pkt, 1);
                } else if (arphdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REPLY) &&
                        arphdr->arp_data.arp_tip == dhcp_server_ip) {
                    /* ARP reply to us → drain pending queue */
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    struct rte_mbuf *drain_pkts[ARP_PENDING_QUEUE_SIZE];
                    U16 drain_count = 0;
                    arp_pending_drain(fastrg_ccb->arp_pending_mp,
                        &ppp_ccb->arp_pq, arphdr->arp_data.arp_sip,
                        &arphdr->arp_data.arp_sha,
                        drain_pkts, &drain_count, ARP_PENDING_QUEUE_SIZE);
                    if (drain_count > 0) {
                        U16 nb_tx = rte_eth_tx_burst(LAN_PORT, tx_q,
                            drain_pkts, drain_count);
                        for(U16 d=0; d<nb_tx; d++)
                            count_tx_packet(fastrg_ccb, drain_pkts[d], LAN_PORT, ccb_id);
                        for(U16 d=nb_tx; d<drain_count; d++)
                            drop_packet(fastrg_ccb, drain_pkts[d], LAN_PORT, ccb_id);
                    }
                    rte_pktmbuf_free(single_pkt);
                } else {
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    rte_eth_tx_burst(WAN_PORT, tx_q, &single_pkt, 1);
                }
                continue;
            }

            /* ---- PPPoE pass-through ---- */
            if (unlikely(vlan_header->next_proto == rte_cpu_to_be_16(ETH_P_PPP_DIS) ||
                    (vlan_header->next_proto == rte_cpu_to_be_16(ETH_P_PPP_SES)))) {
                #ifdef TEST_MODE
                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                continue;
                #else
                count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                wan_pkt[total_wan_tx++] = single_pkt;
                #endif
                continue;
            }

            /* ---- IPv4 ---- */
            if (likely(vlan_header->next_proto == rte_cpu_to_be_16(FRAME_TYPE_IP))) {
                ip_hdr = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(single_pkt, unsigned char *) +
                    sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t));
                ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
                /* Learn MAC from IP packet (before headers are modified) */
                if (is_ip_in_range(ip_hdr->src_addr, dhcp_server_ip, subnet_mask)) {
                    mac_table_learn(ppp_ccb->mac_table,
                        ip_hdr->src_addr, &eth_hdr->src_addr);
                }
                /* Gateway subnet handling */
                if (is_ip_in_range(ip_hdr->dst_addr, dhcp_server_ip, subnet_mask)) {
                    if (ip_hdr->next_proto_id == PROTO_TYPE_ICMP) {
                        icmphdr = (struct rte_icmp_hdr *)(rte_pktmbuf_mtod(single_pkt,
                            unsigned char *) + sizeof(struct rte_ether_hdr) +
                            sizeof(vlan_header_t) + sizeof(struct rte_ipv4_hdr));
                        if (ip_hdr->dst_addr == dhcp_server_ip) {
                            rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
                            rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);
                            ip_hdr->dst_addr = ip_hdr->src_addr;
                            ip_hdr->src_addr = dhcp_server_ip;
                            icmphdr->icmp_type = 0;
                            U32 cksum = ~icmphdr->icmp_cksum & 0xffff;
                            cksum += ~rte_cpu_to_be_16(8 << 8) & 0xffff;
                            cksum += rte_cpu_to_be_16(0 << 8);
                            cksum = (cksum & 0xffff) + (cksum >> 16);
                            cksum = (cksum & 0xffff) + (cksum >> 16);
                            icmphdr->icmp_cksum = ~cksum;
                            rte_eth_tx_burst(LAN_PORT, tx_q, &single_pkt, 1);
                        } else {
                            rte_eth_tx_burst(LAN_PORT, tx_q, &single_pkt, 1);
                        }
                    } else if (ip_hdr->next_proto_id == PROTO_TYPE_UDP) {
                        /* DHCP and DNS on gateway subnet */
                        struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
                        if (udp_hdr->dst_port == rte_be_to_cpu_16(DHCP_SERVER_PORT)) {
                            /* Forward DHCP to control plane */
                            dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
                            if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 0) {
                                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                                continue;
                            }
                            send2cp(fastrg_ccb, single_pkt, EV_DP_DHCP, LAN_PORT);
                        } else if (udp_hdr->dst_port == rte_cpu_to_be_16(DNS_PORT)) {
                            /* Forward DNS (UDP) to control plane */
                            dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
                            if (dhcp_ccb->dns_state.dns_proxy_enabled) {
                                send2cp(fastrg_ccb, single_pkt, EV_DP_DNS, LAN_PORT);
                            } else {
                                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                            }
                        } else {
                            drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        }
                    } else if (ip_hdr->next_proto_id == PROTO_TYPE_TCP) {
                        struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
                        if (tcp_hdr->dst_port == rte_cpu_to_be_16(DNS_PORT)) {
                            /* Forward DNS (TCP) to control plane */
                            dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
                            if (dhcp_ccb->dns_state.dns_proxy_enabled) {
                                send2cp(fastrg_ccb, single_pkt, EV_DP_DNS, LAN_PORT);
                            } else {
                                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                            }
                        } else {
                            drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        }
                    } else {
                        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    }
                    continue;
                }

                /* ---- WAN-bound traffic ---- */
                single_pkt->l2_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
                    sizeof(pppoe_header_t) + sizeof(ppp_payload_t);
                single_pkt->l3_len = sizeof(struct rte_ipv4_hdr);

                if (ip_hdr->next_proto_id == PROTO_TYPE_ICMP) {
                    /* ICMP to WAN → NAT + PPPoE encap */
                    if (unlikely(!rte_is_same_ether_addr(&eth_hdr->dst_addr, &fastrg_ccb->nic_info.hsi_lan_mac))) {
                        count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                        wan_pkt[total_wan_tx++] = single_pkt;
                        continue;
                    }
                    icmphdr = (struct rte_icmp_hdr *)(rte_pktmbuf_mtod(single_pkt,
                        unsigned char *) + sizeof(struct rte_ether_hdr) +
                        sizeof(vlan_header_t) + sizeof(struct rte_ipv4_hdr));
                    if (unlikely(rte_atomic16_read(&ppp_ccb->dp_start_bool) == (BIT16)0)) {
                        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        continue;
                    }
                    U16 new_port_id;
                    U32 icmp_new_cksum;
                    U16 ori_ident = icmphdr->icmp_ident;

                    new_port_id = nat_icmp_learning(eth_hdr, ip_hdr, icmphdr,
                        ppp_ccb->addr_table, ppp_ccb->port_fwd_table);
                    if (unlikely(new_port_id == 0)) {
                        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        continue;
                    }
                    ip_hdr->src_addr = ppp_ccb->hsi_ipv4;
                    icmphdr->icmp_ident = new_port_id;
                    ip_hdr->hdr_checksum = 0;
                    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

                    if (((icmp_new_cksum = icmphdr->icmp_cksum + ori_ident - new_port_id) >> 16) != 0)
                        icmp_new_cksum = (icmp_new_cksum & 0xFFFF) + (icmp_new_cksum >> 16);
                    icmphdr->icmp_cksum = (U16)icmp_new_cksum;

                    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
                    rte_ether_addr_copy(&ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);

                    vlan_header->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);
                    cur = (char *)eth_hdr - pppoe_len;
                    rte_memcpy(cur, eth_hdr, sizeof(struct rte_ether_hdr));
                    rte_memcpy(cur+sizeof(struct rte_ether_hdr), vlan_header, sizeof(vlan_header_t));
                    pppoe_header = (pppoe_header_t *)(cur + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t));
                    pppoe_header->ver_type = VER_TYPE;
                    pppoe_header->code = 0;
                    pppoe_header->session_id = ppp_ccb->session_id;
                    pppoe_header->length = rte_cpu_to_be_16((single_pkt->pkt_len) -
                        (sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t)) + sizeof(ppp_payload_t));
                    *((U16 *)(cur + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
                        sizeof(pppoe_header_t))) = rte_cpu_to_be_16(PPP_IP_PROTOCOL);
                    single_pkt->data_off -= pppoe_len;
                    single_pkt->pkt_len += pppoe_len;
                    single_pkt->data_len += pppoe_len;
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    increase_pppoes_tx_count(ppp_ccb, single_pkt->pkt_len);
                    wan_pkt[total_wan_tx++] = single_pkt;
                } else if (ip_hdr->next_proto_id == PROTO_TYPE_TCP) {
                    /* TCP → NAT + PPPoE encap */
                    if (unlikely(!rte_is_same_ether_addr(&eth_hdr->dst_addr, &fastrg_ccb->nic_info.hsi_lan_mac))) {
                        count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                        wan_pkt[total_wan_tx++] = single_pkt;
                        continue;
                    }
                    if (unlikely(rte_atomic16_read(&ppp_ccb->dp_start_bool) == (BIT16)0)) {
                        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        continue;
                    }
                    ip_hdr = (struct rte_ipv4_hdr *)rte_pktmbuf_adj(single_pkt,
                        (U16)(sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t)));
                    pkt_num = encaps_tcp(fastrg_ccb, &single_pkt, eth_hdr,
                        vlan_header, ip_hdr, ccb_id, tx_q);
                    for(int j=0; j<pkt_num; j++) {
                        wan_pkt[total_wan_tx++] = single_pkt;
                        single_pkt = single_pkt->next;
                    }
                } else if (ip_hdr->next_proto_id == PROTO_TYPE_UDP) {
                    /* UDP → DHCP or NAT + PPPoE encap */
                    if (unlikely(RTE_IS_IPV4_MCAST(rte_be_to_cpu_32(ip_hdr->dst_addr)))) {
                        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        continue;
                    }
                    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
                    if (unlikely(udp_hdr->dst_port == rte_be_to_cpu_16(DHCP_SERVER_PORT))) {
                        /* Forward DHCP to control plane */
                        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
                        if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 0) {
                            drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                            continue;
                        }
                        send2cp(fastrg_ccb, single_pkt, EV_DP_DHCP, LAN_PORT);
                        continue;
                    }
                    if (unlikely(!rte_is_same_ether_addr(&eth_hdr->dst_addr, &fastrg_ccb->nic_info.hsi_lan_mac))) {
                        count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                        wan_pkt[total_wan_tx++] = single_pkt;
                        continue;
                    }
                    if (unlikely(rte_atomic16_read(&ppp_ccb->dp_start_bool) == (BIT16)0)) {
                        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                        continue;
                    }
                    ip_hdr = (struct rte_ipv4_hdr *)rte_pktmbuf_adj(single_pkt,
                        (U16)(sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t)));
                    pkt_num = encaps_udp(fastrg_ccb, &single_pkt, eth_hdr,
                        vlan_header, ip_hdr, ccb_id, tx_q);
                    for(int j=0; j<pkt_num; j++) {
                        wan_pkt[total_wan_tx++] = single_pkt;
                        single_pkt = single_pkt->next;
                    }
                } else if (ip_hdr->next_proto_id == IPPROTO_IGMP) {
                    #ifdef TEST_MODE
                    drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    continue;
                    #else
                    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                    count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
                    wan_pkt[total_wan_tx++] = single_pkt;
                    #endif
                } else {
                    FastRG_LOG(DBG, fastrg_ccb->fp, NULL, NULL,
                        "unknown L4 packet with protocol id %x recv on LAN combined queue",
                        ip_hdr->next_proto_id);
                    drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                }
            } else {
                FastRG_LOG(DBG, fastrg_ccb->fp, NULL, NULL,
                    "unknown ether type %x recv on LAN combined queue",
                    rte_be_to_cpu_16(vlan_header->next_proto));
                drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                continue;
            }
        }
        if (likely(total_wan_tx > 0)) {
            U16 nb_tx = rte_eth_tx_burst(WAN_PORT, tx_q, wan_pkt, total_wan_tx);
            if (unlikely(nb_tx < total_wan_tx)) {
                for(U16 buf=nb_tx; buf<total_wan_tx; buf++) {
                    mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(wan_pkt[buf]);
                    U16 ccb_id = mbuf_priv->ccb_id;
                    drop_packet(fastrg_ccb, wan_pkt[buf], WAN_PORT, ccb_id);
                }
            }
            total_wan_tx = 0;
        }
    }
    return 0;
}

void wan_ctrl_tx(FastRG_t *fastrg_ccb, U16 ccb_id, U8 *mu, U16 mulen)
{
    struct rte_mbuf *pkt;
    unsigned char *buf;

    pkt = rte_pktmbuf_alloc(direct_pool[0]);
    if (pkt == NULL) {
        {
            struct per_ccb_stats *__stats = OPENRG_GET_PER_SUBSCRIBER_STATS(fastrg_ccb, WAN_PORT, ccb_id);
            if (likely(__stats)) increase_ccb_drop_count(__stats, mulen);
        };
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "wan_ctrl_tx failed: rte_pktmbuf_alloc failed: %s\n", rte_strerror(rte_errno));
        return;
    }
    buf = rte_pktmbuf_mtod(pkt, unsigned char *);
    rte_memcpy(buf, mu, mulen);
    pkt->data_len = mulen;
    pkt->pkt_len = mulen;
    count_tx_packet(fastrg_ccb, pkt, WAN_PORT, ccb_id);
    if (rte_eth_tx_burst(WAN_PORT, 0, &pkt, 1) == 0)
        drop_packet(fastrg_ccb, pkt, WAN_PORT, ccb_id);
}

void lan_ctrl_tx(FastRG_t *fastrg_ccb, U16 ccb_id, U8 *mu, U16 mulen)
{
    struct rte_mbuf *pkt;
    unsigned char *buf;

    pkt = rte_pktmbuf_alloc(direct_pool[0]);
    if (pkt == NULL) {
        {
            struct per_ccb_stats *__stats = OPENRG_GET_PER_SUBSCRIBER_STATS(fastrg_ccb, LAN_PORT, ccb_id);
            if (likely(__stats)) increase_ccb_drop_count(__stats, mulen);
        };
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "lan_ctrl_tx failed: rte_pktmbuf_alloc failed: %s\n", rte_strerror(rte_errno));
        return;
    }
    buf = rte_pktmbuf_mtod(pkt, unsigned char *);
    rte_memcpy(buf, mu, mulen);
    pkt->data_len = mulen;
    pkt->pkt_len = mulen;
    count_tx_packet(fastrg_ccb, pkt, LAN_PORT, ccb_id);
    if (rte_eth_tx_burst(LAN_PORT, 0, &pkt, 1) == 0)
        drop_packet(fastrg_ccb, pkt, LAN_PORT, ccb_id);
}

static int lsi_event_callback(U16 port_id, enum rte_eth_event_type type, void *param)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)param;
    struct rte_eth_link link = { 0 };
    tFastRG_MBX *mail = fastrg_malloc(tFastRG_MBX, sizeof(tFastRG_MBX), 2048);
    if (mail == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "lsi_event_callback failed: fastrg_malloc failed\n");
        return -1;
    }

    FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "\n\nIn registered callback...\n");
    FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Event type: %s\n", 
        type == RTE_ETH_EVENT_INTR_LSC ? "LSC interrupt" : "unknown event");
    int link_get_err = rte_eth_link_get_nowait(port_id, &link);
    if (link_get_err != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "lsi_event_callback failed: rte_eth_link_get_nowait failed: %s\n", 
            strerror(-link_get_err));
        fastrg_mfree(mail);
        return -1;
    }
    if (link.link_status) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Port %d Link Up - speed %u Mbps - %s\n\n",
                port_id, (unsigned)link.link_speed,
            (link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ?
                ("full-duplex") : ("half-duplex"));
        mail->refp[0] = LINK_UP;
    } else {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Port %d Link Down\n\n", port_id);
        mail->refp[0] = LINK_DOWN;
    }
    *(U16 *)&(mail->refp[1]) = port_id;
    mail->type = EV_LINK;
    mail->len = 1;
    //enqueue down event to main thread
    rte_ring_enqueue(cp_q, (void *)mail);

    return 0;
}
