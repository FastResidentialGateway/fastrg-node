#ifndef _OPENRG_H_
#define _OPENRG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#include <common.h>

#include <rte_common.h>
#include <rte_atomic.h>
#include <rte_ether.h>
#include <rte_timer.h>
#include <rte_rcu_qsbr.h>

#include "protocol.h"
#include "utils.h"
#include "init.h"

#define MAX_VLAN_ID 4000
#define MIN_VLAN_ID 2

#define MAX_USER_COUNT 4000
#define MIN_USER_COUNT 1

#define INVALID_CCB_ID UINT16_MAX

#define WAN_PORT    1
#define LAN_PORT    0

#define LINK_DOWN   0x0
#define LINK_UP     0x1

enum {
    CLI_QUIT = 0,
    CLI_DISCONNECT,
    CLI_CONNECT,
    CLI_DHCP_START,
    CLI_DHCP_STOP,
};

extern rte_atomic16_t stop_flag;
extern rte_atomic16_t start_flag;

struct nic_info {
    char *vendor_name;
    nic_vendor_t vendor_id;
    struct rte_ether_addr hsi_wan_src_mac;/* FastRG WAN side mac addr */
    struct rte_ether_addr hsi_lan_mac;    /* FastRG LAN side mac addr */
};

struct per_ccb_stats {
    rte_atomic64_t rx_packets;
    rte_atomic64_t rx_bytes;
    rte_atomic64_t tx_packets;
    rte_atomic64_t tx_bytes;
    rte_atomic64_t dropped_packets;
    rte_atomic64_t dropped_bytes;
};

/* FastRG system data structure */
typedef struct FastRG {
    U8                      cur_user;       /* pppoe alive user count */
    U8                      loglvl;         /* FastRG loglvl */
    BOOL                    is_standalone;  /* FastRG standalone mode */
    char                    *version;       /* FastRG version */
    char                    *build_date;    /* build date */
    char                    *eal_args;      /* DPDK EAL args */
    U16                     user_count;     /* total FastRG subscriptor */
    struct lcore_map        lcore;          /* lcore map */
    char                    *unix_sock_path;/* FastRG unix socket file path */
    char                    *node_grpc_ip_port; /* FastRG node grpc ip:port */
    int                     unix_sock_fd;   /* FastRG unix socket file descriptor */
    FILE                    *fp;            /* FastRG log file pointer */
    char                    *node_uuid;     /* FastRG node uuid */
    char                    *controller_address; /* FastRG controller grpc address */
    char                    *etcd_endpoints;/* etcd endpoints */
    U16                     heartbeat_interval; /* heartbeat interval time in seconds */
    struct nic_info         nic_info;
    void                    **ppp_ccb;       /* pppoe control block */
    struct rte_mempool      *ppp_ccb_mp;
    struct rte_rcu_qsbr     *ppp_ccb_rcu;   /* RCU for protecting ppp_ccb array pointer */
    rte_atomic16_t          ppp_ccb_updating; /* flag indicating array is being updated */
    void                    **dhcp_ccb;     /* dhcp control block */
    struct rte_mempool      *dhcp_ccb_mp;
    struct rte_rcu_qsbr     *dhcp_ccb_rcu;  /* RCU for protecting dhcp_ccb array pointer */
    rte_atomic16_t          dhcp_ccb_updating; /* flag indicating array is being updated */
    struct rte_mempool      *arp_pending_mp; /* mempool for ARP pending queue entries */
    rte_atomic16_t          *vlan_userid_map; /* vlan to user id map */
    struct per_ccb_stats    *per_subscriber_stats[PORT_AMOUNT]; /* per subscriber stats */
    U16                     per_subscriber_stats_len;
    struct rte_rcu_qsbr     *per_subscriber_stats_rcu; /* RCU for protecting per_subscriber_stats array pointer */
    rte_atomic16_t          per_subscriber_stats_updating; /* flag indicating stats array is being updated */
    struct rte_timer        link;           /* for physical link checking timer */
    struct rte_timer        heartbeat_timer;/* for controller heartbeat timer */
    BOOL                    i40e_ddp_enabled; /* TRUE if i40e DDP package loaded */
} __rte_cache_aligned FastRG_t;

STATUS fastrg_disable_subscriber_stats(FastRG_t *fastrg_ccb, U16 disable_count, 
    U16 old_count);
STATUS fastrg_gen_northbound_event(fastrg_event_type_t event_type, U8 cmd_type,
    U16 ccb_id);
STATUS fastrg_modify_subscriber_count(FastRG_t *fastrg_ccb, U16 new_count, 
    U16 old_count);

/**
 * @fn OPENRG_GET_PER_SUBSCRIBER_STATS
 * 
 * @brief Get per subscriber stats pointer with RCU protection
 * 
 * @param fastrg_ccb_ptr
 *      FastRG control block pointer
 * @param port_id
 *      Port ID (0 for LAN, 1 for WAN)
 * @param ccb_id
 *      CCB ID
 * @return 
 *      Pointer to per_ccb_stats or NULL if failed
 */
#define OPENRG_GET_PER_SUBSCRIBER_STATS(fastrg_ccb_ptr, port_id, ccb_id) \
    fastrg_get_per_subscriber_stats((fastrg_ccb_ptr)->per_subscriber_stats_rcu, \
        &(fastrg_ccb_ptr)->per_subscriber_stats[port_id], (port_id), (ccb_id))

static __always_inline struct per_ccb_stats *fastrg_get_per_subscriber_stats(
    struct rte_rcu_qsbr *stats_rcu, 
    struct per_ccb_stats **stats_array_ptr,
    U16 port_id, U16 ccb_id)
{
    unsigned int lcore_id = 0;

    if (unlikely(port_id >= PORT_AMOUNT))
        return NULL;

    if (likely(rte_lcore_id() != LCORE_ID_ANY))
        lcore_id = rte_lcore_id();

    // RCU read-side critical section
    rte_rcu_qsbr_thread_online(stats_rcu, lcore_id);

    // Atomically load the stats array pointer for this port
    struct per_ccb_stats *stats_array = __atomic_load_n(stats_array_ptr, __ATOMIC_ACQUIRE);

    struct per_ccb_stats *result = NULL;
    if (likely(stats_array != NULL))
        result = &stats_array[ccb_id];

    rte_rcu_qsbr_quiescent(stats_rcu, lcore_id);
    rte_rcu_qsbr_thread_offline(stats_rcu, lcore_id);

    return result;
}

/**
 * @fn fastrg_add_subscriber_stats
 * 
 * @brief Add more subscriber stats entries
 * 
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param extra_count
 *      Number of extra entries to add
 * @return 
 *      SUCCESS if added successfully, ERROR if failed
 */
STATUS fastrg_add_subscriber_stats(FastRG_t *fastrg_ccb, U16 extra_count);

/**
 * @fn fastrg_remove_subscriber_stats
 * 
 * @brief Remove subscriber stats entries
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param remove_count
 *      Number of entries to remove
 * @param old_count
 *      Old number of entries before removal
 * @return 
 *      SUCCESS if removed successfully, ERROR if failed
 */
STATUS fastrg_remove_subscriber_stats(FastRG_t *fastrg_ccb, U16 remove_count, U16 old_count);

/**
 * @fn fastrg_cleanup_subscriber_stats
 * 
 * @brief Cleanup all subscriber stats entries
 * 
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param total_count
 *      Total number of entries
 */
void fastrg_cleanup_subscriber_stats(FastRG_t *fastrg_ccb, U16 total_count);

int fastrg_start(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
