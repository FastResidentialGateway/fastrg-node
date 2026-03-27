/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
   init.h

     Initiation of FastRG

  Designed by THE on Jan 26, 2021
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/
#ifndef _INIT_H_
#define _INIT_H_

#include <common.h>

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
STATUS sys_init(FastRG_t *fastrg_ccb);
void sys_cleanup(FastRG_t *fastrg_ccb);

extern struct rte_mempool *direct_pool[PORT_AMOUNT];
extern struct rte_mempool *indirect_pool[PORT_AMOUNT];
extern struct rte_ring    *cp_q, *free_mail_ring;

#endif
