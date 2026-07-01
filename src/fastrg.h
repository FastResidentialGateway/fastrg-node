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
#include <rte_ring.h>

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

/* Set TRUE per data-plane lcore at thread startup (fastrg_rcu_dp_register).
 * When set, the RCU ccb/stats getters take a lean load-only fast path: the lcore
 * stays QSBR-online for its whole life and reports quiescent once per burst,
 * instead of entering/exiting a critical section on every getter call.
 * Control-plane / metrics threads leave it FALSE and use the full path. */
extern BOOL fastrg_rcu_persistent[RTE_MAX_LCORE];

#define NIC_MODEL_MAX_LEN 128

struct nic_info {
    char *vendor_name;
    nic_vendor_t vendor_id;
    char model[PORT_AMOUNT][NIC_MODEL_MAX_LEN]; /* human-readable NIC model per port */
    struct rte_ether_addr hsi_wan_src_mac;/* FastRG WAN side mac addr */
    struct rte_ether_addr hsi_lan_mac;    /* FastRG LAN side mac addr */
};

/* Per-lcore counters: each data/ctrl lcore writes ONLY its own slot
 * (per_subscriber_stats[lcore][port]) with a plain += — no atomic, no cross-core
 * cache-line bouncing. Readers (metrics) sum the per-lcore copies with RELAXED
 * loads. 64-bit aligned, single-writer-per-slot ⇒ stores never tear. */
struct per_ccb_stats {
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t dropped_packets;
    uint64_t dropped_bytes;
};

struct lcore_usage_counter {
    uint64_t busy_cycles;
    uint64_t total_cycles;
    const char *role;
} __rte_cache_aligned;

/* FastRG system data structure */
typedef struct FastRG {
    U8                      cur_user;       /* pppoe alive user count */
    U8                      loglvl;         /* FastRG loglvl */
    BOOL                    is_standalone;  /* FastRG standalone mode */
    char                    *version;       /* FastRG version */
    char                    *build_date;    /* build date */
    char                    *eal_args;      /* DPDK EAL args */
    U16                     user_count;     /* total FastRG subscriptor */
    U16                     max_user_count; /* max FastRG subscriptor supported */
    struct lcore_map        lcore;          /* lcore map */
    char                    *log_path;      /* FastRG log file path (pcap captures go in its dir) */
    char                    *unix_sock_path;/* FastRG unix socket file path */
    char                    *node_grpc_ip_port; /* FastRG node grpc ip:port */
    int                     unix_sock_fd;   /* FastRG unix socket file descriptor */
    FILE                    *fp;            /* FastRG log file pointer */
    char                    *node_uuid;     /* FastRG node uuid */
    char                    *controller_address; /* FastRG controller grpc address */
    char                    *etcd_endpoints;/* etcd endpoints */
    char                    *kafka_brokers; /* Kafka brokers for telemetry; NULL/empty = disabled */
    char                    *central_office_location; /* central office location identifier */
    BOOL                    enable_ddp;         /* mirrors EnableDDP config toggle */
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
    /* Per-lcore × per-port stats: [raw rte_lcore_id()][port] -> (user_count+1)
     * entry array (last = unknown user). Only EAL-lcore rows are allocated;
     * each lcore writes only its own row, readers sum across rows. */
    struct per_ccb_stats    *per_subscriber_stats[RTE_MAX_LCORE][PORT_AMOUNT];
    U16                     per_subscriber_stats_len;
    struct rte_rcu_qsbr     *per_subscriber_stats_rcu; /* RCU for protecting per_subscriber_stats array pointer */
    rte_atomic16_t          per_subscriber_stats_updating; /* flag indicating stats array is being updated */
    struct rte_timer        link;           /* for physical link checking timer */
    struct rte_timer        heartbeat_timer;/* for controller heartbeat timer */
    datapath_mode_t         datapath_mode;    /* RSS multi-queue vs software distributor */
    struct rte_distributor  *wan_dist;        /* WAN ingress software distributor (DP_MODE_DISTRIBUTOR) */
    struct rte_distributor  *lan_dist;        /* LAN ingress software distributor (DP_MODE_DISTRIBUTOR) */
    struct rte_ring         *cp_q;            /* data/ctrl plane -> control loop event ring */
    struct rte_ring         *free_mail_ring;  /* pre-allocated tFastRG_MBX slot pool */
    struct rte_ring         *etcd_event_q;    /* etcd watcher threads -> control loop event ring */
    struct lcore_usage_counter *lcore_usage;  /* per-lcore busy/total cycle counters, index by lcore_id */
    char                    *metrics_ip_port; /* Prometheus /metrics HTTP listen addr, e.g. "0.0.0.0:9101" */
    uint64_t                node_start_time;  /* process start time (epoch seconds) — crashloop detection */
    uint64_t                node_restart_total; /* persisted restart count from RESTART_COUNT_FILE */
    /* Per-port link state cache, updated by EV_LINK handler, read by metrics thread (atomic). */
    uint8_t                 nic_link_up[PORT_AMOUNT];    /* 1 = link up, 0 = down */
    uint32_t                nic_link_speed[PORT_AMOUNT]; /* link speed in Mbps */
    uint64_t                nic_link_flaps[PORT_AMOUNT]; /* cumulative link state transitions */
} __rte_cache_aligned FastRG_t;

STATUS fastrg_disable_subscriber_stats(FastRG_t *fastrg_ccb, U16 disable_count, 
    U16 old_count);
STATUS fastrg_gen_northbound_event(FastRG_t *fastrg_ccb, fastrg_event_type_t event_type,
    U8 cmd_type, U16 ccb_id);
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
        (fastrg_ccb_ptr)->per_subscriber_stats, (port_id), (ccb_id))

static __always_inline struct per_ccb_stats *fastrg_get_per_subscriber_stats(
    struct rte_rcu_qsbr *stats_rcu,
    struct per_ccb_stats *(*stats_2d)[PORT_AMOUNT],
    U16 port_id, U16 ccb_id)
{
    if (unlikely(port_id >= PORT_AMOUNT))
        return NULL;

    /* Stats are written only from EAL data/ctrl lcores; the caller writes its
     * own per-lcore row. A non-EAL thread has no row (and must not index the
     * array out of range), so it gets no stats slot. */
    unsigned int lcore_id = rte_lcore_id();
    if (unlikely(lcore_id == LCORE_ID_ANY))
        return NULL;

    /* data-plane lcore: stays QSBR-online for life + quiescent once per burst,
     * so just do the protected load — skip per-call online/quiescent/offline. */
    if (likely(fastrg_rcu_persistent[lcore_id])) {
        struct per_ccb_stats *stats_array =
            __atomic_load_n(&stats_2d[lcore_id][port_id], __ATOMIC_ACQUIRE);
        return likely(stats_array != NULL) ? &stats_array[ccb_id] : NULL;
    }

    // RCU read-side critical section
    rte_rcu_qsbr_thread_online(stats_rcu, lcore_id);

    // Atomically load this lcore's stats array pointer for this port
    struct per_ccb_stats *stats_array =
        __atomic_load_n(&stats_2d[lcore_id][port_id], __ATOMIC_ACQUIRE);

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
