/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DNSD.C

  DNS proxy/relay daemon implementation for FastRG.

  Designed by THE on Apr 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#include <string.h>

#include "dnsd.h"

#include <rte_byteorder.h>
#include <rte_memcpy.h>
#include <rte_ethdev.h>

#include "../dp.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../dp_codec.h"
#pragma GCC diagnostic pop
#include "../pppd/pppd.h"
#include "../dhcpd/dhcpd.h"
#include "../fastrg.h"
#include "../init.h"

STATUS dns_proxy_init(dns_proxy_state_t *state, U32 primary_dns, U32 secondary_dns)
{
    if (state == NULL)
        return ERROR;

    if (dns_cache_init(&state->cache) != SUCCESS)
        return ERROR;
    if (dns_static_init(&state->static_table) != SUCCESS)
        return ERROR;

    state->primary_dns = primary_dns;
    state->secondary_dns = secondary_dns;
    state->active_dns = primary_dns;
    state->last_response_time = fastrg_get_cur_cycles();
    state->next_upstream_id = 1;
    memset(state->pending, 0, sizeof(state->pending));

    return SUCCESS;
}

void dns_proxy_cleanup(dns_proxy_state_t *state)
{
    if (state == NULL)
        return;

    dns_cache_cleanup(&state->cache);
    dns_static_cleanup(&state->static_table);
    memset(state->pending, 0, sizeof(state->pending));
}


void dnsd_check_failover(dns_proxy_state_t *state)
{
    if (state == NULL || state->dns_proxy_enabled == FALSE)
        return;

    if (state->secondary_dns == 0)
        return;  /* no secondary DNS configured */

    U64 now = fastrg_get_cur_cycles();
    if (now - state->last_response_time >= (U64)DNS_FAILOVER_TIMEOUT_SECS * 
            fastrg_get_cycles_in_sec()) {
        /* switch to the other DNS server */
        if (state->active_dns == state->primary_dns)
            state->active_dns = state->secondary_dns;
        else
            state->active_dns = state->primary_dns;
        state->last_response_time = now;
    }
}

dns_pending_query_t *find_free_pending(dns_proxy_state_t *state)
{
    U64 now = fastrg_get_cur_cycles();
    U64 timeout_cycles = (U64)DNS_PENDING_TIMEOUT_SECS * fastrg_get_cycles_in_sec();
    for(int i=0; i<DNS_MAX_PENDING_QUERIES; i++) {
        if (state->pending[i].active == FALSE)
            return &state->pending[i];
        /* lazy expiry: treat timed-out slots as free */
        if (now - state->pending[i].start_tsc >= timeout_cycles) {
            state->pending[i].active = FALSE;
            return &state->pending[i];
        }
    }
    return NULL;
}

dns_pending_query_t *find_pending_by_upstream_id(dns_proxy_state_t *state, U16 upstream_id)
{
    U64 now = fastrg_get_cur_cycles();
    U64 timeout_cycles = (U64)DNS_PENDING_TIMEOUT_SECS * fastrg_get_cycles_in_sec();
    for(int i=0; i<DNS_MAX_PENDING_QUERIES; i++) {
        if (state->pending[i].active == FALSE || state->pending[i].upstream_id != upstream_id)
            continue;
        /* lazy expiry: discard timed-out pending */
        if (now - state->pending[i].start_tsc >= timeout_cycles) {
            state->pending[i].active = FALSE;
            return NULL;
        }
        return &state->pending[i];
    }
    return NULL;
}

/* ================================================================
 * Control-plane versions — work on raw packet buffers (tFastRG_MBX.refp)
 * and send via lan_ctrl_tx() / wan_ctrl_tx() from the ctrl_thread.
 * ================================================================ */

/**
 * CP helper: build and send a DNS UDP response to the LAN client.
 * Modifies headers in pkt_data in-place, sends via lan_ctrl_tx().
 */
