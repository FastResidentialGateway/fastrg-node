/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DHCPD.H

  Designed by THE on MAR 21, 2021
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DHCPD_H_
#define _DHCPD_H_

#include <common.h>

#include <rte_timer.h>
#include <rte_rcu_qsbr.h>

#include "../protocol.h"
#include "../fastrg.h"
#include "../dnsd/dnsd.h"

#define DHCP_CMD_DISABLE 0
#define DHCP_CMD_ENABLE  1

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

typedef struct dhcp_hdr dhcp_hdr_t;

typedef struct ip_pool {
    struct rte_ether_addr   mac_addr;
    U32                     ip_addr;
    BOOL                    used;
}ip_pool_t;

typedef struct lan_user_info {
    U8                      state;
    BOOL                    lan_user_used; // tmp used pool index
    struct rte_ether_addr   mac_addr;
    struct rte_timer        timer;
    U32                     timeout_secs;
}lan_user_info_t;

typedef struct dhcp_ccb_per_lan_user {
    lan_user_info_t     lan_user_info;
    ip_pool_t           ip_pool;
    dhcp_hdr_t          *dhcp_hdr;
    struct dhcp_ccb     *dhcp_ccb;
    U32                 pool_index;
}dhcp_ccb_per_lan_user_t;

typedef struct dhcp_ccb {
    U16                     ccb_id;
    struct rte_ether_hdr    *eth_hdr;
    vlan_header_t           *vlan_hdr;
    struct rte_ipv4_hdr     *ip_hdr;
    struct rte_udp_hdr      *udp_hdr;
    U32                     dhcp_server_ip;
    dhcp_ccb_per_lan_user_t **per_lan_user_pool;
    U32                     per_lan_user_pool_len;
    U32                     subnet_mask; // network order
    rte_atomic16_t          dhcp_bool; //boolean value for accept dhcp packets at data plane
    rte_atomic32_t          active_count; // count of processing dhcp packets
    struct rte_mempool      *dhcp_per_lan_user_mempool;
    FILE                    *log_fp;
    FastRG_t                *fastrg_ccb;
    dns_proxy_state_t       dns_state; /* per-subscriber DNS proxy state */
}dhcp_ccb_t;

int dhcpd(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    struct rte_ether_hdr *eth_hdr, vlan_header_t *vlan_header, 
    struct rte_ipv4_hdr *ip_hdr, struct rte_udp_hdr *udp_hdr, U16 ccb_id);

/**
 * @fn dhcp_init
 * 
 * @brief Initialize DHCP module
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @return
 *      SUCCESS if init successfully, ERROR if init failed
 */
STATUS dhcp_init(FastRG_t *fastrg_ccb);

/**
 * @fn dhcpd_add_ccb
 * 
 * @brief Add DHCP control blocks
 * @param fastrg_ccb 
 *      FastRG control block pointer
 * @param extra_ccb_count 
 *      Number of extra CCBs to add
 * @return 
 *      SUCCESS if added successfully, ERROR if failed
 */
STATUS dhcpd_add_ccb(FastRG_t *fastrg_ccb, U16 extra_ccb_count);

/**
 * @fn dhcpd_disable_ccb
 * 
 * @brief Disable DHCP control blocks, reserve memory region for future use
 * @param fastrg_ccb 
 *      FastRG control block pointer
 * @param disable_ccb_count 
 *      Number of CCBs to disable
 * @param old_ccb_count
 *      Old number of CCBs before disabling
 * @return 
 *      SUCCESS if disabled successfully, ERROR if failed
 */
STATUS dhcpd_disable_ccb(FastRG_t *fastrg_ccb, U16 disable_ccb_count, U16 old_ccb_count);

/**
 * @fn dhcpd_remove_ccb
 * 
 * @brief Remove DHCP control blocks
 * @param fastrg_ccb 
 *      FastRG control block pointer
 * @param remove_ccb_count 
 *      Number of CCBs to remove
 * @param old_ccb_count
 *      Old number of CCBs before removal
 * @return 
 *      SUCCESS if removed successfully, ERROR if failed
 */
STATUS dhcpd_remove_ccb(FastRG_t *fastrg_ccb, U16 remove_ccb_count, U16 old_ccb_count);

/**
 * @fn dhcpd_cleanup_ccb
 * 
 * @brief Cleanup DHCP control blocks
 * @param fastrg_ccb 
 *      FastRG control block pointer
 * @param total_ccb_count
 *      Total number of CCBs
 */
void dhcpd_cleanup_ccb(FastRG_t *fastrg_ccb, U16 total_ccb_count);

/**
 * @fn dhcp_pool_init_by_user
 * 
 * @brief Initialize DHCP IP pool for a user/subscriber
 * @param dhcp_ccb 
 *      DHCP control block pointer
 * @param dhcp_server_ip 
 *      DHCP server IP address
 * @param ip_start 
 *      Start IP address of the pool
 * @param ip_end 
 *      End IP address of the pool
 * @param subnet_mask 
 *      Subnet mask
 */
void dhcp_pool_init_by_user(dhcp_ccb_t *dhcp_ccb, U32 dhcp_server_ip, 
    U32 ip_start, U32 ip_end, U32 subnet_mask);

void release_lan_user(struct rte_timer *tim, 
    dhcp_ccb_per_lan_user_t *per_lan_user_pool);

/**
 * @fn dhcpd_get_ccb
 * 
 * @brief Get DHCP control block by ccb id
 * @param fastrg_ccb_ptr 
 *      FastRG control block pointer
 * @param ccb_id 
 *      CCB ID
 * @return 
 *      dhcp_ccb_t *
 */
#define DHCPD_GET_CCB(fastrg_ccb_ptr, ccb_id) \
    dhcpd_get_ccb((fastrg_ccb_ptr)->dhcp_ccb_rcu, \
        (dhcp_ccb_t ** const *)&(fastrg_ccb_ptr)->dhcp_ccb, \
        (ccb_id))

static __always_inline dhcp_ccb_t *dhcpd_get_ccb(struct rte_rcu_qsbr *dhcp_ccb_rcu, 
    dhcp_ccb_t ** const *dhcp_ccb_array_ptr, U16 ccb_id)
{
    unsigned int lcore_id = 0;

    if (likely(rte_lcore_id() != LCORE_ID_ANY))
        lcore_id = rte_lcore_id();
    // RCU read-side critical section
    rte_rcu_qsbr_thread_online(dhcp_ccb_rcu, lcore_id);
    dhcp_ccb_t **dhcp_ccb_array = __atomic_load_n(dhcp_ccb_array_ptr, __ATOMIC_ACQUIRE);
    dhcp_ccb_t *result = __atomic_load_n(&dhcp_ccb_array[ccb_id], __ATOMIC_ACQUIRE);
    rte_rcu_qsbr_quiescent(dhcp_ccb_rcu, lcore_id);
    rte_rcu_qsbr_thread_offline(dhcp_ccb_rcu, lcore_id);

    return result;
}

#endif
