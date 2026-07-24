#ifndef _CONFIG_SNAPSHOT_H_
#define _CONFIG_SNAPSHOT_H_

#include <stdint.h>
#include <common.h>
#include "etcd_client.h"   /* hsi_config_callback_t / user_count_changed_callback_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent local snapshot of the three etcd config families
 *
 * The node is read-only on etcd. The snapshot mirrors the last-watched etcd
 * values and is the base for offline (etcd-unreachable) edits: every offline
 * edit re-stamps metadata (rv+1, updatedAt, updatedBy) and
 * marks the entry dirty. On reconnect only dirty entries are diffed against
 * etcd (metadata excluded) and, when different, reported to the
 * controller as a full ConfigOfflineEdit over Kafka — the snapshot never
 * writes etcd. Watch updates overwrite entries and clear dirty, so a
 * merely-stale snapshot can never turn into a proposal. */

typedef enum {
    SNAPSHOT_KIND_HSI = 0,   /* configs/{node}/hsi/{user}   */
    SNAPSHOT_KIND_DNS = 1,   /* configs/{node}/dns/{user}   */
    SNAPSHOT_KIND_COUNT = 2, /* user_counts/{node}/ (user_id should be "0" while updating snapshot) */
} snapshot_kind_t;

/**
 * @fn config_snapshot_init
 * @brief Load the persisted snapshot from disk (missing file = empty
 *        snapshot). Call once at startup, before the etcd client starts.
 * @return SUCCESS, or ERROR when an existing file is unreadable/corrupt
 *         (the snapshot then starts empty; the error is informational)
 */
STATUS config_snapshot_init(void);

/**
 * @fn config_snapshot_cleanup
 * @brief Persist and release the in-memory snapshot.
 * @return void
 */
void config_snapshot_cleanup(void);

/**
 * @fn config_snapshot_watch_update
 * @brief Record the etcd value seen by the watch/load path. Overwrites the
 *        entry and clears its dirty flag; value_json == NULL records a key
 *        deletion. Never generates a proposal.
 * @param kind snapshot family
 * @param user_id subscriber id ("0" for SNAPSHOT_KIND_COUNT)
 * @param value_json raw etcd value, or NULL when the key was deleted
 * @return void
 */
void config_snapshot_watch_update(snapshot_kind_t kind, const char *user_id,
    const char *value_json);

/**
 * @fn config_snapshot_get
 * @brief Return the current snapshot value for a key.
 * @param kind snapshot family
 * @param user_id subscriber id
 * @return malloc'd JSON string (caller frees), or NULL when absent
 */
char *config_snapshot_get(snapshot_kind_t kind, const char *user_id);

/**
 * @fn config_snapshot_offline_edit
 * @brief Store the post-edit full value of a key. Stamps metadata 
 *        using the CURRENT snapshot entry as the base (absent →
 *        rv "1", unparsable rv → "2", else cur+1; updatedAt = now UTC,
 *        updatedBy = "fastrg-node-offline"), marks the entry dirty, records
 *        edited_at and appends edit_summary.
 * @param kind snapshot family
 * @param user_id subscriber id
 * @param new_value_json full post-edit value (its metadata, if any, is
 *        replaced by the stamp)
 * @param edit_summary short human-readable description of this edit
 * @return SUCCESS or ERROR (bad input / persist failure)
 */
STATUS config_snapshot_offline_edit(snapshot_kind_t kind, const char *user_id,
    const char *new_value_json, const char *edit_summary);

/**
 * @fn config_snapshot_offline_delete
 * @brief Record an offline deletion of a key as a tombstone:
 *        marks the entry exists=false, dirty=true, edited_at=now, keeping the
 *        pre-delete value so its last-known resourceVersion can be carried on
 *        the proposal. A no-op (SUCCESS) when the key is already absent.
 *        config_snapshot_foreach_dirty then surfaces the tombstone with a
 *        NULL value_json so the reconnect sync emits a delete proposal.
 * @param kind snapshot family
 * @param user_id subscriber id
 * @param edit_summary short human-readable description of this delete
 * @return SUCCESS or ERROR (bad input / persist failure)
 */
STATUS config_snapshot_offline_delete(snapshot_kind_t kind, const char *user_id,
    const char *edit_summary);

/**
 * @fn config_snapshot_content_equal
 * @brief Canonical structural equality with the metadata object excluded on
 *        both sides: field order and whitespace insensitive.
 * @param json_a first value (NULL treated as absent)
 * @param json_b second value
 * @return TRUE when equal
 */
BOOL config_snapshot_content_equal(const char *json_a, const char *json_b);

