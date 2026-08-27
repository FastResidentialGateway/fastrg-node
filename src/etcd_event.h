#ifndef _ETCD_EVENT_H_
#define _ETCD_EVENT_H_

/* The etcd config value shapes and the event the watcher threads hand to the
 * control-plane loop through FastRG_t.etcd_event_q. */

#include <common.h>
#include <stdlib.h>
#include <string.h>

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
    BOOL ipv6_enable;           /* per-subscriber IPv6 enable; defaults to FALSE when absent in etcd */
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

// DNS static record structure for etcd
typedef struct {
    char domain[256];
    char ip[32];
    U32 ttl;
} dns_record_config_t;

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
    ETCD_EVENT_HSI_SWEEP       /* reconcile: keep ccb_ids present in etcd */
} etcd_event_kind_t;

typedef struct etcd_event {
    etcd_event_kind_t  kind;
    etcd_action_type_t action;          /* CREATE/UPDATE/DELETE; unused for sweep */
    int64_t            revision;        /* etcd ModRevision of the key this event carries */
    BOOL               from_reconcile;  /* TRUE: periodic reconcile; FALSE: live watch event */
    /* TRUE when the controller asked the node to restate this config. The
     * dispatcher confirms it as-is when local state already matches, and
     * otherwise re-applies it like any other event. */
    BOOL               from_republish;
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
    if (ev == NULL)
        return;
    if (ev->kind == ETCD_EVENT_HSI) {
        hsi_config_free_port_mappings(&ev->event_data.hsi.config);
    } else if (ev->kind == ETCD_EVENT_HSI_SWEEP) {
        free(ev->event_data.sweep.present_ccb_ids);
    }
    free(ev);
}

/**
 * @fn fastrg_alloc_etcd_event
 *
 * @brief allocate a zeroed etcd event of the given kind
 * @param kind
 *      which event the caller is about to fill in
 * @return
 *      the event, or NULL when out of memory; the caller owns it until it is
 *      enqueued, after which the control-plane loop frees it
 */
static inline etcd_event_t *fastrg_alloc_etcd_event(etcd_event_kind_t kind)
{
    etcd_event_t *ev = (etcd_event_t *)calloc(1, sizeof(*ev));

    if (ev != NULL)
        ev->kind = kind;

    return ev;
}

#endif
