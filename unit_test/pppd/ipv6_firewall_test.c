#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <common.h>

#include <rte_atomic.h>
#include <rte_rcu_qsbr.h>

#include "../../src/fastrg.h"
#include "../../src/pppd/ipv6_firewall.h"
#include "../../src/protocol.h"
#include "../test_helper.h"

static int test_count = 0;
static int pass_count = 0;

/* Mock subscriber ccb owning the firewall pool, hash and free-list.
 * Static (BSS) — a few tens of MB, fine for the test binary. */
static ppp_ccb_t test_ccb;
static struct rte_rcu_qsbr *test_rcu;

/* Two scratch frames: the packet under test, plus the original an ICMPv6
 * error message quotes inside it. */
static U8 outer_buf[512] __rte_cache_aligned;
static U8 inner_buf[512] __rte_cache_aligned;

static const U8 lan_host[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0x64, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};
static const U8 lan_host2[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0x64, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11,
};
static const U8 remote_host[16] = {
    0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x11,
};
static const U8 other_remote[16] = {
    0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0x22,
};
static const U8 path_router[16] = {
    0x2a, 0x00, 0x14, 0x50, 0x40, 0x01, 0x08, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

/* One-time environment: QSBR var with this thread registered as reader 0,
 * then the per-subscriber hash + ring (requires rte_eal_init in test.c). */
static void fw_env_init_once(void)
{
    if (test_rcu != NULL)
        return;
    size_t sz = rte_rcu_qsbr_get_memsize(1);
    test_rcu = calloc(1, sz);
    assert(test_rcu != NULL && rte_rcu_qsbr_init(test_rcu, 1) == 0);
    rte_rcu_qsbr_thread_register(test_rcu, 0);
    rte_rcu_qsbr_thread_online(test_rcu, 0);

    memset(&test_ccb, 0, sizeof(test_ccb));
    rte_spinlock_init(&test_ccb.ipv6_firewall_insert_lock);
    assert(ipv6_firewall_table_init(&test_ccb, 60003, test_rcu) == SUCCESS);
}

static void fw_env_reset(void)
{
    fw_env_init_once();
    /* The test thread is reader 0 and holds no entry pointers here; report
     * quiescent so the reset can drain leftover deferred frees from the
     * previous test (its drain loop would otherwise wait on us). */
    rte_rcu_qsbr_quiescent(test_rcu, 0);
    ipv6_firewall_table_reset(&test_ccb);
    test_ccb.tcp_conntrack_enabled = TRUE;
}

/* Report reader-0 quiescent and drain the RCU defer queue so deleted keys'
 * slots actually return to the free ring (what the data plane gets from
 * per-burst quiescent reporting). */
static void fw_quiesce_reclaim(ppp_ccb_t *ccb)
{
    unsigned int freed = 0, pending = 0, avail = 0;

    rte_rcu_qsbr_quiescent(test_rcu, 0);
    rte_hash_rcu_qsbr_dq_reclaim(ccb->ipv6_firewall_hash, &freed, &pending, &avail);
}

static int expire_at_approx(U64 before_cycles, U64 expire_at, U64 expected_secs)
{
    U64 hz = fastrg_get_cycles_in_sec();
    U64 tolerance = hz / 1000; /* 1 ms tolerance */
    U64 expected = expected_secs * hz;

    return expire_at >= before_cycles + expected &&
           expire_at <= before_cycles + expected + tolerance;
}

/*--------- PACKET BUILDERS ----------*/

static struct rte_ipv6_hdr *build_ip6(U8 *buf, const U8 *src, const U8 *dst,
    U8 proto, U16 payload_len)
{
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)buf;

    memset(buf, 0, 512);
    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(6) << 28);
    ip6->payload_len = rte_cpu_to_be_16(payload_len);
    ip6->proto = proto;
    ip6->hop_limits = 64;
    memcpy(&ip6->src_addr, src, 16);
    memcpy(&ip6->dst_addr, dst, 16);
    return ip6;
}

static struct rte_ipv6_hdr *build_tcp6(U8 *buf, const U8 *src, const U8 *dst,
    U16 sport, U16 dport, U8 flags, U32 seq, U32 ack)
{
    struct rte_ipv6_hdr *ip6 = build_ip6(buf, src, dst, IPPROTO_TCP,
        sizeof(struct rte_tcp_hdr));
    struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip6 + 1);

    tcp->src_port = rte_cpu_to_be_16(sport);
    tcp->dst_port = rte_cpu_to_be_16(dport);
    tcp->sent_seq = rte_cpu_to_be_32(seq);
    tcp->recv_ack = rte_cpu_to_be_32(ack);
    tcp->data_off = 5 << 4;
    tcp->tcp_flags = flags;
    tcp->rx_win = rte_cpu_to_be_16(64240);
    return ip6;
}

