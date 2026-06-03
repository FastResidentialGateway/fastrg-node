#ifndef _KAFKA_PRODUCER_H_
#define _KAFKA_PRODUCER_H_

/*
 * Node -> controller telemetry over Kafka (point 5/6).
 *
 * Reports config-apply results, PPPoE state transitions and runtime errors to
 * topic "fastrg.node.events" (partition key = node_uuid). Payloads are protobuf
 * NodeEvent messages defined in docs/contracts/kafka-events.proto.
 *
 * Non-blocking: produce never blocks the data/control plane; on a full local
 * queue the message is dropped and counted (telemetry tolerates loss). Durable
 * (disk-WAL) buffering across node restarts is a later hardening slice (15).
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

/* Initialize the Kafka producer. brokers = comma-separated host:port list.
 * Returns SUCCESS on success, ERROR otherwise. Safe to call once at startup. */
STATUS kafka_producer_init(const char *brokers, const char *node_uuid);

/* Flush and destroy the producer. */
void kafka_producer_cleanup(void);

/* 1 if the producer is initialized and ready, else 0. */
int kafka_producer_is_ready(void);

/* Report a PPPoE state transition. hsi_ipv4/hsi_ipv4_gw used on "connected"
 * (may be NULL); err_msg used on abnormal transitions (may be NULL). */
void kafka_report_pppoe_state(const char *user_id, kafka_pppoe_phase_t phase,
    const char *hsi_ipv4, const char *hsi_ipv4_gw, const char *err_msg);

/* Report the result of applying an HSI config. action = "create"|"update"|"delete".
 * On failure pass err_code/err_msg (may be NULL on success). */
void kafka_report_config_apply(const char *user_id, const char *action,
    BOOL success, const char *err_code, const char *err_msg);

/* Report a generic runtime error. context may be NULL. */
void kafka_report_runtime_error(const char *module, const char *err_code,
    const char *err_msg, const char *context);

#ifdef __cplusplus
}
#endif

#endif /* _KAFKA_PRODUCER_H_ */
