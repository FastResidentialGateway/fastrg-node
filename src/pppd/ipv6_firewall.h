/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  IPV6_FIREWALL.H

     Per-subscriber IPv6 stateful firewall.

     Inbound traffic is denied unless it belongs to a session a LAN host
     opened. One hash serves both directions: the key is always written
     LAN side first, so a packet and its reply produce the same key.

  Designed by THE on Aug 15, 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _IPV6_FIREWALL_H_
#define _IPV6_FIREWALL_H_

#include <string.h>

#include <common.h>

#include <rte_atomic.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_ip6.h>
#include <rte_memcpy.h>
#include <rte_rcu_qsbr.h>
#include <rte_ring.h>
#include <rte_spinlock.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "pppd.h"
#include "tcp_conntrack.h"

/** Idle lifetime of a UDP or ICMPv6 echo session; TCP uses the per-state
 *  conntrack timeouts instead. */
#define IPV6_FIREWALL_ENTRY_TIMEOUT_SEC 10

/** Slots examined per amortized GC call; the per-subscriber cursor persists,
 *  so successive calls walk the whole pool. */
#define IPV6_FIREWALL_GC_SCAN_CHUNK 512

#define IPV6_FIREWALL_ENTRY_FREE  0
#define IPV6_FIREWALL_ENTRY_READY 2

/*--------- ICMPv6 MESSAGE TYPES ----------*/
#define ICMP6_DST_UNREACH    1
#define ICMP6_PACKET_TOO_BIG 2
#define ICMP6_TIME_EXCEEDED  3
#define ICMP6_PARAM_PROBLEM  4
#define ICMP6_ECHO_REQUEST   128
#define ICMP6_ECHO_REPLY     129

/** Every ICMPv6 header is 8 bytes: type, code, checksum and a 4-byte body. */
#define ICMP6_PTB_HDR_LEN    8

/**
 * @fn icmp6_type_is_error
 *
 * @brief Tell an ICMPv6 error message (types 1 to 4) from an informational
 *        one (128 and up).
 *
 * @param type
 *        ICMPv6 type byte
 *
 * @return TRUE for an error message
 */
static __always_inline BOOL icmp6_type_is_error(U8 type)
{
    return type >= ICMP6_DST_UNREACH && type <= ICMP6_PARAM_PROBLEM;
}

/**
 * @fn ipv6_firewall_expiry_cycles
 *
 * @brief Compute the absolute TSC deadline of a new or refreshed session.
 *
 * @return Deadline in CPU cycles
 */
static __always_inline U64 ipv6_firewall_expiry_cycles(void)
{
    return fastrg_get_cur_cycles() +
        (U64)IPV6_FIREWALL_ENTRY_TIMEOUT_SEC * fastrg_get_cycles_in_sec();
}

/**
 * @fn ipv6_firewall_entry_is_expired
 *
 * @brief Check whether a session has passed its deadline. A freed slot carries
 *        deadline 0 and therefore reads as expired.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param idx
 *        Pool slot index
 *
 * @return Non-zero when the session has expired
 */
static __always_inline int ipv6_firewall_entry_is_expired(const ppp_ccb_t *ppp_ccb, U32 idx)
{
    return fastrg_get_cur_cycles() >
        __atomic_load_n(&ppp_ccb->ipv6_firewall_expire_at[idx], __ATOMIC_RELAXED);
}

/**
 * @fn ipv6_firewall_key_build
 *
 * @brief Fill a session key from an already normalized LAN-first tuple.
 *
 *        rte_hash compares raw key bytes, padding included, so the key must
 *        be zeroed before any field is written.
 *
 * @param key
 *        [out] Key to fill
 * @param lan_addr
 *        Subscriber-side address, 16 bytes
 * @param remote_addr
 *        Internet-side address, 16 bytes
 * @param lan_port
 *        LAN port, or the echo identifier for ICMPv6
 * @param remote_port
 *        Remote port, or 0 for ICMPv6
 * @param proto
 *        L4 protocol number
 */
static __always_inline void ipv6_firewall_key_build(ipv6_firewall_key_t *key,
    const U8 *lan_addr, const U8 *remote_addr, U16 lan_port, U16 remote_port,
    U8 proto)
{
    memset(key, 0, sizeof(*key));
    rte_memcpy(key->lan_addr, lan_addr, 16);
    rte_memcpy(key->remote_addr, remote_addr, 16);
    key->lan_port = lan_port;
    key->remote_port = remote_port;
    key->proto = proto;
}

