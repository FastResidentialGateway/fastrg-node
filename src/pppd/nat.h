#ifndef _NAT_H_
#define _NAT_H_

#include <stdint.h>
#include <inttypes.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_memcpy.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_arp.h>
#include <rte_icmp.h>
#include <rte_flow.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_timer.h>
#include <rte_atomic.h>
#include <rte_memcpy.h>
#include <rte_hash_crc.h>
#include <rte_hash.h>
#include <rte_ring.h>
#include <rte_rcu_qsbr.h>

#include "pppd.h"
#include "tcp_conntrack.h"

#define NAT_ENTRY_TIMEOUT_SEC 10

/* Slots examined per amortized/emergency GC scan call. Bounded so the
 * inline hot-path cost stays small; the nat_gc_ccb_counter persists 
 * across calls, so successive calls cover the whole pool. */
#define NAT_GC_SCAN_CHUNK 512

/* Forced-minimum GC: after this many consecutive full-burst polls with no
 * idle headroom, a data lcore runs one GC chunk anyway, so sustained
 * line-rate traffic cannot starve zombie reclaim entirely.  Worst-case
 * overhead: one 512-slot SoA scan per 1024 x 32 packets (~1.5%). */
#define NAT_GC_FORCE_PERIOD 1024

#define MAX_L4_PORT_NUM 0xffff
#define SYS_MAX_PORT 1000
#define NAT_PORT_RANGE (TOTAL_SOCK_PORT - SYS_MAX_PORT)

#define NAT_ENTRY_FREE       0
#define NAT_ENTRY_FILLING    1
#define NAT_ENTRY_READY      2

/**
 * @fn nat_expiry_cycles
 *
 * @brief Compute the absolute TSC timestamp at which a new/refreshed NAT entry
 *        should expire (now + NAT_ENTRY_TIMEOUT_SEC seconds).
 *
 * @return Expiry timestamp in CPU cycles
 */
static inline U64 nat_expiry_cycles(void)
{
    return fastrg_get_cur_cycles() + (U64)NAT_ENTRY_TIMEOUT_SEC * fastrg_get_cycles_in_sec();
}

/**
 * @fn nat_entry_is_expired
 *
 * @brief Check whether a READY NAT entry has passed its expiry timestamp.
 *        Must only be called on entries whose is_fill == NAT_ENTRY_READY.
 *
 * @param entry  Pointer to NAT address table entry
 *
 * @return Non-zero if the entry has expired, 0 otherwise
 */
static inline int nat_entry_is_expired(addr_table_t *entry)
{
    return fastrg_get_cur_cycles() > __atomic_load_n(entry->expire_slot, __ATOMIC_RELAXED);
}

/**
 * @brief Reverse-direction NAT key: identifies a mapping from the WAN side.
 *        (nat_port, dst_ip, dst_port) — dst included so the same nat_port can
 *        be reused toward different destinations (true port reuse).
 *        8 bytes, naturally packed (no padding) — required, rte_hash compares
 *        raw key bytes.
 */
typedef struct nat_reverse_key {
    U32 dst_ip;   /* remote IP, network byte order */
    U16 nat_port; /* our SNAT port, network byte order */
    U16 dst_port; /* remote port, network byte order */
} nat_reverse_key_t;

/**
 * @brief Forward-direction NAT key: identifies a flow from the LAN side by
 *        its 5-tuple, so an established flow's outbound packets hit their
 *        mapping in one lookup instead of re-walking the candidate-port
 *        sequence.  Protocol-blind, matching nat_entry_same_flow().
 *        12 bytes, naturally packed (no padding) — required, rte_hash
 *        compares raw key bytes.
 */
typedef struct nat_forward_key {
    U32 src_ip;   /* LAN host IP, network byte order */
    U32 dst_ip;   /* remote IP, network byte order */
    U16 src_port; /* LAN host port, network byte order */
    U16 dst_port; /* remote port, network byte order */
} nat_forward_key_t;

/**
 * @fn nat_reverse_hash_free_cb
 *
 * @brief RCU defer-queue callback invoked by rte_hash once all data-plane
 *        readers have passed a quiescent state after a key deletion: only
 *        then is it safe to mark the pool slot FREE and recycle its index,
 *        because no reader can still hold a pointer to the entry.
 *
 * @param p
 *      ppp_ccb_t of the owning subscriber (rcu cfg key_data_ptr)
 * @param key_data
 *      Pool slot index stored as the hash value (cast via uintptr_t)
 */
