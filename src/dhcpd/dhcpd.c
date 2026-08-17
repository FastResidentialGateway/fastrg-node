#include <common.h>

#include <rte_mbuf.h>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_errno.h>

#include "../fastrg.h"
#include "../init.h"
#include "../dbg.h"
#include "dhcp_fsm.h"

/* Fixed per-subscriber pool capacity (double size of 10.0.0.0-10.0.255.255). */
#define DHCP_MAX_POOL_SIZE_PER_USER  (1 << 17)

struct rte_ether_addr zero_mac;

STATUS dhcp_pool_init_by_user(dhcp_ccb_t *dhcp_ccb, U32 dhcp_server_ip, 
    U32 ip_start, U32 ip_end, U32 subnet_mask)
{
    /* In pool update scenario, we don't need to lock here because in dp, 
    each dhcp pool field is only able to be accessed while the dhcp switch is on 
    and only the ctrl thread updates the pool. */
    U32 new_pool_len = rte_be_to_cpu_32(ip_end) >= rte_be_to_cpu_32(ip_start) ? 
        rte_be_to_cpu_32(ip_end) - rte_be_to_cpu_32(ip_start) + 1 : 
        rte_be_to_cpu_32(ip_start) - rte_be_to_cpu_32(ip_end) + 1;
    U32 old_pool_len = dhcp_ccb->per_lan_user_pool_len;
    FastRG_t *fastrg_ccb = dhcp_ccb->fastrg_ccb;

    if (new_pool_len > DHCP_MAX_POOL_SIZE_PER_USER) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG,
            "DHCP: requested pool length %u exceeds fixed per-user capacity %u, rejecting\n",
            new_pool_len, DHCP_MAX_POOL_SIZE_PER_USER);
        return ERROR;
    }

    dhcp_ccb->dhcp_server_ip = dhcp_server_ip; //default dhcp server ip is user provided
    dhcp_ccb->pool_start = rte_be_to_cpu_32(ip_start);
    dhcp_ccb->pool_end = rte_be_to_cpu_32(ip_end);
    dhcp_ccb->subnet_mask = subnet_mask;

    /* Stop every lease timer the previous window may have armed (timers are
     * only ever armed on window entries). */
    for(U32 i=0; i<old_pool_len; i++)
        rte_timer_stop_sync(&dhcp_ccb->per_lan_user_pool[i]->lan_user_info.timer);

    dhcp_ccb->per_lan_user_pool_len = new_pool_len;

    U32 reset_len = RTE_MAX(old_pool_len, new_pool_len);
    for(U32 i=0; i<reset_len; i++) {
        dhcp_ccb_per_lan_user_t *e = dhcp_ccb->per_lan_user_pool[i];
        e->ip_pool.used = FALSE;
        e->lan_user_info.lan_user_used = FALSE;
        rte_ether_addr_copy(&zero_mac, &e->lan_user_info.mac_addr);
        rte_ether_addr_copy(&zero_mac, &e->ip_pool.mac_addr);
        e->lan_user_info.state = S_DHCP_INIT;
        U32 entry_ip = (i < new_pool_len) ? (rte_be_to_cpu_32(ip_start) + i) : 0;
        e->ip_pool.ip_addr = rte_cpu_to_be_32(entry_ip);
        /* A range crossing an octet boundary contains network (.0) and
         * broadcast (.255) addresses a client cannot use. Keep them in the
         * array so the index <-> IP mapping stays intact, but mark them as 
         * reserved so the lease selection paths never hand them out. */
        e->ip_pool.reserved = (i < new_pool_len) ? dhcp_ip_is_reserved(entry_ip) : FALSE;
        e->dhcp_ccb = dhcp_ccb;
        e->pool_index = i;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
        "DHCP: DHCP pool initialized: server_ip=0x%08x, pool_start=0x%08x, pool_end=0x%08x, pool_len=%d, subnet_mask=0x%08x\n", 
        rte_be_to_cpu_32(dhcp_ccb->dhcp_server_ip), rte_be_to_cpu_32(ip_start), 
        rte_be_to_cpu_32(ip_end), new_pool_len, rte_be_to_cpu_32(subnet_mask));
    return SUCCESS;
}

