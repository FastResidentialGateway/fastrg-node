/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  IPV6_FIREWALL.H

     Per-subscriber IPv6 stateful firewall.

     IPv6 is routed, not translated, so nothing stops the internet from
     addressing a LAN host directly the way NAT does for IPv4. This table
     supplies that missing protection: inbound traffic is denied unless it
     belongs to a session a LAN host opened.

     One hash serves both directions. The key is always written LAN side
     first, so an outbound packet and its reply produce the same key and one
     insertion covers the flow — unlike NAT, which needs two hashes because it
     rewrites the port.

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

/** Idle lifetime of a UDP or ICMPv6 echo session. TCP uses the per-state
 *  conntrack timeouts instead, which the state machine writes on top. */
#define IPV6_FIREWALL_ENTRY_TIMEOUT_SEC 10

/** Slots examined per amortized GC call. Bounded so the cost stays small on
 *  the RX loop; the per-subscriber cursor persists, so successive calls walk
 *  the whole pool. */
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
 * @brief Tell an ICMPv6 error message from an informational one: types 1 to 4
 *        report what happened to a packet, 128 and up are queries and replies.
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
 * @brief Compute the absolute TSC deadline of a new or refreshed session
 *        (now + IPV6_FIREWALL_ENTRY_TIMEOUT_SEC seconds).
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
 *        Zeroing first is mandatory, not tidiness: rte_hash compares the raw
 *        key bytes, padding included, so a key built field by field over stack
 *        garbage would hash differently in each direction.
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
 * @brief Build the session key of one IPv6 packet. An inbound packet takes its
 *        destination as the LAN side, an outbound one its source, so both
 *        directions of a flow land on the same key.
 *
 *        Trackable protocols are TCP, UDP and the ICMPv6 echo pair, the latter
 *        keyed by its identifier in place of a port. Extension headers are not
 *        walked, so a packet carrying one has no readable L4 tuple here.
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
        /* Echo is the only tracked ICMPv6 exchange, and only in its natural
         * direction: the request leaves the LAN, the reply comes back. */
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
 * @brief Point a conntrack view at an IPv6 firewall session. The deadline the
 *        state machine writes lives in the subscriber's SoA array, not in the
 *        entry, so the slot index is needed alongside the entry.
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
 * @brief RCU defer-queue callback the hash runs once every data-plane reader
 *        has passed a quiescent state after a key deletion: only then can no
 *        reader still hold the entry, so only then may the slot be recycled.
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

    /* Both SoA stamps go to zero first, which is what marks the slot free to
     * the GC and the LRU scan; neither then touches the entry cache line. */
    __atomic_store_n(&ppp_ccb->ipv6_firewall_expire_at[idx], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ppp_ccb->ipv6_firewall_last_used[idx], 0, __ATOMIC_RELAXED);
    rte_atomic16_set(&ppp_ccb->ipv6_firewall_table[idx].is_fill, IPV6_FIREWALL_ENTRY_FREE);
    rte_ring_enqueue(ppp_ccb->ipv6_firewall_free_ring, (void *)(uintptr_t)idx);
}

/**
 * @fn ipv6_firewall_table_reset
 *
 * @brief Flush every session of a subscriber: empty the hash and refill the
 *        free-list with all pool indices. Control-plane only (subscriber
 *        re-init), must not race data-plane traffic for this subscriber —
 *        callers close the IPv6 forwarding gate first.
 *
 * @param ppp_ccb
 *        Subscriber control block
 */
static inline void ipv6_firewall_table_reset(ppp_ccb_t *ppp_ccb)
{
    unsigned int freed, pending, avail;

    /* Drain the defer queue before rebuilding: a deferred free surviving the
     * reset would fire later and push its (now re-issued) slot index into the
     * refilled free ring, handing one entry to two sessions.
     * Terminates: data lcores report quiescent every poll loop even when
     * idle, and at first init the queue is simply empty. */
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
 * @brief Create-once (find-existing on re-create, mirroring the NAT naming
 *        rules) the per-subscriber session hash and free-list ring, attach the
 *        shared QSBR RCU for deferred reclaim, and fill the free-list.
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
 * @brief Amortized garbage collection: scan up to max_slots pool slots from
 *        the per-subscriber cursor and unlink every expired session. Slot
 *        indices flow back to the free-list through the RCU defer-queue
 *        callback once readers are quiescent. Safe from any lcore — the hash
 *        is multi-writer and a duplicate delete just returns ENOENT.
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

        /* The hot loop reads only the SoA deadline array — 8 slots per cache
         * line, sequential, prefetcher-friendly; 0 = free slot. Entry lines
         * are touched only for actual expired hits. */
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

    /* Deleting a few keys does not fill the defer queue, so its automatic
     * reclaim threshold may never trigger. Drain it on every GC tick so freed
     * slots actually reach the free ring. */
    rte_hash_rcu_qsbr_dq_reclaim(ppp_ccb->ipv6_firewall_hash, &freed, &pending, &available);
    return reclaimed;
}

