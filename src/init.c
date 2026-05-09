#include <sys/signalfd.h>
#include <signal.h>
#include <linux/ethtool.h>

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
#include "../northbound/controller/controller_client.h"

#define NUM_MBUFS 		8191
#define MBUF_CACHE_SIZE 512
#define RING_SIZE 		16384

struct rte_ring    *cp_q, *free_mail_ring;
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

void cleanup_ring()
{
    if (free_mail_ring != NULL) {
        void *mail_slot;
        while (rte_ring_dequeue(free_mail_ring, &mail_slot) == 0)
            fastrg_mfree(mail_slot);
        rte_ring_free(free_mail_ring);
    }
    if (cp_q != NULL)
        rte_ring_free(cp_q);
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
    cp_q = rte_ring_create("state_machine",RING_SIZE,rte_socket_id(),0);
    if (!cp_q) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot create state_machine ring: %s", rte_strerror(rte_errno));
        return ERROR;
    }

    /* Create free mail ring for pre-allocated mail slots */
    free_mail_ring = rte_ring_create("free_mail_ring", RING_BURST_SIZE, rte_socket_id(), RING_F_SC_DEQ);
    if (!free_mail_ring) {
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
        if (rte_ring_enqueue(free_mail_ring, mail_slot) != 0) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Cannot enqueue mail_slot to free_mail_ring: %s", rte_strerror(rte_errno));
            fastrg_mfree(mail_slot);
            goto err;
        }
    }

    return SUCCESS;

err:
    cleanup_ring();
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

        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Port %i driver: %s (ver: %s)", portid, dev_info.driver, dev_info.version);
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "firmware-version: %s", dev_info.fw_version);
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "bus-info: %s", dev_info.bus_info);

        fastrg_ccb->i40e_ddp_enabled = FALSE;
        if (fastrg_ccb->nic_info.vendor_id == NIC_VENDOR_I40E &&
                fastrg_cfg->ddp_pkg_path[0] != '\0') {
            if (i40e_load_ddp_package(fastrg_ccb, fastrg_cfg->ddp_pkg_path) == SUCCESS) {
                fastrg_ccb->i40e_ddp_enabled = TRUE;
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                    "i40e DDP package loaded, multi-queue RSS will be enabled");
            } else {
                FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                    "i40e DDP load failed, falling back to single queue mode");
            }
        }

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

    /* Initialize per_subscriber_stats using RCU-safe function */
    ret = fastrg_add_subscriber_stats(fastrg_ccb, fastrg_ccb->user_count);
    if (ret != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "Cannot initialize per_subscriber_stats");
        goto err;
    }

    return SUCCESS;
err:
    cleanup_ring();
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
    for(int i=0; i<PORT_AMOUNT; i++) {
        if (fastrg_ccb->per_subscriber_stats[i] != NULL) {
            fastrg_mfree(fastrg_ccb->per_subscriber_stats[i]);
            fastrg_ccb->per_subscriber_stats[i] = NULL;
        }
    }
    return ERROR;
}

void sys_cleanup(FastRG_t *fastrg_ccb)
{
    for(int i=0; i<PORT_AMOUNT; i++) {
        if (fastrg_ccb->per_subscriber_stats[i] != NULL) {
            fastrg_mfree(fastrg_ccb->per_subscriber_stats[i]);
            fastrg_ccb->per_subscriber_stats[i] = NULL;
        }
    }

    arp_pending_cleanup_pool(&fastrg_ccb->arp_pending_mp);

    if (fastrg_ccb->node_uuid != NULL) {
        fastrg_mfree(fastrg_ccb->node_uuid);
        fastrg_ccb->node_uuid = NULL;
    }

    cleanup_ring();
    cleanup_mem();
}
