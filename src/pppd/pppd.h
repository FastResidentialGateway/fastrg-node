/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  PPPD.H

     For ppp detection

  Designed by THE on Jan 14, 2019
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _PPPD_H_
#define _PPPD_H_

#include <assert.h>
#include <stdatomic.h>
#include <netinet/in.h>

#include <common.h>

#include <rte_timer.h>
#include <rte_memory.h>
#include <rte_ether.h>
#include <rte_rcu_qsbr.h>
#include <rte_spinlock.h>

#include "header.h"
#include "../fastrg.h"
#include "../init.h"
#include "../mac_table.h"

struct nd6_table;

#define PPP_MSG_BUF_LEN	        128

#define MULTICAST_TAG           4001
#define TOTAL_SOCK_PORT	        65536
/* Parenthesized: the unparenthesized form made "hash % MAX_NAT_ENTRIES" parse
 * as "(hash % 65536) << 2" (% binds tighter than <<), so every computed index
 * was a multiple of 4 and hashing quality suffered 4x clustering. */
#define MAX_NAT_ENTRIES         (TOTAL_SOCK_PORT << 2)
#define PORT_FWD_TABLE_SIZE     TOTAL_SOCK_PORT  /* direct-indexed by eport (0..65535) */
/* IPv6 firewall sessions kept per subscriber.  Fully preallocated like every
 * other subscriber resource, so the data plane never allocates. */
#define IPV6_FIREWALL_MAX_ENTRIES     TOTAL_SOCK_PORT

#define PPPoE_CMD_DISABLE       0
#define PPPoE_CMD_FORCE_DISABLE 1
#define PPPoE_CMD_ENABLE        2

/* Buffer sizes for pppd_ipv6_report_strings(). */
#define PPPD_IPV6_ADDR_STRLEN   INET6_ADDRSTRLEN            /* "fe80::1"          */
#define PPPD_IPV6_PREFIX_STRLEN (INET6_ADDRSTRLEN + 4)      /* address + "/128"   */
#define PPPD_IPV6_DNS_STRLEN    (2 * INET6_ADDRSTRLEN + 2)  /* two servers + ','  */

/**
 * @brief SNAT port forwarding entry (direct-indexed by eport)
 *
 * The array index IS the external port number, so no eport field needed.
 * Maps WAN PPPoE IP:eport → LAN dip:iport.
 * Packed to 16 bytes so total table = 65536 * 16 = 1 MB per ppp_ccb.
 *
 * Do not make this struct cache line aligned. Each entry is parallel accessed 
 * in different lcores, even we align it to cache line, it still has false sharing
 * issue. Therefore, we keep it unaligned to save memory usage. 
 */
typedef struct port_fwd_entry {
    U32            dip;         /**< destination IP on LAN (network byte order) */
    U16            iport;       /**< internal port on LAN (network byte order) */
    rte_atomic16_t is_active;   /**< 1 = active, 0 = free */
    rte_atomic64_t hit_count;   /**< number of packets matched by this rule */
} port_fwd_entry_t;
/**
 * @brief hsi nat table structure
 */
typedef struct addr_table {
    struct rte_ether_addr mac_addr;
    U32                   src_ip; // original src ip from LAN user (e.g. 192.168.0.100), network order
    U32                   dst_ip; // dst ip where LAN user wants to visit (e.g. public ip), network order
    U16                   src_port; // original src port from LAN user, network order
    U16                   dst_port; // dst port where LAN user wants to visit, network order(no order for ICMP type field)
    U16                   nat_port; // translated port, network order
    U8                    proto;     // IPPROTO_TCP / IPPROTO_UDP / IPPROTO_ICMP of the learned flow
    U8                    tcp_state; // TCP conntrack state (tcp_conntrack_state_t), 0 = NONE
    U8                    tcp_fin_flags; // bitmask: bit0 = LAN FIN, bit1 = WAN FIN
    rte_atomic16_t        is_fill;   // is this entry filled or not
    U64                   *expire_slot; // -> ppp_ccb nat_expire_at[own idx]; bound at init, read-only after.
                                        // The structure-of-arrays (SoA) split keeps the GC scan off entry cache
                                        // lines; the back-pointer lets tcp_conntrack handlers reach the slot
                                        // without knowing ppp_ccb.
    /* TCP seq/ack window tracking (host order).  Used by tcp_conntrack_seq_valid
     * to drop blind injection from WAN side; LAN→WAN only updates these fields. */
    U32                   max_seq_end_lan; // highest (seq + payload + SYN/FIN) seen from LAN
    U32                   max_seq_end_wan; // same from WAN
    U32                   max_ack_lan;     // highest ack from LAN
    U32                   max_ack_wan;     // same from WAN
    U16                   max_win_lan;     // last advertised window from LAN (no scaling)
    U16                   max_win_wan;     // same from WAN
}__rte_cache_aligned addr_table_t;