static struct rte_ipv6_hdr *build_udp6(U8 *buf, const U8 *src, const U8 *dst,
    U16 sport, U16 dport)
{
    struct rte_ipv6_hdr *ip6 = build_ip6(buf, src, dst, IPPROTO_UDP,
        sizeof(struct rte_udp_hdr) + 4);
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip6 + 1);

    udp->src_port = rte_cpu_to_be_16(sport);
    udp->dst_port = rte_cpu_to_be_16(dport);
    udp->dgram_len = rte_cpu_to_be_16(sizeof(*udp) + 4);
    return ip6;
}

static struct rte_ipv6_hdr *build_icmp6(U8 *buf, const U8 *src, const U8 *dst,
    U8 type, U16 id)
{
    struct rte_ipv6_hdr *ip6 = build_ip6(buf, src, dst, IPPROTO_ICMPV6,
        ICMP6_PTB_HDR_LEN);
    U8 *icmp6 = (U8 *)(ip6 + 1);
    U16 be_id = rte_cpu_to_be_16(id);

    icmp6[0] = type;
    memcpy(icmp6 + 4, &be_id, sizeof(be_id));
    return ip6;
}

/* An ICMPv6 error message of `type` quoting `quoted_len` bytes of the packet
 * already built in inner_buf. */
static struct rte_ipv6_hdr *build_icmp6_error(U8 *buf, const U8 *src,
    const U8 *dst, U8 type, U16 quoted_len)
{
    struct rte_ipv6_hdr *ip6 = build_ip6(buf, src, dst, IPPROTO_ICMPV6,
        (U16)(ICMP6_PTB_HDR_LEN + quoted_len));
    U8 *icmp6 = (U8 *)(ip6 + 1);

    icmp6[0] = type;
    memcpy(icmp6 + ICMP6_PTB_HDR_LEN, inner_buf, quoted_len);
    return ip6;
}

/* Bytes an ICMPv6 error quotes when it carries a whole IPv6 header plus the
 * first 8 bytes of the L4 header — the minimum a sender must include. */
#define QUOTE_MIN_LEN ((U16)(sizeof(struct rte_ipv6_hdr) + ICMP6_PTB_HDR_LEN))

/* An ICMPv6 message of exactly payload_len bytes, too short to quote anything.
 * The type byte is written only when there is room for it, so payload_len 0
 * really is a packet with no readable type. */
static struct rte_ipv6_hdr *build_icmp6_truncated(U8 *buf, const U8 *src,
    const U8 *dst, U8 type, U16 payload_len)
{
    struct rte_ipv6_hdr *ip6 = build_ip6(buf, src, dst, IPPROTO_ICMPV6,
        payload_len);

    if (payload_len > 0)
        *(U8 *)(ip6 + 1) = type;
    return ip6;
}

/*--------- LOOKUP HELPERS FOR ASSERTIONS ----------*/

static ipv6_firewall_entry_t *fw_find(ppp_ccb_t *ccb, const struct rte_ipv6_hdr *ip6,
    BOOL inbound, U32 *idx)
{
    ipv6_firewall_key_t key;

    if (ipv6_firewall_key_from_packet(ip6, rte_be_to_cpu_16(ip6->payload_len),
            inbound, &key) == FALSE)
        return NULL;
    return ipv6_firewall_lookup(ccb, &key, idx);
}

static U64 fw_deadline(ppp_ccb_t *ccb, U32 idx)
{
    return __atomic_load_n(&ccb->ipv6_firewall_expire_at[idx], __ATOMIC_RELAXED);
}

/**
 * Test 1: the key is normalized, so a reply matches the session its request
 * opened.  This is what the explicit zeroing in ipv6_firewall_key_build() buys: a
 * key built over stack garbage would hash differently per direction and every
 * reply would miss.
 */
static void test_key_normalization(void)
{
    struct rte_ipv6_hdr *out6, *in6;
    ipv6_firewall_key_t out_key, in_key;
    U32 idx = 0;

    printf("\nTesting IPv6 firewall key normalization:\n");
    printf("========================================\n\n");

    fw_env_reset();
    out6 = build_tcp6(outer_buf, lan_host, remote_host, 40000, 443,
        RTE_TCP_SYN_FLAG, 0x1000, 0);
    TEST_ASSERT(ipv6_firewall_key_from_packet(out6, sizeof(struct rte_tcp_hdr), FALSE,
            &out_key) == TRUE,
        "outbound TCP packet yields a key", NULL);

    in6 = build_tcp6(inner_buf, remote_host, lan_host, 443, 40000,
        RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, 0x9000, 0x1001);
    TEST_ASSERT(ipv6_firewall_key_from_packet(in6, sizeof(struct rte_tcp_hdr), TRUE,
            &in_key) == TRUE,
        "inbound TCP packet yields a key", NULL);
    TEST_ASSERT(memcmp(&out_key, &in_key, sizeof(out_key)) == 0,
        "both directions of one flow produce identical key bytes", NULL);

    ipv6_firewall_learn(&test_ccb, out6);
    TEST_ASSERT(rte_hash_count(test_ccb.ipv6_firewall_hash) == 1,
        "outbound packet opens exactly one session", NULL);
    TEST_ASSERT(rte_ring_count(test_ccb.ipv6_firewall_free_ring) == IPV6_FIREWALL_MAX_ENTRIES - 1,
        "exactly one pool slot consumed", NULL);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, in6) == TRUE,
        "the reply of that flow is allowed in", NULL);
    TEST_ASSERT(fw_find(&test_ccb, in6, TRUE, &idx) != NULL,
        "the reply resolves to the same session", NULL);
}

