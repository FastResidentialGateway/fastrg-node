#include <common.h>

#include <rte_mbuf.h>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_errno.h>

#include "../fastrg.h"
#include "../dbg.h"
#include "dhcp_fsm.h"

/* double size of 10.0.0.0-10.0.255.255 */
#define DHCP_MAX_POOL_SIZE_PER_USER  (1 << 17)

struct rte_ether_addr zero_mac;

void alloc_new_pool(dhcp_ccb_t *dhcp_ccb, U32 new_pool_len)
{
    FastRG_t *fastrg_ccb = dhcp_ccb->fastrg_ccb;
    U32 old_pool_len = dhcp_ccb->per_lan_user_pool_len;
    dhcp_ccb_per_lan_user_t **dhcp_ccb_per_lan_user = fastrg_calloc(dhcp_ccb_per_lan_user_t *, 
        new_pool_len, sizeof(dhcp_ccb_per_lan_user_t *), 0);

    if (dhcp_ccb_per_lan_user == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "DHCP: calloc dhcp_ccb_per_lan_user failed\n");
        return;
    }

    /* If there is an existing pool, copy it to the new one */
    if (old_pool_len > 0 && dhcp_ccb->per_lan_user_pool != NULL) {
        rte_memcpy(dhcp_ccb_per_lan_user, dhcp_ccb->per_lan_user_pool, 
            old_pool_len * sizeof(dhcp_ccb_per_lan_user_t *));
    }
    U32 need = new_pool_len - old_pool_len;
    int ret = rte_mempool_get_bulk(dhcp_ccb->dhcp_per_lan_user_mempool, 
        (void **)&dhcp_ccb_per_lan_user[old_pool_len], need);
    if (ret < 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "DHCP: rte_mempool_get_bulk failed\n");
        fastrg_mfree(dhcp_ccb_per_lan_user);
        return;
    }

    if (dhcp_ccb->per_lan_user_pool != NULL)
        fastrg_mfree(dhcp_ccb->per_lan_user_pool);

    dhcp_ccb->per_lan_user_pool_len = new_pool_len;
    dhcp_ccb->per_lan_user_pool = dhcp_ccb_per_lan_user;
}

void adjust_ip_pool(dhcp_ccb_t *dhcp_ccb, U32 new_pool_len)
{
    if (dhcp_ccb->per_lan_user_pool_len < new_pool_len) {
        alloc_new_pool(dhcp_ccb, new_pool_len);
    } else if (dhcp_ccb->per_lan_user_pool_len > new_pool_len) {
        for(U32 i=new_pool_len; i<dhcp_ccb->per_lan_user_pool_len; i++) {
            if (dhcp_ccb->per_lan_user_pool[i]) {
                rte_timer_stop_sync(&dhcp_ccb->per_lan_user_pool[i]->lan_user_info.timer);

                /* Clear state to ensure clean release */
                dhcp_ccb->per_lan_user_pool[i]->ip_pool.used = FALSE;
                dhcp_ccb->per_lan_user_pool[i]->lan_user_info.lan_user_used = FALSE;
            }
        }
        rte_mempool_put_bulk(dhcp_ccb->dhcp_per_lan_user_mempool,
            (void **)&dhcp_ccb->per_lan_user_pool[new_pool_len],
            dhcp_ccb->per_lan_user_pool_len - new_pool_len);
        dhcp_ccb->per_lan_user_pool_len = new_pool_len;
    }
}