/**
 * @brief IPv6 firewall session key, always written LAN side first so that the
 *        outbound packet (LAN = source) and its reply (LAN = destination)
 *        produce the very same 40 bytes and share one hash entry.
 *
 *        rte_hash compares raw key bytes, padding included: every builder must
 *        zero the whole struct before filling it, or the two directions hash
 *        differently and replies never match.
 */
typedef struct ipv6_firewall_key {
    U8  lan_addr[16];    /* subscriber-side address */
    U8  remote_addr[16]; /* internet-side address */
    U16 lan_port;        /* TCP/UDP: LAN port; ICMPv6 echo: identifier */
    U16 remote_port;     /* TCP/UDP: remote port; ICMPv6 echo: 0 */
    U8  proto;           /* IPPROTO_TCP / IPPROTO_UDP / IPPROTO_ICMPV6 */
    U8  pad[3];          /* explicitly zeroed, part of the compared bytes */
} ipv6_firewall_key_t;

/**
 * @brief One IPv6 firewall session.  Exactly one cache line, so a lookup that
 *        hits touches a single line.
 *
 *        The expiry deadline and the LRU recency stamp live in the ppp_ccb
 *        structure-of-arrays instead of here: the GC and the eviction scan walk
 *        those arrays alone and never pull entry lines into cache.
 */
typedef struct ipv6_firewall_entry {
    ipv6_firewall_key_t  key;             /* also the delete key on GC / eviction */
    rte_atomic16_t is_fill;         /* IPV6_FIREWALL_ENTRY_FREE / _READY */
    U8             tcp_state;       /* tcp_conntrack_state_t; NONE for non-TCP */
    U8             tcp_fin_flags;   /* TCP_FIN_FLAG_LAN / _WAN bitmask */
    /* TCP seq/ack window tracking (host order), same meaning as the equally
     * named addr_table_t fields: the baseline tcp_conntrack_seq_valid checks
     * inbound packets against. */
    U32            max_seq_end_lan; /* highest (seq + payload + SYN/FIN) from LAN */
    U32            max_seq_end_wan; /* same from WAN */
    U32            max_ack_lan;     /* highest ack from LAN */
    U32            max_ack_wan;     /* same from WAN */
    U16            max_win_lan;     /* last advertised window from LAN (no scaling) */
    U16            max_win_wan;     /* same from WAN */
}__rte_cache_aligned ipv6_firewall_entry_t;

static_assert(sizeof(ipv6_firewall_entry_t) == RTE_CACHE_LINE_SIZE,
    "ipv6_firewall_entry_t must stay one cache line");

/* Coalescing threshold for expire refreshes.  NAT/conntrack timeouts are
 * seconds-granular, so a refresh that would move the deadline by less than
 * this is skipped — per-packet writes collapse to one write per flow per
 * second and the shared expire cache lines stay in MESI Shared state. */
#define NAT_EXPIRE_COALESCE_SEC 1

/**
 * @fn nat_expire_set
 *
 * @brief Unconditionally (re)arm an entry's expiry deadline.  For state
 *        transitions and fresh entries — a shortened deadline (e.g. TCP
 *        ESTABLISHED -> FIN_WAIT) must take effect immediately.
 *
 * @param slot
 *        Entry's expire slot (addr_table_t.expire_slot)
 * @param target
 *        Absolute TSC deadline
 */
static inline void nat_expire_set(U64 *slot, U64 target)
{
    __atomic_store_n(slot, target, __ATOMIC_RELAXED);
}

/**
 * @fn nat_expire_refresh
 *
 * @brief Extend an entry's expiry deadline with write coalescing: only
 *        store when the new deadline is more than NAT_EXPIRE_COALESCE_SEC
 *        ahead of the current one.  Never shortens — same-state refreshes
 *        only ever push the deadline out, and a longer stored deadline
 *        (e.g. TCP ESTABLISHED 7200s vs the NAT-level 10s refresh) simply
 *        wins.
 *
 * @param slot
 *        Entry's expire slot (addr_table_t.expire_slot)
 * @param target
 *        Absolute TSC deadline to extend to
 */
