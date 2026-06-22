#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "../src/lighthttp.h"
#include "../src/fastrg.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

/* ---- growable buffer ---- */
static void test_buf_basic(void)
{
    printf("\nTesting lighthttp_buf basic ops:\n");
    lighthttp_buf_t b = {0};

    lighthttp_buf_append(&b, "hello", 5);
    TEST_ASSERT(b.len == 5 && strcmp(b.data, "hello") == 0,
        "buf append", "got '%s'", b.data ? b.data : "(null)");

    lighthttp_buf_append(&b, " world", 6);
    TEST_ASSERT(b.len == 11 && strcmp(b.data, "hello world") == 0,
        "buf append append", "got '%s'", b.data);
    TEST_ASSERT(!lighthttp_buf_oom(&b), "buf not in oom state", "oom=%d", b.oom);

    lighthttp_buf_reset(&b);
    TEST_ASSERT(b.len == 0, "buf reset zeroes len", "len=%zu", b.len);
    lighthttp_buf_append(&b, "x", 1);
    TEST_ASSERT(b.len == 1 && b.data[0] == 'x', "buf reuse after reset", "got '%s'", b.data);

    lighthttp_buf_free(&b);
    TEST_ASSERT(b.data == NULL && b.len == 0 && b.cap == 0, "buf free clears struct",
        "data=%p len=%zu cap=%zu", (void *)b.data, b.len, b.cap);
}

static void test_buf_appendf_and_growth(void)
{
    printf("\nTesting lighthttp_buf appendf + growth:\n");
    lighthttp_buf_t b = {0};

    lighthttp_buf_appendf(&b, "n=%d s=%s", 42, "abc");
    TEST_ASSERT(strcmp(b.data, "n=42 s=abc") == 0, "appendf basic", "got '%s'", b.data);

    /* Force growth well beyond the initial capacity. */
    lighthttp_buf_reset(&b);
    for (int i = 0; i < 5000; i++)
        lighthttp_buf_append(&b, "0123456789", 10); /* 50000 bytes */
    TEST_ASSERT(b.len == 50000 && !lighthttp_buf_oom(&b), "buf grows to 50k", "len=%zu", b.len);
    TEST_ASSERT(b.data[49999] == '9' && b.data[50000] == '\0',
        "buf growth preserves content + NUL", "tail='%c'", b.data[49999]);

    /* A single formatted line larger than the internal 512-byte stack buffer. */
    lighthttp_buf_reset(&b);
    char big[1000];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    lighthttp_buf_appendf(&b, "%s", big);
    TEST_ASSERT(b.len == 999 && b.data[998] == 'A' && b.data[999] == '\0',
        "appendf line larger than stack buffer", "len=%zu", b.len);

    lighthttp_buf_free(&b);
}

/* ---- listen-address parser ---- */
static void test_parse_addr(void)
{
    printf("\nTesting lighthttp_parse_addr:\n");
    char host[64];
    int port;

    TEST_ASSERT(lighthttp_parse_addr("0.0.0.0:9091", host, sizeof(host), &port) == 0 &&
        strcmp(host, "0.0.0.0") == 0 && port == 9091, "host:port", "host=%s port=%d", host, port);
    TEST_ASSERT(lighthttp_parse_addr("127.0.0.1:55178", host, sizeof(host), &port) == 0 &&
        strcmp(host, "127.0.0.1") == 0 && port == 55178, "ip:port", "host=%s port=%d", host, port);
    TEST_ASSERT(lighthttp_parse_addr("9091", host, sizeof(host), &port) == 0 &&
        strcmp(host, "0.0.0.0") == 0 && port == 9091, "bare port defaults host", "host=%s port=%d", host, port);
    TEST_ASSERT(lighthttp_parse_addr(":8080", host, sizeof(host), &port) == 0 &&
        strcmp(host, "0.0.0.0") == 0 && port == 8080, "empty host defaults", "host=%s port=%d", host, port);

    TEST_ASSERT(lighthttp_parse_addr("abc", host, sizeof(host), &port) == -1, "non-numeric port rejected", "abc");
    TEST_ASSERT(lighthttp_parse_addr("0.0.0.0:0", host, sizeof(host), &port) == -1, "port 0 rejected", ":0");
    TEST_ASSERT(lighthttp_parse_addr("0.0.0.0:70000", host, sizeof(host), &port) == -1, "port > 65535 rejected", ":70000");
    TEST_ASSERT(lighthttp_parse_addr("1.2.3.4:", host, sizeof(host), &port) == -1, "empty port rejected", "trailing colon");
    TEST_ASSERT(lighthttp_parse_addr(NULL, host, sizeof(host), &port) == -1, "NULL addr rejected", "NULL");
}