void dhcp_pool_init_by_user(dhcp_ccb_t *dhcp_ccb, U32 dhcp_server_ip, 
    U32 ip_start, U32 ip_end, U32 subnet_mask)
{
    /* In pool update scenario, we don't need to lock here because in dp, 
    each dhcp pool field is only able to be accessed while the dhcp switch is on. */
    U32 new_pool_len = rte_be_to_cpu_32(ip_end) >= rte_be_to_cpu_32(ip_start) ? 
        rte_be_to_cpu_32(ip_end) - rte_be_to_cpu_32(ip_start) + 1 : 
        rte_be_to_cpu_32(ip_start) - rte_be_to_cpu_32(ip_end) + 1;
    U32 old_pool_len = dhcp_ccb->per_lan_user_pool_len;
    dhcp_ccb->dhcp_server_ip = dhcp_server_ip; //default dhcp server ip is user provided
    dhcp_ccb->subnet_mask = subnet_mask;
    FastRG_t *fastrg_ccb = dhcp_ccb->fastrg_ccb;

    adjust_ip_pool(dhcp_ccb, new_pool_len);
    if (dhcp_ccb->per_lan_user_pool_len != new_pool_len) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "DHCP: adjust_ip_pool failed\n");
        return;
    }

    for(U32 i=0; i<new_pool_len; i++) {
        dhcp_ccb->per_lan_user_pool[i]->ip_pool.used = FALSE;
        /* old timers should only be stopped in case they are running */
        if (i < old_pool_len)
            rte_timer_stop_sync(&dhcp_ccb->per_lan_user_pool[i]->lan_user_info.timer);
        else
            rte_timer_init(&dhcp_ccb->per_lan_user_pool[i]->lan_user_info.timer);
        dhcp_ccb->per_lan_user_pool[i]->lan_user_info.lan_user_used = FALSE;
        rte_ether_addr_copy(&zero_mac, &dhcp_ccb->per_lan_user_pool[i]->lan_user_info.mac_addr);
        rte_ether_addr_copy(&zero_mac, &dhcp_ccb->per_lan_user_pool[i]->ip_pool.mac_addr);
        dhcp_ccb->per_lan_user_pool[i]->lan_user_info.state = S_DHCP_INIT;
        dhcp_ccb->per_lan_user_pool[i]->ip_pool.ip_addr = rte_cpu_to_be_32((rte_be_to_cpu_32(ip_start) + i));
        dhcp_ccb->per_lan_user_pool[i]->dhcp_ccb = dhcp_ccb;
        dhcp_ccb->per_lan_user_pool[i]->pool_index = i;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
        "DHCP: DHCP pool initialized: server_ip=0x%08x, pool_start=0x%08x, pool_end=0x%08x, pool_len=%d, subnet_mask=0x%08x\n", 
        rte_be_to_cpu_32(dhcp_ccb->dhcp_server_ip), rte_be_to_cpu_32(ip_start), 
        rte_be_to_cpu_32(ip_end), new_pool_len, rte_be_to_cpu_32(subnet_mask));
}

void dhcp_init_by_user(dhcp_ccb_t *dhcp_ccb, U16 ccb_id, 
    struct rte_mempool *dhcp_per_lan_user_mempool)
{
    FastRG_t *fastrg_ccb = dhcp_ccb->fastrg_ccb;

    dhcp_ccb->dhcp_per_lan_user_mempool = dhcp_per_lan_user_mempool;
    dhcp_ccb->log_fp = fastrg_ccb->fp;
    dhcp_ccb->ccb_id = ccb_id;
    rte_atomic16_init(&dhcp_ccb->dhcp_bool);
    rte_atomic32_init(&dhcp_ccb->active_count);
    // TODO: make this configurable via northbound interface
    dhcp_ccb->dns_state.dns_proxy_enabled = TRUE;

    // critical section
    dhcp_pool_init_by_user(dhcp_ccb, 0, 0, 0, 0); //initialize with empty pool
}

STATUS dhcpd_allocate_ccbs(FastRG_t *fastrg_ccb, U16 start_id, U16 count, 
    dhcp_ccb_t **array, struct rte_mempool *dhcp_per_lan_user_mempool)
{
    for(U16 i=0; i<count; i++) {
        U16 ccb_id = start_id + i;

        if (rte_mempool_get(fastrg_ccb->dhcp_ccb_mp, 
                (void **)&array[ccb_id]) < 0) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
                "rte_mempool_get for dhcp_ccb[%u] failed: %s (available: %u)", 
                ccb_id, rte_strerror(rte_errno),
                rte_mempool_avail_count(fastrg_ccb->dhcp_ccb_mp));

            for(U16 j=start_id; j<ccb_id; j++) {
                rte_mempool_put(fastrg_ccb->dhcp_ccb_mp, array[j]);
                array[j] = NULL;
            }
            return ERROR;
        }

        memset(array[ccb_id], 0, sizeof(dhcp_ccb_t));
        array[ccb_id]->fastrg_ccb = fastrg_ccb;
        dhcp_init_by_user(array[ccb_id], ccb_id, dhcp_per_lan_user_mempool);
    }

    return SUCCESS;
}

