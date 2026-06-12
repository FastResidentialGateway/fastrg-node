#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <rte_cycles.h>

#include "etcd_integration.h"
#include "fastrg.h"
#include "dbg.h"
#include "utils.h"
#include "../northbound/controller/etcd_client.h"
#include "../northbound/controller/kafka_producer.h"
#include "pppd/pppd.h"
#include "dhcpd/dhcpd.h"
#include "avl_tree.h"
#include "northbound.h"

// Track self-initiated events to avoid processing them again
#define PENDING_EVENT_TIMEOUT_SECONDS (24 * 60 * 60)  // 1 day in seconds
#define EVENT_RETRY_DELAY_MS 500   // Retry delay in milliseconds
#define EVENT_MAX_RETRIES 5       // Maximum retry attempts

typedef enum {
    PENDING_STATE_WAITING,    // Waiting for revision
    PENDING_STATE_CONFIRMED   // Have revision
} pending_state_t;

// Data structure for storing multiple revisions for multiple events
typedef struct revision_node {
    int64_t revision; // Only used in CONFIRMED state
    pending_state_t state;
    struct revision_node *next;
} revision_node_t;

// Data structure for self-triggered events
typedef struct self_triggered_event {
    U8 action; // e.g., delete/modify
    U8 reference;
    U16 ccb_id;
    revision_node_t *revision_list;  // Linked list of revisions and event states
    time_t timestamp; // When this entry was marked
} self_triggered_event_t;

static avl_tree_t *pending_events_tree = NULL;
static pthread_mutex_t pending_events_mutex = PTHREAD_MUTEX_INITIALIZER;

// Comparison function for self_triggered_event (compares by revision and ccb_id)
static int compare_events(const void *data1, const void *data2)
{
    const self_triggered_event_t *e1 = (const self_triggered_event_t*)data1;
    const self_triggered_event_t *e2 = (const self_triggered_event_t*)data2;

    // key: ccb_id and action
    if (e1->ccb_id < e2->ccb_id)
        return -1;
    if (e1->ccb_id > e2->ccb_id)
        return 1;

    if (e1->action < e2->action)
        return -1;
    if (e1->action > e2->action)
        return 1;

    return 0;
}

// Helper function to add revision to the list
static void add_revision_to_list(self_triggered_event_t *event)
{
    revision_node_t *new_node = fastrg_malloc(revision_node_t, sizeof(revision_node_t), 0);
    if (!new_node)
        return;

    new_node->revision = -1;
    new_node->next = event->revision_list;
    new_node->state = PENDING_STATE_WAITING;
    event->revision_list = new_node;
}

// Helper function to check if revision exists in the list
static BOOL revision_exists_in_list(const self_triggered_event_t *event, int64_t revision)
{
    revision_node_t *current = event->revision_list;
    while (current) {
        if (current->revision == revision)
            return TRUE;
        current = current->next;
    }
    return FALSE;
}

static BOOL has_still_waiting(const self_triggered_event_t *event)
{
    revision_node_t *current = event->revision_list;
    while (current) {
        if (current->state == PENDING_STATE_WAITING)
            return TRUE;
        current = current->next;
    }
    return FALSE;
}

static void assign_revision_to_list(self_triggered_event_t *event, int64_t revision)
{
    revision_node_t *current = event->revision_list;
    while (current) {
        if (current->revision == -1) {
            current->revision = revision;
            current->state = PENDING_STATE_CONFIRMED;
            return;
        }
        current = current->next;
    }
    // If no empty slot, add a new node
    revision_node_t *new_node = fastrg_malloc(revision_node_t, sizeof(revision_node_t), 0);
    if (new_node == NULL)
        return;
    new_node->revision = revision;
    new_node->state = PENDING_STATE_CONFIRMED;
    new_node->next = event->revision_list;
    event->revision_list = new_node;
}

// Helper function to remove a revision from the list
static void remove_revision_from_list(self_triggered_event_t *event, int64_t revision)
{
    revision_node_t **current = &event->revision_list;

    while (*current) {
        if ((*current)->revision == revision) {
            revision_node_t *to_delete = *current;
            *current = (*current)->next;
            fastrg_mfree(to_delete);
            return;
        }
        current = &(*current)->next;
    }
}

// Helper function to free all revisions in the list
static void free_revision_list(revision_node_t *list)
{
    while (list) {
        revision_node_t *next = list->next;
        fastrg_mfree(list);
        list = next;
    }
}

// Free function for self_triggered_event
static void free_event(void *data)
{
    if (data) {
        self_triggered_event_t *event = (self_triggered_event_t *)data;
        free_revision_list(event->revision_list);
        fastrg_mfree(data);
    }
}

