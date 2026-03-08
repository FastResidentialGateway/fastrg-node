#include <common.h>

#include "fastrg.h"
#include "dbg.h"
#include "dhcpd/dhcpd.h"
#include "pppd/pppd.h"
#include "pppd/nat.h"
#include "utils.h"
#include "../northbound/controller/etcd_client.h"

BOOL is_valid_ccb_id(const FastRG_t *fastrg_ccb, int ccb_id)
{
    return (fastrg_ccb != NULL && 
            fastrg_ccb->ppp_ccb != NULL && 
            fastrg_ccb->dhcp_ccb != NULL &&
            ccb_id >= 0 && 
            ccb_id < fastrg_ccb->user_count && 
            DHCPD_GET_CCB(fastrg_ccb, ccb_id) != NULL &&
            PPPD_GET_CCB(fastrg_ccb, ccb_id) != NULL);
}

static inline STATUS set_vlan_map_ccb_id(FastRG_t *fastrg_ccb, U16 vlan_id, U16 ccb_id)
{
    if (vlan_id < 1 || vlan_id > MAX_VLAN_ID) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Invalid VLAN ID: %u (must be 1-%d)", vlan_id, MAX_VLAN_ID);
        return ERROR;
    }

    if (rte_atomic16_cmpset((volatile uint16_t *)&fastrg_ccb->vlan_userid_map[vlan_id - 1].cnt,
            INVALID_CCB_ID, ccb_id) == 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "VLAN ID %u is already assigned to another user", vlan_id);
        return ERROR;
    }
    return SUCCESS;
}

void reset_vlan_map_ccb_id(FastRG_t *fastrg_ccb, U16 vlan_id)
{
    if (vlan_id < 1 || vlan_id > MAX_VLAN_ID)
        return;

    rte_atomic16_set(&fastrg_ccb->vlan_userid_map[vlan_id - 1], INVALID_CCB_ID);
}

