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