// Predicate to check if event is too old
typedef struct {
    time_t now;
} cleanup_context_t;

static bool is_event_expired(const void *data, void *context)
{
    const self_triggered_event_t *event = (const self_triggered_event_t*)data;
    const cleanup_context_t *ctx = (const cleanup_context_t*)context;

    time_t age = ctx->now - event->timestamp;
    return age >= PENDING_EVENT_TIMEOUT_SECONDS;
}

// Add a pending event before etcd operation
void etcd_mark_pending_event(etcd_action_type_t action, U16 ccb_id)
{
    pthread_mutex_lock(&pending_events_mutex);

    // Initialize tree if needed
    if (pending_events_tree == NULL) {
        pending_events_tree = avl_tree_create(compare_events, free_event, NULL);
        if (pending_events_tree == NULL) {
            pthread_mutex_unlock(&pending_events_mutex);
            return;
        }
    }

    // Clean up old entries (older than 1 day)
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    time_t now = now_ts.tv_sec;

    cleanup_context_t ctx = { .now = now };
    avl_tree_delete_if(pending_events_tree, is_event_expired, &ctx);

    // Create and add new pending event to the AVL tree
    self_triggered_event_t search_key = { .action = action, .ccb_id = ccb_id };
    self_triggered_event_t *existing = (self_triggered_event_t *)avl_tree_search(pending_events_tree, &search_key);

    if (existing) {
        // Update existing: reset to WAITING state
        existing->timestamp = now;
        if (existing->reference < UINT8_MAX) {
            add_revision_to_list(existing);
            existing->reference++;
        }
    } else {
        // Create new entry in WAITING state
        self_triggered_event_t *event = fastrg_malloc(self_triggered_event_t, sizeof(self_triggered_event_t), 0);
        if (event) {
            event->revision_list = fastrg_malloc(revision_node_t, sizeof(revision_node_t), 0);
            if (event->revision_list == NULL) {
                fastrg_mfree(event);
                pthread_mutex_unlock(&pending_events_mutex);
                return;
            }
            event->action = action;
            event->ccb_id = ccb_id;
            event->revision_list->revision = -1;
            event->revision_list->state = PENDING_STATE_WAITING;
            event->revision_list->next = NULL;
            event->timestamp = now;
            event->reference = 1; // Initialize reference
            avl_tree_insert(pending_events_tree, event);
        }
    }

    pthread_mutex_unlock(&pending_events_mutex);
}

void etcd_confirm_pending_event(etcd_action_type_t action, U16 ccb_id, int64_t revision)
{
    pthread_mutex_lock(&pending_events_mutex);

    if (pending_events_tree == NULL) {
        pthread_mutex_unlock(&pending_events_mutex);
        return;
    }

    self_triggered_event_t search_key = { .action = action, .ccb_id = ccb_id };
    self_triggered_event_t *found = (self_triggered_event_t *)avl_tree_search(pending_events_tree, &search_key);

    if (found) {
        // Add revision to the list
        assign_revision_to_list(found, revision);
    }

    pthread_mutex_unlock(&pending_events_mutex);
}

// Check if event is self-initiated by matching revision and remove from tracking
BOOL etcd_is_self_event(etcd_action_type_t action, U16 ccb_id, int64_t revision)
{
    for(int retry=0; retry<EVENT_MAX_RETRIES; retry++) {
        pthread_mutex_lock(&pending_events_mutex);

        if (!pending_events_tree) {
            pthread_mutex_unlock(&pending_events_mutex);
            return FALSE;
        }

        // Search for the event
        self_triggered_event_t search_key = {
            .action = action,
            .ccb_id = ccb_id,
            .revision_list = NULL, // Not used in comparison
            .timestamp = 0  // Not used in comparison
        };

        self_triggered_event_t *found = (self_triggered_event_t *)avl_tree_search(pending_events_tree, &search_key);
        if (found) {
            if (revision_exists_in_list(found, revision)) {
                // Match! This is self-event
                found->reference--;
                remove_revision_from_list(found, revision);
                if (found->reference == 0)
                    avl_tree_delete(pending_events_tree, &search_key);
                pthread_mutex_unlock(&pending_events_mutex);
                return TRUE;
            } else if (has_still_waiting(found)) {
                // Still WAITING for revision confirmation
                pthread_mutex_unlock(&pending_events_mutex);

                // Wait a bit and retry
                if (retry < EVENT_MAX_RETRIES - 1) {
                    posix_sleep_ms(EVENT_RETRY_DELAY_MS);
                    continue;
                } else {
                    // Max retries reached, treat as external event
                    return FALSE;
                }
            } else {
                // Revision doesn't match - not self-event
                pthread_mutex_unlock(&pending_events_mutex);
                return FALSE;
            }
        } else {
            // Not found
            pthread_mutex_unlock(&pending_events_mutex);
            if (retry < EVENT_MAX_RETRIES - 1)
                posix_sleep_ms(EVENT_RETRY_DELAY_MS);
        }
    }

    return FALSE;
}

