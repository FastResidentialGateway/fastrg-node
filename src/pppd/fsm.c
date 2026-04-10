#include <inttypes.h>

#include <rte_common.h>
#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_timer.h>
#include <rte_debug.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_flow.h>

#include "../dbg.h"
#include "../fastrg.h"
#include "../dp.h"
#include "nat.h"
#include "fsm.h"
#include "../dhcpd/dhcpd.h"
#include "../dnsd/dnsd.h"
#include "../etcd_integration.h"
#include "../../northbound/controller/etcd_client.h"

static STATUS A_this_layer_start(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_send_config_request(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_this_layer_finish(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_send_terminate_ack(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_send_code_reject(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_create_down_event(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_create_up_event(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_send_config_ack(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_send_config_nak_rej(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_send_terminate_request(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_this_layer_up(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_this_layer_down(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_init_restart_count(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_init_restart_config(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_init_restart_termin(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_send_echo_reply(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_zero_restart_count(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_send_padt(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);
static STATUS A_create_close_to_lower_layer(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb);

tPPP_STATE_TBL  ppp_fsm_tbl_lcp[] = { 
/*//////////////////////////////////////////////////////////////////////////////////
    STATE           EVENT           						      NEXT-STATE            HANDLER       
///////////////////////////////////////////////////////////////////////////////////\*/
{ S_INIT,		  	E_UP,     							    	  	S_CLOSED,		    { 0 }},

/* these actions are "this layer start" (tls) and should retern "UP" event */
{ S_INIT,		  	E_OPEN,     						    		S_STARTING,		    { A_this_layer_start, 0 }},

{ S_INIT, 			E_CLOSE,							      		S_INIT, 		    { 0 }},	                                                      	  

/*---------------------------------------------------------------------------*/
{ S_STARTING,		E_UP,     							    		S_REQUEST_SENT,	  	{ A_send_config_request, A_init_restart_config, 0 }},

{ S_STARTING,		E_OPEN,    							    		S_STARTING,		    { 0 }},

/* these actions are "this layer finish" (tlf) and should retern "DOWN" event */
{ S_STARTING,		E_CLOSE,    						    		S_INIT,			    { A_this_layer_finish, A_send_padt, 0 }},

/*---------------------------------------------------------------------------*/
{ S_CLOSED,			E_UP, 							      		    S_CLOSED,			{ 0 }},

{ S_CLOSED,			E_DOWN, 							      		S_INIT,			    { A_send_padt, 0 }},

{ S_CLOSED,			E_OPEN, 							      		S_REQUEST_SENT,	  	{ A_send_config_request, A_init_restart_config, 0 }},

{ S_CLOSED,			E_CLOSE, 							      		S_CLOSED,		    { 0 }},

{ S_CLOSED,			E_RECV_GOOD_CONFIG_REQUEST, 					S_CLOSED,		    { A_send_terminate_ack, 0 }},

{ S_CLOSED,			E_RECV_BAD_CONFIG_REQUEST, 						S_CLOSED,		    { A_send_terminate_ack, 0 }},

{ S_CLOSED,			E_RECV_CONFIG_ACK, 								S_CLOSED,		    { A_send_terminate_ack, 0 }},

{ S_CLOSED,			E_RECV_CONFIG_NAK_REJ, 							S_CLOSED,		    { A_send_terminate_ack, 0 }},

{ S_CLOSED,			E_RECV_TERMINATE_REQUEST, 						S_CLOSED,		    { A_send_terminate_ack, 0 }},

{ S_CLOSED,			E_RECV_TERMINATE_ACK, 							S_CLOSED,		    { 0 }},

{ S_CLOSED,			E_RECV_UNKNOWN_CODE, 							S_CLOSED,		    { A_send_code_reject, 0 }},

/* recv code/protocol reject when rejected value is acceptable, 
    such as a Code-Reject of an extended code, 
    or a Protocol-Reject of a NCP */ 
{ S_CLOSED,			E_RECV_GOOD_CODE_PROTOCOL_REJECT, 				S_CLOSED,	  		{ 0 }},

{ S_CLOSED,			E_RECV_BAD_CODE_PROTOCOL_REJECT, 				S_CLOSED,			{ A_this_layer_finish, 0 }},

{ S_CLOSED,			E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST, 		S_CLOSED,			{ 0 }},

/*---------------------------------------------------------------------------*/
{ S_STOPPED,		E_DOWN,     						   			S_STARTING,		    { A_this_layer_start, 0 }},

{ S_STOPPED,		E_OPEN,     						   			S_STOPPED,		    { A_create_down_event, A_create_up_event, 0 }},

{ S_STOPPED,		E_CLOSE,     						   			S_CLOSED,		    { A_send_padt, 0 }},

{ S_STOPPED,		E_RECV_GOOD_CONFIG_REQUEST,						S_ACK_SENT,		    { A_init_restart_config, A_send_config_request, A_send_config_ack, 0 }},

{ S_STOPPED,		E_RECV_BAD_CONFIG_REQUEST, 						S_REQUEST_SENT,	  	{ A_init_restart_config, A_send_config_request, A_send_config_nak_rej, 0 }},

{ S_STOPPED,		E_RECV_CONFIG_ACK, 				 				S_STOPPED,		    { A_send_terminate_ack, 0 }},

{ S_STOPPED,		E_RECV_CONFIG_NAK_REJ, 		 					S_STOPPED,		    { A_send_terminate_ack, 0 }},

{ S_STOPPED,		E_RECV_TERMINATE_REQUEST,  						S_STOPPED,		    { A_send_terminate_ack, 0 }},

{ S_STOPPED,		E_RECV_TERMINATE_ACK, 		 					S_STOPPED,		    { 0 }},

{ S_STOPPED,		E_RECV_UNKNOWN_CODE, 			 				S_STOPPED,		    { A_send_code_reject, 0 }},

/* recv code/protocol reject when rejected value is acceptable, 
    such as a Code-Reject of an extended code, 
    or a Protocol-Reject of a NCP */ 
{ S_STOPPED,		E_RECV_GOOD_CODE_PROTOCOL_REJECT, 				S_STOPPED,			{ 0 }},

{ S_STOPPED,		E_RECV_BAD_CODE_PROTOCOL_REJECT, 				S_STOPPED,			{ A_this_layer_finish, 0 }},

{ S_STOPPED,		E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST, 		S_STOPPED,			{ 0 }},

/*---------------------------------------------------------------------------*/
{ S_CLOSING, 		E_DOWN,								     		S_INIT,			    { 0 }},

{ S_CLOSING, 		E_OPEN,								     		S_STOPPING,		    { A_create_down_event, A_create_up_event, 0 }},

{ S_CLOSING, 		E_CLOSE,							     		S_CLOSING,		    { 0 }},

{ S_CLOSING, 		E_TIMEOUT_COUNTER_POSITIVE,						S_CLOSING,		    { A_send_terminate_request, 0 }},

{ S_CLOSING, 		E_TIMEOUT_COUNTER_EXPIRED, 						S_CLOSED,		    { A_this_layer_finish, 0 }},

{ S_CLOSING,		E_RECV_GOOD_CONFIG_REQUEST,						S_CLOSING,		    { 0 }},

{ S_CLOSING,		E_RECV_BAD_CONFIG_REQUEST, 						S_CLOSING,		    { 0 }},

{ S_CLOSING,		E_RECV_CONFIG_ACK, 								S_CLOSING,		    { 0 }},

{ S_CLOSING,		E_RECV_CONFIG_NAK_REJ, 		 					S_CLOSING,		    { 0 }},

{ S_CLOSING,		E_RECV_TERMINATE_REQUEST,  						S_CLOSING,		    { A_send_terminate_ack, 0 }},

{ S_CLOSING,		E_RECV_TERMINATE_ACK, 		 					S_CLOSED,		    { A_create_down_event, A_create_up_event, 0 }},

{ S_CLOSING,		E_RECV_UNKNOWN_CODE, 							S_CLOSING,		    { A_send_code_reject, 0 }},

/* recv code/protocol reject when rejected value is acceptable, 
    such as a Code-Reject of an extended code, 
    or a Protocol-Reject of a NCP */ 
{ S_CLOSING,		E_RECV_GOOD_CODE_PROTOCOL_REJECT, 				S_CLOSING,  		{ 0 }},

{ S_CLOSING,		E_RECV_BAD_CODE_PROTOCOL_REJECT, 				S_CLOSED,			{ A_this_layer_finish, 0 }},

{ S_CLOSING,		E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST, 		S_CLOSING,			{ 0 }},

/*---------------------------------------------------------------------------*/
{ S_STOPPING, 		E_DOWN,								     		S_STARTING, 	    { 0 }},

{ S_STOPPING, 		E_OPEN,								     		S_STOPPING, 	    { A_create_down_event, A_create_up_event, 0 }},

{ S_STOPPING, 		E_CLOSE,							     		S_CLOSING, 		    { 0 }},

{ S_STOPPING, 		E_TIMEOUT_COUNTER_POSITIVE,						S_STOPPING,		    { A_send_terminate_request, 0 }},

{ S_STOPPING, 		E_TIMEOUT_COUNTER_EXPIRED, 						S_STOPPED,		    { A_this_layer_finish, 0 }},

{ S_STOPPING,		E_RECV_GOOD_CONFIG_REQUEST,     				S_STOPPING,			{ 0 }},

{ S_STOPPING,		E_RECV_BAD_CONFIG_REQUEST,     					S_STOPPING,			{ 0 }},

{ S_STOPPING,		E_RECV_CONFIG_ACK, 								S_STOPPING,			{ 0 }},

{ S_STOPPING,		E_RECV_CONFIG_NAK_REJ, 							S_STOPPING,			{ 0 }},

{ S_STOPPING,		E_RECV_TERMINATE_REQUEST, 						S_STOPPING,			{ A_send_terminate_ack, 0 }},

{ S_STOPPING,		E_RECV_TERMINATE_ACK, 							S_STOPPED,			{ A_create_down_event, A_create_up_event, 0 }},

{ S_STOPPING,		E_RECV_UNKNOWN_CODE, 							S_STOPPING,			{ A_send_code_reject, 0 }},

/* recv code/protocol reject when rejected value is acceptable, 
    such as a Code-Reject of an extended code, 
    or a Protocol-Reject of a NCP */ 
{ S_STOPPING,		E_RECV_GOOD_CODE_PROTOCOL_REJECT, 				S_STOPPING,			{ 0 }},

{ S_STOPPING,		E_RECV_BAD_CODE_PROTOCOL_REJECT, 				S_STOPPED,			{ A_this_layer_finish, 0 }},

{ S_STOPPING,		E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST, 		S_STOPPING,			{ 0 }},

/*---------------------------------------------------------------------------*/
{ S_REQUEST_SENT, 	E_DOWN,											S_STARTING, 		{ 0 }},

{ S_REQUEST_SENT, 	E_OPEN,											S_REQUEST_SENT, 	{ 0 }},

{ S_REQUEST_SENT, 	E_CLOSE,										S_CLOSING, 			{ A_init_restart_termin, A_send_terminate_request, 0 }},

{ S_REQUEST_SENT, 	E_TIMEOUT_COUNTER_POSITIVE,						S_REQUEST_SENT, 	{ A_send_config_request, 0 }},

/* may be with "PASSIVE" option, with this option, ppp will not exit but then just wait for a valid LCP packet from peer if there is not received from peer */
{ S_REQUEST_SENT, 	E_TIMEOUT_COUNTER_EXPIRED,						S_STOPPED, 			{ A_this_layer_finish, 0 }},

{ S_REQUEST_SENT, 	E_RECV_GOOD_CONFIG_REQUEST,						S_ACK_SENT,			{ A_send_config_ack, 0 }},

{ S_REQUEST_SENT, 	E_RECV_BAD_CONFIG_REQUEST,						S_REQUEST_SENT,		{ A_send_config_nak_rej, 0 }},

{ S_REQUEST_SENT, 	E_RECV_CONFIG_ACK,								S_ACK_RECEIVED,		{ A_init_restart_count, 0 }},

{ S_REQUEST_SENT, 	E_RECV_CONFIG_NAK_REJ,							S_REQUEST_SENT,		{ A_init_restart_config, A_send_config_request, 0 }},

{ S_REQUEST_SENT, 	E_RECV_TERMINATE_REQUEST,						S_REQUEST_SENT,		{ A_send_terminate_ack, 0 }},

{ S_REQUEST_SENT, 	E_RECV_TERMINATE_ACK,							S_REQUEST_SENT,		{ 0 }},

{ S_REQUEST_SENT, 	E_RECV_UNKNOWN_CODE,							S_REQUEST_SENT,		{ A_send_code_reject, 0 }},

{ S_REQUEST_SENT, 	E_RECV_GOOD_CODE_PROTOCOL_REJECT,				S_REQUEST_SENT,		{ 0 }},

{ S_REQUEST_SENT, 	E_RECV_BAD_CODE_PROTOCOL_REJECT,				S_STOPPED,			{ A_this_layer_finish, 0 }},

{ S_REQUEST_SENT, 	E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST, 		S_REQUEST_SENT,		{ 0 }},

/*---------------------------------------------------------------------------*/
{ S_ACK_RECEIVED, 	E_DOWN,											S_STARTING, 		{ 0 }},

{ S_ACK_RECEIVED, 	E_OPEN,											S_ACK_RECEIVED, 	{ 0 }},

{ S_ACK_RECEIVED, 	E_CLOSE,										S_CLOSING, 			{ A_init_restart_termin, A_send_terminate_request, 0 }},

{ S_ACK_RECEIVED, 	E_TIMEOUT_COUNTER_POSITIVE,						S_REQUEST_SENT, 	{ A_send_config_request, 0 }},

/* may be with "PASSIVE" option, with this option, ppp will not exit but then just wait for a valid LCP packet from peer if there is not received from peer */
{ S_ACK_RECEIVED, 	E_TIMEOUT_COUNTER_EXPIRED,						S_STOPPED, 			{ A_this_layer_finish, 0 }},

{ S_ACK_RECEIVED, 	E_RECV_GOOD_CONFIG_REQUEST,						S_OPENED,			{ A_send_config_ack, A_this_layer_up, 0 }},

{ S_ACK_RECEIVED, 	E_RECV_BAD_CONFIG_REQUEST,						S_ACK_RECEIVED,		{ A_send_config_nak_rej, 0 }},

/* we should silently discard invalid ack/nak/rej packets and not affect transistions of the automaton 
 * so we just send a configure request packet and do nothing 
 * note: in RFC 1661 it rules we whould log this packet because it`s impossible that a correctly formed packet
         will arrive through a coincidentally-timed cross-connection, but we will skip to log in our implementation
 */
{ S_ACK_RECEIVED, 	E_RECV_CONFIG_ACK,								S_REQUEST_SENT,		{ A_send_config_request, 0 }},

{ S_ACK_RECEIVED, 	E_RECV_CONFIG_NAK_REJ,							S_REQUEST_SENT,		{ A_send_config_request, 0 }},

{ S_ACK_RECEIVED, 	E_RECV_TERMINATE_REQUEST,						S_REQUEST_SENT,		{ A_send_terminate_ack, 0 }},

{ S_ACK_RECEIVED, 	E_RECV_TERMINATE_ACK,							S_REQUEST_SENT,		{ 0 }},

{ S_ACK_RECEIVED, 	E_RECV_UNKNOWN_CODE,							S_ACK_RECEIVED,		{ A_send_code_reject, 0 }},

{ S_ACK_RECEIVED, 	E_RECV_GOOD_CODE_PROTOCOL_REJECT,				S_REQUEST_SENT,		{ 0 }},

{ S_ACK_RECEIVED, 	E_RECV_BAD_CODE_PROTOCOL_REJECT,				S_STOPPED,			{ A_this_layer_finish, 0 }},

{ S_ACK_RECEIVED, 	E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST, 		S_ACK_RECEIVED,		{ 0 }},

/*---------------------------------------------------------------------------*/
{ S_ACK_SENT, 		E_DOWN,											S_STARTING, 		{ 0 }},

{ S_ACK_SENT, 		E_OPEN,											S_ACK_SENT, 		{ 0 }},

{ S_ACK_SENT, 		E_CLOSE,										S_CLOSING, 			{ A_init_restart_termin, A_send_terminate_request, 0 }},

{ S_ACK_SENT, 		E_TIMEOUT_COUNTER_POSITIVE,						S_ACK_SENT, 		{ A_send_config_request, 0 }},

/* may be with "PASSIVE" option, with this option, ppp will not exit but then just wait for a valid LCP packet from peer if there is not received from peer */
{ S_ACK_SENT, 		E_TIMEOUT_COUNTER_EXPIRED,						S_STOPPED, 			{ A_this_layer_finish, 0 }},

{ S_ACK_SENT, 		E_RECV_GOOD_CONFIG_REQUEST,						S_ACK_SENT,			{ A_send_config_ack, 0 }},

{ S_ACK_SENT, 		E_RECV_BAD_CONFIG_REQUEST,						S_REQUEST_SENT,		{ A_send_config_nak_rej, 0 }},

{ S_ACK_SENT, 		E_RECV_CONFIG_ACK,								S_OPENED,			{ A_init_restart_count, A_this_layer_up, 0 }},

{ S_ACK_SENT, 		E_RECV_CONFIG_NAK_REJ,							S_ACK_SENT,			{ A_init_restart_config, A_send_config_request, 0 }},

{ S_ACK_SENT, 		E_RECV_TERMINATE_REQUEST,						S_REQUEST_SENT,		{ A_send_terminate_ack, 0 }},

{ S_ACK_SENT, 		E_RECV_TERMINATE_ACK,							S_ACK_SENT,			{ 0 }},

{ S_ACK_SENT, 		E_RECV_UNKNOWN_CODE,							S_ACK_SENT,			{ A_send_code_reject, 0 }},

{ S_ACK_SENT, 		E_RECV_GOOD_CODE_PROTOCOL_REJECT,				S_ACK_SENT,			{ 0 }},

{ S_ACK_SENT, 		E_RECV_BAD_CODE_PROTOCOL_REJECT,				S_STOPPED,			{ A_this_layer_finish, 0 }},

{ S_ACK_SENT, 		E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST, 		S_ACK_SENT,			{ 0 }},

/*---------------------------------------------------------------------------*/
{ S_OPENED, 		E_DOWN,											S_STARTING, 		{ A_this_layer_down, 0 }},

{ S_OPENED, 		E_OPEN,											S_OPENED, 			{ A_create_down_event, A_create_up_event, 0 }},

{ S_OPENED, 		E_CLOSE,										S_CLOSING, 			{ A_this_layer_down, A_init_restart_termin, A_send_terminate_request, 0 }},

{ S_OPENED, 		E_RECV_GOOD_CONFIG_REQUEST,						S_ACK_SENT,			{ A_this_layer_down, A_send_config_request, A_send_config_ack, 0 }},

{ S_OPENED, 		E_RECV_BAD_CONFIG_REQUEST,						S_REQUEST_SENT,		{ A_this_layer_down, A_send_config_request, A_send_config_nak_rej, 0 }},

/* we should silently discard invalid ack/nak/rej packets and not affect transistions of the automaton 
 * so we just send a configure request packet and do nothing 
 * note: in RFC 1661 it rules we whould log this packet because it`s impossible that a correctly formed packet
         will arrive through a coincidentally-timed cross-connection, but we will skip to log in our implementation
 */
{ S_OPENED, 		E_RECV_CONFIG_ACK,								S_REQUEST_SENT,		{ A_this_layer_down, A_send_config_request, 0 }},

{ S_OPENED, 		E_RECV_CONFIG_NAK_REJ,							S_REQUEST_SENT,		{ A_this_layer_down, A_send_config_request, 0 }},

{ S_OPENED, 		E_RECV_TERMINATE_REQUEST,						S_STOPPING,			{ A_this_layer_down, A_init_restart_termin, A_send_terminate_request, A_send_terminate_ack, 0 }},

{ S_OPENED, 		E_RECV_TERMINATE_ACK,							S_REQUEST_SENT,		{ A_this_layer_down, A_send_config_request, 0 }},

{ S_OPENED, 		E_RECV_UNKNOWN_CODE,							S_OPENED,			{ A_send_code_reject, 0 }},

{ S_OPENED, 		E_RECV_GOOD_CODE_PROTOCOL_REJECT,				S_OPENED,			{ 0 }},

{ S_OPENED, 		E_RECV_BAD_CODE_PROTOCOL_REJECT,				S_STOPPING,			{ A_this_layer_down, A_init_restart_termin, A_send_terminate_request, 0 }},

{ S_OPENED, 		E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST,		S_OPENED,			{ A_send_echo_reply, 0 }},

{ S_INVLD, 0, 0, {0}}
};

tPPP_STATE_TBL ppp_fsm_tbl_ncp[] = {
{ S_INIT,		  	E_UP,     							    		S_CLOSED,		    { 0 }},

/* these actions are "this layer start" (tls) and should retern "UP" event */
{ S_INIT,		  	E_OPEN,     						    		S_STARTING,		    { A_this_layer_start, 0 }},

{ S_INIT, 			E_CLOSE,							      		S_INIT, 		    { 0 }},	                                                      	  

/*---------------------------------------------------------------------------*/
{ S_STARTING,		E_UP,     							    		S_REQUEST_SENT,	  	{ A_send_config_request, A_init_restart_config, 0 }},

{ S_STARTING,		E_OPEN,    							    		S_STARTING,		    { 0 }},

/* these actions are "this layer finish" (tlf) and should retern "DOWN" event */
{ S_STARTING,		E_CLOSE,    						    		S_INIT,			    { A_this_layer_finish, 0 }},

/*---------------------------------------------------------------------------*/
{ S_CLOSED,			E_DOWN, 							      		S_INIT,			    { A_create_close_to_lower_layer, 0 }},

{ S_CLOSED,			E_OPEN, 							      		S_REQUEST_SENT,	  	{ A_send_config_request, A_init_restart_config, 0 }},

{ S_CLOSED,			E_CLOSE, 							      		S_CLOSED,		    { 0 }},

{ S_CLOSED,			E_RECV_GOOD_CONFIG_REQUEST, 					S_CLOSED,		    { A_send_terminate_ack, 0 }},

{ S_CLOSED,			E_RECV_BAD_CONFIG_REQUEST, 						S_CLOSED,		    { A_send_terminate_ack, 0 }},

{ S_CLOSED,			E_RECV_CONFIG_ACK, 								S_CLOSED,		    { A_send_terminate_ack, 0 }},

{ S_CLOSED,			E_RECV_CONFIG_NAK_REJ, 							S_CLOSED,		    { A_send_terminate_ack, 0 }},

{ S_CLOSED,			E_RECV_TERMINATE_REQUEST, 						S_CLOSED,		    { A_send_terminate_ack, 0 }},

{ S_CLOSED,			E_RECV_TERMINATE_ACK, 							S_CLOSED,		    { 0 }},

{ S_CLOSED,			E_RECV_UNKNOWN_CODE, 							S_CLOSED,		    { A_send_code_reject, 0 }},

/* recv code/protocol reject when rejected value is acceptable, 
    such as a Code-Reject of an extended code, 
    or a Protocol-Reject of a NCP */ 
{ S_CLOSED,			E_RECV_GOOD_CODE_PROTOCOL_REJECT, 				S_CLOSED,	  		{ 0 }},

{ S_CLOSED,			E_RECV_BAD_CODE_PROTOCOL_REJECT, 				S_CLOSED,			{ A_this_layer_finish, 0 }},

{ S_CLOSED,			E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST, 		S_CLOSED,			{ 0 }},

/*---------------------------------------------------------------------------*/
{ S_STOPPED,		E_DOWN,     						   			S_STARTING,		    { A_this_layer_start, 0 }},

{ S_STOPPED,		E_OPEN,     						   			S_STOPPED,		    { A_create_down_event, A_create_up_event, 0 }},

{ S_STOPPED,		E_CLOSE,     						   			S_CLOSED,		    { 0 }},

{ S_STOPPED,		E_RECV_GOOD_CONFIG_REQUEST,						S_ACK_SENT,		    { A_init_restart_config, A_send_config_request, A_send_config_ack, 0 }},

{ S_STOPPED,		E_RECV_BAD_CONFIG_REQUEST, 						S_REQUEST_SENT,	  	{ A_init_restart_config, A_send_config_request, A_send_config_nak_rej, 0 }},

{ S_STOPPED,		E_RECV_CONFIG_ACK, 				 				S_STOPPED,		    { A_send_terminate_ack, 0 }},

{ S_STOPPED,		E_RECV_CONFIG_NAK_REJ, 		 					S_STOPPED,		    { A_send_terminate_ack, 0 }},

{ S_STOPPED,		E_RECV_TERMINATE_REQUEST,  						S_STOPPED,		    { A_send_terminate_ack, 0 }},

{ S_STOPPED,		E_RECV_TERMINATE_ACK, 		 					S_STOPPED,		    { 0 }},

{ S_STOPPED,		E_RECV_UNKNOWN_CODE, 			 				S_STOPPED,		    { A_send_code_reject, 0 }},

/* recv code/protocol reject when rejected value is acceptable, 
    such as a Code-Reject of an extended code, 
    or a Protocol-Reject of a NCP */ 
{ S_STOPPED,		E_RECV_GOOD_CODE_PROTOCOL_REJECT, 				S_STOPPED,			{ 0 }},

{ S_STOPPED,		E_RECV_BAD_CODE_PROTOCOL_REJECT, 				S_STOPPED,			{ A_this_layer_finish, 0 }},

{ S_STOPPED,		E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST, 		S_STOPPED,			{ 0 }},

/*---------------------------------------------------------------------------*/
{ S_CLOSING, 		E_DOWN,								     		S_INIT,			    { 0 }},

{ S_CLOSING, 		E_OPEN,								     		S_STOPPING,		    { A_create_down_event, A_create_up_event, 0 }},

{ S_CLOSING, 		E_CLOSE,							     		S_CLOSING,		    { 0 }},

{ S_CLOSING, 		E_TIMEOUT_COUNTER_POSITIVE,						S_CLOSING,		    { A_send_terminate_request, 0 }},

{ S_CLOSING, 		E_TIMEOUT_COUNTER_EXPIRED, 						S_CLOSED,		    { A_this_layer_finish, 0 }},

{ S_CLOSING,		E_RECV_GOOD_CONFIG_REQUEST,						S_CLOSING,		    { 0 }},

{ S_CLOSING,		E_RECV_BAD_CONFIG_REQUEST, 						S_CLOSING,		    { 0 }},

{ S_CLOSING,		E_RECV_CONFIG_ACK, 				 				S_CLOSING,		    { 0 }},

{ S_CLOSING,		E_RECV_CONFIG_NAK_REJ, 		 					S_CLOSING,		    { 0 }},

{ S_CLOSING,		E_RECV_TERMINATE_REQUEST,  						S_CLOSING,		    { A_send_terminate_ack, 0 }},

{ S_CLOSING,		E_RECV_TERMINATE_ACK, 		 					S_CLOSED,		    { A_create_down_event, A_create_up_event, 0 }},

{ S_CLOSING,		E_RECV_UNKNOWN_CODE, 			 				S_CLOSING,		    { A_send_code_reject, 0 }},

/* recv code/protocol reject when rejected value is acceptable, 
    such as a Code-Reject of an extended code, 
    or a Protocol-Reject of a NCP */ 
{ S_CLOSING,		E_RECV_GOOD_CODE_PROTOCOL_REJECT, 				S_CLOSING,  		{ 0 }},

{ S_CLOSING,		E_RECV_BAD_CODE_PROTOCOL_REJECT, 				S_CLOSED,			{ A_this_layer_finish, 0 }},

{ S_CLOSING,		E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST, 		S_CLOSING,			{ 0 }},

/*---------------------------------------------------------------------------*/
{ S_STOPPING, 		E_DOWN,								     		S_STARTING, 	    { 0 }},

{ S_STOPPING, 		E_OPEN,								     		S_STOPPING, 	    { A_create_down_event, A_create_up_event, 0 }},

{ S_STOPPING, 		E_CLOSE,							     		S_CLOSING, 		    { 0 }},

{ S_STOPPING, 		E_TIMEOUT_COUNTER_POSITIVE,						S_STOPPING,		    { A_send_terminate_request, 0 }},

{ S_STOPPING, 		E_TIMEOUT_COUNTER_EXPIRED, 						S_STOPPED,		    { A_this_layer_finish, 0 }},

{ S_STOPPING,		E_RECV_GOOD_CONFIG_REQUEST,     				S_STOPPING,			{ 0 }},

{ S_STOPPING,		E_RECV_BAD_CONFIG_REQUEST,     					S_STOPPING,			{ 0 }},

{ S_STOPPING,		E_RECV_CONFIG_ACK, 								S_STOPPING,			{ 0 }},

{ S_STOPPING,		E_RECV_CONFIG_NAK_REJ, 							S_STOPPING,			{ 0 }},

{ S_STOPPING,		E_RECV_TERMINATE_REQUEST, 						S_STOPPING,			{ A_send_terminate_ack, 0 }},

{ S_STOPPING,		E_RECV_TERMINATE_ACK, 							S_STOPPED,			{ A_create_down_event, A_create_up_event, 0 }},

{ S_STOPPING,		E_RECV_UNKNOWN_CODE, 							S_STOPPING,			{ A_send_code_reject, 0 }},

/* recv code/protocol reject when rejected value is acceptable, 
    such as a Code-Reject of an extended code, 
    or a Protocol-Reject of a NCP */ 
{ S_STOPPING,		E_RECV_GOOD_CODE_PROTOCOL_REJECT, 				S_STOPPING,			{ 0 }},

{ S_STOPPING,		E_RECV_BAD_CODE_PROTOCOL_REJECT, 				S_STOPPED,			{ A_this_layer_finish, 0 }},

{ S_STOPPING,		E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST, 		S_STOPPING,			{ 0 }},

/*---------------------------------------------------------------------------*/
{ S_REQUEST_SENT, 	E_DOWN,											S_STARTING, 		{ 0 }},

{ S_REQUEST_SENT, 	E_OPEN,											S_REQUEST_SENT, 	{ 0 }},

{ S_REQUEST_SENT,	E_CLOSE,										S_CLOSING, 			{ A_init_restart_termin, A_send_terminate_request, 0 }},

{ S_REQUEST_SENT, 	E_TIMEOUT_COUNTER_POSITIVE,						S_REQUEST_SENT, 	{ A_send_config_request, 0 }},

/* may be with "PASSIVE" option, with this option, ppp will not exit but then just wait for a valid LCP packet from peer if there is not received form peer */
{ S_REQUEST_SENT,   E_TIMEOUT_COUNTER_EXPIRED,		                S_STOPPED, 		    { A_this_layer_finish, 0 }},

{ S_REQUEST_SENT,   E_RECV_GOOD_CONFIG_REQUEST,		                S_ACK_SENT,		    { A_send_config_ack, 0 }},

{ S_REQUEST_SENT,   E_RECV_BAD_CONFIG_REQUEST,		                S_REQUEST_SENT,	    { A_send_config_nak_rej, 0 }},

{ S_REQUEST_SENT,   E_RECV_CONFIG_ACK,				                S_ACK_RECEIVED,	    { A_init_restart_count, 0 }},

{ S_REQUEST_SENT,   E_RECV_CONFIG_NAK_REJ,			                S_REQUEST_SENT,	    { A_init_restart_config, A_send_config_request, 0 }},

{ S_REQUEST_SENT,   E_RECV_TERMINATE_REQUEST,			            S_REQUEST_SENT,	    { A_send_terminate_ack, 0 }},

{ S_REQUEST_SENT,   E_RECV_TERMINATE_ACK,				            S_REQUEST_SENT,	    { 0 }},

{ S_REQUEST_SENT,   E_RECV_UNKNOWN_CODE,				            S_REQUEST_SENT,	    { A_send_code_reject, 0 }},

{ S_REQUEST_SENT,   E_RECV_GOOD_CODE_PROTOCOL_REJECT,	            S_REQUEST_SENT,	    { 0 }},

{ S_REQUEST_SENT,   E_RECV_BAD_CODE_PROTOCOL_REJECT,	            S_STOPPED,		    { A_this_layer_finish, 0 }},

{ S_REQUEST_SENT,   E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST,      S_REQUEST_SENT,	    { 0 }},

/*---------------------------------------------------------------------------*/
{ S_ACK_RECEIVED,   E_DOWN,							                S_STARTING, 	    { 0 }},

{ S_ACK_RECEIVED,   E_OPEN,							                S_ACK_RECEIVED,     { 0 }},

{ S_ACK_RECEIVED,   E_CLOSE,							            S_CLOSING, 		    { A_init_restart_termin, A_send_terminate_request, 0 }},

{ S_ACK_RECEIVED,   E_TIMEOUT_COUNTER_POSITIVE,		                S_REQUEST_SENT,     { A_send_config_request, 0 }},

/* may be with "PASSIVE" option, with this option, ppp will not exit but then just wait for a valid LCP packet from peer if there is not received form peer */
{ S_ACK_RECEIVED,   E_TIMEOUT_COUNTER_EXPIRED,		                S_STOPPED, 		    { A_this_layer_finish, 0 }},

{ S_ACK_RECEIVED,   E_RECV_GOOD_CONFIG_REQUEST,		                S_OPENED,		    { A_send_config_ack, A_this_layer_up, 0 }},

{ S_ACK_RECEIVED,   E_RECV_BAD_CONFIG_REQUEST,		                S_ACK_RECEIVED,	    { A_send_config_nak_rej, 0 }},

/* we should silently discard invalid ack/nak/rej packets and not affect transistions of the automaton 
 * so we just send a configure request packet and do nothing 
 * note: in RFC 1661 it rules we whould log this packet because it`s impossible that a correctly formed packet
         will arrive through a coincidentally-timed cross-connection, but we will skip to log in our implementation
 */
{ S_ACK_RECEIVED,   E_RECV_CONFIG_ACK,				                S_REQUEST_SENT,	    { A_send_config_request, 0 }},

{ S_ACK_RECEIVED,   E_RECV_CONFIG_NAK_REJ,			                S_REQUEST_SENT,	    { A_send_config_request, 0 }},

{ S_ACK_RECEIVED,   E_RECV_TERMINATE_REQUEST,			            S_REQUEST_SENT,	    { A_send_terminate_ack, 0 }},

{ S_ACK_RECEIVED,   E_RECV_TERMINATE_ACK,				            S_REQUEST_SENT,	    { 0 }},

{ S_ACK_RECEIVED,   E_RECV_UNKNOWN_CODE,				            S_ACK_RECEIVED,	    { A_send_code_reject, 0 }},

{ S_ACK_RECEIVED,   E_RECV_GOOD_CODE_PROTOCOL_REJECT,	            S_REQUEST_SENT,	    { 0 }},

{ S_ACK_RECEIVED,   E_RECV_BAD_CODE_PROTOCOL_REJECT,	            S_STOPPED,		    { A_this_layer_finish, 0 }},

{ S_ACK_RECEIVED,   E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST,      S_ACK_RECEIVED,	    { 0 }},

/*---------------------------------------------------------------------------*/
{ S_ACK_SENT, 	    E_DOWN,								            S_STARTING, 	    { 0 }},

{ S_ACK_SENT, 	    E_OPEN,								            S_ACK_SENT, 	    { 0 }},

{ S_ACK_SENT, 	    E_CLOSE,							            S_CLOSING, 		    { A_init_restart_termin, A_send_terminate_request, 0 }},

{ S_ACK_SENT, 	    E_TIMEOUT_COUNTER_POSITIVE,			            S_ACK_SENT, 	    { A_send_config_request, 0 }},

/* may be with "PASSIVE" option, with this option, ppp will not exit but then just wait for a valid LCP packet from peer if there is not received form peer */
{ S_ACK_SENT, 	    E_TIMEOUT_COUNTER_EXPIRED,			            S_STOPPED, 		    { A_this_layer_finish, 0 }},

{ S_ACK_SENT, 	    E_RECV_GOOD_CONFIG_REQUEST,			            S_ACK_SENT,		    { A_send_config_ack, 0 }},

{ S_ACK_SENT, 	    E_RECV_BAD_CONFIG_REQUEST,			            S_REQUEST_SENT,	    { A_send_config_nak_rej, 0 }},

{ S_ACK_SENT, 	    E_RECV_CONFIG_ACK,					            S_OPENED,		    { A_init_restart_count, A_this_layer_up, 0 }},

{ S_ACK_SENT, 	    E_RECV_CONFIG_NAK_REJ,				            S_ACK_SENT,		    { A_init_restart_config, A_send_config_request, 0 }},

{ S_ACK_SENT, 	    E_RECV_TERMINATE_REQUEST,			            S_REQUEST_SENT,	    { A_send_terminate_ack, 0 }},

{ S_ACK_SENT, 	    E_RECV_TERMINATE_ACK,				            S_ACK_SENT,		    { 0 }},

{ S_ACK_SENT, 	    E_RECV_UNKNOWN_CODE,				            S_ACK_SENT,		    { A_send_code_reject, 0 }},

{ S_ACK_SENT, 	    E_RECV_GOOD_CODE_PROTOCOL_REJECT,	            S_ACK_SENT,		    { 0 }},

{ S_ACK_SENT, 	    E_RECV_BAD_CODE_PROTOCOL_REJECT,	            S_STOPPED,		    { A_this_layer_finish, 0 }},

{ S_ACK_SENT, 	    E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST,      S_ACK_SENT,	        { 0 }},

/*---------------------------------------------------------------------------*/
{ S_OPENED, 	    E_DOWN,								            S_STARTING, 	    { A_this_layer_down, 0 }},

{ S_OPENED, 	    E_OPEN,								            S_OPENED, 		    { A_create_down_event, A_create_up_event, 0 }},

{ S_OPENED, 	    E_CLOSE,							            S_CLOSING, 		    { A_this_layer_down, A_init_restart_termin, A_send_terminate_request, 0 }},

{ S_OPENED, 	    E_RECV_GOOD_CONFIG_REQUEST,			            S_ACK_SENT,		    { A_this_layer_down, A_send_config_request, A_send_config_ack, 0 }},

{ S_OPENED, 	    E_RECV_BAD_CONFIG_REQUEST,			            S_REQUEST_SENT,	    { A_this_layer_down, A_send_config_request, A_send_config_nak_rej, 0 }},

/* we should silently discard invalid ack/nak/rej packets and not affect transistions of the automaton 
 * so we just send a configure request packet and do nothing 
 * note: in RFC 1661 it rules we whould log this packet because it`s impossible that a correctly formed packet
         will arrive through a coincidentally-timed cross-connection, but we will skip to log in our implementation
 */
{ S_OPENED, 	    E_RECV_CONFIG_ACK,					            S_REQUEST_SENT,	    { A_this_layer_down, A_send_config_request, 0 }},

{ S_OPENED, 	    E_RECV_CONFIG_NAK_REJ,				            S_REQUEST_SENT,	    { A_this_layer_down, A_send_config_request, 0 }},

{ S_OPENED, 	    E_RECV_TERMINATE_REQUEST,			            S_STOPPING,		    { A_this_layer_down, A_zero_restart_count, A_send_terminate_ack, 0 }},

{ S_OPENED, 	    E_RECV_TERMINATE_ACK,				            S_REQUEST_SENT,	    { A_this_layer_down, A_send_config_request, 0 }},

{ S_OPENED, 	    E_RECV_UNKNOWN_CODE,				            S_OPENED,		    { A_send_code_reject, 0 }},

{ S_OPENED, 	    E_RECV_GOOD_CODE_PROTOCOL_REJECT,	            S_OPENED,		    { 0 }},

{ S_OPENED, 	    E_RECV_BAD_CODE_PROTOCOL_REJECT,	            S_STOPPING,		    { A_this_layer_down, A_init_restart_termin, A_send_terminate_request, 0 }},

{ S_OPENED, 	    E_RECV_ECHO_REPLY_REQUEST_DISCARD_REQUEST,      S_OPENED,           { A_send_echo_reply, 0 }},

{ S_INVLD, 0, 0, {0}}
};

tPPP_STATE_TBL *ppp_fsm_tbl[] = {
    ppp_fsm_tbl_lcp,
    ppp_fsm_tbl_ncp
};

STATUS PPP_FSM(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb, U16 event)
{	
    int i;
    char *str1, *str2;

    if (!s_ppp_ccb)
        return ERROR;

    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    /* Find a matched state */
    for(i=0; ppp_fsm_tbl[s_ppp_ccb->cp][i].state!=S_INVLD; i++)
        if (ppp_fsm_tbl[s_ppp_ccb->cp][i].state == s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].state)
            break;

    FastRG_LOG(DBG, fastrg_ccb->fp, (U8 *)s_ppp_ccb, PPPLOGMSG, "Current state is %s, event is %s", PPP_state2str(ppp_fsm_tbl[s_ppp_ccb->cp][i].state), PPP_event2str(event));

    if (ppp_fsm_tbl[s_ppp_ccb->cp][i].state == S_INVLD) {
        FastRG_LOG(ERR, fastrg_ccb->fp, (U8 *)s_ppp_ccb, PPPLOGMSG, "Error! user %" PRIu16 " unknown state(%d) specified for the event(%d), current ppp fsm state is %s",
        	s_ppp_ccb->user_num, s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].state,event, PPP_state2str(ppp_fsm_tbl[s_ppp_ccb->cp][i].state));
        return ERROR;
    }

    /**
     * Find a matched event in a specific state.
     * Note : a state can accept several events.
     */
    for(;ppp_fsm_tbl[s_ppp_ccb->cp][i].state==s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].state; i++)
        if (ppp_fsm_tbl[s_ppp_ccb->cp][i].event == event)
            break;

    if (ppp_fsm_tbl[s_ppp_ccb->cp][i].state != s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].state) { /* search until meet the next state */
        FastRG_LOG(INFO, fastrg_ccb->fp, (U8 *)s_ppp_ccb, PPPLOGMSG, "Error! user %" PRIu16 " invalid event(%s) in state(%s)",
            s_ppp_ccb->user_num, PPP_event2str(event), PPP_state2str(s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].state));
  		return SUCCESS; /* still pass to endpoint */
    }

    /* Correct state found */
    if (s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].state != ppp_fsm_tbl[s_ppp_ccb->cp][i].next_state) {
        str1 = PPP_state2str(s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].state);
        str2 = PPP_state2str(ppp_fsm_tbl[s_ppp_ccb->cp][i].next_state);
        FastRG_LOG(DBG, fastrg_ccb->fp, (U8 *)s_ppp_ccb, PPPLOGMSG,"User %" 
            PRIu16 " %s state changed from %s to %s.", s_ppp_ccb->user_num, 
            (s_ppp_ccb->cp == 1 ? "IPCP" : "LCP"), str1, str2);
        s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].state = ppp_fsm_tbl[s_ppp_ccb->cp][i].next_state;
    }

    /* Execute actions */
    for(int j=0; ppp_fsm_tbl[s_ppp_ccb->cp][i].hdl[j]; j++) {
    	s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].timer_counter = 10;
#ifndef UNIT_TEST // in unit test, we do not execute the action functions to avoid complex dependencies
       	if ((*ppp_fsm_tbl[s_ppp_ccb->cp][i].hdl[j])(ppp_timer, s_ppp_ccb) == ERROR)  
            return ERROR;
#endif
    }

    return SUCCESS;
}

/* this layer up/down/start/finish */
STATUS A_this_layer_start(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    PPP_FSM(ppp_timer, s_ppp_ccb, E_UP);

    return SUCCESS;
}

STATUS A_this_layer_finish(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    PPP_FSM(ppp_timer, s_ppp_ccb, E_DOWN);

    return SUCCESS;
}

STATUS A_this_layer_up(__attribute__((unused)) struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    unsigned char buffer[PPP_MSG_BUF_LEN];
    U16 mulen = 0;
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    if (s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].ppp_payload.ppp_protocol == rte_cpu_to_be_16(LCP_PROTOCOL)) {
        memset(buffer,0,PPP_MSG_BUF_LEN);
        rte_timer_reset(&(s_ppp_ccb->ppp_alive), ppp_interval*fastrg_get_cycles_in_sec(), 
            SINGLE, fastrg_ccb->lcore.ctrl_thread, (rte_timer_cb_t)PPP_bye_timer_cb, s_ppp_ccb);
        if (s_ppp_ccb->auth_method == PAP_PROTOCOL)
            build_auth_request_pap(buffer, &mulen, s_ppp_ccb);
        wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);
        FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " LCP connection establish successfully.", s_ppp_ccb->user_num);
        FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " starting Authentication.", s_ppp_ccb->user_num);
    } else if (s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].ppp_payload.ppp_protocol == rte_cpu_to_be_16(IPCP_PROTOCOL)) {
        rte_atomic16_set(&s_ppp_ccb->dp_start_bool, (BIT16)1);
        s_ppp_ccb->phase = DATA_PHASE;
        rte_timer_reset(&(s_ppp_ccb->nat), fastrg_get_cycles_in_sec(), PERIODICAL, 
            fastrg_ccb->lcore.ctrl_thread, (rte_timer_cb_t)nat_rule_timer, s_ppp_ccb);
        FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " IPCP connection establish successfully.", s_ppp_ccb->user_num);
        FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "Now user %" PRIu16 " can start to send data via pppoe session id 0x%x and vlan is %" 
            PRIu16 ".\n", s_ppp_ccb->user_num, rte_be_to_cpu_16(s_ppp_ccb->session_id), rte_atomic16_read(&s_ppp_ccb->vlan_id));
        FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " PPPoE client IP address is %" PRIu8 ".%" PRIu8 ".%" PRIu8 ".%" PRIu8 
            ", PPPoE server IP address is %" PRIu8 ".%" PRIu8 ".%" PRIu8 ".%" PRIu8 "\n", s_ppp_ccb->user_num, 
            *(((U8 *)&(s_ppp_ccb->hsi_ipv4))), *(((U8 *)&(s_ppp_ccb->hsi_ipv4))+1), 
            *(((U8 *)&(s_ppp_ccb->hsi_ipv4))+2), *(((U8 *)&(s_ppp_ccb->hsi_ipv4))+3), 
            *(((U8 *)&(s_ppp_ccb->hsi_ipv4_gw))), *(((U8 *)&(s_ppp_ccb->hsi_ipv4_gw))+1), 
            *(((U8 *)&(s_ppp_ccb->hsi_ipv4_gw))+2), *(((U8 *)&(s_ppp_ccb->hsi_ipv4_gw))+3));
        if (s_ppp_ccb->hsi_primary_dns == 0xffffffff || s_ppp_ccb->hsi_primary_dns == 0x0)
            s_ppp_ccb->hsi_primary_dns = rte_cpu_to_be_32(0x08080808); /* 8.8.8.8 */
        if (s_ppp_ccb->hsi_secondary_dns == 0xffffffff || s_ppp_ccb->hsi_secondary_dns == 0x0)
            s_ppp_ccb->hsi_secondary_dns = rte_cpu_to_be_32(0x01010101); /* 1.1.1.1 */
        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, s_ppp_ccb->user_num - 1);
        if (dns_proxy_init(&dhcp_ccb->dns_state, s_ppp_ccb->hsi_primary_dns, 
                s_ppp_ccb->hsi_secondary_dns) != SUCCESS) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                "User %u: Failed to init DNS proxy", s_ppp_ccb->user_num);
            return ERROR;
        }
        FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " HSI module is spawned.\n", s_ppp_ccb->user_num);
        if (fastrg_ccb->is_standalone == FALSE) {
            char user_id_str[6];
            snprintf(user_id_str, sizeof(user_id_str), "%u", s_ppp_ccb->user_num);
            int64_t revision = 0;
            etcd_mark_pending_event(HSI_ACTION_UPDATE, s_ppp_ccb->user_num - 1);
            if (etcd_client_modify_hsi_config_status(fastrg_ccb->node_uuid, user_id_str, 
                    ENABLE_STATUS_ENABLED, &revision) == ETCD_SUCCESS) {
                etcd_confirm_pending_event(HSI_ACTION_UPDATE, s_ppp_ccb->user_num - 1, revision);
            } else {
                etcd_remove_event(HSI_ACTION_UPDATE, s_ppp_ccb->user_num - 1);
            }
            etcd_client_load_dns_records(fastrg_ccb->node_uuid, user_id_str,
                dns_record_changed_callback, fastrg_ccb);
        }
    }

    return SUCCESS;
}

