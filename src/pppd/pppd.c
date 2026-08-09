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
#include <rte_trace.h>
#include <rte_rcu_qsbr.h>

#include "pppd.h"
#include "nat.h"
#include "fsm.h"
#include "codec.h"
#include "../dp.h"
#include "../dbg.h"
#include "../init.h"
#include "../dp_flow.h"
#include "../dhcpd/dhcpd.h"
#include "../fastrg.h"
#include "../utils.h"
#include "../etcd_integration.h"
#include "../northbound.h"
#include "kafka_producer.h"

void PPP_bye_timer_cb(__attribute__((unused)) struct rte_timer *tim,
    ppp_ccb_t *ppp_ccb)
{
    PPP_bye(ppp_ccb);
}

void PPP_keepalive_cb(__attribute__((unused)) struct rte_timer *tim,
    ppp_ccb_t *s_ppp_ccb)
{
    FastRG_t      *fastrg_ccb = s_ppp_ccb->fastrg_ccb;
    unsigned char  buffer[PPP_MSG_BUF_LEN];
    U16            mulen = 0;

    /* No reply to the last LCP_ECHO_MAX_FAIL Echo-Requests (and no other frame
     * from the peer in between) → treat the peer as dead and tear down. */
    if (s_ppp_ccb->echo_miss_count >= LCP_ECHO_MAX_FAIL) {
        FastRG_LOG(WARN, fastrg_ccb->fp, s_ppp_ccb, PPPLOGMSG,
            "User %" PRIu16 " LCP keepalive: peer unresponsive for %u echo-requests, tearing down session.",
            s_ppp_ccb->user_num, s_ppp_ccb->echo_miss_count);
        PPP_bye(s_ppp_ccb);
        return;
    }

    /* Probe: send our own Echo-Request and count it as outstanding. Any frame
     * received from the peer (handled in decode_lcp/decode_ipcp) resets the
     * counter back to 0. */
    memset(buffer, 0, PPP_MSG_BUF_LEN);
    build_echo_request(buffer, &mulen, s_ppp_ccb);
    wan_ctrl_tx(fastrg_ccb, s_ppp_ccb->user_num - 1, buffer, mulen);
    s_ppp_ccb->echo_miss_count++;
}

void PPP_bye(ppp_ccb_t *s_ppp_ccb)
{
    rte_timer_stop(&(s_ppp_ccb->ppp));
    rte_timer_stop(&(s_ppp_ccb->ppp_ipv6cp));
    rte_timer_stop(&(s_ppp_ccb->pppoe));
    rte_timer_stop(&(s_ppp_ccb->ppp_alive));
    s_ppp_ccb->ipv6cp_up = FALSE;
    s_ppp_ccb->config_request_pending[PPP_CP_IPV6CP] = FALSE;
    rte_atomic16_cmpset((volatile uint16_t *)&s_ppp_ccb->dp_start_bool.cnt, (S16)1, (S16)0);
    switch(s_ppp_ccb->phase) {
        case END_PHASE:
            rte_atomic16_set(&s_ppp_ccb->ppp_bool, 0);
            s_ppp_ccb->ppp_processing = FALSE;
            exit_ppp(s_ppp_ccb);
            break;
        case PPPOE_PHASE:
            s_ppp_ccb->phase--;
            s_ppp_ccb->control_protocol[PPP_CP_LCP].state = S_INIT;
            s_ppp_ccb->control_protocol[PPP_CP_IPCP].state = S_INIT;
            s_ppp_ccb->control_protocol[PPP_CP_IPV6CP].state = S_INIT;
            PPP_bye(s_ppp_ccb);
            break;
        case LCP_PHASE:
            s_ppp_ccb->ppp_processing = TRUE;
            s_ppp_ccb->cp_id = PPP_CP_LCP;
            s_ppp_ccb->control_protocol[PPP_CP_IPCP].state = S_INIT;
            s_ppp_ccb->control_protocol[PPP_CP_IPV6CP].state = S_INIT;
            PPP_FSM(&(s_ppp_ccb->ppp), s_ppp_ccb, E_CLOSE);
            break;
        case DATA_PHASE:
        case IPCP_PHASE:
            s_ppp_ccb->ppp_processing = TRUE;
            /* RFC 1661 §3.7: LCP close is sufficient — skip IPCP terminate. */
            rte_atomic16_set(&s_ppp_ccb->dp_start_bool, (S16)0);
            s_ppp_ccb->control_protocol[PPP_CP_IPCP].state = S_INIT;
            s_ppp_ccb->control_protocol[PPP_CP_IPV6CP].state = S_INIT;
            s_ppp_ccb->phase = LCP_PHASE;
            s_ppp_ccb->cp_id = PPP_CP_LCP;
            PPP_FSM(&(s_ppp_ccb->ppp), s_ppp_ccb, E_CLOSE);
            break;
        default:
            rte_atomic16_set(&s_ppp_ccb->ppp_bool, 0);
            s_ppp_ccb->ppp_processing = FALSE;
            exit_ppp(s_ppp_ccb);
    }
}