/**
 * @fn ipv6_firewall_key_from_packet
 *
 * @brief Build the session key of one IPv6 packet, LAN side first, so both
 *        directions of a flow land on the same key.
 *
 *        Extension headers are not walked, so a packet carrying one is not
 *        trackable.
 *
 * @param ip6
 *        IPv6 header
 * @param l4_len
 *        Bytes readable behind the IPv6 header
 * @param inbound
 *        TRUE when the packet arrived from the WAN
 * @param key
 *        [out] Written only when the function returns TRUE
 *
 * @return TRUE when the packet carries a trackable tuple
 */
static __always_inline BOOL ipv6_firewall_key_from_packet(const struct rte_ipv6_hdr *ip6,
    U16 l4_len, BOOL inbound, ipv6_firewall_key_t *key)
{
    const U8 *src = (const U8 *)&ip6->src_addr;
    const U8 *dst = (const U8 *)&ip6->dst_addr;
    const U8 *l4 = (const U8 *)(ip6 + 1);
    U16 lan_port, remote_port;

    if (ip6->proto == IPPROTO_TCP || ip6->proto == IPPROTO_UDP) {
        const U16 *ports = (const U16 *)l4;

        if (unlikely(l4_len < 2 * sizeof(U16)))
            return FALSE;
        lan_port    = inbound ? ports[1] : ports[0];
        remote_port = inbound ? ports[0] : ports[1];
    } else if (ip6->proto == IPPROTO_ICMPV6) {
        if (unlikely(l4_len < ICMP6_PTB_HDR_LEN))
            return FALSE;
        /* Echo is the only tracked ICMPv6 exchange. */
        if (l4[0] != (inbound ? ICMP6_ECHO_REPLY : ICMP6_ECHO_REQUEST))
            return FALSE;
        rte_memcpy(&lan_port, l4 + 4, sizeof(lan_port)); /* echo identifier */
        remote_port = 0;
    } else {
        return FALSE;
    }

    ipv6_firewall_key_build(key, inbound ? dst : src, inbound ? src : dst,
        lan_port, remote_port, ip6->proto);
    return TRUE;
}

/**
 * @fn ipv6_firewall_conntrack_view
 *
 * @brief Point a conntrack view at an IPv6 firewall session.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param entry
 *        Session entry
 * @param idx
 *        Pool slot index of that entry
 *
 * @return View of the session's conntrack fields
 */
static __always_inline tcp_conntrack_view_t ipv6_firewall_conntrack_view(
    ppp_ccb_t *ppp_ccb, ipv6_firewall_entry_t *entry, U32 idx)
{
    tcp_conntrack_view_t view = {
        .tcp_state       = &entry->tcp_state,
        .tcp_fin_flags   = &entry->tcp_fin_flags,
        .expire_slot     = &ppp_ccb->ipv6_firewall_expire_at[idx],
        .max_seq_end_lan = &entry->max_seq_end_lan,
        .max_seq_end_wan = &entry->max_seq_end_wan,
        .max_ack_lan     = &entry->max_ack_lan,
        .max_ack_wan     = &entry->max_ack_wan,
        .max_win_lan     = &entry->max_win_lan,
        .max_win_wan     = &entry->max_win_wan,
    };

    return view;
}

/**
 * @fn ipv6_firewall_hash_free_cb
 *
 * @brief RCU defer-queue callback: recycles the slot once every data-plane
 *        reader has passed a quiescent state.
 *
 * @param p
 *        ppp_ccb_t of the owning subscriber (rcu cfg key_data_ptr)
 * @param key_data
 *        Pool slot index stored as the hash value (cast via uintptr_t)
 */
static inline void ipv6_firewall_hash_free_cb(void *p, void *key_data)
{
    ppp_ccb_t *ppp_ccb = (ppp_ccb_t *)p;
    U32 idx = (U32)(uintptr_t)key_data;

    /* Zeroing both SoA stamps is what marks the slot free to the GC and the
     * LRU scan. */
    __atomic_store_n(&ppp_ccb->ipv6_firewall_expire_at[idx], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ppp_ccb->ipv6_firewall_last_used[idx], 0, __ATOMIC_RELAXED);
    rte_atomic16_set(&ppp_ccb->ipv6_firewall_table[idx].is_fill, IPV6_FIREWALL_ENTRY_FREE);
    rte_ring_enqueue(ppp_ccb->ipv6_firewall_free_ring, (void *)(uintptr_t)idx);
}