static int cp_send_dns_udp_response_to_lan(FastRG_t *fastrg_ccb, U8 *pkt_data,
    U16 pkt_len __rte_unused, U16 ccb_id,
    const U8 *dns_resp, U16 dns_resp_len)
{
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);

    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
    vlan_header_t *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(vlan_header + 1);
    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);

    /* swap MAC addresses */
    struct rte_ether_addr tmp_mac;
    rte_ether_addr_copy(&eth_hdr->dst_addr, &tmp_mac);
    rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);

    /* swap IP addresses */
    U32 tmp_ip = ip_hdr->src_addr;
    ip_hdr->src_addr = dhcp_ccb->dhcp_server_ip;
    ip_hdr->dst_addr = tmp_ip;

    /* swap UDP ports */
    U16 tmp_port = udp_hdr->src_port;
    udp_hdr->src_port = udp_hdr->dst_port;
    udp_hdr->dst_port = tmp_port;

    /* copy DNS response payload */
    U8 *dns_payload = (U8 *)(udp_hdr + 1);
    rte_memcpy(dns_payload, dns_resp, dns_resp_len);

    /* update UDP length */
    U16 udp_len = sizeof(struct rte_udp_hdr) + dns_resp_len;
    udp_hdr->dgram_len = rte_cpu_to_be_16(udp_len);

    /* update IP length */
    U16 ip_total_len = sizeof(struct rte_ipv4_hdr) + udp_len;
    ip_hdr->total_length = rte_cpu_to_be_16(ip_total_len);
    ip_hdr->hdr_checksum = 0;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

    /* update UDP checksum */
    udp_hdr->dgram_cksum = 0;
    udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip_hdr, udp_hdr);

    U16 out_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + ip_total_len;
    lan_ctrl_tx(fastrg_ccb, ccb_id, pkt_data, out_len);

    return 1;
}

/**
 * CP helper: forward a DNS query to the upstream DNS server via WAN (PPPoE).
 * Builds PPPoE-encapsulated packet in the raw buffer using memmove,
 * then sends via wan_ctrl_tx().
 */
static int cp_forward_dns_query_to_wan(FastRG_t *fastrg_ccb, U8 *pkt_data,
    U16 pkt_len, U16 ccb_id, dns_proxy_state_t *state,
    dns_pending_query_t *pending)
{
    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);

    U16 l2_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t);
    U16 pppoe_extra = sizeof(pppoe_header_t) + sizeof(ppp_payload_t);
    U16 ip_payload_len = pkt_len - l2_len;

    /* Save VLAN TCI before memmove overwrites it */
    vlan_header_t *orig_vlan = (vlan_header_t *)(pkt_data + sizeof(struct rte_ether_hdr));
    U16 vlan_tci = orig_vlan->tci_union.tci_value;

    /* Shift IP+UDP+DNS payload right to make room for PPPoE header */
    memmove(pkt_data + l2_len + pppoe_extra, pkt_data + l2_len, ip_payload_len);

    /* Rewrite IP/UDP headers at their new positions */
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(pkt_data + l2_len + pppoe_extra);
    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);

    ip_hdr->src_addr = ppp_ccb->hsi_ipv4;
    ip_hdr->dst_addr = state->active_dns;

    udp_hdr->src_port = rte_cpu_to_be_16(pending->upstream_id | 0x8000);
    udp_hdr->dst_port = rte_cpu_to_be_16(DNS_PORT);

    /* Replace DNS ID in payload */
    U8 *dns_payload = (U8 *)(udp_hdr + 1);
    dns_payload[0] = (U8)(pending->upstream_id >> 8);
    dns_payload[1] = (U8)(pending->upstream_id & 0xFF);

    /* Recalculate checksums */
    ip_hdr->hdr_checksum = 0;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    udp_hdr->dgram_cksum = 0;
    udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip_hdr, udp_hdr);

    /* Build Ethernet header */
    struct rte_ether_hdr *new_eth = (struct rte_ether_hdr *)pkt_data;
    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &new_eth->src_addr);
    rte_ether_addr_copy(&ppp_ccb->PPP_dst_mac, &new_eth->dst_addr);
    new_eth->ether_type = rte_cpu_to_be_16(VLAN);

    /* Build VLAN header */
    vlan_header_t *new_vlan = (vlan_header_t *)(new_eth + 1);
    new_vlan->tci_union.tci_value = vlan_tci;
    new_vlan->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);

    /* Build PPPoE header */
    pppoe_header_t *pppoe_hdr = (pppoe_header_t *)(new_vlan + 1);
    pppoe_hdr->ver_type = VER_TYPE;
    pppoe_hdr->code = 0;
    pppoe_hdr->session_id = ppp_ccb->session_id;
    pppoe_hdr->length = rte_cpu_to_be_16(ip_payload_len + sizeof(ppp_payload_t));

    /* PPP protocol field */
    ppp_payload_t *ppp_pay = (ppp_payload_t *)(pppoe_hdr + 1);
    ppp_pay->ppp_protocol = rte_cpu_to_be_16(PPP_IP_PROTOCOL);

    U16 total_len = l2_len + pppoe_extra + ip_payload_len;
    wan_ctrl_tx(fastrg_ccb, ccb_id, pkt_data, total_len);

    return 0;
}