/* ---- request-line parser ---- */
static void test_parse_request_line(void)
{
    printf("\nTesting lighthttp_parse_request_line:\n");
    char m[16], p[256];

    TEST_ASSERT(lighthttp_parse_request_line("GET /metrics HTTP/1.1\r\n", m, sizeof(m), p, sizeof(p)) == 0 &&
        strcmp(m, "GET") == 0 && strcmp(p, "/metrics") == 0, "GET /metrics", "m=%s p=%s", m, p);
    TEST_ASSERT(lighthttp_parse_request_line("GET /metrics?x=1 HTTP/1.1\r\n", m, sizeof(m), p, sizeof(p)) == 0 &&
        strcmp(p, "/metrics") == 0, "query string stripped", "p=%s", p);
    TEST_ASSERT(lighthttp_parse_request_line("POST /healthz HTTP/1.0\r\n", m, sizeof(m), p, sizeof(p)) == 0 &&
        strcmp(m, "POST") == 0 && strcmp(p, "/healthz") == 0, "POST /healthz", "m=%s p=%s", m, p);

    TEST_ASSERT(lighthttp_parse_request_line("garbage", m, sizeof(m), p, sizeof(p)) == -1, "no-space line rejected", "garbage");
    TEST_ASSERT(lighthttp_parse_request_line(" /x HTTP/1.1", m, sizeof(m), p, sizeof(p)) == -1, "empty method rejected", "leading space");
}

/* ---- route registration / matching ---- */
static int dummy_handler(lighthttp_buf_t *o, const char **ct, void *ctx)
{
    (void)o;
    (void)ct;
    (void)ctx;
    return 200;
}

static void test_match(void)
{
    printf("\nTesting lighthttp route match:\n");
    lighthttp_server_t s;
    memset(&s, 0, sizeof(s));

    TEST_ASSERT(lighthttp_add_route(&s, "GET", "/metrics", dummy_handler, NULL) == 0, "add /metrics", "n=%d", s.n_routes);
    TEST_ASSERT(lighthttp_add_route(&s, "GET", "/healthz", dummy_handler, NULL) == 0, "add /healthz", "n=%d", s.n_routes);

    TEST_ASSERT(lighthttp_match(&s, "GET", "/metrics") != NULL, "match /metrics", "should match");
    TEST_ASSERT(lighthttp_match(&s, "GET", "/healthz") != NULL, "match /healthz", "should match");
    TEST_ASSERT(lighthttp_match(&s, "GET", "/nope") == NULL, "no match unknown path", "should be NULL");
    TEST_ASSERT(lighthttp_match(&s, "POST", "/metrics") == NULL, "no match wrong method", "should be NULL");

    /* Fill the route table to capacity, then verify overflow is rejected. */
    while (s.n_routes < LIGHTHTTP_MAX_ROUTES)
        lighthttp_add_route(&s, "GET", "/filler", dummy_handler, NULL);
    TEST_ASSERT(lighthttp_add_route(&s, "GET", "/overflow", dummy_handler, NULL) == -1,
        "route table overflow rejected", "n=%d max=%d", s.n_routes, LIGHTHTTP_MAX_ROUTES);
}

void test_lighthttp(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║              lighthttp Module Unit Tests                  ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    test_buf_basic();
    test_buf_appendf_and_growth();
    test_parse_addr();
    test_parse_request_line();
    test_match();

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
