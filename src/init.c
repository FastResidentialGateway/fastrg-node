#include <sys/signalfd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <linux/ethtool.h>
#include <time.h>
#include <inttypes.h>
#include <sys/stat.h>

#include <common.h>

#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_errno.h>
#include <rte_mbuf.h>
#include <rte_byteorder.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>

#include <uuid/uuid.h>

#include "pppd/pppd.h"
#include "dhcpd/dhcp_codec.h"
#include "dhcpd/dhcpd.h"
#include "dp.h"
#include "init.h"
#include "fastrg.h"
#include "mac_table.h"
#include "dp_flow.h"
#include "dbg.h"
#include "version.h"
#include "config.h"
#include "lighthttp.h"
#include "metrics.h"
#include "../northbound/controller/controller_client.h"

#define NUM_MBUFS 		8191
#define MBUF_CACHE_SIZE 512
#define RING_SIZE 		16384
/* Mail slot pool: one slot per in-flight cp_q event. */
#define MAIL_SLOT_RING_SIZE 4096

/* Headroom for the pdump pool, runtime rte_malloc calls, and the small metadata
 * the capacity measurement does not itemise. */
#define HUGEPAGE_RESERVE_BYTES (512ULL * 1024ULL * 1024ULL)

/* Persisted restart counter for crashloop detection (fastrg_node_restart_total). */
#define RESTART_COUNT_DIR  "/var/lib/fastrg"
#define RESTART_COUNT_FILE RESTART_COUNT_DIR "/restart_count"

struct rte_mempool *direct_pool[PORT_AMOUNT];
struct rte_mempool *indirect_pool[PORT_AMOUNT];

#ifdef UNIT_TEST
static uint64_t test_hugepage_free_bytes;
static uint64_t test_subscriber_cost_bytes;

void fastrg_set_hugepage_free_bytes_for_test(uint64_t free_bytes)
{
    test_hugepage_free_bytes = free_bytes;
}

void fastrg_set_subscriber_cost_for_test(uint64_t cost_bytes)
{
    test_subscriber_cost_bytes = cost_bytes;
}

static int fastrg_get_socket_stats(int socket_id, struct rte_malloc_socket_stats *stats)
{
    if (socket_id != 0)
        return -1;

    memset(stats, 0, sizeof(*stats));
    stats->heap_freesz_bytes = test_hugepage_free_bytes;
    return 0;
}
#else
static int fastrg_get_socket_stats(int socket_id, struct rte_malloc_socket_stats *stats)
{
    return rte_malloc_get_socket_stats(socket_id, stats);
}
#endif

/**
 * @fn fastrg_get_system_page_size_for_mempool
 *
 * @brief Page size the pools are carved from, queried once via a throwaway
 *        empty pool and cached.
 *
 * @param page_size
 *      [out] Page size in bytes
 * @return
 *      SUCCESS when the page size was determined
 */
static STATUS fastrg_get_system_page_size_for_mempool(size_t *page_size)
{
    static size_t cached_page_size;
    static BOOL cached;
    struct rte_mempool *mp;
    STATUS ret = ERROR;

    if (cached) {
        *page_size = cached_page_size;
        return SUCCESS;
    }

    /* flags=0 must match the real pools: it decides whether objects are
     * page-bound. */
    mp = rte_mempool_create_empty("fastrg_pgsz_probe", 1, 64, 0, 0,
        (int)rte_socket_id(), 0);
    if (mp == NULL)
        return ERROR;
    if (rte_mempool_set_ops_byname(mp, "ring_mp_mc", NULL) == 0 &&
            rte_mempool_get_page_size(mp, &cached_page_size) == 0) {
        cached = TRUE;
        *page_size = cached_page_size;
        ret = SUCCESS;
    }
    rte_mempool_free(mp);

    return ret;
}

/**
 * @fn fastrg_get_real_mempool_elt_size_in_hugepage
 *
 * @brief One object's footprint inside a pool: the object plus the header and
 *        trailer the pool wraps it in.
 *
 * @param elt_size
 *      Size of one object as handed to rte_mempool_create
 * @return
 *      Bytes one object occupies
 */