STATUS dhcpd_init_rcu(FastRG_t *fastrg_ccb)
{
    size_t sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    fastrg_ccb->dhcp_ccb_rcu = fastrg_calloc(struct rte_rcu_qsbr, 1, sz, RTE_CACHE_LINE_SIZE);
    if (fastrg_ccb->dhcp_ccb_rcu == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, "rte_zmalloc for RCU failed");
        return ERROR;
    }

    if (rte_rcu_qsbr_init(fastrg_ccb->dhcp_ccb_rcu, RTE_MAX_LCORE) != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, "rte_rcu_qsbr_init failed");
        fastrg_mfree(fastrg_ccb->dhcp_ccb_rcu);
        return ERROR;
    }

    unsigned int lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        rte_rcu_qsbr_thread_register(fastrg_ccb->dhcp_ccb_rcu, lcore_id);
    }

    rte_atomic16_init(&fastrg_ccb->dhcp_ccb_updating);

    return SUCCESS;
}

STATUS dhcpd_add_ccb(FastRG_t *fastrg_ccb, U16 extra_ccb_count)
{
    if (extra_ccb_count == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "extra_ccb_count is 0, nothing to do");
        return SUCCESS;
    }

    if (rte_mempool_in_use_count(fastrg_ccb->dhcp_ccb_mp) > fastrg_ccb->user_count) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, DHCPLOGMSG, "we have unused ccb in mempool, no need to add more");
        return SUCCESS;
    }

    if (!rte_atomic16_cmpset((volatile uint16_t *)&fastrg_ccb->dhcp_ccb_updating.cnt, 0, 1)) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "Another resize operation is in progress");
        return ERROR;
    }

    dhcp_ccb_t **old_array = (dhcp_ccb_t **)fastrg_ccb->dhcp_ccb;
    U16 old_user_count = fastrg_ccb->user_count;
    U16 new_user_count = old_user_count + extra_ccb_count;

    dhcp_ccb_t **new_array = fastrg_malloc(dhcp_ccb_t *,  
        sizeof(dhcp_ccb_t *) * new_user_count, 0);
    if (new_array == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "malloc dhcp_ccb array failed");
        rte_atomic16_clear(&fastrg_ccb->dhcp_ccb_updating);
        return ERROR;
    }

    if (old_array != NULL)
        memcpy(new_array, old_array, sizeof(dhcp_ccb_t *) * old_user_count);

    memset(&new_array[old_user_count], 0, sizeof(dhcp_ccb_t *) * extra_ccb_count);

    /* Get the shared dhcp_per_lan_user_mempool, assuming it was created in dhcp_init */
    struct rte_mempool *dhcp_per_lan_user_mempool = rte_mempool_lookup("DHCP_PER_LAN_USER_MEMPOOL");
    if (dhcp_per_lan_user_mempool == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "Failed to lookup DHCP_PER_LAN_USER_MEMPOOL");
        fastrg_mfree(new_array);
        rte_atomic16_clear(&fastrg_ccb->dhcp_ccb_updating);
        return ERROR;
    }

    if (dhcpd_allocate_ccbs(fastrg_ccb, old_user_count, extra_ccb_count, 
            new_array, dhcp_per_lan_user_mempool) == ERROR) {
        fastrg_mfree(new_array);
        rte_atomic16_clear(&fastrg_ccb->dhcp_ccb_updating);
        return ERROR;
    }

    rte_wmb();

    __atomic_store_n(&fastrg_ccb->dhcp_ccb, new_array, __ATOMIC_RELEASE);

    if (old_array != NULL) {
        rte_rcu_qsbr_synchronize(fastrg_ccb->dhcp_ccb_rcu, RTE_QSBR_THRID_INVALID);
        fastrg_mfree(old_array);
    }

    rte_atomic16_clear(&fastrg_ccb->dhcp_ccb_updating);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
        "%u DHCP CCB added, mempool available: %u", 
        extra_ccb_count, rte_mempool_avail_count(fastrg_ccb->dhcp_ccb_mp));

    return SUCCESS;
}

STATUS dhcpd_disable_ccb(FastRG_t *fastrg_ccb, U16 disable_ccb_count, U16 old_ccb_count)
{
    if (disable_ccb_count > old_ccb_count) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "Invalid disabling ccb count %u", disable_ccb_count);
        return ERROR;
    }

    if (disable_ccb_count == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "disable_ccb_count is 0, nothing to do");
        return SUCCESS;
    }

    dhcp_ccb_t **old_array = (dhcp_ccb_t **)fastrg_ccb->dhcp_ccb;

    for(U16 i=0; i<disable_ccb_count; i++) {
        U16 ccb_id = old_ccb_count - 1 - i;
        dhcp_ccb_t *dhcp_ccb = old_array[ccb_id];

        /* Stop DHCP service */
        for(U32 j=0; j<dhcp_ccb->per_lan_user_pool_len; j++) {
            rte_atomic16_set(&dhcp_ccb->dhcp_bool, 0);
            if (dhcp_ccb->per_lan_user_pool[j] != NULL) {
                release_lan_user(&dhcp_ccb->per_lan_user_pool[j]->lan_user_info.timer, 
                    dhcp_ccb->per_lan_user_pool[j]);
            }
        }
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
        "%u DHCP CCBs disabled", 
        disable_ccb_count);

    return SUCCESS;
}

