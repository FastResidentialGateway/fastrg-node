#ifndef _KAFKA_PRODUCER_WAL_H_
#define _KAFKA_PRODUCER_WAL_H_

/*
 * kafka_producer.cpp's C++ internals, declared here so unit tests can reach
 * them. C++ only: the C side declared in kafka_producer.h never sees this.
 */

#include <common.h>

#include <cstdint>
#include <string>
#include <vector>

#include "proto/kafka-events.pb.h"

/*
 * On-disk form of the Kafka write-ahead log: a JSON array of
 * {"seq","payload"} objects, payload hex-encoded so arbitrary protobuf bytes
 * survive a text file.
 *
 * Only part of what the producer buffers reaches the file. A runtime error is
 * gone the moment the process is, so those events are the durable subset;
 * every other event type can be asked for again (a PPPoE republish, a config
 * status republish, an offline edit that stays dirty until Kafka acks it), so
 * they live in memory only. The file therefore holds runtime errors alone,
 * which is why it needs no type field: whatever is read back is replayed
 * as-is.
 */

/* One buffered event: what the producer holds in memory, some of which is
 * also written to the WAL. */
struct KafkaWalEvent {
    int64_t     seq = 0;     /* local monotonic id, used as the per-message opaque */
    std::string payload;     /* serialized NodeEvent protobuf bytes */
    /* Set when an attempt to hand this event to the broker failed. In memory
     * only: a restart replays the whole WAL anyway, so persisting it would add
     * a field to the file format for no gain. */
    bool        delivery_failed = false;
    /* Whether this event belongs in the file. In memory only, for the same
     * reason: everything the file holds is durable by definition. */
    bool        persistent = false;
};

/**
 * @fn kafka_wal_serialize
 *
 * @brief render the durable events as the WAL's JSON array text
 * @param events
 *      buffered events, in order; those with persistent set are written and
 *      the rest are left out, so the text is exactly what the file should hold
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
 *      receives the recovered events, replacing whatever it held; entries
 *      missing a field or carrying a malformed payload are skipped,
 *      delivery_failed comes back clear and persistent comes back set,
 *      because being in the file is what durable means
 * @return
 *      true when the text was usable, false when it is not a JSON array
 */
bool kafka_wal_parse(const std::string &data, std::vector<KafkaWalEvent> &out);

/**
 * @fn kafka_build_config_apply_result
 *
 * @brief fill in the ConfigApplyResult payload of a config-apply event
 * @param out
 *      payload to fill; every field is written, so a reused message carries
 *      nothing over from an earlier call
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
 *      TRUE when the event only restates a config the node was already running
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