static uint64_t fastrg_get_real_mempool_elt_size_in_hugepage(uint32_t elt_size)
{
    struct rte_mempool_objsz sz;

    rte_mempool_calc_obj_size(elt_size, 0, &sz);
    return sz.total_size;
}

STATUS fastrg_get_mempool_span_bytes(uint32_t elt_size, uint32_t objs, uint64_t *span)
{
    uint64_t total_elt = fastrg_get_real_mempool_elt_size_in_hugepage(elt_size);
    uint64_t objs_per_page;
    size_t page_size;

    if (objs == 0 || total_elt == 0) {
        *span = 0;
        return SUCCESS;
    }
    if (fastrg_get_system_page_size_for_mempool(&page_size) != SUCCESS)
        return ERROR;
    if (page_size == 0) {
        *span = total_elt * objs;   /* no pages to straddle */
        return SUCCESS;
    }

    /* Per-object upper bound of rte_mempool_op_calc_mem_size_helper()
     * (rte_mempool_ops_default.c); the per-pool constant part lives in
     * fastrg_get_mempool_fixed_usage_bytes(). */
    objs_per_page = (uint64_t)page_size / total_elt;
    if (objs_per_page == 0) {
        /* Object larger than a page: it gets whole pages to itself. */
        uint64_t pages_per_obj = (total_elt + page_size - 1) / page_size;

        *span = pages_per_obj * page_size * objs;
        return SUCCESS;
    }
    /* Objects from all subscribers pack into shared pages: each one carries
     * page_size / objs_per_page, never a whole page. */
    *span = ((uint64_t)objs * page_size + objs_per_page - 1) / objs_per_page;
    return SUCCESS;
}

uint64_t fastrg_get_mempool_fixed_usage_bytes(uint32_t elt_size)
{
    uint64_t total_elt = fastrg_get_real_mempool_elt_size_in_hugepage(elt_size);

    /* Chunk alignment slack plus the smallest ring: bounded, charged once per
     * pool. */
    return (total_elt > 0 ? total_elt - 1 : 0) +
        rte_ring_get_memsize(1) + 16ULL;
}

/**
 * @fn fastrg_get_subscriber_capacity
 *
 * @brief Subscriber count that fits: (free - overhead) / cost, clamped to
 *        [MIN_USER_COUNT, MAX_USER_COUNT].
 *
 * @param free_bytes
 *      Free hugepage heap
 * @param overhead
 *      Bytes reserved before any subscriber is charged
 * @param cost
 *      Bytes one subscriber costs
 * @return
 *      Subscriber count
 */
static uint64_t fastrg_get_subscriber_capacity(uint64_t free_bytes, uint64_t overhead,
    uint64_t cost)
{
    uint64_t budget = free_bytes > overhead ? free_bytes - overhead : 0;
    uint64_t n = cost > 0 ? budget / cost : 0;

    if (n < MIN_USER_COUNT)
        n = MIN_USER_COUNT;
    else if (n > MAX_USER_COUNT)
        n = MAX_USER_COUNT;
    return n;
}

