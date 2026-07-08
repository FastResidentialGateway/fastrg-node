#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <common.h>

#include "../../src/dnsd/dns_codec.h"
#include "../../src/fastrg.h"
#include "../test_helper.h"

static int test_count = 0;
static int pass_count = 0;

/* Build a minimal DNS query for "example.com" type A */
static int build_test_query(U8 *buf, int buflen, U16 id)
{
    if (buflen < 30) return -1;
    memset(buf, 0, buflen);

    /* Header */
    buf[0] = (id >> 8) & 0xFF;
    buf[1] = id & 0xFF;
    buf[2] = 0x01; /* RD=1 */
    buf[3] = 0x00;
    buf[4] = 0x00; buf[5] = 0x01; /* QDCOUNT=1 */
    buf[6] = buf[7] = buf[8] = buf[9] = buf[10] = buf[11] = 0;

    /* Question: \x07example\x03com\x00 */
    int off = 12;
    buf[off++] = 7; memcpy(buf + off, "example", 7); off += 7;
    buf[off++] = 3; memcpy(buf + off, "com", 3); off += 3;
    buf[off++] = 0; /* root */

    /* QTYPE=A(1) QCLASS=IN(1) */
    buf[off++] = 0; buf[off++] = 1;
    buf[off++] = 0; buf[off++] = 1;

    return off;
}

/* Build a minimal DNS response with one A record */
static int build_test_response(U8 *buf, int buflen, U16 id, U32 ip_net, U32 ttl)
{
    if (buflen < 50) return -1;
    int off = build_test_query(buf, buflen, id);
    if (off < 0) return -1;

    /* Flip QR flag */
    buf[2] |= 0x80;
    /* ANCOUNT=1 */
    buf[6] = 0; buf[7] = 1;

    /* Answer: pointer to name at offset 12 */
    buf[off++] = 0xC0; buf[off++] = 0x0C;
    /* TYPE=A */
    buf[off++] = 0; buf[off++] = 1;
    /* CLASS=IN */
    buf[off++] = 0; buf[off++] = 1;
    /* TTL */
    buf[off++] = (ttl >> 24) & 0xFF;
    buf[off++] = (ttl >> 16) & 0xFF;
    buf[off++] = (ttl >> 8) & 0xFF;
    buf[off++] = ttl & 0xFF;
    /* RDLENGTH=4 */
    buf[off++] = 0; buf[off++] = 4;
    /* RDATA */
    memcpy(buf + off, &ip_net, 4); off += 4;

    return off;
}

/* Extract RCODE from flags (already in host byte order after parsing) */
static inline U16 get_rcode(dns_message_t *msg) {
    return msg->header.flags & 0x000F;
}

static void test_dns_parse_query(void)
{
    printf("\nTesting dns_parse_query:\n");
    printf("=========================================\n\n");

    U8 buf[128];
    int len = build_test_query(buf, sizeof(buf), 0x1234);
    TEST_ASSERT(len > 0, "build_test_query succeeds", "");

    dns_message_t msg;
    memset(&msg, 0, sizeof(msg));
    int ret = dns_parse_query(buf, len, &msg);
    TEST_ASSERT(ret == 0, "dns_parse_query returns 0 for valid query", "");
    TEST_ASSERT(msg.header.id == 0x1234, "query ID correct",
        "got 0x%04x", msg.header.id);
    TEST_ASSERT(strcmp(msg.question.name, "example.com") == 0, "domain parsed correctly",
        "got '%s'", msg.question.name);
    TEST_ASSERT(msg.question.qtype == DNS_TYPE_A, "qtype is A",
        "got %u", msg.question.qtype);
    TEST_ASSERT(msg.question.qclass == DNS_CLASS_IN, "qclass is IN",
        "got %u", msg.question.qclass);
}

static void test_dns_parse_query_null(void)
{
    printf("\nTesting dns_parse_query with NULL/short input:\n");
    printf("=========================================\n\n");

    dns_message_t msg;
    int ret = dns_parse_query(NULL, 0, &msg);
    TEST_ASSERT(ret != 0, "dns_parse_query rejects NULL data", "");

    U8 short_buf[4] = {0};
    ret = dns_parse_query(short_buf, 4, &msg);
    TEST_ASSERT(ret != 0, "dns_parse_query rejects short buffer", "");
}