static inline void nat_expire_refresh(U64 *slot, U64 target)
{
    U64 cur = __atomic_load_n(slot, __ATOMIC_RELAXED);

    if (target > cur + (U64)NAT_EXPIRE_COALESCE_SEC * fastrg_get_cycles_in_sec())
        __atomic_store_n(slot, target, __ATOMIC_RELAXED);
}

/**
 * @brief hsi control block structure
 */
typedef struct {
    FastRG_t              *fastrg_ccb;       /* pointer to fastrg control block */
    U16	                  user_num;          /* subscriptor id */
    rte_atomic16_t        vlan_id;           /* subscriptor vlan */
    struct rte_ether_hdr  eth_hdr;
    vlan_header_t         vlan_header __rte_aligned(sizeof(vlan_header_t));
    pppoe_header_t        pppoe_header __rte_aligned(sizeof(vlan_header_t));
    control_protocol_t    control_protocol[PPP_CP_COUNT]; /* per-control-protocol negotiation automata;
                                                           * connection stage is stored in phase */
    pppoe_phase_t         pppoe_phase;       /* store pppoe info */
    U8                    cp_id;             /* current control protocol: PPP_CP_* */
    U8                    phase;             /* pppoe connection phase */
    U16                   session_id;        /* pppoe session id */
    struct rte_ether_addr PPP_dst_mac;       /* pppoe server mac addr */
    U32                   hsi_ipv4;          /* ip addr pppoe server assign to pppoe client */
    U32                   hsi_ipv4_gw;       /* ip addr gateway pppoe server assign to pppoe client */
    U32                   hsi_primary_dns;   /* 1st dns addr pppoe server assign to pppoe client */
    U32                   hsi_secondary_dns; /* 2nd dns addr pppoe server assign to pppoe client */
    U8                    identifier[PPP_CP_COUNT]; /* per-CP Configure-Request id; auth frames reuse LCP [0] */
    BOOL                  config_request_pending[PPP_CP_COUNT]; /* outstanding per-CP Configure-Request */
    U32                   magic_num;         /* ppp pkt magic number, in network order */
    U16                   mru;               /* MRU we propose in LCP Configure-Request, host order; 0 = default */
    BOOL                  lcp_auth_rejected; /* peer Configure-Rejected our authentication-protocol option */
    BOOL                  lcp_mru_rejected;  /* peer Configure-Rejected our MRU option */
    BOOL                  lcp_magic_rejected; /* peer Configure-Rejected our magic-number option */
    BOOL                  peer_requires_auth; /* peer's Configure-Request carried an AUTH option (we must authenticate) */
    U16                   auth_method;       /* use chap or pap */
    U8                    *ppp_user_acc;     /* pap/chap account (NUL-terminated) */
    U8                    *ppp_passwd;       /* pap/chap password (NUL-terminated) */
    rte_spinlock_t        cred_lock;         /* lock for updating credentials, ppp_user_acc and ppp_passwd */
    U32	                  ppp_interval;      /* LCP keepalive echo interval, seconds */
    U32                   echo_miss_count;   /* consecutive unanswered LCP echo-requests; reset on any frame from peer */
    rte_atomic16_t        ppp_bool;          /* boolean flag for accept ppp packets at data plane */
    /* HSI data-plane gate. Single writer: the control plane. Storing 1 is a
     * publish — it hands the data plane the session fields written before it
     * (session_id, PPP_dst_mac, hsi_ipv4), so every store of 1 is preceded by
     * rte_smp_wmb() and every data-plane read goes through
     * pppd_dp_gate_open(), which supplies the paired read barrier.
     * Storing 0 deliberately carries no barrier: ccbs are preallocated and
     * never freed, so a reader that races the close only encapsulates a
     * handful of packets from stale (or already cleared) fields, which the
     * upstream drops. That race window exists regardless of ordering. */
    rte_atomic16_t        dp_start_bool;
    rte_atomic16_t        redial_pending;    /* desire=connect arrived mid-teardown; redial once down */
    BOOL                  ppp_processing;    /* boolean flag for checking ppp is disconnecting */
    addr_table_t          addr_table[MAX_NAT_ENTRIES]; /* hsi nat entry pool (slots referenced by both nat hashes) */
    U64                   nat_expire_at[MAX_NAT_ENTRIES]; /* structure-of-arrays (SoA) expiry deadline time, parallel to addr_table (8/cache line
                                                           * so the GC scan walks 8x denser than entry lines); 0 = slot free */
    U64                   nat_enospc;        /* learning failures: ports exhausted / pool dry / hash full (RELAXED add) */
    U64                   nat_gc_reclaimed;  /* entries reclaimed by GC scans (RELAXED add) */
    struct rte_hash       *nat_reverse_hash;  /* (nat_port,dst_ip,dst_port) → addr_table slot idx (WAN→LAN);
                                               * owns slot reclaim via its RCU dq callback */
    struct rte_hash       *nat_forward_hash;  /* 5-tuple → addr_table slot idx (LAN→WAN established-flow fast path) */
    struct rte_ring       *nat_free_ring;    /* free-list of addr_table slot indices (MPMC) */
    U32                   nat_gc_counter;    /* amortized expired-slot scan position (approximate, racy by design) */
    rte_spinlock_t        nat_insert_lock;   /* serializes miss-path inserts only (double-checked); per-packet
                                              * same-flow refresh stays lock-free via lock free hash lookup */
    port_fwd_entry_t      port_fwd_table[PORT_FWD_TABLE_SIZE]; /* SNAT port forwarding, direct-indexed by eport */
    mac_table_t           *mac_table;        /* per-subscriber LAN host MAC table (fixed 64K-entry lock free hash) */
    arp_pending_queue_t   arp_pq;            /* ARP pending queue for unresolved port-fwd destinations */
    struct rte_timer      pppoe;             /* pppoe timer */
    struct rte_timer      ppp;               /* ppp timer */
    struct rte_timer      ppp_alive;         /* PPP connection checking timer */
    /* PPPoE session counters are per-lcore now: FastRG_t.pppoes_stats[lcore][ccb_id]. */
    /* Per-subscriber TCP conntrack (SPI) enable. Written by control plane
     * (apply_hsi_config / SetTcpConntrack), read on every inbound TCP packet
     * by data-plane cores. 1-byte aligned store/load is atomic on x86 (TSO);
     * volatile blocks the compiler from hoisting/caching the load. */
    volatile BOOL         tcp_conntrack_enabled;
    /* Per-subscriber IPv6 enable. Written by apply_hsi_config(), then read by
     * the PPP control plane when IPV6CP negotiation is implemented. The
     * 1-byte aligned store/load follows the same atomicity rule as conntrack. */
    volatile BOOL         ipv6_enabled;
    /* ---- IPv6 stateful firewall (per subscriber, mirrors the NAT block) ---- */
    ipv6_firewall_entry_t       ipv6_firewall_table[IPV6_FIREWALL_MAX_ENTRIES]; /* session pool (slots referenced by the hash) */
    U64                   ipv6_firewall_expire_at[IPV6_FIREWALL_MAX_ENTRIES];  /* SoA expiry deadline, parallel to the pool; 0 = slot free */
    U64                   ipv6_firewall_last_used[IPV6_FIREWALL_MAX_ENTRIES];  /* SoA last-hit stamp for LRU eviction; 0 = slot free.
                                                                    * Separate from the deadline because per-state TCP timeouts
                                                                    * make "expires first" and "idle longest" different sessions */
    U64                   ipv6_firewall_enospc;          /* sessions not created: pool dry or hash full (RELAXED add) */
    U64                   ipv6_firewall_gc_reclaimed;    /* sessions reclaimed by GC scans (RELAXED add) */
    U64                   ipv6_firewall_evicted;         /* sessions dropped by LRU to make room (RELAXED add) */
    U64                   ipv6_firewall_icmp6_err_passed;  /* inbound ICMPv6 errors matching a live session (RELAXED add) */
    U64                   ipv6_firewall_icmp6_err_dropped; /* inbound ICMPv6 errors matching nothing (RELAXED add) */
    struct rte_hash       *ipv6_firewall_hash;      /* ipv6_firewall_key_t -> pool slot idx, both directions;
                                               * owns slot reclaim via its RCU dq callback */
    struct rte_ring       *ipv6_firewall_free_ring; /* free-list of pool slot indices (MPMC) */
    U32                   ipv6_firewall_gc_counter; /* amortized expired-slot scan position (approximate, racy by design) */
    rte_spinlock_t        ipv6_firewall_insert_lock; /* serializes miss-path inserts and LRU eviction for this subscriber;
                                                * kept separate from nat_insert_lock so IPv4 and IPv6 never block each other */
    struct rte_timer      ppp_ipv6cp;         /* IPV6CP retransmit timer */
    U8                    ipv6cp_local_iid[8]; /* negotiated local interface identifier */
    U8                    ipv6cp_peer_iid[8];  /* negotiated peer interface identifier */
    /* IPv6 readiness is written by the control plane. The volatile access is
     * reserved for the data-plane reader introduced with IPv6 forwarding. */
    volatile BOOL         ipv6cp_up;
    /* DHCPv6-PD client state is owned by the control plane. Future RA and
     * IPv6 data-plane readers consume the published prefix after ready is set. */
    U8                    dhcp6_state;
    U8                    dhcp6_xid[3];
    U8                    dhcp6_server_duid[130];
    U16                   dhcp6_server_duid_len;
    U32                   dhcp6_t1;
    U8                    dhcp6_retry;
    struct rte_timer      dhcp6_timer;
    U8                    hsi_ipv6_pd_prefix[16];
    U8                    hsi_ipv6_pd_plen;
    U8                    hsi_ipv6_lan_prefix[16];
    U8                    hsi_ipv6_dns[2][16];
    volatile BOOL         dhcp6_pd_ready;
    /* Data-plane IPv6 forwarding gate: the AND of ipv6_enabled, ipv6cp_up and
     * dhcp6_pd_ready, recomputed by the control plane (its only writer) via
     * pppd_ipv6_dp_gate_update(). Kept independent of dp_start_bool so that an
     * IPCP failure never stops IPv6 forwarding and an IPV6CP or prefix
     * delegation failure never stops IPv4 forwarding. */
    rte_atomic16_t        ipv6_dp_bool;
    /* Cycle stamp of the last WAN->LAN neighbor-cache miss handed to the
     * control plane. Data lcores claim a new stamp with a relaxed
     * compare-exchange before escalating, so traffic to an unresolved LAN
     * address cannot flood the control-plane ring. */
    U64                   nd6_miss_last_cycles;
    struct nd6_table      *nd6_table;       /* single-writer IPv6 neighbor cache */
    struct rte_timer      ra_timer;         /* periodic LAN router advertisement for IPv6 */
    U64                   last_rs_ra_cycles; /* last RS-triggered RA, for rate limiting in IPv6 */
}__rte_cache_aligned ppp_ccb_t;