static inline void nat_reverse_hash_free_cb(void *p, void *key_data)
{
    ppp_ccb_t *ppp_ccb = (ppp_ccb_t *)p;
    U32 idx = (U32)(uintptr_t)key_data;

    /* Zero the SoA deadline first so the GC scan skips this slot without
     * ever touching its entry cache line again */
    __atomic_store_n(&ppp_ccb->nat_expire_at[idx], 0, __ATOMIC_RELAXED);
    rte_atomic16_set(&ppp_ccb->addr_table[idx].is_fill, NAT_ENTRY_FREE);
    rte_ring_enqueue(ppp_ccb->nat_free_ring, (void *)(uintptr_t)idx);
}

/**
 * @fn nat_table_reset
 *
 * @brief Flush all NAT state of a subscriber: empty both hashes and
 *        refill the free-list with every pool index.  Control-plane only
 *        (subscriber re-init), must not race data-plane traffic for this sub.
 *
 * @param ppp_ccb
 *      Subscriber control block
 */
static inline void nat_table_reset(ppp_ccb_t *ppp_ccb)
{
    unsigned int freed, pending, avail;

    /* Drain both defer queues before rebuilding: a deferred free surviving
     * the reset would fire later and push its (now re-issued) slot index
     * into the refilled free ring — two flows sharing one entry — and the
     * same staleness corrupts the hashes' internal key-slot allocators.
     * Terminates: data lcores report quiescent every poll loop even when
     * idle, and at first init the queues are simply empty. */
    do {
        freed = pending = avail = 0;
        rte_hash_rcu_qsbr_dq_reclaim(ppp_ccb->nat_reverse_hash, &freed, &pending, &avail);
    } while (pending != 0);
    do {
        freed = pending = avail = 0;
        rte_hash_rcu_qsbr_dq_reclaim(ppp_ccb->nat_forward_hash, &freed, &pending, &avail);
    } while (pending != 0);

    rte_hash_reset(ppp_ccb->nat_reverse_hash);
    rte_hash_reset(ppp_ccb->nat_forward_hash);
    rte_ring_reset(ppp_ccb->nat_free_ring);
    for(U32 i=0; i<MAX_NAT_ENTRIES; i++) {
        ppp_ccb->addr_table[i].expire_slot = &ppp_ccb->nat_expire_at[i];
        ppp_ccb->nat_expire_at[i] = 0;
        rte_atomic16_set(&ppp_ccb->addr_table[i].is_fill, NAT_ENTRY_FREE);
        rte_ring_enqueue(ppp_ccb->nat_free_ring, (void *)(uintptr_t)i);
    }
    ppp_ccb->nat_gc_counter = 0;
    ppp_ccb->nat_enospc = 0;
    ppp_ccb->nat_gc_reclaimed = 0;
}

/**
 * @fn nat_table_init
 *
 * @brief Create-once (find-existing on re-create, mirroring arp_pq naming
 *        rules) the per-subscriber reverse + forward hashes and free-list
 *        ring, attach the shared QSBR RCU for deferred reclaim, and fill the
 *        free-list.  Pool slot recycling is owned solely by the reverse
 *        hash's dq callback; the forward hash rides the same RCU only for
 *        its internal key-slot recycling (NULL data callback).
 *
 * @param ppp_ccb
 *      Subscriber control block
 * @param ccb_id
 *      Subscriber index, used for unique DPDK object naming
 * @param rcu
 *      QSBR variable shared with the data-plane lcores (ppp_ccb_rcu)
 *
 * @return SUCCESS / ERROR
 */