static void test_dns_parse_response(void)
{
    printf("\nTesting dns_parse_response:\n");
    printf("=========================================\n\n");

    U8 buf[128];
    U32 ip_net = htonl(0x01020304); /* 1.2.3.4 */
    int len = build_test_response(buf, sizeof(buf), 0xABCD, ip_net, 300);
    TEST_ASSERT(len > 0, "build_test_response succeeds", "");

    dns_message_t msg;
    memset(&msg, 0, sizeof(msg));
    int ret = dns_parse_response(buf, len, &msg);
    TEST_ASSERT(ret == 0, "dns_parse_response returns 0 for valid response", "");
    TEST_ASSERT(msg.header.id == 0xABCD, "response ID correct",
        "got 0x%04x", msg.header.id);
    TEST_ASSERT(msg.header.ancount >= 1, "ancount >= 1",
        "got %u", msg.header.ancount);
    TEST_ASSERT(msg.min_ttl == 300, "min_ttl is 300", "got %u", msg.min_ttl);
    TEST_ASSERT(get_rcode(&msg) == 0, "rcode is 0 (NOERROR)",
        "got %u", get_rcode(&msg));
}

static void test_dns_build_response_a(void)
{
    printf("\nTesting dns_build_response_a:\n");
    printf("=========================================\n\n");

    U8 query_buf[128];
    int qlen = build_test_query(query_buf, sizeof(query_buf), 0x5678);
    TEST_ASSERT(qlen > 0, "build query for response", "");

    U8 resp_buf[512];
    U32 ip_net = htonl(0xC0A80101); /* 192.168.1.1 */
    int rlen = dns_build_response_a(query_buf, qlen, ip_net, 3600,
        resp_buf, sizeof(resp_buf));
    TEST_ASSERT(rlen > 0, "dns_build_response_a returns positive length",
        "got %d", rlen);
    TEST_ASSERT(rlen <= (int)sizeof(resp_buf), "response fits in buffer", "");

    /* Verify response can be parsed back */
    dns_message_t msg;
    memset(&msg, 0, sizeof(msg));
    int ret = dns_parse_response(resp_buf, rlen, &msg);
    TEST_ASSERT(ret == 0, "built response is parseable", "");
    TEST_ASSERT(msg.header.id == 0x5678, "response ID matches query ID",
        "got 0x%04x", msg.header.id);
    TEST_ASSERT(get_rcode(&msg) == 0, "rcode is NOERROR", "");
}

static void test_dns_build_response_nxdomain(void)
{
    printf("\nTesting dns_build_response_nxdomain:\n");
    printf("=========================================\n\n");

    U8 query_buf[128];
    int qlen = build_test_query(query_buf, sizeof(query_buf), 0x9999);

    U8 resp_buf[512];
    int rlen = dns_build_response_nxdomain(query_buf, qlen,
        resp_buf, sizeof(resp_buf));
    TEST_ASSERT(rlen > 0, "dns_build_response_nxdomain returns positive length", "");

    dns_message_t msg;
    memset(&msg, 0, sizeof(msg));
    int ret = dns_parse_response(resp_buf, rlen, &msg);
    TEST_ASSERT(ret == 0, "nxdomain response is parseable", "");
    TEST_ASSERT(get_rcode(&msg) == DNS_RCODE_NXDOMAIN, "rcode is NXDOMAIN",
        "got %u", get_rcode(&msg));
}

static void test_dns_build_response_servfail(void)
{
    printf("\nTesting dns_build_response_servfail:\n");
    printf("=========================================\n\n");

    U8 query_buf[128];
    int qlen = build_test_query(query_buf, sizeof(query_buf), 0x1111);

    U8 resp_buf[512];
    int rlen = dns_build_response_servfail(query_buf, qlen,
        resp_buf, sizeof(resp_buf));
    TEST_ASSERT(rlen > 0, "dns_build_response_servfail returns positive length", "");

    dns_message_t msg;
    memset(&msg, 0, sizeof(msg));
    int ret = dns_parse_response(resp_buf, rlen, &msg);
    TEST_ASSERT(ret == 0, "servfail response is parseable", "");
    TEST_ASSERT(get_rcode(&msg) == DNS_RCODE_SERVFAIL, "rcode is SERVFAIL",
        "got %u", get_rcode(&msg));
}

/* ============ dns_parse_name wire-format hardening tests ============ */

static void test_dns_parse_name_compression(void)
{
    printf("\nTesting dns_parse_name valid compression:\n");
    printf("=========================================\n\n");

    /* header padding (12B) + "example.com" at 12 + "www" + pointer to 12 */
    U8 pkt[64] = {0};
    int off = 12;
    pkt[off++] = 7; memcpy(pkt + off, "example", 7); off += 7;
    pkt[off++] = 3; memcpy(pkt + off, "com", 3); off += 3;
    pkt[off++] = 0;
    U16 www_off = off;
    pkt[off++] = 3; memcpy(pkt + off, "www", 3); off += 3;
    pkt[off++] = 0xC0; pkt[off++] = 0x0C;

    char name[DNS_MAX_DOMAIN_LEN + 1];
    U16 consumed = dns_parse_name(pkt, off, www_off, name, sizeof(name));
    TEST_ASSERT(consumed == 6,
        "pointer name consumes only its own wire bytes (label + 2B pointer)",
        "expected 6, got %u", consumed);
    TEST_ASSERT(strcmp(name, "www.example.com") == 0,
        "compression pointer expands to the full domain", "got '%s'", name);

    /* root name: single zero byte */
    U8 root_pkt[16] = {0};
    consumed = dns_parse_name(root_pkt, sizeof(root_pkt), 12, name, sizeof(name));
    TEST_ASSERT(consumed == 1 && name[0] == '\0',
        "root name (lone 0x00) consumes 1 byte and yields empty name",
        "consumed=%u name='%s'", consumed, name);
}