int dnsd_cp_process_lan_udp_query(FastRG_t *fastrg_ccb, U8 *pkt_data, U16 pkt_len, U16 ccb_id)
{
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
    if (dhcp_ccb == NULL)
        return -1;

    dns_proxy_state_t *state = &dhcp_ccb->dns_state;
    if (state->dns_proxy_enabled == FALSE)
        return -1;

    /* Parse headers from raw buffer */
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
    vlan_header_t *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(vlan_header + 1);
    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);

    /* extract DNS payload */
    U8 *dns_data = (U8 *)(udp_hdr + 1);
    U16 udp_payload_len = rte_be_to_cpu_16(udp_hdr->dgram_len) - sizeof(struct rte_udp_hdr);
    if (udp_payload_len < DNS_HDR_LEN)
        return -1;

    dns_message_t query_msg;
    if (dns_parse_query(dns_data, udp_payload_len, &query_msg) != SUCCESS)
        return -1;

    /* Priority 1: check static records */
    dns_static_record_t *static_rec = dns_static_lookup(&state->static_table,
        query_msg.question.name);
    if (static_rec != NULL && query_msg.question.qtype == DNS_TYPE_A) {
        U8 resp_buf[DNS_MAX_PACKET_LEN];
        U16 resp_len = dns_build_response_a(dns_data, udp_payload_len,
            static_rec->ip_addr, static_rec->ttl, resp_buf, sizeof(resp_buf));
        if (resp_len > 0) {
            return cp_send_dns_udp_response_to_lan(fastrg_ccb, pkt_data, pkt_len,
                ccb_id, resp_buf, resp_len);
        }
    }

    /* Priority 2: cache fallback (when upstream is unreachable) */
    dns_cache_entry_t *cache_entry = dns_cache_lookup(&state->cache,
        query_msg.question.name, query_msg.question.qtype);
    if (cache_entry != NULL) {
        U8 resp_buf[DNS_MAX_PACKET_LEN];
        memcpy(resp_buf, cache_entry->response, cache_entry->response_len);
        resp_buf[0] = (U8)(query_msg.header.id >> 8);
        resp_buf[1] = (U8)(query_msg.header.id & 0xFF);

        return cp_send_dns_udp_response_to_lan(fastrg_ccb, pkt_data, pkt_len,
            ccb_id, resp_buf, cache_entry->response_len);
    }

    /* Priority 3: forward to upstream DNS server */
    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    if (rte_atomic16_read(&ppp_ccb->dp_start_bool) != (BIT16)0 && state->active_dns != 0) {
        dns_pending_query_t *pending = find_free_pending(state);
        if (pending != NULL) {
            memset(pending, 0, sizeof(*pending));
            pending->active = TRUE;
            pending->original_id = query_msg.header.id;
            pending->upstream_id = state->next_upstream_id++;
            if (state->next_upstream_id == 0)
                state->next_upstream_id = 1;
            pending->client_ip = ip_hdr->src_addr;
            pending->client_port = udp_hdr->src_port;
            rte_memcpy(pending->client_mac, &eth_hdr->src_addr, 6);
            pending->vlan_tci = vlan_header->tci_union.tci_value;
            pending->start_tsc = fastrg_get_cur_cycles();
            pending->is_tcp = 0;
            pending->qtype = query_msg.question.qtype;
            snprintf(pending->domain, sizeof(pending->domain), "%s", query_msg.question.name);

            return cp_forward_dns_query_to_wan(fastrg_ccb, pkt_data, pkt_len,
                ccb_id, state, pending);
        }
    }

    /* no resolution possible - return SERVFAIL */
    U8 resp_buf[DNS_MAX_PACKET_LEN];
    U16 resp_len = dns_build_response_servfail(dns_data, udp_payload_len,
        resp_buf, sizeof(resp_buf));
    if (resp_len > 0) {
        return cp_send_dns_udp_response_to_lan(fastrg_ccb, pkt_data, pkt_len,
            ccb_id, resp_buf, resp_len);
    }

    return -1;
}