STATUS fastrg_compute_max_user_count(FastRG_t *fastrg_ccb)
{
    uint64_t free_bytes = 0;
    uint64_t stats_per_sub = 0;
    unsigned int lcore_id;

    for(int socket_id=0; socket_id<RTE_MAX_NUMA_NODES; socket_id++) {
        struct rte_malloc_socket_stats stats;
        if (fastrg_get_socket_stats(socket_id, &stats) == 0)
            free_bytes += stats.heap_freesz_bytes;
    }

#ifdef UNIT_TEST
    if (test_subscriber_cost_bytes != 0) {
        /* Injected cost replaces the measured inputs. */
        fastrg_ccb->max_user_count = (U16)fastrg_get_subscriber_capacity(free_bytes,
            HUGEPAGE_RESERVE_BYTES, test_subscriber_cost_bytes);
        fastrg_ccb->subscriber_cost_bytes = test_subscriber_cost_bytes;
        return SUCCESS;
    }
#endif

    ccb_memory_info_t ppp_size_info = {0}, dhcp_size_info = {0};

    if (pppd_get_subscriber_real_size(fastrg_ccb, &ppp_size_info) != SUCCESS)
        return ERROR;
    dhcp_get_subscriber_real_size(&dhcp_size_info);

    /* The +1 unknown-user slot in each stats row is charged to the reserve. */
    RTE_LCORE_FOREACH(lcore_id) {
        stats_per_sub += (uint64_t)PORT_AMOUNT * sizeof(struct per_ccb_stats);
        stats_per_sub += sizeof(struct pppoes_lcore_stats);
    }

    const ccb_memory_info_t *size_infos[] = { &ppp_size_info, &dhcp_size_info };
    uint64_t plain_bytes = stats_per_sub;
    uint64_t cost = stats_per_sub;
    uint64_t fixed = 0;

    for(unsigned i=0; i<sizeof(size_infos)/sizeof(size_infos[0]); i++) {
        const ccb_memory_info_t *info = size_infos[i];

        plain_bytes += info->plain_bytes_per_sub;
        cost += info->plain_bytes_per_sub;
        for(int j=0; j<info->n_pools; j++) {
            uint32_t objs = info->pools[j].objs_per_sub;
            uint32_t elt = info->pools[j].elt_size;
            uint64_t span;

            plain_bytes += (uint64_t)objs * elt;
            if (fastrg_get_mempool_span_bytes(elt, objs, &span) != SUCCESS) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                    "Subscriber capacity: cannot determine mempool page size");
                return ERROR;
            }
            /* 16 bytes per object covers the worst-case ring slot: 8 bytes,
             * rounded up to a power-of-two slot count. */
            cost += span + 16ULL * objs;
            fixed += fastrg_get_mempool_fixed_usage_bytes(elt);
        }
    }

    uint64_t n = fastrg_get_subscriber_capacity(free_bytes,
        HUGEPAGE_RESERVE_BYTES + fixed, cost);

    fastrg_ccb->max_user_count = (U16)n;
    fastrg_ccb->subscriber_cost_bytes = cost;
    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "Subscriber capacity: free=%" PRIu64 " reserve=%" PRIu64
        " ccb plain=%" PRIu64 " page+ring correction=%" PRIu64
        " cost/sub=%" PRIu64 " fixed=%" PRIu64 " computed max=%u",
        free_bytes, HUGEPAGE_RESERVE_BYTES,
        plain_bytes, cost - plain_bytes, cost, fixed, fastrg_ccb->max_user_count);

    return SUCCESS;
}

struct nic_info vendor[] = {
    { "mlx5_pci", NIC_VENDOR_MLX5 },
    { "net_ixgbe", NIC_VENDOR_IXGBE },
    { "net_vmxnet3", NIC_VENDOR_VMXNET3 },
    { "net_i40e", NIC_VENDOR_I40E },
    { "net_ice", NIC_VENDOR_ICE },
    { NULL, NIC_VENDOR_UNKNOWN }
};

void cleanup_mem()
{
    for(int i=0; i<PORT_AMOUNT; i++) {
        if (direct_pool[i]) {
            rte_mempool_free(direct_pool[i]);
            direct_pool[i] = NULL;
        }
        if (indirect_pool[i]) {
            rte_mempool_free(indirect_pool[i]);
            indirect_pool[i] = NULL;
        }
    }
}

void cleanup_ring(FastRG_t *fastrg_ccb)
{
    dp_tx_handoff_pkt_cleanup(fastrg_ccb);
    if (fastrg_ccb->free_mail_ring != NULL) {
        void *mail_slot;
        while (rte_ring_dequeue(fastrg_ccb->free_mail_ring, &mail_slot) == 0)
            fastrg_mfree(mail_slot);
        rte_ring_free(fastrg_ccb->free_mail_ring);
        fastrg_ccb->free_mail_ring = NULL;
    }
    if (fastrg_ccb->cp_q != NULL) {
        rte_ring_free(fastrg_ccb->cp_q);
        fastrg_ccb->cp_q = NULL;
    }
}

