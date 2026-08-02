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
    /* TRUE when ip_addr is a network/broadcast address (lowest octet .0 or
     * .255) that must never be leased. The flag is negative on purpose so a
     * zero-initialized entry stays assignable, and it is kept separate from
     * "used" because release_lan_user() clears "used" when a lease expires. */
    BOOL                    reserved;
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
    U32                     pool_start; // host order
    U32                     pool_end; // host order
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
 * @fn dhcpd_cleanup_ccb
 *
 * @brief Cleanup DHCP control blocks. All threads must already be stopped.
 * @param fastrg_ccb
 *      FastRG control block pointer
 */
void dhcpd_cleanup_ccb(FastRG_t *fastrg_ccb);

/**
 * @fn dhcp_pool_init_by_user
 *
 * @brief Initialize (or re-window) the DHCP IP pool for a subscriber
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
 * @return
 *      SUCCESS if the window was applied, ERROR if the requested range
 *      exceeds the dhcp pool maximum capacity (state left untouched)
 */
STATUS dhcp_pool_init_by_user(dhcp_ccb_t *dhcp_ccb, U32 dhcp_server_ip, 
    U32 ip_start, U32 ip_end, U32 subnet_mask);

/**
 * @fn dhcp_ip_is_reserved
 *
 * @brief Tell whether an address is a network/broadcast address that must
 *      never be leased to a client. A pool range may cross an octet
 *      boundary, so the decision is made on the address itself (lowest
 *      octet) instead of being derived from the configured subnet mask.
 * @param host_order_ip
 *      IPv4 address in host byte order
 * @return
 *      TRUE if the lowest octet is 0 or 255, FALSE otherwise
 */
static inline BOOL dhcp_ip_is_reserved(U32 host_order_ip)
{
    U8 host_byte = host_order_ip & 0xff;

    return (host_byte == 0x00 || host_byte == 0xff) ? TRUE : FALSE;
}

/**
 * @fn dhcp_validate_pool_range
 *
 * @brief Validate a subscriber DHCP pool range before it is applied, so an
 *      inconsistent configuration is rejected instead of silently producing
 *      a pool the data plane cannot serve
 * @param dhcp_server_ip
 *      DHCP server IP address, network byte order
 * @param ip_start
 *      Start IP address of the pool, network byte order
 * @param ip_end
 *      End IP address of the pool, network byte order
 * @param subnet_mask
 *      Subnet mask, network byte order
 * @return
 *      SUCCESS if start <= end and both ends share the DHCP server subnet,
 *      or if every parameter is 0 (the pool-clearing call); ERROR otherwise
 */
STATUS dhcp_validate_pool_range(U32 dhcp_server_ip, U32 ip_start,
    U32 ip_end, U32 subnet_mask);

void release_lan_user(struct rte_timer *tim, 
    dhcp_ccb_per_lan_user_t *per_lan_user_pool);

/**
 * @fn DHCPD_GET_CCB
 *
 * @brief Get DHCP control block by ccb id
 *
 * @param fastrg_ccb_ptr
 *      FastRG control block pointer
 * @param ccb_id
 *      CCB ID
 * @return
 *      dhcp_ccb_t *
 */
#define DHCPD_GET_CCB(fastrg_ccb_ptr, ccb_id) \
    ((dhcp_ccb_t *)(fastrg_ccb_ptr)->dhcp_ccb[(ccb_id)])

#endif
