#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

#include "etcd_integration.h"
#include "fastrg.h"
#include "dbg.h"
#include "utils.h"
#include "../northbound/controller/etcd_client.h"
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
        pppoe_command_received_callback,
        user_count_changed_callback,
        NULL, // No need to load DNS records here since they are loaded while PPPoE connections are established
        fastrg_ccb);

    if (load_status != ETCD_SUCCESS) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Failed to load existing configs, continuing anyway");
        // Continue even if loading fails - the watcher will still work for new changes
    }

    /* Write initial subscriber count to etcd */
    U16 current_subscriber_count = 0;
    etcd_status_t get_status = etcd_client_get_subscriber_count(fastrg_ccb->node_uuid, 
        &current_subscriber_count);
    if (get_status != ETCD_SUCCESS || current_subscriber_count != fastrg_ccb->user_count) {
        char subscriber_count_str[8];
        snprintf(subscriber_count_str, sizeof(subscriber_count_str), 
            "%u", fastrg_ccb->user_count);
        etcd_status_t put_status = etcd_client_put_subscriber_count(
            fastrg_ccb->node_uuid, subscriber_count_str, "fastrg_node_startup");
        if (put_status != ETCD_SUCCESS) {
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, 
                "Failed to write initial subscriber count to etcd for node: %s", fastrg_ccb->node_uuid);
            // Continue even if this fails
        } else {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
                "Wrote initial subscriber count (%s) to etcd for node: %s", 
                subscriber_count_str, fastrg_ccb->node_uuid);
        }
    }

    // Start etcd watching
    etcd_status_t status = etcd_client_start_watch(
        fastrg_ccb->node_uuid, 
        hsi_config_changed_callback, 
        pppoe_command_received_callback,
        user_count_changed_callback,
        sync_request_callback,
        dns_record_changed_callback);

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
    } else if (ccb_id >= fastrg_ccb->user_count && etcd_is_self_event(action, ccb_id, revision)) {
        // If ccb_id exceeds current user_count but is a self-event, ignore it
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
            "Ignoring self-initiated modification for out-of-range user %s (revision %" PRId64 ")", user_id, revision);
        return SUCCESS;
    }

    switch (action) {
        /* CREATE action is treated as an update with is_update = FALSE */
        case HSI_ACTION_CREATE:
            is_update = FALSE;
            /* fallthrough */
        case HSI_ACTION_UPDATE:
            // Check if this is a self-initiated modification (e.g., from modify_hsi_config_status)
            if (etcd_is_self_event(HSI_ACTION_UPDATE, ccb_id, revision)) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
                    "Ignoring self-initiated modification for user %s (revision %" PRId64 ")", user_id, revision);
                return SUCCESS;
            }

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
            // If apply failed, delete the config from etcd to maintain consistency
            if (ret == ERROR) {
                int64_t delete_revision = 0;
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                    "Failed to apply HSI config for user %s, deleting from etcd", user_id);

                // Mark this as a self-initiated deletion with its revision
                etcd_mark_pending_event(HSI_ACTION_DELETE, ccb_id);

                // Delete and capture the revision
                etcd_status_t etcd_ret = etcd_client_delete_hsi_config(node_id, user_id, &delete_revision);
                if (etcd_ret == ETCD_SUCCESS) {
                    // Confirm with revision
                    etcd_confirm_pending_event(HSI_ACTION_DELETE, ccb_id, delete_revision);
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
                        "Deleted HSI config from etcd for user %s (revision %ld)", user_id, delete_revision);
                } else {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                        "Failed to delete HSI config from etcd for user %s", user_id);
                    etcd_remove_event(HSI_ACTION_DELETE, ccb_id);
                }
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
            }
            break;

        case HSI_ACTION_DELETE:
            // Check if this is a self-initiated deletion (from failed apply above)
            if (etcd_is_self_event(HSI_ACTION_DELETE, ccb_id, revision)) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
                    "Ignoring self-initiated delete event for user %s (revision %ld)", user_id, revision);
                ret = SUCCESS;
                break;
            }

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
                    char subscriber_count_str[8] = { 0 };
                    snprintf(subscriber_count_str, sizeof(subscriber_count_str), "%u", current_count);
                    etcd_client_put_subscriber_count(fastrg_ccb->node_uuid, subscriber_count_str, "fastrg_node");
                    return ERROR;
                }

                // Add DHCP CCBs
                if (dhcpd_add_ccb(fastrg_ccb, to_add) != SUCCESS) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                        "Failed to add %u DHCP CCBs", to_add);
                    pppd_disable_ccb(fastrg_ccb, to_add, current_count + to_add); // Disable the PPPoE CCBs that were just added
                    char subscriber_count_str[8] = { 0 };
                    snprintf(subscriber_count_str, sizeof(subscriber_count_str), "%u", current_count);
                    etcd_client_put_subscriber_count(fastrg_ccb->node_uuid, subscriber_count_str, "fastrg_node");
                    return ERROR;
                }

                if (fastrg_modify_subscriber_count(fastrg_ccb, new_count, current_count) != SUCCESS) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                        "Failed to modify internal subscriber count to %d", new_count);
                    pppd_disable_ccb(fastrg_ccb, to_add, current_count + to_add); // Disable the PPPoE CCBs that were just added
                    dhcpd_disable_ccb(fastrg_ccb, to_add, current_count + to_add); // Disable the DHCP CCBs that were just added
                    char subscriber_count_str[8] = { 0 };
                    snprintf(subscriber_count_str, sizeof(subscriber_count_str), "%u", current_count);
                    etcd_client_put_subscriber_count(fastrg_ccb->node_uuid, subscriber_count_str, "fastrg_node");
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

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
        "Sync request received after etcd reconnection, writing local state to etcd");

    // Write subscriber count to etcd
    char subscriber_count_str[8];
    snprintf(subscriber_count_str, sizeof(subscriber_count_str), "%u", fastrg_ccb->user_count);
    etcd_status_t sc_status = etcd_client_put_subscriber_count(
        fastrg_ccb->node_uuid, subscriber_count_str, "etcd_reconnect_sync");

    if (sc_status == ETCD_SUCCESS) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
            "Wrote subscriber count (%s) to etcd after reconnection", subscriber_count_str);
    } else {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, 
            "Failed to write subscriber count to etcd after reconnection");
    }

    // Write all active HSI configs to etcd
    // Note: We only write configs that are currently active in the local system
    // This assumes that the ccb array contains the active configurations
    int written_count = 0;
    for(int ccb_id=0; ccb_id<fastrg_ccb->user_count; ccb_id++) {
        ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
        hsi_config_t config = { 0 };

        /* Copy CCB data to hsi_config_t */
        snprintf(config.user_id, sizeof(config.user_id), "%d", ccb_id + 1);
        snprintf(config.vlan_id, sizeof(config.vlan_id), "%d", rte_atomic16_read(&ppp_ccb->vlan_id));
        /* vlan id 0 means this subscriber is inactive */
        if (rte_atomic16_read(&ppp_ccb->vlan_id) == 0) {
            FastRG_LOG(DBG, fastrg_ccb->fp, NULL, NULL, 
                "Skipping inactive subscriber %u during etcd reconnection sync", ccb_id + 1);
            continue;
        }
        strncpy(config.account_name, (const char *)ppp_ccb->ppp_user_acc, sizeof(config.account_name) - 1);
        strncpy(config.password, (const char *)ppp_ccb->ppp_passwd, sizeof(config.password) - 1);
        struct in_addr s_ip_addr = {
            .s_addr = dhcp_ccb->per_lan_user_pool[0]->ip_pool.ip_addr,
        };
        struct in_addr e_ip_addr = {
            .s_addr = dhcp_ccb->per_lan_user_pool[dhcp_ccb->per_lan_user_pool_len - 1]->ip_pool.ip_addr,
        };
        char s_ip_addr_str[INET_ADDRSTRLEN];
        char e_ip_addr_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &s_ip_addr, s_ip_addr_str, sizeof(s_ip_addr_str)) == NULL || 
                inet_ntop(AF_INET, &e_ip_addr, e_ip_addr_str, sizeof(e_ip_addr_str)) == NULL) {
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, 
                "Failed to convert DHCP address pool to string for subscriber %u during etcd reconnection sync", ccb_id + 1);
            continue;
        }
        snprintf(config.dhcp_addr_pool, sizeof(config.dhcp_addr_pool), "%s~%s", 
            s_ip_addr_str, e_ip_addr_str);
        struct in_addr subnet_mask_addr = {
            .s_addr = dhcp_ccb->subnet_mask,
        };
        char subnet_mask_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &subnet_mask_addr, subnet_mask_str, sizeof(subnet_mask_str)) == NULL) {
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, 
                "Failed to convert subnet mask to string for subscriber %u during etcd reconnection sync", ccb_id + 1);
            continue;
        }
        snprintf(config.dhcp_subnet, sizeof(config.dhcp_subnet), "%s", subnet_mask_str);
        struct in_addr gateway_addr = {
            .s_addr = dhcp_ccb->dhcp_server_ip,
        };
        char gateway_addr_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &gateway_addr, gateway_addr_str, sizeof(gateway_addr_str)) == NULL) {
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, 
                "Failed to convert gateway IP to string for subscriber %u during etcd reconnection sync", ccb_id + 1);
            continue;
        }
        snprintf(config.dhcp_gateway, sizeof(config.dhcp_gateway), "%s", gateway_addr_str);

        /* Populate port-mapping from local port_fwd_table */
        /* First pass: count active entries to size the heap allocation */
        int active_port_mapping_count = 0;
        for(U32 eport=0; eport<PORT_FWD_TABLE_SIZE; eport++) {
            if (rte_atomic16_read(&ppp_ccb->port_fwd_table[eport].is_active) != 0)
                active_port_mapping_count++;
        }
        config.port_mappings = NULL;
        config.port_mapping_count = 0;
        if (active_port_mapping_count > 0)
            config.port_mappings = malloc(active_port_mapping_count * sizeof(port_mapping_t));
        if (config.port_mappings != NULL) {
            for(U32 eport=0; eport<PORT_FWD_TABLE_SIZE; eport++) {
                if (rte_atomic16_read(&ppp_ccb->port_fwd_table[eport].is_active) == 0)
                    continue;
                port_mapping_t *pm = &config.port_mappings[config.port_mapping_count];
                pm->eport = (U16)eport;
                struct in_addr pm_addr = { .s_addr = ppp_ccb->port_fwd_table[eport].dip };
                if (inet_ntop(AF_INET, &pm_addr, pm->dip, sizeof(pm->dip)) == NULL)
                    continue;
                pm->dport = ppp_ccb->port_fwd_table[eport].iport;
                config.port_mapping_count++;
            }
        }

        hsi_enable_status_t enable_status = ENABLE_STATUS_DISABLED;
        if (ppp_ccb->phase == DATA_PHASE) {
            enable_status = ENABLE_STATUS_ENABLED;
        } else if (ppp_ccb->phase <= END_PHASE) {
            enable_status = ENABLE_STATUS_DISABLED;
        } else {
            enable_status = ENABLE_STATUS_ENABLING; // Treat other phases as in-progress
        }
        etcd_status_t hsi_status = etcd_client_put_hsi_config(
            fastrg_ccb->node_uuid, config.user_id, &config, enable_status, "etcd_reconnect_sync");
        hsi_config_free_port_mappings(&config);

        if (hsi_status == ETCD_SUCCESS) {
            written_count++;
        } else {
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, 
                "Failed to write HSI config for user %u to etcd after reconnection", ccb_id + 1);
        }
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
        "Wrote %d HSI config(s) to etcd after reconnection", written_count);

    /* Write static DNS records for all active subscribers back to etcd */
    int dns_written_count = 0;
    for(int dns_ccb_id=0; dns_ccb_id<fastrg_ccb->user_count; dns_ccb_id++) {
        ppp_ccb_t *dns_ppp_ccb = PPPD_GET_CCB(fastrg_ccb, dns_ccb_id);
        if (rte_atomic16_read(&dns_ppp_ccb->vlan_id) == 0)
            continue; /* skip inactive subscribers */

        dhcp_ccb_t *dns_dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, dns_ccb_id);
        if (dns_dhcp_ccb == NULL || !dns_dhcp_ccb->dns_state.dns_proxy_enabled)
            continue;

        dns_static_table_t *tbl = &dns_dhcp_ccb->dns_state.static_table;
        char dns_user_id_str[8];
        snprintf(dns_user_id_str, sizeof(dns_user_id_str), "%d", dns_ccb_id + 1);

        for(U32 j=0; j<DNS_STATIC_MAX_RECORDS; j++) {
            dns_static_record_t *rec = &tbl->records[j];
            if (!rec->active)
                continue;

            struct in_addr dns_ip_addr = { .s_addr = rec->ip_addr };
            char dns_ip_str[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &dns_ip_addr, dns_ip_str, sizeof(dns_ip_str)) == NULL)
                continue;

            dns_record_config_t etcd_rec;
            memset(&etcd_rec, 0, sizeof(etcd_rec));
            strncpy(etcd_rec.domain, rec->domain, sizeof(etcd_rec.domain) - 1);
            strncpy(etcd_rec.ip, dns_ip_str, sizeof(etcd_rec.ip) - 1);
            etcd_rec.ttl = rec->ttl;

            if (etcd_client_put_dns_record(fastrg_ccb->node_uuid, dns_user_id_str,
                    &etcd_rec) == ETCD_SUCCESS) {
                dns_written_count++;
            } else {
                FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                    "Failed to write DNS record %s for user %s to etcd after reconnection",
                    rec->domain, dns_user_id_str);
            }
        }
    }
    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "Wrote %d DNS static record(s) to etcd after reconnection", dns_written_count);
}