STATUS apply_hsi_config(FastRG_t *fastrg_ccb, int ccb_id, const hsi_config_t *config, BOOL is_update)
{
    U32 dhcp_ip_start, dhcp_ip_end, dhcp_subnet_mask, dhcp_gateway;
    int ret;

    if (!is_valid_ccb_id(fastrg_ccb, ccb_id) || !config)
        return ERROR;

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);

    U16 vlan_id;
    if (parse_vlan_id(config->vlan_id, &vlan_id) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Invalid VLAN ID: %s", config->vlan_id);
        return ERROR;
    }

    /* check dhcp configuration is valid */
    if (parse_ip_range(config->dhcp_addr_pool, &dhcp_ip_start, &dhcp_ip_end) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Invalid DHCP address pool: %s", config->dhcp_addr_pool);
        return ERROR;
    }
    if (parse_ip(config->dhcp_subnet, &dhcp_subnet_mask) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Invalid DHCP subnet mask: %s", config->dhcp_subnet);
        return ERROR;
    }
    if (parse_ip(config->dhcp_gateway, &dhcp_gateway) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Invalid DHCP gateway: %s", config->dhcp_gateway);
        return ERROR;
    }

    U16 ori_ppp_status = rte_atomic16_exchange((volatile uint16_t *)&ppp_ccb->ppp_bool.cnt, 0);
    U16 ori_dp_status = rte_atomic16_exchange((volatile uint16_t *)&ppp_ccb->dp_start_bool.cnt, 0);
    U16 ori_dhcp_status = rte_atomic16_exchange((volatile uint16_t *)&dhcp_ccb->dhcp_bool.cnt, 0);

    /* we don't apply config if this is a new configuration while hsi is still active */
    if (is_update == FALSE) {
        if (ori_dhcp_status == 1) {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
                "DHCP is already enabled for user %d while applying new HSI config", 
                ccb_id + 1);
            ret = ERROR;
            goto out;
        }
        if (ori_ppp_status == 1) {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
                "PPPoE is already enabled for user %d while applying new HSI config", 
                ccb_id + 1);
            ret = ERROR;
            goto out;
        }
    }

    /* On x86, rte_atomic32_inc() ensures the processing order of active_count 
        and dhcp_bool are strong order. Therefore, we don't need barrier here. */
    //rte_smp_mb();

    /* check if there are active DHCP packets being processed */
    U32 spin_count = 0;
    U32 yield_threshold = 1000; // check fast for 1000 times
    uint64_t start_tsc = rte_rdtsc();
    uint64_t timeout_us = 1000000; // 1 second timeout
    while (rte_atomic32_read(&dhcp_ccb->active_count) > 0) {
        if (spin_count < yield_threshold) {
            rte_pause();
            spin_count++;
        } else {
            rte_delay_ms(1);
            uint64_t elapsed_us = (rte_rdtsc() - start_tsc) * 1000000 / rte_get_tsc_hz();
            if (elapsed_us > timeout_us) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
                    "DHCP: Timeout waiting for active dhcp packets\n");
                ret = ERROR;
                goto out;
            }
        }
    }
    /* No more active DHCP packets, we can update now */

    /* Enable HSI for this user, if ppp_ccb is in NOT_CONFIGURED phase, 
    even it is a UPDATE event, we will treat it as a CREATE event */
    if (is_update == FALSE || ppp_ccb->phase == NOT_CONFIGURED) { // means this is a new config or the config exists in etcd but not in local
        if (set_vlan_map_ccb_id(fastrg_ccb, vlan_id, ccb_id) == ERROR) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "config new VLAN ID %u to user %u failed", vlan_id, ccb_id + 1);
            ret = ERROR;
            goto out;
        }
        U16 original_vlan_id = rte_atomic16_read(&ppp_ccb->vlan_id);
        if (ppp_init_config_by_user(fastrg_ccb, ppp_ccb, ccb_id, vlan_id, config->account_name, config->password) == ERROR) {
            reset_vlan_map_ccb_id(fastrg_ccb, vlan_id);
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "config PPPoE for user %u failed", ccb_id + 1);
            ret = ERROR;
            goto out;
        }
        if (original_vlan_id != vlan_id)
            reset_vlan_map_ccb_id(fastrg_ccb, original_vlan_id);
    } else {
        /* Only execute if VLAN ID has changed */
        U16 original_vlan_id = rte_atomic16_read(&ppp_ccb->vlan_id);
        if (original_vlan_id != vlan_id) {
            if (set_vlan_map_ccb_id(fastrg_ccb, vlan_id, ccb_id) == ERROR) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "config new VLAN ID %u to user %u failed", vlan_id, ccb_id + 1);
                ret = ERROR;
                goto out;
            }
            /* Remove original vlan map */
            reset_vlan_map_ccb_id(fastrg_ccb, original_vlan_id);
        }
        if (ppp_update_config_by_user(ppp_ccb, vlan_id, config->account_name, config->password) == ERROR) {
            if (original_vlan_id != vlan_id) {
                reset_vlan_map_ccb_id(fastrg_ccb, vlan_id);
                set_vlan_map_ccb_id(fastrg_ccb, original_vlan_id, ccb_id);
            }
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "update PPPoE config for user %u failed", ccb_id + 1);
            ret = ERROR;
            goto out;
        }
    }

    // Apply DHCP configuration
    dhcp_pool_init_by_user(dhcp_ccb, dhcp_gateway, 
        dhcp_ip_start, dhcp_ip_end, dhcp_subnet_mask);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
        "Applied HSI config for user %d: DHCP enabled with pool %s", 
        ccb_id + 1, config->dhcp_addr_pool);

    ret = SUCCESS;
    goto out;

out:
    rte_atomic16_set(&ppp_ccb->ppp_bool, ori_ppp_status);
    rte_atomic16_set(&ppp_ccb->dp_start_bool, ori_dp_status);
    rte_atomic16_set(&dhcp_ccb->dhcp_bool, ori_dhcp_status);

    return ret;
}

STATUS remove_hsi_config(FastRG_t *fastrg_ccb, int ccb_id)
{
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id))
        return ERROR;

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);

    if (rte_atomic16_read(&ppp_ccb->vlan_id) == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "User %u is not active", ccb_id + 1);
        return ERROR;
    }

    // Disable HSI and DHCP for this user
    if (rte_atomic16_read(&ppp_ccb->ppp_bool) != 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "PPPoE is still used for user %d", ccb_id + 1);
        return ERROR;
    }

    if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) != 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "DHCP is still used for user %d", ccb_id + 1);
        return ERROR;
    }

    reset_vlan_map_ccb_id(fastrg_ccb, rte_atomic16_read(&ppp_ccb->vlan_id));
    // Remove DHCP and PPPoE configuration
    ppp_cleanup_config_by_user(ppp_ccb, ccb_id);
    dhcp_pool_init_by_user(dhcp_ccb, 0, 0, 0, 0); //initialize with empty pool

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Removed HSI config for user %d", ccb_id + 1);

    return SUCCESS;
}