/**
 * @fn pppd_dp_gate_open
 *
 * @brief Data-plane read side of the dp_start_bool gate: report whether the
 *        subscriber's data path is open, and when it is, order this read
 *        ahead of the session fields the gate publishes.
 *
 * @param ppp_ccb subscriber control block
 *
 * @return TRUE when the gate is open and the published session fields are
 *         safe to read, FALSE when the packet must be dropped
 */
static __always_inline BOOL pppd_dp_gate_open(const ppp_ccb_t *ppp_ccb)
{
    if (rte_atomic16_read(&ppp_ccb->dp_start_bool) == (S16)0)
        return FALSE;
    /* Pairs with the rte_smp_wmb() that precedes every store of 1 to
     * dp_start_bool: seeing the gate open must also mean seeing the
     * session_id, PPP_dst_mac and hsi_ipv4 written before it was opened. */
    rte_smp_rmb();
    return TRUE;
}

static inline struct rte_timer *ppp_cp_timer(ppp_ccb_t *ppp_ccb)
{
    return ppp_ccb->cp_id == PPP_CP_IPV6CP ? &ppp_ccb->ppp_ipv6cp : &ppp_ccb->ppp;
}

/**
 * @fn pppd_ipv6_dp_gate_update
 *
 * @brief Recompute a subscriber's IPv6 data-plane gate from ipv6_enabled,
 *        ipv6cp_up and dhcp6_pd_ready. Call it from the control plane after
 *        every write to any of those three flags.
 *
 *        Opening the gate publishes a write barrier first, so a data lcore
 *        that observes the gate open also observes the LAN prefix, session id,
 *        peer MAC and VLAN written before the call. Closing needs no barrier:
 *        a packet already in flight reads consistent-but-stale fields, and the
 *        control block itself is preallocated and never freed.
 *
 * @param ppp_ccb
 *      Subscriber control block (NULL tolerated)
 * @return
 *      void
 */
