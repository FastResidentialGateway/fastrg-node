/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DP.H

  Designed by THE on JAN 21, 2021
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DP_H_
#define _DP_H_

#include <rte_hash_crc.h>
#include <rte_ip.h>
#include <rte_udp.h>

#include "dhcpd/dhcpd.h"
#include "fastrg.h"
#include "pppd/pppd.h"

/**
 * Thread argument for data-plane RX threads.
 * Each wan_data_rx / lan_rx thread gets its own instance.
 */
typedef struct dp_rx_arg {
    FastRG_t *fastrg_ccb;
    U16 rx_queue_id;    /**< RX queue to poll                       */
    U16 tx_queue_id;    /**< TX queue on opposite port (= rx_queue) */
} dp_rx_arg_t;

/**
 * Thread argument for a software-distributor worker lcore (DP_MODE_DISTRIBUTOR).
 * Each wan_dist_worker / lan_dist_worker thread gets its own instance.
 */
typedef struct dist_worker_arg {
    FastRG_t *fastrg_ccb;
    struct rte_distributor *dist;   /**< distributor instance to pull from   */
    U16 worker_id;                  /**< worker index 0..N-1                 */
    U16 tx_queue_id;                /**< dedicated TX queue (= worker_id + 1) */
} dist_worker_arg_t;

void wan_ctrl_tx(FastRG_t *fastrg_ccb, U16 ccb_id, U8 *mu, U16 mulen);
void lan_ctrl_tx(FastRG_t *fastrg_ccb, U16 ccb_id, U8 *mu, U16 mulen);
int wan_ctrl_rx(void *arg);
int wan_data_rx(void *arg);
int lan_ctrl_rx(void *arg);
int lan_data_rx(void *arg);
int wan_dist_rx(void *arg);
int lan_dist_rx(void *arg);
int wan_dist_worker(void *arg);
int lan_dist_worker(void *arg);
STATUS PORT_INIT(FastRG_t *fastrg_ccb, U16 port);

typedef struct mbuf_priv {
    U16 ccb_id;
    U32 dhcp_server_ip;
    U32 dhcp_subnet_mask;
    struct rte_ether_hdr *eth_hdr;
    vlan_header_t *vlan_hdr;
} mbuf_priv_t;

/* The caller owns this per-lcore slot exclusively, so a plain non-atomic RMW is
 * safe (no other writer) and cheap (no lock prefix, no cache-line bounce). */
static inline void increase_ccb_drop_count(struct per_ccb_stats *stats, U32 pkt_len)
{
    stats->dropped_packets++;
    stats->dropped_bytes += pkt_len;
}

static inline void increase_ccb_rx_count(struct per_ccb_stats *stats, U32 pkt_len)
{
    stats->rx_packets++;
    stats->rx_bytes += pkt_len;
}

static inline void increase_ccb_tx_count(struct per_ccb_stats *stats, U32 pkt_len)
{
    stats->tx_packets++;
    stats->tx_bytes += pkt_len;
}

/* Per-lcore PPPoE session counters: write the caller's own lcore slot with a
 * plain += (no atomic). Slot resolved via ppp_ccb_rcu-protected getter. */
static inline void increase_pppoes_tx_count(FastRG_t *fastrg_ccb, U16 ccb_id, U32 pkt_len)
{
    struct pppoes_lcore_stats *slot = FASTRG_GET_PPPOES_STATS(fastrg_ccb, ccb_id);
    if (likely(slot)) {
        slot->tx_packets++;
        slot->tx_bytes += pkt_len;
    }
}

static inline void increase_pppoes_rx_count(FastRG_t *fastrg_ccb, U16 ccb_id, U32 pkt_len)
{
    struct pppoes_lcore_stats *slot = FASTRG_GET_PPPOES_STATS(fastrg_ccb, ccb_id);
    if (likely(slot)) {
        slot->rx_packets++;
        slot->rx_bytes += pkt_len;
    }
}

static inline void drop_packet(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    U8 port_id, U16 ccb_id)
{
    struct per_ccb_stats *stats = FASTRG_GET_PER_SUBSCRIBER_STATS(fastrg_ccb, port_id, ccb_id);
    if (likely(stats)) increase_ccb_drop_count(stats, single_pkt->pkt_len);
    rte_pktmbuf_free(single_pkt);
}

static inline void count_rx_packet(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    U8 port_id, U16 ccb_id)
{
    struct per_ccb_stats *stats = FASTRG_GET_PER_SUBSCRIBER_STATS(fastrg_ccb, port_id, ccb_id);
    if (likely(stats)) increase_ccb_rx_count(stats, single_pkt->pkt_len);
}

