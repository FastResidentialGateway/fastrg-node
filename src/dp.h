/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DP.H

  Designed by THE on JAN 21, 2021
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DP_H_
#define _DP_H_

#include <rte_ethdev.h>
#include <rte_hash_crc.h>
#include <rte_ip.h>
#include <rte_udp.h>

#include "dbg.h"
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

/**
 * @fn PORT_INIT
 * 
 * @brief Initialize a port for data-plane operation.
 * @param fastrg_ccb 
 *      FastRG main control block
 * @param port
 *      port id to initialize
 * @return SUCCESS on success, ERROR on failure
 */
STATUS PORT_INIT(FastRG_t *fastrg_ccb, U16 port);

/**
 * @fn PORT_CLOSE
 * 
 * @brief Shutdown-time counterpart of PORT_INIT. Unregisters the LSC
 *        callback first so a stop-time link-down interrupt can no longer
 *        enqueue an EV_LINK mail, then stops and closes the port so the
 *        driver hands every mbuf still held in its RX/TX queues back to
 *        the mempools before those pools are freed (cleanup_mem).
 * @param fastrg_ccb 
 *      FastRG main control block
 * @param port 
 *      port id to close
 * @return void
 */
void PORT_CLOSE(FastRG_t *fastrg_ccb, U16 port);

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
 * plain += (no atomic). */
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

/* TX ring depth per queue. */
#define TX_RING_SIZE 512

/* ---------------------------------------------------------------------------
 * TX queue ownership
 *
 * One lcore per TX queue: two lcores writing one queue corrupt its ring, so
 * queue numbers are assigned only here, never at the call site.
 *
 * With N data queues, per port:
 *   queue 0      control thread
 *   queue 1..N   the data lcore reading the other port
 *   queue N+1    this port's queue-0 poller, for its own replies
 *   queue N+2    the other port's queue-0 poller, for cross-port forwarding
 *
 * To send on a queue you do not own, hand the packet to
 * send_pkt_to_other_lcore(); the owner drains it with
 * check_pkt_from_other_lcore() into its own next burst.
 * ------------------------------------------------------------------------- */
typedef enum {
    FASTRG_TX_SENDER_CTRL_THREAD = 0, /* only index 0 exists */
    FASTRG_TX_SENDER_WAN_DATA,        /* index = data queue index, 0..N-1 */
    FASTRG_TX_SENDER_LAN_DATA,        /* index = data queue index, 0..N-1 */
    /* The two queue-0 pollers; each owns one queue on both ports. Only
     * index 0 exists, one poller per port. */
    FASTRG_TX_SENDER_LAN_CTRL,        /* polls LAN queue 0 */
    FASTRG_TX_SENDER_WAN_CTRL         /* polls WAN queue 0 */
} fastrg_tx_sender_t;

/* Returned when a sender does not transmit on the port asked about. */
#define FASTRG_TX_QUEUE_NONE 0xFFFF

/* Handoff ring depth; bounded so a slow owner cannot back-pressure the lcore
 * handing the packet over. */
#define TX_HANDOFF_RING_SIZE 256

/**
 * @fn get_tx_queue_count
 *
 * @brief TX queues a port needs for a given number of data queues.
 *
 * @param data_queues
 *      Number of data queues (N)
 * @return
 *      Queue count: N data queues, queue 0, and the two poller queues
 */
U16 get_tx_queue_count(U16 data_queues);

/**
 * @fn get_tx_queue_id_for_sender
 *
 * @brief Get TX queue ID for specific sender.
 *
 * @param sender
 *      Which kind of lcore is asking
 * @param index
 *      Data queue index for the data lcores; must be 0 for every other sender
 * @param port_id
 *      Port being transmitted on
 * @param data_queues
 *      Number of data queues (N)
 * @return
 *      Queue id, or FASTRG_TX_QUEUE_NONE when that sender does not transmit on
 *      that port
 */
U16 get_tx_queue_id_for_sender(fastrg_tx_sender_t sender, U16 index, U16 port_id, U16 data_queues);

/**
 * @fn check_pkt_from_other_lcore
 *
 * @brief Get packets packets handed over from other lcores into this burst.
 *
 * The reason we need to pass pkts to other lcores is because the 
 * TX queue is owned by one lcore only. If another lcore has a packet 
 * to send on that queue, it must hand it over to the owner. The owner 
 * collects those packets from the handoff ring and sends them in its own burst.
 *
 * @param ring
 *      The queue's handoff ring, may be NULL
 * @param batch
 *      Batch being built
 * @param batch_len
 *      Packets already in the batch
 * @param batch_max
 *      Capacity of the batch
 * @return
 *      New batch length
 */
U16 check_pkt_from_other_lcore(struct rte_ring *ring, struct rte_mbuf **batch,
    U16 batch_len, U16 batch_max);