// Helper function to execute PPPoE dial
STATUS execute_pppoe_dial(FastRG_t *fastrg_ccb, int ccb_id, const pppoe_command_t *command)
{
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id) || !command)
        return ERROR;

    // Set up PPPoE session parameters
    // This would typically involve calling into the PPPoE subsystem
    // For now, just log the action
    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
        "Executing PPPoE dial for user %d: VLAN %s, Account %s", 
        ccb_id + 1, command->vlan, command->account);

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
    BOOL is_pppoe_enabled = FALSE, is_dhcp_enabled = FALSE;

    if (rte_atomic16_read(&ppp_ccb->ppp_bool) == 1) {
        is_pppoe_enabled = TRUE;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "HSI is already enabled for user %d", ccb_id + 1);
    }

    if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 1) {
        is_dhcp_enabled = TRUE;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "DHCP is already enabled for user %d", ccb_id + 1);
    }

    if (is_pppoe_enabled == FALSE && 
            fastrg_gen_northbound_event(EV_NORTHBOUND_PPPoE, PPPoE_CMD_ENABLE, ccb_id) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to generate PPPoE enable event for user %d", ccb_id + 1);
        return ERROR;
    }

    if (is_dhcp_enabled == FALSE && 
            fastrg_gen_northbound_event(EV_NORTHBOUND_DHCP, DHCP_CMD_ENABLE, ccb_id) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to generate DHCP enable event for user %d", ccb_id + 1);
        if (is_pppoe_enabled == FALSE && 
                fastrg_gen_northbound_event(EV_NORTHBOUND_PPPoE, PPPoE_CMD_DISABLE, ccb_id) == ERROR) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to generate PPPoE disable event for user %d", ccb_id + 1);
            return ERROR;
        }
        return ERROR;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
        "Applied HSI config for user %d", 
        ccb_id + 1);

    return SUCCESS;
}

// Helper function to execute PPPoE hangup
STATUS execute_pppoe_hangup(FastRG_t *fastrg_ccb, int ccb_id)
{
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id))
        return ERROR;

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
            "Executing PPPoE hangup for user %d", ccb_id + 1);

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);

    if (rte_atomic16_read(&ppp_ccb->ppp_bool) == 0)
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "HSI is already disabled for user %d", ccb_id + 1);
    if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 0)
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "DHCP is already disabled for user %d", ccb_id + 1);

    if (fastrg_gen_northbound_event(EV_NORTHBOUND_PPPoE, PPPoE_CMD_DISABLE, ccb_id) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to generate PPPoE disable event for user %d", ccb_id + 1);
        return ERROR;
    }

    if (fastrg_gen_northbound_event(EV_NORTHBOUND_DHCP, DHCP_CMD_DISABLE, ccb_id) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to generate DHCP disable event for user %d", ccb_id + 1);
        return ERROR;
    }

    return SUCCESS;
}

STATUS set_snat_port_fwd(FastRG_t *fastrg_ccb, U16 ccb_id, U16 eport,
    const char *dip, U16 iport)
{
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id) || dip == NULL)
        return ERROR;

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    if (ppp_ccb->phase != DATA_PHASE) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "User %u has not established PPPoE connection, cannot set SNAT port forwarding",
            ccb_id + 1);
        return ERROR;
    }

    U32 dip_be;
    if (parse_ip(dip, &dip_be) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Invalid destination IP: %s", dip);
        return ERROR;
    }

    port_fwd_add(ppp_ccb->port_fwd_table, eport, dip_be, iport);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "User %u: SNAT port forward added eport=%u -> %s:%u",
        ccb_id + 1, eport, dip, iport);

    return SUCCESS;
}

STATUS remove_snat_port_fwd(FastRG_t *fastrg_ccb, U16 ccb_id, U16 eport)
{
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id))
        return ERROR;

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);

    if (port_fwd_remove(ppp_ccb->port_fwd_table, eport) == ERROR) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
            "Port forwarding rule not found for user %u, eport=%u",
            ccb_id + 1, eport);
        return ERROR;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "User %u: SNAT port forward removed eport=%u",
        ccb_id + 1, eport);

    return SUCCESS;
}