/**
 * setup_signalfd
 *
 * This function sets up a signalfd to monitor signals specified in the
 * given mask before EAL initialization.
 */
int setup_signalfd()
{
    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    /* Block SIGINT/SIGTERM for this thread/process so they will be delivered via signalfd */
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
        perror("block signal failed");
        return -1;
    }

    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd == -1) { 
        perror("signal fd create failed"); 
        return -1; 
    }

    printf("signalfd created (fd=%d).\n", sfd);

    return sfd;
}

STATUS init_mem(FastRG_t *fastrg_ccb)
{
    char buf[PATH_MAX];
    struct rte_mempool *mp;

    /* Creates a new mempool in memory to hold the mbufs. */
    for(int i=0; i<PORT_AMOUNT; i++) {
        if (direct_pool[i] == NULL) {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Creating direct mempool on port %i", i);
            snprintf(buf, sizeof(buf), "pool_direct_%i", i);
            mp = rte_pktmbuf_pool_create(buf, NUM_MBUFS, MBUF_CACHE_SIZE, sizeof(mbuf_priv_t), 
                RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
            if (mp == NULL) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot create direct mempool: %s", rte_strerror(rte_errno));
                goto err;
            }
            direct_pool[i] = mp;
        }

        if (indirect_pool[i] == NULL) {
            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Creating indirect mempool on port %i", i);
            snprintf(buf, sizeof(buf), "pool_indirect_%i", i);

            mp = rte_pktmbuf_pool_create(buf, NUM_MBUFS, MBUF_CACHE_SIZE, 0, 0, rte_socket_id());
            if (mp == NULL) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot create indirect mempool: %s", rte_strerror(rte_errno));
                goto err;
            }
            indirect_pool[i] = mp;
        }
    }

    return SUCCESS;

err:
    cleanup_mem();
    return ERROR;
}

STATUS init_ring(FastRG_t *fastrg_ccb)
{
    fastrg_ccb->cp_q = rte_ring_create("state_machine",RING_SIZE,rte_socket_id(),0);
    if (!fastrg_ccb->cp_q) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot create state_machine ring: %s", rte_strerror(rte_errno));
        return ERROR;
    }

    /* Multiple control and data-plane threads dequeue mail slots, so the ring must use MC dequeue. */
    fastrg_ccb->free_mail_ring = rte_ring_create("free_mail_ring", MAIL_SLOT_RING_SIZE, rte_socket_id(), 0);
    if (!fastrg_ccb->free_mail_ring) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot create free_mail_ring", rte_strerror(rte_errno));
        goto err;
    }

    /* Fill the pool: a DPDK ring holds size-1 entries. */
    for(int i=0; i<MAIL_SLOT_RING_SIZE-1; i++) {
        tFastRG_MBX *mail_slot = fastrg_malloc(tFastRG_MBX, sizeof(tFastRG_MBX), 0);
        if (!mail_slot) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot allocate memory for mail_slot: %s", rte_strerror(rte_errno));
            goto err;
        }
        if (rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail_slot) != 0) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot enqueue mail_slot to free_mail_ring: %s", rte_strerror(rte_errno));
            fastrg_mfree(mail_slot);
            goto err;
        }
    }

    return SUCCESS;

err:
    cleanup_ring(fastrg_ccb);
    return ERROR;
}

