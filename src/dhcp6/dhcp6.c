#include <stdlib.h>
#include <string.h>

#include <common.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip6.h>
#include <rte_memcpy.h>
#include <rte_udp.h>

#include "dhcp6.h"
#include "../dbg.h"
#include "../dp.h"
#include "../nd6/nd6.h"
#include "../protocol.h"
#include "../utils.h"
#include "../pppd/codec.h"
#include "../pppd/fsm.h"
#include "../pppd/header.h"

#define DHCP6_HEADER_LEN          4
#define DHCP6_OPTION_HEADER_LEN   4
#define DHCP6_IA_PD_HEADER_LEN    12
#define DHCP6_IAPREFIX_LEN        25
#define DHCP6_SERVER_DUID_MAX     130
#define DHCP6_REQUEST_MAX_RETRY   10
#define DHCP6_SOLICIT_RETRY_SEC   3
#define DHCP6_REQUEST_RETRY_SEC   3
#define DHCP6_RENEW_RETRY_SEC     10
#define DHCP6_NO_PREFIX_RETRY_SEC 60

typedef struct dhcp6_response {
    U8 server_duid[DHCP6_SERVER_DUID_MAX];
    U16 server_duid_len;
    U32 t1;
    U8 prefix[16];
    U8 prefix_plen;
    U8 dns[2][16];
    BOOL no_prefix;
} dhcp6_response_t;

static U16 dhcp6_read_be16(const U8 *p)
{
    U16 value;

    rte_memcpy(&value, p, sizeof(value));
    return rte_be_to_cpu_16(value);
}

static U32 dhcp6_read_be32(const U8 *p)
{
    U32 value;

    rte_memcpy(&value, p, sizeof(value));
    return rte_be_to_cpu_32(value);
}

static void dhcp6_write_be16(U8 *p, U16 value)
{
    value = rte_cpu_to_be_16(value);
    rte_memcpy(p, &value, sizeof(value));
}

static void dhcp6_write_be32(U8 *p, U32 value)
{
    value = rte_cpu_to_be_32(value);
    rte_memcpy(p, &value, sizeof(value));
}

static STATUS dhcp6_append_option(U8 **cursor, const U8 *end, U16 code,
    const U8 *value, U16 len)
{
    if ((size_t)(end - *cursor) < (size_t)DHCP6_OPTION_HEADER_LEN + len)
        return ERROR;

    dhcp6_write_be16(*cursor, code);
    dhcp6_write_be16(*cursor + 2, len);
    if (len > 0)
        rte_memcpy(*cursor + DHCP6_OPTION_HEADER_LEN, value, len);
    *cursor += DHCP6_OPTION_HEADER_LEN + len;
    return SUCCESS;
}

static void dhcp6_build_client_duid(const ppp_ccb_t *ppp_ccb,
    U8 duid[DHCP6_DUID_LL_LEN])
{
    dhcp6_write_be16(&duid[0], 3); /* DUID-LL */
    dhcp6_write_be16(&duid[2], 1); /* Ethernet */
    rte_memcpy(&duid[4],
        ppp_ccb->fastrg_ccb->nic_info.hsi_wan_src_mac.addr_bytes,
        RTE_ETHER_ADDR_LEN);
}

static void dhcp6_new_xid(ppp_ccb_t *ppp_ccb)
{
    U32 xid = (U32)rand();

    ppp_ccb->dhcp6_xid[0] = (U8)(xid >> 16);
    ppp_ccb->dhcp6_xid[1] = (U8)(xid >> 8);
    ppp_ccb->dhcp6_xid[2] = (U8)xid;
}

void dhcp6_derive_lan_prefix(const U8 pd_prefix[16], U8 pd_plen,
    U8 lan_prefix[16])
{
    U8 full_bytes = pd_plen / 8;
    U8 partial_bits = pd_plen % 8;

    memset(lan_prefix, 0, 16);
    if (pd_plen > DHCP6_LAN_PLEN)
        return;
    if (full_bytes > 0)
        rte_memcpy(lan_prefix, pd_prefix, full_bytes);
    if (partial_bits != 0) {
        U8 mask = (U8)(0xff << (8 - partial_bits));
        lan_prefix[full_bytes] = pd_prefix[full_bytes] & mask;
    }
}

