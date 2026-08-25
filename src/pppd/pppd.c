/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  PPPD.C

    - purpose : for ppp detection

  Designed by THE on Jan 14, 2019
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#include <arpa/inet.h>

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
#include <rte_malloc.h>
#include <inttypes.h>

#include "pppd.h"
#include "nat.h"
#include "ipv6_firewall.h"
#include "fsm.h"
#include "codec.h"
#include "../dp.h"
#include "../dbg.h"
#include "../init.h"
#include "../dp_flow.h"
#include "../dhcpd/dhcpd.h"
#include "../dhcp6/dhcp6.h"
#include "../nd6/nd6.h"
#include "../fastrg.h"
#include "../utils.h"
#include "../etcd_integration.h"
#include "../northbound.h"
#include "kafka_producer.h"

void pppd_ipv6_dp_gate_update(ppp_ccb_t *ppp_ccb)
{
    if (ppp_ccb == NULL)
        return;
    if (ppp_ccb->ipv6_enabled != FALSE && ppp_ccb->ipv6cp_up != FALSE &&
            ppp_ccb->dhcp6_pd_ready != FALSE) {
        /* Publish every subscriber field the forwarding path reads (LAN
         * prefix, session id, peer MAC, VLAN) before the gate that authorizes
         * reading them. Pairs with the rte_smp_rmb() in
         * pppd_ipv6_dp_gate_open(). */
        rte_smp_wmb();
        rte_atomic16_set(&ppp_ccb->ipv6_dp_bool, (S16)1);
    } else {
        rte_atomic16_set(&ppp_ccb->ipv6_dp_bool, (S16)0);
    }
}

void pppd_ipv6_report_strings(const ppp_ccb_t *ppp_ccb, char *addr_str,
    U32 addr_len, char *prefix_str, U32 prefix_len, char *dns_str,
    U32 dns_len)
{
    if (ppp_ccb == NULL)
        return;

    if (addr_str != NULL && addr_len > 0) {
        /* RFC 5072: the WAN address is the negotiated interface identifier
         * under the link-local prefix. The node runs IA_PD only, so this is
         * the session's sole WAN IPv6 address. */
        U8 link_local[16] = { 0xfe, 0x80 };

        addr_str[0] = '\0';
        rte_memcpy(&link_local[8], ppp_ccb->ipv6cp_local_iid,
            sizeof(ppp_ccb->ipv6cp_local_iid));
        inet_ntop(AF_INET6, link_local, addr_str, addr_len);
    }

    if (prefix_str != NULL && prefix_len > 0) {
        char prefix[INET6_ADDRSTRLEN];

        prefix_str[0] = '\0';
        /* The whole delegated prefix, not the /64 carved out for the LAN. */
        if (inet_ntop(AF_INET6, ppp_ccb->hsi_ipv6_pd_prefix, prefix,
                sizeof(prefix)) != NULL)
            snprintf(prefix_str, prefix_len, "%s/%u", prefix,
                ppp_ccb->hsi_ipv6_pd_plen);
    }

    if (dns_str != NULL && dns_len > 0) {
        U32 used = 0;

        dns_str[0] = '\0';
        for(U32 i=0; i<RTE_DIM(ppp_ccb->hsi_ipv6_dns); i++) {
            char server[INET6_ADDRSTRLEN];
            int written;

            /* An all-zero entry is an unused server slot. */
            if (ipv6_addr_is_unset(ppp_ccb->hsi_ipv6_dns[i]) == TRUE)
                continue;
            if (inet_ntop(AF_INET6, ppp_ccb->hsi_ipv6_dns[i], server,
                    sizeof(server)) == NULL)
                continue;
            written = snprintf(dns_str + used, dns_len - used, "%s%s",
                used > 0 ? "," : "", server);
            if (written < 0 || (U32)written >= dns_len - used)
                break;
            used += (U32)written;
        }
    }
}

