#ifndef _ETCD_CLIENT_H_
#define _ETCD_CLIENT_H_

#include <common.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ETCD_SUCCESS = 0,
    ETCD_ERROR = -1,
    ETCD_CONNECTION_FAILED = -2,
    ETCD_WATCH_FAILED = -3,
    ETCD_CONFIG_PARSE_FAILED = -4,
    ETCD_KEY_NOT_FOUND = -5
} etcd_status_t;

typedef enum {
    HSI_ACTION_CREATE = 1,
    HSI_ACTION_UPDATE = 2,
    HSI_ACTION_DELETE = 3,
    COMMAND_ACTION_DIAL = 4,
    COMMAND_ACTION_HANGUP = 5
} etcd_action_type_t;

#define ETCD_RETRY_BASE_TIME 1 // in second

// Fallback error reason categories for quick problem identification
typedef enum {
    ERROR_REASON_CALLBACK_FAILED = 1,        // Callback function returned error
    ERROR_REASON_PARSE_FAILED = 2,           // JSON parsing failed
    ERROR_REASON_INVALID_FORMAT = 3,         // Invalid key/value format
    ERROR_REASON_MISSING_FIELD = 4,          // Required field missing in config
    ERROR_REASON_RESOURCE_UNAVAILABLE = 5,   // System resource not available
    ERROR_REASON_TIMEOUT = 6,                // Processing timeout
    ERROR_REASON_UNKNOWN = 99                // Unknown error
} etcd_error_reason_t;

// SNAT port-mapping entry for etcd config
typedef struct {
    U16 eport;           // external port (host byte order)
    char dip[32];        // destination LAN IP string
    U16 dport;           // destination/internal port (host byte order)
} port_mapping_t;

// HSI config structure matching Go's HSIConfig
typedef struct {
    char user_id[64];
    char vlan_id[16];
    char account_name[256];
    char password[256];
    char dhcp_addr_pool[64];
    char dhcp_subnet[32];
    char dhcp_gateway[32];
    port_mapping_t *port_mappings;  // heap-allocated; use hsi_config_free_port_mappings() to free
    int port_mapping_count;
} hsi_config_t;

// Free heap-allocated port_mappings inside an hsi_config_t
static inline void hsi_config_free_port_mappings(hsi_config_t *cfg) {
    if (cfg && cfg->port_mappings) {
        free(cfg->port_mappings);
        cfg->port_mappings = NULL;
        cfg->port_mapping_count = 0;
    }
}

// PPPoE command structure
typedef struct {
    char action[16];        // "dial" or "hangup"
    char user_id[64];
    char vlan[16];
    char account[256];
    char password[256];
    long timestamp;
} pppoe_command_t;

// User count config structure for dynamic scaling
typedef struct {
    int user_count;         // New user count to scale to
} user_count_config_t;

typedef enum {
    ENABLE_STATUS_ENABLED = 1,
    ENABLE_STATUS_ENABLING = 2,
    ENABLE_STATUS_DISABLING = 3,
    ENABLE_STATUS_DISABLED = 4
} hsi_enable_status_t;

// Full HSI config structure including metadata
typedef struct {
    hsi_config_t config;
    hsi_enable_status_t enable_status;
    char updated_by[64];
    char updated_at[32];
    char resource_version[64];
} hsi_config_full_t;

// Callback function types
typedef STATUS (*hsi_config_callback_t)(const char *node_id, const char *user_id, 
    const hsi_config_t *config, etcd_action_type_t action, 
    int64_t revision, void *user_data);

typedef STATUS (*pppoe_command_callback_t)(const char *node_id, 
    const pppoe_command_t *command, void *user_data);

typedef STATUS (*user_count_changed_callback_t)(const char *node_id,
    const user_count_config_t *config, etcd_action_type_t action,
    int64_t revision, void *user_data);

// Callback to request local state sync to etcd after reconnection
// This callback is invoked when etcd reconnects and needs to check/sync state
// The callback should write local HSI configs and subscriber count to etcd if they don't exist
typedef void (*sync_request_callback_t)(const char *node_id, void *user_data);

/* Initialize etcd client */
etcd_status_t etcd_client_init(const char *etcd_endpoints, void* user_data);

/* Start watching etcd for changes */
etcd_status_t etcd_client_start_watch(const char *node_uuid,
    hsi_config_callback_t hsi_callback,
    pppoe_command_callback_t command_callback,
    user_count_changed_callback_t user_count_callback,
    sync_request_callback_t sync_request_callback);

/* Stop watching etcd */
void etcd_client_stop_watch(void);

/* Delete processed command from etcd */
etcd_status_t etcd_client_delete_command(const char *command_key);

/* Check if etcd client is initialized */
int etcd_client_is_initialized(void);

/* Put or delete HSI config for a node/user
 * key: configs/{nodeId}/hsi/{userId}
 * value: JSON matching HSIConfigWithMetadata
 */
etcd_status_t etcd_client_put_hsi_config(const char *node_id, const char *user_id, 
    const hsi_config_t *config, hsi_enable_status_t enable_status, const char *updated_by);
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
 *        Output structure to receive the config and status
 * @return
 *        ETCD_SUCCESS or error code
 */
etcd_status_t etcd_client_get_hsi_config(const char *node_id, 
    const char *user_id, hsi_config_full_t *output);

/**
 * @fn etcd_client_modify_hsi_config_status
 * 
 * @brief Modify HSI config status (enable/disable)
 * This function updates only the metadata.enableStatus field in etcd
 * and marks the change to prevent the watcher from processing it
 * @param node_id
 *      Node UUID
 * @param user_id
 *      User identifier
 * @param enable_status
 *      HSI command status
 * @return
 *      ETCD_SUCCESS or error code
 */
etcd_status_t etcd_client_modify_hsi_config_status(const char *node_id, 
    const char *user_id, hsi_enable_status_t enable_status, int64_t *revision);

/**
 * @fn etcd_client_get_hsi_config_status
 * 
 * @brief Get HSI config status from etcd
 *        This function reads the current HSI config status
 * @param node_id
 *        Node UUID
 * @param user_id
 *        User identifier
 * @param output_status
 *        Current HSI config status
 * @return
 *        ETCD_SUCCESS or error code
 */
etcd_status_t etcd_client_get_hsi_config_status(const char *node_id, 
    const char *user_id, hsi_enable_status_t *output_status);

/**
 * @fn etcd_client_put_subscriber_count
 * 
 * @brief Set subscriber count config to etcd
 * @param node_id
 *        Node UUID
 * @param subscriber_count_str
 *        Subscriber count to set
 * @param updated_by
 *        Identifier of who updated this config
 * @return
 *        ETCD_SUCCESS or error code
 */
etcd_status_t etcd_client_put_subscriber_count(const char *node_id, 
    const char *subscriber_count_str, const char *updated_by);

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
 *        Node UUID
 * @param hsi_callback
 *        Callback to invoke for each config
 * @param command_callback
 *        Callback to invoke for each command
 * @param user_count_callback
 *        Callback to invoke for user count config
 * @param user_data
 *        User data to pass to callback
 * @return
 *        ETCD_SUCCESS or error code
 */
etcd_status_t etcd_client_load_existing_configs(const char *node_uuid,
    hsi_config_callback_t hsi_callback, 
    pppoe_command_callback_t command_callback,
    user_count_changed_callback_t user_count_callback,
    void *user_data);

/* Cleanup etcd client */
void etcd_client_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* _ETCD_CLIENT_H_ */