void etcd_remove_event(etcd_action_type_t action, U16 ccb_id)
{
    pthread_mutex_lock(&pending_events_mutex);

    if (!pending_events_tree) {
        pthread_mutex_unlock(&pending_events_mutex);
        return;
    }

    // Search for the event
    self_triggered_event_t search_key = {
        .action = action,
        .ccb_id = ccb_id,
        .revision_list = NULL,
        .timestamp = 0  // Not used in comparison
    };

    // Remove the event if it exists
    avl_tree_delete(pending_events_tree, &search_key);

    pthread_mutex_unlock(&pending_events_mutex);
}

STATUS etcd_integration_init(FastRG_t *fastrg_ccb) 
{
    if (!fastrg_ccb)
        return ERROR;

    etcd_status_t status = etcd_client_init(fastrg_ccb->etcd_endpoints, (void *)fastrg_ccb);
    if (status != ETCD_SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to initialize etcd client with endpoints: %s", fastrg_ccb->etcd_endpoints);
        return ERROR;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Etcd client initialized with endpoints: %s", fastrg_ccb->etcd_endpoints);
    return SUCCESS;
}

STATUS etcd_integration_start(FastRG_t *fastrg_ccb)
{
    if (fastrg_ccb == NULL || fastrg_ccb->node_uuid == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp ? fastrg_ccb->fp : stdout, NULL, NULL, 
            "Invalid FastRG context or missing node UUID");
        return ERROR;
    }

    // Load existing HSI configs from etcd before starting the watcher
    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Loading existing HSI configs for node: %s", fastrg_ccb->node_uuid);
    etcd_status_t load_status = etcd_client_load_existing_configs(
        fastrg_ccb->node_uuid,
        hsi_config_changed_callback,
        user_count_changed_callback,
        NULL, // No need to load DNS records here since they are loaded while PPPoE connections are established
        fastrg_ccb);

    if (load_status != ETCD_SUCCESS) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Failed to load existing configs, continuing anyway");
        // Continue even if loading fails - the watcher will still work for new changes
    }

    /* The node no longer seeds config/subscriber-count into etcd: config is owned
     * by the CLI/controller. The node only reads (watch) and, when offline, queues
     * CLI-originated writes for flush (slice 10). */

    /* Load any offline config-write queue persisted from a previous session; the
     * watchdog flushes it to etcd once etcd is reachable. */
    etcd_client_queue_load();

    // Start etcd watching. Watch/reconcile events are delivered to fastrg_loop
    // via FastRG_t.etcd_event_q; only sync_request_callback is passed through.
    etcd_status_t status = etcd_client_start_watch(
        fastrg_ccb->node_uuid, sync_request_callback);

    if (status != ETCD_SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to start etcd watching for node: %s", fastrg_ccb->node_uuid);
        return ERROR;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Etcd integration started for node: %s", fastrg_ccb->node_uuid);
    return SUCCESS;
}

void etcd_integration_stop(FastRG_t *fastrg_ccb)
{
    if (!fastrg_ccb)
        return;

    // Stop etcd watching
    etcd_client_stop_watch();

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Etcd integration stopped");
}

void etcd_integration_cleanup(FastRG_t *fastrg_ccb)
{
    etcd_integration_stop(fastrg_ccb);
    etcd_client_cleanup();

    // Clean up the pending events AVL tree
    pthread_mutex_lock(&pending_events_mutex);
    if (pending_events_tree) {
        avl_tree_destroy(pending_events_tree);
        pending_events_tree = NULL;
    }
    pthread_mutex_unlock(&pending_events_mutex);

    if (fastrg_ccb)
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Etcd integration cleaned up");
}

int parse_user_id(const char *user_id_str, int max_count)
{
    if (!user_id_str || user_id_str[0] == '\0')
        return -1;

    char *endptr;
    long val = strtol(user_id_str, &endptr, 10);

    // Check conversion error
    if (endptr == user_id_str || *endptr != '\0')
        return -1;

    // Convert to 0-based index and validate range
    int ccb_id = (int)val - 1;
    if (ccb_id < 0)
        return -1;

    return ccb_id;
}