/**
 * @fn ipv6_firewall_table_reset
 *
 * @brief Flush every session of a subscriber and refill the free-list.
 *
 *        Control-plane only: the caller must close the IPv6 forwarding gate
 *        first, so no data-plane traffic races this subscriber.
 *
 * @param ppp_ccb
 *        Subscriber control block
 */
static inline void ipv6_firewall_table_reset(ppp_ccb_t *ppp_ccb)
{
    unsigned int freed, pending, avail;

    /* Drain the defer queue first: a deferred free surviving the reset would
     * push an already re-issued slot index into the refilled free ring.
     * Data lcores report quiescent every poll loop, so this terminates. */
    do {
        freed = pending = avail = 0;
        rte_hash_rcu_qsbr_dq_reclaim(ppp_ccb->ipv6_firewall_hash, &freed, &pending, &avail);
    } while (pending != 0);

    rte_hash_reset(ppp_ccb->ipv6_firewall_hash);
    rte_ring_reset(ppp_ccb->ipv6_firewall_free_ring);
    for(U32 i=0; i<IPV6_FIREWALL_MAX_ENTRIES; i++) {
        ppp_ccb->ipv6_firewall_expire_at[i] = 0;
        ppp_ccb->ipv6_firewall_last_used[i] = 0;
        rte_atomic16_set(&ppp_ccb->ipv6_firewall_table[i].is_fill, IPV6_FIREWALL_ENTRY_FREE);
        rte_ring_enqueue(ppp_ccb->ipv6_firewall_free_ring, (void *)(uintptr_t)i);
    }
    ppp_ccb->ipv6_firewall_gc_counter = 0;
    ppp_ccb->ipv6_firewall_enospc = 0;
    ppp_ccb->ipv6_firewall_gc_reclaimed = 0;
    ppp_ccb->ipv6_firewall_evicted = 0;
    ppp_ccb->ipv6_firewall_icmp6_err_passed = 0;
    ppp_ccb->ipv6_firewall_icmp6_err_dropped = 0;
}

/**
 * @fn ipv6_firewall_table_destroy
 *
 * @brief Free the per-subscriber hash and free-list ring (subscriber removal
 *        or init rollback). Idempotent.
 *
 * @param ppp_ccb
 *        Subscriber control block
 */
static inline void ipv6_firewall_table_destroy(ppp_ccb_t *ppp_ccb)
{
    if (ppp_ccb->ipv6_firewall_hash != NULL) {
        rte_hash_free(ppp_ccb->ipv6_firewall_hash);
        ppp_ccb->ipv6_firewall_hash = NULL;
    }
    if (ppp_ccb->ipv6_firewall_free_ring != NULL) {
        rte_ring_free(ppp_ccb->ipv6_firewall_free_ring);
        ppp_ccb->ipv6_firewall_free_ring = NULL;
    }
}

/**
 * @fn ipv6_firewall_table_init
 *
 * @brief Create the per-subscriber session hash and free-list ring, attach
 *        the shared QSBR RCU for deferred reclaim, and fill the free-list.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param ccb_id
 *        Subscriber index, used for unique DPDK object naming
 * @param rcu
 *        QSBR variable shared with the data-plane lcores (ppp_ccb_rcu)
 *
 * @return SUCCESS / ERROR
 */
static inline STATUS ipv6_firewall_table_init(ppp_ccb_t *ppp_ccb, U16 ccb_id,
    struct rte_rcu_qsbr *rcu)
{
    char name[RTE_RING_NAMESIZE];

    snprintf(name, sizeof(name), "ipv6_firewall_%u", ccb_id);
    ppp_ccb->ipv6_firewall_hash = rte_hash_find_existing(name);
    if (ppp_ccb->ipv6_firewall_hash == NULL) {
        struct rte_hash_parameters params = {
            .name = name,
            .entries = IPV6_FIREWALL_MAX_ENTRIES,
            .key_len = sizeof(ipv6_firewall_key_t),
            .hash_func = rte_hash_crc,
            .hash_func_init_val = 0,
            .socket_id = (int)rte_socket_id(),
            .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF |
                          RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD,
        };
        ppp_ccb->ipv6_firewall_hash = rte_hash_create(&params);
        if (ppp_ccb->ipv6_firewall_hash == NULL)
            return ERROR;
        struct rte_hash_rcu_config rcu_cfg = {
            .v = rcu,
            .mode = RTE_HASH_QSBR_MODE_DQ,
            .key_data_ptr = ppp_ccb,
            .free_key_data_func = ipv6_firewall_hash_free_cb,
        };
        if (rte_hash_rcu_qsbr_add(ppp_ccb->ipv6_firewall_hash, &rcu_cfg) != 0) {
            rte_hash_free(ppp_ccb->ipv6_firewall_hash);
            ppp_ccb->ipv6_firewall_hash = NULL;
            return ERROR;
        }
    }