void ppp_build_state_report(const ppp_ccb_t *ppp_ccb, ppp_state_report_t *report)
{
    if (ppp_ccb == NULL || report == NULL)
        return;

    memset(report, 0, sizeof(*report));
    snprintf(report->user_id, sizeof(report->user_id), "%u", ppp_ccb->user_num);

    switch (ppp_ccb->phase) {
        case DATA_PHASE:
            report->phase = PPP_REPORT_CONNECTED;
            break;
        case PPPOE_PHASE:
        case LCP_PHASE:
        case AUTH_PHASE:
        case IPCP_PHASE:
            report->phase = PPP_REPORT_CONNECTING;
            break;
        default:
            /* END_PHASE and NOT_CONFIGURED: no session, so no addresses. */
            report->phase = PPP_REPORT_DISCONNECTED;
            break;
    }
    if (report->phase != PPP_REPORT_CONNECTED)
        return;

    struct in_addr ip = { .s_addr = ppp_ccb->hsi_ipv4 };
    struct in_addr gw = { .s_addr = ppp_ccb->hsi_ipv4_gw };
    inet_ntop(AF_INET, &ip, report->ipv4, sizeof(report->ipv4));
    inet_ntop(AF_INET, &gw, report->ipv4_gw, sizeof(report->ipv4_gw));

    /* IPv6 off, IPV6CP down, or a lease being renewed leave report fields 
     * empty, which every consumer reads as "not reported". */
    if (pppd_ipv6_dp_gate_open(ppp_ccb) == TRUE)
        pppd_ipv6_report_strings(ppp_ccb, report->ipv6_addr,
            sizeof(report->ipv6_addr), report->ipv6_pd_prefix,
            sizeof(report->ipv6_pd_prefix), report->ipv6_dns,
            sizeof(report->ipv6_dns));
}

static kafka_pppoe_phase_t report_phase_to_kafka(ppp_report_phase_t phase)
{
    switch (phase) {
        case PPP_REPORT_CONNECTED:
            return KAFKA_PPPOE_CONNECTED;
        case PPP_REPORT_CONNECTING:
            return KAFKA_PPPOE_CONNECTING;
        default:
            return KAFKA_PPPOE_DISCONNECTED;
    }
}

/* Sending is deliberately not exported: the controller must only ever see
 * reports this file built, so callers go through ppp_report_connection_status()
 * and cannot hand over a report they assembled themselves. */
static STATUS ppp_send_state_report(ppp_ccb_t *ppp_ccb,
    const ppp_state_report_t *report)
{
    if (ppp_ccb == NULL || report == NULL ||
            ppp_ccb->fastrg_ccb->is_standalone == TRUE)
        return ERROR;

    /* An empty field means "nothing to report": pass NULL so the controller
     * stores a NULL column rather than an empty string. */
    kafka_report_pppoe_state(report->user_id, report_phase_to_kafka(report->phase),
        report->ipv4[0] != '\0' ? report->ipv4 : NULL,
        report->ipv4_gw[0] != '\0' ? report->ipv4_gw : NULL, NULL,
        report->ipv6_addr[0] != '\0' ? report->ipv6_addr : NULL,
        report->ipv6_pd_prefix[0] != '\0' ? report->ipv6_pd_prefix : NULL,
        report->ipv6_dns[0] != '\0' ? report->ipv6_dns : NULL);
    return SUCCESS;
}

