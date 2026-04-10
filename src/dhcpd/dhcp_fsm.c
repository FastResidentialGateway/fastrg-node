#include <common.h>

#include <rte_ethdev.h>

#include "../dbg.h"
#include "../fastrg.h"
#include "dhcp_fsm.h"
#include "dhcpd.h"

STATUS A_send_dhcp_offer(dhcp_ccb_t *dhcp_ccb, U32 pool_index);
STATUS A_wait_request_timer(dhcp_ccb_t *dhcp_ccb, U32 pool_index);
STATUS A_send_dhcp_ack(dhcp_ccb_t *dhcp_ccb, U32 pool_index);
STATUS A_send_dhcp_nak(dhcp_ccb_t *dhcp_ccb, U32 pool_index);
STATUS A_wait_lease_timer(dhcp_ccb_t *dhcp_ccb, U32 pool_index);
STATUS A_release(dhcp_ccb_t *dhcp_ccb, U32 pool_index);
STATUS A_mark_ip_conflicted(dhcp_ccb_t *dhcp_ccb, U32 pool_index);
STATUS A_send_dhcp_ack_inform(dhcp_ccb_t *dhcp_ccb, U32 pool_index);

tDHCP_STATE_TBL  dhcp_fsm_tbl[] = { 
/*//////////////////////////////////////////////////////////////////////////////////
    STATE                   EVENT              NEXT-STATE                HANDLER       
///////////////////////////////////////////////////////////////////////////////////\*/
{ S_DHCP_INIT,           E_DISCOVER,         S_DHCP_OFFER_SENT,       { A_send_dhcp_offer, A_wait_request_timer, 0 }},

{ S_DHCP_INIT,           E_GOOD_REQUEST,     S_DHCP_ACK_SENT,         { A_send_dhcp_ack, A_wait_lease_timer, 0 }},

{ S_DHCP_INIT,           E_BAD_REQUEST,      S_DHCP_INIT,             { A_send_dhcp_nak, 0 }},

{ S_DHCP_INIT,           E_INFORM,           S_DHCP_INIT,             { A_send_dhcp_ack_inform, 0 }},

{ S_DHCP_OFFER_SENT,     E_DISCOVER,         S_DHCP_OFFER_SENT,       { A_send_dhcp_offer, A_wait_request_timer, 0 }},

{ S_DHCP_OFFER_SENT,     E_TIMEOUT,          S_DHCP_INIT,             { A_release, 0 }},

{ S_DHCP_OFFER_SENT,     E_GOOD_REQUEST,     S_DHCP_ACK_SENT,         { A_send_dhcp_ack, A_wait_lease_timer, 0 }},

{ S_DHCP_OFFER_SENT,     E_BAD_REQUEST,      S_DHCP_INIT,             { A_send_dhcp_nak, A_release, 0 }},

{ S_DHCP_ACK_SENT,       E_TIMEOUT,          S_DHCP_INIT,             { A_release, 0 }},

{ S_DHCP_ACK_SENT,       E_RELEASE,          S_DHCP_INIT,             { A_release, 0 }},

{ S_DHCP_ACK_SENT,       E_GOOD_REQUEST,     S_DHCP_ACK_SENT,         { A_send_dhcp_ack, A_wait_lease_timer, 0 }},

{ S_DHCP_ACK_SENT,       E_BAD_REQUEST,      S_DHCP_INIT,             { A_send_dhcp_nak, A_release, 0 }},

{ S_DHCP_ACK_SENT,       E_DISCOVER,         S_DHCP_OFFER_SENT,       { A_send_dhcp_offer, A_wait_request_timer, 0 }},

{ S_DHCP_ACK_SENT,       E_DECLINE,          S_DHCP_INIT,             { A_mark_ip_conflicted, 0 }},

{ S_DHCP_ACK_SENT,       E_INFORM,           S_DHCP_ACK_SENT,         { A_send_dhcp_ack_inform, 0 }},

{ S_DHCP_INVLD, 0, 0, {0}}

};

/***********************************************************************
 * dhcp_fsm
 *
 * purpose : finite state machine.
 * input   : dhcp_timer - timer
 *			 dhcp_ccb - user connection info.
 *           event -
 * return  : error status
 ***********************************************************************/