/***********************************************************************
 * A_this_layer_down
 *
 * purpose : To notify upper layer this layer is leaving OPEN state.
 * input   : ppp - timer
 *			 s_ppp_ccb - user connection info.
 *           event -
 * return  : error status
 ***********************************************************************/
STATUS A_this_layer_down(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    if (s_ppp_ccb->cp == 1) {
        FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "IPCP layer is down");
        PPP_FSM(ppp_timer, s_ppp_ccb, E_CLOSE);
        rte_atomic16_set(&s_ppp_ccb->dp_start_bool, (BIT16)0);
    } else if (s_ppp_ccb->cp == 0) {
        PPP_FSM(ppp_timer, s_ppp_ccb, E_CLOSE);
        FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "LCP layer is down");
    }

    return SUCCESS;
}

STATUS A_init_restart_count(__attribute__((unused)) struct rte_timer *ppp_timer, __attribute__((unused)) ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "init restart count");

    return SUCCESS;
}

STATUS A_init_restart_config(struct rte_timer *ppp_timer, __attribute__((unused)) ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " init config req timer start.\n", s_ppp_ccb->user_num);
    rte_timer_stop(ppp_timer);
    s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].timer_counter = 9;
    rte_timer_reset(ppp_timer, 3*fastrg_get_cycles_in_sec(), PERIODICAL, fastrg_ccb->lcore.ctrl_thread, 
        (rte_timer_cb_t)A_send_config_request, s_ppp_ccb);

    return SUCCESS;
}