STATUS ppp_report_connection_status(ppp_ccb_t *ppp_ccb)
{
    ppp_state_report_t report;

    ppp_build_state_report(ppp_ccb, &report);
    return ppp_send_state_report(ppp_ccb, &report);
}

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
    rte_timer_stop(&(s_ppp_ccb->dhcp6_timer));
    rte_timer_stop(&(s_ppp_ccb->ra_timer));
    rte_timer_stop(&(s_ppp_ccb->pppoe));
    rte_timer_stop(&(s_ppp_ccb->ppp_alive));
    dhcp6_pd_stop(s_ppp_ccb);
    s_ppp_ccb->ipv6cp_up = FALSE;
    pppd_ipv6_dp_gate_update(s_ppp_ccb);
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
    nd6_table_free(ppp_ccb->nd6_table);
    ppp_ccb->nd6_table = NULL;
    mac_table_free(ppp_ccb->mac_table);
    ppp_ccb->mac_table = NULL;
    arp_pending_cleanup_queue(&ppp_ccb->arp_pq, fastrg_ccb->arp_pending_mp);
    nat_table_destroy(ppp_ccb);
    ipv6_firewall_table_destroy(ppp_ccb);
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
 *        elements: MAC and IPv6-neighbor table hashes, ARP pending ring,
 *        NAT hashes + free ring, IPv6 firewall hash + free ring, and the
 *        PPPoE header-tag buffer. Called only from pppd_allocate_ccbs() at
 *        init — after this, runtime configuration changes reuse/reset these
 *        objects and never allocate or free.
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

    ppp_ccb->nd6_table = nd6_table_alloc(ccb_id, fastrg_ccb->ppp_ccb_rcu);
    if (ppp_ccb->nd6_table == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
            "nd6_table allocation failed for ccb %u", ccb_id);
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

    if (ipv6_firewall_table_init(ppp_ccb, ccb_id, fastrg_ccb->ppp_ccb_rcu) != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
            "IPv6 firewall hash/ring creation failed for ccb %u", ccb_id);
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

static STATUS pppd_create_checked_size_ccb(FastRG_t *fastrg_ccb, ppp_ccb_t *ppp_ccb,
    struct rte_rcu_qsbr *rcu)
{
    STATUS ret;

    /* Lend the caller's QSBR to the element builders and put the field back;
     * the real one does not exist yet at this point. */
    struct rte_rcu_qsbr *saved_rcu = fastrg_ccb->ppp_ccb_rcu;

    fastrg_ccb->ppp_ccb_rcu = rcu;
    ret = pppd_construct_ccb_elements(fastrg_ccb, ppp_ccb, 0);
    fastrg_ccb->ppp_ccb_rcu = saved_rcu;

    return ret;
}

static void pppd_destroy_checked_size_ccb(FastRG_t *fastrg_ccb, ppp_ccb_t *ppp_ccb)
{
    pppd_destroy_ccb_elements(fastrg_ccb, ppp_ccb);
}

/**
 * @fn pppd_real_heap_free_bytes
 *
 * @brief Free bytes across every heap, read from the allocator itself.
 *
 * @return
 *      Free bytes summed over all sockets
 */
static uint64_t pppd_real_heap_free_bytes(void)
{
    uint64_t free_bytes = 0;

    for(int socket_id=0; socket_id<RTE_MAX_NUMA_NODES; socket_id++) {
        struct rte_malloc_socket_stats stats;
        if (rte_malloc_get_socket_stats(socket_id, &stats) == 0)
            free_bytes += stats.heap_freesz_bytes;
    }
    return free_bytes;
}

/**
 * @fn pppd_probe_element_bytes
 *
 * @brief Measure one subscriber's element cost by building one and tearing it
 *        down.
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @param probe_bytes
 *      [out] Measured cost of one subscriber's elements
 * @return
 *      SUCCESS when the measurement completed and the heap was restored exactly
 */
