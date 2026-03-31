/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  PPPD.C

    - purpose : for ppp detection

  Designed by THE on Jan 14, 2019
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#include <common.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_timer.h>
#include <rte_ether.h>
#include <rte_memcpy.h>
#include <rte_flow.h>
#include <rte_atomic.h>
#include <rte_pdump.h>
#include <rte_trace.h>
#include <rte_rcu_qsbr.h>

#include "pppd.h"
#include "fsm.h"
#include "../dp.h"
#include "../dbg.h"
#include "../init.h"
#include "../dp_flow.h"
#include "../dhcpd/dhcpd.h"
#include "../fastrg.h"
#include "../utils.h"
#include "../etcd_integration.h"
#include "../northbound.h"

U32	            ppp_interval;

void PPP_bye_timer_cb(__attribute__((unused)) struct rte_timer *tim, 
    ppp_ccb_t *ppp_ccb)
{
    PPP_bye(ppp_ccb);
}

void PPP_bye(ppp_ccb_t *s_ppp_ccb)
{
    rte_timer_stop(&(s_ppp_ccb->ppp));
    rte_timer_stop(&(s_ppp_ccb->pppoe));
    rte_timer_stop(&(s_ppp_ccb->ppp_alive));
    rte_timer_stop(&s_ppp_ccb->nat);
    rte_atomic16_cmpset((volatile uint16_t *)&s_ppp_ccb->dp_start_bool.cnt, (BIT16)1, (BIT16)0);
    switch(s_ppp_ccb->phase) {
        case END_PHASE:
            rte_atomic16_set(&s_ppp_ccb->ppp_bool, 0);
            s_ppp_ccb->ppp_processing = FALSE;
            exit_ppp(s_ppp_ccb);
            break;
        case PPPOE_PHASE:
            s_ppp_ccb->phase--;
            s_ppp_ccb->ppp_phase[0].state = S_INIT;
            s_ppp_ccb->ppp_phase[1].state = S_INIT;
            PPP_bye(s_ppp_ccb);
            break;
        case LCP_PHASE:
            s_ppp_ccb->ppp_processing = TRUE;
            s_ppp_ccb->cp = 0;
            s_ppp_ccb->ppp_phase[1].state = S_INIT;
            PPP_FSM(&(s_ppp_ccb->ppp), s_ppp_ccb, E_CLOSE);
            break;
        case DATA_PHASE:
            /* modify pppoe phase from DATA_PHASE to IPCP_PHASE */
            s_ppp_ccb->phase--;
        case IPCP_PHASE:
            s_ppp_ccb->ppp_processing = TRUE;
            /* set ppp control protocol to IPCP */
            s_ppp_ccb->cp = 1;
            PPP_FSM(&(s_ppp_ccb->ppp), s_ppp_ccb, E_CLOSE);
            break;
        default:
            rte_atomic16_set(&s_ppp_ccb->ppp_bool, 0);
            s_ppp_ccb->ppp_processing = FALSE;
            exit_ppp(s_ppp_ccb);
    }
}

STATUS ppp_update_config_by_user(ppp_ccb_t *ppp_ccb, U16 vlan_id, const char *user_name, const char *password)
{
    rte_atomic16_set(&ppp_ccb->vlan_id, vlan_id);

    /* We don't need to lock here because in dp, we don't need this field */
    if (ppp_ccb->ppp_user_acc != NULL)
        fastrg_mfree(ppp_ccb->ppp_user_acc);
    ppp_ccb->ppp_user_acc = fastrg_calloc(U8, 1, strlen(user_name) + 1, 0);
    if (ppp_ccb->ppp_user_acc == NULL) {
        FastRG_LOG(ERR, ppp_ccb->fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "account buffer update failed: %s", rte_strerror(errno));
        return ERROR;
    }
    strcpy((char *)ppp_ccb->ppp_user_acc, user_name);
    if (ppp_ccb->ppp_passwd != NULL)
        fastrg_mfree(ppp_ccb->ppp_passwd);
    ppp_ccb->ppp_passwd = fastrg_calloc(U8, 1, strlen(password) + 1, 0);
    if (ppp_ccb->ppp_passwd == NULL) {
        FastRG_LOG(ERR, ppp_ccb->fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "password buffer update failed: %s", rte_strerror(errno));
        return ERROR;
    }
    strcpy((char *)ppp_ccb->ppp_passwd, password);

    return SUCCESS;
}