STATUS dhcp_validate_pool_range(U32 dhcp_server_ip, U32 ip_start,
    U32 ip_end, U32 subnet_mask)
{
    /* The all-zero call clears the pool (disable / init time) and carries no
     * range to validate. */
    if (dhcp_server_ip == 0 && ip_start == 0 && ip_end == 0 && subnet_mask == 0)
        return SUCCESS;

    if (rte_be_to_cpu_32(ip_start) > rte_be_to_cpu_32(ip_end))
        return ERROR;

    /* The data plane decides whether a packet belongs to a subscriber LAN with
     * is_ip_in_range(ip, dhcp_server_ip, subnet_mask). A pool reaching outside
     * that subnet would be leased out but never recognized afterwards, so
     * refuse the configuration instead of accepting it silently. */
    if ((ip_start & subnet_mask) != (dhcp_server_ip & subnet_mask) ||
            (ip_end & subnet_mask) != (dhcp_server_ip & subnet_mask))
        return ERROR;

    return SUCCESS;
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
    /* Default before any HSI config is applied; overridden per-subscriber
     * by apply_hsi_config() using dns_proxy_enable from etcd. */
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
            goto err;
        }

        memset(array[ccb_id], 0, sizeof(dhcp_ccb_t));
        array[ccb_id]->fastrg_ccb = fastrg_ccb;

        array[ccb_id]->per_lan_user_pool = fastrg_calloc(dhcp_ccb_per_lan_user_t *,
            DHCP_MAX_POOL_SIZE_PER_USER, sizeof(dhcp_ccb_per_lan_user_t *), 0);
        if (array[ccb_id]->per_lan_user_pool == NULL) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG,
                "per_lan_user_pool array allocation failed for dhcp_ccb[%u]", ccb_id);
            rte_mempool_put(fastrg_ccb->dhcp_ccb_mp, array[ccb_id]);
            array[ccb_id] = NULL;
            goto err;
        }
        if (rte_mempool_get_bulk(dhcp_per_lan_user_mempool,
                (void **)array[ccb_id]->per_lan_user_pool,
                DHCP_MAX_POOL_SIZE_PER_USER) < 0) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG,
                "per-LAN-user entry preallocation failed for dhcp_ccb[%u]", ccb_id);
            fastrg_mfree(array[ccb_id]->per_lan_user_pool);
            rte_mempool_put(fastrg_ccb->dhcp_ccb_mp, array[ccb_id]);
            array[ccb_id] = NULL;
            goto err;
        }
        for(U32 j=0; j<DHCP_MAX_POOL_SIZE_PER_USER; j++) {
            dhcp_ccb_per_lan_user_t *e = array[ccb_id]->per_lan_user_pool[j];
            memset(e, 0, sizeof(*e));
            rte_timer_init(&e->lan_user_info.timer);
            e->dhcp_ccb = array[ccb_id];
            e->pool_index = j;
        }

        dhcp_init_by_user(array[ccb_id], ccb_id, dhcp_per_lan_user_mempool);
    }

    return SUCCESS;

err:
    for(U16 j=start_id; start_id+count>j; j++) {
        if (array[j] == NULL)
            continue;
        if (array[j]->per_lan_user_pool != NULL) {
            rte_mempool_put_bulk(dhcp_per_lan_user_mempool,
                (void **)array[j]->per_lan_user_pool,
                DHCP_MAX_POOL_SIZE_PER_USER);
            fastrg_mfree(array[j]->per_lan_user_pool);
            array[j]->per_lan_user_pool = NULL;
        }
        rte_mempool_put(fastrg_ccb->dhcp_ccb_mp, array[j]);
        array[j] = NULL;
    }
    return ERROR;
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
        if (dhcp_ccb == NULL)
            continue;

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

void dhcpd_cleanup_ccb(FastRG_t *fastrg_ccb)
{
    if (fastrg_ccb == NULL)
        return;

    if (fastrg_ccb->dhcp_ccb != NULL) {
        for(U16 ccb_id=0; ccb_id<fastrg_ccb->max_user_count; ccb_id++) {
            dhcp_ccb_t *dhcp_ccb = (dhcp_ccb_t *)fastrg_ccb->dhcp_ccb[ccb_id];
            if (dhcp_ccb == NULL)
                continue;

            rte_atomic16_set(&dhcp_ccb->dhcp_bool, 0);

            if (dhcp_ccb->per_lan_user_pool != NULL) {
                for(U32 j=0; j<dhcp_ccb->per_lan_user_pool_len; j++) {
                    if (dhcp_ccb->per_lan_user_pool[j] != NULL)
                        rte_timer_stop_sync(&dhcp_ccb->per_lan_user_pool[j]->lan_user_info.timer);
                }
                rte_mempool_put_bulk(dhcp_ccb->dhcp_per_lan_user_mempool,
                    (void **)dhcp_ccb->per_lan_user_pool,
                    DHCP_MAX_POOL_SIZE_PER_USER);
                fastrg_mfree(dhcp_ccb->per_lan_user_pool);
                dhcp_ccb->per_lan_user_pool = NULL;
            }

            rte_mempool_put(fastrg_ccb->dhcp_ccb_mp, dhcp_ccb);
        }
        fastrg_mfree(fastrg_ccb->dhcp_ccb);
        fastrg_ccb->dhcp_ccb = NULL;
    }

    if (fastrg_ccb->dhcp_ccb_mp != NULL) {
        rte_mempool_free(fastrg_ccb->dhcp_ccb_mp);
        fastrg_ccb->dhcp_ccb_mp = NULL;
    }

    /* Free the shared per-LAN-user mempool */
    struct rte_mempool *dhcp_per_lan_user_mempool = rte_mempool_lookup("DHCP_PER_LAN_USER_MEMPOOL");
    if (dhcp_per_lan_user_mempool != NULL)
        rte_mempool_free(dhcp_per_lan_user_mempool);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
        "DHCP cleanup completed");
}