static inline STATUS nat_table_init(ppp_ccb_t *ppp_ccb, U16 ccb_id, struct rte_rcu_qsbr *rcu)
{
    char name[RTE_RING_NAMESIZE];

    snprintf(name, sizeof(name), "nat_reverse_%u", ccb_id);
    ppp_ccb->nat_reverse_hash = rte_hash_find_existing(name);
    if (ppp_ccb->nat_reverse_hash == NULL) {
        struct rte_hash_parameters params = {
            .name = name,
            .entries = MAX_NAT_ENTRIES,
            .key_len = sizeof(nat_reverse_key_t),
            .hash_func = rte_hash_crc,
            .hash_func_init_val = 0,
            .socket_id = (int)rte_socket_id(),
            .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF |
                          RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD,
        };
        ppp_ccb->nat_reverse_hash = rte_hash_create(&params);
        if (ppp_ccb->nat_reverse_hash == NULL)
            return ERROR;
        struct rte_hash_rcu_config rcu_cfg = {
            .v = rcu,
            .mode = RTE_HASH_QSBR_MODE_DQ,
            .key_data_ptr = ppp_ccb,
            .free_key_data_func = nat_reverse_hash_free_cb,
        };
        if (rte_hash_rcu_qsbr_add(ppp_ccb->nat_reverse_hash, &rcu_cfg) != 0) {
            rte_hash_free(ppp_ccb->nat_reverse_hash);
            ppp_ccb->nat_reverse_hash = NULL;
            return ERROR;
        }
    }

    snprintf(name, sizeof(name), "nat_forward_%u", ccb_id);
    ppp_ccb->nat_forward_hash = rte_hash_find_existing(name);
    if (ppp_ccb->nat_forward_hash == NULL) {
        struct rte_hash_parameters params = {
            .name = name,
            .entries = MAX_NAT_ENTRIES,
            .key_len = sizeof(nat_forward_key_t),
            .hash_func = rte_hash_crc,
            .hash_func_init_val = 0,
            .socket_id = (int)rte_socket_id(),
            .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF |
                          RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD,
        };
        ppp_ccb->nat_forward_hash = rte_hash_create(&params);
        if (ppp_ccb->nat_forward_hash == NULL) {
            rte_hash_free(ppp_ccb->nat_reverse_hash);
            ppp_ccb->nat_reverse_hash = NULL;
            return ERROR;
        }
        struct rte_hash_rcu_config rcu_cfg = {
            .v = rcu,
            .mode = RTE_HASH_QSBR_MODE_DQ,
            /* No data callback: slot reclaim belongs to the reverse hash;
             * this only defers the hash's internal key-slot recycling. */
            .free_key_data_func = NULL,
        };
        if (rte_hash_rcu_qsbr_add(ppp_ccb->nat_forward_hash, &rcu_cfg) != 0) {
            rte_hash_free(ppp_ccb->nat_forward_hash);
            ppp_ccb->nat_forward_hash = NULL;
            rte_hash_free(ppp_ccb->nat_reverse_hash);
            ppp_ccb->nat_reverse_hash = NULL;
            return ERROR;
        }
    }

    snprintf(name, sizeof(name), "nat_free_%u", ccb_id);
    ppp_ccb->nat_free_ring = rte_ring_lookup(name);
    if (ppp_ccb->nat_free_ring == NULL) {
        ppp_ccb->nat_free_ring = rte_ring_create(name, MAX_NAT_ENTRIES,
            (int)rte_socket_id(), RING_F_EXACT_SZ);
        if (ppp_ccb->nat_free_ring == NULL) {
            rte_hash_free(ppp_ccb->nat_forward_hash);
            ppp_ccb->nat_forward_hash = NULL;
            rte_hash_free(ppp_ccb->nat_reverse_hash);
            ppp_ccb->nat_reverse_hash = NULL;
            return ERROR;
        }
    }

    nat_table_reset(ppp_ccb);
    return SUCCESS;
}

/**
 * @fn nat_table_destroy
 *
 * @brief Free the per-subscriber hashes + free-list ring (subscriber
 *        removal / init rollback).  Idempotent.
 *
 * @param ppp_ccb
 *      Subscriber control block
 */
static inline void nat_table_destroy(ppp_ccb_t *ppp_ccb)
{
    if (ppp_ccb->nat_reverse_hash != NULL) {
        rte_hash_free(ppp_ccb->nat_reverse_hash);
        ppp_ccb->nat_reverse_hash = NULL;
    }
    if (ppp_ccb->nat_forward_hash != NULL) {
        rte_hash_free(ppp_ccb->nat_forward_hash);
        ppp_ccb->nat_forward_hash = NULL;
    }
    if (ppp_ccb->nat_free_ring != NULL) {
        rte_ring_free(ppp_ccb->nat_free_ring);
        ppp_ccb->nat_free_ring = NULL;
    }
}

/**
 * @fn nat_entry_del_keys
 *
 * @brief Unlink a READY entry from both hashes: forward key first, then
 *        reverse.  The order is the safety invariant — deleting the reverse
 *        key is what schedules the pool slot for RCU reclaim, so by the time
 *        the slot can recycle no forward key may still reference it.  Safe
 *        from any lcore; racing deleters are fine (duplicate deletes just
 *        return ENOENT, only the reverse-delete winner accounts the reclaim).
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param entry
 *        READY entry to unlink (its fields provide both keys)
 *
 * @return 1 if this caller won the reverse-key delete, 0 otherwise
 */
static inline int nat_entry_del_keys(ppp_ccb_t *ppp_ccb, addr_table_t *entry)
{
    nat_forward_key_t fkey = {
        .src_ip = entry->src_ip,
        .dst_ip = entry->dst_ip,
        .src_port = entry->src_port,
        .dst_port = entry->dst_port,
    };
    nat_reverse_key_t rkey = {
        .dst_ip = entry->dst_ip,
        .nat_port = entry->nat_port,
        .dst_port = entry->dst_port,
    };

    rte_hash_del_key(ppp_ccb->nat_forward_hash, &fkey);
    return rte_hash_del_key(ppp_ccb->nat_reverse_hash, &rkey) >= 0;
}