STATUS dhcp_fsm(dhcp_ccb_t *dhcp_ccb, U32 lan_user_id, BIT16 event)
{	
    int      i;
    BOOL     retval;
    char     str1[30], str2[30];
    FastRG_t *fastrg_ccb = dhcp_ccb->fastrg_ccb;

    /* Find a matched state */
    for(i=0; dhcp_fsm_tbl[i].state!=S_DHCP_INVLD; i++)
        if (dhcp_fsm_tbl[i].state == dhcp_ccb->per_lan_user_pool[lan_user_id]->lan_user_info.state)
            break;
    FastRG_LOG(INFO, fastrg_ccb->fp, (U8 *)dhcp_ccb, DHCPLOGMSG, 
        "subscriber %u lan user %u Current state is %s\n", 
        dhcp_ccb->ccb_id, lan_user_id, 
        DHCP_state2str(dhcp_fsm_tbl[i].state));
    if (dhcp_fsm_tbl[i].state == S_DHCP_INVLD) {
        FastRG_LOG(INFO, fastrg_ccb->fp, (U8 *)dhcp_ccb, DHCPLOGMSG, 
            "Error! subscriber %u lan user %u unknown state(%d) specified for the event(%d)\n",
        	dhcp_ccb->ccb_id, lan_user_id, 
            dhcp_ccb->per_lan_user_pool[lan_user_id]->lan_user_info.state, 
            event);
        return ERROR;
    }

    /*
     * Find a matched event in a specific state.
     * Note : a state can accept several events.
     */
    for(;dhcp_fsm_tbl[i].state==dhcp_ccb->per_lan_user_pool[lan_user_id]->lan_user_info.state; i++)
        if (dhcp_fsm_tbl[i].event == event)
            break;

    /* search until meet the next state */
    if (dhcp_fsm_tbl[i].state != 
            dhcp_ccb->per_lan_user_pool[lan_user_id]->lan_user_info.state) {
        FastRG_LOG(INFO, fastrg_ccb->fp, (U8 *)dhcp_ccb, DHCPLOGMSG, 
            "Error! subscriber %u lan user %u invalid event(%d) in state(%s)\n",
            dhcp_ccb->ccb_id, lan_user_id, event, 
            DHCP_state2str(dhcp_ccb->per_lan_user_pool[lan_user_id]->lan_user_info.state));
        return ERROR;
    }

    /* Correct state found */
    if (dhcp_ccb->per_lan_user_pool[lan_user_id]->lan_user_info.state
            != dhcp_fsm_tbl[i].next_state) {
        strcpy(str1, DHCP_state2str(dhcp_ccb->per_lan_user_pool[lan_user_id]->lan_user_info.state));
        strcpy(str2, DHCP_state2str(dhcp_fsm_tbl[i].next_state));
        FastRG_LOG(INFO, fastrg_ccb->fp, (U8 *)dhcp_ccb, DHCPLOGMSG, 
            "subscriber %u lan user %u dhcp state changed from %s to %s\n", 
            dhcp_ccb->ccb_id, lan_user_id, str1, str2);
        dhcp_ccb->per_lan_user_pool[lan_user_id]->lan_user_info.state = dhcp_fsm_tbl[i].next_state;
    }

    for(int j=0; dhcp_fsm_tbl[i].hdl[j]; j++) {
       	retval = (*dhcp_fsm_tbl[i].hdl[j])(dhcp_ccb, lan_user_id);
       	if (retval == ERROR) {
            FastRG_LOG(ERR, fastrg_ccb->fp, (U8 *)dhcp_ccb, DHCPLOGMSG, 
                "subscriber %u lan user %u dhcp handler %d execution failed\n", 
                dhcp_ccb->ccb_id, lan_user_id, j);
            return ERROR;
        }
        FastRG_LOG(INFO, fastrg_ccb->fp, (U8 *)dhcp_ccb, DHCPLOGMSG, 
            "subscriber %u lan user %u dhcp handler %d executed successfully\n", 
            dhcp_ccb->ccb_id, lan_user_id, j);
    }
    return SUCCESS;
}

STATUS A_send_dhcp_offer(dhcp_ccb_t *dhcp_ccb, U32 pool_index)
{
    struct rte_ether_addr macaddr;

    rte_eth_macaddr_get(0, &macaddr);
    return build_dhcp_offer(dhcp_ccb->per_lan_user_pool[pool_index], &macaddr);
}

void request_timer(__attribute__((unused))struct rte_timer *tim, 
    dhcp_ccb_per_lan_user_t *per_lan_user_pool)
{
    dhcp_fsm(per_lan_user_pool->dhcp_ccb, 
        per_lan_user_pool->pool_index, E_TIMEOUT);
}