void dhcp_get_subscriber_real_size(ccb_memory_info_t *out)
{
    if (out == NULL)
        return;

    memset(out, 0, sizeof(*out));
    /* The per-subscriber array of lease pointers, plus this subscriber's slot
     * in the ccb pointer array. */
    out->plain_bytes_per_sub =
        (uint64_t)DHCP_MAX_POOL_SIZE_PER_USER * sizeof(dhcp_ccb_per_lan_user_t *) +
        sizeof(dhcp_ccb_t *);
    out->n_pools = 2;
    out->pools[0].objs_per_sub = 1;
    out->pools[0].elt_size = sizeof(dhcp_ccb_t);
    /* Every subscriber reserves a full lease block in the shared pool. */
    out->pools[1].objs_per_sub = DHCP_MAX_POOL_SIZE_PER_USER;
    out->pools[1].elt_size = sizeof(dhcp_ccb_per_lan_user_t);
}

STATUS dhcp_init(FastRG_t *fastrg_ccb)
{
    fastrg_ccb->dhcp_ccb_mp = rte_mempool_create(
        "dhcp_ccb_pool",                     /* name */
        fastrg_ccb->max_user_count,          /* user count */
        sizeof(dhcp_ccb_t),                  /* dhcp_ccb size */
        /* No per-lcore cache: every object is taken exactly once at init and
         * returned only at shutdown. */
        0,                                   /* per-lcore cache size */
        0,                                   /* private_data_size */
        NULL, NULL,                          /* mp_init, mp_init_arg */
        NULL, NULL,                          /* obj_init, obj_init_arg */
        rte_socket_id(),                     /* socket_id */
        0                                    /* flags */
    );
    if (fastrg_ccb->dhcp_ccb_mp == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, 
            "rte_mempool_create failed: %s", rte_strerror(rte_errno));
        return ERROR;
    }

    /* No per-lcore cache: every object is taken exactly once at init and
     * returned only at shutdown. */
    struct rte_mempool *dhcp_per_lan_user_mempool = rte_mempool_create("DHCP_PER_LAN_USER_MEMPOOL", 
        DHCP_MAX_POOL_SIZE_PER_USER * fastrg_ccb->max_user_count, 
        sizeof(dhcp_ccb_per_lan_user_t), 0, 0, NULL, NULL, NULL, NULL, 
        rte_socket_id(), 0);
    if (dhcp_per_lan_user_mempool == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG, "Failed to create DHCP_PER_LAN_USER_MEMPOOL");
        rte_mempool_free(fastrg_ccb->dhcp_ccb_mp);
        fastrg_ccb->dhcp_ccb_mp = NULL;
        return ERROR;
    }

    for(int i=0; i<RTE_ETHER_ADDR_LEN; i++)
        zero_mac.addr_bytes[i] = 0;

    /* Fixed-max full preallocation, mirroring pppd_init: array + all slots
     * (with their full per-LAN-user entry capacity) are allocated now; the
     * array pointer never changes afterwards. */
    fastrg_ccb->dhcp_ccb = (void **)fastrg_calloc(dhcp_ccb_t *,
        fastrg_ccb->max_user_count, sizeof(dhcp_ccb_t *), 0);
    if (fastrg_ccb->dhcp_ccb == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG,
            "dhcp_ccb array allocation failed");
        rte_mempool_free(dhcp_per_lan_user_mempool);
        rte_mempool_free(fastrg_ccb->dhcp_ccb_mp);
        fastrg_ccb->dhcp_ccb_mp = NULL;
        return ERROR;
    }

    if (dhcpd_allocate_ccbs(fastrg_ccb, 0, fastrg_ccb->max_user_count,
            (dhcp_ccb_t **)fastrg_ccb->dhcp_ccb, dhcp_per_lan_user_mempool) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, DHCPLOGMSG,
            "preallocating %u DHCP CCBs failed", fastrg_ccb->max_user_count);
        fastrg_mfree(fastrg_ccb->dhcp_ccb);
        fastrg_ccb->dhcp_ccb = NULL;
        rte_mempool_free(dhcp_per_lan_user_mempool);
        rte_mempool_free(fastrg_ccb->dhcp_ccb_mp);
        fastrg_ccb->dhcp_ccb_mp = NULL;
        return ERROR;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, DHCPLOGMSG,
        "============ DHCP init successfully (%u slots preallocated) ==============\n",
        fastrg_ccb->max_user_count);
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
    S16 event;
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

    if (single_pkt != NULL) {
        U8 *pkt_data = rte_pktmbuf_mtod(single_pkt, U8 *);
        U32 udp_offset = (U32)((U8 *)udp_hdr - pkt_data);
        U16 udp_len = rte_be_to_cpu_16(udp_hdr->dgram_len);
        if (udp_offset + udp_len > rte_pktmbuf_pkt_len(single_pkt)) {
            rte_atomic32_dec(&dhcp_ccb->active_count);
            return -1;
        }
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