/**
 * @fn nat_gc_scan_by_ccb
 *
 * @brief Amortized garbage collection: scan up to max_slots pool slots from
 *        the per-subscriber cursor and unlink every expired READY entry from
 *        both hashes.  Slot indices flow back to the free-list via the RCU
 *        defer-queue callback once readers are quiescent.  Restores the
 *        self-cleaning property the old probe-walk eviction provided.
 *        Safe from any lcore (hashes are multi-writer; duplicate deletes of
 *        the same key just return ENOENT).
 *
 * @param ppp_ccb
 *      Subscriber control block
 * @param max_slots
 *      Upper bound of slots to examine in this call
 *
 * @return
 *      Number of expired entries whose keys were deleted
 */
static inline U32 nat_gc_scan_by_ccb(ppp_ccb_t *ppp_ccb, U32 max_slots)
{
    U32 start = __atomic_fetch_add(&ppp_ccb->nat_gc_counter, max_slots, __ATOMIC_RELAXED);
    U32 reclaimed = 0;

    U64 now = fastrg_get_cur_cycles();

    for(U32 n=0; n<max_slots; n++) {
        U32 idx = (start + n) % MAX_NAT_ENTRIES;

        /* Hot loop reads only the SoA deadline array — 8 slots per cache
         * line, sequential, prefetcher-friendly; 0 = free slot.  The 64B
         * entry line is touched only for actual expired hits. */
        U64 deadline = __atomic_load_n(&ppp_ccb->nat_expire_at[idx], __ATOMIC_RELAXED);
        if (deadline == 0 || deadline > now)
            continue;

        addr_table_t *entry = &ppp_ccb->addr_table[idx];
        if (rte_atomic16_read(&entry->is_fill) != NAT_ENTRY_READY)
            continue;
        if (nat_entry_del_keys(ppp_ccb, entry))
            reclaimed++;
    }
    if (reclaimed > 0)
        __atomic_fetch_add(&ppp_ccb->nat_gc_reclaimed, (U64)reclaimed, __ATOMIC_RELAXED);
    return reclaimed;
}

/**
 * @fn nat_slot_alloc
 *
 * @brief Pop a free pool slot index.  On exhaustion, run an emergency GC
 *        chunk plus an explicit defer-queue reclaim and retry once — under
 *        sustained flow churn expired entries are the slots we get back.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param idx
 *        [out] Allocated pool slot index
 *
 * @return SUCCESS, or ERROR if the pool is truly exhausted by live flows
 */
static inline STATUS nat_slot_alloc(ppp_ccb_t *ppp_ccb, U32 *idx)
{
    void *obj;

    if (likely(rte_ring_dequeue(ppp_ccb->nat_free_ring, &obj) == 0)) {
        *idx = (U32)(uintptr_t)obj;
        return SUCCESS;
    }

    nat_gc_scan_by_ccb(ppp_ccb, NAT_GC_SCAN_CHUNK);
    unsigned int freed = 0, pending = 0, available = 0;
    rte_hash_rcu_qsbr_dq_reclaim(ppp_ccb->nat_reverse_hash, &freed, &pending, &available);
    if (rte_ring_dequeue(ppp_ccb->nat_free_ring, &obj) == 0) {
        *idx = (U32)(uintptr_t)obj;
        return SUCCESS;
    }
    return ERROR;
}

/**
 * @fn compute_initial_nat_port
 * 
 * @brief Compute initial NAT port from source info
 * 
 * @param src_ip
 *        Source IP in network byte order
 * @param src_port
 *        Source port in network byte order
 * 
 * @return NAT port in range SYS_MAX_PORT to TOTAL_SOCK_PORT - 1 in network byte order
 */
static inline U16 compute_initial_nat_port(U32 src_ip, U16 src_port)
{
    U32 hash;
    hash = rte_hash_crc_4byte(src_ip, 0);
    hash = rte_hash_crc_4byte((U32)src_port, hash);
    return rte_cpu_to_be_16(((U16)hash % NAT_PORT_RANGE) + SYS_MAX_PORT);
}

/**
 * @fn nat_entry_same_flow
 * 
 * @brief Check if entry is for the same flow (exact 5-tuple match)
 * 
 * @param entry
 *       Pointer to NAT entry
 * @param nat_port
 *       NAT port in network byte order
 * @param src_ip
 *       Source IP in network byte order
 * @param src_port
 *       Source port in network byte order
 * @param dst_ip
 *       Destination IP in network byte order
 * @param dst_port
 *       Destination port in network byte order
 * 
 * @return 1 if matches, 0 otherwise
 */