static void dhcp6_clear_lease(ppp_ccb_t *ppp_ccb)
{
    /* Publish not-ready before clearing fields that data-plane readers
     * consume. */
    ppp_ccb->dhcp6_pd_ready = FALSE;
    pppd_ipv6_dp_gate_update(ppp_ccb);
    ppp_ccb->dhcp6_t1 = 0;
    ppp_ccb->hsi_ipv6_pd_plen = 0;
    memset(ppp_ccb->hsi_ipv6_pd_prefix, 0,
        sizeof(ppp_ccb->hsi_ipv6_pd_prefix));
    memset(ppp_ccb->hsi_ipv6_lan_prefix, 0,
        sizeof(ppp_ccb->hsi_ipv6_lan_prefix));
    memset(ppp_ccb->hsi_ipv6_dns, 0, sizeof(ppp_ccb->hsi_ipv6_dns));
}

STATUS dhcp6_build_packet(ppp_ccb_t *ppp_ccb, U8 msg_type, U8 *buffer,
    U16 *packet_len)
{
    struct rte_ether_hdr *eth_hdr;
    vlan_header_t *vlan_hdr;
    pppoe_header_t *pppoe_hdr;
    ppp_payload_t *ppp_payload;
    struct rte_ipv6_hdr *ip6;
    struct rte_udp_hdr *udp;
    U8 *dhcp;
    U8 *cursor;
    const U8 *end;
    U8 client_duid[DHCP6_DUID_LL_LEN];
    U8 ia_pd[DHCP6_IA_PD_HEADER_LEN];
    U16 dhcp_len;
    U16 udp_len;
    U16 pppoe_len;

    if (ppp_ccb == NULL || ppp_ccb->fastrg_ccb == NULL || buffer == NULL ||
            packet_len == NULL ||
            (msg_type != DHCP6_SOLICIT && msg_type != DHCP6_REQUEST &&
             msg_type != DHCP6_RENEW))
        return ERROR;
    if (msg_type != DHCP6_SOLICIT &&
            (ppp_ccb->dhcp6_server_duid_len == 0 ||
             ppp_ccb->dhcp6_server_duid_len > DHCP6_SERVER_DUID_MAX))
        return ERROR;

    memset(buffer, 0, DHCP6_PACKET_MAX_LEN);
    eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_hdr = (vlan_header_t *)(eth_hdr + 1);
    pppoe_hdr = (pppoe_header_t *)(vlan_hdr + 1);
    ppp_payload = (ppp_payload_t *)(pppoe_hdr + 1);
    ip6 = (struct rte_ipv6_hdr *)(ppp_payload + 1);
    udp = (struct rte_udp_hdr *)(ip6 + 1);
    dhcp = (U8 *)(udp + 1);
    cursor = dhcp;
    end = buffer + DHCP6_PACKET_MAX_LEN;

    rte_ether_addr_copy(&ppp_ccb->fastrg_ccb->nic_info.hsi_wan_src_mac,
        &eth_hdr->src_addr);
    rte_ether_addr_copy(&ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);
    vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(
        rte_atomic16_read(&ppp_ccb->vlan_id) & 0x0fff);
    vlan_hdr->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);

    pppoe_hdr->ver_type = VER_TYPE;
    pppoe_hdr->code = SESSION_DATA;
    pppoe_hdr->session_id = ppp_ccb->session_id;
    ppp_payload->ppp_protocol = rte_cpu_to_be_16(PPP_IPV6_PROTOCOL);

    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(6) << 28);
    ip6->proto = IPPROTO_UDP;
    ip6->hop_limits = 1;
    U8 *src = (U8 *)&ip6->src_addr;
    U8 *dst = (U8 *)&ip6->dst_addr;
    src[0] = 0xfe;
    src[1] = 0x80;
    rte_memcpy(src + 8, ppp_ccb->ipv6cp_local_iid,
        sizeof(ppp_ccb->ipv6cp_local_iid));
    dst[0] = 0xff;
    dst[1] = 0x02;
    dst[13] = 0x01;
    dst[15] = 0x02;

    udp->src_port = rte_cpu_to_be_16(DHCP6_CLIENT_PORT);
    udp->dst_port = rte_cpu_to_be_16(DHCP6_SERVER_PORT);

    *cursor++ = msg_type;
    rte_memcpy(cursor, ppp_ccb->dhcp6_xid, sizeof(ppp_ccb->dhcp6_xid));
    cursor += sizeof(ppp_ccb->dhcp6_xid);
    dhcp6_build_client_duid(ppp_ccb, client_duid);
    if (dhcp6_append_option(&cursor, end, DHCP6_OPT_CLIENTID, client_duid,
            sizeof(client_duid)) == ERROR)
        return ERROR;
    if (msg_type != DHCP6_SOLICIT &&
            dhcp6_append_option(&cursor, end, DHCP6_OPT_SERVERID,
                ppp_ccb->dhcp6_server_duid,
                ppp_ccb->dhcp6_server_duid_len) == ERROR)
        return ERROR;

    dhcp6_write_be32(&ia_pd[0], ppp_ccb->user_num);
    dhcp6_write_be32(&ia_pd[4], 0);
    dhcp6_write_be32(&ia_pd[8], 0);
    if (dhcp6_append_option(&cursor, end, DHCP6_OPT_IA_PD, ia_pd,
            sizeof(ia_pd)) == ERROR)
        return ERROR;

    dhcp_len = (U16)(cursor - dhcp);
    udp_len = sizeof(*udp) + dhcp_len;
    ip6->payload_len = rte_cpu_to_be_16(udp_len);
    udp->dgram_len = rte_cpu_to_be_16(udp_len);
    udp->dgram_cksum = 0;
    udp->dgram_cksum = rte_ipv6_udptcp_cksum(ip6, udp);

    pppoe_len = sizeof(*ppp_payload) + sizeof(*ip6) + udp_len;
    pppoe_hdr->length = rte_cpu_to_be_16(pppoe_len);
    *packet_len = sizeof(*eth_hdr) + sizeof(*vlan_hdr) + sizeof(*pppoe_hdr) +
        pppoe_len;
    return SUCCESS;
}