int dnsd_cp_process_lan_tcp_query(FastRG_t *fastrg_ccb, U8 *pkt_data, U16 pkt_len, U16 ccb_id)
{
    /*
     * TCP DNS: the DNS payload is prefixed with a 2-byte length field.
     * For now, we handle single-message TCP DNS queries only (no connection
     * tracking). The query is handled similarly to UDP but with TCP framing.
     *
     * NOTE: Full TCP DNS proxy (with connection state) is complex and rarely
     * needed for residential gateways. Most DNS queries use UDP.
     * TCP DNS is primarily used for large responses (>512 bytes) and zone
     * transfers. We support it at a basic level by treating each TCP segment
     * containing a complete DNS message as a single query.
     */
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
    if (dhcp_ccb == NULL)
        return -1;

    dns_proxy_state_t *state = &dhcp_ccb->dns_state;
    if (state->dns_proxy_enabled == FALSE)
        return -1;

    /* Parse headers from raw buffer */
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
    vlan_header_t *vlan_header __rte_unused = (vlan_header_t *)(eth_hdr + 1);
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(vlan_header + 1);
    struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

    /* TCP DNS has 2-byte length prefix before DNS payload */
    U16 tcp_hdr_len = (tcp_hdr->data_off >> 4) * 4;
    U8 *tcp_payload = (U8 *)tcp_hdr + tcp_hdr_len;
    U16 ip_total = rte_be_to_cpu_16(ip_hdr->total_length);
    U16 tcp_payload_len = ip_total - sizeof(struct rte_ipv4_hdr) - tcp_hdr_len;

    if (tcp_payload_len < 2 + DNS_HDR_LEN)
        return -1;

    U16 dns_len = (U16)((tcp_payload[0] << 8) | tcp_payload[1]);
    U8 *dns_data = tcp_payload + 2;

    if (dns_len > tcp_payload_len - 2 || dns_len < DNS_HDR_LEN)
        return -1;

    dns_message_t query_msg;
    if (dns_parse_query(dns_data, dns_len, &query_msg) != SUCCESS)
        return -1;

    /* check static records */
    dns_static_record_t *static_rec = dns_static_lookup(&state->static_table,
        query_msg.question.name);
    if (static_rec != NULL && query_msg.question.qtype == DNS_TYPE_A) {
        U8 resp_buf[DNS_MAX_PACKET_LEN];
        U16 resp_len = dns_build_response_a(dns_data, dns_len,
            static_rec->ip_addr, static_rec->ttl, resp_buf, sizeof(resp_buf));
        if (resp_len > 0) {
            /* write 2-byte length + DNS response into TCP payload */
            tcp_payload[0] = (U8)(resp_len >> 8);
            tcp_payload[1] = (U8)(resp_len & 0xFF);
            rte_memcpy(tcp_payload + 2, resp_buf, resp_len);

            /* swap addresses for response */
            struct rte_ether_addr tmp_mac;
            rte_ether_addr_copy(&eth_hdr->dst_addr, &tmp_mac);
            rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
            rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);

            U32 tmp_ip = ip_hdr->src_addr;
            ip_hdr->src_addr = dhcp_ccb->dhcp_server_ip;
            ip_hdr->dst_addr = tmp_ip;

            U16 tmp_port = tcp_hdr->src_port;
            tcp_hdr->src_port = tcp_hdr->dst_port;
            tcp_hdr->dst_port = tmp_port;

            /* update IP total length */
            U16 new_ip_len = sizeof(struct rte_ipv4_hdr) + tcp_hdr_len + 2 + resp_len;
            ip_hdr->total_length = rte_cpu_to_be_16(new_ip_len);
            ip_hdr->hdr_checksum = 0;
            ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
            tcp_hdr->cksum = 0;
            tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);

            U16 out_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + new_ip_len;
            lan_ctrl_tx(fastrg_ccb, ccb_id, pkt_data, out_len);
            return 1;
        }
    }

    return -1;
}