static STATUS pppd_probe_element_bytes(FastRG_t *fastrg_ccb, uint64_t *probe_bytes)
{
    struct rte_rcu_qsbr *probe_rcu;
    ppp_ccb_t *probe_ccb;
    uint64_t before, after, restored;
    size_t rcu_size = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    STATUS ret = ERROR;

    /* Measuring apparatus, not subscriber cost: allocated before the snapshot,
     * freed after it. */
    probe_rcu = fastrg_calloc(struct rte_rcu_qsbr, 1, rcu_size, RTE_CACHE_LINE_SIZE);
    if (probe_rcu == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, "Capacity probe: cannot allocate QSBR");
        return ERROR;
    }
    if (rte_rcu_qsbr_init(probe_rcu, RTE_MAX_LCORE) != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, "Capacity probe: QSBR init failed");
        fastrg_mfree(probe_rcu);
        return ERROR;
    }
    probe_ccb = fastrg_calloc(ppp_ccb_t, 1, sizeof(ppp_ccb_t), RTE_CACHE_LINE_SIZE);
    if (probe_ccb == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, "Capacity probe: cannot allocate probe ccb");
        fastrg_mfree(probe_rcu);
        return ERROR;
    }

    before = pppd_real_heap_free_bytes();
    if (pppd_create_checked_size_ccb(fastrg_ccb, probe_ccb, probe_rcu) != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
            "Capacity probe: building one subscriber's elements failed");
        goto out;
    }
    after = pppd_real_heap_free_bytes();
    pppd_destroy_checked_size_ccb(fastrg_ccb, probe_ccb);
    restored = pppd_real_heap_free_bytes();

    if (after >= before) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
            "Capacity probe: heap did not shrink while building a subscriber");
        goto out;
    }
    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, PPPLOGMSG,
        "PROBEMEASURE before=%" PRIu64 " after=%" PRIu64 " restored=%" PRIu64
        " delta=%" PRId64, before, after, restored, (int64_t)(restored - before));
    /* Shortfall is a leak, surplus means the window was polluted; either way
     * the measurement is invalid. */
    if (restored != before) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG,
            "Capacity probe: heap not restored, before=%" PRIu64 " restored=%" PRIu64
            " delta=%" PRId64, before, restored, (int64_t)(restored - before));
        goto out;
    }

    *probe_bytes = before - after;
    ret = SUCCESS;

out:
    fastrg_mfree(probe_ccb);
    fastrg_mfree(probe_rcu);
    return ret;
}

