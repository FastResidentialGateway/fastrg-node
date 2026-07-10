#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <common.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "../../src/dnsd/dnsd.h"
#include "../../src/dhcpd/dhcpd.h"
#include "../../src/fastrg.h"
#include "../../src/init.h"
#include "../../src/protocol.h"
#include "../test_helper.h"

static int test_count = 0;
static int pass_count = 0;

/* ---- dnsd_check_failover tests ---- */

static void test_failover_disabled(void)
{
    printf("\nTesting dnsd_check_failover (proxy disabled):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0x02020202);
    /* dns_proxy_init leaves dns_proxy_enabled = FALSE */
    state.last_response_time = fastrg_get_cur_cycles() -
        (U64)(DNS_FAILOVER_TIMEOUT_SECS + 10) * fastrg_get_cycles_in_sec();

    U32 before = state.active_dns;
    dnsd_check_failover(&state);
    TEST_ASSERT(state.active_dns == before,
        "failover does NOT fire when dns_proxy_enabled is FALSE", "");
    dns_proxy_cleanup(&state);
}

static void test_failover_no_secondary(void)
{
    printf("\nTesting dnsd_check_failover (no secondary DNS):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);
    state.dns_proxy_enabled = TRUE;
    state.last_response_time = fastrg_get_cur_cycles() -
        (U64)(DNS_FAILOVER_TIMEOUT_SECS + 10) * fastrg_get_cycles_in_sec();

    U32 before = state.active_dns;
    dnsd_check_failover(&state);
    TEST_ASSERT(state.active_dns == before,
        "failover does NOT fire when secondary_dns is 0", "");
    dns_proxy_cleanup(&state);
}

static void test_failover_timeout_not_reached(void)
{
    printf("\nTesting dnsd_check_failover (timeout not reached):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0x02020202);
    state.dns_proxy_enabled = TRUE;
    /* last response just now — timeout NOT reached */
    state.last_response_time = fastrg_get_cur_cycles();

    U32 before = state.active_dns;
    dnsd_check_failover(&state);
    TEST_ASSERT(state.active_dns == before,
        "active_dns unchanged when timeout not reached", "");
    dns_proxy_cleanup(&state);
}

static void test_failover_primary_to_secondary(void)
{
    printf("\nTesting dnsd_check_failover (primary -> secondary):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0x02020202);
    state.dns_proxy_enabled = TRUE;
    /* simulate timeout expired */
    state.last_response_time = fastrg_get_cur_cycles() -
        (U64)(DNS_FAILOVER_TIMEOUT_SECS + 1) * fastrg_get_cycles_in_sec();

    dnsd_check_failover(&state);
    TEST_ASSERT(state.active_dns == state.secondary_dns,
        "active_dns switches to secondary after timeout", "");

    /* simulate timeout expired again — should switch back to primary */
    state.last_response_time = fastrg_get_cur_cycles() -
        (U64)(DNS_FAILOVER_TIMEOUT_SECS + 1) * fastrg_get_cycles_in_sec();
    dnsd_check_failover(&state);
    TEST_ASSERT(state.active_dns == state.primary_dns,
        "active_dns switches back to primary on second timeout", "");
    dns_proxy_cleanup(&state);
}

/* ---- find_free_pending tests ---- */

static void test_find_free_pending_all_inactive(void)
{
    printf("\nTesting find_free_pending (all slots inactive):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    dns_pending_query_t *slot = find_free_pending(&state);
    TEST_ASSERT(slot != NULL, "returns a slot from fresh state", "");
    TEST_ASSERT(slot->active == FALSE, "returned slot is inactive", "");
    dns_proxy_cleanup(&state);
}

static void test_find_free_pending_skips_active(void)
{
    printf("\nTesting find_free_pending (first slot active, second free):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    /* Mark slot 0 as active and fresh */
    state.pending[0].active    = TRUE;
    state.pending[0].start_tsc = fastrg_get_cur_cycles();
    state.pending[0].upstream_id = 1;

    dns_pending_query_t *slot = find_free_pending(&state);
    TEST_ASSERT(slot != NULL, "returns a slot", "");
    TEST_ASSERT(slot != &state.pending[0], "does not return the active slot 0", "");
    TEST_ASSERT(slot->active == FALSE, "returned slot is inactive", "");
    dns_proxy_cleanup(&state);
}

static void test_find_free_pending_expired(void)
{
    printf("\nTesting find_free_pending (all active, one expired):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    /* Fill all 32 slots as active with fresh start_tsc */
    U64 now = fastrg_get_cur_cycles();
    for (int i = 0; i < DNS_MAX_PENDING_QUERIES; i++) {
        state.pending[i].active    = TRUE;
        state.pending[i].start_tsc = now;
        state.pending[i].upstream_id = (U16)(i + 1);
    }

    /* Expire slot 5 by backdating its start_tsc */
    state.pending[5].start_tsc = now -
        (U64)(DNS_PENDING_TIMEOUT_SECS + 1) * fastrg_get_cycles_in_sec();

    dns_pending_query_t *slot = find_free_pending(&state);
    TEST_ASSERT(slot != NULL, "returns a slot despite all being active", "");
    TEST_ASSERT(slot == &state.pending[5], "returns the expired slot (slot 5)", "");
    TEST_ASSERT(slot->active == FALSE, "expired slot is marked inactive", "");
    dns_proxy_cleanup(&state);
}

static void test_find_free_pending_full(void)
{
    printf("\nTesting find_free_pending (all slots active and fresh):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    U64 now = fastrg_get_cur_cycles();
    for (int i = 0; i < DNS_MAX_PENDING_QUERIES; i++) {
        state.pending[i].active    = TRUE;
        state.pending[i].start_tsc = now;
        state.pending[i].upstream_id = (U16)(i + 1);
    }

    dns_pending_query_t *slot = find_free_pending(&state);
    TEST_ASSERT(slot == NULL, "returns NULL when all slots are active and fresh", "");
    dns_proxy_cleanup(&state);
}

/* ---- find_pending_by_upstream_id tests ---- */

static void test_find_pending_by_id_hit(void)
{
    printf("\nTesting find_pending_by_upstream_id (match found):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    state.pending[3].active      = TRUE;
    state.pending[3].upstream_id = 42;
    state.pending[3].start_tsc   = fastrg_get_cur_cycles();

    dns_pending_query_t *found = find_pending_by_upstream_id(&state, 42);
    TEST_ASSERT(found != NULL, "returns non-NULL for matching id", "");
    TEST_ASSERT(found == &state.pending[3], "returns correct slot (slot 3)", "");
    TEST_ASSERT(found->upstream_id == 42, "upstream_id matches", "");
    dns_proxy_cleanup(&state);
}

static void test_find_pending_by_id_miss(void)
{
    printf("\nTesting find_pending_by_upstream_id (no match):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    state.pending[0].active      = TRUE;
    state.pending[0].upstream_id = 7;
    state.pending[0].start_tsc   = fastrg_get_cur_cycles();

    dns_pending_query_t *found = find_pending_by_upstream_id(&state, 99);
    TEST_ASSERT(found == NULL, "returns NULL when upstream_id not found", "");
    dns_proxy_cleanup(&state);
}

static void test_find_pending_by_id_expired(void)
{
    printf("\nTesting find_pending_by_upstream_id (match but expired):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    /* Slot with matching id but backdated (timed out) */
    state.pending[1].active      = TRUE;
    state.pending[1].upstream_id = 55;
    state.pending[1].start_tsc   = fastrg_get_cur_cycles() -
        (U64)(DNS_PENDING_TIMEOUT_SECS + 1) * fastrg_get_cycles_in_sec();

    dns_pending_query_t *found = find_pending_by_upstream_id(&state, 55);
    TEST_ASSERT(found == NULL, "returns NULL for timed-out pending query", "");
    TEST_ASSERT(state.pending[1].active == FALSE,
        "expired slot is marked inactive after lookup", "");
    dns_proxy_cleanup(&state);
}

static void test_find_pending_by_id_inactive(void)
{
    printf("\nTesting find_pending_by_upstream_id (slot inactive):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    /* Slot has matching id but active == FALSE */
    state.pending[2].active      = FALSE;
    state.pending[2].upstream_id = 77;
    state.pending[2].start_tsc   = fastrg_get_cur_cycles();

    dns_pending_query_t *found = find_pending_by_upstream_id(&state, 77);
    TEST_ASSERT(found == NULL, "returns NULL when slot is inactive", "");
    dns_proxy_cleanup(&state);
}

/* ---- dnsd_cp_process_lan_tcp_query tests ---- */

/*
 * dnsd_cp_process_lan_tcp_query resolves ccb_id through DHCPD_GET_CCB, so
 * it must run against the shared fastrg_ccb->dhcp_ccb[0] rather than a
 * private fixture. On a static-record hit it calls lan_ctrl_tx, which
 * needs a real (unconfigured) LAN port + backing mempool to fall through
 * DPDK's dummy tx burst safely -- same pattern as PPP_decode_frame's
 * WAN-side fixture in pppd/codec_test.c.
 */
typedef struct dns_tcp_ctx {
    U8 buf[2048];
    struct rte_ether_hdr *eth_hdr;
    vlan_header_t *vlan_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_tcp_hdr *tcp_hdr;
    U8 *tcp_payload;
    dhcp_ccb_t *dhcp_ccb;
} dns_tcp_ctx_t;

static void dns_tcp_env_init(FastRG_t *fastrg_ccb)
{
    if (direct_pool[0] == NULL) {
        direct_pool[0] = rte_pktmbuf_pool_create("dnsd_tcp_pool", 32, 0, 0,
            RTE_MBUF_DEFAULT_BUF_SIZE, (int)rte_socket_id());
    }
    memset(fastrg_ccb->per_subscriber_stats, 0,
        sizeof(fastrg_ccb->per_subscriber_stats));
    fastrg_rcu_persistent[rte_lcore_id()] = TRUE;
}

static void dns_tcp_ctx_init(dns_tcp_ctx_t *c, FastRG_t *fastrg_ccb)
{
    memset(c, 0, sizeof(*c));

    c->eth_hdr = (struct rte_ether_hdr *)c->buf;
    c->eth_hdr->src_addr = (struct rte_ether_addr){
        .addr_bytes = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
    c->eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    c->vlan_hdr = (vlan_header_t *)(c->eth_hdr + 1);
    c->vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(0x0064);
    c->vlan_hdr->next_proto = rte_cpu_to_be_16(ETH_P_IP);

    c->ip_hdr = (struct rte_ipv4_hdr *)(c->vlan_hdr + 1);
    c->ip_hdr->version_ihl = 0x45;
    c->ip_hdr->time_to_live = 64;
    c->ip_hdr->next_proto_id = IPPROTO_TCP;
    c->ip_hdr->src_addr = rte_cpu_to_be_32(0xC0A80264); /* 192.168.2.100 (LAN client) */
    c->ip_hdr->dst_addr = rte_cpu_to_be_32(0xC0A80201); /* 192.168.2.1 (this node) */

    c->tcp_hdr = (struct rte_tcp_hdr *)(c->ip_hdr + 1);
    c->tcp_hdr->src_port = rte_cpu_to_be_16(53211);
    c->tcp_hdr->dst_port = rte_cpu_to_be_16(53);
    c->tcp_hdr->data_off = 5 << 4; /* 20-byte TCP header, no options */

    c->tcp_payload = (U8 *)(c->tcp_hdr + 1);

    c->dhcp_ccb = fastrg_ccb->dhcp_ccb[0];
    dns_proxy_cleanup(&c->dhcp_ccb->dns_state);
    memset(&c->dhcp_ccb->dns_state, 0, sizeof(c->dhcp_ccb->dns_state));
    dns_proxy_init(&c->dhcp_ccb->dns_state, 0x01010101, 0);
    c->dhcp_ccb->dns_state.dns_proxy_enabled = TRUE;
    c->dhcp_ccb->dhcp_server_ip = rte_cpu_to_be_32(0xC0A80201);
    c->dhcp_ccb->fastrg_ccb = fastrg_ccb;
}

/* Writes a [2-byte length][DNS query] TCP payload sized exactly query_len,
 * and sets ip_hdr->total_length to match. */
static void dns_tcp_ctx_set_query(dns_tcp_ctx_t *c, const U8 *query, U16 query_len)
{
    c->tcp_payload[0] = (U8)(query_len >> 8);
    c->tcp_payload[1] = (U8)(query_len & 0xFF);
    memcpy(c->tcp_payload + 2, query, query_len);

    U16 ip_total = sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr) +
        2 + query_len;
    c->ip_hdr->total_length = rte_cpu_to_be_16(ip_total);
}

static U16 dns_tcp_ctx_frame_len(dns_tcp_ctx_t *c)
{
    return sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        rte_be_to_cpu_16(c->ip_hdr->total_length);
}

/* Builds a minimal single-question DNS query (RD=1) for `domain` into buf. */
static U16 build_dns_query(U8 *buf, const char *domain, U16 qtype, U16 id)
{
    memset(buf, 0, DNS_MAX_PACKET_LEN);
    buf[0] = (U8)(id >> 8);
    buf[1] = (U8)(id & 0xFF);
    buf[2] = 0x01; /* RD=1 */
    buf[3] = 0x00;
    buf[4] = 0x00;
    buf[5] = 0x01; /* QDCOUNT=1 */

    U16 off = 12;
    const char *label = domain;
    while (*label) {
        const char *dot = strchr(label, '.');
        U8 len = dot ? (U8)(dot - label) : (U8)strlen(label);
        buf[off++] = len;
        memcpy(buf + off, label, len);
        off += len;
        label += len;
        if (*label == '.') label++;
    }
    buf[off++] = 0; /* root */
    buf[off++] = (U8)(qtype >> 8);
    buf[off++] = (U8)(qtype & 0xFF);
    buf[off++] = 0;
    buf[off++] = 1; /* QCLASS=IN */
    return off;
}

static void test_tcp_query_proxy_disabled(FastRG_t *fastrg_ccb)
{
    printf("\nTesting dnsd_cp_process_lan_tcp_query (proxy disabled):\n");
    printf("=========================================\n\n");

    dns_tcp_ctx_t c;
    dns_tcp_ctx_init(&c, fastrg_ccb);
    c.dhcp_ccb->dns_state.dns_proxy_enabled = FALSE;

    U8 q[DNS_MAX_PACKET_LEN];
    U16 qlen = build_dns_query(q, "www.fastrg.org", DNS_TYPE_A, 1);
    dns_tcp_ctx_set_query(&c, q, qlen);

    int ret = dnsd_cp_process_lan_tcp_query(fastrg_ccb, c.buf,
        dns_tcp_ctx_frame_len(&c), 0);
    TEST_ASSERT(ret == -1, "disabled proxy returns -1", "got %d", ret);
    dns_proxy_cleanup(&c.dhcp_ccb->dns_state);
}

static void test_tcp_query_payload_too_short(FastRG_t *fastrg_ccb)
{
    printf("\nTesting dnsd_cp_process_lan_tcp_query (TCP payload too short):\n");
    printf("=========================================\n\n");

    dns_tcp_ctx_t c;
    dns_tcp_ctx_init(&c, fastrg_ccb);

    /* 2-byte length prefix + 5 bytes of "DNS" (< DNS_HDR_LEN=12) */
    U8 q[5] = {0};
    dns_tcp_ctx_set_query(&c, q, sizeof(q));

    int ret = dnsd_cp_process_lan_tcp_query(fastrg_ccb, c.buf,
        dns_tcp_ctx_frame_len(&c), 0);
    TEST_ASSERT(ret == -1, "payload shorter than 2+DNS_HDR_LEN returns -1",
        "got %d", ret);
    dns_proxy_cleanup(&c.dhcp_ccb->dns_state);
}

static void test_tcp_query_length_prefix_overrun(FastRG_t *fastrg_ccb)
{
    printf("\nTesting dnsd_cp_process_lan_tcp_query (length prefix overrun):\n");
    printf("=========================================\n\n");

    dns_tcp_ctx_t c;
    dns_tcp_ctx_init(&c, fastrg_ccb);

    U8 q[DNS_MAX_PACKET_LEN];
    U16 qlen = build_dns_query(q, "www.fastrg.org", DNS_TYPE_A, 1);
    dns_tcp_ctx_set_query(&c, q, qlen);
    /* claim more bytes than are actually present in the TCP payload */
    c.tcp_payload[0] = (U8)((qlen + 100) >> 8);
    c.tcp_payload[1] = (U8)((qlen + 100) & 0xFF);

    int ret = dnsd_cp_process_lan_tcp_query(fastrg_ccb, c.buf,
        dns_tcp_ctx_frame_len(&c), 0);
    TEST_ASSERT(ret == -1, "length prefix beyond payload returns -1", "got %d", ret);
    dns_proxy_cleanup(&c.dhcp_ccb->dns_state);
}

static void test_tcp_query_length_prefix_too_small(FastRG_t *fastrg_ccb)
{
    printf("\nTesting dnsd_cp_process_lan_tcp_query (length prefix < DNS_HDR_LEN):\n");
    printf("=========================================\n\n");

    dns_tcp_ctx_t c;
    dns_tcp_ctx_init(&c, fastrg_ccb);

    U8 q[DNS_MAX_PACKET_LEN];
    U16 qlen = build_dns_query(q, "www.fastrg.org", DNS_TYPE_A, 1);
    dns_tcp_ctx_set_query(&c, q, qlen);
    /* length prefix claims fewer bytes than a bare DNS header */
    c.tcp_payload[0] = 0;
    c.tcp_payload[1] = 5;

    int ret = dnsd_cp_process_lan_tcp_query(fastrg_ccb, c.buf,
        dns_tcp_ctx_frame_len(&c), 0);
    TEST_ASSERT(ret == -1, "length prefix under DNS_HDR_LEN returns -1", "got %d", ret);
    dns_proxy_cleanup(&c.dhcp_ccb->dns_state);
}

static void test_tcp_query_malformed_body_rejected(FastRG_t *fastrg_ccb)
{
    printf("\nTesting dnsd_cp_process_lan_tcp_query (malformed DNS body):\n");
    printf("=========================================\n\n");

    dns_tcp_ctx_t c;
    dns_tcp_ctx_init(&c, fastrg_ccb);

    U8 q[DNS_MAX_PACKET_LEN];
    U16 qlen = build_dns_query(q, "www.fastrg.org", DNS_TYPE_A, 1);
    dns_tcp_ctx_set_query(&c, q, qlen);
    /* self-pointing compression pointer in the qname -- dns_parse_query's
     * loop guard must reject it (see dns_codec_test.c's malicious tests) */
    c.tcp_payload[2 + 12] = 0xC0;
    c.tcp_payload[2 + 13] = 0x0C;

    int ret = dnsd_cp_process_lan_tcp_query(fastrg_ccb, c.buf,
        dns_tcp_ctx_frame_len(&c), 0);
    TEST_ASSERT(ret == -1, "unparseable DNS body returns -1", "got %d", ret);
    dns_proxy_cleanup(&c.dhcp_ccb->dns_state);
}

static void test_tcp_query_static_match_builds_response(FastRG_t *fastrg_ccb)
{
    printf("\nTesting dnsd_cp_process_lan_tcp_query (static record match):\n");
    printf("=========================================\n\n");

    dns_tcp_ctx_t c;
    dns_tcp_ctx_init(&c, fastrg_ccb);

    U32 static_ip = rte_cpu_to_be_32(0xC0A80205); /* 192.168.2.5 */
    TEST_ASSERT(dns_static_add(&c.dhcp_ccb->dns_state.static_table,
        "www.fastrg.org", static_ip, 300) == SUCCESS,
        "seed static record", "");

    U8 q[DNS_MAX_PACKET_LEN];
    U16 qlen = build_dns_query(q, "www.fastrg.org", DNS_TYPE_A, 0xBEEF);
    dns_tcp_ctx_set_query(&c, q, qlen);

    struct rte_ether_addr orig_client_mac = c.eth_hdr->src_addr;
    U32 orig_client_ip = c.ip_hdr->src_addr;
    U16 orig_client_port = c.tcp_hdr->src_port;

    int ret = dnsd_cp_process_lan_tcp_query(fastrg_ccb, c.buf,
        dns_tcp_ctx_frame_len(&c), 0);
    TEST_ASSERT(ret == 1, "static match returns 1 (responded in place)", "got %d", ret);

    U16 resp_len = (U16)((c.tcp_payload[0] << 8) | c.tcp_payload[1]);
    TEST_ASSERT(resp_len == qlen + 16,
        "response length is query + 16-byte A answer RR",
        "expect %u got %u", qlen + 16, resp_len);

    U8 *resp = c.tcp_payload + 2;
    TEST_ASSERT(resp[6] == 0 && resp[7] == 1, "ANCOUNT is 1",
        "got %u", (resp[6] << 8) | resp[7]);
    TEST_ASSERT(memcmp(resp + resp_len - 4, &static_ip, 4) == 0,
        "answer RDATA carries the static record's IP", "");
    TEST_ASSERT(resp[0] == 0xBE && resp[1] == 0xEF,
        "response DNS ID echoes the query ID", "got 0x%02x%02x", resp[0], resp[1]);

    TEST_ASSERT(rte_is_same_ether_addr(&c.eth_hdr->dst_addr, &orig_client_mac),
        "Ethernet dst is the original client MAC", "");
    TEST_ASSERT(rte_is_same_ether_addr(&c.eth_hdr->src_addr,
        &fastrg_ccb->nic_info.hsi_lan_mac),
        "Ethernet src is this node's LAN MAC", "");
    TEST_ASSERT(c.ip_hdr->src_addr == c.dhcp_ccb->dhcp_server_ip,
        "IP src is the DHCP server (this node) address", "");
    TEST_ASSERT(c.ip_hdr->dst_addr == orig_client_ip,
        "IP dst is the original client address", "");
    TEST_ASSERT(c.tcp_hdr->src_port == rte_cpu_to_be_16(53),
        "TCP src port is 53", "got %u", rte_be_to_cpu_16(c.tcp_hdr->src_port));
    TEST_ASSERT(c.tcp_hdr->dst_port == orig_client_port,
        "TCP dst port is the original client port", "");
    TEST_ASSERT(rte_be_to_cpu_16(c.ip_hdr->total_length) ==
        sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr) + 2 + resp_len,
        "IP total_length covers IP + TCP + length-prefixed response", "");

    dns_proxy_cleanup(&c.dhcp_ccb->dns_state);
}

static void test_tcp_query_qtype_mismatch_no_fallback(FastRG_t *fastrg_ccb)
{
    printf("\nTesting dnsd_cp_process_lan_tcp_query (AAAA query, static table has A only):\n");
    printf("=========================================\n\n");

    dns_tcp_ctx_t c;
    dns_tcp_ctx_init(&c, fastrg_ccb);

    U32 static_ip = rte_cpu_to_be_32(0xC0A80205);
    dns_static_add(&c.dhcp_ccb->dns_state.static_table, "www.fastrg.org",
        static_ip, 300);

    U8 q[DNS_MAX_PACKET_LEN];
    U16 qlen = build_dns_query(q, "www.fastrg.org", DNS_TYPE_AAAA, 1);
    dns_tcp_ctx_set_query(&c, q, qlen);

    int ret = dnsd_cp_process_lan_tcp_query(fastrg_ccb, c.buf,
        dns_tcp_ctx_frame_len(&c), 0);
    TEST_ASSERT(ret == -1,
        "AAAA query against an A-only static record returns -1 (no cache/upstream on TCP)",
        "got %d", ret);
    dns_proxy_cleanup(&c.dhcp_ccb->dns_state);
}

static void test_tcp_query_no_static_record_no_fallback(FastRG_t *fastrg_ccb)
{
    printf("\nTesting dnsd_cp_process_lan_tcp_query (no static record, no cache/upstream):\n");
    printf("=========================================\n\n");

    dns_tcp_ctx_t c;
    dns_tcp_ctx_init(&c, fastrg_ccb);

    U8 q[DNS_MAX_PACKET_LEN];
    U16 qlen = build_dns_query(q, "unmatched.example.com", DNS_TYPE_A, 1);
    dns_tcp_ctx_set_query(&c, q, qlen);

    int ret = dnsd_cp_process_lan_tcp_query(fastrg_ccb, c.buf,
        dns_tcp_ctx_frame_len(&c), 0);
    TEST_ASSERT(ret == -1,
        "no static match returns -1: TCP path has no cache/upstream fallback",
        "got %d", ret);
    dns_proxy_cleanup(&c.dhcp_ccb->dns_state);
}

void test_dnsd(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           DNS Daemon Unit Tests                             ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    /* dnsd_check_failover tests */
    test_failover_disabled();
    test_failover_no_secondary();
    test_failover_timeout_not_reached();
    test_failover_primary_to_secondary();

    /* find_free_pending tests */
    test_find_free_pending_all_inactive();
    test_find_free_pending_skips_active();
    test_find_free_pending_expired();
    test_find_free_pending_full();

    /* find_pending_by_upstream_id tests */
    test_find_pending_by_id_hit();
    test_find_pending_by_id_miss();
    test_find_pending_by_id_expired();
    test_find_pending_by_id_inactive();

    /* dnsd_cp_process_lan_tcp_query tests */
    dns_tcp_env_init(fastrg_ccb);
    test_tcp_query_proxy_disabled(fastrg_ccb);
    test_tcp_query_payload_too_short(fastrg_ccb);
    test_tcp_query_length_prefix_overrun(fastrg_ccb);
    test_tcp_query_length_prefix_too_small(fastrg_ccb);
    test_tcp_query_malformed_body_rejected(fastrg_ccb);
    test_tcp_query_static_match_builds_response(fastrg_ccb);
    test_tcp_query_qtype_mismatch_no_fallback(fastrg_ccb);
    test_tcp_query_no_static_record_no_fallback(fastrg_ccb);

    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║  Test Summary                                              ║\n");
    printf("╠════════════════════════════════════════════════════════════╣\n");
    printf("║  Total Tests:  %3d                                         ║\n", test_count);
    printf("║  Passed:       %3d                                         ║\n", pass_count);
    printf("║  Failed:       %3d                                         ║\n", test_count - pass_count);
    printf("║  Success Rate: %3d%%                                        ║\n", 
           test_count > 0 ? (pass_count * 100 / test_count) : 0);
    printf("╚════════════════════════════════════════════════════════════╝\n");

    if (pass_count == test_count) {
        printf("\n✓ All tests passed!\n");
    } else {
        printf("\n✗ Some tests failed!\n");
    }

    *total_tests += test_count;
    *total_pass += pass_count;
}