/**
 * @fn ppp_copy_credentials
 *
 * @brief Replace the ccb's account/password buffers (unbounded length).
 *        The new buffers are allocated outside the lock; the pointer swap
 *        and the old-buffer free happen under cred_lock, so a concurrent
 *        reader (gRPC GetFastrgHsiInfo, codec PAP/CHAP builders, etcd
 *        sync-match — all of which take cred_lock) can never chase a freed
 *        pointer or observe a torn string.
 *
 * @param ppp_ccb
 *      PPP control block pointer
 * @param user_name
 *      Account name (NUL-terminated)
 * @param password
 *      Password (NUL-terminated)
 * @return
 *      SUCCESS if replaced, ERROR on allocation failure (old credentials
 *      remain intact)
 */
static STATUS ppp_copy_credentials(ppp_ccb_t *ppp_ccb, const char *user_name,
    const char *password)
{
    U8 *new_acc = fastrg_calloc(U8, 1, strlen(user_name) + 1, 0);
    U8 *new_pwd = fastrg_calloc(U8, 1, strlen(password) + 1, 0);

    if (new_acc == NULL || new_pwd == NULL) {
        FastRG_LOG(ERR, ppp_ccb->fastrg_ccb->fp, NULL, PPPLOGMSG,
            "credential buffer allocation failed: %s", rte_strerror(errno));
        if (new_acc != NULL)
            fastrg_mfree(new_acc);
        if (new_pwd != NULL)
            fastrg_mfree(new_pwd);
        return ERROR;
    }
    strcpy((char *)new_acc, user_name);
    strcpy((char *)new_pwd, password);

    rte_spinlock_lock(&ppp_ccb->cred_lock);
    U8 *old_acc = ppp_ccb->ppp_user_acc;
    U8 *old_pwd = ppp_ccb->ppp_passwd;
    ppp_ccb->ppp_user_acc = new_acc;
    ppp_ccb->ppp_passwd = new_pwd;
    rte_spinlock_unlock(&ppp_ccb->cred_lock);

    /* Safe outside the lock: after the swap above no reader can still hold
     * the old pointers (every reader fetches them under cred_lock and
     * finishes using them before unlocking). */
    if (old_acc != NULL)
        fastrg_mfree(old_acc);
    if (old_pwd != NULL)
        fastrg_mfree(old_pwd);

    return SUCCESS;
}

STATUS ppp_update_config_by_user(ppp_ccb_t *ppp_ccb, U16 vlan_id, const char *user_name, const char *password)
{
    if (ppp_copy_credentials(ppp_ccb, user_name, password) == ERROR)
        return ERROR;
    rte_atomic16_set(&ppp_ccb->vlan_id, vlan_id);
    return SUCCESS;
}

/**
 * @fn pppd_destroy_ccb_elements
 *
 * @brief Free a subscriber slot's elements. Only used on the init failure
 *        rollback path and at shutdown (pppd_cleanup_ccb). Idempotent.
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param ppp_ccb
 *      PPP control block pointer
 */
static void pppd_destroy_ccb_elements(FastRG_t *fastrg_ccb, ppp_ccb_t *ppp_ccb)
{
    mac_table_free(ppp_ccb->mac_table);
    ppp_ccb->mac_table = NULL;
    arp_pending_cleanup_queue(&ppp_ccb->arp_pq, fastrg_ccb->arp_pending_mp);
    nat_table_destroy(ppp_ccb);
    if (ppp_ccb->pppoe_phase.pppoe_header_tag != NULL) {
        fastrg_mfree(ppp_ccb->pppoe_phase.pppoe_header_tag);
        ppp_ccb->pppoe_phase.pppoe_header_tag = NULL;
    }
    if (ppp_ccb->ppp_user_acc != NULL) {
        fastrg_mfree(ppp_ccb->ppp_user_acc);
        ppp_ccb->ppp_user_acc = NULL;
    }
    if (ppp_ccb->ppp_passwd != NULL) {
        fastrg_mfree(ppp_ccb->ppp_passwd);
        ppp_ccb->ppp_passwd = NULL;
    }
}