static inline void count_tx_packet(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    U8 port_id, U16 ccb_id)
{
    struct per_ccb_stats *stats = FASTRG_GET_PER_SUBSCRIBER_STATS(fastrg_ccb, port_id, ccb_id); 
    if (likely(stats)) increase_ccb_tx_count(stats, single_pkt->pkt_len); 
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
            } else if (rte_be_to_cpu_16(ip_hdr->total_length) >
                    sizeof(struct rte_udp_hdr) + sizeof(struct rte_ipv4_hdr) && 
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
 * compute_flow_tag - per-direction 5-tuple hash used as the rte_distributor tag.
 *
 * The distributor keeps every packet sharing a tag on a single worker and in
 * order, so deriving the tag from the direction's stable 5-tuple reproduces the
 * same per-flow single-owner affinity that hardware RSS provides in
 * DP_MODE_RSS. Only the low 16 bits matter (the burst distributor masks the tag
 * to a 15-bit flow id and forces it odd), and CRC32 spreads entropy there.
 */
static inline U32 compute_flow_tag(U32 src_ip, U32 dst_ip, U16 src_port,
    U16 dst_port, U8 proto)
{
    U32 h = rte_hash_crc_4byte(src_ip, 0);
    h = rte_hash_crc_4byte(dst_ip, h);
    h = rte_hash_crc_4byte(((U32)src_port << 16) | dst_port, h);
    h = rte_hash_crc_1byte(proto, h);
    return h;
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

    if (rte_ring_dequeue(fastrg_ccb->free_mail_ring, (void **)&slot) == 0) {
        slot->mbuf = single_pkt;
        slot->type = evt_type;
        slot->len = single_pkt->pkt_len;
        slot->ccb_id = ccb_id;
        slot->port_id = port_id;
        /* cp_q is full: return slot to free_mail_ring */
        if (rte_ring_enqueue(fastrg_ccb->cp_q, slot) != 0) {
            rte_ring_enqueue(fastrg_ccb->free_mail_ring, slot);
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
 * @fn fastrg_rcu_dp_register
 *
 * @brief Mark the calling data-plane lcore as persistently QSBR-online for all
 *        three RCUs (ppp_ccb / dhcp_ccb / per_subscriber_stats).  Call once at
 *        data thread startup, after the start_flag spin.  Pairs with
 *        fastrg_rcu_dp_quiescent() called once per poll loop.
 *
 * @param fastrg_ccb
 *      FastRG control block (holds the three qsbr handles)
 */
static inline void fastrg_rcu_dp_register(FastRG_t *fastrg_ccb)
{
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_thread_online(fastrg_ccb->ppp_ccb_rcu, lcore_id);
    rte_rcu_qsbr_thread_online(fastrg_ccb->dhcp_ccb_rcu, lcore_id);
    rte_rcu_qsbr_thread_online(fastrg_ccb->per_subscriber_stats_rcu, lcore_id);
    fastrg_rcu_persistent[lcore_id] = TRUE;   /* online first, then flip flag */
}

/**
 * @fn fastrg_rcu_dp_quiescent
 *
 * @brief Report a quiescent state on all three RCUs for the calling data-plane
 *        lcore.  Call once at the top of each poll loop (after rte_eth_rx_burst /
 *        rte_distributor_get_pkt), before touching any RCU-protected pointer.
 *
 * @param fastrg_ccb
 *      FastRG control block
 */
static inline void fastrg_rcu_dp_quiescent(FastRG_t *fastrg_ccb)
{
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_quiescent(fastrg_ccb->ppp_ccb_rcu, lcore_id);
    rte_rcu_qsbr_quiescent(fastrg_ccb->dhcp_ccb_rcu, lcore_id);
    rte_rcu_qsbr_quiescent(fastrg_ccb->per_subscriber_stats_rcu, lcore_id);
}

/**
 * @fn fastrg_rcu_dp_unregister
 *
 * @brief Report QSBR-offline on all three RCUs for the calling data-plane lcore
 *        and clear its persistent flag.  MUST be called once when the thread
 *        leaves its poll loop (on stop_flag), before returning.  Without this
 *        the thread exits while still recorded online, and the cleanup-path
 *        rte_rcu_qsbr_synchronize() waits on it forever (it never reports
 *        quiescent again) — hanging shutdown and leaking the online state into
 *        the hugepages, which then stalls the next startup.
 *
 * @param fastrg_ccb
 *      FastRG control block
 */
static inline void fastrg_rcu_dp_unregister(FastRG_t *fastrg_ccb)
{
    unsigned int lcore_id = rte_lcore_id();
    fastrg_rcu_persistent[lcore_id] = FALSE;
    rte_rcu_qsbr_thread_offline(fastrg_ccb->ppp_ccb_rcu, lcore_id);
    rte_rcu_qsbr_thread_offline(fastrg_ccb->dhcp_ccb_rcu, lcore_id);
    rte_rcu_qsbr_thread_offline(fastrg_ccb->per_subscriber_stats_rcu, lcore_id);
}

#endif /* _DP_H_ */