static void dhcp6_send(ppp_ccb_t *ppp_ccb, U8 msg_type)
{
    U8 buffer[DHCP6_PACKET_MAX_LEN];
    U16 packet_len = 0;

    if (dhcp6_build_packet(ppp_ccb, msg_type, buffer, &packet_len) == ERROR)
        return;
#ifndef UNIT_TEST
    wan_ctrl_tx(ppp_ccb->fastrg_ccb, ppp_ccb->user_num - 1, buffer,
        packet_len);
#endif
}

static void dhcp6_arm(ppp_ccb_t *ppp_ccb, U32 seconds, U32 type)
{
    rte_timer_stop(&ppp_ccb->dhcp6_timer);
    rte_timer_reset(&ppp_ccb->dhcp6_timer,
        (U64)seconds * fastrg_get_cycles_in_sec(), type,
        ppp_ccb->fastrg_ccb->lcore.ctrl_thread, dhcp6_timer_cb, ppp_ccb);
}

static STATUS dhcp6_parse_ia_pd(ppp_ccb_t *ppp_ccb, const U8 *data,
    U16 len, dhcp6_response_t *response)
{
    const U8 *cursor;
    U16 remaining;
    BOOL has_prefix = FALSE;

    if (len < DHCP6_IA_PD_HEADER_LEN ||
            dhcp6_read_be32(data) != ppp_ccb->user_num)
        return ERROR;
    response->t1 = dhcp6_read_be32(data + 4);
    cursor = data + DHCP6_IA_PD_HEADER_LEN;
    remaining = len - DHCP6_IA_PD_HEADER_LEN;
    while (remaining > 0) {
        U16 code;
        U16 opt_len;

        if (remaining < DHCP6_OPTION_HEADER_LEN)
            return ERROR;
        code = dhcp6_read_be16(cursor);
        opt_len = dhcp6_read_be16(cursor + 2);
        if (opt_len > remaining - DHCP6_OPTION_HEADER_LEN)
            return ERROR;
        const U8 *value = cursor + DHCP6_OPTION_HEADER_LEN;
        if (code == DHCP6_OPT_STATUS) {
            if (opt_len < sizeof(U16))
                return ERROR;
            if (dhcp6_read_be16(value) == DHCP6_STATUS_NO_PREFIX)
                response->no_prefix = TRUE;
        } else if (code == DHCP6_OPT_IAPREFIX) {
            if (has_prefix || opt_len != DHCP6_IAPREFIX_LEN)
                return ERROR;
            if (value[8] != DHCP6_PD_PLEN) {
                FastRG_LOG(WARN, ppp_ccb->fastrg_ccb->fp, ppp_ccb,
                    PPPLOGMSG,
                    "User %" PRIu16 " DHCPv6-PD server returned /%u; only /56 is supported.",
                    ppp_ccb->user_num, value[8]);
                return ERROR;
            }
            rte_memcpy(response->prefix, value + 9,
                sizeof(response->prefix));
            response->prefix_plen = value[8];
            has_prefix = TRUE;
        }
        cursor += DHCP6_OPTION_HEADER_LEN + opt_len;
        remaining -= DHCP6_OPTION_HEADER_LEN + opt_len;
    }

    if (response->no_prefix == FALSE && has_prefix == FALSE)
        return ERROR;
    return SUCCESS;
}

