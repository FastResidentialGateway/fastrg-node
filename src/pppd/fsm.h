/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
 	fsm.h

     Finite State Machine for PPP connection/call

  Designed by THE on Jan 14, 2019
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/
#ifndef _FSM_H_
#define _FSM_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <common.h>

#include <rte_timer.h> 

#include "codec.h"

typedef struct{
    U8      state;
    U16     event;
    U8      next_state;
    STATUS  (*hdl[10])(struct rte_timer *, ppp_ccb_t *);
} tPPP_STATE_TBL;

/*--------- STATE TYPE ----------*/
typedef enum {
    S_INIT,
    S_STARTING,
    S_CLOSED,
    S_STOPPED,
    S_CLOSING,
    S_STOPPING,
    S_REQUEST_SENT,
    S_ACK_RECEIVED,
    S_ACK_SENT,
    S_OPENED,
    S_INVLD,
} PPP_STATE;

/*----------------- EVENT TYPE --------------------
Q_ : Quest primitive 
E_ : Event */
typedef enum {
    E_UP,
    E_DOWN,
    E_OPEN,
    E_CLOSE,
    E_TIMEOUT_COUNTER_POSITIVE,
    E_TIMEOUT_COUNTER_EXPIRED,
    E_RECV_GOOD_CONFIG_REQUEST,
    E_RECV_BAD_CONFIG_REQUEST,
    E_RECV_CONFIG_ACK,
    E_RECV_CONFIG_NAK_REJ,
    E_RECV_TERMINATE_REQUEST,
    E_RECV_TERMINATE_ACK,
    E_RECV_UNKNOWN_CODE,
    E_RECV_GOOD_CODE_PROTOCOL_REJECT,
    E_RECV_BAD_CODE_PROTOCOL_REJECT,
    E_RECV_ECHO_REQUEST,
    E_RECV_ECHO_REPLY,
    E_UNKNOWN, // for log usage, not for fsm
} PPP_EVENT_TYPE;

/**
 * @fn PPP_FSM
 * 
 * @brief PPP Finite State Machine
 * 
 * @param ppp_timer
 *      PPP timer
 * @param s_ppp_ccb
 *      PPP control block pointer
 * @param event
 *      Event type
 * 
 * @return STATUS
 *      SUCCESS or ERROR
 */
STATUS PPP_FSM(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb, U16 event);
STATUS A_padi_timer_func(struct rte_timer *tim, ppp_ccb_t *s_ppp_ccb);
STATUS A_padr_timer_func(struct rte_timer *tim, ppp_ccb_t *s_ppp_ccb);

#ifdef __cplusplus
}
#endif

#endif /* header */