STATUS init_port(FastRG_t *fastrg_ccb, struct fastrg_config *fastrg_cfg)
{
    struct ethtool_drvinfo 	dev_info;
    U8 						portid;

    if (rte_eth_macaddr_get(0, &fastrg_ccb->nic_info.hsi_lan_mac) != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "rte_eth_macaddr_get failed for LAN port: %s", rte_strerror(rte_errno));
        return ERROR;
    }
    if (rte_eth_macaddr_get(1, &fastrg_ccb->nic_info.hsi_wan_src_mac) != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "rte_eth_macaddr_get failed for WAN port: %s", rte_strerror(rte_errno));
        return ERROR;
    }

    /* Initialize all ports. */
    for(portid=0; portid<PORT_AMOUNT; portid++) {
        memset(&dev_info, 0, sizeof(dev_info));
        if (get_drvinfo(portid, &dev_info)) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                "Error getting info for port %i: %s", portid, 
                rte_strerror(rte_errno));
            return ERROR;
        }

        fastrg_ccb->nic_info.vendor_id = NIC_VENDOR_UNKNOWN;
        for(int i=0; vendor[i].vendor_name!=NULL; i++) {
            if (strcmp((const char *)dev_info.driver, vendor[i].vendor_name) == 0) {
                fastrg_ccb->nic_info.vendor_id = vendor[i].vendor_id;
                fastrg_ccb->nic_info.vendor_name = vendor[i].vendor_name;
                break;
            }
        }

        if (fastrg_get_nic_model(portid, fastrg_ccb->nic_info.model[portid],
                sizeof(fastrg_ccb->nic_info.model[portid])) != SUCCESS)
            fastrg_ccb->nic_info.model[portid][0] = '\0';

        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Port %i driver: %s (ver: %s)", portid, dev_info.driver, dev_info.version);
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Port %i model: %s", portid,
            fastrg_ccb->nic_info.model[portid][0] ? fastrg_ccb->nic_info.model[portid] : "unknown");
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "firmware-version: %s", dev_info.fw_version);
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "bus-info: %s", dev_info.bus_info);

        if (fastrg_ccb->enable_ddp == TRUE &&
                fastrg_ccb->nic_info.vendor_id == NIC_VENDOR_I40E &&
                fastrg_cfg->ddp_pkg_path[0] != '\0') {
            if (i40e_load_ddp_package(fastrg_ccb, portid, fastrg_cfg->ddp_pkg_path) == SUCCESS) {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                    "i40e DDP package loaded, multi-queue RSS will be enabled");
            } else {
                fastrg_ccb->enable_ddp = FALSE;
                FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                    "i40e DDP load failed, falling back to single queue mode");
            }
        }

        /* Select data-plane mode: hardware PPPoE-aware RSS (ICE/E810 or
         * i40e/X710 with DDP), otherwise the software rte_distributor path.
         * Decided per port but identical for both ports of the same NIC. */
        if (fastrg_ccb->enable_ddp == TRUE &&
                (fastrg_ccb->nic_info.vendor_id == NIC_VENDOR_ICE ||
                 fastrg_ccb->nic_info.vendor_id == NIC_VENDOR_I40E))
            fastrg_ccb->datapath_mode = DP_MODE_RSS;
        else
            fastrg_ccb->datapath_mode = DP_MODE_DISTRIBUTOR;

        if (PORT_INIT(fastrg_ccb, portid) == ERROR) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot init port %"PRIu8 "", portid);
            return ERROR;
        }
    }

    fastrg_ccb->version = GIT_COMMIT_ID;
    fastrg_ccb->build_date = BUILD_TIME;

	fastrg_ccb->node_uuid = fastrg_malloc(char, UUID_STR_LEN, 0);
	if (fastrg_ccb->node_uuid == NULL) {
		FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot allocate memory for node_uuid");
		return ERROR;
	}
    if (fastrg_get_id(fastrg_ccb->fp, fastrg_ccb->node_uuid) == ERROR) {
		FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Get node ID failed");
		return ERROR;
	}

    return SUCCESS;
}

/**
 * @fn init_node_runtime_state
 * @brief Seed observability state exposed via Prometheus (/metrics): process start
 *        time, a restart counter persisted across restarts (crashloop detection),
 *        and the per-port NIC link-state cache. Must be called after init_port() so
 *        link status can be read. Failures are non-fatal (logged, not propagated).
 *
 * @param fastrg_ccb
 *      FastRG control block
 */