static inline int nat_entry_same_flow(addr_table_t *entry, U16 nat_port, 
    U32 src_ip, U16 src_port, U32 dst_ip, U16 dst_port)
{
    return (entry->nat_port == nat_port &&
            entry->src_ip == src_ip &&
            entry->src_port == src_port &&
            entry->dst_ip == dst_ip &&
            entry->dst_port == dst_port);
}

/**
 * @fn nat_learning_port_reuse
 *
 * @brief NAT learning with TRUE port reuse support.
 *
 * Per-packet fast path: one lock-free forward-hash lookup on the 5-tuple —
 * an established flow hits its mapping directly, with no initial-port
 * recomputation and no candidate-port walk (a flow that skipped N conflicted
 * ports at creation no longer re-pays those N lookups on every packet).
 *
 * Only a forward miss (genuinely new flow, or a transient GC race) enters
 * the candidate-port walk.  Port reuse logic there is unchanged from the
 * probing implementation:
 * - Same (nat_port, dst_ip, dst_port) from a different source = CONFLICT, try next nat_port
 * - Same nat_port with different dst = OK, port reuse achieved
 *
 * The new-flow insert takes the per-subscriber lock and double-checks both
 * hashes (rte_hash_add_key_data on an existing key would silently REPLACE
 * its data): forward for "someone just learned this flow", reverse for
 * "someone just took this port".  Keys are inserted reverse-first so a
 * failed forward add can unpublish via the reverse key (whose RCU dq owns
 * slot reclaim); deletions run forward-first for the mirror invariant.
 *
 * @param ppp_ccb
 *      Subscriber control block (owns pool, rev hash, free-list, port-fwd table)
 * @param eth_hdr
 *      Pointer to Ethernet header (for copying MAC address)
 * @param src_ip
 *      Source IP in network byte order
 * @param dst_ip
 *      Destination IP in network byte order
 * @param src_port
 *      Source port in network byte order
 * @param dst_port
 *      Destination port in network byte order
 * @param out_entry
 *      [out, may be NULL] The learned/refreshed pool entry, so callers
 *      (TCP conntrack) need not look it up again
 *
 * @return
 *      Allocated nat_port in network byte order, or 0 if all ports exhausted
 */