/**
 * @fn dp_tx_handoff_pkt_init
 *
 * @brief Resolve the control TX queues, size the per-queue tables and create
 *        the handoff rings.
 *
 * One ring per data queue. Rings are MP-SC.
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @return
 *      SUCCESS, or ERROR when a control queue is missing from the layout or a
 *      ring cannot be created
 */
STATUS dp_tx_handoff_pkt_init(FastRG_t *fastrg_ccb);

/**
 * @fn dp_tx_handoff_pkt_cleanup
 *
 * @brief Free anything still sitting on the handoff rings and release them.
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @return
 *      void
 */
void dp_tx_handoff_pkt_cleanup(FastRG_t *fastrg_ccb);

static __always_inline BOOL is_tx_queue_valid(FastRG_t *fastrg_ccb, U16 port_id, U16 queue_id)
{
    return port_id < PORT_AMOUNT && queue_id < fastrg_ccb->tx_queue_count[port_id];
}

/* Sum specific TX queue's counters across all lcores into *out. */
static __always_inline void fastrg_sum_tx_queue_stats(
    FastRG_t *fastrg_ccb, U16 port_id, U16 queue_id, struct tx_queue_stats *out)
{
    out->full_packets = 0; out->short_bursts = 0; out->handoff_dropped = 0;
    if (unlikely(!is_tx_queue_valid(fastrg_ccb, port_id, queue_id)))
        return;
    unsigned int lcore;
    RTE_LCORE_FOREACH(lcore) {
        struct tx_queue_stats *row = __atomic_load_n(
            &fastrg_ccb->tx_queue_stats[lcore][port_id], __ATOMIC_ACQUIRE);
        if (row == NULL)
            continue;
        out->full_packets += __atomic_load_n(&row[queue_id].full_packets, __ATOMIC_RELAXED);
        out->short_bursts += __atomic_load_n(&row[queue_id].short_bursts, __ATOMIC_RELAXED);
        out->handoff_dropped += __atomic_load_n(&row[queue_id].handoff_dropped, __ATOMIC_RELAXED);
    }
}

static inline const char *tx_descriptor_status_name(int status)
{
    switch (status) {
    case RTE_ETH_TX_DESC_FULL:   return "FULL";
    case RTE_ETH_TX_DESC_DONE:   return "DONE";
    case RTE_ETH_TX_DESC_UNAVAIL:return "UNAVAIL";
    default:                     return "ERR";
    }
}

/* Count while handoff ring is full, per lcore, per port, per queue */
static __always_inline void count_tx_handoff_drop(FastRG_t *fastrg_ccb, 
    U16 port_id, U16 queue_id)
{
    unsigned int lcore_id = rte_lcore_id();

    if (unlikely(!is_tx_queue_valid(fastrg_ccb, port_id, queue_id) ||
            lcore_id == LCORE_ID_ANY))
        return;
    struct tx_queue_stats *row = fastrg_ccb->tx_queue_stats[lcore_id][port_id];
    if (likely(row != NULL))
        row[queue_id].handoff_dropped += 1;
}

/**
 * @fn count_tx_queue_refused
 *
 * @brief Record a TX burst the queue would not take in full, and describe the
 *        queue's descriptors only at the first time it happens on that queue.
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param port_id
 *      Port the burst was sent on
 * @param queue_id
 *      TX queue the burst was sent on
 * @param offered
 *      Packets handed to rte_eth_tx_burst
 * @param accepted
 *      Packets it took
 *
 * @return void
 */
static inline void count_tx_queue_refused(FastRG_t *fastrg_ccb, U16 port_id, U16 queue_id,
                            U16 offered, U16 accepted)
{
    static const U16 probe_offsets[] = {0, 32, 64, 128, 256, TX_RING_SIZE - 1};
    unsigned int lcore_id = rte_lcore_id();

    if (unlikely(!is_tx_queue_valid(fastrg_ccb, port_id, queue_id) ||
            lcore_id == LCORE_ID_ANY))
        return;

    struct tx_queue_stats *row = fastrg_ccb->tx_queue_stats[lcore_id][port_id];
    if (unlikely(row == NULL))
        return;
    row[queue_id].full_packets += (uint64_t)(offered - accepted);
    row[queue_id].short_bursts += 1;

    U8 *logged = fastrg_ccb->tx_full_logged_flag[port_id];
    if (unlikely(logged == NULL) ||
        __atomic_exchange_n(&logged[queue_id], 1, __ATOMIC_RELAXED))
        return;

    char probe[256];
    int used = 0;
    for(unsigned int i=0; i<RTE_DIM(probe_offsets); i++) {
        U16 off = probe_offsets[i];
        int status = rte_eth_tx_descriptor_status(port_id, queue_id, off);
        int n = snprintf(probe + used, sizeof(probe) - used, "%s%u=%s",
            i ? " " : "", off, tx_descriptor_status_name(status));
        if (n < 0 || (size_t)(used + n) >= sizeof(probe))
            break;
        used += n;
    }
    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "TX shortfall port %u queue %u lcore %u: offered %u accepted %u; descriptors %s",
        port_id, queue_id, lcore_id, offered, accepted, probe);
}


