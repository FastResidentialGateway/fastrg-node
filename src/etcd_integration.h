#ifndef _ETCD_INTEGRATION_H_
#define _ETCD_INTEGRATION_H_

#include <common.h>

#include "fastrg.h"
#include "../northbound/controller/etcd_client.h"

/**
 * Pending event tracking for etcd operations
 * Used to detect self-generated events during watch callbacks
 */

/**
 * @fn etcd_mark_pending_event
 * 
 * @brief 
 *      Mark an event as pending before etcd operation
 * @param action
 *      Etcd action type (PUT/DELETE)
 * @param ccb_id
 *      User ccb ID (user_id - 1)
 * @return 
 *      void
 */
void etcd_mark_pending_event(etcd_action_type_t action, U16 ccb_id);

/**
 * @fn etcd_confirm_pending_event
 * 
 * @brief 
 *      Confirm a pending event with its revision after etcd operation
 * @param action
 *      Etcd action type (PUT/DELETE)
 * @param ccb_id
 *      User ccb ID (user_id - 1)
 * @param revision
 *      Etcd revision number of the operation
 * @return 
 *      void
 */
void etcd_confirm_pending_event(etcd_action_type_t action, U16 ccb_id, int64_t revision);

/**
 * @fn etcd_is_self_event
 * 
 * @brief 
 *      Check if an event is self-initiated by matching revision
 * @param action
 *      Etcd action type (PUT/DELETE)
 * @param ccb_id
 *      User ccb ID (user_id - 1)
 * @param revision
 *      Etcd revision number of the event
 * @return 
 *      TRUE if self-initiated, FALSE otherwise
 */
BOOL etcd_is_self_event(etcd_action_type_t action, U16 ccb_id, int64_t revision);

/**
 * @fn etcd_remove_event
 * 
 * @brief 
 *      Remove a pending event from tracking
 * @param action
 *      Etcd action type (PUT/DELETE)
 * @param ccb_id
 *      User ccb ID (user_id - 1)
 * @return 
 *      void
 */
void etcd_remove_event(etcd_action_type_t action, U16 ccb_id);

/**
 * @fn etcd_integration_init
 * 
 * @brief 
 *      Initialize etcd integration
 * @param fastrg_ccb
 *      Pointer to FastRG context
 * @return 
 *      SUCCESS on success, ERROR on failure
 */
STATUS etcd_integration_init(FastRG_t *fastrg_ccb);

/**
 * @fn etcd_integration_start
 * 
 * @brief 
 *      Start etcd integration (watching)
 * @param fastrg_ccb
 *      Pointer to FastRG context
 * @return 
 *      SUCCESS on success, ERROR on failure
 */
STATUS etcd_integration_start(FastRG_t *fastrg_ccb);

/**
 * @fn etcd_integration_stop
 * 
 * @brief 
 *      Stop etcd integration
 * @param fastrg_ccb
 *      Pointer to FastRG context
 * @return 
 *      void
 */
void etcd_integration_stop(FastRG_t *fastrg_ccb);

/**
 * @fn etcd_integration_cleanup
 * 
 * @brief 
 *      Cleanup etcd integration resources
 * @param fastrg_ccb
 *      Pointer to FastRG context
 * @return 
 *      void
 */
void etcd_integration_cleanup(FastRG_t *fastrg_ccb);

/**
 * @fn hsi_config_changed_callback
 * 
 * @brief 
 *      Callback for HSI config changes from etcd
 * @param node_id
 *      Node UUID
 * @param user_id
 *      User identifier (start from 1)
 * @param config
 *      Pointer to HSI configuration (NULL if deleted)
 * @param action
 *      Etcd action type (CREATE/UPDATE/DELETE)
 * @param revision
 *      Etcd revision number of the event
 * @param user_data
 *      User data pointer (FastRG context)
 * @return 
 *      SUCCESS on success, ERROR on failure
 */
STATUS hsi_config_changed_callback(const char *node_id, const char *user_id, 
    const hsi_config_t *config, etcd_action_type_t action, 
    int64_t revision, void *user_data);

/**
 * @fn pppoe_command_received_callback
 * 
 * @brief 
 *      Callback for PPPoE command received from etcd
 * @param node_id
 *      Node UUID
 * @param command
 *      Pointer to PPPoE command structure
 * @param user_data
 *      User data pointer (FastRG context)
 * @return 
 *      SUCCESS on success, ERROR on failure
 */
STATUS pppoe_command_received_callback(const char *node_id, 
    const pppoe_command_t *command, void *user_data);

/**
 * @fn user_count_changed_callback
 * 
 * @brief 
 *      Callback for user count configuration changes from etcd
 *      This callback handles dynamic scaling by adding or removing CCBs
 * @param node_id
 *      Node UUID
 * @param config
 *      Pointer to user count configuration
 * @param action
 *      Etcd action type (CREATE/UPDATE/DELETE)
 * @param revision
 *      Etcd revision number of the event
 * @param user_data
 *      User data pointer (FastRG context)
 * @return 
 *      SUCCESS on success, ERROR on failure
 */
STATUS user_count_changed_callback(const char *node_id,
    const user_count_config_t *config, etcd_action_type_t action,
    int64_t revision, void *user_data);

/**
 * @fn sync_request_callback
 * 
 * @brief Callback to handle sync requests from etcd after reconnection
 *      This callback writes local state (HSI configs and subscriber count) to etcd
 * @param node_id
 *      Node UUID
 * @param user_data
 *      User data pointer (FastRG context)
 * @return 
 *      void
 */
void sync_request_callback(const char *node_id, void *user_data);

/**
 * @fn dns_record_changed_callback
 * 
 * @brief 
 *      Callback for DNS static record changes from etcd
 * @param node_id
 *      Node UUID
 * @param user_id
 *      User identifier (subscriber ID)
 * @param record
 *      Pointer to DNS record config (domain always set; ip/ttl set for PUT)
 * @param action
 *      Etcd action type (CREATE/UPDATE/DELETE)
 * @param revision
 *      Etcd revision number of the event
 * @param user_data
 *      User data pointer (FastRG context)
 * @return 
 *      SUCCESS on success, ERROR on failure
 */
STATUS dns_record_changed_callback(const char *node_id, const char *user_id,
    const dns_record_config_t *record, etcd_action_type_t action,
    int64_t revision, void *user_data);

#endif /* _ETCD_INTEGRATION_H_ */
