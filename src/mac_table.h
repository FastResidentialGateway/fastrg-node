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

     Implementation: a fixed-capacity DPDK rte_hash (created once at init,
     never resized, never freed at runtime) keyed by the host IPv4 address.
     The 6-byte MAC plus a 16-bit generation number are packed into the
     64-bit application-data word of the hash, so learning / re-learning a
     MAC is a single atomic pdata store inside rte_hash (release order) and
     a lookup is a single atomic load — a concurrent reader can never
     observe a torn MAC (the old direct-index table updated the 6-byte
     MAC non-atomically under a separate valid flag).

  Designed by THE on 2026/03/29
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _MAC_TABLE_H_
#define _MAC_TABLE_H_

#include <common.h>

#include <rte_ether.h>
#include <rte_atomic.h>
#include <rte_hash.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

/* ------------------------------------------------------------------ */
/*  MAC table sizing                                                  */
/* ------------------------------------------------------------------ */

/** Fixed per-subscriber capacity: 64K entries = one fully-populated /16.
 *  Hard-wired by design decision: we cannot assume a LAN device count,
 *  and 64K (~2MB/subscriber) is the ruled capacity. */
#define MAC_TABLE_MAX_ENTRIES 65536

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
 * @brief Per-subscriber MAC table handle.
 *
 * hash data word layout (uintptr_t, 64-bit):
 *   bits  0..47 : MAC address bytes 0..5 (byte 0 in bits 0..7)
 *   bits 48..63 : generation stamp of the entry. This is used to invalidate 
 *                 all entries in O(1) without calling rte_hash_reset.
 */
typedef struct mac_table {
    struct rte_hash *hash;       /**< key = U32 IP (BE), data = packed MAC+gen */
    U16              generation; /**< current generation (RELAXED atomic access) */
    U64              learn_fail; /**< learns dropped because the table is full  */
} mac_table_t;

/**
 * @brief ARP pending queue entry (allocated from rte_mempool).
 */