/**
 * @fn pppd_construct_ccb_elements
 *
 * @brief Construction of a subscriber slot's runtime-immutable
 *        elements: MAC table hash, ARP pending ring, NAT hashes + free
 *        ring, and the PPPoE header-tag buffer. Called only from
 *        pppd_allocate_ccbs() at init — after this, runtime configuration
 *        changes reuse/reset these objects and never allocate or free.
 *
 * @param fastrg_ccb
 *      FastRG control block pointer
 * @param ppp_ccb
 *      PPP control block pointer (zeroed by the caller)
 * @param ccb_id
 *      Subscriber index (used for unique DPDK object naming)
 * @return
 *      SUCCESS if all elements were created, ERROR otherwise
 */
static STATUS pppd_construct_ccb_elements(FastRG_t *fastrg_ccb, ppp_ccb_t *ppp_ccb, U16 ccb_id)
{
    ppp_ccb->mac_table = mac_table_alloc(ccb_id);
    if (ppp_ccb->mac_table == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
            "mac_table allocation failed for ccb %u", ccb_id);
        goto err;
    }

    if (arp_pending_init_queue(&ppp_ccb->arp_pq, ccb_id) != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
            "arp_pending ring creation failed for ccb %u", ccb_id);
        goto err;
    }

    if (nat_table_init(ppp_ccb, ccb_id, fastrg_ccb->ppp_ccb_rcu) != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
            "NAT rev hash/ring creation failed for ccb %u", ccb_id);
        goto err;
    }

    ppp_ccb->pppoe_phase.pppoe_header_tag = fastrg_malloc(pppoe_header_tag_t,
        PPPoE_TAG_DEFAULT_MAX_LEN, RTE_CACHE_LINE_SIZE);
    if (ppp_ccb->pppoe_phase.pppoe_header_tag == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
            "pppoe header tag allocation failed for ccb %u: %s",
            ccb_id, rte_strerror(errno));
        goto err;
    }

    /* Credential lock: serializes ppp_user_acc/ppp_passwd replacement
     * against every reader (see pppd.h). Buffers themselves stay NULL until
     * the first ppp_init_config_by_user allocates them. */
    rte_spinlock_init(&ppp_ccb->cred_lock);

    return SUCCESS;

err:
    pppd_destroy_ccb_elements(fastrg_ccb, ppp_ccb);
    return ERROR;
}