static inline U16 nat_learning_port_reuse(ppp_ccb_t *ppp_ccb, struct rte_ether_hdr *eth_hdr,
    U32 src_ip, U32 dst_ip, U16 src_port, U16 dst_port, addr_table_t **out_entry)
{
    nat_forward_key_t fkey = { .src_ip = src_ip, .dst_ip = dst_ip,
        .src_port = src_port, .dst_port = dst_port };
    void *fdata;

    /* Per-packet fast path: established flow, one lock-free lookup (LAN side
     * is trusted: no expiry check, traffic just revives the mapping) */
    if (likely(rte_hash_lookup_data(ppp_ccb->nat_forward_hash, &fkey, &fdata) >= 0)) {
        addr_table_t *entry = &ppp_ccb->addr_table[(U32)(uintptr_t)fdata];

        rte_atomic_thread_fence(rte_memory_order_acquire);
        nat_expire_refresh(entry->expire_slot, nat_expiry_cycles());
        if (out_entry != NULL)
            *out_entry = entry;
        return entry->nat_port;
    }

    U16 nat_port = compute_initial_nat_port(src_ip, src_port);
    U16 start_nat_port = nat_port;

    do {
        U32 nat_port_host = rte_be_to_cpu_16(nat_port);
        void *data;

        /* Skip ports reserved by static port forwarding rules */
        if (rte_atomic16_read(&ppp_ccb->port_fwd_table[nat_port_host].is_active) == 1)
            goto next_nat_port;

        nat_reverse_key_t key = { .dst_ip = dst_ip, .nat_port = nat_port, .dst_port = dst_port };

        /* Lock-free fast path: existing mapping for this (port, dst)? */
        if (rte_hash_lookup_data(ppp_ccb->nat_reverse_hash, &key, &data) >= 0) {
            addr_table_t *entry = &ppp_ccb->addr_table[(U32)(uintptr_t)data];

            /* Same flow already exists — refresh and done (LAN side is
             * trusted: no expiry check, traffic just revives the mapping) */
            if (nat_entry_same_flow(entry, nat_port, src_ip, src_port, dst_ip, dst_port)) {
                nat_expire_refresh(entry->expire_slot, nat_expiry_cycles());
                if (out_entry != NULL)
                    *out_entry = entry;
                return nat_port;
            }
            /* Different source, still alive: genuine conflict → next port */
            if (!nat_entry_is_expired(entry))
                goto next_nat_port;
            /* Expired mapping of someone else: evict both keys (slot
             * recycles via RCU) and fall through to insert fresh for us */
            nat_entry_del_keys(ppp_ccb, entry);
        }

        /* Miss (or just evicted): insert under the per-sub lock, re-checking
         * both hashes so two lcores can't double-add (add would replace) */
        rte_spinlock_lock(&ppp_ccb->nat_insert_lock);
        if (rte_hash_lookup_data(ppp_ccb->nat_forward_hash, &fkey, &fdata) >= 0) {
            /* Raced: another lcore just learned this very flow */
            addr_table_t *entry = &ppp_ccb->addr_table[(U32)(uintptr_t)fdata];

            rte_spinlock_unlock(&ppp_ccb->nat_insert_lock);
            nat_expire_refresh(entry->expire_slot, nat_expiry_cycles());
            if (out_entry != NULL)
                *out_entry = entry;
            return entry->nat_port;
        }
        if (rte_hash_lookup_data(ppp_ccb->nat_reverse_hash, &key, &data) >= 0) {
            addr_table_t *entry = &ppp_ccb->addr_table[(U32)(uintptr_t)data];

            rte_spinlock_unlock(&ppp_ccb->nat_insert_lock);
            if (nat_entry_same_flow(entry, nat_port, src_ip, src_port, dst_ip, dst_port)) {
                nat_expire_refresh(entry->expire_slot, nat_expiry_cycles());
                if (out_entry != NULL)
                    *out_entry = entry;
                return nat_port;
            }
            goto next_nat_port; /* raced: someone else owns this key now */
        }

        U32 idx;
        if (nat_slot_alloc(ppp_ccb, &idx) != SUCCESS) {
            rte_spinlock_unlock(&ppp_ccb->nat_insert_lock);
            __atomic_fetch_add(&ppp_ccb->nat_enospc, 1, __ATOMIC_RELAXED);
            return 0; /* pool exhausted by live flows */
        }

        addr_table_t *entry = &ppp_ccb->addr_table[idx];
        rte_ether_addr_copy(&eth_hdr->src_addr, &entry->mac_addr);
        entry->src_ip = src_ip;
        entry->dst_ip = dst_ip;
        entry->src_port = src_port;
        entry->dst_port = dst_port;
        entry->nat_port = nat_port;
        entry->tcp_state = TCP_CONNTRACK_NONE;
        entry->tcp_fin_flags = 0;
        entry->max_seq_end_lan = 0;
        entry->max_seq_end_wan = 0;
        entry->max_ack_lan     = 0;
        entry->max_ack_wan     = 0;
        entry->max_win_lan     = 0;
        entry->max_win_wan     = 0;
        nat_expire_set(entry->expire_slot, nat_expiry_cycles());
        rte_atomic_thread_fence(rte_memory_order_release);
        rte_atomic16_set(&entry->is_fill, NAT_ENTRY_READY);

        if (rte_hash_add_key_data(ppp_ccb->nat_reverse_hash, &key,
                (void *)(uintptr_t)idx) < 0) {
            /* Hash ENOSPC: give the never-published slot straight back */
            rte_atomic16_set(&entry->is_fill, NAT_ENTRY_FREE);
            rte_ring_enqueue(ppp_ccb->nat_free_ring, (void *)(uintptr_t)idx);
            rte_spinlock_unlock(&ppp_ccb->nat_insert_lock);
            goto next_nat_port;
        }
        if (rte_hash_add_key_data(ppp_ccb->nat_forward_hash, &fkey,
                (void *)(uintptr_t)idx) < 0) {
            /* Forward ENOSPC: unpublish via the reverse key — a reader may
             * already hold the entry, so the slot must recycle through the
             * RCU dq, not go straight back.  A different port would not
             * help (fkey has no port in it), so fail the flow. */
            rte_hash_del_key(ppp_ccb->nat_reverse_hash, &key);
            rte_spinlock_unlock(&ppp_ccb->nat_insert_lock);
            __atomic_fetch_add(&ppp_ccb->nat_enospc, 1, __ATOMIC_RELAXED);
            return 0;
        }
        rte_spinlock_unlock(&ppp_ccb->nat_insert_lock);

        if (out_entry != NULL)
            *out_entry = entry;
        return nat_port;

next_nat_port:
        nat_port_host = rte_be_to_cpu_16(nat_port);
        nat_port_host++;
        if (nat_port_host >= TOTAL_SOCK_PORT)
            nat_port_host = SYS_MAX_PORT;
        nat_port = rte_cpu_to_be_16(nat_port_host);

    } while (nat_port != start_nat_port);

    /* All NAT ports exhausted */
    __atomic_fetch_add(&ppp_ccb->nat_enospc, 1, __ATOMIC_RELAXED);
    return 0;
}