STATUS ppp_init_config_by_user(FastRG_t *fastrg_ccb, ppp_ccb_t *ppp_ccb, U16 ccb_id, U16 vlan_id, 
    const char *user_name, const char *password)
{
    ppp_ccb->fastrg_ccb = fastrg_ccb;
    ppp_ccb->ppp_phase[0].state = S_INIT;
    ppp_ccb->ppp_phase[1].state = S_INIT;
    ppp_ccb->pppoe_phase.active = FALSE;

    ppp_ccb->user_num = ccb_id + 1;
    rte_atomic16_set(&ppp_ccb->vlan_id, vlan_id);

    ppp_ccb->hsi_ipv4 = 0x0;
    ppp_ccb->hsi_ipv4_gw = 0x0;
    ppp_ccb->hsi_primary_dns = 0x0;
    ppp_ccb->hsi_secondary_dns = 0x0;
    // vlan_id of each subscriptor is 0 to indicate unconfigured
    ppp_ccb->phase = vlan_id != 0 ? END_PHASE : NOT_CONFIGURED;
    ppp_ccb->is_pap_auth = FALSE;
    ppp_ccb->auth_method = PAP_PROTOCOL;
    ppp_ccb->magic_num = rte_cpu_to_be_32((rand() % 0xFFFFFFFE) + 1);
    ppp_ccb->identifier = 0x0;
    for(int j=0; j<TOTAL_SOCK_PORT; j++) {
        rte_atomic16_init(&ppp_ccb->addr_table[j].is_alive);
        rte_atomic16_init(&ppp_ccb->addr_table[j].is_fill);
    }
    memset(ppp_ccb->PPP_dst_mac.addr_bytes, 0, ETH_ALEN);
    rte_timer_init(&(ppp_ccb->pppoe));
    rte_timer_init(&(ppp_ccb->ppp));
    rte_timer_init(&(ppp_ccb->nat));
    rte_timer_init(&(ppp_ccb->ppp_alive));
    rte_timer_init(&(ppp_ccb->etcd_pppoe_status_timer));
    rte_atomic16_init(&ppp_ccb->dp_start_bool);
    rte_atomic16_init(&ppp_ccb->ppp_bool);
    rte_atomic64_init(&ppp_ccb->pppoes_rx_bytes);
    rte_atomic64_init(&ppp_ccb->pppoes_tx_bytes);
    rte_atomic64_init(&ppp_ccb->pppoes_rx_packets);
    rte_atomic64_init(&ppp_ccb->pppoes_tx_packets);

    /* MAC table: allocate once, reset on re-init */
    if (ppp_ccb->mac_table == NULL) {
        ppp_ccb->mac_table = mac_table_alloc();
        if (ppp_ccb->mac_table == NULL) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
                "mac_table allocation failed");
            goto err;
        }
    } else {
        memset(ppp_ccb->mac_table, 0,
            (size_t)MAC_TABLE_SIZE * sizeof(mac_table_entry_t));
    }

    /* ARP pending ring: create once, flush on re-init */
    if (ppp_ccb->arp_pq.ring == NULL) {
        if (arp_pending_init_queue(&ppp_ccb->arp_pq, ccb_id) != SUCCESS) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
                "arp_pending ring creation failed for ccb %u", ccb_id);
            goto err;
        }
    } else {
        arp_pending_flush(fastrg_ccb->arp_pending_mp, &ppp_ccb->arp_pq);
    }

    if (ppp_ccb->ppp_user_acc != NULL)
        fastrg_mfree(ppp_ccb->ppp_user_acc);
    ppp_ccb->ppp_user_acc = fastrg_calloc(U8, 1, strlen(user_name) + 1, 0);
    if (ppp_ccb->ppp_user_acc == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "account buffer allocation failed: %s", rte_strerror(errno));
        goto err;
    }
    strcpy((char *)ppp_ccb->ppp_user_acc, user_name);

    if (ppp_ccb->ppp_passwd != NULL)
        fastrg_mfree(ppp_ccb->ppp_passwd);
    ppp_ccb->ppp_passwd = fastrg_calloc(U8, 1, strlen(password) + 1, 0);
    if (ppp_ccb->ppp_passwd == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "password buffer allocation failed: %s", 
            rte_strerror(errno));
        goto err;
    }
    strcpy((char *)ppp_ccb->ppp_passwd, password);

    if (ppp_ccb->pppoe_phase.pppoe_header_tag == NULL)
        ppp_ccb->pppoe_phase.pppoe_header_tag = fastrg_malloc(pppoe_header_tag_t, 
            PPPoE_TAG_DEFAULT_MAX_LEN, RTE_CACHE_LINE_SIZE);
    if (ppp_ccb->pppoe_phase.pppoe_header_tag == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "pppoe header tag allocation failed: %s", 
            rte_strerror(errno));
        goto err;
    }

    return SUCCESS;