int dnsd_cp_process_wan_udp_response(FastRG_t *fastrg_ccb, U8 *pkt_data, U16 pkt_len, U16 ccb_id)
{
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
    if (dhcp_ccb == NULL)
        return 0;

    dns_proxy_state_t *state = &dhcp_ccb->dns_state;

    /* Parse headers from raw buffer */
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
    vlan_header_t *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(vlan_header + 1);
    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);

    /* extract DNS payload */
    U8 *dns_data = (U8 *)(udp_hdr + 1);
    U16 udp_payload_len = rte_be_to_cpu_16(udp_hdr->dgram_len) - sizeof(struct rte_udp_hdr);
    if (udp_payload_len < DNS_HDR_LEN)
        return 0;

    /* get upstream query ID from DNS header */
    U16 upstream_id = (U16)((dns_data[0] << 8) | dns_data[1]);

    /* find matching pending query */
    dns_pending_query_t *pending = find_pending_by_upstream_id(state, upstream_id);
    if (pending == NULL)
        return 0;  /* not a DNS proxy response */

    /* update failover tracking */
    state->last_response_time = fastrg_get_cur_cycles();

    /* parse response for caching */
    dns_message_t resp_msg;
    if (dns_parse_response(dns_data, udp_payload_len, &resp_msg) == SUCCESS) {
        U16 rcode = resp_msg.header.flags & 0x0F;
        if (rcode == DNS_RCODE_OK && resp_msg.answer_count > 0) {
            /* restore original query ID before caching */
            dns_data[0] = (U8)(pending->original_id >> 8);
            dns_data[1] = (U8)(pending->original_id & 0xFF);

            dns_cache_insert(&state->cache, pending->domain,
                pending->qtype, dns_data, udp_payload_len,
                resp_msg.min_ttl > 0 ? resp_msg.min_ttl : 60);
        }
    }

    /* restore original DNS ID */
    dns_data[0] = (U8)(pending->original_id >> 8);
    dns_data[1] = (U8)(pending->original_id & 0xFF);

    /* rewrite packet to send to LAN client */
    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);
    rte_memcpy(&eth_hdr->dst_addr, pending->client_mac, 6);
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    vlan_header->tci_union.tci_value = pending->vlan_tci;
    vlan_header->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IP);

    ip_hdr->src_addr = dhcp_ccb->dhcp_server_ip;
    ip_hdr->dst_addr = pending->client_ip;

    udp_hdr->src_port = rte_cpu_to_be_16(DNS_PORT);
    udp_hdr->dst_port = pending->client_port;

    ip_hdr->hdr_checksum = 0;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    udp_hdr->dgram_cksum = 0;
    udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip_hdr, udp_hdr);

    U16 out_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        rte_be_to_cpu_16(ip_hdr->total_length);

    /* clear pending query */
    pending->active = FALSE;

    lan_ctrl_tx(fastrg_ccb, ccb_id, pkt_data, out_len);

    return 1;
}