BOOL hsi_config_matches_local(const char *user_id,
    const hsi_config_t *etcd_config, void *user_data)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)user_data;
    if (!fastrg_ccb || !user_id || !etcd_config)
        return FALSE;

    int ccb_id = parse_user_id(user_id, fastrg_ccb->user_count);
    if (ccb_id < 0 || !is_valid_ccb_id(fastrg_ccb, ccb_id))
        return FALSE;

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
    if (!ppp_ccb || !dhcp_ccb)
        return FALSE;

    U16 etcd_vlan;
    if (parse_vlan_id(etcd_config->vlan_id, &etcd_vlan) == ERROR) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: bad etcd vlan_id=\"%s\"", user_id, etcd_config->vlan_id);
        return FALSE;
    }
    U16 local_vlan = (U16)rte_atomic16_read(&ppp_ccb->vlan_id);
    if (local_vlan != etcd_vlan) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: VLAN mismatch local=%u etcd=%u", user_id, local_vlan, etcd_vlan);
        return FALSE;
    }

    if (ppp_ccb->ppp_user_acc == NULL ||
        strcmp((const char *)ppp_ccb->ppp_user_acc, etcd_config->account_name) != 0) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: account mismatch local=\"%s\" etcd=\"%s\"",
            user_id, ppp_ccb->ppp_user_acc ? (const char *)ppp_ccb->ppp_user_acc : "(null)",
            etcd_config->account_name);
        return FALSE;
    }
    if (ppp_ccb->ppp_passwd == NULL ||
        strcmp((const char *)ppp_ccb->ppp_passwd, etcd_config->password) != 0) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: password mismatch", user_id);
        return FALSE;
    }

    U32 etcd_gw, etcd_subnet, etcd_ip_start, etcd_ip_end;
    if (parse_ip(etcd_config->dhcp_gateway, &etcd_gw) == ERROR ||
            parse_ip(etcd_config->dhcp_subnet, &etcd_subnet) == ERROR ||
            parse_ip_range(etcd_config->dhcp_addr_pool, &etcd_ip_start, &etcd_ip_end) == ERROR) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: failed to parse etcd dhcp fields", user_id);
        return FALSE;
    }
    if (dhcp_ccb->dhcp_server_ip != etcd_gw) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: gateway mismatch local=0x%08x etcd=0x%08x",
            user_id, dhcp_ccb->dhcp_server_ip, etcd_gw);
        return FALSE;
    }
    if (dhcp_ccb->subnet_mask != etcd_subnet) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: subnet mismatch local=0x%08x etcd=0x%08x",
            user_id, dhcp_ccb->subnet_mask, etcd_subnet);
        return FALSE;
    }

    U32 expected_pool_len = rte_be_to_cpu_32(etcd_ip_end) >= rte_be_to_cpu_32(etcd_ip_start) ?
        rte_be_to_cpu_32(etcd_ip_end) - rte_be_to_cpu_32(etcd_ip_start) + 1 :
        rte_be_to_cpu_32(etcd_ip_start) - rte_be_to_cpu_32(etcd_ip_end) + 1;
    if (dhcp_ccb->per_lan_user_pool_len != expected_pool_len) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: pool_len mismatch local=%u etcd=%u",
            user_id, dhcp_ccb->per_lan_user_pool_len, expected_pool_len);
        return FALSE;
    }
    if (expected_pool_len > 0 && dhcp_ccb->per_lan_user_pool &&
        dhcp_ccb->per_lan_user_pool[0] &&
        dhcp_ccb->per_lan_user_pool[0]->ip_pool.ip_addr != etcd_ip_start) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: pool_start mismatch local=0x%08x etcd=0x%08x",
            user_id, dhcp_ccb->per_lan_user_pool[0]->ip_pool.ip_addr, etcd_ip_start);
        return FALSE;
    }

    BOOL local_dns_proxy_enable = dhcp_ccb->dns_state.dns_proxy_enabled;
    BOOL etcd_dns_proxy_enable = etcd_config->dns_proxy_enable;
    if (local_dns_proxy_enable != etcd_dns_proxy_enable) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: dns_proxy_enable mismatch local=%s etcd=%s",
            user_id, local_dns_proxy_enable ? "true" : "false",
            etcd_dns_proxy_enable ? "true" : "false");
        return FALSE;
    }

    BOOL local_tcp_conntrack_enable = ppp_ccb->tcp_conntrack_enabled;
    BOOL etcd_tcp_conntrack_enable = etcd_config->tcp_conntrack_enable;
    if (local_tcp_conntrack_enable != etcd_tcp_conntrack_enable) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: tcp_conntrack_enable mismatch local=%s etcd=%s",
            user_id, local_tcp_conntrack_enable ? "true" : "false",
            etcd_tcp_conntrack_enable ? "true" : "false");
        return FALSE;
    }

    int local_active = 0;
    for(int i=0; i<PORT_FWD_TABLE_SIZE; i++) {
        if (rte_atomic16_read(&ppp_ccb->port_fwd_table[i].is_active) == 1)
            local_active++;
    }
    if (local_active != etcd_config->port_mapping_count) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Sync match[%s]: port_mapping count mismatch local=%d etcd=%d",
            user_id, local_active, etcd_config->port_mapping_count);
        return FALSE;
    }
    for(int i=0; i<etcd_config->port_mapping_count; i++) {
        const port_mapping_t *pm = &etcd_config->port_mappings[i];
        if (pm->eport >= PORT_FWD_TABLE_SIZE)
            return FALSE;
        const port_fwd_entry_t *e = &ppp_ccb->port_fwd_table[pm->eport];
        if (rte_atomic16_read((rte_atomic16_t *)&e->is_active) != 1) {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                "Sync match[%s]: port_fwd[%u] inactive in local", user_id, pm->eport);
            return FALSE;
        }
        if (e->iport != pm->dport) {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                "Sync match[%s]: port_fwd[%u] iport mismatch local=%u etcd=%u",
                user_id, pm->eport, e->iport, pm->dport);
            return FALSE;
        }
        U32 etcd_dip;
        if (parse_ip(pm->dip, &etcd_dip) == ERROR || e->dip != etcd_dip) {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                "Sync match[%s]: port_fwd[%u] dip mismatch local=0x%08x etcd=\"%s\"",
                user_id, pm->eport, e->dip, pm->dip);
            return FALSE;
        }
    }

    return TRUE;
}