err:
    ppp_ccb->phase = NOT_CONFIGURED;
    rte_atomic16_set(&ppp_ccb->vlan_id, 0);
    mac_table_free(ppp_ccb->mac_table);
    arp_pending_cleanup_queue(&ppp_ccb->arp_pq, fastrg_ccb->arp_pending_mp);
    ppp_ccb->mac_table = NULL;
    if (ppp_ccb->ppp_user_acc != NULL) {
        fastrg_mfree(ppp_ccb->ppp_user_acc);
        ppp_ccb->ppp_user_acc = NULL;
    }
    if (ppp_ccb->ppp_passwd != NULL) {
        fastrg_mfree(ppp_ccb->ppp_passwd);
        ppp_ccb->ppp_passwd = NULL;
    }
    if (ppp_ccb->pppoe_phase.pppoe_header_tag != NULL) {
        fastrg_mfree(ppp_ccb->pppoe_phase.pppoe_header_tag);
        ppp_ccb->pppoe_phase.pppoe_header_tag = NULL;
    }
    return ERROR;
}

STATUS pppd_allocate_ccbs(FastRG_t *fastrg_ccb, U16 start_id, U16 count, ppp_ccb_t **array)
{
    for(U16 i=0; i<count; i++) {
        U16 ccb_id = start_id + i;

        if (rte_mempool_get(fastrg_ccb->ppp_ccb_mp, 
                (void **)&array[ccb_id]) < 0) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
                "rte_mempool_get for ppp_ccb[%u] failed: %s (available: %u)", 
                ccb_id, rte_strerror(rte_errno),
                rte_mempool_avail_count(fastrg_ccb->ppp_ccb_mp));

            for(U16 j=start_id; j<ccb_id; j++) {
                rte_mempool_put(fastrg_ccb->ppp_ccb_mp, array[j]);
                array[j] = NULL;
            }
            return ERROR;
        }

        memset(array[ccb_id], 0, sizeof(ppp_ccb_t));

        /* subscriptor id starts from 1 */
        /* vlan of each subscriptor is 0 to indicate unused */
        if (ppp_init_config_by_user(fastrg_ccb, array[ccb_id], ccb_id, 0, 
                "asdf", "zxcv") == ERROR) {
            for(U16 j=start_id; j<=ccb_id; j++) {
                rte_mempool_put(fastrg_ccb->ppp_ccb_mp, array[j]);
                array[j] = NULL;
            }
            return ERROR;
        }
    }

    return SUCCESS;
}