    snprintf(name, sizeof(name), "ipv6_firewall_free_%u", ccb_id);
    ppp_ccb->ipv6_firewall_free_ring = rte_ring_lookup(name);
    if (ppp_ccb->ipv6_firewall_free_ring == NULL) {
        ppp_ccb->ipv6_firewall_free_ring = rte_ring_create(name, IPV6_FIREWALL_MAX_ENTRIES,
            (int)rte_socket_id(), RING_F_EXACT_SZ);
        if (ppp_ccb->ipv6_firewall_free_ring == NULL) {
            ipv6_firewall_table_destroy(ppp_ccb);
            return ERROR;
        }
    }

    ipv6_firewall_table_reset(ppp_ccb);
    return SUCCESS;
}

/**
 * @fn ipv6_firewall_gc_scan_by_ccb
 *
 * @brief Amortized garbage collection: unlink every expired session in up to
 *        max_slots pool slots from the per-subscriber cursor.
 *
 *        Safe from any lcore: the hash is multi-writer and a duplicate delete
 *        just returns ENOENT.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param max_slots
 *        Upper bound of slots to examine in this call
 *
 * @return Number of expired sessions whose keys were deleted
 */
static inline U32 ipv6_firewall_gc_scan_by_ccb(ppp_ccb_t *ppp_ccb, U32 max_slots)
{
    U32 start = __atomic_fetch_add(&ppp_ccb->ipv6_firewall_gc_counter, max_slots,
        __ATOMIC_RELAXED);
    U32 reclaimed = 0;
    unsigned int freed = 0, pending = 0, available = 0;
    U64 now = fastrg_get_cur_cycles();

    for(U32 n=0; n<max_slots; n++) {
        U32 idx = (start + n) % IPV6_FIREWALL_MAX_ENTRIES;

        /* Deadline 0 marks a free slot. */
        U64 deadline = __atomic_load_n(&ppp_ccb->ipv6_firewall_expire_at[idx], __ATOMIC_RELAXED);
        if (deadline == 0 || deadline > now)
            continue;

        ipv6_firewall_entry_t *entry = &ppp_ccb->ipv6_firewall_table[idx];
        if (rte_atomic16_read(&entry->is_fill) != IPV6_FIREWALL_ENTRY_READY)
            continue;
        if (rte_hash_del_key(ppp_ccb->ipv6_firewall_hash, &entry->key) >= 0)
            reclaimed++;
    }
    if (reclaimed > 0)
        __atomic_fetch_add(&ppp_ccb->ipv6_firewall_gc_reclaimed, (U64)reclaimed,
            __ATOMIC_RELAXED);

    /* The defer queue's automatic reclaim threshold may never be reached, so
     * drain it on every GC tick. */
    rte_hash_rcu_qsbr_dq_reclaim(ppp_ccb->ipv6_firewall_hash, &freed, &pending, &available);
    return reclaimed;
}

/**
 * @fn ipv6_firewall_evict_lru
 *
 * @brief Unlink the least recently used session so a new one can take its
 *        slot.
 *
 *        Caller must hold the subscriber's insert lock.
 *
 * @param ppp_ccb
 *        Subscriber control block
 *
 * @return 1 when a victim's key was deleted, 0 when there was nothing to evict
 */
