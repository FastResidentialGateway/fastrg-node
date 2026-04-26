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
    TCP_CONNTRACK_INVLD,         /* Sentinel (table terminator) */
} tcp_conntrack_state_t;

/*--------- TCP CONNTRACK EVENT TYPE ----------*/
typedef enum {
    TCP_EV_SYN,       /* SYN only (no ACK) */
    TCP_EV_SYN_ACK,   /* SYN + ACK */
    TCP_EV_ACK,       /* ACK (data or handshake) */
    TCP_EV_FIN_ORIG,  /* FIN from originator (LAN→WAN) */
    TCP_EV_FIN_RESP,  /* FIN from responder (WAN→LAN) */
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

/*--------- FIN FLAGS BITMASK ----------*/
#define TCP_FIN_FLAG_ORIGINATOR  0x01  /* FIN seen from originator (LAN→WAN) */
#define TCP_FIN_FLAG_RESPONDER   0x02  /* FIN seen from responder (WAN→LAN) */

/* Forward declaration */
struct addr_table;

/*--------- STATE TABLE STRUCTURE ----------*/
typedef struct {
    U8     state;        /* current TCP conntrack state */
    U8     event;        /* TCP flag event (direction-aware for FIN) */
    U8     next_state;   /* state to transition to */
    STATUS (*hdl[4])(struct addr_table *);  /* NULL-terminated action handler chain */
} tcp_conntrack_state_tbl_t;

/**
 * @fn tcp_flags_to_event
 *
 * @brief Convert TCP header flags + packet direction to a direction-aware conntrack event.
 *        FIN events are split by direction so the state table can handle
 *        originator-close (→FIN_WAIT) and responder-close (→CLOSE_WAIT) without
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
        return is_reply ? TCP_EV_FIN_RESP : TCP_EV_FIN_ORIG;
    if (tcp_flags & RTE_TCP_ACK_FLAG)
        return TCP_EV_ACK;
    return TCP_EV_INVLD;
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
 *        TRUE if packet is in reply direction (WAN→LAN), FALSE for originator (LAN→WAN)
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