STATUS ppp_init_config_by_user(FastRG_t *fastrg_ccb, ppp_ccb_t *ppp_ccb, U16 ccb_id, U16 vlan_id, 
    const char *user_name, const char *password)
{
    ppp_ccb->fastrg_ccb = fastrg_ccb;
    if (ppp_copy_credentials(ppp_ccb, user_name, password) == ERROR)
        return ERROR;
    ppp_ccb->control_protocol[PPP_CP_LCP].state = S_INIT;
    ppp_ccb->control_protocol[PPP_CP_IPCP].state = S_INIT;
    ppp_ccb->control_protocol[PPP_CP_IPV6CP].state = S_INIT;
    ppp_ccb->pppoe_phase.active = FALSE;

    ppp_ccb->user_num = ccb_id + 1;
    rte_atomic16_set(&ppp_ccb->vlan_id, vlan_id);

    ppp_ccb->ppp_interval = LCP_ALIVE_ECHO_INTERVAL;  // LCP keepalive echo interval: 30 seconds
    ppp_ccb->echo_miss_count = 0;

    ppp_ccb->hsi_ipv4 = 0x0;
    ppp_ccb->hsi_ipv4_gw = 0x0;
    ppp_ccb->hsi_primary_dns = 0xffffffff; /* 0xffffffff means no dns assigned by server */
    ppp_ccb->hsi_secondary_dns = 0xffffffff; /* 0xffffffff means no dns assigned by server */
    // vlan_id of each subscriptor is 0 to indicate unconfigured
    ppp_ccb->phase = vlan_id != 0 ? END_PHASE : NOT_CONFIGURED;
    ppp_ccb->mru = MAX_RECV_UNIT;
    ppp_ccb->lcp_auth_rejected = FALSE;
    ppp_ccb->lcp_mru_rejected = FALSE;
    ppp_ccb->lcp_magic_rejected = FALSE;
    ppp_ccb->peer_requires_auth = FALSE;
    ppp_ccb->auth_method = PAP_PROTOCOL;
    ppp_ccb->magic_num = rte_cpu_to_be_32((rand() % 0xFFFFFFFE) + 1);
    memset(ppp_ccb->identifier, 0, sizeof(ppp_ccb->identifier));
    memset(ppp_ccb->config_request_pending, 0, sizeof(ppp_ccb->config_request_pending));
    /* Bound was TOTAL_SOCK_PORT before — only the first quarter of the pool
     * got initialized (benign only because ccbs come zeroed from the mempool).
     * expire deadlines live in the SoA array; nat_table_reset() zeroes them
     * and binds each entry's expire_slot. */
    for(int j=0; j<MAX_NAT_ENTRIES; j++) {
        rte_atomic16_init(&ppp_ccb->addr_table[j].is_fill);
    }
    rte_spinlock_init(&ppp_ccb->nat_insert_lock);
    memset(ppp_ccb->PPP_dst_mac.addr_bytes, 0, ETH_ALEN);
    rte_timer_init(&(ppp_ccb->pppoe));
    rte_timer_init(&(ppp_ccb->ppp));
    rte_timer_init(&(ppp_ccb->ppp_ipv6cp));
    rte_timer_init(&(ppp_ccb->ppp_alive));
    rte_atomic16_init(&ppp_ccb->dp_start_bool);
    rte_atomic16_init(&ppp_ccb->ppp_bool);
    rte_atomic16_init(&ppp_ccb->redial_pending);
    /* Default before any HSI config is applied; overridden per-subscriber
     * by apply_hsi_config() using tcp_conntrack_enable from etcd. */
    ppp_ccb->tcp_conntrack_enabled = TRUE;
    /* Default before any HSI config is applied; overridden per-subscriber
     * by apply_hsi_config() using ipv6_enable from etcd. */
    ppp_ccb->ipv6_enabled = FALSE;
    memset(ppp_ccb->ipv6cp_local_iid, 0, sizeof(ppp_ccb->ipv6cp_local_iid));
    memset(ppp_ccb->ipv6cp_peer_iid, 0, sizeof(ppp_ccb->ipv6cp_peer_iid));
    ppp_ccb->ipv6cp_up = FALSE;

    /* All elements below were preallocated by pppd_construct_ccb_elements()
     * at init; a (re)configuration only resets their logical content. No
     * allocation, no free — this function can no longer fail past the
     * credential check above. */

    /* MAC table: O(1) generation bump invalidates every learned entry
     * without touching the hash structure (safe vs concurrent learns). */
    mac_table_reset(ppp_ccb->mac_table);

    /* ARP pending ring: drop queued packets from the previous config. */
    arp_pending_flush(fastrg_ccb->arp_pending_mp, &ppp_ccb->arp_pq);

    /* NAT hashes + free-list: flush all learned flows. */
    nat_table_reset(ppp_ccb);

    return SUCCESS;
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
                pppd_destroy_ccb_elements(fastrg_ccb, array[j]);
                rte_mempool_put(fastrg_ccb->ppp_ccb_mp, array[j]);
                array[j] = NULL;
            }
            return ERROR;
        }

        memset(array[ccb_id], 0, sizeof(ppp_ccb_t));

        /* subscriptor id starts from 1 */
        /* vlan of each subscriptor is 0 to indicate unused */
        if (pppd_construct_ccb_elements(fastrg_ccb, array[ccb_id], ccb_id) == ERROR ||
                ppp_init_config_by_user(fastrg_ccb, array[ccb_id], ccb_id, 0,
                "asdf", "zxcv") == ERROR) {
            for(U16 j=start_id; j<=ccb_id; j++) {
                pppd_destroy_ccb_elements(fastrg_ccb, array[j]);
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
        fastrg_ccb->ppp_ccb_rcu = NULL;
        return ERROR;
    }

    unsigned int lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        rte_rcu_qsbr_thread_register(fastrg_ccb->ppp_ccb_rcu, lcore_id);
    }

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
        if (ppp_ccb == NULL)
            continue;
        exit_ppp(ppp_ccb);
        reset_vlan_map_ccb_id(fastrg_ccb, rte_atomic16_read(&ppp_ccb->vlan_id));
        ppp_cleanup_config_by_user(ppp_ccb, ccb_id);
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, PPPLOGMSG, 
        "%u CCBs disabled", remove_ccb_count);

    return SUCCESS;
}