STATUS dhcpd_remove_ccb(FastRG_t *fastrg_ccb, U16 remove_ccb_count, U16 old_ccb_count)
{
    if (remove_ccb_count > old_ccb_count) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "Invalid removing ccb count %u", remove_ccb_count);
        return ERROR;
    }

    if (remove_ccb_count == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "remove_ccb_count is 0, nothing to do");
        return SUCCESS;
    }

    if (!rte_atomic16_cmpset((volatile uint16_t *)&fastrg_ccb->dhcp_ccb_updating.cnt, 0, 1)) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "Another resize operation is in progress");
        return ERROR;
    }

    dhcp_ccb_t **old_array = (dhcp_ccb_t **)fastrg_ccb->dhcp_ccb;
    U16 new_user_count = old_ccb_count - remove_ccb_count;

    for(U16 i=0; i<remove_ccb_count; i++) {
        U16 ccb_id = old_ccb_count - 1 - i;
        dhcp_ccb_t *dhcp_ccb = old_array[ccb_id];

        /* Stop DHCP service and wait for active processing to complete */
        rte_atomic16_set(&dhcp_ccb->dhcp_bool, 0);
        while(rte_atomic32_read(&dhcp_ccb->active_count) > 0)
            rte_pause();

        /* Free per-LAN-user pool */
        if (dhcp_ccb->per_lan_user_pool != NULL) {
            for(U32 j=0; j<dhcp_ccb->per_lan_user_pool_len; j++) {
                if (dhcp_ccb->per_lan_user_pool[j] != NULL)
                    rte_timer_stop_sync(&dhcp_ccb->per_lan_user_pool[j]->lan_user_info.timer);
            }
            if (dhcp_ccb->per_lan_user_pool_len > 0) {
                rte_mempool_put_bulk(dhcp_ccb->dhcp_per_lan_user_mempool,
                    (void **)dhcp_ccb->per_lan_user_pool,
                    dhcp_ccb->per_lan_user_pool_len);
            }
            fastrg_mfree(dhcp_ccb->per_lan_user_pool);
            dhcp_ccb->per_lan_user_pool = NULL;
        }

        rte_mempool_put(fastrg_ccb->dhcp_ccb_mp, old_array[ccb_id]);
        old_array[ccb_id] = NULL;
    }

    if (new_user_count == 0) {
        __atomic_store_n(&fastrg_ccb->dhcp_ccb, (dhcp_ccb_t **)NULL, __ATOMIC_RELEASE);

        rte_rcu_qsbr_synchronize(fastrg_ccb->dhcp_ccb_rcu, RTE_QSBR_THRID_INVALID);
        fastrg_mfree(old_array);
    } else {
        dhcp_ccb_t **new_array = fastrg_malloc(dhcp_ccb_t *, 
            sizeof(dhcp_ccb_t *) * new_user_count, 0);
        if (new_array == NULL) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
                "malloc new smaller dhcp_ccb array failed");
            rte_atomic16_clear(&fastrg_ccb->dhcp_ccb_updating);
            return ERROR;
        }

        rte_memcpy(new_array, old_array, sizeof(dhcp_ccb_t *) * new_user_count);

        rte_wmb();

        __atomic_store_n(&fastrg_ccb->dhcp_ccb, new_array, __ATOMIC_RELEASE);

        rte_rcu_qsbr_synchronize(fastrg_ccb->dhcp_ccb_rcu, RTE_QSBR_THRID_INVALID);

        fastrg_mfree(old_array);
    }

    rte_atomic16_clear(&fastrg_ccb->dhcp_ccb_updating);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
        "%u DHCP CCBs removed, mempool available: %u", 
        remove_ccb_count, rte_mempool_avail_count(fastrg_ccb->dhcp_ccb_mp));

    return SUCCESS;
}