#define PPPOE_ACTION_DIAL   "dial"
#define PPPOE_ACTION_HANGUP "hangup"
STATUS pppoe_command_received_callback(const char *node_id, const pppoe_command_t *command, void *user_data)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)user_data;
    STATUS ret = SUCCESS;
    int64_t revision = 0;

    if (!fastrg_ccb || !node_id || !command)
        return ERROR;

    int ccb_id = parse_user_id(command->user_id, fastrg_ccb->user_count);
    if (ccb_id < 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Invalid user_id in command: %s (valid range: 1-%d)", 
            command->user_id, fastrg_ccb->user_count);
        return ERROR;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
        "PPPoE command received: action=%s, user=%s, vlan=%s, account=%s", 
        command->action, command->user_id, command->vlan, command->account);

    if (strcmp(command->action, PPPOE_ACTION_DIAL) == 0) {
        // Execute PPPoE dial
        ret = execute_pppoe_dial(fastrg_ccb, ccb_id, command);
        if (ret == SUCCESS) {
            etcd_mark_pending_event(HSI_ACTION_UPDATE, ccb_id);
            if (etcd_client_modify_hsi_config_status(fastrg_ccb->node_uuid, command->user_id, 
                    ENABLE_STATUS_ENABLING, &revision) == ETCD_SUCCESS) {
                etcd_confirm_pending_event(HSI_ACTION_UPDATE, ccb_id, revision);
            } else {
                etcd_remove_event(HSI_ACTION_UPDATE, ccb_id);
            }
        }
    } else if (strcmp(command->action, PPPOE_ACTION_HANGUP) == 0) {
        // Execute PPPoE hangup
        ret = execute_pppoe_hangup(fastrg_ccb, ccb_id);
        if (ret == SUCCESS) {
            etcd_mark_pending_event(HSI_ACTION_UPDATE, ccb_id);
            if (etcd_client_modify_hsi_config_status(fastrg_ccb->node_uuid, command->user_id, 
                    ENABLE_STATUS_DISABLING, &revision) == ETCD_SUCCESS) {
                etcd_confirm_pending_event(HSI_ACTION_UPDATE, ccb_id, revision);
            } else {
                etcd_remove_event(HSI_ACTION_UPDATE, ccb_id);
            }
        }
    } else {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Unknown PPPoE action: %s", command->action);
        ret = ERROR;
    }

    return ret;
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
