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
    ETCD_KEY_NOT_FOUND = -5,
    ETCD_CAS_CONFLICT = -6      // Compare-And-Swap exhausted retries (concurrent writer won)
} etcd_status_t;

typedef enum {
    HSI_ACTION_CREATE = 1,
    HSI_ACTION_UPDATE = 2,
    HSI_ACTION_DELETE = 3
} etcd_action_type_t;

/* PPPoE desired connection state, stored in the HSI config object as
 * config.desire_status. Only the CLI/controller change it (connect/disconnect).
 * The node reconciles the live PPPoE session toward this value. */
#define DESIRE_STATUS_CONNECT    "connect"
#define DESIRE_STATUS_DISCONNECT "disconnect"

#define ETCD_RETRY_BASE_TIME 1 // in second

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
    BOOL dns_proxy_enable;      /* per-subscriber DNS proxy enable; defaults to TRUE when absent in etcd */
    BOOL tcp_conntrack_enable;  /* per-subscriber TCP SPI enable; defaults to TRUE when absent in etcd */
    char desire_status[16];     /* "connect"/"disconnect"; empty = disconnect. Only CLI/controller set it. */
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

// User count config structure for dynamic scaling
typedef struct {
    int user_count;         // New user count to scale to
} user_count_config_t;

// Full HSI config structure including metadata.
// PPPoE desired state lives in config.desire_status; observed/actual status is
// reported to the controller via Kafka (no longer stored in etcd metadata).
typedef struct {
    hsi_config_t config;
    char updated_by[64];
    char updated_at[32];
    char resource_version[64];
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

// DNS static record structure for etcd
typedef struct {
    char domain[256];
    char ip[32];
    U32 ttl;
} dns_record_config_t;

// DNS record callback type
// path: configs/{nodeId}/{subscriberId}/dns/{domain}
typedef STATUS (*dns_record_callback_t)(const char *node_id, const char *user_id,
    const dns_record_config_t *record, etcd_action_type_t action,
    int64_t revision, void *user_data);

/* ---- Asynchronous etcd event delivery -----------------------------------
 * etcd watcher threads parse + self-event-filter, then hand a heap-allocated
 * etcd_event_t to the control-plane loop (fastrg_loop) via the etcd_event_q
 * ring. fastrg_loop is the single thread that applies changes to CCBs, so the
 * apply path needs no locking.
 */
typedef enum {
    ETCD_EVENT_HSI = 1,        /* HSI config create/update/delete         */
    ETCD_EVENT_USER_COUNT,     /* subscriber-count change                 */
    ETCD_EVENT_DNS_RECORD,     /* DNS static record create/update/delete  */
    ETCD_EVENT_HSI_SWEEP       /* reconcile: keep ccb_ids present in etcd       */
} etcd_event_kind_t;

typedef struct etcd_event {
    etcd_event_kind_t  kind;
    etcd_action_type_t action;          /* CREATE/UPDATE/DELETE; unused for sweep */
    int64_t            revision;
    BOOL               from_reconcile;  /* TRUE: periodic reconcile; FALSE: live watch event */
    char               node_id[64];
    char               user_id[64];
    union {
        struct {
            hsi_config_t config;        /* config.port_mappings is heap-owned by this event */
            BOOL         desire_connect;/* derived from config.desire_status == "connect" */
            char         resource_version[24]; /* metadata.resourceVersion of the watched
                                                * value; empty when absent/unparsable. Used
                                                * for ConfigApplyResult.applied_resource_version. */
        } hsi;
        user_count_config_t user_count;
        dns_record_config_t dns_record;
        struct {
            int *present_ccb_ids;       /* heap-owned: ccb_ids that exist in etcd */
            int  count;
        } sweep;
    } event_data;
} etcd_event_t;

/* Free an etcd_event_t and any heap payload it owns. */
static inline void etcd_event_free(etcd_event_t *ev) {
    if (!ev)
        return;
    if (ev->kind == ETCD_EVENT_HSI) {
        hsi_config_free_port_mappings(&ev->event_data.hsi.config);
    } else if (ev->kind == ETCD_EVENT_HSI_SWEEP) {
        free(ev->event_data.sweep.present_ccb_ids);
    }
    free(ev);
}

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
 *        Output structure to receive the config and status
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