void dhcpd_cleanup_ccb(FastRG_t *fastrg_ccb, U16 total_ccb_count)
{
    if (fastrg_ccb == NULL)
        return;

    if (fastrg_ccb->dhcp_ccb != NULL && total_ccb_count > 0)
        dhcpd_remove_ccb(fastrg_ccb, total_ccb_count, total_ccb_count);

    if (fastrg_ccb->dhcp_ccb_mp != NULL) {
        rte_mempool_free(fastrg_ccb->dhcp_ccb_mp);
        fastrg_ccb->dhcp_ccb_mp = NULL;
    }

    if (fastrg_ccb->dhcp_ccb_rcu != NULL) {
        fastrg_mfree(fastrg_ccb->dhcp_ccb_rcu);
        fastrg_ccb->dhcp_ccb_rcu = NULL;
    }

    /* Free the shared per-LAN-user mempool */
    struct rte_mempool *dhcp_per_lan_user_mempool = rte_mempool_lookup("DHCP_PER_LAN_USER_MEMPOOL");
    if (dhcp_per_lan_user_mempool != NULL)
        rte_mempool_free(dhcp_per_lan_user_mempool);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
        "DHCP cleanup completed");
}

STATUS dhcp_init(FastRG_t *fastrg_ccb)
{
    unsigned int mempool_size = 1U << (31 - __builtin_clz(fastrg_ccb->user_count) + 1);

    if (dhcpd_init_rcu(fastrg_ccb) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "dhcpd_init_rcu failed");
        return ERROR;
    }

    fastrg_ccb->dhcp_ccb_mp = rte_mempool_create(
        "dhcp_ccb_pool",                     /* name */
        mempool_size,                        /* user count */
        sizeof(dhcp_ccb_t),                  /* dhcp_ccb size */
        mempool_size * 2 / 3,                /* per-lcore cache size */
        0,                                   /* private_data_size */
        NULL, NULL,                          /* mp_init, mp_init_arg */
        NULL, NULL,                          /* obj_init, obj_init_arg */
        rte_socket_id(),                     /* socket_id */
        0                                    /* flags */
    );
    if (fastrg_ccb->dhcp_ccb_mp == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "rte_mempool_create failed: %s", rte_strerror(rte_errno));
        fastrg_mfree(fastrg_ccb->dhcp_ccb_rcu);
        return ERROR;
    }

    /* Create shared per-LAN-user mempool */
    struct rte_mempool *dhcp_per_lan_user_mempool = rte_mempool_create("DHCP_PER_LAN_USER_MEMPOOL", 
        DHCP_MAX_POOL_SIZE_PER_USER * fastrg_ccb->user_count, 
        sizeof(dhcp_ccb_per_lan_user_t), RTE_MEMPOOL_CACHE_MAX_SIZE, 0, NULL, NULL, NULL, NULL, 
        rte_socket_id(), 0);
    if (dhcp_per_lan_user_mempool == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, "Failed to create DHCP_PER_LAN_USER_MEMPOOL");
        rte_mempool_free(fastrg_ccb->dhcp_ccb_mp);
        fastrg_mfree(fastrg_ccb->dhcp_ccb_rcu);
        return ERROR;
    }

    for(int i=0; i<RTE_ETHER_ADDR_LEN; i++)
        zero_mac.addr_bytes[i] = 0;

    U16 initial_user_count = fastrg_ccb->user_count;
    /* assume we want to add ccbs from 0 to initial_user_count */
    fastrg_ccb->user_count = 0;
    fastrg_ccb->dhcp_ccb = NULL;
    if (dhcpd_add_ccb(fastrg_ccb, initial_user_count) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "dhcpd_add_ccb for initial %u CCBs failed", initial_user_count);
        rte_mempool_free(dhcp_per_lan_user_mempool);
        rte_mempool_free(fastrg_ccb->dhcp_ccb_mp);
        fastrg_mfree(fastrg_ccb->dhcp_ccb_rcu);
        return ERROR;
    }
    fastrg_ccb->user_count = initial_user_count;

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
        "============ DHCP init successfully ==============\n");
    return SUCCESS;
}

void release_lan_user(struct rte_timer *tim, dhcp_ccb_per_lan_user_t *per_lan_user_pool)
{
    rte_timer_stop(tim);
    per_lan_user_pool->ip_pool.used = FALSE;
    per_lan_user_pool->lan_user_info.lan_user_used = FALSE;
    rte_ether_addr_copy(&zero_mac, &per_lan_user_pool->lan_user_info.mac_addr);
    per_lan_user_pool->lan_user_info.state = S_DHCP_INIT;
}