STATUS A_init_restart_termin(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " init termin req timer start.\n", s_ppp_ccb->user_num);
    rte_timer_stop(ppp_timer);
    s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].timer_counter = 9;
    rte_timer_reset(ppp_timer, 3*fastrg_get_cycles_in_sec(), PERIODICAL, fastrg_ccb->lcore.ctrl_thread, 
        (rte_timer_cb_t)A_send_terminate_request, s_ppp_ccb);

    return SUCCESS;
}

STATUS A_send_config_request(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    unsigned char buffer[PPP_MSG_BUF_LEN];
    U16 mulen = 0;

    if (s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].timer_counter == 0) {
    	rte_timer_stop(ppp_timer);
        FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " config request timeout.\n", s_ppp_ccb->user_num);
    	PPP_FSM(ppp_timer, s_ppp_ccb, E_TIMEOUT_COUNTER_EXPIRED);
        return SUCCESS;
    }
    build_config_request(buffer, &mulen, s_ppp_ccb);
    wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);
    s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].timer_counter--;

    return SUCCESS;
}

STATUS A_send_config_nak_rej(__attribute__((unused)) struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    unsigned char buffer[PPP_MSG_BUF_LEN];
    U16 mulen = 0;

    build_config_nak_rej(buffer, &mulen, s_ppp_ccb);
    wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);

    return SUCCESS;
}