STATUS pppd_init_rcu(FastRG_t *fastrg_ccb)
{
    size_t sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    fastrg_ccb->ppp_ccb_rcu = fastrg_calloc(struct rte_rcu_qsbr, 1, sz, RTE_CACHE_LINE_SIZE);
    if (fastrg_ccb->ppp_ccb_rcu == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, "rte_zmalloc for RCU failed");
        return ERROR;
    }

    if (rte_rcu_qsbr_init(fastrg_ccb->ppp_ccb_rcu, RTE_MAX_LCORE) != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, "rte_rcu_qsbr_init failed");
        fastrg_mfree(fastrg_ccb->ppp_ccb_rcu);
        return ERROR;
    }

    unsigned int lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        rte_rcu_qsbr_thread_register(fastrg_ccb->ppp_ccb_rcu, lcore_id);
    }

    rte_atomic16_init(&fastrg_ccb->ppp_ccb_updating);

    return SUCCESS;
}

STATUS pppd_add_ccb(FastRG_t *fastrg_ccb, U16 extra_ccb_count)
{
    if (extra_ccb_count == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "extra_ccb_count is 0, nothing to do");
        return SUCCESS;
    }

    if (rte_mempool_in_use_count(fastrg_ccb->ppp_ccb_mp) > fastrg_ccb->user_count) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, PPPLOGMSG, "we have unused ccb in mempool, no need to add more");
        return SUCCESS;
    }

    if (!rte_atomic16_cmpset((volatile uint16_t *)&fastrg_ccb->ppp_ccb_updating.cnt, 0, 1)) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "Another resize operation is in progress");
        return ERROR;
    }

    ppp_ccb_t **old_array = (ppp_ccb_t **)fastrg_ccb->ppp_ccb;
    U16 old_user_count = fastrg_ccb->user_count;
    U16 new_user_count = old_user_count + extra_ccb_count;

    ppp_ccb_t **new_array = fastrg_malloc(ppp_ccb_t *,  
        sizeof(ppp_ccb_t *) * new_user_count, 0);
    if (new_array == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "realloc ppp_ccb array failed");
        rte_atomic16_clear(&fastrg_ccb->ppp_ccb_updating);
        return ERROR;
    }

    if (old_array != NULL)
        memcpy(new_array, old_array, sizeof(ppp_ccb_t *) * old_user_count);

    memset(&new_array[old_user_count], 0, sizeof(ppp_ccb_t *) * extra_ccb_count);

    if (pppd_allocate_ccbs(fastrg_ccb, old_user_count, extra_ccb_count, new_array) == ERROR) {
        fastrg_mfree(new_array);
        rte_atomic16_clear(&fastrg_ccb->ppp_ccb_updating);
        return ERROR;
    }

    rte_wmb();

    __atomic_store_n(&fastrg_ccb->ppp_ccb, new_array, __ATOMIC_RELEASE);

    if (old_array != NULL) {
        rte_rcu_qsbr_synchronize(fastrg_ccb->ppp_ccb_rcu, RTE_QSBR_THRID_INVALID);
        fastrg_mfree(old_array);
    }

    rte_atomic16_clear(&fastrg_ccb->ppp_ccb_updating);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, PPPLOGMSG, 
        "%u PPP CCB added, mempool available: %u", 
        extra_ccb_count, rte_mempool_avail_count(fastrg_ccb->ppp_ccb_mp));

    return SUCCESS;
}