typedef struct arp_pending_pkt {
    struct rte_mbuf *mbuf;          /**< queued packet (fully prepared except dst MAC) */
    U32              target_ip;     /**< destination IP we are resolving (net order) */
    U16              tx_queue;      /**< LAN TX queue the packet was heading for */
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
 * @fn mac_table_pack
 *
 * @brief Pack a MAC address and a generation stamp into the 64-bit hash
 *        data word. bits 0..47 for MAC address bytes, bits 48..63 for generation.
 *
 * @param mac
 *      MAC address to pack
 * @param gen
 *      Generation stamp
 * @return
 *      Packed 64-bit value
 */
static __always_inline uintptr_t mac_table_pack(const struct rte_ether_addr *mac, U16 gen)
{
    uintptr_t v = 0;

    for(int i=0; i<RTE_ETHER_ADDR_LEN; i++)
        v |= (uintptr_t)mac->addr_bytes[i] << (i * 8);
    v |= (uintptr_t)gen << 48;
    return v;
}

/**
 * @fn mac_table_unpack
 *
 * @brief Unpack the 64-bit hash data word into a MAC address and its
 *        generation stamp.
 *
 * @param v
 *      Packed 64-bit value
 * @param[out] mac
 *      Unpacked MAC address
 * @return
 *      Generation stamp of the entry
 */
static __always_inline U16 mac_table_unpack(uintptr_t v, struct rte_ether_addr *mac)
{
    for(int i=0; i<RTE_ETHER_ADDR_LEN; i++)
        mac->addr_bytes[i] = (U8)(v >> (i * 8));
    return (U16)(v >> 48);
}

/**
 * @fn mac_table_lookup
 *
 * @brief Look up the MAC learned for a host IP. Lock-free (LF hash lookup +
 *        one atomic data-word load); safe against concurrent learns.
 *
 * @param table
 *      MAC table handle (from ppp_ccb)
 * @param ip_be
 *      Host IP address in network byte order
 * @param[out] mac_out
 *      Learned MAC address on SUCCESS
 * @return
 *      SUCCESS if a current-generation entry exists, ERROR otherwise.
 */
static __always_inline STATUS mac_table_lookup(
    mac_table_t *table, U32 ip_be, struct rte_ether_addr *mac_out)
{
    void *data;

    if (unlikely(table == NULL))
        return ERROR;
    if (rte_hash_lookup_data(table->hash, &ip_be, &data) < 0)
        return ERROR;
    U16 gen = mac_table_unpack((uintptr_t)data, mac_out);
    if (unlikely(gen != __atomic_load_n(&table->generation, __ATOMIC_RELAXED)))
        return ERROR;   /* stale entry from before the last mac_table_reset */
    return SUCCESS;
}

/**
 * @fn mac_table_learn
 *
 * @brief Learn (store) a MAC ↔ IP mapping. Called on the LAN uplink hot
 *        path by multiple data lcores concurrently.
 *
 * Common case (already learned, same MAC, current generation) is a pure
 * lock-free lookup with no store, so per-packet calls do not dirty the
 * entry. Only a new IP, a changed MAC, or a stale generation performs the
 * add (which atomically overwrites the packed data word for existing keys).
 *
 * @param table
 *      MAC table handle (from ppp_ccb)
 * @param ip_be
 *      Host IP address in network byte order
 * @param mac
 *      MAC address to store
 */
static __always_inline void mac_table_learn(
    mac_table_t *table, U32 ip_be, const struct rte_ether_addr *mac)
{
    void *data;

    if (unlikely(table == NULL))
        return;

    U16 gen = __atomic_load_n(&table->generation, __ATOMIC_RELAXED);
    uintptr_t want = mac_table_pack(mac, gen);

    if (likely(rte_hash_lookup_data(table->hash, &ip_be, &data) >= 0 &&
               (uintptr_t)data == want))
        return;

    if (unlikely(rte_hash_add_key_data(table->hash, &ip_be, (void *)want) < 0))
        __atomic_fetch_add(&table->learn_fail, 1, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/*  Non-inline function declarations  (implemented in mac_table.c)    */
/* ------------------------------------------------------------------ */

/**
 * @fn mac_table_alloc
 *
 * @brief Allocate a MAC table: create the fixed-capacity lock free rte_hash.
 *
 * @param ccb_id
 *      Subscriber slot index; makes the rte_hash name unique
 *      process-wide.
 * @return Table handle on success, NULL on failure.
 */
mac_table_t *mac_table_alloc(U16 ccb_id);

/**
 * @fn mac_table_free
 *
 * @brief Free a MAC table previously returned by mac_table_alloc().
 *
 * @param table
 *      Handle returned by mac_table_alloc()
 */
void mac_table_free(mac_table_t *table);

/**
 * @fn mac_table_reset
 *
 * @brief Invalidate every learned entry by bumping the table generation.
 *        O(1), control-plane, safe against concurrent data-plane learns
 *        and lookups. Stale keys keep occupying hash slots until the same 
 *        IP is re-learned; capacity is bounded by 64K distinct IPs ever learned.
 *
 * @param table
 *      MAC table handle (NULL tolerated)
 */
void mac_table_reset(mac_table_t *table);

/**
 * @fn mac_table_iterate
 *
 * @brief Walk the table, returning only current-generation entries.
 *        Control-plane diagnostics (gRPC GetArpTable); safe to run
 *        concurrently with data-plane learns in lock free mode.
 *
 * @param table
 *      MAC table handle
 * @param[in,out] next
 *      Iterator cursor; start from 0
 * @param[out] ip_out
 *      Entry host IP (network byte order)
 * @param[out] mac_out
 *      Entry MAC address
 * @return
 *      Hash position (>= 0) of the returned entry, or -1 at the end.
 */
int32_t mac_table_iterate(const mac_table_t *table, U32 *next, U32 *ip_out,
    struct rte_ether_addr *mac_out);

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
 * @param tx_queue
 *      LAN TX queue the packet was originally received on. Kept because the 
 *      drain runs on a different lcore and has to send it back through that 
 *      queue's owner.
 * @return
 *      SUCCESS on success, ERROR on mempool exhaustion
 */
STATUS arp_pending_enqueue(struct rte_mempool *mp, arp_pending_queue_t *q,
    struct rte_mbuf *mbuf, U32 target_ip, U16 tx_queue);

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
 * @param tx_queues
 *      Output array, same indices as tx_pkts, holding each packet's TX queue
 * @param tx_count
 *      [in/out] current count in tx_pkts
 * @param tx_max
 *      Capacity of tx_pkts
 */
void arp_pending_drain(struct rte_mempool *mp, arp_pending_queue_t *q,
    U32 resolved_ip, const struct rte_ether_addr *mac,
    struct rte_mbuf **tx_pkts, U16 tx_queues[], U16 *tx_count, U16 tx_max);

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
 * @fn encode_arp_request
 *
 * @brief Encode a broadcast ARP request frame (eth + vlan + arp) into a
 * caller-supplied buffer. Pure codec — no mbuf/NIC involvement, so it is
 * unit-testable without DPDK device or mempool state.
 *
 * @param buf
 *      Output buffer, at least 46 bytes
 * @param src_mac
 *      Our LAN MAC address
 * @param src_ip
 *      Our gateway IP (network byte order)
 * @param target_ip
 *      IP to resolve (network byte order)
 * @param vlan_id
 *      Subscriber VLAN tag (host byte order)
 * @return
 *      Number of bytes written (46)
 */
U16 encode_arp_request(U8 *buf, const struct rte_ether_addr *src_mac, U32 src_ip,
    U32 target_ip, U16 vlan_id);

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
 * @param pool
 *      Mempool to allocate the request from; must be the pool the target queue
 *      already carries, so one queue never mixes two pools
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS send_arp_request(const struct rte_ether_addr *src_mac, U32 src_ip,
    U32 target_ip, U16 vlan_id, U16 tx_q, struct rte_mempool *pool);

#endif /* _MAC_TABLE_H_ */