STATUS pppd_get_subscriber_real_size(FastRG_t *fastrg_ccb, ccb_memory_info_t *out)
{
    uint64_t probe_bytes = 0;

    if (out == NULL)
        return ERROR;
    if (pppd_probe_element_bytes(fastrg_ccb, &probe_bytes) != SUCCESS)
        return ERROR;

    memset(out, 0, sizeof(*out));
    out->plain_bytes_per_sub = probe_bytes + sizeof(ppp_ccb_t *);
    out->n_pools = 1;
    out->pools[0].objs_per_sub = 1;
    out->pools[0].elt_size = sizeof(ppp_ccb_t);
    return SUCCESS;
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
    for(int i=0; i<MAX_NAT_ENTRIES; i++)
        rte_atomic16_init(&ppp_ccb->addr_table[i].is_fill);
    rte_spinlock_init(&ppp_ccb->nat_insert_lock);
    for(int i=0; i<IPV6_FIREWALL_MAX_ENTRIES; i++)
        rte_atomic16_init(&ppp_ccb->ipv6_firewall_table[i].is_fill);
    rte_spinlock_init(&ppp_ccb->ipv6_firewall_insert_lock);
    memset(ppp_ccb->PPP_dst_mac.addr_bytes, 0, ETH_ALEN);
    rte_timer_init(&(ppp_ccb->pppoe));
    rte_timer_init(&(ppp_ccb->ppp));
    rte_timer_init(&(ppp_ccb->ppp_ipv6cp));
    rte_timer_init(&(ppp_ccb->dhcp6_timer));
    rte_timer_init(&(ppp_ccb->ra_timer));
    rte_timer_init(&(ppp_ccb->ppp_alive));
    rte_atomic16_init(&ppp_ccb->dp_start_bool);
    rte_atomic16_init(&ppp_ccb->ppp_bool);
    rte_atomic16_init(&ppp_ccb->ipv6_dp_bool);
    rte_atomic16_init(&ppp_ccb->redial_pending);
    /* Default before any HSI config is applied; overridden per-subscriber
     * by apply_hsi_config() using tcp_conntrack_enable from etcd. */
    ppp_ccb->tcp_conntrack_enabled = TRUE;
    /* Default before any HSI config is applied; overridden per-subscriber
     * by apply_hsi_config() using ipv6_enable from etcd. */
    ppp_ccb->ipv6_enabled = FALSE;
    pppd_ipv6_dp_gate_update(ppp_ccb);
    memset(ppp_ccb->ipv6cp_local_iid, 0, sizeof(ppp_ccb->ipv6cp_local_iid));
    memset(ppp_ccb->ipv6cp_peer_iid, 0, sizeof(ppp_ccb->ipv6cp_peer_iid));
    ppp_ccb->ipv6cp_up = FALSE;
    pppd_ipv6_dp_gate_update(ppp_ccb);
    dhcp6_pd_stop(ppp_ccb);

    /* All elements below were preallocated by pppd_construct_ccb_elements()
     * at init; a (re)configuration only resets their logical content. No
     * allocation, no free — this function can no longer fail past the
     * credential check above. */

    /* MAC table: O(1) generation bump invalidates every learned entry
     * without touching the hash structure (safe vs concurrent learns). */
    mac_table_reset(ppp_ccb->mac_table);
    nd6_table_reset(ppp_ccb->nd6_table);

    /* ARP pending ring: drop queued packets from the previous config. */
    arp_pending_flush(fastrg_ccb->arp_pending_mp, &ppp_ccb->arp_pq);

    /* NAT hashes + free-list: flush all learned flows. */
    nat_table_reset(ppp_ccb);

    /* IPv6 firewall hash + free-list: flush all learned sessions. Both resets
     * run after pppd_ipv6_dp_gate_update() closed the IPv6 gate above, which
     * is what keeps new packets away from a table being rebuilt. */
    ipv6_firewall_table_reset(ppp_ccb);

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
    if (pppd_init_rcu(fastrg_ccb) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, PPPLOGMSG, 
            "pppd_init_rcu failed");
        return ERROR;
    }

    fastrg_ccb->ppp_ccb_mp = rte_mempool_create(
        "ppp_ccb_pool",                      /* name */
        fastrg_ccb->max_user_count,          /* user count */
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
    rte_timer_stop(&(ppp_ccb->dhcp6_timer));
    rte_timer_stop(&(ppp_ccb->ra_timer));
    rte_timer_stop(&(ppp_ccb->pppoe));
    rte_timer_stop(&(ppp_ccb->ppp_alive));
    dhcp6_pd_stop(ppp_ccb);
    nd6_table_reset(ppp_ccb->nd6_table);
    ppp_ccb->phase = END_PHASE;
    ppp_ccb->control_protocol[PPP_CP_LCP].state = S_INIT;
    ppp_ccb->control_protocol[PPP_CP_IPCP].state = S_INIT;
    ppp_ccb->control_protocol[PPP_CP_IPV6CP].state = S_INIT;
    ppp_ccb->config_request_pending[PPP_CP_IPV6CP] = FALSE;
    memset(ppp_ccb->ipv6cp_local_iid, 0, sizeof(ppp_ccb->ipv6cp_local_iid));
    memset(ppp_ccb->ipv6cp_peer_iid, 0, sizeof(ppp_ccb->ipv6cp_peer_iid));
    ppp_ccb->ipv6cp_up = FALSE;
    pppd_ipv6_dp_gate_update(ppp_ccb);
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
    kafka_report_pppoe_state(uid, KAFKA_PPPOE_DISCONNECTED, NULL, NULL, NULL, NULL, NULL, NULL);

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

    U16 vlan_offset = sizeof(struct rte_ether_hdr);
    U16 ppp_offset = vlan_offset + sizeof(vlan_header_t) +
        sizeof(pppoe_header_t);
    if (len >= ppp_offset + sizeof(ppp_payload_t)) {
        vlan_header_t *vlan_hdr = (vlan_header_t *)(pkt_data + vlan_offset);
        ppp_payload_t *ppp_payload = (ppp_payload_t *)(pkt_data + ppp_offset);

        if (vlan_hdr->next_proto == rte_cpu_to_be_16(ETH_P_PPP_SES) &&
                ppp_payload->ppp_protocol ==
                    rte_cpu_to_be_16(PPP_IPV6_PROTOCOL)) {
            U16 ipv6_offset = ppp_offset + sizeof(*ppp_payload);

            dhcp6_wan_input(ppp_ccb, pkt_data + ipv6_offset,
                len - ipv6_offset);
            return SUCCESS;
        }
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