static inline int ipv6_firewall_evict_lru(ppp_ccb_t *ppp_ccb)
{
    U64 oldest = UINT64_MAX;
    U32 victim = IPV6_FIREWALL_MAX_ENTRIES;

    for(U32 i=0; i<IPV6_FIREWALL_MAX_ENTRIES; i++) {
        U64 stamp = __atomic_load_n(&ppp_ccb->ipv6_firewall_last_used[i], __ATOMIC_RELAXED);

        if (stamp == 0 || stamp >= oldest)
            continue;
        oldest = stamp;
        victim = i;
    }
    if (victim == IPV6_FIREWALL_MAX_ENTRIES)
        return 0;

    ipv6_firewall_entry_t *entry = &ppp_ccb->ipv6_firewall_table[victim];
    if (rte_atomic16_read(&entry->is_fill) != IPV6_FIREWALL_ENTRY_READY)
        return 0;
    if (rte_hash_del_key(ppp_ccb->ipv6_firewall_hash, &entry->key) < 0)
        return 0;
    __atomic_fetch_add(&ppp_ccb->ipv6_firewall_evicted, 1, __ATOMIC_RELAXED);
    return 1;
}

/**
 * @fn ipv6_firewall_slot_alloc
 *
 * @brief Pop a free pool slot, falling back to expired-session reclaim and
 *        then LRU eviction.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param idx
 *        [out] Allocated pool slot index
 *
 * @return SUCCESS, or ERROR when the evicted slot is still waiting for readers
 *         to go quiescent
 */
static inline STATUS ipv6_firewall_slot_alloc(ppp_ccb_t *ppp_ccb, U32 *idx)
{
    void *obj;
    unsigned int freed = 0, pending = 0, available = 0;

    if (likely(rte_ring_dequeue(ppp_ccb->ipv6_firewall_free_ring, &obj) == 0)) {
        *idx = (U32)(uintptr_t)obj;
        return SUCCESS;
    }

    ipv6_firewall_gc_scan_by_ccb(ppp_ccb, IPV6_FIREWALL_GC_SCAN_CHUNK);
    if (rte_ring_dequeue(ppp_ccb->ipv6_firewall_free_ring, &obj) == 0) {
        *idx = (U32)(uintptr_t)obj;
        return SUCCESS;
    }

    if (ipv6_firewall_evict_lru(ppp_ccb)) {
        rte_hash_rcu_qsbr_dq_reclaim(ppp_ccb->ipv6_firewall_hash, &freed, &pending, &available);
        if (rte_ring_dequeue(ppp_ccb->ipv6_firewall_free_ring, &obj) == 0) {
            *idx = (U32)(uintptr_t)obj;
            return SUCCESS;
        }
    }
    return ERROR;
}

/**
 * @fn ipv6_firewall_lookup
 *
 * @brief Find a session by key, expired or not.
 *
 *        The acquire fence pairs with the release fence in
 *        ipv6_firewall_insert(), so a hit may read the entry fields.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param key
 *        Normalized session key
 * @param out_idx
 *        [out] Pool slot index, written only on a hit
 *
 * @return The session entry, or NULL when there is none
 */
static __always_inline ipv6_firewall_entry_t *ipv6_firewall_lookup(ppp_ccb_t *ppp_ccb,
    const ipv6_firewall_key_t *key, U32 *out_idx)
{
    void *data;

    if (rte_hash_lookup_data(ppp_ccb->ipv6_firewall_hash, key, &data) < 0)
        return NULL;

    rte_atomic_thread_fence(rte_memory_order_acquire);
    *out_idx = (U32)(uintptr_t)data;
    return &ppp_ccb->ipv6_firewall_table[*out_idx];
}

/**
 * @fn ipv6_firewall_lookup_live
 *
 * @brief Session lookup for inbound packets: an expired session counts as a
 *        miss and is unlinked on the spot, never revived.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param key
 *        Normalized session key
 * @param out_idx
 *        [out] Pool slot index, written only on a hit
 *
 * @return The live session entry, or NULL
 */
static __always_inline ipv6_firewall_entry_t *ipv6_firewall_lookup_live(ppp_ccb_t *ppp_ccb,
    const ipv6_firewall_key_t *key, U32 *out_idx)
{
    ipv6_firewall_entry_t *entry = ipv6_firewall_lookup(ppp_ccb, key, out_idx);

    if (entry == NULL)
        return NULL;
    if (unlikely(ipv6_firewall_entry_is_expired(ppp_ccb, *out_idx))) {
        rte_hash_del_key(ppp_ccb->ipv6_firewall_hash, key);
        return NULL;
    }
    return entry;
}

/**
 * @fn ipv6_firewall_mark_used
 *
 * @brief Record traffic on a session: push its idle deadline out and stamp
 *        the LRU recency.
 *
 *        Both writes coalesce to at most one per session per second.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param idx
 *        Pool slot index
 */