static STATUS dhcp6_parse_response(ppp_ccb_t *ppp_ccb, const U8 *dhcp,
    U16 len, U8 expected_type, dhcp6_response_t *response)
{
    U8 client_duid[DHCP6_DUID_LL_LEN];
    const U8 *cursor;
    U16 remaining;
    BOOL has_server = FALSE;
    BOOL has_client = FALSE;
    BOOL has_ia_pd = FALSE;
    BOOL has_dns = FALSE;

    if (len < DHCP6_HEADER_LEN || dhcp[0] != expected_type ||
            memcmp(&dhcp[1], ppp_ccb->dhcp6_xid,
                sizeof(ppp_ccb->dhcp6_xid)) != 0)
        return ERROR;

    memset(response, 0, sizeof(*response));
    dhcp6_build_client_duid(ppp_ccb, client_duid);
    cursor = dhcp + DHCP6_HEADER_LEN;
    remaining = len - DHCP6_HEADER_LEN;
    while (remaining > 0) {
        U16 code;
        U16 opt_len;

        if (remaining < DHCP6_OPTION_HEADER_LEN)
            return ERROR;
        code = dhcp6_read_be16(cursor);
        opt_len = dhcp6_read_be16(cursor + 2);
        if (opt_len > remaining - DHCP6_OPTION_HEADER_LEN)
            return ERROR;
        const U8 *value = cursor + DHCP6_OPTION_HEADER_LEN;

        if (code == DHCP6_OPT_SERVERID) {
            if (has_server || opt_len == 0 ||
                    opt_len > sizeof(response->server_duid))
                return ERROR;
            rte_memcpy(response->server_duid, value, opt_len);
            response->server_duid_len = opt_len;
            has_server = TRUE;
        } else if (code == DHCP6_OPT_CLIENTID) {
            if (has_client || opt_len != sizeof(client_duid) ||
                    memcmp(value, client_duid, sizeof(client_duid)) != 0)
                return ERROR;
            has_client = TRUE;
        } else if (code == DHCP6_OPT_IA_PD) {
            if (has_ia_pd ||
                    dhcp6_parse_ia_pd(ppp_ccb, value, opt_len,
                        response) == ERROR)
                return ERROR;
            has_ia_pd = TRUE;
        } else if (code == DHCP6_OPT_DNS_SERVERS) {
            if (has_dns || opt_len % 16 != 0)
                return ERROR;
            U16 copy_len = RTE_MIN(opt_len, (U16)sizeof(response->dns));
            if (copy_len > 0)
                rte_memcpy(response->dns, value, copy_len);
            has_dns = TRUE;
        }

        cursor += DHCP6_OPTION_HEADER_LEN + opt_len;
        remaining -= DHCP6_OPTION_HEADER_LEN + opt_len;
    }

    return has_server && has_client && has_ia_pd ? SUCCESS : ERROR;
}