/**
 * @fn send_pkt_to_other_lcore
 *
 * @brief Hand one packet to the lcore that owns a TX queue.
 *
 * Enqueues only; the owner transmits it in its own next burst.
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param port_id
 *      Port the queue belongs to
 * @param queue_id
 *      TX queue whose owner should send the packet
 * @param pkt
 *      Packet to hand over; still the caller's to free when this returns ERROR
 * @return
 *      SUCCESS once the owner has it, ERROR when the queue has no ring or the
 *      ring is full
 */
static __always_inline STATUS send_pkt_to_other_lcore(FastRG_t *fastrg_ccb, U16 port_id,
    U16 queue_id, struct rte_mbuf *pkt)
{
    if (unlikely(!is_tx_queue_valid(fastrg_ccb, port_id, queue_id)))
        return ERROR;

    struct rte_ring *ring = fastrg_ccb->tx_handoff_ring[port_id][queue_id];
    if (unlikely(ring == NULL || rte_ring_enqueue(ring, pkt) != 0)) {
        count_tx_handoff_drop(fastrg_ccb, port_id, queue_id);
        return ERROR;
    }
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
 * @fn fastrg_rcu_dp_offline
 *
 * @brief Temporarily mark the calling data-plane lcore QSBR-offline on both
 *        RCUs while it enters a blocking operation.
 *
 * @param fastrg_ccb
 *      FastRG control block
 */
static inline void fastrg_rcu_dp_offline(FastRG_t *fastrg_ccb)
{
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_thread_offline(fastrg_ccb->ppp_ccb_rcu, lcore_id);
    rte_rcu_qsbr_thread_offline(fastrg_ccb->pdump_rcu, lcore_id);
}

/**
 * @fn fastrg_rcu_dp_online
 *
 * @brief Mark the calling data-plane lcore QSBR-online on both RCUs after
 *        a blocking operation.  This also reports a quiescent state.
 * @param fastrg_ccb
 *      FastRG control block
 */
static inline void fastrg_rcu_dp_online(FastRG_t *fastrg_ccb)
{
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_thread_online(fastrg_ccb->ppp_ccb_rcu, lcore_id);
    rte_rcu_qsbr_thread_online(fastrg_ccb->pdump_rcu, lcore_id);
}

/**
 * @fn fastrg_rcu_dp_register
 *
 * @brief Mark the calling data-plane lcore as persistently QSBR-online for
 *        both RCUs (ppp_ccb / pdump). ppp_ccb_rcu drives the NAT slot
 *        reclaim defer queues; the pdump RCU marks possible RX/TX burst
 *        callback execution.  Call once at data thread startup, after the
 *        start_flag spin.  Pairs with fastrg_rcu_dp_quiescent() called once
 *        per poll loop.
 *
 * @param fastrg_ccb
 *      FastRG control block (holds the qsbr handles)
 */
static inline void fastrg_rcu_dp_register(FastRG_t *fastrg_ccb)
{
    fastrg_rcu_dp_online(fastrg_ccb);
    fastrg_rcu_persistent[rte_lcore_id()] = TRUE;   /* online first, then flip flag */
}

/**
 * @fn fastrg_rcu_dp_quiescent
 *
 * @brief Report a quiescent state on both RCUs for the calling data-plane
 *        lcore.  Call once at the top of each poll loop (after rte_eth_rx_burst /
 *        rte_distributor_get_pkt), before touching any RCU-protected pointer.
 *        Reporting pdump_rcu here proves the previous burst and any pdump
 *        callback it invoked have completed.
 *
 * @param fastrg_ccb
 *      FastRG control block
 */
static inline void fastrg_rcu_dp_quiescent(FastRG_t *fastrg_ccb)
{
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_quiescent(fastrg_ccb->ppp_ccb_rcu, lcore_id);
    rte_rcu_qsbr_quiescent(fastrg_ccb->pdump_rcu, lcore_id);
}

/**
 * @fn fastrg_rcu_dp_unregister
 *
 * @brief Report QSBR-offline on both RCUs for the calling data-plane lcore
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
    fastrg_rcu_persistent[rte_lcore_id()] = FALSE;  /* flag first, then go offline */
    fastrg_rcu_dp_offline(fastrg_ccb);
}

#endif /* _DP_H_ */