static __always_inline void ipv6_firewall_mark_used(ppp_ccb_t *ppp_ccb, U32 idx)
{
    U64 now = fastrg_get_cur_cycles();
    U64 last = __atomic_load_n(&ppp_ccb->ipv6_firewall_last_used[idx], __ATOMIC_RELAXED);

    nat_expire_refresh(&ppp_ccb->ipv6_firewall_expire_at[idx],
        now + (U64)IPV6_FIREWALL_ENTRY_TIMEOUT_SEC * fastrg_get_cycles_in_sec());
    if (now - last > (U64)NAT_EXPIRE_COALESCE_SEC * fastrg_get_cycles_in_sec())
        __atomic_store_n(&ppp_ccb->ipv6_firewall_last_used[idx], now, __ATOMIC_RELAXED);
}

/**
 * @fn ipv6_firewall_insert
 *
 * @brief Open a new session under the per-subscriber insert lock, re-checking
 *        the hash because rte_hash_add_key_data silently replaces an existing
 *        key.
 *
 *        The release fence orders the entry writes ahead of the READY flag
 *        and the hash insertion that expose it to other lcores.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param key
 *        Normalized session key
 * @param out_idx
 *        [out] Pool slot index, written only on success
 *
 * @return The session entry, or NULL when no slot could be obtained
 */
