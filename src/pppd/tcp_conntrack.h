/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  TCP_CONNTRACK.H

     TCP Connection Tracking State Machine for SNAT
     Table-driven FSM modeled

  Designed by THE on Apr 15, 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _TCP_CONNTRACK_H_
#define _TCP_CONNTRACK_H_

#include <common.h>

#include <rte_tcp.h>
#include <rte_byteorder.h>

#include "pppd.h"

/*--------- TCP CONNTRACK STATE TYPE ----------*/
typedef enum {
    TCP_CONNTRACK_NONE,          /* 0 - Non-TCP or untracked (backward compat) */
    TCP_CONNTRACK_SYN_SENT,      /* SYN sent, awaiting SYN-ACK */
    TCP_CONNTRACK_SYN_RECV,      /* SYN-ACK seen, awaiting final ACK */
    TCP_CONNTRACK_ESTABLISHED,   /* Three-way handshake complete */
    TCP_CONNTRACK_FIN_WAIT,      /* First FIN seen */
    TCP_CONNTRACK_CLOSE_WAIT,    /* FIN received from remote, waiting local close */
    TCP_CONNTRACK_LAST_ACK,      /* FIN sent after CLOSE_WAIT */
    TCP_CONNTRACK_TIME_WAIT,     /* Both FINs acked, waiting 2*MSL */
    TCP_CONNTRACK_CLOSE,         /* RST received or fully closed */
    TCP_CONNTRACK_MID_STREAM,    /* Loose mid-stream pickup; promotes to ESTABLISHED on WAN-side ACK */
    TCP_CONNTRACK_INVLD,         /* Sentinel (table terminator) */
} tcp_conntrack_state_t;

/*--------- TCP CONNTRACK EVENT TYPE ----------*/
typedef enum {
    TCP_EV_SYN,       /* SYN only (no ACK) */
    TCP_EV_SYN_ACK,   /* SYN + ACK */
    TCP_EV_ACK,       /* ACK (data or handshake) */
    TCP_EV_FIN_LAN,   /* FIN from LAN (LAN→WAN) */
    TCP_EV_FIN_WAN,   /* FIN from WAN (WAN→LAN) */
    TCP_EV_RST,       /* RST from either side */
    TCP_EV_INVLD,     /* Sentinel */
} tcp_conntrack_event_t;

/*--------- TIMEOUT VALUES (in seconds, used to compute absolute expiry timestamp) ----------*/
#define TCP_TIMEOUT_NONE         10
#define TCP_TIMEOUT_SYN_SENT     30
#define TCP_TIMEOUT_SYN_RECV     30
#define TCP_TIMEOUT_ESTABLISHED  7200
#define TCP_TIMEOUT_FIN_WAIT     120
#define TCP_TIMEOUT_CLOSE_WAIT   60
#define TCP_TIMEOUT_LAST_ACK     30
#define TCP_TIMEOUT_TIME_WAIT    120
#define TCP_TIMEOUT_CLOSE        10
#define TCP_TIMEOUT_MID_STREAM   60

/*--------- FIN FLAGS BITMASK ----------*/
#define TCP_FIN_FLAG_LAN  0x01  /* FIN seen from LAN (LAN→WAN) */
#define TCP_FIN_FLAG_WAN  0x02  /* FIN seen from WAN (WAN→LAN) */

/*--------- STATE TABLE STRUCTURE ----------*/
typedef struct {
    U8     state;        /* current TCP conntrack state */
    U8     event;        /* TCP flag event (direction-aware for FIN) */
    U8     next_state;   /* state to transition to */
    /* NULL-terminated action handler chain.  is_reply lets a handler take
     * direction-dependent action (e.g. MID_STREAM promotes to ESTABLISHED only
     * when the ACK comes from the WAN side). */
    STATUS (*hdl[4])(struct addr_table *, BOOL is_reply);
} tcp_conntrack_state_tbl_t;

/**
 * @fn tcp_flags_to_event
 *
 * @brief Convert TCP header flags + packet direction to a direction-aware conntrack event.
 *        FIN events are split by direction so the state table can handle
 *        LAN-close (→FIN_WAIT) and WAN-close (→CLOSE_WAIT) without
 *        special-case logic in the FSM body.
 *
 * @param tcp_flags
 *        TCP flags byte from rte_tcp_hdr
 * @param is_reply
 *        TRUE if packet is in reply direction (WAN→LAN)
 *
 * @return tcp_conntrack_event_t
 */
static inline tcp_conntrack_event_t tcp_flags_to_event(U8 tcp_flags, BOOL is_reply)
{
    if (tcp_flags & RTE_TCP_RST_FLAG)
        return TCP_EV_RST;
    if ((tcp_flags & RTE_TCP_SYN_FLAG) && (tcp_flags & RTE_TCP_ACK_FLAG))
        return TCP_EV_SYN_ACK;
    if (tcp_flags & RTE_TCP_SYN_FLAG)
        return TCP_EV_SYN;
    if (tcp_flags & RTE_TCP_FIN_FLAG)
        return is_reply ? TCP_EV_FIN_WAN : TCP_EV_FIN_LAN;
    if (tcp_flags & RTE_TCP_ACK_FLAG)
        return TCP_EV_ACK;
    return TCP_EV_INVLD;
}