BOOL dns_record_matches_local(const char *user_id,
    const dns_record_config_t *etcd_record, void *user_data)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)user_data;
    if (!fastrg_ccb || !user_id || !etcd_record)
        return FALSE;

    int ccb_id = atoi(user_id) - 1;
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id))
        return FALSE;

    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
    if (!dhcp_ccb)
        return FALSE;

    dns_static_record_t *local_rec =
        dns_static_lookup(&dhcp_ccb->dns_state.static_table, etcd_record->domain);
    if (!local_rec || !local_rec->active)
        return FALSE;

    U32 etcd_ip = 0;
    if (etcd_record->ip[0] != '\0' && parse_ip(etcd_record->ip, &etcd_ip) == ERROR)
        return FALSE;
    if (local_rec->ip_addr != etcd_ip)
        return FALSE;
    if (local_rec->ttl != etcd_record->ttl)
        return FALSE;

    return TRUE;
}

/* Reconcile the live PPPoE session of a subscriber toward its desired state.
 * desire_status ("connect"/"disconnect"; empty treated as disconnect) is the only
 * source of PPPoE intent, set exclusively by the CLI/controller. Idempotent:
 * execute_pppoe_dial/hangup skip when the session is already in the target state.
 *
 * TODO(slice 13): add dial-rate limiting (stagger) so a node restart that loads
 * many desire_status=connect subscribers does not issue all PADIs at once.
 */
/* Minimum spacing between PPPoE dials, to avoid a PADI storm when a node
 * restart loads many desire_status=connect subscribers at once (slice 13). */
#define PPPOE_DIAL_MIN_GAP_US 50000   /* 50 ms */

static void reconcile_pppoe_desire(FastRG_t *fastrg_ccb, int ccb_id, const char *desire_status)
{
    static uint64_t s_last_dial_cycles = 0;   /* control-plane thread only */

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    if (ppp_ccb == NULL)
        return;

    BOOL want_connect = (desire_status != NULL &&
        strcmp(desire_status, DESIRE_STATUS_CONNECT) == 0);
    BOOL is_connected = (rte_atomic16_read(&ppp_ccb->ppp_bool) == 1);

    if (want_connect && !is_connected) {
        /* Stagger consecutive dials: enforce a minimum gap since the last one.
         * Only consecutive dials (bulk restart/reconcile) ever wait; an isolated
         * dial proceeds immediately. */
        uint64_t hz = rte_get_tsc_hz();
        if (hz > 0 && s_last_dial_cycles != 0) {
            uint64_t min_gap = (hz / 1000000ULL) * PPPOE_DIAL_MIN_GAP_US;
            uint64_t elapsed = rte_get_tsc_cycles() - s_last_dial_cycles;
            if (elapsed < min_gap)
                rte_delay_us_block((min_gap - elapsed) / (hz / 1000000ULL));
        }
        s_last_dial_cycles = rte_get_tsc_cycles();
        rte_atomic16_set(&ppp_ccb->redial_pending, 0);  /* dialing now, no deferral needed */
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "desire_status=connect for user %d, dialing PPPoE", ccb_id + 1);
        execute_pppoe_dial(fastrg_ccb, ccb_id);
    } else if (!want_connect && is_connected) {
        rte_atomic16_set(&ppp_ccb->redial_pending, 0);  /* we want it down */
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "desire_status=disconnect for user %d, hanging up PPPoE", ccb_id + 1);
        execute_pppoe_hangup(fastrg_ccb, ccb_id);
    } else if (want_connect && is_connected && ppp_ccb->ppp_processing == TRUE) {
        /* Desire is connect but the session is still tearing down from an earlier
         * hangup (ppp_bool stays 1, and ppp_processing==TRUE, until END_PHASE).
         * execute_pppoe_dial would no-op here because ppp_bool==1, so the connect
         * would be silently dropped and only recovered by the next 60s reconcile
         * sweep. Instead, remember the intent: exit_ppp re-dials the moment the
         * teardown completes. (Loop-safe: ppp_processing is TRUE only while
         * disconnecting, so a failing dial-up never sets this.) */
        rte_atomic16_set(&ppp_ccb->redial_pending, 1);
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "desire_status=connect for user %d during teardown; will redial after it completes",
            ccb_id + 1);
    } else {
        /* Stable connected (and desired) or stable down (and not desired): nothing
         * to do, and no deferred redial should linger. */
        rte_atomic16_set(&ppp_ccb->redial_pending, 0);
    }
}