static void init_node_runtime_state(FastRG_t *fastrg_ccb)
{
    /* Process start time (Unix epoch seconds) for fastrg_node_start_time_seconds. */
    fastrg_ccb->node_start_time = (uint64_t)time(NULL);

    /* Restart counter persisted across restarts for crashloop detection. */
    uint64_t restart_count = 0;
    FILE *rf = fopen(RESTART_COUNT_FILE, "r");
    if (rf) {
        if (fscanf(rf, "%" SCNu64, &restart_count) != 1)
            restart_count = 0;
        fclose(rf);
    }
    restart_count++;
    mkdir(RESTART_COUNT_DIR, 0755); /* ignore EEXIST */
    rf = fopen(RESTART_COUNT_FILE, "w");
    if (rf) {
        fprintf(rf, "%" PRIu64 "\n", restart_count);
        fclose(rf);
    } else {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
            "Cannot persist restart count to %s", RESTART_COUNT_FILE);
    }
    fastrg_ccb->node_restart_total = restart_count;

    /* Seed the per-port link-state cache from current NIC link status. */
    for(U8 p=0; p<PORT_AMOUNT; p++) {
        struct rte_eth_link lnk = {0};
        if (rte_eth_link_get_nowait(p, &lnk) == 0) {
            fastrg_ccb->nic_link_up[p] = lnk.link_status ? 1 : 0;
            fastrg_ccb->nic_link_speed[p] = lnk.link_status ? lnk.link_speed : 0;
        } else {
            fastrg_ccb->nic_link_up[p] = 0;
            fastrg_ccb->nic_link_speed[p] = 0;
        }
        fastrg_ccb->nic_link_flaps[p] = 0;
    }
}

/* Startup latch for the metrics listener: pending until the thread reports,
 * then ok or failed exactly once, same contract as the gRPC server latch. */
typedef enum {
    METRICS_STARTUP_PENDING,
    METRICS_STARTUP_OK,
    METRICS_STARTUP_FAILED,
} metrics_startup_state_t;

static pthread_mutex_t metrics_startup_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t metrics_startup_cv = PTHREAD_COND_INITIALIZER;
static metrics_startup_state_t metrics_startup_result = METRICS_STARTUP_PENDING;

/**
 * @fn metrics_startup_publish
 *
 * @brief Hand the metrics listener's verdict to whoever is waiting on it.
 *
 *        Both outcomes must be published, or a failed bind blocks the startup
 *        path forever.
 *
 * @param result
 *      METRICS_STARTUP_OK or METRICS_STARTUP_FAILED
 */
static void metrics_startup_publish(metrics_startup_state_t result)
{
    pthread_mutex_lock(&metrics_startup_lock);
    metrics_startup_result = result;
    pthread_mutex_unlock(&metrics_startup_lock);
    pthread_cond_broadcast(&metrics_startup_cv);
}

int metrics_server_wait_ready(void)
{
    metrics_startup_state_t result;

    pthread_mutex_lock(&metrics_startup_lock);
    while (metrics_startup_result == METRICS_STARTUP_PENDING)
        pthread_cond_wait(&metrics_startup_cv, &metrics_startup_lock);
    /* Consume the verdict and re-arm the latch: every metrics thread launch
     * pairs with exactly one wait. */
    result = metrics_startup_result;
    metrics_startup_result = METRICS_STARTUP_PENDING;
    pthread_mutex_unlock(&metrics_startup_lock);

    return result == METRICS_STARTUP_OK ? 0 : -1;
}

void *metrics_server_run(void *arg)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)arg;
    lighthttp_server_t *srv = &fastrg_ccb->metrics_server;

    if (lighthttp_init(srv, fastrg_ccb->metrics_ip_port) != 0) {
        int saved = errno;

        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "metrics: failed to start /metrics server on %s: %s (errno %d)",
            fastrg_ccb->metrics_ip_port, strerror(saved), saved);
        metrics_startup_publish(METRICS_STARTUP_FAILED);
        return NULL;
    }
    lighthttp_add_route(srv, "GET", "/metrics", metrics_build, fastrg_ccb);
    /* Future read-only endpoints (e.g. GET /healthz) register here. */

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "metrics: Prometheus /metrics listening on %s:%d", srv->host, srv->port);
    metrics_startup_publish(METRICS_STARTUP_OK);

    /* Publish lighthttp_init()'s listen_fd store before loading the stop request.
     * This full barrier prevents a StoreLoad reorder where the stop path sees -1
     * while this thread sees the old stop flag, leaving accept() and join blocked. */
    rte_smp_mb();
    if (rte_atomic16_read(&fastrg_ccb->metrics_stop_requested))
        lighthttp_stop(srv);
    else
        lighthttp_serve(srv); /* blocks until the listen socket fails */

    return NULL;
}