STATUS A_send_config_ack(__attribute__((unused)) struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    unsigned char buffer[PPP_MSG_BUF_LEN];
    U16 mulen = 0;

    build_config_ack(buffer, &mulen, s_ppp_ccb);
    wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);

    return SUCCESS;
}

STATUS A_send_terminate_request(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    unsigned char buffer[PPP_MSG_BUF_LEN];
    U16 mulen = 0;

    if (s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].timer_counter == 0) {
    	rte_timer_stop(ppp_timer);
        FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 " terminate request timeout.\n", s_ppp_ccb->user_num);
    	PPP_FSM(ppp_timer, s_ppp_ccb, E_TIMEOUT_COUNTER_EXPIRED);
        return SUCCESS;
    }
    build_terminate_request(buffer, &mulen, s_ppp_ccb);
    wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);
    s_ppp_ccb->ppp_phase[s_ppp_ccb->cp].timer_counter--;

    return SUCCESS;
}

STATUS A_send_terminate_ack(__attribute__((unused)) struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    unsigned char buffer[PPP_MSG_BUF_LEN];
    U16 mulen = 0;

    build_terminate_ack(buffer, &mulen, s_ppp_ccb);
    wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);

    return SUCCESS;
}

STATUS A_send_code_reject(__attribute__((unused)) struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    unsigned char buffer[PPP_MSG_BUF_LEN];
    U16 mulen = 0;

    if (build_code_reject(buffer,s_ppp_ccb,&mulen) < 0)
        return FALSE;
    wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);

    return SUCCESS;
}