STATUS hsi_config_changed_callback(const char *node_id, const char *user_id,
    const hsi_config_t *config, etcd_action_type_t action,
    int64_t revision, void *user_data)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)user_data;
    STATUS ret = SUCCESS;
    BOOL is_update = TRUE;

    if (!fastrg_ccb || !node_id || !user_id)
        return ERROR;

    int ccb_id = parse_user_id(user_id, fastrg_ccb->user_count);
    if (ccb_id < 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Invalid user_id: %s (valid range: 1~%d)", 
            user_id, fastrg_ccb->user_count);
        return ERROR;
    }

    /* Self-event filtering already happened on the watcher thread before this
     * event was enqueued (see process_hsi_event), so no etcd_is_self_event()
     * check is needed here. */
    switch (action) {
        /* CREATE action is treated as an update with is_update = FALSE */
        case HSI_ACTION_CREATE:
            is_update = FALSE;
            /* fallthrough */
        case HSI_ACTION_UPDATE:
            if (!config) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Null config for HSI user %s", user_id);
                return ERROR;
            }

            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
                "HSI config %s for user %s (revision %" PRId64 "): VLAN=%s, Account=%s, DHCP_Pool=%s", 
                (action == HSI_ACTION_CREATE) ? "created" : "updated",
                user_id, revision, config->vlan_id, config->account_name, config->dhcp_addr_pool);

            // Apply HSI configuration
            ret = apply_hsi_config(fastrg_ccb, ccb_id, config, is_update);
            // The node no longer writes to etcd to "fix" a failed apply; etcd is
            // owned by the CLI/controller. The failure is reported to the controller
            // via Kafka (slice 11); here we just log. apply_hsi_config cleans up its
            // own local state on failure.
            if (ret == ERROR) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                    "Failed to apply HSI config for user %s (will be reported via Kafka)", user_id);
            } else {
                // HSI config applied successfully, now reconcile port-mapping rules
                if (config->port_mapping_count > 0 || action == HSI_ACTION_UPDATE) {
                    STATUS pm_ret = reconcile_port_mapping(fastrg_ccb, ccb_id,
                        config->port_mappings, config->port_mapping_count);
                    if (pm_ret == ERROR) {
                        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                            "Port-mapping reconciliation failed for user %s", user_id);
                        ret = ERROR; // Signal failure so caller writes fallback error
                    }
                }

                /* Reconcile the live PPPoE session toward the desired state.
                 * desire_status is the single source of truth for PPPoE intent
                 * (set only by CLI/controller). This runs for live updates,
                 * periodic reconcile, and startup load alike, so the node
                 * re-establishes/tears down sessions from desire_status alone.
                 * execute_pppoe_dial/hangup are idempotent (skip when already
                 * in the target state). */
                reconcile_pppoe_desire(fastrg_ccb, ccb_id, config->desire_status);
            }
            break;

        case HSI_ACTION_DELETE:
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
                "HSI config deleted for user %s (revision %ld)", user_id, revision);

            // Remove HSI configuration
            ret = remove_hsi_config(fastrg_ccb, ccb_id);
            break;

        default:
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Unknown HSI action type: %d", action);
            ret = ERROR;
            break;
    }

    return ret;
}