STATUS sys_init(FastRG_t *fastrg_ccb, struct fastrg_config *fastrg_cfg)
{
    STATUS ret;

    ret = init_mem(fastrg_ccb);
    if (ret)
        goto err;
    ret = init_ring(fastrg_ccb);
    if (ret)
        goto err;

    /* init RTE timer library */
    rte_timer_subsystem_init();

    ret = init_port(fastrg_ccb, fastrg_cfg);
    if (ret != 0)
        goto err;

    /* Build the "which lcore owns which TX queue" table. Must run after port
     * setup: the NIC may grant fewer TX queues than requested.  */
    if (dp_tx_handoff_pkt_init(fastrg_ccb) != SUCCESS) {
        ret = ERROR;
        goto err;
    }

    if (fastrg_compute_max_user_count(fastrg_ccb) != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Cannot determine subscriber capacity");
        goto err;
    }

    /* Seed Prometheus observability state now that NIC ports are up. */
    init_node_runtime_state(fastrg_ccb);

    rte_timer_init(&fastrg_ccb->link);
    rte_timer_init(&fastrg_ccb->heartbeat_timer);
    rte_timer_init(&fastrg_ccb->nd6_age_timer);

    size_t rcu_size = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);

    fastrg_ccb->pdump_rcu = fastrg_calloc(struct rte_rcu_qsbr, 1, rcu_size, RTE_CACHE_LINE_SIZE);
    if (fastrg_ccb->pdump_rcu == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Cannot allocate memory for pdump_rcu");
        goto err;
    }
    ret = rte_rcu_qsbr_init(fastrg_ccb->pdump_rcu, RTE_MAX_LCORE);
    if (ret != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "rte_rcu_qsbr_init failed for pdump_rcu: %s", rte_strerror(-ret));
        goto err;
    }
    unsigned int lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        ret = rte_rcu_qsbr_thread_register(fastrg_ccb->pdump_rcu, lcore_id);
        if (ret != 0) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                "rte_rcu_qsbr_thread_register failed for pdump_rcu lcore %u: %s",
                lcore_id, rte_strerror(-ret));
            goto err;
        }
    }

    /* Initialize ARP pending mempool for MAC table resolution */
    ret = arp_pending_init_pool(&fastrg_ccb->arp_pending_mp);
    if (ret != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Cannot create ARP pending mempool");
        goto err;
    }

    fastrg_ccb->lcore_usage = fastrg_calloc(struct lcore_usage_counter,
        RTE_MAX_LCORE, sizeof(struct lcore_usage_counter), RTE_CACHE_LINE_SIZE);
    if (fastrg_ccb->lcore_usage == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Cannot allocate memory for lcore_usage counters");
        goto err;
    }

    /* One applied-revision slot per subscriber, same fixed-max lifetime as the
     * stats rows below. */
    fastrg_ccb->hsi_subscriber_last_revision =
        fastrg_calloc(S64, fastrg_ccb->max_user_count, sizeof(S64), 0);
    if (fastrg_ccb->hsi_subscriber_last_revision == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot allocate hsi_subscriber_last_revision");
        goto err;
    }

    /* Fixed-max stats rows: allocate every EAL lcore's per-port subscriber
     * row and PPPoE-session row once, sized max_user_count+1 (the last entry
     * is the unknown-user slot). The rows are never resized or swapped at
     * runtime; they are freed only at shutdown. */
    RTE_LCORE_FOREACH(lcore_id) {
        for(int i=0; i<PORT_AMOUNT; i++) {
            fastrg_ccb->per_subscriber_stats[lcore_id][i] =
                fastrg_calloc(struct per_ccb_stats,
                    (fastrg_ccb->max_user_count + 1), sizeof(struct per_ccb_stats), 0);
            if (fastrg_ccb->per_subscriber_stats[lcore_id][i] == NULL) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                    "Cannot allocate per_subscriber_stats[%u][%d]", lcore_id, i);
                goto err;
            }
        }
        fastrg_ccb->pppoes_stats[lcore_id] =
            fastrg_calloc(struct pppoes_lcore_stats,
                (fastrg_ccb->max_user_count + 1), sizeof(struct pppoes_lcore_stats), 0);
        if (fastrg_ccb->pppoes_stats[lcore_id] == NULL) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                "Cannot allocate pppoes_stats[%u]", lcore_id);
            goto err;
        }
    }

    return SUCCESS;