/**
 * Test 2: default deny — anything the LAN did not ask for is dropped, and a
 * session matches only its exact tuple.
 */
static void test_unsolicited_inbound_dropped(void)
{
    struct rte_ipv6_hdr *ip6;

    printf("\nTesting unsolicited inbound traffic:\n");
    printf("====================================\n\n");

    fw_env_reset();
    ip6 = build_tcp6(outer_buf, remote_host, lan_host, 443, 40000,
        RTE_TCP_SYN_FLAG, 0x1000, 0);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "inbound TCP without a session is dropped", NULL);

    ip6 = build_udp6(outer_buf, remote_host, lan_host, 53, 40000);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "inbound UDP without a session is dropped", NULL);

    /* Open one UDP session and probe every way of getting its tuple wrong. */
    ip6 = build_udp6(outer_buf, lan_host, remote_host, 40000, 53);
    ipv6_firewall_learn(&test_ccb, ip6);

    ip6 = build_udp6(outer_buf, remote_host, lan_host, 53, 40000);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == TRUE,
        "the exact reply tuple is allowed in", NULL);

    ip6 = build_udp6(outer_buf, remote_host, lan_host, 54, 40000);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "a different remote port misses the session", NULL);

    ip6 = build_udp6(outer_buf, other_remote, lan_host, 53, 40000);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "a different remote address misses the session", NULL);

    ip6 = build_udp6(outer_buf, remote_host, lan_host2, 53, 40000);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "a different LAN host misses the session", NULL);

    ip6 = build_tcp6(outer_buf, remote_host, lan_host, 53, 40000,
        RTE_TCP_ACK_FLAG, 0x1000, 0);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "the same ports over a different protocol miss the session", NULL);
}

/**
 * Test 3: ICMPv6 echo is tracked by identifier, and only in its natural
 * direction.
 */
static void test_icmp6_echo_session(void)
{
    struct rte_ipv6_hdr *ip6;

    printf("\nTesting ICMPv6 echo sessions:\n");
    printf("=============================\n\n");

    fw_env_reset();
    ip6 = build_icmp6(outer_buf, lan_host, remote_host, ICMP6_ECHO_REQUEST, 0x4242);
    ipv6_firewall_learn(&test_ccb, ip6);
    TEST_ASSERT(rte_hash_count(test_ccb.ipv6_firewall_hash) == 1,
        "an outbound echo request opens a session", NULL);

    ip6 = build_icmp6(outer_buf, remote_host, lan_host, ICMP6_ECHO_REPLY, 0x4242);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == TRUE,
        "the echo reply carrying that identifier is allowed in", NULL);

    ip6 = build_icmp6(outer_buf, remote_host, lan_host, ICMP6_ECHO_REPLY, 0x4243);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "an echo reply with another identifier is dropped", NULL);

    ip6 = build_icmp6(outer_buf, remote_host, lan_host, ICMP6_ECHO_REQUEST, 0x4242);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "an inbound echo request is dropped even with a matching identifier", NULL);
    TEST_ASSERT(rte_hash_count(test_ccb.ipv6_firewall_hash) == 1,
        "none of the rejected packets opened a session", NULL);
}

/**
 * Test 4: an inbound ICMPv6 error message is judged by the packet it quotes,
 * and each verdict lands on its own counter.
 */