STATUS pppd_disable_ccb(FastRG_t *fastrg_ccb, U16 remove_ccb_count, U16 old_ccb_count)
{
    if (remove_ccb_count > old_ccb_count) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "Invalid disabling ccb count %u", remove_ccb_count);
        return ERROR;
    }

    if (remove_ccb_count == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "remove_ccb_count is 0, nothing to do");
        return SUCCESS;
    }

    ppp_ccb_t **old_array = (ppp_ccb_t **)fastrg_ccb->ppp_ccb;

    for(U16 i=0; i<remove_ccb_count; i++) {
        U16 ccb_id = old_ccb_count - 1 - i;
        ppp_ccb_t *ppp_ccb = old_array[ccb_id];
        exit_ppp(ppp_ccb);
        rte_timer_stop(&ppp_ccb->etcd_pppoe_status_timer);
        reset_vlan_map_ccb_id(fastrg_ccb, rte_atomic16_read(&ppp_ccb->vlan_id));
        ppp_cleanup_config_by_user(ppp_ccb, ccb_id);
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, PPPLOGMSG, 
        "%u CCBs disabled", remove_ccb_count);

    return SUCCESS;
}

STATUS pppd_remove_ccb(FastRG_t *fastrg_ccb, U16 remove_ccb_count, U16 old_ccb_count)
{
    if (remove_ccb_count > old_ccb_count) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "Invalid removing ccb count %u", remove_ccb_count);
        return ERROR;
    }

    if (remove_ccb_count == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "remove_ccb_count is 0, nothing to do");
        return SUCCESS;
    }

    if (!rte_atomic16_cmpset((volatile uint16_t *)&fastrg_ccb->ppp_ccb_updating.cnt, 0, 1)) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "Another resize operation is in progress");
        return ERROR;
    }

    ppp_ccb_t **old_array = (ppp_ccb_t **)fastrg_ccb->ppp_ccb;
    U16 new_user_count = old_ccb_count - remove_ccb_count;

    for(U16 i=0; i<remove_ccb_count; i++) {
        U16 ccb_id = old_ccb_count - 1 - i;
        ppp_ccb_t *ppp_ccb = old_array[ccb_id];
        exit_ppp(ppp_ccb);
        rte_timer_stop(&ppp_ccb->etcd_pppoe_status_timer);
        if (ppp_ccb->ppp_user_acc != NULL)
            fastrg_mfree(ppp_ccb->ppp_user_acc);
        if (ppp_ccb->ppp_passwd != NULL)
            fastrg_mfree(ppp_ccb->ppp_passwd);
        if (ppp_ccb->pppoe_phase.pppoe_header_tag != NULL)
            fastrg_mfree(ppp_ccb->pppoe_phase.pppoe_header_tag);
        arp_pending_cleanup_queue(&ppp_ccb->arp_pq, fastrg_ccb->arp_pending_mp);
        if (ppp_ccb->mac_table != NULL) {
            mac_table_free(ppp_ccb->mac_table);
            ppp_ccb->mac_table = NULL;
        }
        rte_mempool_put(fastrg_ccb->ppp_ccb_mp, old_array[ccb_id]);
        old_array[ccb_id] = NULL;
    }

    if (new_user_count == 0) {
        __atomic_store_n(&fastrg_ccb->ppp_ccb, (ppp_ccb_t **)NULL, __ATOMIC_RELEASE);

        rte_rcu_qsbr_synchronize(fastrg_ccb->ppp_ccb_rcu, RTE_QSBR_THRID_INVALID);
        fastrg_mfree(old_array);
    } else {
        ppp_ccb_t **new_array = fastrg_malloc(ppp_ccb_t *, new_user_count, 0);
        if (new_array == NULL) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
                "malloc new smaller ppp_ccb array failed");
            rte_atomic16_clear(&fastrg_ccb->ppp_ccb_updating);
            return ERROR;
        }

        rte_memcpy(new_array, old_array, sizeof(ppp_ccb_t *) * new_user_count);

        rte_wmb();

        __atomic_store_n(&fastrg_ccb->ppp_ccb, new_array, __ATOMIC_RELEASE);

        rte_rcu_qsbr_synchronize(fastrg_ccb->ppp_ccb_rcu, RTE_QSBR_THRID_INVALID);

        fastrg_mfree(old_array);
    }

    rte_atomic16_clear(&fastrg_ccb->ppp_ccb_updating);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, PPPLOGMSG, 
        "%u CCBs removed, mempool available: %u", 
        remove_ccb_count, rte_mempool_avail_count(fastrg_ccb->ppp_ccb_mp));

    return SUCCESS;
}

