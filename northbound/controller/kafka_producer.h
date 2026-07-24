#ifndef _KAFKA_PRODUCER_H_
#define _KAFKA_PRODUCER_H_

/*
 * Node -> controller telemetry over Kafka (point 5/6).
 *
 * Reports config-apply results, PPPoE state transitions and runtime errors to
 * topic "fastrg.node.events" (partition key = node_uuid).
 *
 * Non-blocking: produce never blocks the data/control plane. Events are written
 * to a durable on-disk WAL (/etc/fastrg/kafka_queue.json) before being produced
 * and removed once delivery is confirmed, so undelivered events survive a node
 * restart and are replayed on the next kafka_producer_init() (slice 15). Only a
 * full WAL (long outage) drops the oldest, counted (telemetry tolerates loss).
 *
 * All kafka_report_* functions are no-ops until kafka_producer_init() succeeds,
 * so call sites can invoke them unconditionally.
 */

#include <common.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PPPoE transition phases, mirrors PPPoEPhase in kafka-events.proto. */
typedef enum {
    KAFKA_PPPOE_CONNECTING = 1,
    KAFKA_PPPOE_CONNECTED = 2,
    KAFKA_PPPOE_DISCONNECTING = 3,
    KAFKA_PPPOE_DISCONNECTED = 4
} kafka_pppoe_phase_t;

/**
 * @fn kafka_producer_init
 *
 * @brief initialize the Kafka producer, load the WAL and replay any buffered
 *        events; safe to call once at startup
 * @param brokers
 *      comma-separated host:port broker list
 * @param node_uuid
 *      this node's UUID, used as the partition key of every event
 * @return
 *      SUCCESS on success, ERROR otherwise
 */
STATUS kafka_producer_init(const char *brokers, const char *node_uuid);

/**
 * @fn kafka_producer_cleanup
 *
 * @brief flush outstanding deliveries and destroy the producer
 * @return
 *      void
 */
void kafka_producer_cleanup(void);

/**
 * @fn kafka_producer_is_ready
 *
 * @brief report whether the producer has been initialized successfully
 * @return
 *      1 if initialized and ready, else 0
 */
int kafka_producer_is_ready(void);

/**
 * @fn kafka_report_pppoe_state
 *
 * @brief report a PPPoE state transition
 * @param user_id
 *      subscriber id string
 * @param phase
 *      transition phase (mirrors PPPoEPhase in kafka-events.proto)
 * @param hsi_ipv4
 *      assigned IPv4 on "connected" transitions (may be NULL)
 * @param hsi_ipv4_gw
 *      gateway IPv4 on "connected" transitions (may be NULL)
 * @param err_msg
 *      error description on abnormal transitions (may be NULL)
 * @return
 *      void
 */
void kafka_report_pppoe_state(const char *user_id, kafka_pppoe_phase_t phase,
    const char *hsi_ipv4, const char *hsi_ipv4_gw, const char *err_msg);

/**
 * @fn kafka_report_config_apply
 *
 * @brief report the result of applying an HSI config
 * @param user_id
 *      subscriber id string
 * @param action
 *      "create" | "update" | "delete"
 * @param success
 *      TRUE when the apply succeeded
 * @param err_code
 *      machine-readable error code on failure (may be NULL on success)
 * @param err_msg
 *      human-readable error description on failure (may be NULL on success)
 * @param applied_resource_version
 *      metadata.resourceVersion of the config this apply targeted (may be
 *      NULL/empty; the controller falls back to its transitional guard)
 * @return
 *      void
 */
void kafka_report_config_apply(const char *user_id, const char *action,
    BOOL success, const char *err_code, const char *err_msg,
    const char *applied_resource_version);

/* Kinds mirroring OfflineEditKind in kafka-events.proto. */
typedef enum {
    KAFKA_OFFLINE_EDIT_HSI = 1,
    KAFKA_OFFLINE_EDIT_DNS = 2,
    KAFKA_OFFLINE_EDIT_COUNT = 3
} kafka_offline_edit_kind_t;

/**
 * @fn kafka_report_config_offline_edit
 *
 * @brief report an offline (etcd-unreachable) config edit for controller
 *        arbitration
 * @param kind
 *      target key family
 * @param user_id
 *      subscriber id string ("0" for SUBSCRIBER_COUNT)
 * @param config_json
 *      full snapshot JSON of the target key, metadata envelope included
 * @param resource_version
 *      metadata.resourceVersion stamped into config_json (duplicated for the
 *      consumer; must match the value inside the JSON)
 * @param edited_at
 *      unix time (seconds) of the last offline edit
 * @param edit_summary
 *      accumulated human-readable summary of the edits (may be empty)
 * @return
 *      void
 */
void kafka_report_config_offline_edit(kafka_offline_edit_kind_t kind,
    const char *user_id, const char *config_json, const char *resource_version,
    int64_t edited_at, const char *edit_summary);

/**
 * @fn kafka_report_config_offline_delete
 *
 * @brief report an offline (etcd-unreachable) config DELETE for controller
 *        arbitration: a tombstone with deleted=true and empty config_json
 * @param kind
 *      target key family (only KAFKA_OFFLINE_EDIT_HSI is legal)
 * @param user_id
 *      subscriber id string
 * @param resource_version
 *      the key's last-known rv at delete time, not incremented
 * @param edited_at
 *      wall-clock unix time (seconds) of the node's delete
 * @param edit_summary
 *      accumulated human-readable summary of the edits (may be empty)
 * @return
 *      void
 */
void kafka_report_config_offline_delete(kafka_offline_edit_kind_t kind,
    const char *user_id, const char *resource_version, int64_t edited_at,
    const char *edit_summary);

/**
 * @fn kafka_report_offline_edits
 *
 * @brief report every dirty snapshot entry (offline edits and delete
 *        tombstones) to the controller. Diffs each entry against the etcd
 *        current value first. MUST run BEFORE any path that mirrors etcd 
 *        values into the snapshot (boot-time load, state reconcile): 
 *        mirror writes clear the dirty flag and would silently swallow a 
 *        pending proposal (report-before-mirror)
 * @return
 *      TRUE when every dirty entry was handled; FALSE when etcd is
 *      unreachable or some entry is still pending (transient read failure)
 *      and will be retried on the next etcd watchdog tick
 */
BOOL kafka_report_offline_edits(void);

/**
 * @fn kafka_report_runtime_error
 *
 * @brief report a generic runtime error
 * @param module
 *      originating module name
 * @param err_code
 *      machine-readable error code
 * @param err_msg
 *      human-readable error description
 * @param context
 *      free-form context for debugging (may be NULL)
 * @return
 *      void
 */
void kafka_report_runtime_error(const char *module, const char *err_code,
    const char *err_msg, const char *context);

#ifdef __cplusplus
}
#endif

#endif /* _KAFKA_PRODUCER_H_ */
