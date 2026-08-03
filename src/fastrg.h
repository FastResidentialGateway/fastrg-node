#ifndef _FASTRG_H_
#define _FASTRG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>
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
#include "lighthttp.h"

#define MAX_VLAN_ID 4000
#define MIN_VLAN_ID 2

#define MAX_USER_COUNT 2000
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

/* Per-lcore PPPoE session counters, same per-lcore scheme as per_ccb_stats:
 * pppoes_stats[lcore][ccb_id], each lcore writes only its own row with a plain
 * += (no atomic); readers sum the rows with RELAXED loads. */
struct pppoes_lcore_stats {
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t tx_bytes;
};

struct lcore_usage_counter {
    uint64_t busy_cycles;
    uint64_t total_cycles;
    const char *role;
} __rte_cache_aligned;

/* FastRG system data structure */
typedef struct FastRG {
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
    /* Fixed-max prealloc: the ppp_ccb / dhcp_ccb pointer arrays are sized to
     * max_user_count at init, every slot is allocated and base-initialized
     * then, and neither the arrays nor the slots are ever swapped, resized or
     * freed at runtime. user_count is purely the accessible upper bound. */
    void                    **ppp_ccb;       /* pppoe control block, the array never be changed */
    struct rte_mempool      *ppp_ccb_mp;
    /* ppp_ccb_rcu does not guard the array pointer ppp_ccb. It is
     * KEPT because the per-subscriber NAT slot reclaim depends on it: the NAT
     * reverse/forward rte_hash defer queues (rte_hash_rcu_qsbr_add) use this
     * QSBR variable, and the data-plane lcores stay persistently online on it
     * reporting one quiescent state per burst (fastrg_rcu_dp_quiescent). */
    struct rte_rcu_qsbr     *ppp_ccb_rcu;
    void                    **dhcp_ccb;     /* dhcp control block, the array never be changed */
    struct rte_mempool      *dhcp_ccb_mp;
    struct rte_mempool      *arp_pending_mp; /* mempool for ARP pending queue entries */
    rte_atomic16_t          *vlan_userid_map; /* vlan to user id map */
    /* Per-lcore × per-port stats: [raw rte_lcore_id()][port] ->
     * (max_user_count+1) entry array, allocated once at init and freed only
     * at shutdown (index max_user_count = unknown-user slot). Only EAL-lcore
     * rows are allocated; each lcore writes only its own row, readers sum
     * across rows. */
    struct per_ccb_stats    *per_subscriber_stats[RTE_MAX_LCORE][PORT_AMOUNT];
    /* pdump_rcu does not protect a data pointer. It marks intervals
     * where data-plane lcores may be inside RX/TX bursts and therefore pdump
     * callbacks. Callback removal does not wait for callbacks already in
     * flight; teardown synchronizes this timeline after removal and frees the
     * ring, mempool, and filter only after every reader crosses a burst boundary. */
    struct rte_rcu_qsbr     *pdump_rcu;
    /* Per-lcore PPPoE session stats: [raw rte_lcore_id()] ->
     * (max_user_count+1) entry array, fixed like per_subscriber_stats. */
    struct pppoes_lcore_stats *pppoes_stats[RTE_MAX_LCORE];
    struct rte_timer        link;           /* for physical link checking timer */
    struct rte_timer        heartbeat_timer;/* for controller heartbeat timer */
    datapath_mode_t         datapath_mode;    /* RSS multi-queue vs software distributor */
    U16                     dp_ctrl_txq_self; /* data core self-port control packet TX queue (N+1) */
    U16                     dp_ctrl_txq_opposite;/* data core opposite-port control packet TX queue (N+2) */
    struct rte_distributor  *wan_dist;        /* WAN ingress software distributor (DP_MODE_DISTRIBUTOR) */
    struct rte_distributor  *lan_dist;        /* LAN ingress software distributor (DP_MODE_DISTRIBUTOR) */
    struct rte_ring         *cp_q;            /* data/ctrl plane -> control loop event ring */
    struct rte_ring         *free_mail_ring;  /* pre-allocated tFastRG_MBX slot pool */
    struct rte_ring         *etcd_event_q;    /* etcd watcher threads -> control loop event ring */
    struct lcore_usage_counter *lcore_usage;  /* per-lcore busy/total cycle counters, index by lcore_id */
    char                    *metrics_ip_port; /* Prometheus /metrics HTTP listen addr, e.g. "0.0.0.0:9101" */
    pthread_t               metrics_thread;   /* joinable Prometheus HTTP server thread */
    BOOL                    metrics_thread_started;
    /* Set on every graceful shutdown before lighthttp_stop(). Covers the startup
     * window where stop runs while the metrics thread has not yet published its
     * listen_fd (still scheduled-but-not-running or inside lighthttp_init):
     * lighthttp_stop() then exchanges -1 and is a no-op, so without this flag the
     * thread would open its fd afterwards and block in accept() forever, hanging
     * the pthread_join in fastrg_stop(). The thread re-checks this flag right
     * after lighthttp_init() (under rte_smp_mb()) and closes its own fd. */
    rte_atomic16_t          metrics_stop_requested;
    lighthttp_server_t      metrics_server;
    pthread_t               grpc_thread;      /* joinable northbound gRPC server thread */
    BOOL                    grpc_thread_started;
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

/**
 * @fn FASTRG_GET_PER_SUBSCRIBER_STATS
 *
 * @brief Get the calling lcore's per-subscriber stats slot.
 *
 * @param fastrg_ccb_ptr
 *      FastRG control block pointer
 * @param port_id
 *      Port ID (0 for LAN, 1 for WAN)
 * @param ccb_id
 *      CCB ID (max_user_count = unknown-user slot)
 * @return
 *      Pointer to per_ccb_stats or NULL if the caller has no row
 */
#define FASTRG_GET_PER_SUBSCRIBER_STATS(fastrg_ccb_ptr, port_id, ccb_id) \
    fastrg_get_per_subscriber_stats((fastrg_ccb_ptr)->per_subscriber_stats, \
        (port_id), (ccb_id))

static __always_inline struct per_ccb_stats *fastrg_get_per_subscriber_stats(
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

    struct per_ccb_stats *stats_array = stats_2d[lcore_id][port_id];
    return likely(stats_array != NULL) ? &stats_array[ccb_id] : NULL;
}

/* Return the calling lcore's PPPoE-session stats slot for ccb_id (mirrors
 * FASTRG_GET_PER_SUBSCRIBER_STATS; no port dimension). */
#define FASTRG_GET_PPPOES_STATS(fastrg_ccb_ptr, ccb_id) \
    fastrg_get_pppoes_stats((fastrg_ccb_ptr)->pppoes_stats, (ccb_id))

static __always_inline struct pppoes_lcore_stats *fastrg_get_pppoes_stats(
    struct pppoes_lcore_stats **stats_1d,
    U16 ccb_id)
{
    /* Written only from EAL data/ctrl lcores; a non-EAL caller has no row. */
    unsigned int lcore_id = rte_lcore_id();
    if (unlikely(lcore_id == LCORE_ID_ANY))
        return NULL;

    struct pppoes_lcore_stats *stats_array = stats_1d[lcore_id];
    return likely(stats_array != NULL) ? &stats_array[ccb_id] : NULL;
}

/* Sum a subscriber's per-lcore packet stats across all lcores into *out.
 * Keeps the summation in C so the gRPC/metrics readers stay thin marshalling shells. */
static __always_inline void fastrg_sum_subscriber_stats(
    FastRG_t *fastrg_ccb, U16 port_id, U16 ccb_id, struct per_ccb_stats *out)
{
    out->rx_packets = 0; out->rx_bytes = 0;
    out->tx_packets = 0; out->tx_bytes = 0;
    out->dropped_packets = 0; out->dropped_bytes = 0;
    if (unlikely(port_id >= PORT_AMOUNT))
        return;
    unsigned int lcore;
    RTE_LCORE_FOREACH(lcore) {
        struct per_ccb_stats *lcore_stats =
            __atomic_load_n(&fastrg_ccb->per_subscriber_stats[lcore][port_id], __ATOMIC_ACQUIRE);
        if (lcore_stats == NULL)
            continue;
        out->rx_packets      += __atomic_load_n(&lcore_stats[ccb_id].rx_packets, __ATOMIC_RELAXED);
        out->rx_bytes        += __atomic_load_n(&lcore_stats[ccb_id].rx_bytes, __ATOMIC_RELAXED);
        out->tx_packets      += __atomic_load_n(&lcore_stats[ccb_id].tx_packets, __ATOMIC_RELAXED);
        out->tx_bytes        += __atomic_load_n(&lcore_stats[ccb_id].tx_bytes, __ATOMIC_RELAXED);
        out->dropped_packets += __atomic_load_n(&lcore_stats[ccb_id].dropped_packets, __ATOMIC_RELAXED);
        out->dropped_bytes   += __atomic_load_n(&lcore_stats[ccb_id].dropped_bytes, __ATOMIC_RELAXED);
    }
}

/* Sum a PPPoE session's per-lcore counters across all lcores into *out. */
static __always_inline void fastrg_sum_pppoes_stats(
    FastRG_t *fastrg_ccb, U16 ccb_id, struct pppoes_lcore_stats *out)
{
    out->rx_packets = 0; out->rx_bytes = 0;
    out->tx_packets = 0; out->tx_bytes = 0;
    unsigned int lcore;
    RTE_LCORE_FOREACH(lcore) {
        struct pppoes_lcore_stats *lcore_stats =
            __atomic_load_n(&fastrg_ccb->pppoes_stats[lcore], __ATOMIC_ACQUIRE);
        if (lcore_stats == NULL)
            continue;
        out->rx_packets += __atomic_load_n(&lcore_stats[ccb_id].rx_packets, __ATOMIC_RELAXED);
        out->rx_bytes   += __atomic_load_n(&lcore_stats[ccb_id].rx_bytes, __ATOMIC_RELAXED);
        out->tx_packets += __atomic_load_n(&lcore_stats[ccb_id].tx_packets, __ATOMIC_RELAXED);
        out->tx_bytes   += __atomic_load_n(&lcore_stats[ccb_id].tx_bytes, __ATOMIC_RELAXED);
    }
}

/**
 * @fn fastrg_cleanup_subscriber_stats
 *
 * @brief Free every per-lcore per-subscriber stats row.
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 */
void fastrg_cleanup_subscriber_stats(FastRG_t *fastrg_ccb);

/* Per-lcore PPPoE session stats: disable (zero a range) and shutdown
 * cleanup. */
STATUS fastrg_disable_pppoes_stats(FastRG_t *fastrg_ccb, U16 disable_count, U16 old_count);

/**
 * @fn fastrg_cleanup_pppoes_stats
 *
 * @brief Free every per-lcore per-pppoes stats row.
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 */
void fastrg_cleanup_pppoes_stats(FastRG_t *fastrg_ccb);

int fastrg_start(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