void pppd_cleanup_ccb(FastRG_t *fastrg_ccb, U16 total_ccb_count)
{
    if (fastrg_ccb == NULL)
        return;

    if (fastrg_ccb->ppp_ccb != NULL && total_ccb_count > 0)
        pppd_remove_ccb(fastrg_ccb, total_ccb_count, total_ccb_count);

    if (fastrg_ccb->ppp_ccb_mp != NULL) {
        rte_mempool_free(fastrg_ccb->ppp_ccb_mp);
        fastrg_ccb->ppp_ccb_mp = NULL;
    }

    if (fastrg_ccb->ppp_ccb_rcu != NULL) {
        fastrg_mfree(fastrg_ccb->ppp_ccb_rcu);
        fastrg_ccb->ppp_ccb_rcu = NULL;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, PPPLOGMSG, 
        "pppd cleanup completed");
}

STATUS pppd_init(FastRG_t *fastrg_ccb)
{
    // calculate mempool size as the next power of 2 greater than user_count
    unsigned int mempool_size = 1U << (31 - __builtin_clz(fastrg_ccb->user_count) + 1);

    if (pppd_init_rcu(fastrg_ccb) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "pppd_init_rcu failed");
        return ERROR;
    }

    fastrg_ccb->ppp_ccb_mp = rte_mempool_create(
        "ppp_ccb_pool",                      /* name */
        mempool_size,                        /* user count */
        sizeof(ppp_ccb_t),                   /* ppp_ccb size */
        mempool_size * 2 / 3,                /* per-lcore cache size */
        0,                                   /* private_data_size */
        NULL, NULL,                          /* mp_init, mp_init_arg */
        NULL, NULL,                          /* obj_init, obj_init_arg */
        rte_socket_id(),                     /* socket_id */
        0                                    /* flags */
    );
    if (fastrg_ccb->ppp_ccb_mp == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "rte_mempool_create failed: %s", rte_strerror(rte_errno));
        fastrg_mfree(fastrg_ccb->ppp_ccb_rcu);
        return ERROR;
    }

    srand(time(NULL));
    ppp_interval = (uint32_t)(3 * SECOND); 

    U16 initial_user_count = fastrg_ccb->user_count;
    /* assume we want to add ccbs from 0 to initial_user_count */
    fastrg_ccb->user_count = 0;
    fastrg_ccb->ppp_ccb = NULL;
    if (pppd_add_ccb(fastrg_ccb, initial_user_count) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "pppd_add_ccb for initial %u CCBs failed", initial_user_count);
        rte_mempool_free(fastrg_ccb->ppp_ccb_mp);
        fastrg_mfree(fastrg_ccb->ppp_ccb_rcu);
        return ERROR;
    }

    fastrg_ccb->user_count = initial_user_count;

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, PPPLOGMSG, 
        "============ pppoe init successfully ==============\n");
    return SUCCESS;
}

void ppp_cleanup_config_by_user(ppp_ccb_t *ppp_ccb, U16 ccb_id)
{
    FastRG_t *fastrg_ccb = ppp_ccb->fastrg_ccb;

    ppp_init_config_by_user(fastrg_ccb, ppp_ccb, ccb_id, 0, "asdf", "zxcv");
}