err:
    if (fastrg_ccb->lcore_usage != NULL) {
        fastrg_mfree(fastrg_ccb->lcore_usage);
        fastrg_ccb->lcore_usage = NULL;
    }
    if (fastrg_ccb->hsi_subscriber_last_revision != NULL) {
        fastrg_mfree(fastrg_ccb->hsi_subscriber_last_revision);
        fastrg_ccb->hsi_subscriber_last_revision = NULL;
    }
    cleanup_ring(fastrg_ccb);
    cleanup_mem();
    arp_pending_cleanup_pool(&fastrg_ccb->arp_pending_mp);
    if (fastrg_ccb->pdump_rcu) {
        fastrg_mfree(fastrg_ccb->pdump_rcu);
        fastrg_ccb->pdump_rcu = NULL;
    }
    if (fastrg_ccb->node_uuid != NULL) {
        fastrg_mfree(fastrg_ccb->node_uuid);
        fastrg_ccb->node_uuid = NULL;
    }
    unsigned int lcore_id_err;
    RTE_LCORE_FOREACH(lcore_id_err) {
        for(int i=0; i<PORT_AMOUNT; i++) {
            if (fastrg_ccb->per_subscriber_stats[lcore_id_err][i] != NULL) {
                fastrg_mfree(fastrg_ccb->per_subscriber_stats[lcore_id_err][i]);
                fastrg_ccb->per_subscriber_stats[lcore_id_err][i] = NULL;
            }
        }
        if (fastrg_ccb->pppoes_stats[lcore_id_err] != NULL) {
            fastrg_mfree(fastrg_ccb->pppoes_stats[lcore_id_err]);
            fastrg_ccb->pppoes_stats[lcore_id_err] = NULL;
        }
    }

    return ERROR;
}

void sys_cleanup(FastRG_t *fastrg_ccb)
{
    unsigned int lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        for(int i=0; i<PORT_AMOUNT; i++) {
            if (fastrg_ccb->per_subscriber_stats[lcore_id][i] != NULL) {
                fastrg_mfree(fastrg_ccb->per_subscriber_stats[lcore_id][i]);
                fastrg_ccb->per_subscriber_stats[lcore_id][i] = NULL;
            }
        }
        if (fastrg_ccb->pppoes_stats[lcore_id] != NULL) {
            fastrg_mfree(fastrg_ccb->pppoes_stats[lcore_id]);
            fastrg_ccb->pppoes_stats[lcore_id] = NULL;
        }
    }

    arp_pending_cleanup_pool(&fastrg_ccb->arp_pending_mp);

    if (fastrg_ccb->node_uuid != NULL) {
        fastrg_mfree(fastrg_ccb->node_uuid);
        fastrg_ccb->node_uuid = NULL;
    }

    if (fastrg_ccb->lcore_usage != NULL) {
        fastrg_mfree(fastrg_ccb->lcore_usage);
        fastrg_ccb->lcore_usage = NULL;
    }

    if (fastrg_ccb->hsi_subscriber_last_revision != NULL) {
        fastrg_mfree(fastrg_ccb->hsi_subscriber_last_revision);
        fastrg_ccb->hsi_subscriber_last_revision = NULL;
    }

    cleanup_ring(fastrg_ccb);
    cleanup_mem();
}