static inline ipv6_firewall_entry_t *ipv6_firewall_insert(ppp_ccb_t *ppp_ccb,
    const ipv6_firewall_key_t *key, U32 *out_idx)
{
    ipv6_firewall_entry_t *entry;
    void *data;
    U32 idx;

    rte_spinlock_lock(&ppp_ccb->ipv6_firewall_insert_lock);
    if (rte_hash_lookup_data(ppp_ccb->ipv6_firewall_hash, key, &data) >= 0) {
        /* Raced: another lcore just opened this very session. */
        rte_spinlock_unlock(&ppp_ccb->ipv6_firewall_insert_lock);
        *out_idx = (U32)(uintptr_t)data;
        return &ppp_ccb->ipv6_firewall_table[*out_idx];
    }
    if (unlikely(ipv6_firewall_slot_alloc(ppp_ccb, &idx) != SUCCESS)) {
        rte_spinlock_unlock(&ppp_ccb->ipv6_firewall_insert_lock);
        __atomic_fetch_add(&ppp_ccb->ipv6_firewall_enospc, 1, __ATOMIC_RELAXED);
        return NULL;
    }

    entry = &ppp_ccb->ipv6_firewall_table[idx];
    entry->key = *key;
    entry->tcp_state = TCP_CONNTRACK_NONE;
    entry->tcp_fin_flags = 0;
    entry->max_seq_end_lan = 0;
    entry->max_seq_end_wan = 0;
    entry->max_ack_lan     = 0;
    entry->max_ack_wan     = 0;
    entry->max_win_lan     = 0;
    entry->max_win_wan     = 0;
    nat_expire_set(&ppp_ccb->ipv6_firewall_expire_at[idx], ipv6_firewall_expiry_cycles());
    __atomic_store_n(&ppp_ccb->ipv6_firewall_last_used[idx], fastrg_get_cur_cycles(),
        __ATOMIC_RELAXED);
    rte_atomic_thread_fence(rte_memory_order_release);
    rte_atomic16_set(&entry->is_fill, IPV6_FIREWALL_ENTRY_READY);

    if (unlikely(rte_hash_add_key_data(ppp_ccb->ipv6_firewall_hash, key,
            (void *)(uintptr_t)idx) < 0)) {
        /* Hash full: the slot was never published, so it goes straight back
         * instead of through the defer queue. */
        __atomic_store_n(&ppp_ccb->ipv6_firewall_expire_at[idx], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&ppp_ccb->ipv6_firewall_last_used[idx], 0, __ATOMIC_RELAXED);
        rte_atomic16_set(&entry->is_fill, IPV6_FIREWALL_ENTRY_FREE);
        rte_ring_enqueue(ppp_ccb->ipv6_firewall_free_ring, (void *)(uintptr_t)idx);
        rte_spinlock_unlock(&ppp_ccb->ipv6_firewall_insert_lock);
        __atomic_fetch_add(&ppp_ccb->ipv6_firewall_enospc, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    rte_spinlock_unlock(&ppp_ccb->ipv6_firewall_insert_lock);

    *out_idx = idx;
    return entry;
}

/**
 * @fn ipv6_firewall_learn
 *
 * @brief Open or renew the session an outbound packet belongs to, so its
 *        reply is allowed back in.
 *
 *        A packet that gets no session (untrackable tuple, no slot available)
 *        is still forwarded, but its reply will be dropped.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param ip6
 *        IPv6 header of the outbound packet
 */
static __always_inline void ipv6_firewall_learn(ppp_ccb_t *ppp_ccb,
    struct rte_ipv6_hdr *ip6)
{
    U16 payload_len = rte_be_to_cpu_16(ip6->payload_len);
    ipv6_firewall_entry_t *entry;
    ipv6_firewall_key_t key;
    U32 idx = 0;

    if (unlikely(ppp_ccb->ipv6_firewall_hash == NULL))
        return;
    /* conntrack reads the whole TCP header, so require it before learning. */
    if (ip6->proto == IPPROTO_TCP && unlikely(payload_len < sizeof(struct rte_tcp_hdr)))
        return;
    if (ip6->proto == IPPROTO_UDP && unlikely(payload_len < sizeof(struct rte_udp_hdr)))
        return;
    if (ipv6_firewall_key_from_packet(ip6, payload_len, FALSE, &key) == FALSE)
        return;

    entry = ipv6_firewall_lookup(ppp_ccb, &key, &idx);
    if (unlikely(entry == NULL)) {
        entry = ipv6_firewall_insert(ppp_ccb, &key, &idx);
        if (unlikely(entry == NULL))
            return;
    } else {
        ipv6_firewall_mark_used(ppp_ccb, idx);
    }

    if (ip6->proto == IPPROTO_TCP) {
        struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip6 + 1);
        tcp_conntrack_view_t view = ipv6_firewall_conntrack_view(ppp_ccb, entry, idx);
        U16 hdr_len = (U16)(((tcp->data_off >> 4) & 0x0F) * 4);
        U16 tcp_payload = payload_len > hdr_len ? (U16)(payload_len - hdr_len) : 0;

        /* LAN to WAN is trusted: no sequence validation, only the state
         * machine and the LAN-side baseline the reply is checked against. */
        tcp_conntrack_fsm_view(&view, tcp->tcp_flags, FALSE);
        tcp_conntrack_seq_update_view(&view, tcp, tcp_payload, FALSE);
    }
}

/**
 * @fn ipv6_firewall_tcp_inbound_pass
 *
 * @brief Stateful inspection of an inbound TCP packet that already matched a
 *        live session.
 *
 *        The checks always run; tcp_conntrack_enabled only decides whether a
 *        packet failing them is dropped or forwarded unenforced.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param entry
 *        Matched session entry
 * @param idx
 *        Pool slot index of that entry
 * @param ip6
 *        IPv6 header of the inbound packet
 * @param payload_len
 *        Bytes behind the IPv6 header
 *
 * @return TRUE to forward the packet to the LAN
 */
static __always_inline BOOL ipv6_firewall_tcp_inbound_pass(ppp_ccb_t *ppp_ccb,
    ipv6_firewall_entry_t *entry, U32 idx, struct rte_ipv6_hdr *ip6, U16 payload_len)
{
    struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip6 + 1);
    tcp_conntrack_view_t view = ipv6_firewall_conntrack_view(ppp_ccb, entry, idx);
    BOOL inbound_ok, seq_ok;
    U16 hdr_len, tcp_payload;

    if (unlikely(payload_len < sizeof(struct rte_tcp_hdr)))
        return FALSE;

    inbound_ok = tcp_conntrack_inbound_valid(entry->tcp_state, tcp->tcp_flags);
    seq_ok = (inbound_ok != FALSE) ?
             tcp_conntrack_seq_valid_view(&view, tcp, TRUE) : FALSE;
    if (inbound_ok == FALSE || seq_ok == FALSE)
        return ppp_ccb->tcp_conntrack_enabled ? FALSE : TRUE;

    hdr_len = (U16)(((tcp->data_off >> 4) & 0x0F) * 4);
    tcp_payload = payload_len > hdr_len ? (U16)(payload_len - hdr_len) : 0;
    tcp_conntrack_fsm_view(&view, tcp->tcp_flags, TRUE);
    tcp_conntrack_seq_update_view(&view, tcp, tcp_payload, TRUE);
    return TRUE;
}