/* Callback for config_snapshot_foreach_dirty. resource_version is the value
 * stamped into the entry's metadata; edited_at is the unix time (seconds) of
 * the last offline edit; edit_summary accumulates all edits since the entry
 * last became clean. value_json == NULL marks a tombstone (offline delete); 
 * resource_version then carries the key's last-known rv. */
typedef void (*snapshot_dirty_cb_t)(snapshot_kind_t kind, const char *user_id,
    const char *value_json, const char *resource_version, int64_t edited_at,
    const char *edit_summary, void *user_data);

/**
 * @fn config_snapshot_foreach
 * @brief Invoke cb for every existing entry (dirty or not). Used by the
 *        boot-time fallback when etcd is unreachable and the snapshot is the
 *        operating base.
 * @param cb callback
 * @param user_data opaque pointer passed through
 * @return void
 */
void config_snapshot_foreach(snapshot_dirty_cb_t cb, void *user_data);

/**
 * @fn config_snapshot_foreach_dirty
 * @brief Invoke cb for every dirty entry (reconnect sync).
 * @param cb callback
 * @param user_data opaque pointer passed through
 * @return void
 */
void config_snapshot_foreach_dirty(snapshot_dirty_cb_t cb, void *user_data);

/**
 * @fn config_snapshot_clear_dirty
 * @brief Clear an entry's dirty flag and accumulated summary (after the
 *        reconnect sync handled it).
 * @param kind snapshot family
 * @param user_id subscriber id
 * @return void
 */
void config_snapshot_clear_dirty(snapshot_kind_t kind, const char *user_id);

/* Offline field-edit kinds for config_snapshot_field_merge(): the node applies
 * the change locally and merges the same edit onto its persisted snapshot;
 * the snapshot is reported for controller arbitration on reconnect. */
#define SNAPSHOT_FIELD_KIND_DNS_PROXY     "dns_proxy"      /* value: "true"/"false" */
#define SNAPSHOT_FIELD_KIND_TCP_CONNTRACK "tcp_conntrack"  /* value: "true"/"false" */
#define SNAPSHOT_FIELD_KIND_SNAT_UPSERT   "snat_upsert"    /* value: {"eport","dip","dport"} */
#define SNAPSHOT_FIELD_KIND_SNAT_REMOVE   "snat_remove"    /* value: eport string */
#define SNAPSHOT_FIELD_KIND_DNS_ADD       "dns_add"        /* value: {"domain","ip","ttl"} */
#define SNAPSHOT_FIELD_KIND_DNS_DEL       "dns_del"        /* value: domain string */
#define SNAPSHOT_FIELD_KIND_DESIRE        "desire_status"  /* value: "connect"/"disconnect" */

/**
 * @fn config_snapshot_field_merge
 *
 * @brief Pure JSON merge applying one field-level edit onto a config
 *        value (see the kind defines for value formats). HSI kinds require an
 *        existing config object; dns_add creates the records envelope when
 *        absent; dns_del on an absent key fails (nothing to delete). Used by
 *        the offline-edit path to apply edits onto the local snapshot;
 *        exposed for direct unit testing.
 * @param kind
 *        One of the SNAPSHOT_FIELD_KIND_* strings
 * @param current_json
 *        Current etcd value, or NULL when the key is absent
 * @param value
 *        The queued field value (format per kind)
 * @param out_json
 *        Output: malloc'd merged JSON on SUCCESS; caller frees
 * @return
 *        SUCCESS or ERROR (merge not applicable / bad input)
 */
STATUS config_snapshot_field_merge(const char *kind, const char *current_json,
    const char *value, char **out_json);

/**
 * @fn config_snapshot_apply_all
 *
 * @brief boot-time fallback: apply the persisted snapshot as the operating
 *        base when etcd is unreachable at startup. The snapshot's subscriber
 *        count is applied first, then every HSI config, through the same
 *        callbacks the etcd load path uses (HSI JSON is parsed via
 *        etcd_client_parse_hsi_config — the schema is the etcd client's
 *        domain). DNS records load lazily on PPPoE establishment as usual.
 * @param node_id
 *      node UUID
 * @param hsi_callback
 *      callback applying one HSI config
 * @param user_count_callback
 *      callback applying the subscriber count
 * @param user_data
 *      opaque pointer passed through to the callbacks
 * @return
 *      void
 */
void config_snapshot_apply_all(const char *node_id,
    hsi_config_callback_t hsi_callback,
    user_count_changed_callback_t user_count_callback,
    void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* _CONFIG_SNAPSHOT_H_ */