int dhcpd(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    struct rte_ether_hdr *eth_hdr, vlan_header_t *vlan_header, 
    struct rte_ipv4_hdr *ip_hdr, struct rte_udp_hdr *udp_hdr, U16 ccb_id)
{
    BIT16 event;
    int cur_tmp_pool_index = -1;

    if (ccb_id >= fastrg_ccb->user_count) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "DHCP: invalid user_index %d\n", ccb_id);
        return -1;
    }

    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
    if (dhcp_ccb == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "DHCP: Invalid CCB ID %u in DHCP processing\n", ccb_id);
        return -1;
    }

    rte_atomic32_inc(&dhcp_ccb->active_count);

    /* On x86, rte_atomic32_inc() ensures the processing order of active_count 
        and dhcp_bool are strong order. Therefore, we don't need barrier here. */
    //rte_smp_mb();

    /* Double check to avoid control plane disables dhcp after we increase active_count */
    if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 0) {
        rte_atomic32_dec(&dhcp_ccb->active_count);
        return -1;
    }

    /* Temporarily pick one index from lan_user_info array and save it to dhcp_ccb */
    for(int i=0; i<dhcp_ccb->per_lan_user_pool_len; i++) {
        if (rte_is_same_ether_addr(&eth_hdr->src_addr, 
                &dhcp_ccb->per_lan_user_pool[i]->lan_user_info.mac_addr)) {
            cur_tmp_pool_index = i;
            break;
        } else if (dhcp_ccb->per_lan_user_pool[i]->lan_user_info.lan_user_used == FALSE) {
            cur_tmp_pool_index = i;
            rte_ether_addr_copy(&eth_hdr->src_addr, &dhcp_ccb->per_lan_user_pool[i]->lan_user_info.mac_addr);
            dhcp_ccb->per_lan_user_pool[i]->lan_user_info.lan_user_used = TRUE;
            break;
        }
    }
    /* If dhcp ip pool is full, drop the packet */
    if (cur_tmp_pool_index < 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "DHCP: no available lan_user_info entry\n");
        rte_atomic32_dec(&dhcp_ccb->active_count);
        return -1;
    }
    /* If no more packet from the host, clear all information in dhcp_ccb */
    rte_timer_stop_sync(&dhcp_ccb->per_lan_user_pool[cur_tmp_pool_index]->lan_user_info.timer);
    rte_timer_reset(&dhcp_ccb->per_lan_user_pool[cur_tmp_pool_index]->lan_user_info.timer, 
        LEASE_TIMEOUT * 2 * fastrg_get_cycles_in_sec(), SINGLE, 
        fastrg_ccb->lcore.ctrl_thread, (rte_timer_cb_t)release_lan_user, 
        dhcp_ccb->per_lan_user_pool[cur_tmp_pool_index]);

    event = dhcp_decode(dhcp_ccb, 
        dhcp_ccb->per_lan_user_pool[cur_tmp_pool_index], 
        &cur_tmp_pool_index, eth_hdr, vlan_header, ip_hdr, udp_hdr);
    if (event < 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "DHCP: dhcp_decode failed\n");
        release_lan_user(&dhcp_ccb->per_lan_user_pool[cur_tmp_pool_index]->lan_user_info.timer, 
            dhcp_ccb->per_lan_user_pool[cur_tmp_pool_index]);
        rte_atomic32_dec(&dhcp_ccb->active_count);
        return -1;
    } else if (event == 0) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "DHCP: no support dhcp option found\n");
        release_lan_user(&dhcp_ccb->per_lan_user_pool[cur_tmp_pool_index]->lan_user_info.timer, 
            dhcp_ccb->per_lan_user_pool[cur_tmp_pool_index]);
        rte_atomic32_dec(&dhcp_ccb->active_count);
        return 0;
    }
    FastRG_LOG(DBG, fastrg_ccb->fp, NULL, NULL, "DHCP: event = %d picked pool_index = %d\n", 
        event, cur_tmp_pool_index);

    if (dhcp_fsm(dhcp_ccb, cur_tmp_pool_index, event) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "DHCP: dhcp_fsm failed\n");
        release_lan_user(&dhcp_ccb->per_lan_user_pool[cur_tmp_pool_index]->lan_user_info.timer, 
            dhcp_ccb->per_lan_user_pool[cur_tmp_pool_index]);
        rte_atomic32_dec(&dhcp_ccb->active_count);
        return -1;
    }

    rte_atomic32_dec(&dhcp_ccb->active_count);
    return 1;
}