STATUS ppp_connect(ppp_ccb_t *ppp_ccb)
{
    FastRG_t *fastrg_ccb = ppp_ccb->fastrg_ccb;

    if (ppp_ccb->phase > END_PHASE) {
        FastRG_LOG(ERR, fastrg_ccb->fp, ppp_ccb, PPPLOGMSG, 
            "Error! User %u is in a pppoe connection", ppp_ccb->user_num);
        return ERROR;
    }
    ppp_ccb->phase = PPPOE_PHASE;
    ppp_ccb->pppoe_phase.max_retransmit = MAX_RETRAN;
    ppp_ccb->pppoe_phase.timer_counter = 0;
    if (pppoe_send_pkt(ENCODE_PADI, ppp_ccb) == ERROR)
        PPP_bye(ppp_ccb);
    /* set ppp starting boolean flag to TRUE */
    rte_atomic16_set(&ppp_ccb->ppp_bool, 1);
    rte_timer_reset(&ppp_ccb->pppoe, rte_get_timer_hz(), PERIODICAL, 
        fastrg_ccb->lcore.ctrl_thread, (rte_timer_cb_t)A_padi_timer_func, ppp_ccb);

    return SUCCESS;
}

STATUS ppp_disconnect(ppp_ccb_t *ppp_ccb)
{
    FastRG_t *fastrg_ccb = ppp_ccb->fastrg_ccb;
    if (ppp_ccb->phase == END_PHASE) {
        FastRG_LOG(ERR, fastrg_ccb->fp, ppp_ccb, PPPLOGMSG, "Error! User %u is in init phase", ppp_ccb->user_num);
        return ERROR;
    }
    if (ppp_ccb->ppp_processing == TRUE) {
        FastRG_LOG(ERR, fastrg_ccb->fp, ppp_ccb, PPPLOGMSG, 
            "Error! User %u is disconnecting pppoe connection, please wait...", ppp_ccb->user_num);
        return ERROR;
    }
    PPP_bye(ppp_ccb);

    return SUCCESS;
}

void check_etcd_pppoe_status(struct rte_timer *tim, ppp_ccb_t *ppp_ccb)
{
    FastRG_t *fastrg_ccb = ppp_ccb->fastrg_ccb;
    char *node_id = fastrg_ccb->node_uuid;
    char user_id_str[8] = { 0 };
    hsi_enable_status_t hsi_enable_status;
    int64_t revision = 0;

    snprintf(user_id_str, sizeof(user_id_str), "%u", ppp_ccb->user_num);
    etcd_status_t status = etcd_client_get_hsi_config_status(node_id, user_id_str, &hsi_enable_status);
    if (status != ETCD_SUCCESS && status != ETCD_KEY_NOT_FOUND) {
        if (tim->expire >= (ETCD_RETRY_BASE_TIME << 5)) { // try for 5 times
            FastRG_LOG(ERR, fastrg_ccb->fp, ppp_ccb, PPPLOGMSG, 
                "User %" PRIu16 " failed to get HSI config status from etcd after multiple attempts.\n", 
                ppp_ccb->user_num);
            return;
        }
        tim->expire <<= 1;
        rte_timer_reset(tim, tim->expire, SINGLE, fastrg_ccb->lcore.ctrl_thread, 
            (rte_timer_cb_t)check_etcd_pppoe_status, ppp_ccb);
        return;
    }
    if (rte_atomic16_read(&ppp_ccb->ppp_bool) == 0 && hsi_enable_status != ENABLE_STATUS_DISABLED) {
        etcd_mark_pending_event(HSI_ACTION_UPDATE, ppp_ccb->user_num - 1);
        if (etcd_client_modify_hsi_config_status(fastrg_ccb->node_uuid, user_id_str, 
                ENABLE_STATUS_DISABLED, &revision) == ETCD_SUCCESS) {
            etcd_confirm_pending_event(HSI_ACTION_UPDATE, ppp_ccb->user_num - 1, revision);
        } else {
            etcd_remove_event(HSI_ACTION_UPDATE, ppp_ccb->user_num - 1);
        }
    } else if (rte_atomic16_read(&ppp_ccb->ppp_bool) == 1 && 
            hsi_enable_status != ENABLE_STATUS_ENABLED) {
        etcd_mark_pending_event(HSI_ACTION_UPDATE, ppp_ccb->user_num - 1);
        if (etcd_client_modify_hsi_config_status(fastrg_ccb->node_uuid, user_id_str, 
                ENABLE_STATUS_ENABLED, &revision) == ETCD_SUCCESS) {
            etcd_confirm_pending_event(HSI_ACTION_UPDATE, ppp_ccb->user_num - 1, revision);
        } else {
            etcd_remove_event(HSI_ACTION_UPDATE, ppp_ccb->user_num - 1);
        }
    }
}

