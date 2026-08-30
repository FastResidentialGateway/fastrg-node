#ifndef _KAFKA_PRODUCER_WAL_H_
#define _KAFKA_PRODUCER_WAL_H_

/* kafka_producer.cpp internals, exposed for unit tests. C++ only. */

#include <common.h>

#include <cstdint>
#include <string>
#include <vector>

#include "proto/kafka-events.pb.h"

/* On-disk WAL format: JSON array of {"seq","payload"}, payload hex-encoded.
 * Holds runtime errors only, so entries need no type field. */

/* One buffered event; written to the WAL only when persistent is set. */
struct KafkaWalEvent {
    int64_t     seq = 0;     /* local monotonic id, used as the per-message opaque */
    std::string payload;     /* serialized NodeEvent protobuf bytes */
    /* Retry flag, in memory only. */
    bool        delivery_failed = false;
    /* Whether this event belongs in the file; in memory only. */
    bool        persistent = false;
};

/**
 * @fn kafka_wal_serialize
 *
 * @brief render the durable events as the WAL's JSON array text
 * @param events
 *      buffered events, in order; only those with persistent set are written
 * @return
 *      the JSON text, "[]" when nothing durable is buffered
 */
std::string kafka_wal_serialize(const std::vector<KafkaWalEvent> &events);

/**
 * @fn kafka_wal_parse
 *
 * @brief read the WAL's JSON array text back into events
 * @param data
 *      WAL file contents; empty text is an empty WAL, not an error
 * @param out
 *      recovered events, replacing its contents; malformed entries skipped
 * @return
 *      true when the text was usable, false when it is not a JSON array
 */
bool kafka_wal_parse(const std::string &data, std::vector<KafkaWalEvent> &out);

/**
 * @fn kafka_build_config_apply_result
 *
 * @brief fill in the ConfigApplyResult payload of a config-apply event
 * @param out
 *      payload to fill; every field is written, so a reused message is safe
 * @param action
 *      "create" | "update" | "delete"; NULL leaves the field empty
 * @param success
 *      TRUE when the apply succeeded
 * @param err_code
 *      machine-readable error code on failure (may be NULL)
 * @param err_msg
 *      human-readable error description on failure (may be NULL)
 * @param applied_resource_version
 *      metadata.resourceVersion of the config this apply targeted (may be NULL)
 * @param republished
 *      TRUE when the controller requested a republish of this subscriber's
 *      config. The controller skips the audit record for restates, so a
 *      republish sweep cannot flood the audit trail.
 * @param applied_mod_revision
 *      etcd ModRevision of the config value the node applied; 0 means unknown
 * @return
 *      void
 */
void kafka_build_config_apply_result(fastrg::events::v1::ConfigApplyResult *out,
    const char *action, BOOL success, const char *err_code, const char *err_msg,
    const char *applied_resource_version, BOOL republished,
    int64_t applied_mod_revision);

#endif /* _KAFKA_PRODUCER_WAL_H_ */
