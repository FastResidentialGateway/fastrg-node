#ifndef NORTHBOUND_H
#define NORTHBOUND_H

#include <common.h>

#include "fastrg.h"
#include "../northbound/controller/etcd_client.h"

/**
 * @fn apply_hsi_config
 * 
 * @brief This function configures PPPoE and DHCP settings for a specific user.
 *        During configuration, the user's services are temporarily disabled to
 *        ensure atomic updates.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based index)
 * @param config
 *      HSI configuration to apply
 * @param is_update
 *      TRUE if updating existing config, FALSE if creating new
 *
 * @return 
 *      SUCCESS on success, ERROR on failure 
 */
STATUS apply_hsi_config(FastRG_t *fastrg_ccb, int ccb_id, const hsi_config_t *config, 
    BOOL is_update);

/**
 * @fn remove_hsi_config
 * 
 * @brief Remove HSI configuration for a user
 * 
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based index)
 *
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS remove_hsi_config(FastRG_t *fastrg_ccb, int ccb_id);

/**
 * @fn execute_pppoe_dial
 * 
 * @brief Execute PPPoE dial command for a user
 * 
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based index)
 * @param command
 *      PPPoE command details
 *
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS execute_pppoe_dial(FastRG_t *fastrg_ccb, int ccb_id, const pppoe_command_t *command);

/**
 * @fn execute_pppoe_hangup
 * 
 * @brief Execute PPPoE hangup command for a user
 * 
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based index)
 * 
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS execute_pppoe_hangup(FastRG_t *fastrg_ccb, int ccb_id);

void reset_vlan_map_ccb_id(FastRG_t *fastrg_ccb, U16 vlan_id);

/**
 * @fn set_snat_port_fwd
 *
 * @brief Add a static SNAT port forwarding rule for a user.
 *        Maps eport on the WAN PPPoE IP to dip:iport on the LAN.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based)
 * @param eport
 *      External port (host byte order)
 * @param dip
 *      Destination LAN IP string (e.g. "192.168.1.100")
 * @param iport
 *      Internal port (host byte order)
 *
 * @return SUCCESS on success, ERROR on failure
 */
STATUS set_snat_port_fwd(FastRG_t *fastrg_ccb, U16 ccb_id, U16 eport,
    const char *dip, U16 iport);

/**
 * @fn remove_snat_port_fwd
 *
 * @brief Remove a static SNAT port forwarding rule for a user.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based)
 * @param eport
 *      External port (host byte order)
 *
 * @return SUCCESS on success, ERROR on failure
 */
STATUS remove_snat_port_fwd(FastRG_t *fastrg_ccb, U16 ccb_id, U16 eport);

#endif /* NORTHBOUND_H */