static void test_dns_parse_name_malicious(void)
{
    printf("\nTesting dns_parse_name malicious input:\n");
    printf("=========================================\n\n");

    char name[DNS_MAX_DOMAIN_LEN + 1];
    U8 pkt[64] = {0};

    /* self-pointing compression pointer: infinite loop without the guard */
    pkt[12] = 0xC0; pkt[13] = 0x0C;
    TEST_ASSERT(dns_parse_name(pkt, 32, 12, name, sizeof(name)) == 0,
        "self-pointing pointer rejected (loop guard)", NULL);

    /* two pointers chasing each other */
    memset(pkt, 0, sizeof(pkt));
    pkt[12] = 0xC0; pkt[13] = 0x0E;
    pkt[14] = 0xC0; pkt[15] = 0x0C;
    TEST_ASSERT(dns_parse_name(pkt, 32, 12, name, sizeof(name)) == 0,
        "mutually-looping pointers rejected", NULL);

    /* pointer target beyond packet end */
    memset(pkt, 0, sizeof(pkt));
    pkt[12] = 0xC0; pkt[13] = 0xFF;
    TEST_ASSERT(dns_parse_name(pkt, 32, 12, name, sizeof(name)) == 0,
        "pointer past pkt_len rejected", NULL);

    /* pointer truncated at the last byte of the packet */
    memset(pkt, 0, sizeof(pkt));
    pkt[12] = 0xC0;
    TEST_ASSERT(dns_parse_name(pkt, 13, 12, name, sizeof(name)) == 0,
        "truncated 1-byte pointer rejected", NULL);

    /* label length 64 — above DNS_MAX_LABEL_LEN but not a pointer tag */
    memset(pkt, 0, sizeof(pkt));
    pkt[12] = 0x40;
    TEST_ASSERT(dns_parse_name(pkt, 32, 12, name, sizeof(name)) == 0,
        "label length > 63 rejected", NULL);

    /* label runs past the end of the packet */
    memset(pkt, 0, sizeof(pkt));
    pkt[12] = 10; memcpy(pkt + 13, "ab", 2);
    TEST_ASSERT(dns_parse_name(pkt, 15, 12, name, sizeof(name)) == 0,
        "label overrunning pkt_len rejected", NULL);

    /* parsed name longer than the caller's buffer */
    memset(pkt, 0, sizeof(pkt));
    pkt[12] = 10; memcpy(pkt + 13, "aaaaaaaaaa", 10); pkt[23] = 0;
    TEST_ASSERT(dns_parse_name(pkt, 32, 12, name, 8) == 0,
        "name overflowing name_buf rejected", NULL);
}

static void test_dns_parse_query_malicious(void)
{
    printf("\nTesting dns_parse_query/response with malicious names:\n");
    printf("=========================================\n\n");

    dns_message_t msg;
    U8 buf[128];

    /* full query whose qname is a self-pointing pointer */
    int len = build_test_query(buf, sizeof(buf), 0x4444);
    buf[12] = 0xC0; buf[13] = 0x0C;
    TEST_ASSERT(dns_parse_query(buf, len, &msg) != 0,
        "query with looping qname pointer rejected", NULL);

    /* query whose name runs to the end of the buffer: no terminator, no qtype */
    len = build_test_query(buf, sizeof(buf), 0x4545);
    TEST_ASSERT(dns_parse_query(buf, 12 + 12, &msg) != 0,
        "query truncated inside the qname rejected", NULL);

    /* valid response, then poison the answer's name pointer into a self-loop.
     * build_test_response puts the answer name pointer right after the
     * question section (12B header + 13B qname + 4B qtype/qclass = 29). */
    len = build_test_response(buf, sizeof(buf), 0x4646, htonl(0x01020304), 60);
    TEST_ASSERT(len > 0, "build_test_response succeeds", NULL);
    buf[29] = 0xC0; buf[30] = 29;
    TEST_ASSERT(dns_parse_response(buf, len, &msg) != 0,
        "response with looping answer-name pointer rejected", NULL);
}

void test_dns_codec(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           DNS Codec Unit Tests                             ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_dns_parse_query();
    test_dns_parse_query_null();
    test_dns_parse_response();
    test_dns_build_response_a();
    test_dns_build_response_nxdomain();
    test_dns_build_response_servfail();
    test_dns_parse_name_compression();
    test_dns_parse_name_malicious();
    test_dns_parse_query_malicious();

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