static void test_icmp6_error_validation(void)
{
    struct rte_ipv6_hdr *ip6;
    U64 passed_before, dropped_before, deadline_before;
    U32 idx = 0;

    printf("\nTesting inbound ICMPv6 error validation:\n");
    printf("========================================\n\n");

    fw_env_reset();
    /* A live UDP session for the errors to quote. */
    ip6 = build_udp6(outer_buf, lan_host, remote_host, 40000, 53);
    ipv6_firewall_learn(&test_ccb, ip6);
    TEST_ASSERT(fw_find(&test_ccb, ip6, FALSE, &idx) != NULL,
        "the quoted flow has a session to match", NULL);
    deadline_before = fw_deadline(&test_ccb, idx);

    passed_before = test_ccb.ipv6_firewall_icmp6_err_passed;
    dropped_before = test_ccb.ipv6_firewall_icmp6_err_dropped;

    build_udp6(inner_buf, lan_host, remote_host, 40000, 53);
    ip6 = build_icmp6_error(outer_buf, path_router, lan_host,
        ICMP6_PACKET_TOO_BIG, QUOTE_MIN_LEN);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == TRUE,
        "Packet Too Big quoting a live session is allowed in", NULL);
    TEST_ASSERT(test_ccb.ipv6_firewall_icmp6_err_passed == passed_before + 1,
        "the pass counter advances", "passed=%" PRIu64,
        test_ccb.ipv6_firewall_icmp6_err_passed);
    TEST_ASSERT(fw_deadline(&test_ccb, idx) == deadline_before,
        "letting the error through does not extend the session", NULL);

    dropped_before = test_ccb.ipv6_firewall_icmp6_err_dropped;
    build_udp6(inner_buf, lan_host, remote_host, 40001, 53);
    ip6 = build_icmp6_error(outer_buf, path_router, lan_host,
        ICMP6_PACKET_TOO_BIG, QUOTE_MIN_LEN);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "an error quoting a tuple with no session is dropped", NULL);
    TEST_ASSERT(test_ccb.ipv6_firewall_icmp6_err_dropped == dropped_before + 1,
        "the drop counter advances", "dropped=%" PRIu64,
        test_ccb.ipv6_firewall_icmp6_err_dropped);

    dropped_before = test_ccb.ipv6_firewall_icmp6_err_dropped;
    build_udp6(inner_buf, lan_host, remote_host, 40000, 53);
    ip6 = build_icmp6_error(outer_buf, path_router, lan_host,
        ICMP6_PACKET_TOO_BIG, (U16)(QUOTE_MIN_LEN - 1));
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "an error quoting too few bytes to check is dropped", NULL);
    TEST_ASSERT(test_ccb.ipv6_firewall_icmp6_err_dropped == dropped_before + 1,
        "the truncated error is counted as dropped", NULL);

    /* The quoted source has to be the host being notified; here it is not. */
    build_udp6(inner_buf, lan_host2, remote_host, 40000, 53);
    ip6 = build_icmp6_error(outer_buf, path_router, lan_host,
        ICMP6_TIME_EXCEEDED, QUOTE_MIN_LEN);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "an error whose quoted source is not the addressee is dropped", NULL);

    /* An error about an outbound echo request must reach the pinging host. */
    ip6 = build_icmp6(outer_buf, lan_host, remote_host, ICMP6_ECHO_REQUEST, 0x7777);
    ipv6_firewall_learn(&test_ccb, ip6);
    build_icmp6(inner_buf, lan_host, remote_host, ICMP6_ECHO_REQUEST, 0x7777);
    ip6 = build_icmp6_error(outer_buf, path_router, lan_host,
        ICMP6_DST_UNREACH, QUOTE_MIN_LEN);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == TRUE,
        "Destination Unreachable quoting a live echo session is allowed in", NULL);

    /* Informational types get no such treatment: they need a session of their
     * own, and only the echo reply can ever have one. */
    ip6 = build_icmp6(outer_buf, remote_host, lan_host, 133, 0x7777);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "an inbound router solicitation is dropped", NULL);
    ip6 = build_icmp6(outer_buf, remote_host, lan_host, 128, 0x7777);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "an inbound echo request is dropped", NULL);
}

/**
 * Test 5: TCP sessions age on the conntrack per-state timeouts, not on the
 * flat 10 second idle timeout UDP uses.
 */
static void test_tcp_per_state_timeout(void)
{
    struct rte_ipv6_hdr *ip6;
    ipv6_firewall_entry_t *entry;
    U64 before;
    U32 idx = 0;

    printf("\nTesting TCP per-state session timeouts:\n");
    printf("=======================================\n\n");

    fw_env_reset();
    before = fastrg_get_cur_cycles();
    ip6 = build_tcp6(outer_buf, lan_host, remote_host, 40100, 443,
        RTE_TCP_SYN_FLAG, 0x1000, 0);
    ipv6_firewall_learn(&test_ccb, ip6);
    entry = fw_find(&test_ccb, ip6, FALSE, &idx);
    TEST_ASSERT(entry != NULL && entry->tcp_state == TCP_CONNTRACK_SYN_SENT,
        "an outbound SYN opens the session in SYN_SENT", NULL);
    TEST_ASSERT(expire_at_approx(before, fw_deadline(&test_ccb, idx),
            TCP_TIMEOUT_SYN_SENT),
        "SYN_SENT arms the 30 second deadline", NULL);

    ip6 = build_tcp6(outer_buf, remote_host, lan_host, 443, 40100,
        RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, 0x9000, 0x1001);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == TRUE &&
            entry->tcp_state == TCP_CONNTRACK_SYN_RECV,
        "the SYN-ACK is allowed in and moves the session to SYN_RECV", NULL);

    before = fastrg_get_cur_cycles();
    ip6 = build_tcp6(outer_buf, lan_host, remote_host, 40100, 443,
        RTE_TCP_ACK_FLAG, 0x1001, 0x9001);
    ipv6_firewall_learn(&test_ccb, ip6);
    TEST_ASSERT(entry->tcp_state == TCP_CONNTRACK_ESTABLISHED,
        "the final ACK establishes the session", NULL);
    TEST_ASSERT(expire_at_approx(before, fw_deadline(&test_ccb, idx),
            TCP_TIMEOUT_ESTABLISHED),
        "ESTABLISHED arms the 7200 second deadline", NULL);

    /* Closing must shorten the deadline, which only an unconditional set can
     * do — a coalesced refresh never moves it backwards. */
    before = fastrg_get_cur_cycles();
    ip6 = build_tcp6(outer_buf, lan_host, remote_host, 40100, 443,
        RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG, 0x1001, 0x9001);
    ipv6_firewall_learn(&test_ccb, ip6);
    TEST_ASSERT(entry->tcp_state == TCP_CONNTRACK_FIN_WAIT,
        "an outbound FIN moves the session to FIN_WAIT", NULL);
    TEST_ASSERT(expire_at_approx(before, fw_deadline(&test_ccb, idx),
            TCP_TIMEOUT_FIN_WAIT),
        "FIN_WAIT shortens the deadline to 120 seconds", NULL);

    before = fastrg_get_cur_cycles();
    ip6 = build_tcp6(outer_buf, lan_host, remote_host, 40100, 443,
        RTE_TCP_RST_FLAG, 0x1001, 0x9001);
    ipv6_firewall_learn(&test_ccb, ip6);
    TEST_ASSERT(entry->tcp_state == TCP_CONNTRACK_CLOSE,
        "a RST closes the session", NULL);
    TEST_ASSERT(expire_at_approx(before, fw_deadline(&test_ccb, idx),
            TCP_TIMEOUT_CLOSE),
        "CLOSE shortens the deadline to 10 seconds", NULL);
}