/**
 * @fn ipv6_firewall_evict_lru
 *
 * @brief Unlink the session that has gone unused the longest, so a new one can
 *        take its slot. Only the insert path calls this, under the insert
 *        lock, so a subscriber never has two evictors at once.
 *
 *        The scan reads the recency array alone, 512 KB sequential, and picks
 *        the smallest non-zero stamp. Stamps keep moving while it runs, so the
 *        victim is the oldest as of the moment it was read — that only decides
 *        which session is dropped, never memory safety: the slot still comes
 *        back through the RCU defer queue like any other deletion. Losing the
 *        key to a concurrent GC delete is equally harmless, the caller simply
 *        finds no free slot this time.
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
 * @brief Pop a free pool slot. When the free-list is empty, reclaim expired
 *        sessions first — those cost a live connection nothing — and only then
 *        evict the least recently used live one, so a new connection can
 *        always be opened.
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
 * @brief Find a session by key, without judging its age. Lock-free; the
 *        acquire fence pairs with the release fence the insert publishes the
 *        entry with, so a hit may read the entry fields right away.
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
 *        miss and is unlinked on the spot. The WAN side must never revive one,
 *        or a single early packet would hold the door open indefinitely.
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
 * @brief Record traffic on a session: push its idle deadline out and stamp the
 *        LRU recency. Both writes coalesce to at most one per session per
 *        second, so the arrays every data lcore shares stay in the shared
 *        cache state under per-packet load. One second is plenty of resolution
 *        for "used just now" versus "idle for minutes".
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
 * @brief Open a new session. Takes the per-subscriber insert lock and
 *        re-checks the hash first, because rte_hash_add_key_data silently
 *        replaces an existing key and would orphan the slot behind it.
 *
 *        The entry is filled, its deadline and recency stamped, and only then
 *        published: the release fence orders all of that ahead of the READY
 *        flag and the hash insertion that expose it to other lcores.
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
 * @brief Open or renew the session an outbound packet belongs to, so its reply
 *        is allowed back in. Called for every LAN to WAN packet the classifier
 *        decided to forward.
 *
 *        The LAN side is trusted, so an expired session is revived rather than
 *        treated as a miss. A packet with no trackable tuple (unknown
 *        protocol, extension header, truncated L4) is still forwarded, it just
 *        gets no session — its reply will be dropped.
 *
 *        When no slot is available the packet also goes out without a session.
 *        The next outbound packet of the flow (a TCP retransmit, the next UDP
 *        datagram) finds the reclaimed slot and opens it, so a connection
 *        costs at most one retry rather than failing.
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
    /* TCP needs its whole header here: conntrack reads the data offset, the
     * flags, the sequence, the ack and the window. */
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

        /* LAN to WAN is trusted: no sequence validation, just run the state
         * machine and move the LAN-side baseline so the reply can be checked
         * against it. A fresh session starts at NONE, so a SYN opens
         * SYN_SENT and any other first packet enters the MID_STREAM
         * probation the IPv4 path already uses for flows picked up mid-life. */
        tcp_conntrack_fsm_view(&view, tcp->tcp_flags, FALSE);
        tcp_conntrack_seq_update_view(&view, tcp, tcp_payload, FALSE);
    }
}

/**
 * @fn ipv6_firewall_tcp_inbound_pass
 *
 * @brief Stateful inspection of an inbound TCP packet that already matched a
 *        live session. The state and sequence checks always run so tracking
 *        stays current; tcp_conntrack_enabled only decides whether a packet
 *        failing them is dropped or forwarded unenforced. This is the same
 *        split the IPv4 path uses, governed by the same per-subscriber switch.
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
 *        These messages report what happened to a packet a LAN host sent, so
 *        their outer source is a router on the path and can never match a
 *        session. What must match is the packet quoted inside: its source has
 *        to be the host being notified, and its tuple has to name a session
 *        that is still alive. Anything else is forged and gets dropped.
 *
 *        Letting one through must not refresh the session — an error report is
 *        not traffic and extends nobody's lifetime. Every quoted byte is
 *        attacker controlled and is only ever read.
 *
 *        Dropping these outright would be the simpler rule and a worse one:
 *        Packet Too Big is how a host learns to shrink its packets, and
 *        without it small packets flow while large ones vanish.
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

    /* The error header, the quoted IPv6 header and the first 8 bytes of the
     * quoted L4 header are the least a sender is required to include. This is
     * also the only length check on this path: anything shorter, down to a
     * message that does not even hold a full ICMPv6 header, is rejected and
     * counted here. */
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
 *        forwarded. Default deny: only what answers something a LAN host
 *        started gets through.
 *
 *        The decision order is fixed:
 *          1. ICMPv6 error messages, validated against the packet they quote
 *          2. an open session (TCP additionally passes stateful inspection)
 *          3. user-defined rules
 *          4. drop
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

    /* One byte is all it takes to tell an ICMPv6 error message apart, and the
     * length guard has to come first or that byte may not be there to read.
     * Whether the rest of the message is long enough to check is decided
     * inside, so a truncated error message is counted as a rejected error
     * rather than disappearing into the anonymous drop below. */
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
