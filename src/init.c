#include <sys/signalfd.h>
#include <signal.h>
#include <linux/ethtool.h>
#include <time.h>
#include <inttypes.h>
#include <sys/stat.h>

#include <common.h>

#include <rte_ring.h>
#include <rte_errno.h>
#include <rte_mbuf.h>
#include <rte_byteorder.h>
#include <rte_ethdev.h>

#include <uuid/uuid.h>

#include "pppd/pppd.h"
#include "dhcpd/dhcp_codec.h"
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
#define ETCD_EVENT_RING_SIZE 4096

/* Persisted restart counter for crashloop detection (fastrg_node_restart_total). */
#define RESTART_COUNT_DIR  "/var/lib/fastrg"
#define RESTART_COUNT_FILE RESTART_COUNT_DIR "/restart_count"

struct rte_mempool *direct_pool[PORT_AMOUNT];
struct rte_mempool *indirect_pool[PORT_AMOUNT];

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
    if (fastrg_ccb->etcd_event_q != NULL) {
        rte_ring_free(fastrg_ccb->etcd_event_q);
        fastrg_ccb->etcd_event_q = NULL;
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

    /* Create free mail ring for pre-allocated mail slots */
    fastrg_ccb->free_mail_ring = rte_ring_create("free_mail_ring", RING_BURST_SIZE, rte_socket_id(), RING_F_SC_DEQ);
    if (!fastrg_ccb->free_mail_ring) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot create free_mail_ring", rte_strerror(rte_errno));
        goto err;
    }

    /* Pre-allocate and enqueue 31 mail slots to free_mail_ring */
    for(int i=0; i<RING_BURST_SIZE-1; i++) {
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

    /* etcd watcher threads (multi-producer) -> fastrg_loop (single consumer) */
    fastrg_ccb->etcd_event_q = rte_ring_create("etcd_event_q", ETCD_EVENT_RING_SIZE,
        rte_socket_id(), RING_F_SC_DEQ);
    if (!fastrg_ccb->etcd_event_q) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot create etcd_event_q: %s", rte_strerror(rte_errno));
        goto err;
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
    if (fastrg_get_id(fastrg_ccb->node_uuid) == ERROR) {
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

/**
 * @fn metrics_server_run
 * @brief pthread entry point for the Prometheus /metrics HTTP server. Registers
 *        the metrics thread's RCU reader slot, binds lighthttp on the configured
 *        address and serves GET /metrics. Non-fatal on bind failure (observability
 *        only) — logs a warning and exits the thread.
 *
 * @param arg
 *      FastRG control block (FastRG_t *)
 */
void *metrics_server_run(void *arg)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)arg;
    lighthttp_server_t srv;

    /* Register our dedicated RCU reader slot before serving any scrape. */
    metrics_rcu_register(fastrg_ccb);

    if (lighthttp_init(&srv, fastrg_ccb->metrics_ip_port) != 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
            "metrics: failed to start /metrics server on %s; Prometheus scrape disabled",
            fastrg_ccb->metrics_ip_port);
        return NULL;
    }
    lighthttp_add_route(&srv, "GET", "/metrics", metrics_build, fastrg_ccb);
    /* Future read-only endpoints (e.g. GET /healthz) register here. */

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "metrics: Prometheus /metrics listening on %s:%d", srv.host, srv.port);

    lighthttp_serve(&srv); /* blocks until the listen socket fails */
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

    /* Seed Prometheus observability state now that NIC ports are up. */
    init_node_runtime_state(fastrg_ccb);

    rte_timer_init(&fastrg_ccb->link);
    rte_timer_init(&fastrg_ccb->heartbeat_timer);

    /* Initialize RCU for per_subscriber_stats */
    size_t rcu_size = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    fastrg_ccb->per_subscriber_stats_rcu = fastrg_calloc(struct rte_rcu_qsbr, 1, rcu_size, RTE_CACHE_LINE_SIZE);
    if (fastrg_ccb->per_subscriber_stats_rcu == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "Cannot allocate memory for per_subscriber_stats_rcu");
        goto err;
    }
    ret = rte_rcu_qsbr_init(fastrg_ccb->per_subscriber_stats_rcu, RTE_MAX_LCORE);
    if (ret != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "rte_rcu_qsbr_init failed for per_subscriber_stats_rcu: %s", rte_strerror(-ret));
        goto err;
    }

    /* Register all lcores for per_subscriber_stats RCU */
    unsigned int lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        ret = rte_rcu_qsbr_thread_register(fastrg_ccb->per_subscriber_stats_rcu, lcore_id);
        if (ret != 0) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                "rte_rcu_qsbr_thread_register failed for lcore %u: %s", 
                lcore_id, rte_strerror(-ret));
            goto err;
        }
    }

    rte_atomic16_init(&fastrg_ccb->per_subscriber_stats_updating);

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

    /* Initialize per_subscriber_stats using RCU-safe function */
    ret = fastrg_add_subscriber_stats(fastrg_ccb, fastrg_ccb->user_count);
    if (ret != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "Cannot initialize per_subscriber_stats");
        goto err;
    }
    /* NOTE: pppoes_stats is initialised in fastrg_start() AFTER pppd_init(),
     * because it is protected by ppp_ccb_rcu which pppd_init creates. */

    return SUCCESS;
err:
    if (fastrg_ccb->lcore_usage != NULL) {
        fastrg_mfree(fastrg_ccb->lcore_usage);
        fastrg_ccb->lcore_usage = NULL;
    }
    cleanup_ring(fastrg_ccb);
    cleanup_mem();
    arp_pending_cleanup_pool(&fastrg_ccb->arp_pending_mp);
    if (fastrg_ccb->per_subscriber_stats_rcu) {
        fastrg_mfree(fastrg_ccb->per_subscriber_stats_rcu);
        fastrg_ccb->per_subscriber_stats_rcu = NULL;
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

    cleanup_ring(fastrg_ccb);
    cleanup_mem();
}
