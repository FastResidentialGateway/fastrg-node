#ifndef _ETCD_CLIENT_H_
#define _ETCD_CLIENT_H_

#include <common.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "../../src/etcd_event.h"

typedef enum {
    ETCD_SUCCESS = 0,
    ETCD_ERROR = -1,
    ETCD_CONNECTION_FAILED = -2,
    ETCD_WATCH_FAILED = -3,
    ETCD_CONFIG_PARSE_FAILED = -4,
    ETCD_KEY_NOT_FOUND = -5,
    ETCD_CAS_CONFLICT = -6      // Compare-And-Swap exhausted retries (concurrent writer won)
} etcd_status_t;

#define ETCD_RETRY_BASE_TIME 1 // in second

// Full HSI config structure including metadata.
// PPPoE desired state lives in config.desire_status; observed/actual status is
// reported to the controller via Kafka (no longer stored in etcd metadata).
// resource_version is the controller-stamped version inside the JSON;
// mod_revision is etcd's own ModRevision for the key which this node reads from,
// and is used by the node to report to the controller to indicate which version
// the node uses.
typedef struct {
    hsi_config_t config;
    char updated_by[64];
    char updated_at[32];
    char resource_version[64];
    int64_t mod_revision;
} hsi_config_full_t;

// Callback function types
typedef STATUS (*hsi_config_callback_t)(const char *node_id, const char *user_id, 
    const hsi_config_t *config, etcd_action_type_t action, 
    int64_t revision, void *user_data);

typedef STATUS (*user_count_changed_callback_t)(const char *node_id,
    const user_count_config_t *config, etcd_action_type_t action,
    int64_t revision, void *user_data);

// Callback to request local state sync to etcd after reconnection
// This callback is invoked when etcd reconnects and needs to check/sync state
// The callback should write local HSI configs and subscriber count to etcd if they don't exist
typedef void (*sync_request_callback_t)(const char *node_id, void *user_data);

// DNS record callback type
// path: configs/{nodeId}/{subscriberId}/dns/{domain}
typedef STATUS (*dns_record_callback_t)(const char *node_id, const char *user_id,
    const dns_record_config_t *record, etcd_action_type_t action,
    int64_t revision, void *user_data);

/* Initialize etcd client */
etcd_status_t etcd_client_init(const char *etcd_endpoints, void* user_data);

/* Start watching etcd for changes. Watch/reconcile events are delivered to the
 * control-plane loop via FastRG_t.etcd_event_q; only sync_request_callback
 * (invoked when etcd has no data for this node) is passed through here. */
etcd_status_t etcd_client_start_watch(const char *node_uuid,
    sync_request_callback_t sync_request_callback);

/* Stop watching etcd */
void etcd_client_stop_watch(void);

/* Delete processed command from etcd */
etcd_status_t etcd_client_delete_command(const char *command_key);

/* Check if etcd client is initialized */
int etcd_client_is_initialized(void);

/* Last-known etcd reachability, updated by the watchdog each tick (and set on a
 * successful init). Non-blocking. Returns 1 if etcd is currently reachable.
 * Used by the node gRPC server to decide whether to accept CLI config writes:
 * reject when SDN + reachable (CLI must go via controller/etcd), accept+queue
 * when etcd is unreachable. */
int etcd_client_is_connected(void);

/* ---- Offline edits ------------------------------------------------------
 * The node is read-only on etcd (writes go via the controller). Offline
 * (etcd-unreachable) edits are applied to the persisted local snapshot (see
 * config_snapshot.h) and reported to the controller as ConfigOfflineEdit
 * events over Kafka on reconnect — the node never writes etcd.
 */

/**
 * @fn etcd_client_render_hsi_config
 *
 * @brief Serialize an hsi_config_t into the HSIConfigWithMetadata JSON
 *        envelope (same shape as the etcd value; metadata fields are
 *        placeholders — the snapshot layer re-stamps them).
 *        Used by the offline-edit path to store full configs into the local
 *        snapshot.
 * @param node_id
 *        Node UUID
 * @param config
 *        Config to serialize
 * @return
 *        malloc'd JSON string (caller frees), or NULL on error
 */
char *etcd_client_render_hsi_config(const char *node_id, const hsi_config_t *config);

