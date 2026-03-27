#ifndef _DP_FLOW_H_
#define _DP_FLOW_H_

#include <rte_flow.h>

#include <common.h>

/**
 * @fn setup_port_flows - Install all rte_flow rules for a port.
 *
 * @brief Rules installed (highest priority first):
 *   P0: PPPoE Discovery (ETH 0x8863)          -> queue 0
 *   P0: PPPoE Session + inner IPv4/IPv6 TCP    -> RSS queues 1..N
 *   P0: PPPoE Session + inner IPv4/IPv6 UDP    -> RSS queues 1..N
 *   P1: PPPoE Session fallback (ETH 0x8864)    -> queue 0
 *   P1: IPv4/IPv6 TCP                          -> RSS queues 1..N
 *   P1: IPv4/IPv6 UDP                          -> RSS queues 1..N
 *
 * @param fastrg_ccb
 *      fastrg ccb
 * @param port_id
 *      DPDK port identifier.
 * @param total_queues
 *      Total queue count returned by fastrg_calc_queue_count().
 * @param error
 *      Output error descriptor.
 * @return
 *      0 on success, -1 on failure.
 */
int setup_port_flows(FastRG_t *fastrg_ccb, uint16_t port_id, uint16_t total_queues,
    struct rte_flow_error *error);

#endif
