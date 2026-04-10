/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  PPPD.H

     For ppp detection

  Designed by THE on Jan 14, 2019
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _PPPD_H_
#define _PPPD_H_

#include <stdatomic.h>

#include <common.h>

#include <rte_timer.h>
#include <rte_memory.h>
#include <rte_ether.h>
#include <rte_rcu_qsbr.h>

#include "header.h"
#include "../fastrg.h"
#include "../mac_table.h"

#define PPP_MSG_BUF_LEN	        128

#define MULTICAST_TAG           4001
#define TOTAL_SOCK_PORT	        65536
#define MAX_NAT_ENTRIES         TOTAL_SOCK_PORT << 2
#define PORT_FWD_TABLE_SIZE     TOTAL_SOCK_PORT  /* direct-indexed by eport (0..65535) */

#define PPPoE_CMD_DISABLE       0
#define PPPoE_CMD_FORCE_DISABLE 1
#define PPPoE_CMD_ENABLE        2

/**
 * @brief SNAT port forwarding entry (direct-indexed by eport)
 *
 * The array index IS the external port number, so no eport field needed.
 * Maps WAN PPPoE IP:eport → LAN dip:iport.
 * Packed to 16 bytes so total table = 65536 * 16 = 1 MB per ppp_ccb.
 */
typedef struct port_fwd_entry {
    U32            dip;         /**< destination IP on LAN (network byte order) */
    U16            iport;       /**< internal port on LAN (host byte order) */
    rte_atomic16_t is_active;   /**< 1 = active, 0 = free */
    rte_atomic64_t hit_count;   /**< number of packets matched by this rule */
} __rte_cache_aligned port_fwd_entry_t;
/**
 * @brief hsi nat table structure
 */
typedef struct addr_table {
    struct rte_ether_addr mac_addr;
    U32                   src_ip; // original src ip from LAN user (e.g. 192.168.0.100)
    U32                   dst_ip; // dst ip where LAN user wants to visit (e.g. public ip)
    U16                   src_port; // original src port from LAN user
    U16                   dst_port; // dst port where LAN user wants to visit
    U16                   nat_port;
    rte_atomic16_t        is_fill; // is this entry filled or not
    rte_atomic16_t        is_alive; // counter for checking entry alive or not every second
}__rte_cache_aligned addr_table_t;

/**
 * @brief hsi control block structure
 */
typedef struct {
    FastRG_t              *fastrg_ccb;       /* pointer to fastrg control block */
    U16	                  user_num;          /* subscriptor id */
    rte_atomic16_t        vlan_id;           /* subscriptor vlan */
    struct rte_ether_hdr  eth_hdr;
    vlan_header_t         vlan_header __rte_aligned(sizeof(vlan_header_t));
    pppoe_header_t        pppoe_header __rte_aligned(sizeof(vlan_header_t));
    ppp_phase_t           ppp_phase[2];      /* store lcp and ipcp info, index 0 means lcp, index 1 means ipcp */
    pppoe_phase_t         pppoe_phase;       /* store pppoe info */
    U8                    cp:1;              /* cp is "control protocol", means we need to determine cp is LCP or NCP after parsing packet */
    U8                    phase:7;           /* pppoe connection phase */
    U16                   session_id;        /* pppoe session id */
    struct rte_ether_addr PPP_dst_mac;       /* pppoe server mac addr */
    U32                   hsi_ipv4;          /* ip addr pppoe server assign to pppoe client */
    U32                   hsi_ipv4_gw;       /* ip addr gateway pppoe server assign to pppoe client */
    U32                   hsi_primary_dns;   /* 1st dns addr pppoe server assign to pppoe client */
    U32                   hsi_secondary_dns; /* 2nd dns addr pppoe server assign to pppoe client */
    U8                    identifier;        /* ppp pkt id */
    U32                   magic_num;         /* ppp pkt magic number, in network order */
    BOOL                  is_pap_auth;       /* pap auth boolean flag */
    U16                   auth_method;       /* use chap or pap */
    U8                    *ppp_user_acc;     /* pap/chap account */
    U8                    *ppp_passwd;       /* pap/chap password */
    rte_atomic16_t        ppp_bool;          /* boolean flag for accept ppp packets at data plane */
    rte_atomic16_t        dp_start_bool;     /* hsi data plane starting boolean flag */
    BOOL                  ppp_processing;    /* boolean flag for checking ppp is disconnecting */
    addr_table_t          addr_table[MAX_NAT_ENTRIES]; /* hsi nat addr table */
    port_fwd_entry_t      port_fwd_table[PORT_FWD_TABLE_SIZE]; /* SNAT port forwarding, direct-indexed by eport */
    mac_table_entry_t     *mac_table;        /* per-subscriber LAN host MAC table (255^3 entries) */
    arp_pending_queue_t   arp_pq;            /* ARP pending queue for unresolved port-fwd destinations */
    struct rte_timer      pppoe;             /* pppoe timer */
    struct rte_timer      ppp;               /* ppp timer */
    struct rte_timer      nat;               /* nat table timer */
    struct rte_timer      ppp_alive;         /* PPP connection checking timer */
    struct rte_timer      etcd_pppoe_status_timer; /* etcd pppoe status checking timer */
    rte_atomic64_t        pppoes_rx_bytes;
    rte_atomic64_t        pppoes_tx_bytes;
    rte_atomic64_t        pppoes_rx_packets;
    rte_atomic64_t        pppoes_tx_packets;
}__rte_cache_aligned ppp_ccb_t;

