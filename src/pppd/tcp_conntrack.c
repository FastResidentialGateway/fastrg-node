/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  TCP_CONNTRACK.C

     TCP Connection Tracking State Machine for SNAT
     Table-driven FSM modeled

  Designed by THE on Apr 15, 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#include <string.h>

#include <rte_atomic.h>
#include <rte_byteorder.h>

#include <common.h>

#include "tcp_conntrack.h"
#include "pppd.h"

/*//////////////////////////////////////////////////////////////////////////////////
    ACTION HANDLERS
    Each handler performs one side-effect on the NAT entry during a state transition.
    All handlers are static — the state table (below) is the only caller.
    The is_reply parameter lets a handler take direction-dependent action; most
    handlers ignore it.
///////////////////////////////////////////////////////////////////////////////////*/

static STATUS tcp_act_timeout_syn_sent(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    nat_expire_set(((addr_table_t *)entry)->expire_slot,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_SYN_SENT * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_syn_recv(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    nat_expire_set(((addr_table_t *)entry)->expire_slot,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_SYN_RECV * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_established(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    nat_expire_set(((addr_table_t *)entry)->expire_slot,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_ESTABLISHED * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

/* Same deadline as tcp_act_timeout_established but write-coalesced: the
 * (ESTABLISHED, ACK) row fires on EVERY data packet of an established flow,
 * so an unconditional store would dirty the shared expire cache line per
 * packet.  Transitions INTO established keep the unconditional setter. */
static STATUS tcp_act_refresh_established(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    nat_expire_refresh(((addr_table_t *)entry)->expire_slot,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_ESTABLISHED * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_fin_wait(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    nat_expire_set(((addr_table_t *)entry)->expire_slot,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_FIN_WAIT * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_close_wait(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    nat_expire_set(((addr_table_t *)entry)->expire_slot,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_CLOSE_WAIT * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_last_ack(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    nat_expire_set(((addr_table_t *)entry)->expire_slot,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_LAST_ACK * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_time_wait(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    nat_expire_set(((addr_table_t *)entry)->expire_slot,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_TIME_WAIT * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_close(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    nat_expire_set(((addr_table_t *)entry)->expire_slot,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_CLOSE * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_mid_stream(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    nat_expire_set(((addr_table_t *)entry)->expire_slot,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_MID_STREAM * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

/* MID_STREAM + ACK: only promote to ESTABLISHED when the ACK comes from the
 * WAN side — that's the bidirectional confirmation we were waiting for.
 * On a LAN-side ACK we just refresh the MID_STREAM timeout. */
static STATUS tcp_act_mid_stream_ack(struct addr_table *entry, BOOL is_reply)
{
    addr_table_t *e = (addr_table_t *)entry;

    if (is_reply) {
        e->tcp_state = TCP_CONNTRACK_ESTABLISHED;
        nat_expire_set(e->expire_slot,
            fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_ESTABLISHED * fastrg_get_cycles_in_sec());
    } else {
        nat_expire_refresh(e->expire_slot,
            fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_MID_STREAM * fastrg_get_cycles_in_sec());
    }
    return SUCCESS;
}

static STATUS tcp_act_set_fin_lan(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    ((addr_table_t *)entry)->tcp_fin_flags |= TCP_FIN_FLAG_LAN;
    return SUCCESS;
}

static STATUS tcp_act_set_fin_wan(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    ((addr_table_t *)entry)->tcp_fin_flags |= TCP_FIN_FLAG_WAN;
    return SUCCESS;
}

static STATUS tcp_act_reset_fin_flags(struct addr_table *entry, BOOL is_reply)
{
    (void)is_reply;
    ((addr_table_t *)entry)->tcp_fin_flags = 0;
    return SUCCESS;
}

/*//////////////////////////////////////////////////////////////////////////////////
    STATE            EVENT              NEXT-STATE              HANDLERS
    Splitting TCP_EV_FIN into TCP_EV_FIN_LAN / TCP_EV_FIN_WAN eliminates
    the need for any direction special-casing in the FSM body.
///////////////////////////////////////////////////////////////////////////////////*/
static tcp_conntrack_state_tbl_t tcp_conntrack_tbl[] = {
    /* NONE: initial state for new TCP entries.  A non-SYN first packet means the flow
     * was already established before we started tracking it (e.g. FastRG restarted while
     * LAN clients held long-lived connections); on bare ACK we route via MID_STREAM
     * (60s probationary timeout) instead of jumping straight to ESTABLISHED, so we
     * don't grant a 7200s lease on something we haven't seen the WAN-side ACK confirm. */
    { TCP_CONNTRACK_NONE,        TCP_EV_SYN,       TCP_CONNTRACK_SYN_SENT,      { tcp_act_timeout_syn_sent,  NULL } },
    { TCP_CONNTRACK_NONE,        TCP_EV_SYN_ACK,   TCP_CONNTRACK_SYN_RECV,      { tcp_act_timeout_syn_recv,  NULL } },
    { TCP_CONNTRACK_NONE,        TCP_EV_ACK,       TCP_CONNTRACK_MID_STREAM,    { tcp_act_timeout_mid_stream, NULL } },
    { TCP_CONNTRACK_NONE,        TCP_EV_FIN_LAN,   TCP_CONNTRACK_FIN_WAIT,      { tcp_act_set_fin_lan, tcp_act_timeout_fin_wait,   NULL } },
    { TCP_CONNTRACK_NONE,        TCP_EV_FIN_WAN,   TCP_CONNTRACK_CLOSE_WAIT,    { tcp_act_set_fin_wan, tcp_act_timeout_close_wait, NULL } },
    { TCP_CONNTRACK_NONE,        TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close,     NULL } },

    /* MID_STREAM: probationary state for loose pickup.  Promote to ESTABLISHED only
     * on a WAN-side ACK (handled inside tcp_act_mid_stream_ack); LAN-side
     * traffic just refreshes the 60s timeout. */
    { TCP_CONNTRACK_MID_STREAM,  TCP_EV_ACK,       TCP_CONNTRACK_MID_STREAM,    { tcp_act_mid_stream_ack, NULL } },
    { TCP_CONNTRACK_MID_STREAM,  TCP_EV_SYN,       TCP_CONNTRACK_SYN_SENT,      { tcp_act_reset_fin_flags, tcp_act_timeout_syn_sent, NULL } },
    { TCP_CONNTRACK_MID_STREAM,  TCP_EV_SYN_ACK,   TCP_CONNTRACK_MID_STREAM,    { tcp_act_timeout_mid_stream, NULL } },
    { TCP_CONNTRACK_MID_STREAM,  TCP_EV_FIN_LAN,   TCP_CONNTRACK_FIN_WAIT,      { tcp_act_set_fin_lan, tcp_act_timeout_fin_wait,   NULL } },
    { TCP_CONNTRACK_MID_STREAM,  TCP_EV_FIN_WAN,   TCP_CONNTRACK_CLOSE_WAIT,    { tcp_act_set_fin_wan, tcp_act_timeout_close_wait, NULL } },
    { TCP_CONNTRACK_MID_STREAM,  TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },

    /* SYN_SENT: SYN sent from LAN, awaiting SYN-ACK */
    { TCP_CONNTRACK_SYN_SENT,    TCP_EV_SYN,       TCP_CONNTRACK_SYN_SENT,      { tcp_act_timeout_syn_sent,  NULL } },  /* SYN retransmit */
    { TCP_CONNTRACK_SYN_SENT,    TCP_EV_SYN_ACK,   TCP_CONNTRACK_SYN_RECV,      { tcp_act_timeout_syn_recv,  NULL } },
    { TCP_CONNTRACK_SYN_SENT,    TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close,     NULL } },

    /* SYN_RECV: SYN-ACK seen, awaiting final ACK */
    { TCP_CONNTRACK_SYN_RECV,    TCP_EV_ACK,       TCP_CONNTRACK_ESTABLISHED,   { tcp_act_timeout_established, NULL } },
    { TCP_CONNTRACK_SYN_RECV,    TCP_EV_SYN_ACK,   TCP_CONNTRACK_SYN_RECV,      { tcp_act_timeout_syn_recv,    NULL } },  /* SYN-ACK retransmit */
    { TCP_CONNTRACK_SYN_RECV,    TCP_EV_FIN_LAN,   TCP_CONNTRACK_FIN_WAIT,      { tcp_act_set_fin_lan, tcp_act_timeout_fin_wait, NULL } },
    { TCP_CONNTRACK_SYN_RECV,    TCP_EV_FIN_WAN,   TCP_CONNTRACK_FIN_WAIT,      { tcp_act_set_fin_wan, tcp_act_timeout_fin_wait, NULL } },
    { TCP_CONNTRACK_SYN_RECV,    TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },

    /* ESTABLISHED: connection fully open.  SYN means the client reused the ephemeral
     * port before the NAT entry expired (7200s) — reset to SYN_SENT so the new
     * handshake can complete.  SYN-ACK in ESTABLISHED is always a server retransmit:
     * the server didn't receive our final ACK and is resending its SYN-ACK; stay in
     * ESTABLISHED and just refresh the timeout — do NOT regress to SYN_RECV, which
     * would cause state oscillation every time the server retransmits. */
    { TCP_CONNTRACK_ESTABLISHED, TCP_EV_ACK,       TCP_CONNTRACK_ESTABLISHED,   { tcp_act_refresh_established,  NULL } },
    { TCP_CONNTRACK_ESTABLISHED, TCP_EV_SYN,       TCP_CONNTRACK_SYN_SENT,      { tcp_act_reset_fin_flags, tcp_act_timeout_syn_sent, NULL } },
    { TCP_CONNTRACK_ESTABLISHED, TCP_EV_SYN_ACK,   TCP_CONNTRACK_ESTABLISHED,   { tcp_act_timeout_established, NULL } },
    { TCP_CONNTRACK_ESTABLISHED, TCP_EV_FIN_LAN,   TCP_CONNTRACK_FIN_WAIT,      { tcp_act_set_fin_lan, tcp_act_timeout_fin_wait,   NULL } },
    { TCP_CONNTRACK_ESTABLISHED, TCP_EV_FIN_WAN,   TCP_CONNTRACK_CLOSE_WAIT,    { tcp_act_set_fin_wan, tcp_act_timeout_close_wait, NULL } },
    { TCP_CONNTRACK_ESTABLISHED, TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },

    /* FIN_WAIT: FIN sent by LAN, awaiting FIN from WAN */
    { TCP_CONNTRACK_FIN_WAIT,    TCP_EV_FIN_WAN,   TCP_CONNTRACK_TIME_WAIT,     { tcp_act_set_fin_wan, tcp_act_timeout_time_wait, NULL } },
    { TCP_CONNTRACK_FIN_WAIT,    TCP_EV_FIN_LAN,   TCP_CONNTRACK_FIN_WAIT,      { tcp_act_set_fin_lan, tcp_act_timeout_fin_wait,  NULL } },  /* FIN retransmit */
    { TCP_CONNTRACK_FIN_WAIT,    TCP_EV_ACK,       TCP_CONNTRACK_FIN_WAIT,      { tcp_act_timeout_fin_wait, NULL } },
    { TCP_CONNTRACK_FIN_WAIT,    TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },

    /* CLOSE_WAIT: FIN received from WAN, waiting for LAN to close */
    { TCP_CONNTRACK_CLOSE_WAIT,  TCP_EV_FIN_LAN,   TCP_CONNTRACK_LAST_ACK,      { tcp_act_set_fin_lan, tcp_act_timeout_last_ack,   NULL } },
    { TCP_CONNTRACK_CLOSE_WAIT,  TCP_EV_ACK,       TCP_CONNTRACK_CLOSE_WAIT,    { tcp_act_timeout_close_wait, NULL } },
    { TCP_CONNTRACK_CLOSE_WAIT,  TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },

    /* LAST_ACK: waiting for final ACK after LAN sent FIN in CLOSE_WAIT */
    { TCP_CONNTRACK_LAST_ACK,    TCP_EV_ACK,       TCP_CONNTRACK_TIME_WAIT,     { tcp_act_timeout_time_wait, NULL } },
    { TCP_CONNTRACK_LAST_ACK,    TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },

    /* TIME_WAIT: both FINs acked, waiting 2*MSL */
    { TCP_CONNTRACK_TIME_WAIT,   TCP_EV_ACK,       TCP_CONNTRACK_TIME_WAIT,     { tcp_act_timeout_time_wait, NULL } },  /* late ACK retransmit */
    { TCP_CONNTRACK_TIME_WAIT,   TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },
    { TCP_CONNTRACK_TIME_WAIT,   TCP_EV_SYN,       TCP_CONNTRACK_SYN_SENT,      { tcp_act_reset_fin_flags, tcp_act_timeout_syn_sent, NULL } },  /* new connection reuse during 2MSL */

    /* CLOSE: RST or fully closed.  No SYN_ACK rule: SNAT entries are generated at LAN → WAN, 
     * so a LAN-side SYN_ACK on a CLOSE entry has no legitimate path — leave it. */
    { TCP_CONNTRACK_CLOSE,       TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },
    { TCP_CONNTRACK_CLOSE,       TCP_EV_SYN,       TCP_CONNTRACK_SYN_SENT,      { tcp_act_reset_fin_flags, tcp_act_timeout_syn_sent, NULL } },    /* new connection from LAN */

    /* Sentinel */
    { TCP_CONNTRACK_INVLD, 0, 0, { NULL } },
};

/* Both enums are contiguous 0-based, so the sentinels double as the counts
 * and (state, event) can index a 2-D array directly. */
#define TCP_NSTATE TCP_CONNTRACK_INVLD /* 10 */
#define TCP_NEVENT TCP_EV_INVLD        /*  6 */

/* (state, event) → tcp_conntrack_tbl row index; -1 = no transition defined */
static int8_t tcp_fsm_idx[TCP_NSTATE][TCP_NEVENT];

/* Whether a state has at least one table row.  Preserves the former "state
 * not found in table → ERROR" contract: a state stripped of all its rows
 * must keep failing loudly instead of degrading to silent ignore.
 * Static storage → already zero-initialized to all-FALSE (unlike tcp_fsm_idx,
 * whose empty value is -1 and therefore needs the explicit memset). */
static BOOL tcp_state_has_rows[TCP_NSTATE];

/**
 * @fn tcp_conntrack_build_index
 *
 * @brief Build the O(1) (state, event) → row index from tcp_conntrack_tbl,
 *        which stays the single source of truth.  Runs before main() via GCC
 *        constructor (the table is compile-time static-initialized, so it is
 *        ready).  First matching row wins, mirroring the former linear-scan
 *        semantics, and rows no longer need to be grouped by state.
 */
static void __attribute__((constructor)) tcp_conntrack_build_index(void)
{
    memset(tcp_fsm_idx, -1, sizeof(tcp_fsm_idx));
    for(int i=0; tcp_conntrack_tbl[i].state != TCP_CONNTRACK_INVLD; i++) {
        U8 s = tcp_conntrack_tbl[i].state;
        U8 e = tcp_conntrack_tbl[i].event;

        if (s < TCP_NSTATE && e < TCP_NEVENT) {
            tcp_state_has_rows[s] = TRUE;
            if (tcp_fsm_idx[s][e] < 0)
                tcp_fsm_idx[s][e] = (int8_t)i;
        }
    }
}

STATUS tcp_conntrack_fsm(struct addr_table *entry, U8 tcp_flags, BOOL is_reply)
{
    addr_table_t *e = (addr_table_t *)entry;
    tcp_conntrack_event_t event = tcp_flags_to_event(tcp_flags, is_reply);

    if (event == TCP_EV_INVLD)
        return SUCCESS;

    /* Out-of-range (corrupted) state */
    if (e->tcp_state >= TCP_NSTATE)
        return ERROR;

    int8_t idx = tcp_fsm_idx[e->tcp_state][event];

    /* No transition defined for (state, event): ignore if the state has any
     * table row at all; a state with no rows keeps the former "state not
     * found in table" ERROR contract. */
    if (idx < 0)
        return tcp_state_has_rows[e->tcp_state] ? SUCCESS : ERROR;

    const tcp_conntrack_state_tbl_t *t = &tcp_conntrack_tbl[idx];

    /* State transition */
    e->tcp_state = t->next_state;

    /* Execute NULL-terminated handler chain.  A handler may further mutate
     * tcp_state (e.g. tcp_act_mid_stream_ack promotes MID_STREAM→ESTABLISHED on
     * WAN-side ACK). */
    for(int j=0; j<4 && t->hdl[j]; j++) {
        if ((*t->hdl[j])(entry, is_reply) == ERROR)
            return ERROR;
    }

    return SUCCESS;
}

const char *tcp_conntrack_state2str(U8 state)
{
    static const char *state_str[] = {
        [TCP_CONNTRACK_NONE]        = "NONE",
        [TCP_CONNTRACK_SYN_SENT]    = "SYN_SENT",
        [TCP_CONNTRACK_SYN_RECV]    = "SYN_RECV",
        [TCP_CONNTRACK_ESTABLISHED] = "ESTABLISHED",
        [TCP_CONNTRACK_FIN_WAIT]    = "FIN_WAIT",
        [TCP_CONNTRACK_CLOSE_WAIT]  = "CLOSE_WAIT",
        [TCP_CONNTRACK_LAST_ACK]    = "LAST_ACK",
        [TCP_CONNTRACK_TIME_WAIT]   = "TIME_WAIT",
        [TCP_CONNTRACK_CLOSE]       = "CLOSE",
        [TCP_CONNTRACK_MID_STREAM]  = "MID_STREAM",
    };

    if (state >= TCP_CONNTRACK_INVLD)
        return "UNKNOWN";
    return state_str[state];
}