STATUS dhcp6_process_message(ppp_ccb_t *ppp_ccb, const U8 *dhcp, U16 len)
{
    dhcp6_response_t response;
    U8 expected_type;

    if (ppp_ccb == NULL || dhcp == NULL)
        return ERROR;
    if (ppp_ccb->dhcp6_state == DHCP6_S_SOLICITING)
        expected_type = DHCP6_ADVERTISE;
    else if (ppp_ccb->dhcp6_state == DHCP6_S_REQUESTING ||
            ppp_ccb->dhcp6_state == DHCP6_S_RENEWING)
        expected_type = DHCP6_REPLY;
    else
        return ERROR;

    if (dhcp6_parse_response(ppp_ccb, dhcp, len, expected_type,
            &response) == ERROR)
        return ERROR;
    if ((ppp_ccb->dhcp6_state == DHCP6_S_REQUESTING ||
            ppp_ccb->dhcp6_state == DHCP6_S_RENEWING) &&
            (response.server_duid_len != ppp_ccb->dhcp6_server_duid_len ||
             memcmp(response.server_duid, ppp_ccb->dhcp6_server_duid,
                 response.server_duid_len) != 0))
        return ERROR;

    if (response.no_prefix == TRUE) {
        FastRG_LOG(WARN, ppp_ccb->fastrg_ccb->fp, ppp_ccb, PPPLOGMSG,
            "User %" PRIu16 " DHCPv6-PD server has no prefix available; retrying in 60 seconds.",
            ppp_ccb->user_num);
        dhcp6_clear_lease(ppp_ccb);
        memset(ppp_ccb->dhcp6_server_duid, 0,
            sizeof(ppp_ccb->dhcp6_server_duid));
        ppp_ccb->dhcp6_server_duid_len = 0;
        ppp_ccb->dhcp6_state = DHCP6_S_SOLICITING;
        ppp_ccb->dhcp6_retry = 0;
        dhcp6_new_xid(ppp_ccb);
        dhcp6_arm(ppp_ccb, DHCP6_NO_PREFIX_RETRY_SEC, SINGLE);
        return SUCCESS;
    }

    if (ppp_ccb->dhcp6_state == DHCP6_S_SOLICITING) {
        rte_memcpy(ppp_ccb->dhcp6_server_duid, response.server_duid,
            response.server_duid_len);
        ppp_ccb->dhcp6_server_duid_len = response.server_duid_len;
        ppp_ccb->dhcp6_t1 = response.t1 != 0 ? response.t1 : DHCP6_DEFAULT_T1;
        rte_memcpy(ppp_ccb->hsi_ipv6_pd_prefix, response.prefix,
            sizeof(response.prefix));
        ppp_ccb->hsi_ipv6_pd_plen = response.prefix_plen;
        dhcp6_derive_lan_prefix(response.prefix, response.prefix_plen,
            ppp_ccb->hsi_ipv6_lan_prefix);
        rte_memcpy(ppp_ccb->hsi_ipv6_dns, response.dns,
            sizeof(response.dns));
        ppp_ccb->dhcp6_state = DHCP6_S_REQUESTING;
        ppp_ccb->dhcp6_retry = 0;
        dhcp6_send(ppp_ccb, DHCP6_REQUEST);
        dhcp6_arm(ppp_ccb, DHCP6_REQUEST_RETRY_SEC, PERIODICAL);
        return SUCCESS;
    }

    /* Snapshot the lease this reply replaces, for the northbound re-report at
     * the end of this function. */
    BOOL was_ready = ppp_ccb->dhcp6_pd_ready;
    BOOL lease_changed = memcmp(ppp_ccb->hsi_ipv6_pd_prefix, response.prefix,
                sizeof(response.prefix)) != 0 ||
            ppp_ccb->hsi_ipv6_pd_plen != response.prefix_plen ||
            memcmp(ppp_ccb->hsi_ipv6_dns, response.dns,
                sizeof(response.dns)) != 0 ? TRUE : FALSE;

    ppp_ccb->dhcp6_pd_ready = FALSE;
    pppd_ipv6_dp_gate_update(ppp_ccb);
    rte_memcpy(ppp_ccb->hsi_ipv6_pd_prefix, response.prefix,
        sizeof(response.prefix));
    ppp_ccb->hsi_ipv6_pd_plen = response.prefix_plen;
    dhcp6_derive_lan_prefix(response.prefix, response.prefix_plen,
        ppp_ccb->hsi_ipv6_lan_prefix);
    rte_memcpy(ppp_ccb->hsi_ipv6_dns, response.dns,
        sizeof(response.dns));
    ppp_ccb->dhcp6_t1 = response.t1 != 0 ? response.t1 : DHCP6_DEFAULT_T1;
    ppp_ccb->dhcp6_state = DHCP6_S_BOUND;
    ppp_ccb->dhcp6_retry = 0;
    /* All delegated data is complete before it becomes visible to readers. */
    ppp_ccb->dhcp6_pd_ready = TRUE;
    pppd_ipv6_dp_gate_update(ppp_ccb);
    nd6_ra_start(ppp_ccb);
    dhcp6_arm(ppp_ccb, ppp_ccb->dhcp6_t1, SINGLE);
    FastRG_LOG(INFO, ppp_ccb->fastrg_ccb->fp, ppp_ccb, PPPLOGMSG,
        "User %" PRIu16 " DHCPv6-PD lease is ready (/%u, T1=%u seconds).",
        ppp_ccb->user_num, ppp_ccb->hsi_ipv6_pd_plen, ppp_ccb->dhcp6_t1);

    /* Delegation finishes after IPCP opens, so the connected event sent back
     * then carried no IPv6 — re-send it now that the lease exists. A renew
     * that returns the same prefix and DNS servers sends nothing, keeping one
     * event per T1 out of the pipeline. Before IPCP opens there is nothing to
     * re-send: ipcp_layer_up() will pick this lease up itself.
     *
     * Losing IPv6 mid-session (IPV6CP down, or a renew with no prefix) closes
     * the forwarding gate but sends no event: a connected event racing a
     * teardown is worse than a stale IPv6 row on the controller, which the
     * next disconnect or successful lease corrects anyway. */
    if ((was_ready == FALSE || lease_changed == TRUE) &&
            ppp_ccb->phase == DATA_PHASE)
        ppp_report_connection_status(ppp_ccb);
    return SUCCESS;
}

