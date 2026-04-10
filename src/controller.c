#include <time.h>
#include <stdlib.h>
#include <string.h>

#include <rte_timer.h>
#include <rte_cycles.h>

#include "controller.h"
#include "fastrg.h"
#include "dbg.h"
#include "../northbound/controller/controller_client.h"

void controller_heartbeat_timer_cb(__rte_unused struct rte_timer *tim, void *arg)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)arg;

    if (fastrg_ccb->node_uuid == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Node UUID or local IP not available for heartbeat");
        return;
    }

    // Get current timestamp
    time_t current_time = time(NULL);

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

    // Reset the timer for next heartbeat
    uint64_t timer_ticks = fastrg_get_cycles_in_sec() * fastrg_ccb->heartbeat_interval;
    rte_timer_reset(&fastrg_ccb->heartbeat_timer, timer_ticks, SINGLE, 
        fastrg_ccb->lcore.timer_thread, controller_heartbeat_timer_cb, fastrg_ccb);
}

int controller_init(FastRG_t *fastrg_ccb)
{
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
    // Stop heartbeat timer
    rte_timer_stop(&fastrg_ccb->heartbeat_timer);

    U8 ip_addr[INET6_ADDRSTRLEN];
    if (get_local_ip_for_server(fastrg_ccb->controller_address, ip_addr, sizeof(ip_addr)) != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to get local IP for controller connection");
        return;
    }

    controller_unregister_node(fastrg_ccb->node_uuid, (const char *)ip_addr, fastrg_ccb->version);
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
        fastrg_ccb->version
    );

    if (status == CONTROLLER_SUCCESS) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Node registered successfully with controller");

        // Start heartbeat timer
        uint64_t timer_ticks = fastrg_get_cycles_in_sec() * fastrg_ccb->heartbeat_interval;
        rte_timer_reset(&fastrg_ccb->heartbeat_timer, timer_ticks, SINGLE,
                        fastrg_ccb->lcore.timer_thread, controller_heartbeat_timer_cb, fastrg_ccb);

        return 0;
    } else {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Failed to register node with controller, status: %d", status);
        return -1;
    }
}
