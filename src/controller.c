#include <time.h>
#include <sys/sysinfo.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <rte_lcore.h>

#include "controller.h"
#include "fastrg.h"
#include "dbg.h"
#include "../northbound/controller/controller_client.h"

/**
 * @fn controller_heartbeat_send
 *
 * @brief Send one heartbeat to the controller and log the result
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @return
 *      void
 */
static void controller_heartbeat_send(FastRG_t *fastrg_ccb)
{
    if (fastrg_ccb->node_uuid == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Node UUID or local IP not available for heartbeat");
        return;
    }

    // Get host uptime in seconds since boot
    struct sysinfo si;
    time_t current_time = (sysinfo(&si) == 0) ? (time_t)si.uptime : time(NULL);

    U8 ip_addr[INET6_ADDRSTRLEN];
    if (get_local_ip_for_server(fastrg_ccb->controller_address, ip_addr, sizeof(ip_addr)) != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to get local IP for heartbeat");
        return;
    }

    controller_status_t status = controller_send_heartbeat(
        fastrg_ccb->node_uuid, (long)current_time, (const char *)ip_addr);
    if (status == CONTROLLER_SUCCESS) {
        FastRG_LOG(DBG, fastrg_ccb->fp, NULL, NULL, "Heartbeat sent successfully");
    } else {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Failed to send heartbeat, status: %d", status);
    }
}

/**
 * @fn controller_heartbeat_wait
 *
 * @brief Wait one heartbeat interval, returning early if a stop is requested
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @return
 *      TRUE to send the next heartbeat, FALSE if a stop was requested
 */
static BOOL controller_heartbeat_wait(FastRG_t *fastrg_ccb)
{
    controller_heartbeat_t *hb = &fastrg_ccb->heartbeat;
    struct timespec deadline;
    int ret = 0;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += fastrg_ccb->heartbeat_interval;

    pthread_mutex_lock(&hb->lock);
    while (hb->stop_requested == FALSE && ret != ETIMEDOUT)
        ret = pthread_cond_timedwait(&hb->cond, &hb->lock, &deadline);
    BOOL keep_running = (hb->stop_requested == FALSE);
    pthread_mutex_unlock(&hb->lock);

    return keep_running;
}

/**
 * @fn controller_heartbeat_thread
 *
 * @brief Send a heartbeat every heartbeat_interval seconds until a stop is requested
 *
 * @param arg
 *      FastRG control block
 * @return
 *      NULL
 */
static void *controller_heartbeat_thread(void *arg)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)arg;

    while (controller_heartbeat_wait(fastrg_ccb) == TRUE)
        controller_heartbeat_send(fastrg_ccb);

    return NULL;
}

/**
 * @fn controller_heartbeat_stop
 *
 * @brief Stop the heartbeat thread and wait for it to exit; no-op if it never started
 *
 * @details
 *      If the thread is inside a heartbeat RPC, the join waits for that RPC's
 *      deadlines to expire (up to 15 s in the re-register path).
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @return
 *      void
 */
static void controller_heartbeat_stop(FastRG_t *fastrg_ccb)
{
    controller_heartbeat_t *hb = &fastrg_ccb->heartbeat;

    if (hb->started == FALSE)
        return;

    pthread_mutex_lock(&hb->lock);
    hb->stop_requested = TRUE;
    pthread_cond_signal(&hb->cond);
    pthread_mutex_unlock(&hb->lock);

    int ret = pthread_join(hb->thread, NULL);
    if (ret != 0)
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
            "Failed to join heartbeat thread: %s", strerror(ret));
    hb->started = FALSE;
}

int controller_init(FastRG_t *fastrg_ccb)
{
    controller_heartbeat_t *hb = &fastrg_ccb->heartbeat;

    hb->started = FALSE;
    hb->stop_requested = FALSE;
    pthread_mutex_init(&hb->lock, NULL);
    // The heartbeat wait uses CLOCK_MONOTONIC so wall-clock jumps do not stretch or skip it
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&hb->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    if (!fastrg_ccb->controller_address) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Controller address not configured");
        return -1;
    }

    // Initialize controller client
    if (controller_client_init(fastrg_ccb->controller_address) != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to initialize controller client");
        return -1;
    }

    return 0;
}

void controller_cleanup(FastRG_t *fastrg_ccb)
{
    // Join the heartbeat thread first so only this thread uses the controller client from here on
    controller_heartbeat_stop(fastrg_ccb);

    // Report shutdown instead of unregistering: unregistering deletes nodes/<uuid>
    // so the node disappears from the controller UI, while reporting shutdown only
    // marks it inactive and keeps it visible. Registering on next boot flips it
    // back to active. A failure here must not hold up shutdown.
    controller_status_t status = controller_report_shutdown(fastrg_ccb->node_uuid);
    if (status == CONTROLLER_SUCCESS)
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Reported shutdown to controller, node marked inactive");
    else
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Failed to report shutdown to controller, status: %d", status);

    // Cleanup controller client
    controller_client_cleanup();

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Controller client cleaned up");
}

int controller_register_this_node(FastRG_t *fastrg_ccb)
{
    if (!fastrg_ccb->node_uuid || !fastrg_ccb->version) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Missing information for node registration");
        return -1;
    }

    U8 ip_addr[16];
    if (get_local_ip_for_server(fastrg_ccb->controller_address, ip_addr, sizeof(ip_addr)) != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to get local IP for controller connection");
        controller_client_cleanup();
        return -1;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Controller client initialized, local IP: %s", ip_addr);

    controller_status_t status = controller_register_node(
        fastrg_ccb->node_uuid,
        (const char *)ip_addr,
        fastrg_ccb->version,
        fastrg_ccb->central_office_location,
        fastrg_ccb->node_grpc_port
    );

    if (status == CONTROLLER_SUCCESS) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Node registered successfully with controller");

        // Started only after registration, so the controller client never has two users at once
        if (fastrg_create_pthread("fastrg_hbeat", controller_heartbeat_thread, fastrg_ccb,
                rte_lcore_id(), &fastrg_ccb->heartbeat.thread) != SUCCESS) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to start heartbeat thread");
            return -1;
        }
        fastrg_ccb->heartbeat.started = TRUE;

        return 0;
    } else {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to register node with controller, status: %d", status);
        return -1;
    }
}
