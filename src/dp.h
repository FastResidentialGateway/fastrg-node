/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DP.H

  Designed by THE on JAN 21, 2021
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DP_H_
#define _DP_H_

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

void wan_ctrl_tx(FastRG_t *fastrg_ccb, U16 ccb_id, U8 *mu, U16 mulen);
int wan_ctrl_rx(void *arg);
int wan_data_rx(void *arg);
int lan_ctrl_rx(void *arg);
int lan_data_rx(void *arg);
STATUS PORT_INIT(FastRG_t *fastrg_ccb, U16 port);

typedef struct mbuf_priv {
    U16 ccb_id;
    U32 dhcp_server_ip;
    U32 dhcp_subnet_mask;
    struct rte_ether_hdr *eth_hdr;
    vlan_header_t *vlan_hdr;
} mbuf_priv_t;

static inline void increase_ccb_drop_count(struct per_ccb_stats *stats, U32 pkt_len)
{
    rte_atomic64_inc(&stats->dropped_packets);
    rte_atomic64_add(&stats->dropped_bytes, pkt_len);
}

static inline void increase_ccb_rx_count(struct per_ccb_stats *stats, U32 pkt_len)
{
    rte_atomic64_inc(&stats->rx_packets);
    rte_atomic64_add(&stats->rx_bytes, pkt_len);
}

static inline void increase_ccb_tx_count(struct per_ccb_stats *stats, U32 pkt_len)
{
    rte_atomic64_inc(&stats->tx_packets);
    rte_atomic64_add(&stats->tx_bytes, pkt_len);
}

static inline void increase_pppoes_tx_count(ppp_ccb_t *ppp_ccb, U32 pkt_len)
{
    rte_atomic64_inc(&ppp_ccb->pppoes_tx_packets);
    rte_atomic64_add(&ppp_ccb->pppoes_tx_bytes, pkt_len);
}

static inline void increase_pppoes_rx_count(ppp_ccb_t *ppp_ccb, U32 pkt_len)
{
    rte_atomic64_inc(&ppp_ccb->pppoes_rx_packets);
    rte_atomic64_add(&ppp_ccb->pppoes_rx_bytes, pkt_len);
}

static inline void drop_packet(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    U8 port_id, U16 ccb_id)
{
    struct per_ccb_stats *stats = OPENRG_GET_PER_SUBSCRIBER_STATS(fastrg_ccb, port_id, ccb_id);
    if (likely(stats)) increase_ccb_drop_count(stats, single_pkt->pkt_len);
    rte_pktmbuf_free(single_pkt);
}

static inline void count_rx_packet(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    U8 port_id, U16 ccb_id)
{
    struct per_ccb_stats *stats = OPENRG_GET_PER_SUBSCRIBER_STATS(fastrg_ccb, port_id, ccb_id);
    if (likely(stats)) increase_ccb_rx_count(stats, single_pkt->pkt_len);
}

static inline void count_tx_packet(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    U8 port_id, U16 ccb_id)
{
    struct per_ccb_stats *stats = OPENRG_GET_PER_SUBSCRIBER_STATS(fastrg_ccb, port_id, ccb_id); 
    if (likely(stats)) increase_ccb_tx_count(stats, single_pkt->pkt_len); 
}

#endif /* _DP_H_ */