STATUS user_count_changed_callback(const char *node_id, 
    const user_count_config_t *config, etcd_action_type_t action,
    int64_t revision, void *user_data)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)user_data;
    STATUS ret = SUCCESS;

    if (!fastrg_ccb || !node_id || !config)
        return ERROR;

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "User count change request received (revision %" PRId64 "): action=%d, new_count=%d, current_count=%d",
        revision, action, config->user_count, fastrg_ccb->user_count);

    switch (action) {
        case HSI_ACTION_CREATE:
        case HSI_ACTION_UPDATE: {
            int new_count = config->user_count;
            int current_count = fastrg_ccb->user_count;

            if (new_count <= 0) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                    "Invalid user count: %d", new_count);
                return ERROR;
            }

            if (new_count == current_count) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                    "User count unchanged: %d", current_count);
                return SUCCESS;
            }

            if (new_count > current_count) {
                // Need to add CCBs
                U16 to_add = (U16)(new_count - current_count);
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                    "Adding %u CCBs (current: %d, target: %d)", to_add, current_count, new_count);

                // Add PPPoE CCBs
                if (pppd_add_ccb(fastrg_ccb, to_add) != SUCCESS) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                        "Failed to add %u PPPoE CCBs", to_add);
                    /* Node is read-only on etcd: report the failure via Kafka
                     * instead of writing the count back. The desired count stays
                     * in etcd; the resulting drift is visible to the controller. */
                    kafka_report_runtime_error("user_count", "CCB_ALLOC_FAILED",
                        "failed to add PPPoE CCBs for subscriber-count increase", NULL);
                    return ERROR;
                }

                // Add DHCP CCBs
                if (dhcpd_add_ccb(fastrg_ccb, to_add) != SUCCESS) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                        "Failed to add %u DHCP CCBs", to_add);
                    pppd_disable_ccb(fastrg_ccb, to_add, current_count + to_add); // Disable the PPPoE CCBs that were just added
                    kafka_report_runtime_error("user_count", "CCB_ALLOC_FAILED",
                        "failed to add DHCP CCBs for subscriber-count increase", NULL);
                    return ERROR;
                }

                if (fastrg_modify_subscriber_count(fastrg_ccb, new_count, current_count) != SUCCESS) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                        "Failed to modify internal subscriber count to %d", new_count);
                    pppd_disable_ccb(fastrg_ccb, to_add, current_count + to_add); // Disable the PPPoE CCBs that were just added
                    dhcpd_disable_ccb(fastrg_ccb, to_add, current_count + to_add); // Disable the DHCP CCBs that were just added
                    kafka_report_runtime_error("user_count", "COUNT_APPLY_FAILED",
                        "failed to apply internal subscriber-count increase", NULL);
                    return ERROR;
                }

                if (ret == SUCCESS) {
                    fastrg_ccb->user_count = new_count;
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                        "Successfully added %u CCBs, new user_count: %d", to_add, fastrg_ccb->user_count);
                }
            } else {
                // Need to remove CCBs
                U16 to_remove = (U16)(current_count - new_count);
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                    "Removing %u CCBs (current: %d, target: %d)", to_remove, current_count, new_count);
                fastrg_ccb->user_count = new_count;
                /* we don't need to remove CCBs explicitly because the ccbs maybe reused in the future */
                pppd_disable_ccb(fastrg_ccb, to_remove, current_count);
                dhcpd_disable_ccb(fastrg_ccb, to_remove, current_count);
                fastrg_disable_subscriber_stats(fastrg_ccb, to_remove, current_count);
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                    "Successfully removed %u CCBs, new user_count: %d", to_remove, fastrg_ccb->user_count);
            }
            break;
        }

        case HSI_ACTION_DELETE:
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                "User count config deleted (revision %" PRId64 ")", revision);
            // Deletion means we should keep the current user_count
            ret = SUCCESS;
            break;

        default:
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                "Unknown user count action type: %d", action);
            ret = ERROR;
            break;
    }

    return ret;
}

// Callback to handle sync request from etcd_client after reconnection
// This writes local state to etcd if etcd doesn't have the data
void sync_request_callback(const char *node_id, void *user_data)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)user_data;

    if (!fastrg_ccb || !node_id) {
        return;
    }

    /* etcd has no data for this node after (re)connection. The node is read-only
     * on etcd now: config and subscriber count are owned by the CLI/controller,
     * so the node does NOT seed them back. Local-vs-etcd reconciliation is handled
     * by the watch/reconcile path; CLI-originated offline writes are flushed from
     * the local queue (slice 10). */
    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "etcd (re)connected with no data for node %s; node is read-only on etcd, "
        "not seeding config back", node_id);
}