/* Drive a session all the way to ESTABLISHED and report its entry. */
static ipv6_firewall_entry_t *establish_tcp(U16 lan_port, U32 *idx)
{
    struct rte_ipv6_hdr *ip6;

    ip6 = build_tcp6(outer_buf, lan_host, remote_host, lan_port, 443,
        RTE_TCP_SYN_FLAG, 0x1000, 0);
    ipv6_firewall_learn(&test_ccb, ip6);
    ip6 = build_tcp6(outer_buf, remote_host, lan_host, 443, lan_port,
        RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, 0x9000, 0x1001);
    ipv6_firewall_inbound_pass(&test_ccb, ip6);
    ip6 = build_tcp6(outer_buf, lan_host, remote_host, lan_port, 443,
        RTE_TCP_ACK_FLAG, 0x1001, 0x9001);
    ipv6_firewall_learn(&test_ccb, ip6);
    return fw_find(&test_ccb, ip6, FALSE, idx);
}

/**
 * Test 6: inbound TCP flags must fit the tracked state, and
 * tcp_conntrack_enabled decides enforcement exactly as it does for IPv4.
 */
static void test_tcp_inbound_state_check(void)
{
    struct rte_ipv6_hdr *ip6;
    ipv6_firewall_entry_t *entry;
    U32 idx = 0;
    U32 seq_end_before;

    printf("\nTesting inbound TCP state enforcement:\n");
    printf("======================================\n\n");

    fw_env_reset();
    entry = establish_tcp(40200, &idx);
    TEST_ASSERT(entry != NULL && entry->tcp_state == TCP_CONNTRACK_ESTABLISHED,
        "session established for the state check", NULL);

    ip6 = build_tcp6(outer_buf, remote_host, lan_host, 443, 40200,
        RTE_TCP_SYN_FLAG, 0x1234, 0);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "a bare SYN against an ESTABLISHED session is dropped", NULL);
    TEST_ASSERT(entry->tcp_state == TCP_CONNTRACK_ESTABLISHED,
        "the rejected packet leaves the state alone", NULL);

    test_ccb.tcp_conntrack_enabled = FALSE;
    seq_end_before = entry->max_seq_end_wan;
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == TRUE,
        "with conntrack disabled the same SYN is forwarded", NULL);
    TEST_ASSERT(entry->tcp_state == TCP_CONNTRACK_ESTABLISHED &&
            entry->max_seq_end_wan == seq_end_before,
        "an unenforced packet updates neither the state nor the sequence baseline",
        NULL);
    test_ccb.tcp_conntrack_enabled = TRUE;
}

/**
 * Test 7: the shared sequence window check runs for IPv6 too, so blind
 * injection into a known tuple is still rejected.
 */
static void test_tcp_inbound_seq_check(void)
{
    struct rte_ipv6_hdr *ip6;
    ipv6_firewall_entry_t *entry;
    U32 idx = 0;
    U32 ack_before;

    printf("\nTesting inbound TCP sequence validation:\n");
    printf("========================================\n\n");

    fw_env_reset();
    entry = establish_tcp(40300, &idx);
    TEST_ASSERT(entry != NULL && entry->max_seq_end_lan != 0,
        "the LAN-side baseline is seeded by the outbound packets", NULL);

    ip6 = build_tcp6(outer_buf, remote_host, lan_host, 443, 40300,
        RTE_TCP_ACK_FLAG, 0x9001, 0x1001);
    ack_before = entry->max_ack_wan;
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == TRUE,
        "an in-window inbound ACK is allowed in", NULL);
    TEST_ASSERT(entry->max_ack_wan != ack_before || entry->max_seq_end_wan != 0,
        "the accepted packet advances the WAN-side baseline", NULL);

    /* Half the sequence space away: far outside the tolerated window. */
    ip6 = build_tcp6(outer_buf, remote_host, lan_host, 443, 40300,
        RTE_TCP_ACK_FLAG, 0x9001 + 0x40000000, 0x1001);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE,
        "an out-of-window inbound sequence is dropped", NULL);

    test_ccb.tcp_conntrack_enabled = FALSE;
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == TRUE,
        "with conntrack disabled the same packet is forwarded", NULL);
    test_ccb.tcp_conntrack_enabled = TRUE;
}

