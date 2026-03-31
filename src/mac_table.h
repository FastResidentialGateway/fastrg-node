/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  MAC_TABLE.H

     Per-subscriber MAC address table for LAN host resolution.

     Learned from:
       1. DHCP client requests
       2. ARP requests targeting our gateway
       3. ICMP ping to our gateway
       4. LAN packets going to WAN

     Used by port-forwarding reverse path to resolve destination MAC
     instead of broadcasting.

  Designed by THE on 2026/03/29
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _MAC_TABLE_H_
#define _MAC_TABLE_H_

#include <common.h>

#include <rte_ether.h>
#include <rte_atomic.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

/* ------------------------------------------------------------------ */
/*  MAC table sizing                                                  */
/* ------------------------------------------------------------------ */

/** Each dimension covers octets 0..254 (255 values). */
#define MAC_TABLE_DIM           255
/** Total MAC table entries: 255 * 255 * 255 = 16,581,375.
 *  Covers 10.0.0.0 ~ 10.254.254.254  (or any /8-class subnet). */
#define MAC_TABLE_SIZE          ((U32)MAC_TABLE_DIM * MAC_TABLE_DIM * MAC_TABLE_DIM)

/* ------------------------------------------------------------------ */
/*  ARP pending queue sizing                                          */
/* ------------------------------------------------------------------ */

/** Max pending packets per subscriber waiting for ARP resolution. */
#define ARP_PENDING_QUEUE_SIZE  64

/** Global mempool capacity — total pending entries across all subscribers. */
#define ARP_PENDING_POOL_SIZE   4096

/* ------------------------------------------------------------------ */
/*  Data structures                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief MAC table entry — 8 bytes.
 *
 * Direct-indexed by ip_to_mac_idx().
 * Zero-initialised table ⇒ all entries start with valid == 0.
 */
typedef struct mac_table_entry {
    struct rte_ether_addr mac;      /**< learned MAC address (6 B) */
    rte_atomic16_t        valid;    /**< 0 = empty, 1 = learned   (2 B) */
} mac_table_entry_t;

/**
 * @brief ARP pending queue entry (allocated from rte_mempool).
 */
typedef struct arp_pending_pkt {
    struct rte_mbuf *mbuf;          /**< queued packet (fully prepared except dst MAC) */
    U32              target_ip;     /**< destination IP we are resolving (net order) */
} arp_pending_pkt_t;

/**
 * @brief Per-subscriber ARP pending queue (lock-free rte_ring).
 *
 * Uses DPDK rte_ring (MPMC) so that enqueue (lan_data_rx) and
 * drain (lan_ctrl_rx / ARP reply) can operate concurrently
 * without locks or data races.
 */
typedef struct arp_pending_queue {
    struct rte_ring *ring;   /**< MPMC ring of arp_pending_pkt_t * */
} arp_pending_queue_t;

/* ------------------------------------------------------------------ */
/*  Inline helpers — hot path                                         */
/* ------------------------------------------------------------------ */

/**
 * @fn ip_to_mac_idx
 * 
 * @brief Convert a host IP (network byte order) to MAC table index.
 *
 * Uses octets 2/3/4 (the "host" part of a /8 network):
 *   For IP a.B.C.D  →  index = B * 255*255 + C * 255 + D
 * where B, C, D ∈ [0, 254].  Octet 255 is out of range (broadcast).
 *
 * @param ip_be
 *      IP address in network byte order
 * @param[out] idx
 *      Resulting index
 * @return
 *      SUCCESS on success, ERROR if any octet >= 255
 */
static __always_inline STATUS ip_to_mac_idx(U32 ip_be, U32 *idx)
{
    U32 ip = rte_be_to_cpu_32(ip_be);
    U32 b  = (ip >> 16) & 0xFF;
    U32 c  = (ip >> 8)  & 0xFF;
    U32 d  = ip & 0xFF;

    if (unlikely(b >= MAC_TABLE_DIM || c >= MAC_TABLE_DIM || d >= MAC_TABLE_DIM))
        return ERROR;

    *idx = b * (U32)(MAC_TABLE_DIM * MAC_TABLE_DIM) + c * MAC_TABLE_DIM + d;
    return SUCCESS;
}

/**
 * @fn mac_table_lookup
 * 
 * @brief Look up a MAC address in the per-subscriber MAC table.
 *
 * @param table
 *      MAC table pointer (from ppp_ccb)
 * @param ip_be
 *      Host IP address in network byte order
 * @return
 *      Pointer to the valid entry, or NULL if not found / out of range.
 */
static __always_inline mac_table_entry_t *mac_table_lookup(
    mac_table_entry_t *table, U32 ip_be)
{
    if (unlikely(table == NULL))
        return NULL;

    U32 idx;
    if (unlikely(ip_to_mac_idx(ip_be, &idx) == ERROR))
        return NULL;

    mac_table_entry_t *e = &table[idx];
    if (likely(rte_atomic16_read(&e->valid) != 0)) {
        rte_rmb();      /* read MAC after checking valid flag */
        return e;
    }
    return NULL;
}

/**
 * @fn mac_table_learn
 * 
 * @brief Learn (store) a MAC ↔ IP mapping in the MAC table.
 * 
 * @param table
 *      MAC table pointer (from ppp_ccb)
 * @param ip_be
 *      Host IP address in network byte order
 * @param mac
 *      MAC address to store
 */
