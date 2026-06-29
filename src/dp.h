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
