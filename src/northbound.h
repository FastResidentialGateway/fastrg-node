#ifndef NORTHBOUND_H
#define NORTHBOUND_H

#include <common.h>

#include "fastrg.h"
#include "../northbound/controller/etcd_client.h"

BOOL is_valid_ccb_id(const FastRG_t *fastrg_ccb, int ccb_id);

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
 *
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS execute_pppoe_dial(FastRG_t *fastrg_ccb, int ccb_id);

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
 * @fn reconcile_port_mapping
 *
 * @brief Reconcile the local port forwarding table with the desired
 *        port-mapping configuration from etcd.  Entries not present in
 *        the desired set are removed; entries in the desired set that
 *        do not yet exist are added.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based)
 * @param mappings
 *      Array of desired port-mapping entries
 * @param mappings_count
 *      Number of entries in mappings array
 *
 * @return SUCCESS if all entries reconciled, ERROR if any entry failed
 */
STATUS reconcile_port_mapping(FastRG_t *fastrg_ccb, int ccb_id,
    const port_mapping_t *mappings, int mapping_count);

/**
 * @fn apply_dns_record
 *
 * @brief Apply a static DNS record for a subscriber.
 *        Adds or updates a static DNS entry in the subscriber's DNS proxy state.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based)
 * @param record
 *      DNS record configuration
 *
 * @return SUCCESS on success, ERROR on failure
 */
STATUS apply_dns_record(FastRG_t *fastrg_ccb, int ccb_id,
    const dns_record_config_t *record);

/**
 * @fn remove_dns_record
 *
 * @brief Remove a static DNS record for a subscriber.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based)
 * @param domain
 *      Domain name to remove
 *
 * @return SUCCESS on success, ERROR on failure
 */
STATUS remove_dns_record(FastRG_t *fastrg_ccb, int ccb_id, const char *domain);

#endif /* NORTHBOUND_H */