/**
 * @fn nat_reverse_lookup
 * 
 * @brief Reverse lookup for inbound packets (WAN -> LAN)
 * Find SNAT entry for original SIP and SPORT
 * 
 * @param nat_port
 *        Pkt dst port in network byte order
 * @param remote_ip
 *        Remote source IP in network byte order
 * @param remote_port
 *        Remote source port in network byte order
 * @param ppp_ccb
 *        Subscriber control block (owns pool + hashes)
 *
 * @return Pointer to matching address table entry, or NULL if not found
 */
static inline addr_table_t *nat_reverse_lookup(U16 nat_port, U32 remote_ip, U16 remote_port,
    ppp_ccb_t *ppp_ccb)
{
    nat_reverse_key_t key = { .dst_ip = remote_ip, .nat_port = nat_port, .dst_port = remote_port };
    void *data;

    /* O(1) lock-free lookup — a miss is a miss, no table walk (kills the
     * port-scan full-scan DoS the probing implementation had) */
    if (rte_hash_lookup_data(ppp_ccb->nat_reverse_hash, &key, &data) < 0)
        return NULL;

    addr_table_t *entry = &ppp_ccb->addr_table[(U32)(uintptr_t)data];

    rte_atomic_thread_fence(rte_memory_order_acquire);

    /* WAN side is untrusted: an expired mapping is a miss — inbound traffic
     * must not revive it (would hold the UDP injection window open forever).
     * Unlink both keys; the slot recycles via the RCU defer queue. */
    if (nat_entry_is_expired(entry)) {
        nat_entry_del_keys(ppp_ccb, entry);
        return NULL;
    }

    nat_expire_refresh(entry->expire_slot, nat_expiry_cycles());
    return entry;
}

/**
 * @fn nat_icmp_learning
 * 
 * @brief NAT learning for ICMP packets
 *
 * @param ppp_ccb
 *        Subscriber control block (owns pool + hashes + port-fwd table)
 * @param eth_hdr
 *        Pointer to Ethernet header
 * @param ip_hdr
 *        Pointer to IPv4 header
 * @param icmphdr
 *        Pointer to ICMP header
 *
 * @return NAT port in network byte order, or 0 if all ports exhausted
 */
static inline U16 nat_icmp_learning(ppp_ccb_t *ppp_ccb, struct rte_ether_hdr *eth_hdr,
    struct rte_ipv4_hdr *ip_hdr, struct rte_icmp_hdr *icmphdr)
{
    return nat_learning_port_reuse(ppp_ccb, eth_hdr,
        ip_hdr->src_addr, ip_hdr->dst_addr,
        icmphdr->icmp_ident, icmphdr->icmp_type, NULL);
}

/**
 * @fn nat_udp_learning
 * 
 * @brief NAT learning for UDP packets
 *
 * @param ppp_ccb
 *        Subscriber control block (owns pool + hashes + port-fwd table)
 * @param eth_hdr
 *        Pointer to Ethernet header
 * @param ip_hdr
 *        Pointer to IPv4 header
 * @param udphdr
 *        Pointer to UDP header
 *
 * @return NAT port in network byte order, or 0 if all ports exhausted
 */
static inline U16 nat_udp_learning(ppp_ccb_t *ppp_ccb, struct rte_ether_hdr *eth_hdr,
    struct rte_ipv4_hdr *ip_hdr, struct rte_udp_hdr *udphdr)
{
    return nat_learning_port_reuse(ppp_ccb, eth_hdr,
        ip_hdr->src_addr, ip_hdr->dst_addr,
        udphdr->src_port, udphdr->dst_port, NULL);
}

/**
 * @fn nat_tcp_learning
 * 
 * @brief NAT learning for TCP packets with connection tracking
 *
 * @param ppp_ccb
 *        Subscriber control block (owns pool + hashes + port-fwd table)
 * @param eth_hdr
 *        Pointer to Ethernet header
 * @param ip_hdr
 *        Pointer to IPv4 header
 * @param tcphdr
 *        Pointer to TCP header
 *
 * @return NAT port in network byte order, or 0 if all ports exhausted
 */
static inline U16 nat_tcp_learning(ppp_ccb_t *ppp_ccb, struct rte_ether_hdr *eth_hdr,
    struct rte_ipv4_hdr *ip_hdr, struct rte_tcp_hdr *tcphdr)
{
    addr_table_t *entry = NULL;
    U16 nat_port = nat_learning_port_reuse(ppp_ccb, eth_hdr,
        ip_hdr->src_addr, ip_hdr->dst_addr,
        tcphdr->src_port, tcphdr->dst_port, &entry);

