/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  TCP_CONNTRACK.C

     TCP Connection Tracking State Machine for SNAT
     Table-driven FSM modeled

  Designed by THE on Apr 15, 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#include <rte_atomic.h>

#include <common.h>

#include "tcp_conntrack.h"
#include "pppd.h"

/*//////////////////////////////////////////////////////////////////////////////////
    ACTION HANDLERS
    Each handler performs one side-effect on the NAT entry during a state transition.
    All handlers are static — the state table (below) is the only caller.
///////////////////////////////////////////////////////////////////////////////////*/

static STATUS tcp_act_timeout_none(struct addr_table *entry)
{
    rte_atomic64_set(&((addr_table_t *)entry)->expire_at,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_NONE * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_syn_sent(struct addr_table *entry)
{
    rte_atomic64_set(&((addr_table_t *)entry)->expire_at,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_SYN_SENT * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_syn_recv(struct addr_table *entry)
{
    rte_atomic64_set(&((addr_table_t *)entry)->expire_at,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_SYN_RECV * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_established(struct addr_table *entry)
{
    rte_atomic64_set(&((addr_table_t *)entry)->expire_at,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_ESTABLISHED * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_fin_wait(struct addr_table *entry)
{
    rte_atomic64_set(&((addr_table_t *)entry)->expire_at,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_FIN_WAIT * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_close_wait(struct addr_table *entry)
{
    rte_atomic64_set(&((addr_table_t *)entry)->expire_at,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_CLOSE_WAIT * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_last_ack(struct addr_table *entry)
{
    rte_atomic64_set(&((addr_table_t *)entry)->expire_at,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_LAST_ACK * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_time_wait(struct addr_table *entry)
{
    rte_atomic64_set(&((addr_table_t *)entry)->expire_at,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_TIME_WAIT * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_timeout_close(struct addr_table *entry)
{
    rte_atomic64_set(&((addr_table_t *)entry)->expire_at,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_CLOSE * fastrg_get_cycles_in_sec());
    return SUCCESS;
}

static STATUS tcp_act_set_fin_orig(struct addr_table *entry)
{
    ((addr_table_t *)entry)->tcp_fin_flags |= TCP_FIN_FLAG_ORIGINATOR;
    return SUCCESS;
}

static STATUS tcp_act_set_fin_resp(struct addr_table *entry)
{
    ((addr_table_t *)entry)->tcp_fin_flags |= TCP_FIN_FLAG_RESPONDER;
    return SUCCESS;
}

static STATUS tcp_act_reset_fin_flags(struct addr_table *entry)
{
    ((addr_table_t *)entry)->tcp_fin_flags = 0;
    return SUCCESS;
}

/*//////////////////////////////////////////////////////////////////////////////////
    STATE            EVENT              NEXT-STATE              HANDLERS
    Splitting TCP_EV_FIN into TCP_EV_FIN_ORIG / TCP_EV_FIN_RESP eliminates
    the need for any direction special-casing in the FSM body.
///////////////////////////////////////////////////////////////////////////////////*/
static tcp_conntrack_state_tbl_t tcp_conntrack_tbl[] = {
    /* NONE: initial state for new TCP entries */
    { TCP_CONNTRACK_NONE,        TCP_EV_SYN,       TCP_CONNTRACK_SYN_SENT,     { tcp_act_timeout_syn_sent,  NULL } },
    { TCP_CONNTRACK_NONE,        TCP_EV_SYN_ACK,   TCP_CONNTRACK_SYN_RECV,     { tcp_act_timeout_syn_recv,  NULL } },
    { TCP_CONNTRACK_NONE,        TCP_EV_ACK,       TCP_CONNTRACK_NONE,          { tcp_act_timeout_none,      NULL } },
    { TCP_CONNTRACK_NONE,        TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close,     NULL } },

    /* SYN_SENT: SYN sent from originator, awaiting SYN-ACK */
    { TCP_CONNTRACK_SYN_SENT,    TCP_EV_SYN,       TCP_CONNTRACK_SYN_SENT,      { tcp_act_timeout_syn_sent,  NULL } },  /* SYN retransmit */
    { TCP_CONNTRACK_SYN_SENT,    TCP_EV_SYN_ACK,   TCP_CONNTRACK_SYN_RECV,      { tcp_act_timeout_syn_recv,  NULL } },
    { TCP_CONNTRACK_SYN_SENT,    TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close,     NULL } },

    /* SYN_RECV: SYN-ACK seen, awaiting final ACK */
    { TCP_CONNTRACK_SYN_RECV,    TCP_EV_ACK,       TCP_CONNTRACK_ESTABLISHED,   { tcp_act_timeout_established, NULL } },
    { TCP_CONNTRACK_SYN_RECV,    TCP_EV_SYN_ACK,   TCP_CONNTRACK_SYN_RECV,      { tcp_act_timeout_syn_recv,    NULL } },  /* SYN-ACK retransmit */
    { TCP_CONNTRACK_SYN_RECV,    TCP_EV_FIN_ORIG,  TCP_CONNTRACK_FIN_WAIT,      { tcp_act_set_fin_orig, tcp_act_timeout_fin_wait, NULL } },
    { TCP_CONNTRACK_SYN_RECV,    TCP_EV_FIN_RESP,  TCP_CONNTRACK_FIN_WAIT,      { tcp_act_set_fin_resp, tcp_act_timeout_fin_wait, NULL } },
    { TCP_CONNTRACK_SYN_RECV,    TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },

    /* ESTABLISHED: connection fully open */
    { TCP_CONNTRACK_ESTABLISHED, TCP_EV_ACK,       TCP_CONNTRACK_ESTABLISHED,   { tcp_act_timeout_established,  NULL } },
    { TCP_CONNTRACK_ESTABLISHED, TCP_EV_FIN_ORIG,  TCP_CONNTRACK_FIN_WAIT,      { tcp_act_set_fin_orig, tcp_act_timeout_fin_wait,   NULL } },
    { TCP_CONNTRACK_ESTABLISHED, TCP_EV_FIN_RESP,  TCP_CONNTRACK_CLOSE_WAIT,    { tcp_act_set_fin_resp, tcp_act_timeout_close_wait, NULL } },
    { TCP_CONNTRACK_ESTABLISHED, TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },

    /* FIN_WAIT: FIN sent by originator, awaiting FIN from responder */
    { TCP_CONNTRACK_FIN_WAIT,    TCP_EV_FIN_RESP,  TCP_CONNTRACK_TIME_WAIT,     { tcp_act_set_fin_resp, tcp_act_timeout_time_wait, NULL } },
    { TCP_CONNTRACK_FIN_WAIT,    TCP_EV_FIN_ORIG,  TCP_CONNTRACK_FIN_WAIT,      { tcp_act_set_fin_orig, tcp_act_timeout_fin_wait,  NULL } },  /* FIN retransmit */
    { TCP_CONNTRACK_FIN_WAIT,    TCP_EV_ACK,       TCP_CONNTRACK_FIN_WAIT,      { tcp_act_timeout_fin_wait, NULL } },
    { TCP_CONNTRACK_FIN_WAIT,    TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },

    /* CLOSE_WAIT: FIN received from responder, waiting for originator to close */
    { TCP_CONNTRACK_CLOSE_WAIT,  TCP_EV_FIN_ORIG,  TCP_CONNTRACK_LAST_ACK,      { tcp_act_set_fin_orig, tcp_act_timeout_last_ack,   NULL } },
    { TCP_CONNTRACK_CLOSE_WAIT,  TCP_EV_ACK,       TCP_CONNTRACK_CLOSE_WAIT,    { tcp_act_timeout_close_wait, NULL } },
    { TCP_CONNTRACK_CLOSE_WAIT,  TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },

    /* LAST_ACK: waiting for final ACK after originator sent FIN in CLOSE_WAIT */
    { TCP_CONNTRACK_LAST_ACK,    TCP_EV_ACK,       TCP_CONNTRACK_TIME_WAIT,     { tcp_act_timeout_time_wait, NULL } },
    { TCP_CONNTRACK_LAST_ACK,    TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },

    /* TIME_WAIT: both FINs acked, waiting 2*MSL */
    { TCP_CONNTRACK_TIME_WAIT,   TCP_EV_ACK,       TCP_CONNTRACK_TIME_WAIT,     { tcp_act_timeout_time_wait, NULL } },  /* late ACK retransmit */
    { TCP_CONNTRACK_TIME_WAIT,   TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },
    { TCP_CONNTRACK_TIME_WAIT,   TCP_EV_SYN,       TCP_CONNTRACK_SYN_SENT,      { tcp_act_reset_fin_flags, tcp_act_timeout_syn_sent, NULL } },  /* new connection reuse during 2MSL */

    /* CLOSE: RST or fully closed */
    { TCP_CONNTRACK_CLOSE,       TCP_EV_RST,       TCP_CONNTRACK_CLOSE,         { tcp_act_timeout_close, NULL } },
    { TCP_CONNTRACK_CLOSE,       TCP_EV_SYN,       TCP_CONNTRACK_SYN_SENT,      { tcp_act_reset_fin_flags, tcp_act_timeout_syn_sent, NULL } },    /* new connection from originator */
    { TCP_CONNTRACK_CLOSE,       TCP_EV_SYN_ACK,   TCP_CONNTRACK_SYN_RECV,      { tcp_act_reset_fin_flags, tcp_act_timeout_syn_recv, NULL } },    /* new passive connection */

    /* Sentinel */
    { TCP_CONNTRACK_INVLD, 0, 0, { NULL } },
};

STATUS tcp_conntrack_fsm(struct addr_table *entry, U8 tcp_flags, BOOL is_reply)
{
    addr_table_t *e = (addr_table_t *)entry;
    tcp_conntrack_event_t event = tcp_flags_to_event(tcp_flags, is_reply);
    int i;

    if (event == TCP_EV_INVLD)
        return SUCCESS;

    /* Find matched state */
    for(i=0; tcp_conntrack_tbl[i].state != TCP_CONNTRACK_INVLD; i++)
        if (tcp_conntrack_tbl[i].state == e->tcp_state)
            break;

    if (tcp_conntrack_tbl[i].state == TCP_CONNTRACK_INVLD)
        return ERROR;

    /* Find matched event within that state */
    for(; tcp_conntrack_tbl[i].state == e->tcp_state; i++)
        if (tcp_conntrack_tbl[i].event == (U8)event)
            break;

    /* No matching event for current state — ignore */
    if (tcp_conntrack_tbl[i].state != e->tcp_state)
        return SUCCESS;

    /* State transition */
    e->tcp_state = tcp_conntrack_tbl[i].next_state;

    /* Execute NULL-terminated handler chain */
    for(int j=0; tcp_conntrack_tbl[i].hdl[j]; j++) {
        if ((*tcp_conntrack_tbl[i].hdl[j])(entry) == ERROR)
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
    };

    if (state >= TCP_CONNTRACK_INVLD)
        return "UNKNOWN";
    return state_str[state];
}