void pppd_ipv6_dp_gate_update(ppp_ccb_t *ppp_ccb);

/**
 * @fn pppd_ipv6_report_strings
 *
 * @brief Format a subscriber's IPv6 session state for northbound reporting
 *        (Kafka events, gRPC, CLI). Every output buffer is written, and one
 *        that has nothing to report is left as an empty string.
 *
 *        Formatting only — the caller decides whether the fields are ready to
 *        be read (control plane: the three IPv6 flags in program order; other
 *        threads: pppd_ipv6_dp_gate_open()).
 *
 * @param ppp_ccb
 *      Subscriber control block (NULL tolerated)
 * @param addr_str
 *      Receives the WAN link-local address built from the IPV6CP interface
 *      identifier, e.g. "fe80::1"
 * @param addr_len
 *      Size of addr_str, at least PPPD_IPV6_ADDR_STRLEN
 * @param prefix_str
 *      Receives the whole delegated prefix in CIDR form, e.g.
 *      "2001:db8:ab00::/56"
 * @param prefix_len
 *      Size of prefix_str, at least PPPD_IPV6_PREFIX_STRLEN
 * @param dns_str
 *      Receives the DNS servers separated by ',' without spaces; unused
 *      server slots are skipped
 * @param dns_len
 *      Size of dns_str, at least PPPD_IPV6_DNS_STRLEN
 * @return
 *      void
 */