/**
 * @fn etcd_client_parse_hsi_config
 *
 * @brief parse an etcd-schema HSI config JSON (metadata envelope or legacy
 *        bare config) into an hsi_config_t; the parser lives here because the
 *        JSON schema is the etcd client's domain
 * @param value_json
 *      raw JSON value (etcd value or snapshot mirror of it)
 * @param out_config
 *      parsed config; caller frees port_mappings via
 *      hsi_config_free_port_mappings()
 * @param out_is_enabled
 *      optional; receives the config's desire_status == "connect"
 * @return
 *      SUCCESS, or ERROR when the client is uninitialized or the JSON is
 *      unparsable
 */
STATUS etcd_client_parse_hsi_config(const char *value_json, hsi_config_t *out_config,
    BOOL *out_is_enabled);

/**
 * @fn etcd_client_get_value
 *
 * @brief read a single etcd key's current value (offline-edit reporter
 *        primitive)
 * @param key
 *      full etcd key
 * @param out_value
 *      receives a malloc'd copy of the value on ETCD_SUCCESS (caller frees);
 *      NULL otherwise
 * @return
 *      ETCD_SUCCESS, ETCD_KEY_NOT_FOUND when the key is absent, or
 *      ETCD_ERROR on a transient read failure
 */
etcd_status_t etcd_client_get_value(const char *key, char **out_value);
/**
 * @fn etcd_client_delete_hsi_config
 * 
 * @brief Delete HSI config from etcd
 * @param node_id
 *       Node UUID
 * @param user_id
 *       User identifier (username or circuit-id)
 * @param revision
 *       Output parameter for etcd revision (optional, can be NULL)
 * @return
 *       ETCD_STATUS_SUCCESS or error code
 */
etcd_status_t etcd_client_delete_hsi_config(const char *node_id, 
    const char *user_id, int64_t *revision);

/**
 * @fn etcd_client_get_hsi_config
 * 
 * @brief Get HSI config from etcd
 *        This function reads the current HSI config including its metadata
 * @param node_id
 *        Node UUID
 * @param user_id
 *        User identifier
 * @param output
 *        Output structure to receive the config, its metadata and the key's
 *        etcd ModRevision
 * @return
 *        ETCD_SUCCESS or error code
 */
etcd_status_t etcd_client_get_hsi_config(const char *node_id,
    const char *user_id, hsi_config_full_t *output);

/**
 * @fn etcd_client_get_subscriber_count
 * 
 * @brief Get subscriber count from etcd
 * @param node_id
 *        Node UUID
 * @param subscriber_count
 *        Output parameter to receive subscriber count
 * @return
 *        ETCD_SUCCESS or error code
 */
etcd_status_t etcd_client_get_subscriber_count(const char* node_id, 
    U16 *subscriber_count);

/**
 * @fn etcd_client_load_existing_configs
 * 
 * @brief Load existing HSI configs from etcd on startup
 * This function reads all existing configs under configs/{nodeId}/hsi/
 * and invokes the callback for each one
 * @param node_uuid
 *      Node UUID
 * @param hsi_callback
 *      Callback to invoke for each config
 * @param user_count_callback
 *      Callback to invoke for user count config
 * @param user_data
 *      User data to pass to callback
 * @return
 *      ETCD_SUCCESS or error code
 */
etcd_status_t etcd_client_load_existing_configs(const char *node_uuid,
    hsi_config_callback_t hsi_callback,
    user_count_changed_callback_t user_count_callback,
    dns_record_callback_t dns_record_callback,
    void *user_data);

/**
 * @fn etcd_client_load_dns_records
 *
 * @brief Load all DNS static records for a specific subscriber from etcd.
 *        Called when a PPPoE session comes up to restore per-user DNS overrides.
 *        key pattern: configs/{nodeId}/dns/{userId}/{domain}
 * @param node_uuid Node UUID
 *      Node UUID
 * @param user_id
 *      Subscriber user ID string
 * @param dns_record_callback
 *      Callback invoked for each record found (action = HSI_ACTION_CREATE)
 * @param user_data
 *      Opaque pointer forwarded to the callback
 * @return
 *      ETCD_SUCCESS on success, ETCD_ERROR otherwise
 */
etcd_status_t etcd_client_load_dns_records(const char *node_uuid,
    const char *user_id,
    dns_record_callback_t dns_record_callback,
    void *user_data);

/* Cleanup etcd client */
void etcd_client_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* _ETCD_CLIENT_H_ */