STATUS A_wait_request_timer(dhcp_ccb_t *dhcp_ccb, U32 pool_index)
{
    lan_user_info_t *lan_user_info = &dhcp_ccb->per_lan_user_pool[pool_index]->lan_user_info;
    struct rte_timer *tim = &lan_user_info->timer;
    FastRG_t *fastrg_ccb = dhcp_ccb->fastrg_ccb;

    rte_timer_stop(tim);
    rte_timer_reset(tim, 5 * fastrg_get_cycles_in_sec(), SINGLE, 
        fastrg_ccb->lcore.ctrl_thread, (rte_timer_cb_t)request_timer, 
        dhcp_ccb->per_lan_user_pool[pool_index]);
    return SUCCESS;
}

STATUS A_send_dhcp_ack(dhcp_ccb_t *dhcp_ccb, U32 pool_index)
{
    struct rte_ether_addr macaddr;

    rte_eth_macaddr_get(0, &macaddr);
    return build_dhcp_ack(dhcp_ccb->per_lan_user_pool[pool_index], &macaddr);
}

STATUS A_send_dhcp_nak(dhcp_ccb_t *dhcp_ccb, U32 pool_index)
{
    struct rte_ether_addr macaddr;

    rte_eth_macaddr_get(0, &macaddr);
    return build_dhcp_nak(dhcp_ccb->per_lan_user_pool[pool_index], &macaddr);
}

void lease_timer(__attribute__((unused))struct rte_timer *tim, 
    dhcp_ccb_per_lan_user_t *per_lan_user_pool)
{
    dhcp_fsm(per_lan_user_pool->dhcp_ccb, 
        per_lan_user_pool->pool_index, E_TIMEOUT);
}

STATUS A_wait_lease_timer(dhcp_ccb_t *dhcp_ccb, U32 pool_index)
{
    struct rte_timer *tim = &dhcp_ccb->per_lan_user_pool[pool_index]->lan_user_info.timer;
    FastRG_t *fastrg_ccb = dhcp_ccb->fastrg_ccb;
    lan_user_info_t *lan_user_info = &dhcp_ccb->per_lan_user_pool[pool_index]->lan_user_info;

    rte_timer_stop(tim);
    rte_timer_reset(tim, (lan_user_info->timeout_secs != 0 ? 
        lan_user_info->timeout_secs : LEASE_TIMEOUT) 
        * fastrg_get_cycles_in_sec(), SINGLE, fastrg_ccb->lcore.ctrl_thread, 
        (rte_timer_cb_t)lease_timer, 
        dhcp_ccb->per_lan_user_pool[pool_index]);

    return SUCCESS;
}

STATUS A_release(dhcp_ccb_t *dhcp_ccb, U32 pool_index)
{
    release_lan_user(&dhcp_ccb->per_lan_user_pool[pool_index]->lan_user_info.timer, 
        dhcp_ccb->per_lan_user_pool[pool_index]);
    return SUCCESS;
}

STATUS A_mark_ip_conflicted(dhcp_ccb_t *dhcp_ccb, U32 pool_index)
{
    dhcp_ccb_per_lan_user_t *per_lan_user = dhcp_ccb->per_lan_user_pool[pool_index];
    ip_pool_t *ip_pool = &per_lan_user->ip_pool;
    lan_user_info_t *lan_user_info = &per_lan_user->lan_user_info;
    FastRG_t *fastrg_ccb = dhcp_ccb->fastrg_ccb;

    ip_pool->used = TRUE;
    lan_user_info->lan_user_used = TRUE;
    memset(&lan_user_info->mac_addr, 0, sizeof(struct rte_ether_addr));
    memset(&ip_pool->mac_addr, 0, sizeof(struct rte_ether_addr));
    rte_timer_stop_sync(&lan_user_info->timer);
    rte_timer_reset(&lan_user_info->timer, 
        360 * fastrg_get_cycles_in_sec(), SINGLE, // mark as conflicted for 6 minutes
        fastrg_ccb->lcore.ctrl_thread, (rte_timer_cb_t)release_lan_user, 
        per_lan_user);

    return SUCCESS;
}

STATUS A_send_dhcp_ack_inform(dhcp_ccb_t *dhcp_ccb, U32 pool_index)
{
    struct rte_ether_addr macaddr;

    rte_eth_macaddr_get(0, &macaddr);
    return build_dhcp_ack_inform(dhcp_ccb->per_lan_user_pool[pool_index], &macaddr);
}