void exit_ppp(ppp_ccb_t *ppp_ccb)
{
    FastRG_t *fastrg_ccb = ppp_ccb->fastrg_ccb;

    rte_atomic16_cmpset((U16 *)&(ppp_ccb->ppp_bool.cnt), 1, 0);
    rte_timer_stop(&(ppp_ccb->ppp));
    rte_timer_stop(&(ppp_ccb->pppoe));
    rte_timer_stop(&(ppp_ccb->ppp_alive));
    rte_timer_stop(&ppp_ccb->nat);
    fastrg_ccb->cur_user--;
    ppp_ccb->phase = END_PHASE;
    ppp_ccb->ppp_phase[0].state = S_INIT;
    ppp_ccb->ppp_phase[1].state = S_INIT;
    ppp_ccb->pppoe_phase.active = FALSE;
    FastRG_LOG(INFO, fastrg_ccb->fp, ppp_ccb, PPPLOGMSG, "User %" PRIu16 
        " HSI module is terminated.\n", ppp_ccb->user_num);

    if (fastrg_ccb->is_standalone == FALSE) {
        char user_id_str[6];
        snprintf(user_id_str, sizeof(user_id_str), "%u", ppp_ccb->user_num);
        int64_t revision = 0;
        etcd_mark_pending_event(HSI_ACTION_UPDATE, ppp_ccb->user_num - 1);
        if (etcd_client_modify_hsi_config_status(fastrg_ccb->node_uuid, user_id_str, 
                ENABLE_STATUS_DISABLED, &revision) == ETCD_SUCCESS) {
            etcd_confirm_pending_event(HSI_ACTION_UPDATE, ppp_ccb->user_num - 1, revision);
        } else {
            etcd_remove_event(HSI_ACTION_UPDATE, ppp_ccb->user_num - 1);
        }
        rte_timer_reset(&ppp_ccb->etcd_pppoe_status_timer, ETCD_RETRY_BASE_TIME, 
            SINGLE, fastrg_ccb->lcore.ctrl_thread, (rte_timer_cb_t)check_etcd_pppoe_status, ppp_ccb);
    }
}

STATUS ppp_process(FastRG_t *fastrg_ccb, void *mail)
{
    tFastRG_MBX	*pppoe_mail = (tFastRG_MBX *)mail;
    int         ret;
    U16	        event, ccb_id = 0;

    ret = get_ccb_id(fastrg_ccb, pppoe_mail->refp, &ccb_id);
    if (ret == ERROR)
        return ERROR;

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    if (ppp_ccb == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "Invalid CCB ID %u in PPP processing", ccb_id);
        return ERROR;
    }

    ret = PPP_decode_frame(pppoe_mail->refp, pppoe_mail->len, &event, ppp_ccb);
    if (ret == ERROR)					
        return ERROR;

    if (check_auth_result(ppp_ccb) == 1)
        return ERROR;

    ppp_ccb->ppp_phase[ppp_ccb->cp].event = event;
    PPP_FSM(&(ppp_ccb->ppp), ppp_ccb, event);
    codec_cleanup_ppp_ccb(ppp_ccb);

    return SUCCESS;
}