void pppd_cleanup_ccb(FastRG_t *fastrg_ccb)
{
    if (fastrg_ccb == NULL)
        return;

    if (fastrg_ccb->ppp_ccb != NULL) {
        for(U16 ccb_id=0; ccb_id<fastrg_ccb->max_user_count; ccb_id++) {
            ppp_ccb_t *ppp_ccb = (ppp_ccb_t *)fastrg_ccb->ppp_ccb[ccb_id];
            if (ppp_ccb == NULL)
                continue;
            exit_ppp(ppp_ccb);
            pppd_destroy_ccb_elements(fastrg_ccb, ppp_ccb);
            rte_mempool_put(fastrg_ccb->ppp_ccb_mp, ppp_ccb);
        }
        fastrg_mfree(fastrg_ccb->ppp_ccb);
        fastrg_ccb->ppp_ccb = NULL;
    }

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
    // calculate mempool size as the next power of 2 greater than max_user_count
    unsigned int mempool_size = 1U << (31 - __builtin_clz(fastrg_ccb->max_user_count) + 1);

    if (pppd_init_rcu(fastrg_ccb) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "pppd_init_rcu failed");
        return ERROR;
    }

    fastrg_ccb->ppp_ccb_mp = rte_mempool_create(
        "ppp_ccb_pool",                      /* name */
        mempool_size,                        /* user count */
        sizeof(ppp_ccb_t),                   /* ppp_ccb size */
        /* No per-lcore cache: every object is taken exactly once at init
         * below (fixed-max prealloc) and returned only at shutdown, so a
         * cache would only strand objects. */
        0,                                   /* per-lcore cache size */
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
        fastrg_ccb->ppp_ccb_rcu = NULL;
        return ERROR;
    }

    srand(time(NULL));

    /* Fixed-max full preallocation: the pointer array covers max_user_count
     * slots. User_count (0 at boot, driven by etcd/CLI/gRPC config) is purely the
     * accessible upper bound. The array pointer never changes after
     * this point. */
    fastrg_ccb->ppp_ccb = (void **)fastrg_calloc(ppp_ccb_t *,
        fastrg_ccb->max_user_count, sizeof(ppp_ccb_t *), 0);
    if (fastrg_ccb->ppp_ccb == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
            "ppp_ccb array allocation failed");
        rte_mempool_free(fastrg_ccb->ppp_ccb_mp);
        fastrg_ccb->ppp_ccb_mp = NULL;
        fastrg_mfree(fastrg_ccb->ppp_ccb_rcu);
        fastrg_ccb->ppp_ccb_rcu = NULL;
        return ERROR;
    }

    if (pppd_allocate_ccbs(fastrg_ccb, 0, fastrg_ccb->max_user_count,
            (ppp_ccb_t **)fastrg_ccb->ppp_ccb) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "preallocating %u CCBs failed", fastrg_ccb->max_user_count);
        fastrg_mfree(fastrg_ccb->ppp_ccb);
        fastrg_ccb->ppp_ccb = NULL;
        rte_mempool_free(fastrg_ccb->ppp_ccb_mp);
        fastrg_ccb->ppp_ccb_mp = NULL;
        fastrg_mfree(fastrg_ccb->ppp_ccb_rcu);
        fastrg_ccb->ppp_ccb_rcu = NULL;
        return ERROR;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, PPPLOGMSG, 
        "============ pppoe init successfully (%u/%u slots preallocated) ==============\n",
        fastrg_ccb->max_user_count, fastrg_ccb->max_user_count);
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
    rte_timer_reset(&ppp_ccb->pppoe, fastrg_get_cycles_in_sec(), PERIODICAL, 
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

void exit_ppp(ppp_ccb_t *ppp_ccb)
{
    FastRG_t *fastrg_ccb = ppp_ccb->fastrg_ccb;
    U16 ccb_id = ppp_ccb->user_num - 1;
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);

    rte_atomic16_cmpset((U16 *)&(ppp_ccb->ppp_bool.cnt), 1, 0);
    rte_timer_stop(&(ppp_ccb->ppp));
    rte_timer_stop(&(ppp_ccb->ppp_ipv6cp));
    rte_timer_stop(&(ppp_ccb->pppoe));
    rte_timer_stop(&(ppp_ccb->ppp_alive));
    ppp_ccb->phase = END_PHASE;
    ppp_ccb->control_protocol[PPP_CP_LCP].state = S_INIT;
    ppp_ccb->control_protocol[PPP_CP_IPCP].state = S_INIT;
    ppp_ccb->control_protocol[PPP_CP_IPV6CP].state = S_INIT;
    ppp_ccb->config_request_pending[PPP_CP_IPV6CP] = FALSE;
    memset(ppp_ccb->ipv6cp_local_iid, 0, sizeof(ppp_ccb->ipv6cp_local_iid));
    memset(ppp_ccb->ipv6cp_peer_iid, 0, sizeof(ppp_ccb->ipv6cp_peer_iid));
    ppp_ccb->ipv6cp_up = FALSE;
    ppp_ccb->pppoe_phase.active = FALSE;
    ppp_ccb->hsi_ipv4 = 0x0;
    ppp_ccb->hsi_ipv4_gw = 0x0;
    ppp_ccb->hsi_primary_dns = 0xffffffff; /* 0xffffffff means no dns assigned by server */
    ppp_ccb->hsi_secondary_dns = 0xffffffff; /* 0xffffffff means no dns assigned by server */
    /* LCP option negotiation state is per-session (RFC 1661): a redial starts a
     * fresh negotiation, so rejected-option suppression must not leak across. */
    ppp_ccb->mru = MAX_RECV_UNIT;
    ppp_ccb->lcp_auth_rejected = FALSE;
    ppp_ccb->lcp_mru_rejected = FALSE;
    ppp_ccb->lcp_magic_rejected = FALSE;
    ppp_ccb->peer_requires_auth = FALSE;
    dns_proxy_cleanup(&dhcp_ccb->dns_state);
    FastRG_LOG(INFO, fastrg_ccb->fp, ppp_ccb, PPPLOGMSG, "User %" PRIu16
        " HSI module is terminated.\n", ppp_ccb->user_num);

    /* PPPoE "disconnected" transition → controller via Kafka; the node no longer
     * writes connection status back to etcd. */
    char uid[8];
    snprintf(uid, sizeof(uid), "%u", ppp_ccb->user_num);
    kafka_report_pppoe_state(uid, KAFKA_PPPOE_DISCONNECTED, NULL, NULL, NULL);

    /* Honour a deferred connect: a desire_status=connect that arrived while this
     * session was tearing down was parked (redial_pending) instead of being
     * dropped. Now that the session is fully down (ppp_bool just cleared above),
     * re-dial immediately rather than waiting up to one 60s reconcile sweep.
     * execute_pppoe_dial only enqueues a northbound ENABLE event, so this is safe
     * to call from the teardown context. Cleared first so a failing redial cannot
     * loop (the next attempt falls back to the periodic reconcile). */
    if (rte_atomic16_cmpset((U16 *)&ppp_ccb->redial_pending.cnt, 1, 0)) {
        FastRG_LOG(INFO, fastrg_ccb->fp, ppp_ccb, PPPLOGMSG,
            "User %" PRIu16 " teardown complete; honouring deferred connect (redial)",
            ppp_ccb->user_num);
        execute_pppoe_dial(fastrg_ccb, ccb_id);
    }
}

STATUS ppp_process(FastRG_t *fastrg_ccb, U8 *pkt_data, U16 len)
{
    int         ret;
    U16	        event = E_UNKNOWN, ccb_id = 0;

    ret = get_ccb_id(fastrg_ccb, pkt_data, &ccb_id);
    if (ret == ERROR)
        return ERROR;

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    if (ppp_ccb == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "Invalid CCB ID %u in PPP processing", ccb_id);
        return ERROR;
    }

    ret = PPP_decode_frame(pkt_data, len, &event, ppp_ccb);
    if (ret == ERROR)					
        return ERROR;

    if (check_auth_result(ppp_ccb) == 1)
        return ERROR;

    ppp_ccb->control_protocol[ppp_ccb->cp_id].event = event;
    PPP_FSM(ppp_cp_timer(ppp_ccb), ppp_ccb, event);
    codec_cleanup_ppp_ccb(ppp_ccb);

    return SUCCESS;
}