/**
 * @fn tcp_conntrack_inbound_valid
 *
 * @brief SPI (Stateful Packet Inspection) guard for inbound (WAN→LAN) TCP packets.
 *        Returns TRUE only if the TCP flags are consistent with the current conntrack
 *        state from the WAN→LAN direction.  Call this before tcp_conntrack_fsm() on
 *        the inbound path; drop the packet if it returns FALSE.
 *
 *        Per-state allowlist:
 *          NONE                  → ACK, FIN (resp), RST  (mid-stream pickup)
 *          MID_STREAM            → ACK, FIN (resp), RST, SYN-ACK
 *          SYN_SENT              → SYN-ACK, RST
 *          SYN_RECV              → SYN-ACK (retransmit), ACK, FIN (resp), RST
 *          ESTABLISHED           → ACK, SYN-ACK (retransmit / port-reuse race),
 *                                  FIN (resp), RST
 *          FIN_WAIT              → ACK, FIN (resp), RST
 *          CLOSE_WAIT, LAST_ACK, TIME_WAIT → ACK, RST
 *          CLOSE, INVLD          → nothing (drop all)
 *
 * @param state     Current tcp_state of the NAT entry
 * @param tcp_flags TCP flags byte from rte_tcp_hdr
 *
 * @return TRUE if the packet is consistent with the tracked state; FALSE to drop
 */
static inline BOOL tcp_conntrack_inbound_valid(U8 state, U8 tcp_flags)
{
    tcp_conntrack_event_t event = tcp_flags_to_event(tcp_flags, TRUE);

    switch ((tcp_conntrack_state_t)state) {
    case TCP_CONNTRACK_NONE:
        /* Mid-stream pickup: NAT entry exists but LAN-side's first packet was not SYN
         * (e.g. pre-existing connections that survived a FastRG restart).  Accept the
         * usual mid-flow flags so the WAN side can pass while the FSM catches up. */
        return (event == TCP_EV_ACK || event == TCP_EV_FIN_WAN || event == TCP_EV_RST);

    case TCP_CONNTRACK_MID_STREAM:
        /* Same allowlist as NONE — we picked up an existing flow on the LAN side.
         * Accept SYN-ACK too in case the WAN side is mid-handshake retransmit. */
        return (event == TCP_EV_ACK || event == TCP_EV_FIN_WAN ||
                event == TCP_EV_RST || event == TCP_EV_SYN_ACK);

    case TCP_CONNTRACK_SYN_SENT:
        return (event == TCP_EV_SYN_ACK || event == TCP_EV_RST);

    case TCP_CONNTRACK_SYN_RECV:
        /* Include SYN-ACK so retransmits from WAN are forwarded to LAN */
        return (event == TCP_EV_SYN_ACK || event == TCP_EV_ACK ||
                event == TCP_EV_FIN_WAN || event == TCP_EV_RST);

    case TCP_CONNTRACK_ESTABLISHED:
        /* Allow SYN-ACK so a delayed retransmit (server still waiting for our final
         * ACK) or a fresh handshake on a reused 4-tuple isn't dropped — the FSM will
         * reset the entry back to SYN_RECV and let the connection complete. */
        return (event == TCP_EV_ACK || event == TCP_EV_FIN_WAN ||
                event == TCP_EV_RST || event == TCP_EV_SYN_ACK);

    case TCP_CONNTRACK_FIN_WAIT:
        return (event == TCP_EV_ACK || event == TCP_EV_FIN_WAN || event == TCP_EV_RST);

    case TCP_CONNTRACK_CLOSE_WAIT:
    case TCP_CONNTRACK_LAST_ACK:
    case TCP_CONNTRACK_TIME_WAIT:
        return (event == TCP_EV_ACK || event == TCP_EV_RST);

    default:
        /* CLOSE, INVLD: connection fully closed; WAN cannot initiate through SNAT,
         * so all inbound is dropped */
        return FALSE;
    }
}

/**
 * @fn tcp_conntrack_seq_valid
 *
 * @brief TCP sequence/ack window check for inbound (WAN→LAN) packets.
 *        Drops blind injection from a WAN attacker who knows the 4-tuple but
 *        not the live seq window.  Only called on the inbound path; LAN→WAN
 *        is trusted and only updates state via tcp_conntrack_seq_update().
 *
 *        SYN packets and zero-baseline (uninitialised) entries always pass —
 *        seeding happens on the first accepted packet via seq_update().
 *
 * @param e           NAT entry (already located via reverse lookup)
 * @param tcp_hdr     TCP header
 * @param is_reply    TRUE for WAN→LAN
 *
 * @return TRUE if seq/ack are within tolerated window, FALSE to drop.
 */