STATUS A_send_echo_reply(__attribute__((unused)) struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    unsigned char buffer[PPP_MSG_BUF_LEN];
    U16 mulen = 0;

    build_echo_reply(buffer, &mulen, s_ppp_ccb);
    wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);

    return SUCCESS;
}

STATUS A_create_up_event(__attribute__((unused)) struct rte_timer *ppp_timer, __attribute__((unused)) ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "create up event");

    return SUCCESS;
}

STATUS A_create_down_event(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "create down event");

    PPP_FSM(ppp_timer, s_ppp_ccb, E_DOWN);

    return SUCCESS;
}

STATUS A_zero_restart_count(__attribute__((unused)) struct rte_timer *ppp_timer, __attribute__((unused)) ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    FastRG_LOG(DBG, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "zero restart count");

    return SUCCESS;
}

STATUS A_send_padt(__attribute__((unused)) struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    pppoe_send_pkt(ENCODE_PADT, s_ppp_ccb);
    s_ppp_ccb->phase = END_PHASE;

    return SUCCESS;
}

STATUS A_create_close_to_lower_layer(struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t *fastrg_ccb = s_ppp_ccb->fastrg_ccb;

    FastRG_LOG(INFO, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG, "User %" PRIu16 
        " notify lower layer to close connection.\n", s_ppp_ccb->user_num);
    s_ppp_ccb->cp = 0;
    s_ppp_ccb->phase -= 2;
    rte_timer_stop(&(s_ppp_ccb->ppp_alive));
    PPP_FSM(ppp_timer, s_ppp_ccb, E_CLOSE);

    return SUCCESS;
}

STATUS A_padi_timer_func(__attribute__((unused)) struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    return pppoe_send_pkt(ENCODE_PADI, s_ppp_ccb);
}

STATUS A_padr_timer_func(__attribute__((unused)) struct rte_timer *ppp_timer, ppp_ccb_t *s_ppp_ccb)
{
    return pppoe_send_pkt(ENCODE_PADR, s_ppp_ccb);
}