void pppd_ipv6_report_strings(const ppp_ccb_t *ppp_ccb, char *addr_str,
    U32 addr_len, char *prefix_str, U32 prefix_len, char *dns_str,
    U32 dns_len);

/**
 * @fn pppd_ipv6_dp_gate_open
 *
 * @brief Data-plane side of the IPv6 gate: report whether IPv6 forwarding is
 *        open for this subscriber and, when it is, order the read against the
 *        subscriber fields the control plane published before opening it.
 *
 * @param ppp_ccb
 *      Subscriber control block
 * @return
 *      TRUE when IPv6 forwarding fields may be read, FALSE otherwise
 */
static __always_inline BOOL pppd_ipv6_dp_gate_open(const ppp_ccb_t *ppp_ccb)
{
    if (rte_atomic16_read(&ppp_ccb->ipv6_dp_bool) == (S16)0)
        return FALSE;
    /* Pairs with the rte_smp_wmb() in pppd_ipv6_dp_gate_update(). */
    rte_smp_rmb();
    return TRUE;
}

typedef enum {
    PPP_REPORT_CONNECTED = 1,
    PPP_REPORT_CONNECTING,
    PPP_REPORT_DISCONNECTED
} ppp_report_phase_t;

/** One subscriber's PPPoE state, ready to be reported northbound. A field with
 *  nothing to report is an empty string. */
typedef struct {
    ppp_report_phase_t phase;
    char user_id[8];
    char ipv4[INET_ADDRSTRLEN];
    char ipv4_gw[INET_ADDRSTRLEN];
    char ipv6_addr[PPPD_IPV6_ADDR_STRLEN];
    char ipv6_pd_prefix[PPPD_IPV6_PREFIX_STRLEN];
    char ipv6_dns[PPPD_IPV6_DNS_STRLEN];
} ppp_state_report_t;

/**
 * @fn ppp_build_state_report
 *
 * @brief fill a state report from the subscriber's current control block: the
 *      PPPoE phase, and for a session carrying data the assigned IPv4 address
 *      and gateway plus the IPv6 address, delegated prefix and DNS servers.
 *      Reads the IPv6 fields only while the IPv6 is enabled.
 *
 * @param ppp_ccb
 *      PPP control block pointer
 * @param report
 *      receives the report; every field is written
 *
 * @return
 *      void
 */
void ppp_build_state_report(const ppp_ccb_t *ppp_ccb, ppp_state_report_t *report);

/**
 * @fn ppp_report_connection_status
 *
 * @brief report the subscriber's current PPPoE state to the controller over
 *      Kafka: build the report from the control block, then send it. The
 *      controller overwrites its whole row per event, so the event always
 *      carries the complete state rather than a partial one.
 *
 *      Call it from the control plane whenever the reportable state changes,
 *      and from the northbound path to restate what the subscriber looks like
 *      right now.
 *
 * @param ppp_ccb
 *      PPP control block pointer
 *
 * @return
 *      SUCCESS when the event was handed to the Kafka producer, ERROR in
 *      standalone mode (no controller to report to) or on a bad argument
 */