STATUS dns_record_changed_callback(const char *node_id, const char *user_id,
    const dns_record_config_t *record, etcd_action_type_t action,
    int64_t revision, void *user_data)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)user_data;
    if (!fastrg_ccb || !user_id || !record) {
        return ERROR;
    }

    int ccb_id = atoi(user_id) - 1;
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id)) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
            "DNS record callback: invalid ccb_id %d for user %s", ccb_id, user_id);
        return ERROR;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "DNS record event: user=%s domain=%s action=%d rev=%ld",
        user_id, record->domain, action, (long)revision);

    switch (action) {
        case HSI_ACTION_CREATE:
        case HSI_ACTION_UPDATE:
            return apply_dns_record(fastrg_ccb, ccb_id, record);
        case HSI_ACTION_DELETE:
            return remove_dns_record(fastrg_ccb, ccb_id, record->domain);
        default:
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                "DNS record callback: unknown action %d", action);
            return ERROR;
    }
}

/* Reconcile sweep: remove subscribers active locally but no longer present in
 * etcd. Runs on the control-plane loop, so reading CCB state here is race-free. */
static void etcd_reconcile_sweep(FastRG_t *fastrg_ccb, const int *present, int count)
{
    for(int ccb_id=0; ccb_id<fastrg_ccb->user_count; ccb_id++) {
        ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
        if (ppp_ccb == NULL || rte_atomic16_read(&ppp_ccb->vlan_id) == 0)
            continue;   /* not active locally */

        BOOL in_etcd = FALSE;
        for(int i=0; i<count; i++) {
            if (present[i] == ccb_id) {
                in_etcd = TRUE;
                break;
            }
        }
        if (in_etcd == FALSE) {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                "Reconcile sweep: user %d active locally but absent from etcd, removing",
                ccb_id + 1);
            remove_hsi_config(fastrg_ccb, ccb_id);
        }
    }
}

void etcd_event_dispatch(FastRG_t *fastrg_ccb, etcd_event_t *ev)
{
    if (fastrg_ccb == NULL || ev == NULL)
        return;

    switch (ev->kind) {
        case ETCD_EVENT_HSI: {
            const hsi_config_t *cfg =
                (ev->action == HSI_ACTION_DELETE) ? NULL : &ev->event_data.hsi.config;

            /* Reconcile events skip re-applying configs whose local state already
             * matches, but still reconcile the PPPoE session toward desire_status
             * so a dropped session is re-dialed (or a stale one torn down). */
            if (ev->from_reconcile && cfg != NULL &&
                    hsi_config_matches_local(ev->user_id, cfg, fastrg_ccb)) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                    "Reconcile: HSI user %s already matches local state, reconciling PPPoE only",
                    ev->user_id);
                int rc_ccb_id = parse_user_id(ev->user_id, fastrg_ccb->user_count);
                if (rc_ccb_id >= 0)
                    reconcile_pppoe_desire(fastrg_ccb, rc_ccb_id, cfg->desire_status);
                break;
            }

            STATUS ret = hsi_config_changed_callback(ev->node_id, ev->user_id, cfg,
                ev->action, ev->revision, fastrg_ccb);

            /* Report the apply result to the controller via Kafka for live watch
             * events only (reconcile/startup-load re-applies would be noise). The
             * etcd failed_events/ namespace is removed. */
            if (ev->from_reconcile == FALSE) {
                const char *action_str = (ev->action == HSI_ACTION_CREATE) ? "create"
                                       : (ev->action == HSI_ACTION_DELETE) ? "delete" : "update";
                if (ret == SUCCESS) {
                    kafka_report_config_apply(ev->user_id, action_str, TRUE, NULL, NULL);
                } else {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                        "HSI %s apply failed for user %s (reported via Kafka)", action_str, ev->user_id);
                    kafka_report_config_apply(ev->user_id, action_str, FALSE,
                        "apply_failed", "node failed to apply HSI config");
                }
            }
            break;
        }

        case ETCD_EVENT_USER_COUNT:
            user_count_changed_callback(ev->node_id, &ev->event_data.user_count,
                ev->action, ev->revision, fastrg_ccb);
            break;

        case ETCD_EVENT_DNS_RECORD:
            if (ev->from_reconcile && ev->action != HSI_ACTION_DELETE &&
                    dns_record_matches_local(ev->user_id, &ev->event_data.dns_record, fastrg_ccb)) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                    "Reconcile: DNS record for user %s already matches, skipping", ev->user_id);
                break;
            }
            dns_record_changed_callback(ev->node_id, ev->user_id, &ev->event_data.dns_record,
                ev->action, ev->revision, fastrg_ccb);
            break;

        case ETCD_EVENT_HSI_SWEEP:
            etcd_reconcile_sweep(fastrg_ccb, ev->event_data.sweep.present_ccb_ids, ev->event_data.sweep.count);
            break;

        default:
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                "etcd_event_dispatch: unknown event kind %d", ev->kind);
            break;
    }
}
