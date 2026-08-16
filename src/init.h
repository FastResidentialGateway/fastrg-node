/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
   init.h

     Initiation of FastRG

  Designed by THE on Jan 26, 2021
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/
#ifndef _INIT_H_
#define _INIT_H_

#include <common.h>

#include "config.h"

typedef struct FastRG FastRG_t;

#define PORT_AMOUNT 2

typedef enum {
    NIC_VENDOR_UNKNOWN = 0,
    NIC_VENDOR_MLX5    = 1,
    NIC_VENDOR_IXGBE   = 2,
    NIC_VENDOR_I40E    = 3,
    NIC_VENDOR_ICE     = 4,
    NIC_VENDOR_VMXNET3 = 5
} nic_vendor_t;

int setup_signalfd(void);
void fastrg_compute_max_user_count(FastRG_t *fastrg_ccb);
STATUS sys_init(FastRG_t *fastrg_ccb, struct fastrg_config *fastrg_cfg);
void sys_cleanup(FastRG_t *fastrg_ccb);

#ifdef UNIT_TEST
void fastrg_set_hugepage_free_bytes_for_test(uint64_t free_bytes);
#endif

/**
 * @fn metrics_server_run
 * @brief pthread entry point for the Prometheus /metrics HTTP server. Registers
 *        the metrics thread's RCU reader slot, binds lighthttp on the configured
 *        address and serves GET /metrics. Publishes the bind verdict through the
 *        startup latch, then exits the thread if the bind failed.
 *
 * @param arg
 *      FastRG control block (FastRG_t *)
 */
void *metrics_server_run(void *arg);

/**
 * @fn metrics_server_wait_ready
 * @brief Block until the metrics thread has published whether its listener came
 *        up. The bind happens on that thread, so this is how the startup path
 *        gets to see the verdict and abort on a failure instead of running on
 *        with no way to be scraped.
 *
 *        Each metrics thread launch pairs with exactly one wait: the verdict is
 *        consumed here and the latch re-arms for the next launch.
 *
 * @return
 *      0 when the listener is up, -1 when it failed to start
 */
int metrics_server_wait_ready(void);

extern struct rte_mempool *direct_pool[PORT_AMOUNT];
extern struct rte_mempool *indirect_pool[PORT_AMOUNT];

#endif