static __always_inline void mac_table_learn(
    mac_table_entry_t *table, U32 ip_be,
    const struct rte_ether_addr *mac)
{
    if (unlikely(table == NULL))
        return;

    U32 idx;
    if (unlikely(ip_to_mac_idx(ip_be, &idx) == ERROR))
        return;

    mac_table_entry_t *e = &table[idx];
    rte_ether_addr_copy(mac, &e->mac);
    rte_wmb();  /* ensure MAC is visible before valid flag */
    rte_atomic16_set(&e->valid, 1);
}

/* ------------------------------------------------------------------ */
/*  Non-inline function declarations  (implemented in mac_table.c)    */
/* ------------------------------------------------------------------ */

/**
 * @fn mac_table_alloc
 * 
 * @brief Allocate a MAC table (zero-initialised → all entries invalid).
 * 
 * @return Heap pointer on success, NULL on failure.
 */
mac_table_entry_t *mac_table_alloc(void);

/**
 * @fn mac_table_free
 * 
 * @brief Free a MAC table previously returned by mac_table_alloc().
 * 
 * @param table
 *      Pointer returned by mac_table_alloc()
 */
void mac_table_free(mac_table_entry_t *table);

/**
 * @fn arp_pending_init_pool
 * 
 * @brief Create the global ARP-pending mempool.  Call once at sys_init.
 * Stores the pool pointer in fastrg_ccb->arp_pending_mp.
 *
 * @param arp_mp_out
 *      Output pointer for the created mempool
 * @return
 *      SUCCESS / ERROR
 */
STATUS arp_pending_init_pool(struct rte_mempool **arp_mp_out);

/**
 * @fn arp_pending_cleanup_pool
 * 
 * @brief Free the global ARP-pending mempool.
 * Call once at sys_cleanup.
 * 
 * @param arp_mp_ptr
 *      Pointer to the mempool pointer (i.e. &fastrg_ccb->arp_pending_mp)
 */
void arp_pending_cleanup_pool(struct rte_mempool **arp_mp_ptr);

/**
 * @fn arp_pending_init_queue
 * 
 * @brief Create a per-subscriber SPSC rte_ring for ARP pending packets.
 *
 * @param q
 *      Per-subscriber queue to initialise
 * @param ccb_id
 *      Subscriber index (used for unique ring naming)
 * @return
 *      SUCCESS / ERROR
 */
STATUS arp_pending_init_queue(arp_pending_queue_t *q, int ccb_id);

/**
 * @fn arp_pending_cleanup_queue
 * 
 * @brief Destroy a per-subscriber ARP pending ring (flush + free ring).
 *
 * @param q
 *      Per-subscriber queue
 * @param mp
 *      Global ARP-pending mempool (for returning entries)
 */
void arp_pending_cleanup_queue(arp_pending_queue_t *q, struct rte_mempool *mp);

/**
 * @fn arp_pending_enqueue
 * 
 * @brief Enqueue a packet into a subscriber's ARP pending queue.
 *
 * If the queue is full the oldest entry is dropped (its mbuf freed,
 * its mempool slot returned).
 *
 * @param mp
 *      Global ARP-pending mempool
 * @param q
 *      Per-subscriber queue
 * @param mbuf
 *      Packet to queue (ownership transferred on success)
 * @param target_ip
 *      Destination IP being resolved (network byte order)
 * @return
 *      SUCCESS on success, ERROR on mempool exhaustion
 */
STATUS arp_pending_enqueue(struct rte_mempool *mp, arp_pending_queue_t *q,
    struct rte_mbuf *mbuf, U32 target_ip);

/**
 * @fn arp_pending_drain
 * 
 * @brief Drain all pending packets matching a resolved IP.
 *
 * For each match the destination MAC in the Ethernet header is set,
 * the mbuf is appended to tx_pkts, and the mempool slot is freed.
 * Non-matching entries stay in the queue.
 *
 * @param mp
 *      Global ARP-pending mempool
 * @param q
 *      Per-subscriber queue
 * @param resolved_ip
 *      IP that was just resolved (network byte order)
 * @param mac
 *      Resolved MAC address
 * @param tx_pkts
 *      Output array to collect ready-to-send mbufs
 * @param tx_count
 *      [in/out] current count in tx_pkts
 * @param tx_max
 *      Capacity of tx_pkts
 */
void arp_pending_drain(struct rte_mempool *mp, arp_pending_queue_t *q,
    U32 resolved_ip, const struct rte_ether_addr *mac,
    struct rte_mbuf **tx_pkts, U16 *tx_count, U16 tx_max);

/**
 * @fn arp_pending_flush
 * 
 * @brief Flush (free) every entry in a subscriber's ARP pending queue.
 *
 * Call during subscriber cleanup / reconfiguration.
 *
 * @param mp
 *      Global ARP-pending mempool (may be NULL during unit test)
 * @param q
 *      Per-subscriber queue
 */
void arp_pending_flush(struct rte_mempool *mp, arp_pending_queue_t *q);

/**
 * @fn send_arp_request
 * 
 * @brief Build and TX an ARP request on the LAN port.
 *
 * @param src_mac
 *      Our LAN MAC address
 * @param src_ip
 *      Our gateway IP (network byte order)
 * @param target_ip
 *      IP to resolve (network byte order)
 * @param vlan_id
 *      Subscriber VLAN tag (host byte order)
 * @param tx_q
 *      LAN TX queue id
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS send_arp_request(const struct rte_ether_addr *src_mac, U32 src_ip,
    U32 target_ip, U16 vlan_id, U16 tx_q);

#endif /* _MAC_TABLE_H_ */