/**
 * Test 8: UDP and echo sessions live on a 10 second idle timeout; traffic in
 * either direction renews them, but only the LAN side may revive an expired
 * one.
 */
static void test_udp_idle_expiry(void)
{
    struct rte_ipv6_hdr *out6, *in6;
    U64 before;
    U32 idx = 0;

    printf("\nTesting UDP session expiry and revival:\n");
    printf("=======================================\n\n");

    fw_env_reset();
    before = fastrg_get_cur_cycles();
    out6 = build_udp6(outer_buf, lan_host, remote_host, 40400, 53);
    ipv6_firewall_learn(&test_ccb, out6);
    TEST_ASSERT(fw_find(&test_ccb, out6, FALSE, &idx) != NULL &&
            expire_at_approx(before, fw_deadline(&test_ccb, idx),
                IPV6_FIREWALL_ENTRY_TIMEOUT_SEC),
        "a new UDP session gets the 10 second deadline", NULL);

    /* Wind the deadline back past the coalescing threshold so the inbound
     * refresh has to write. */
    nat_expire_set(&test_ccb.ipv6_firewall_expire_at[idx],
        fw_deadline(&test_ccb, idx) - 2 * fastrg_get_cycles_in_sec());
    before = fw_deadline(&test_ccb, idx);
    in6 = build_udp6(inner_buf, remote_host, lan_host, 53, 40400);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, in6) == TRUE &&
            fw_deadline(&test_ccb, idx) > before,
        "an inbound packet on a live session pushes the deadline out", NULL);

    /* Expire it, then knock from the WAN: the session must not come back. */
    nat_expire_set(&test_ccb.ipv6_firewall_expire_at[idx], fastrg_get_cur_cycles() - 1);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, in6) == FALSE,
        "an expired session does not let inbound traffic through", NULL);
    TEST_ASSERT(fw_find(&test_ccb, in6, TRUE, &idx) == NULL,
        "the expired session is unlinked by the inbound miss", NULL);
    /* The key is unlinked immediately, but the slot only comes back once
     * every reader has passed a quiescent state. */
    fw_quiesce_reclaim(&test_ccb);
    TEST_ASSERT(rte_hash_count(test_ccb.ipv6_firewall_hash) == 0 &&
            rte_ring_count(test_ccb.ipv6_firewall_free_ring) == IPV6_FIREWALL_MAX_ENTRIES,
        "its slot returns to the free ring after readers go quiescent",
        "hash=%u ring=%u", rte_hash_count(test_ccb.ipv6_firewall_hash),
        rte_ring_count(test_ccb.ipv6_firewall_free_ring));

    /* The LAN side is trusted and simply opens the session again. */
    ipv6_firewall_learn(&test_ccb, out6);
    TEST_ASSERT(rte_hash_count(test_ccb.ipv6_firewall_hash) == 1 &&
            ipv6_firewall_inbound_pass(&test_ccb, in6) == TRUE,
        "outbound traffic reopens the session and the reply flows again", NULL);
}

/**
 * Test 9: the recency stamp is written at most once per session per second,
 * so per-packet traffic does not keep dirtying the shared array.
 */
static void test_recency_coalescing(void)
{
    struct rte_ipv6_hdr *out6;
    U64 stamp_after_learn, stamp_after_hit;
    U32 idx = 0;

    printf("\nTesting LRU recency write coalescing:\n");
    printf("=====================================\n\n");

    fw_env_reset();
    out6 = build_udp6(outer_buf, lan_host, remote_host, 40500, 53);
    ipv6_firewall_learn(&test_ccb, out6);
    TEST_ASSERT(fw_find(&test_ccb, out6, FALSE, &idx) != NULL,
        "session opened for the recency check", NULL);
    stamp_after_learn = __atomic_load_n(&test_ccb.ipv6_firewall_last_used[idx],
        __ATOMIC_RELAXED);
    TEST_ASSERT(stamp_after_learn != 0,
        "a live session always carries a non-zero recency stamp", NULL);

    ipv6_firewall_learn(&test_ccb, out6);
    TEST_ASSERT(__atomic_load_n(&test_ccb.ipv6_firewall_last_used[idx],
            __ATOMIC_RELAXED) == stamp_after_learn,
        "a hit within the same second leaves the stamp untouched", NULL);

    /* Age the stamp by two seconds; the next hit must refresh it. */
    __atomic_store_n(&test_ccb.ipv6_firewall_last_used[idx],
        stamp_after_learn - 2 * fastrg_get_cycles_in_sec(), __ATOMIC_RELAXED);
    ipv6_firewall_learn(&test_ccb, out6);
    stamp_after_hit = __atomic_load_n(&test_ccb.ipv6_firewall_last_used[idx],
        __ATOMIC_RELAXED);
    TEST_ASSERT(stamp_after_hit >= stamp_after_learn,
        "a hit more than a second later writes a fresh stamp",
        "stamp=%" PRIu64 " learn=%" PRIu64, stamp_after_hit, stamp_after_learn);
}