STATUS ppp_report_connection_status(ppp_ccb_t *ppp_ccb);

/**
 * @fn ppp_report_all_connection_status
 *
 * @brief report every configured subscriber's current PPPoE state to the
 *      controller, one event each.
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 *
 * @return
 *      how many events were handed to the Kafka producer; 0 in standalone
 *      mode or on a bad argument
 */
U32 ppp_report_all_connection_status(FastRG_t *fastrg_ccb);

void   exit_ppp(ppp_ccb_t *ppp_ccb);

/**
 * @fn ppp_process
 * 
 * @brief PPPoE / PPP protocol processing
 * 
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param pkt_data
 *      Pointer to the PPPoE / PPP packet data
 * @param len
 *      Length of the packet data
 * 
 * @return SUCCESS if process successfully, ERROR if process failed
 */
STATUS ppp_process(FastRG_t *fastrg_ccb, U8 *pkt_data, U16 len);

STATUS ppp_connect(ppp_ccb_t *ppp_ccb);
STATUS ppp_disconnect(ppp_ccb_t *ppp_ccb);
STATUS ppp_update_config_by_user(ppp_ccb_t *ppp_ccb, U16 vlan_id, const char *user_name, 
    const char *password);
STATUS ppp_init_config_by_user(FastRG_t *fastrg_ccb, ppp_ccb_t *ppp_ccb, U16 ccb_id, 
    U16 vlan_id, const char *user_name, const char *password);
void   ppp_cleanup_config_by_user(ppp_ccb_t *ppp_ccb, U16 ccb_id);
void   PPP_bye_timer_cb(__attribute__((unused)) struct rte_timer *tim,
    ppp_ccb_t *ppp_ccb);
/* Periodic LCP keepalive: probes the peer with an Echo-Request each tick and
 * tears the session down once LCP_ECHO_MAX_FAIL probes go unanswered. */
void   PPP_keepalive_cb(__attribute__((unused)) struct rte_timer *tim,
    ppp_ccb_t *ppp_ccb);

/**
 * @fn pppd_init
 * 
 * @brief PPPoE / PPP protocol initialization function
 * 
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @return
 *      SUCCESS if init successfully, ERROR if init failed
 */
STATUS pppd_init(FastRG_t *fastrg_ccb);

/**
 * @fn PPP_bye
 * 
 * @brief PPPoE / PPP connection closing processing function
 * 
 * @param ppp_ccb
 *      PPP control block pointer
 * @return
 *      void
 */
void PPP_bye(ppp_ccb_t *ppp_ccb);

/**
 * @fn pppd_disable_ccb
 *
 * @brief Disable ppp control blocks, reserve memory region for future use
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param remove_ccb_count
 *      Number of ccbs to disable
 * @param old_ccb_count
 *      Old number of ccbs before disable
 * @return
 *      SUCCESS if disabled successfully, ERROR if failed
 */
STATUS pppd_disable_ccb(FastRG_t *fastrg_ccb, U16 remove_ccb_count, U16 old_ccb_count);

/**
 * @fn pppd_cleanup_ccb
 *
 * @brief Cleanup all ppp control blocks
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 */
void pppd_cleanup_ccb(FastRG_t *fastrg_ccb);

/**
 * @fn PPPD_GET_CCB
 *
 * @brief
 *      Get ppp control block by ccb id
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param ccb_id
 *      CCB ID
 * @return
 *      ppp_ccb_t *
 */
#define PPPD_GET_CCB(fastrg_ccb_ptr, ccb_id) \
    ((ppp_ccb_t *)(fastrg_ccb_ptr)->ppp_ccb[(ccb_id)])

/**
 * @fn pppd_get_subscriber_real_size
 *
 * @brief Calculate per ccb memory usage, store per mempool size and directly 
 *        allocated memory size info in out
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @param out
 *      [out] Size info, filled only on success
 * @return
 *      SUCCESS when the measurement completed and the heap was restored exactly
 */
STATUS pppd_get_subscriber_real_size(FastRG_t *fastrg_ccb, ccb_memory_info_t *out);

#endif