extern U32 ppp_interval;

void   exit_ppp(ppp_ccb_t *ppp_ccb);

/**
 * @fn ppp_process
 * 
 * @brief PPPoE / PPP protocol processing
 * 
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param pkt_data
 *      Pointer to the PPPoE / PPP packet data
 * @param len
 *      Length of the packet data
 * 
 * @return SUCCESS if process successfully, ERROR if process failed
 */
STATUS ppp_process(FastRG_t *fastrg_ccb, U8 *pkt_data, U16 len);

STATUS ppp_connect(ppp_ccb_t *ppp_ccb);
STATUS ppp_disconnect(ppp_ccb_t *ppp_ccb);
STATUS ppp_update_config_by_user(ppp_ccb_t *ppp_ccb, U16 vlan_id, const char *user_name, 
    const char *password);
STATUS ppp_init_config_by_user(FastRG_t *fastrg_ccb, ppp_ccb_t *ppp_ccb, U16 ccb_id, 
    U16 vlan_id, const char *user_name, const char *password);
void   ppp_cleanup_config_by_user(ppp_ccb_t *ppp_ccb, U16 ccb_id);
void   PPP_bye_timer_cb(__attribute__((unused)) struct rte_timer *tim, 
    ppp_ccb_t *ppp_ccb);

/**
 * @fn pppd_init
 * 
 * @brief PPPoE / PPP protocol initialization function
 * 
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @return
 *      SUCCESS if init successfully, ERROR if init failed
 */
STATUS pppd_init(FastRG_t *fastrg_ccb);

/**
 * @fn PPP_bye
 * 
 * @brief PPPoE / PPP connection closing processing function
 * 
 * @param ppp_ccb
 *      PPP control block pointer
 * @return
 *      void
 */
void PPP_bye(ppp_ccb_t *ppp_ccb);

/**
 * @fn pppd_add_ccb
 * 
 * @brief Add more ppp control blocks
 * 
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param extra_ccb_count
 *      Number of extra ccbs to add
 * @return 
 *      SUCCESS if added successfully, ERROR if failed
 */
STATUS pppd_add_ccb(FastRG_t *fastrg_ccb, U16 extra_ccb_count);

/**
 * @fn pppd_disable_ccb
 * 
 * @brief Disable ppp control blocks, reserve memory region for future use
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param remove_ccb_count
 *      Number of ccbs to disable
 * @param old_ccb_count
 *      Old number of ccbs before disable
 * @return 
 *      SUCCESS if disabled successfully, ERROR if failed
 */
STATUS pppd_disable_ccb(FastRG_t *fastrg_ccb, U16 remove_ccb_count, U16 old_ccb_count);

/**
 * @fn pppd_remove_ccb
 * 
 * @brief Remove ppp control blocks
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param remove_ccb_count
 *      Number of ccbs to remove
 * @param old_ccb_count
 *      Old number of ccbs before removal
 * @return 
 *      SUCCESS if removed successfully, ERROR if failed
 */
STATUS pppd_remove_ccb(FastRG_t *fastrg_ccb, U16 remove_ccb_count, U16 old_ccb_count);

/**
 * @fn pppd_cleanup_ccb
 * 
 * @brief Cleanup all ppp control blocks
 * 
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param total_ccb_count
 *      Total number of ccbs
 */
void pppd_cleanup_ccb(FastRG_t *fastrg_ccb, U16 total_ccb_count);

/**
 * @fn PPPD_GET_CCB
 * 
 * @brief 
 *      Get ppp control block by ccb id
 * 
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param ccb_id 
 *      CCB ID
 * @return 
 *      ppp_ccb_t *
 */
#define PPPD_GET_CCB(fastrg_ccb_ptr, ccb_id) \
    pppd_get_ccb((fastrg_ccb_ptr)->ppp_ccb_rcu, \
        (ppp_ccb_t ** const *)&(fastrg_ccb_ptr)->ppp_ccb, \
        (ccb_id))

static __always_inline ppp_ccb_t *pppd_get_ccb(struct rte_rcu_qsbr *ppp_ccb_rcu, 
    ppp_ccb_t ** const *ppp_ccb_array_ptr, U16 ccb_id)
{
    unsigned int lcore_id = 0;

    if (likely(rte_lcore_id() != LCORE_ID_ANY))
        lcore_id = rte_lcore_id();
    // RCU read-side critical section
    rte_rcu_qsbr_thread_online(ppp_ccb_rcu, lcore_id);
    ppp_ccb_t **ppp_ccb_array = __atomic_load_n(ppp_ccb_array_ptr, __ATOMIC_ACQUIRE);
    ppp_ccb_t *result = __atomic_load_n(&ppp_ccb_array[ccb_id], __ATOMIC_ACQUIRE);
    rte_rcu_qsbr_quiescent(ppp_ccb_rcu, lcore_id);
    rte_rcu_qsbr_thread_offline(ppp_ccb_rcu, lcore_id);

    return result;
}

/**
 * @fn check_etcd_pppoe_status
 * 
 * @brief Check etcd pppoe status and update if necessary
 * 
 * @param tim
 *      Timer pointer
 * @param ppp_ccb
 *      PPP control block pointer
 */
void check_etcd_pppoe_status(struct rte_timer *tim, ppp_ccb_t *ppp_ccb);

#endif
