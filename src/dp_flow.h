#ifndef _DP_FLOW_H_
#define _DP_FLOW_H_

#include <rte_flow.h>

#include <common.h>

/**
 * @fn setup_port_flows - Install all rte_flow rules for a port.
 *
 * @brief Rules installed (highest priority first):
 *   Port 0 (LAN):
 *     P0: VLAN / IPv4 TCP                        -> RSS queues 1..N
 *     P0: VLAN / IPv4 UDP                        -> RSS queues 1..N
 *   Port 1 (WAN, PPPoE):
 *     P0: PPPoE Discovery (ETH 0x8863)           -> queue 0
 *     ICE/E810 only (rte_flow pattern path):
 *       P0: eth/pppoes/IPv4/TCP                  -> RSS queues 1..N
 *       P0: eth/pppoes/IPv4/UDP                  -> RSS queues 1..N
 *       P0: eth/pppoes/IPv6/TCP                  -> RSS queues 1..N
 *       P0: eth/pppoes/IPv6/UDP                  -> RSS queues 1..N
 *     i40e+DDP only (PCTYPE mapping path, see i40e_setup_pppoe_rss):
 *       PCTYPE 15 (PPPoE/IPv4) -> SW flow type 28 -> RSS enabled
 *       PCTYPE 16 (PPPoE/IPv6) -> SW flow type 29 -> RSS enabled
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

/**
 * @fn i40e_load_ddp_package
 *
 * @brief Load a DDP package onto all i40e (X710) ports via the
 *        rte_pmd_i40e_process_ddp_package() API.  Must be called after
 *        rte_eal_init() but before rte_eth_dev_configure() so that the
 *        new profile is active when queues and flow rules are set up.
 *
 * @param fastrg_ccb
 *      FastRG control block (used for logging).
 * @param pkg_path
 *      Filesystem path to the DDP .pkg file (e.g. "/lib/firmware/…/gtp.pkg").
 * @return
 *      SUCCESS if the package was loaded on at least one i40e port,
 *      ERROR otherwise.
 */
STATUS i40e_load_ddp_package(FastRG_t *fastrg_ccb, const char *pkg_path);

#endif