void dhcp6_wan_input(ppp_ccb_t *ppp_ccb, U8 *ipv6_pkt, U16 len)
{
    struct rte_ipv6_hdr *ip6;
    struct rte_udp_hdr *udp;
    U16 ip_payload_len;
    U16 udp_len;

    if (ppp_ccb == NULL || ppp_ccb->ipv6_enabled == FALSE ||
            ppp_ccb->ipv6cp_up == FALSE)
        return;
    if (ipv6_pkt == NULL || len < sizeof(struct rte_ipv6_hdr))
        return;

    ip6 = (struct rte_ipv6_hdr *)ipv6_pkt;
    ip_payload_len = rte_be_to_cpu_16(ip6->payload_len);
    if (rte_ipv6_check_version(ip6) != 0 ||
            ip_payload_len != len - sizeof(*ip6))
        return;
    if (ip6->proto == IPPROTO_ICMPV6) {
        FastRG_LOG(DBG, ppp_ccb->fastrg_ccb->fp, ppp_ccb, PPPLOGMSG,
            "User %" PRIu16 " ICMPv6 control packet is reserved for the IPv6 control module.",
            ppp_ccb->user_num);
        return;
    }
    if (ip6->proto != IPPROTO_UDP ||
            ip_payload_len < sizeof(struct rte_udp_hdr) + DHCP6_HEADER_LEN)
        return;

    udp = (struct rte_udp_hdr *)(ip6 + 1);
    udp_len = rte_be_to_cpu_16(udp->dgram_len);
    if (rte_be_to_cpu_16(udp->dst_port) != DHCP6_CLIENT_PORT ||
            udp_len != ip_payload_len || udp_len < sizeof(*udp) + DHCP6_HEADER_LEN)
        return;
    dhcp6_process_message(ppp_ccb, (U8 *)(udp + 1), udp_len - sizeof(*udp));
}