static inline BOOL tcp_conntrack_seq_valid(addr_table_t *e, struct rte_tcp_hdr *tcp_hdr,
    BOOL is_reply)
{
    U32 seq = rte_be_to_cpu_32(tcp_hdr->sent_seq);
    U32 ack = rte_be_to_cpu_32(tcp_hdr->recv_ack);
    U8  flags = tcp_hdr->tcp_flags;

    /* SYN establishes the seq baseline — let it pass and seed via seq_update. */
    if (flags & RTE_TCP_SYN_FLAG)
        return TRUE;

    /* Pick the opposite-direction state to compare against. */
    U32 peer_max_ack    = is_reply ? e->max_ack_lan    : e->max_ack_wan;
    U32 peer_max_seqend = is_reply ? e->max_seq_end_lan: e->max_seq_end_wan;

    /* Zero baseline: first packet seeds state, accept it. */
    if (peer_max_seqend == 0 && peer_max_ack == 0)
        return TRUE;

    /* Window check.  We don't parse TCP options so we don't know the scaled
     * window; use a generous fixed slack (16 MB) that covers realistic BDP for
     * residential / 10G-ish links while still catching blind seq injection
     * (random hits limited to ~32MB / 4GB ≈ 0.75% per packet). */
    int32_t seq_delta = (int32_t)(seq - peer_max_ack);
    if (seq_delta < -(int32_t)0x00FFFFFF || seq_delta > (int32_t)0x00FFFFFF)
        return FALSE;

    /* ACK must be vaguely near what the peer has actually sent.  Same slack;
     * negative delta is expected when the local side is sending faster than
     * the remote can ack (BDP in flight), positive delta is bounded by what
     * the peer has actually transmitted (small slack absorbs reordering). */
    if (flags & RTE_TCP_ACK_FLAG) {
        int32_t ack_delta = (int32_t)(ack - peer_max_seqend);
        if (ack_delta < -(int32_t)0x00FFFFFF || ack_delta > (int32_t)0xFFFF)
            return FALSE;
    }

    return TRUE;
}

/**
 * @fn tcp_conntrack_seq_update
 *
 * @brief Update the per-direction max(seq+payload), max(ack), and last window
 *        on a NAT entry after an accepted TCP packet.  Must be called on both
 *        directions to keep the baseline current.
 *
 * @param e           NAT entry
 * @param tcp_hdr     TCP header
 * @param payload_len TCP payload length in bytes (excludes TCP header)
 * @param is_reply    TRUE for WAN→LAN, FALSE for LAN→WAN
 */
static inline void tcp_conntrack_seq_update(addr_table_t *e, struct rte_tcp_hdr *tcp_hdr,
                                             U16 payload_len, BOOL is_reply)
{
    U32 seq = rte_be_to_cpu_32(tcp_hdr->sent_seq);
    U32 ack = rte_be_to_cpu_32(tcp_hdr->recv_ack);
    U16 win = rte_be_to_cpu_16(tcp_hdr->rx_win);
    /* SYN and FIN occupy one byte of seq space each. */
    U32 seq_end = seq + payload_len +
        ((tcp_hdr->tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_FIN_FLAG)) ? 1 : 0);

    /* Monotonic max with wrap-aware signed compare, but seed unconditionally
     * when the field is still zero — otherwise an ISN ≥ 2^31 looks "older
     * than zero" under signed math and the baseline never advances. */
    if (is_reply) {
        if (e->max_seq_end_wan == 0 ||
            (int32_t)(seq_end - e->max_seq_end_wan) > 0) e->max_seq_end_wan = seq_end;
        if (e->max_ack_wan == 0 ||
            (int32_t)(ack - e->max_ack_wan) > 0)         e->max_ack_wan     = ack;
        e->max_win_wan = win;
    } else {
        if (e->max_seq_end_lan == 0 ||
            (int32_t)(seq_end - e->max_seq_end_lan) > 0) e->max_seq_end_lan = seq_end;
        if (e->max_ack_lan == 0 ||
            (int32_t)(ack - e->max_ack_lan) > 0)         e->max_ack_lan     = ack;
        e->max_win_lan = win;
    }
}

/**
 * @fn tcp_conntrack_fsm
 *
 * @brief TCP connection tracking finite state machine.
 *        Table-driven lookup:
 *        1. Scan table for matching current state
 *        2. Within that state, scan for matching event
 *        3. Transition to next_state
 *        4. Execute action handler chain
 *
 * @param entry
 *        Pointer to NAT address table entry
 * @param tcp_flags
 *        TCP flags byte from rte_tcp_hdr
 * @param is_reply
 *        TRUE if packet is in WAN direction (WAN→LAN), FALSE for LAN (LAN→WAN)
 *
 * @return SUCCESS, or ERROR if a handler fails
 */
STATUS tcp_conntrack_fsm(struct addr_table *entry, U8 tcp_flags, BOOL is_reply);

/**
 * @fn tcp_conntrack_state2str
 *
 * @brief Convert TCP conntrack state to string for logging
 *
 * @param state
 *        TCP conntrack state
 *
 * @return String representation of the state
 */
const char *tcp_conntrack_state2str(U8 state);

#endif /* _TCP_CONNTRACK_H_ */
