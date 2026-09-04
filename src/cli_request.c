#include <common.h>

#include "cli_request.h"
#include "dbg.h"
#include "dhcpd/dhcpd.h"
#include "pppd/pppd.h"
#include "pppd/nat.h"
#include "northbound.h"
#include "utils.h"

U32 cli_request_result_pack(U32 seq, U8 verdict)
{
    return (seq << CLI_REQUEST_VERDICT_BITS) | (verdict & CLI_REQUEST_VERDICT_MASK);
}

void cli_request_publish(FastRG_t *fastrg_ccb, U32 seq, STATUS ret)
{
    if (seq == 0)
        return;

    /* Release: everything this request wrote is published before the verdict. */
    rte_smp_wmb();
    rte_atomic32_set(&fastrg_ccb->cli_request_result,
        (int32_t)cli_request_result_pack(seq,
            ret == SUCCESS ? CLI_REQUEST_OK : CLI_REQUEST_FAILED));
}

void cli_request_abandon(FastRG_t *fastrg_ccb, U32 seq)
{
    if (seq == 0)
        return;

    rte_atomic32_set(&fastrg_ccb->cli_request_abandoned, (int32_t)seq);
}

BOOL cli_request_is_abandoned(FastRG_t *fastrg_ccb, U32 seq)
{
    if (seq == 0)
        return FALSE;

    /* Consume the mark so it can never match a later request. A stale seq that
     * never matches is harmless: it cannot come round again within 2^30. */
    if (rte_atomic32_cmpset((volatile uint32_t *)&fastrg_ccb->cli_request_abandoned.cnt,
            seq, 0) == 0)
        return FALSE;

    return TRUE;
}

BOOL cli_request_dropped(FastRG_t *fastrg_ccb,
    const fastrg_event_northbound_msg_t *msg, void *heap_payload)
{
    if (cli_request_is_abandoned(fastrg_ccb, msg->seq) == FALSE)
        return FALSE;

    free(heap_payload);
    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "CLI request %u dropped: caller gave up\n", msg->seq);
    return TRUE;
}

STATUS set_dns_proxy_enable(FastRG_t *fastrg_ccb, U16 ccb_id, BOOL enable)
{
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id))
        return ERROR;

    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
    dhcp_ccb->dns_state.dns_proxy_enabled = enable;

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %u: DNS proxy %s",
        ccb_id + 1, enable == TRUE ? "enabled" : "disabled");

    return SUCCESS;
}

STATUS set_tcp_conntrack_enable(FastRG_t *fastrg_ccb, U16 ccb_id, BOOL enable)
{
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id))
        return ERROR;

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    ppp_ccb->tcp_conntrack_enabled = enable;

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %u: TCP conntrack %s",
        ccb_id + 1, enable == TRUE ? "enabled" : "disabled");

    return SUCCESS;
}

STATUS set_ipv6_enable(FastRG_t *fastrg_ccb, U16 ccb_id, BOOL enable)
{
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id))
        return ERROR;

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    BOOL was_enabled = ppp_ccb->ipv6_enabled;

    ppp_ccb->ipv6_enabled = enable;
    pppd_ipv6_dp_gate_update(ppp_ccb);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %u: IPv6 %s",
        ccb_id + 1, enable == TRUE ? "enabled" : "disabled");

    /* A live session only picks the new value up by redialing. */
    if (ppp_ccb->ipv6_enabled != was_enabled &&
            fastrg_gen_northbound_event(fastrg_ccb, EV_NORTHBOUND_PPPoE,
                PPPoE_CMD_IPV6_CHANGED, ccb_id, NULL) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Failed to queue the IPv6 change for user %u", ccb_id + 1);
        return ERROR;
    }

    return SUCCESS;
}

STATUS set_snat_port_fwd(FastRG_t *fastrg_ccb, U16 ccb_id, U16 eport,
    const char *dip, U16 iport)
{
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id) || dip == NULL)
        return ERROR;

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    if (ppp_ccb->phase != DATA_PHASE) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
            "User %u has not established PPPoE connection, SNAT port forwarding will be set but not applied",
            ccb_id + 1);
    }

    U32 dip_be;
    if (parse_ip(dip, &dip_be) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Invalid destination IP: %s", dip);
        return ERROR;
    }

    port_fwd_add(ppp_ccb->port_fwd_table, eport, dip_be, iport);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "User %u: SNAT port forward added eport=%u -> %s:%u",
        ccb_id + 1, eport, dip, iport);

    return SUCCESS;
}

STATUS remove_snat_port_fwd(FastRG_t *fastrg_ccb, U16 ccb_id, U16 eport)
{
    if (!is_valid_ccb_id(fastrg_ccb, ccb_id))
        return ERROR;

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);

    if (port_fwd_remove(ppp_ccb->port_fwd_table, eport) == ERROR) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
            "Port forwarding rule not found for user %u, eport=%u",
            ccb_id + 1, eport);
        return ERROR;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "User %u: SNAT port forward removed eport=%u",
        ccb_id + 1, eport);

    return SUCCESS;
}

U32 dns_cache_dump(const dns_cache_t *cache, dns_cache_entry_t *out, U32 max_outs)
{
    if (cache == NULL || out == NULL || max_outs == 0)
        return 0;

    U32 count = 0;

    for(U32 i=0; i<DNS_CACHE_BUCKET_COUNT && count<max_outs; i++) {
        for(const dns_cache_entry_t *entry=cache->buckets[i];
                entry!=NULL && count<max_outs; entry=entry->next) {
            dns_cache_entry_t *dst = &out[count++];

            rte_memcpy(dst, entry, sizeof(*dst));
            /* we copy all entries to a flat array so we don't need to copy the 
            chain pointer */
            dst->next = NULL;
        }
    }

    return count;
}

U32 dns_static_dump(const dns_static_table_t *table, dns_static_record_t *out, U32 max_outs)
{
    if (table == NULL || out == NULL || max_outs == 0)
        return 0;

    U32 count = 0;
    for(U32 i=0; i<DNS_STATIC_MAX_RECORDS && count<max_outs; i++) {
        if (table->records[i].active != TRUE)
            continue;
        out[count++] = table->records[i];
    }

    return count;
}