/**
 * Test 11: an ICMPv6 error message too short to check is rejected, and lands
 * on the error counter rather than disappearing into the general drop — the
 * counter is the only way to tell "PMTUD notices are being refused" apart
 * from ordinary unsolicited traffic.
 */
static void test_icmp6_error_truncated_counted(void)
{
    struct rte_ipv6_hdr *ip6;
    U64 dropped_before;
    U16 len;

    printf("\nTesting truncated inbound ICMPv6 error accounting:\n");
    printf("=================================================\n\n");

    fw_env_reset();
    /* A live session, so nothing here is rejected merely for lack of one. */
    ip6 = build_udp6(outer_buf, lan_host, remote_host, 40700, 53);
    ipv6_firewall_learn(&test_ccb, ip6);

    /* Every length between "just the type byte" and "one byte short of
     * checkable" must be rejected and counted. */
    for(len=1; len<=QUOTE_MIN_LEN + ICMP6_PTB_HDR_LEN - 1; len++) {
        dropped_before = test_ccb.ipv6_firewall_icmp6_err_dropped;
        ip6 = build_icmp6_truncated(outer_buf, path_router, lan_host,
            ICMP6_PACKET_TOO_BIG, len);
        if (ipv6_firewall_inbound_pass(&test_ccb, ip6) != FALSE ||
                test_ccb.ipv6_firewall_icmp6_err_dropped != dropped_before + 1)
            break;
    }
    TEST_ASSERT(len == QUOTE_MIN_LEN + ICMP6_PTB_HDR_LEN,
        "every truncated ICMPv6 error is rejected and counted",
        "first length not counted: %u", len);

    /* A message with no payload at all carries no type byte to read. It must
     * not be treated as an error message — reading that byte would be an
     * out-of-bounds read. */
    dropped_before = test_ccb.ipv6_firewall_icmp6_err_dropped;
    ip6 = build_icmp6_truncated(outer_buf, path_router, lan_host,
        ICMP6_PACKET_TOO_BIG, 0);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE &&
            test_ccb.ipv6_firewall_icmp6_err_dropped == dropped_before,
        "an ICMPv6 packet with no type byte is dropped without being counted",
        NULL);

    /* Informational types stay out of the error accounting whatever their
     * length. */
    dropped_before = test_ccb.ipv6_firewall_icmp6_err_dropped;
    ip6 = build_icmp6_truncated(outer_buf, remote_host, lan_host,
        ICMP6_ECHO_REPLY, 4);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == FALSE &&
            test_ccb.ipv6_firewall_icmp6_err_dropped == dropped_before,
        "a truncated informational ICMPv6 message is not counted as an error",
        NULL);

    /* The full-length path is unchanged: a quoted live session still passes. */
    build_udp6(inner_buf, lan_host, remote_host, 40700, 53);
    ip6 = build_icmp6_error(outer_buf, path_router, lan_host,
        ICMP6_PACKET_TOO_BIG, QUOTE_MIN_LEN);
    TEST_ASSERT(ipv6_firewall_inbound_pass(&test_ccb, ip6) == TRUE,
        "a full-length error quoting a live session still passes", NULL);
}

/*--------- SMALL-TABLE FIXTURE FOR EVICTION ----------*/

static ppp_ccb_t small_ccb;
#define SMALL_POOL_SLOTS 8

/* A pool of SMALL_POOL_SLOTS entries sharing the real code paths, so filling
 * it up is cheap enough to test what happens when it is full. */
static STATUS small_env_init(void)
{
    static int small_gen; /* unique DPDK object names per call */
    char name[RTE_RING_NAMESIZE];

    memset(&small_ccb, 0, sizeof(small_ccb));
    rte_spinlock_init(&small_ccb.ipv6_firewall_insert_lock);
    small_ccb.tcp_conntrack_enabled = TRUE;
    small_gen++;

    snprintf(name, sizeof(name), "ipv6_firewall_small_%d", small_gen);
    struct rte_hash_parameters params = {
        .name = name,
        .entries = SMALL_POOL_SLOTS,
        .key_len = sizeof(ipv6_firewall_key_t),
        .hash_func = rte_hash_crc,
        .hash_func_init_val = 0,
        .socket_id = (int)rte_socket_id(),
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF |
                      RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD,
    };
    small_ccb.ipv6_firewall_hash = rte_hash_create(&params);
    if (small_ccb.ipv6_firewall_hash == NULL)
        return ERROR;
    struct rte_hash_rcu_config rcu_cfg = {
        .v = test_rcu,
        .mode = RTE_HASH_QSBR_MODE_DQ,
        .key_data_ptr = &small_ccb,
        .free_key_data_func = ipv6_firewall_hash_free_cb,
    };
    if (rte_hash_rcu_qsbr_add(small_ccb.ipv6_firewall_hash, &rcu_cfg) != 0)
        return ERROR;

    snprintf(name, sizeof(name), "ipv6_firewall_small_free_%d", small_gen);
    small_ccb.ipv6_firewall_free_ring = rte_ring_create(name, SMALL_POOL_SLOTS,
        (int)rte_socket_id(), RING_F_EXACT_SZ);
    if (small_ccb.ipv6_firewall_free_ring == NULL)
        return ERROR;

    for(U32 i=0; i<SMALL_POOL_SLOTS; i++)
        rte_ring_enqueue(small_ccb.ipv6_firewall_free_ring, (void *)(uintptr_t)i);
    return SUCCESS;
}