void dhcp6_timer_cb(__attribute__((unused)) struct rte_timer *tim, void *arg)
{
    ppp_ccb_t *ppp_ccb = (ppp_ccb_t *)arg;

    if (ppp_ccb == NULL || ppp_ccb->ipv6_enabled == FALSE ||
            ppp_ccb->ipv6cp_up == FALSE) {
        if (ppp_ccb != NULL)
            dhcp6_pd_stop(ppp_ccb);
        return;
    }

    if (ppp_ccb->dhcp6_state == DHCP6_S_BOUND) {
        dhcp6_new_xid(ppp_ccb);
        ppp_ccb->dhcp6_state = DHCP6_S_RENEWING;
        ppp_ccb->dhcp6_retry = 0;
        dhcp6_send(ppp_ccb, DHCP6_RENEW);
        dhcp6_arm(ppp_ccb, DHCP6_RENEW_RETRY_SEC, PERIODICAL);
    } else if (ppp_ccb->dhcp6_state == DHCP6_S_RENEWING) {
        dhcp6_send(ppp_ccb, DHCP6_RENEW);
        ppp_ccb->dhcp6_retry++;
        dhcp6_arm(ppp_ccb, DHCP6_RENEW_RETRY_SEC, PERIODICAL);
    } else if (ppp_ccb->dhcp6_state == DHCP6_S_REQUESTING) {
        if (ppp_ccb->dhcp6_retry >= DHCP6_REQUEST_MAX_RETRY) {
            dhcp6_clear_lease(ppp_ccb);
            memset(ppp_ccb->dhcp6_server_duid, 0,
                sizeof(ppp_ccb->dhcp6_server_duid));
            ppp_ccb->dhcp6_server_duid_len = 0;
            dhcp6_new_xid(ppp_ccb);
            ppp_ccb->dhcp6_state = DHCP6_S_SOLICITING;
            ppp_ccb->dhcp6_retry = 0;
            dhcp6_send(ppp_ccb, DHCP6_SOLICIT);
            dhcp6_arm(ppp_ccb, DHCP6_SOLICIT_RETRY_SEC, PERIODICAL);
        } else {
            dhcp6_send(ppp_ccb, DHCP6_REQUEST);
            ppp_ccb->dhcp6_retry++;
            dhcp6_arm(ppp_ccb, DHCP6_REQUEST_RETRY_SEC, PERIODICAL);
        }
    } else if (ppp_ccb->dhcp6_state == DHCP6_S_SOLICITING) {
        dhcp6_send(ppp_ccb, DHCP6_SOLICIT);
        ppp_ccb->dhcp6_retry++;
        dhcp6_arm(ppp_ccb, DHCP6_SOLICIT_RETRY_SEC, PERIODICAL);
    }
}

void dhcp6_pd_start(ppp_ccb_t *ppp_ccb)
{
    if (ppp_ccb == NULL || ppp_ccb->ipv6_enabled == FALSE ||
            ppp_ccb->ipv6cp_up == FALSE)
        return;

    dhcp6_pd_stop(ppp_ccb);
    dhcp6_new_xid(ppp_ccb);
    ppp_ccb->dhcp6_state = DHCP6_S_SOLICITING;
    dhcp6_send(ppp_ccb, DHCP6_SOLICIT);
    dhcp6_arm(ppp_ccb, DHCP6_SOLICIT_RETRY_SEC, PERIODICAL);
}

void dhcp6_pd_stop(ppp_ccb_t *ppp_ccb)
{
    if (ppp_ccb == NULL)
        return;

    nd6_ra_stop(ppp_ccb);
    rte_timer_stop(&ppp_ccb->dhcp6_timer);
    dhcp6_clear_lease(ppp_ccb);
    ppp_ccb->dhcp6_state = DHCP6_S_IDLE;
    memset(ppp_ccb->dhcp6_xid, 0, sizeof(ppp_ccb->dhcp6_xid));
    memset(ppp_ccb->dhcp6_server_duid, 0,
        sizeof(ppp_ccb->dhcp6_server_duid));
    ppp_ccb->dhcp6_server_duid_len = 0;
    ppp_ccb->dhcp6_retry = 0;
}