/**
 * @fn ipv6_firewall_icmp6_error_pass
 *
 * @brief Decide whether an inbound ICMPv6 error message may reach the LAN.
 *
 *        The outer source is a router on the path, so the match is made on the
 *        packet quoted inside: its source must be the notified LAN host and
 *        its tuple must name a session that is still alive.
 *
 *        Passing one must not refresh that session, and every quoted byte is
 *        attacker controlled and is only ever read.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param ip6
 *        IPv6 header of the inbound error message
 * @param payload_len
 *        Bytes behind the IPv6 header
 *
 * @return TRUE to forward the error message to the LAN
 */
static __always_inline BOOL ipv6_firewall_icmp6_error_pass(ppp_ccb_t *ppp_ccb,
    const struct rte_ipv6_hdr *ip6, U16 payload_len)
{
    const struct rte_ipv6_hdr *quoted = (const struct rte_ipv6_hdr *)
        ((const U8 *)(ip6 + 1) + ICMP6_PTB_HDR_LEN);
    ipv6_firewall_key_t key;
    U16 quoted_l4_len;
    U32 idx = 0;

    /* Required minimum: error header, quoted IPv6 header and the first 8
     * bytes of the quoted L4 header. This is the only length check on this
     * path. */
    if (unlikely(payload_len < ICMP6_PTB_HDR_LEN + sizeof(*quoted) + ICMP6_PTB_HDR_LEN))
        goto drop;
    quoted_l4_len = (U16)(payload_len - ICMP6_PTB_HDR_LEN - sizeof(*quoted));

    /* An error report only ever travels back to the sender of the quoted
     * packet, so those two addresses must be the same host. */
    if (memcmp(&quoted->src_addr, &ip6->dst_addr, 16) != 0)
        goto drop;
    if (ipv6_firewall_key_from_packet(quoted, quoted_l4_len, FALSE, &key) == FALSE)
        goto drop;
    if (ipv6_firewall_lookup_live(ppp_ccb, &key, &idx) == NULL)
        goto drop;

    __atomic_fetch_add(&ppp_ccb->ipv6_firewall_icmp6_err_passed, 1, __ATOMIC_RELAXED);
    return TRUE;

drop:
    __atomic_fetch_add(&ppp_ccb->ipv6_firewall_icmp6_err_dropped, 1, __ATOMIC_RELAXED);
    return FALSE;
}

/**
 * @fn ipv6_firewall_inbound_pass
 *
 * @brief Decide whether an inbound IPv6 packet addressed to the LAN may be
 *        forwarded.
 *
 *        Default deny: only what answers something a LAN host started gets
 *        through.
 *
 * @param ppp_ccb
 *        Subscriber control block
 * @param ip6
 *        IPv6 header of the inbound packet
 *
 * @return TRUE to forward the packet to the LAN
 */
static __always_inline BOOL ipv6_firewall_inbound_pass(ppp_ccb_t *ppp_ccb,
    struct rte_ipv6_hdr *ip6)
{
    U16 payload_len = rte_be_to_cpu_16(ip6->payload_len);
    const U8 *l4 = (const U8 *)(ip6 + 1);
    ipv6_firewall_entry_t *entry;
    ipv6_firewall_key_t key;
    U32 idx = 0;

    if (unlikely(ppp_ccb->ipv6_firewall_hash == NULL))
        return FALSE;

    /* The payload_len guard must come before reading the type byte. */
    if (unlikely(ip6->proto == IPPROTO_ICMPV6 && payload_len > 0 &&
            icmp6_type_is_error(l4[0])))
        return ipv6_firewall_icmp6_error_pass(ppp_ccb, ip6, payload_len);

    if (likely(ipv6_firewall_key_from_packet(ip6, payload_len, TRUE, &key) == TRUE)) {
        entry = ipv6_firewall_lookup_live(ppp_ccb, &key, &idx);
        if (likely(entry != NULL)) {
            ipv6_firewall_mark_used(ppp_ccb, idx);
            if (ip6->proto != IPPROTO_TCP)
                return TRUE;
            return ipv6_firewall_tcp_inbound_pass(ppp_ccb, entry, idx, ip6, payload_len);
        }
    }

    /* TODO: consult this subscriber's user-defined inbound rules here, once
     * the northbound carries them, and return TRUE for a packet they allow. */
    return FALSE;
}

#endif /* _IPV6_FIREWALL_H_ */