/**
 * Test 10: when the pool is full of live sessions, opening a new one evicts
 * the least recently used session rather than failing.
 */
static void test_lru_eviction(void)
{
    struct rte_ipv6_hdr *ip6;
    ipv6_firewall_key_t victim_key;
    void *victim_data;
    U64 evicted_before, enospc_before;
    U32 i;

    printf("\nTesting LRU eviction when the pool is full:\n");
    printf("===========================================\n\n");

    fw_env_init_once();
    TEST_ASSERT(small_env_init() == SUCCESS, "small fixture created", NULL);

    for(i=0; i<SMALL_POOL_SLOTS; i++) {
        ip6 = build_udp6(outer_buf, lan_host, remote_host, (U16)(41000 + i), 53);
        ipv6_firewall_learn(&small_ccb, ip6);
    }
    TEST_ASSERT(rte_hash_count(small_ccb.ipv6_firewall_hash) == SMALL_POOL_SLOTS &&
            rte_ring_count(small_ccb.ipv6_firewall_free_ring) == 0,
        "the pool is full of live sessions",
        "hash=%u ring=%u", rte_hash_count(small_ccb.ipv6_firewall_hash),
        rte_ring_count(small_ccb.ipv6_firewall_free_ring));

    /* Lay out distinct recency stamps and remember whose is the oldest. */
    for(i=0; i<SMALL_POOL_SLOTS; i++)
        __atomic_store_n(&small_ccb.ipv6_firewall_last_used[i],
            fastrg_get_cur_cycles() + i, __ATOMIC_RELAXED);
    victim_key = small_ccb.ipv6_firewall_table[0].key;
    evicted_before = small_ccb.ipv6_firewall_evicted;

    ip6 = build_udp6(outer_buf, lan_host, remote_host, 42000, 53);
    ipv6_firewall_learn(&small_ccb, ip6);
    TEST_ASSERT(rte_hash_lookup_data(small_ccb.ipv6_firewall_hash, &victim_key,
            &victim_data) < 0,
        "the least recently used session is gone", NULL);
    TEST_ASSERT(small_ccb.ipv6_firewall_evicted == evicted_before + 1,
        "the eviction is counted", "evicted=%" PRIu64, small_ccb.ipv6_firewall_evicted);

    /* The evicted slot only comes back once readers are quiescent, so the
     * packet that triggered the eviction goes out without a session and the
     * next one of that flow opens it. */
    fw_quiesce_reclaim(&small_ccb);
    ipv6_firewall_learn(&small_ccb, ip6);
    TEST_ASSERT(rte_hash_count(small_ccb.ipv6_firewall_hash) == SMALL_POOL_SLOTS,
        "the retry opens the new session in the reclaimed slot",
        "hash=%u", rte_hash_count(small_ccb.ipv6_firewall_hash));

    /* Nothing left to reclaim and nothing evictable: the attempt fails and is
     * counted, and no slot leaks. */
    for(i=0; i<SMALL_POOL_SLOTS; i++)
        __atomic_store_n(&small_ccb.ipv6_firewall_last_used[i], 0, __ATOMIC_RELAXED);
    enospc_before = small_ccb.ipv6_firewall_enospc;
    ip6 = build_udp6(outer_buf, lan_host, remote_host, 42001, 53);
    ipv6_firewall_learn(&small_ccb, ip6);
    TEST_ASSERT(small_ccb.ipv6_firewall_enospc == enospc_before + 1,
        "an unservable session opening is counted",
        "enospc=%" PRIu64, small_ccb.ipv6_firewall_enospc);
    TEST_ASSERT(rte_ring_count(small_ccb.ipv6_firewall_free_ring) == 0,
        "no slot leaked", NULL);

    ipv6_firewall_table_destroy(&small_ccb);
}

void test_ipv6_firewall(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║           IPv6 FIREWALL MODULE UNIT TESTS                ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_key_normalization();
    test_unsolicited_inbound_dropped();
    test_icmp6_echo_session();
    test_icmp6_error_validation();
    test_tcp_per_state_timeout();
    test_tcp_inbound_state_check();
    test_tcp_inbound_seq_check();
    test_udp_idle_expiry();
    test_recency_coalescing();
    test_icmp6_error_truncated_counted();
    test_lru_eviction();

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