    if (nat_port != 0 && entry != NULL) {
        /* LAN→WAN: trusted, no seq validation; just run FSM and update
         * the LAN-direction baseline so WAN→LAN can be checked.  The entry
         * comes straight from learning — no second lookup needed. */
        tcp_conntrack_fsm(entry, tcphdr->tcp_flags, FALSE);
        U16 ip_hdr_len  = (ip_hdr->version_ihl & 0x0F) * 4;
        U16 tcp_hdr_len = ((tcphdr->data_off >> 4) & 0x0F) * 4;
        U16 payload_len = rte_be_to_cpu_16(ip_hdr->total_length) -
                          ip_hdr_len - tcp_hdr_len;
        tcp_conntrack_seq_update(entry, tcphdr, payload_len, FALSE);
    }
    return nat_port;
}

/*======================================================================
 * SNAT Port Forwarding Helpers  –  O(1) direct-index implementation
 *
 * Key idea: eport is U16 (0..65535), so we use it directly as the
 * array index.  No hash, no collision, no loop.
 *======================================================================*/

/**
 * @fn port_fwd_lookup_by_eport
 *
 * @brief O(1) lookup: index directly by eport value.
 *
 * @param table
 *      port_fwd_table[PORT_FWD_TABLE_SIZE] inside ppp_ccb
 * @param eport
 *      External port in HOST byte order
 *
 * @return
 *      Pointer to entry if active, NULL otherwise
 */
static inline port_fwd_entry_t *port_fwd_lookup_by_eport(
    port_fwd_entry_t table[], U16 eport)
{
    port_fwd_entry_t *entry = &table[eport];
    if (rte_atomic16_read(&entry->is_active) == 1)
        return entry;
    return NULL;
}

/**
 * @fn port_fwd_add
 *
 * @brief Add / update a static port forwarding entry.
 *        O(1): write directly to table[eport].
 *
 * @param table
 *      Port forwarding table
 * @param eport
 *      External port (host byte order)
 * @param dip
 *      Destination LAN IP (network byte order)
 * @param iport
 *      Internal port (host byte order)
 */
static inline void port_fwd_add(port_fwd_entry_t table[],
    U16 eport, U32 dip, U16 iport)
{
    port_fwd_entry_t *entry = &table[eport];
    if (rte_atomic16_read(&entry->is_active) == 1)
        return; /* Already active, do not overwrite to avoid disrupting existing flow */
    entry->dip = dip;
    entry->iport = iport;
    rte_atomic64_set(&entry->hit_count, 0);
    rte_atomic_thread_fence(rte_memory_order_release);
    rte_atomic16_set(&entry->is_active, 1);
}

/**
 * @fn port_fwd_remove
 *
 * @brief Remove a port forwarding entry.
 *        O(1): clear table[eport] directly.
 *
 * @param table
 *      Port forwarding table
 * @param eport
 *      External port (host byte order)
 *
 * @return
 *      SUCCESS on success, ERROR if entry was not active
 */
static inline STATUS port_fwd_remove(port_fwd_entry_t table[], U16 eport)
{
    port_fwd_entry_t *entry = &table[eport];
    if (rte_atomic16_read(&entry->is_active) == 0)
        return ERROR;
    rte_atomic16_set(&entry->is_active, 0);
    rte_atomic_thread_fence(rte_memory_order_release);
    entry->dip = 0;
    entry->iport = 0;
    rte_atomic64_set(&entry->hit_count, 0);
    return SUCCESS;
}

/**
 * @fn nat_port_fwd_reverse_lookup
 *
 * @brief O(1) inbound port-forward lookup for the data-path hot path.
 *
 * @param table
 *      port_fwd_table inside ppp_ccb
 * @param dst_port
 *      WAN packet destination port, NETWORK byte order
 * @param out_dip
 *      [out] LAN destination IP (network byte order)
 * @param out_iport
 *      [out] LAN internal port (network byte order)
 *
 * @return
 *      SUCCESS if matched, ERROR if not
 */
static inline STATUS nat_port_fwd_reverse_lookup(port_fwd_entry_t table[],
    U16 dst_port, U32 *out_dip, U16 *out_iport)
{
    U16 eport_host = rte_be_to_cpu_16(dst_port);
    port_fwd_entry_t *entry = port_fwd_lookup_by_eport(table, eport_host);
    if (entry == NULL)
        return ERROR;
    *out_dip = entry->dip;
    *out_iport = rte_cpu_to_be_16(entry->iport);
    return SUCCESS;
}

#endif
