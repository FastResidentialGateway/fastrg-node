#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "../src/lighthttp.h"
#include "../src/fastrg.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

/* ---- connection timeouts ---- */
static void test_conn_timeouts(void)
{
    printf("\nTesting lighthttp connection timeouts:\n");
    int sockets[2];
    int pair_ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    TEST_ASSERT(pair_ret == 0, "socketpair for timeout test", "ret=%d errno=%d", pair_ret, errno);
    if (pair_ret != 0)
        return;

    int ret = lighthttp_set_conn_timeouts(sockets[0], 1);
    TEST_ASSERT(ret == 0, "set connection timeouts", "ret=%d errno=%d", ret, errno);

    struct timeval recv_timeout;
    socklen_t timeout_len = sizeof(recv_timeout);
    memset(&recv_timeout, 0, sizeof(recv_timeout));
    int get_ret = getsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, &timeout_len);
    TEST_ASSERT(get_ret == 0 && recv_timeout.tv_sec == 1 && recv_timeout.tv_usec == 0,
        "receive timeout is readable", "ret=%d timeout=%ld.%06ld",
        get_ret, (long)recv_timeout.tv_sec, (long)recv_timeout.tv_usec);

    TEST_ASSERT(lighthttp_set_conn_timeouts(-1, 1) == -1,
        "invalid fd timeout rejected", "errno=%d", errno);

    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    errno = 0;
    char byte;
    ssize_t read_ret = read(sockets[0], &byte, sizeof(byte));
    int read_errno = errno;
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (double)(end.tv_sec - start.tv_sec) +
                     (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;

    TEST_ASSERT(read_ret == -1 && (read_errno == EAGAIN || read_errno == EWOULDBLOCK),
        "idle read expires with would-block", "ret=%zd errno=%d elapsed=%.3f", read_ret, read_errno, elapsed);
    TEST_ASSERT(elapsed >= 0.5 && elapsed < 3.0,
        "idle read expires after about one second", "elapsed=%.3f", elapsed);

    close(sockets[0]);
    close(sockets[1]);
}

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

static int find_available_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(fd);
        return -1;
    }

    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

static void test_stop(void)
{
    printf("\nTesting lighthttp stop:\n");
    int port = find_available_port();
    lighthttp_server_t s;
    char addr[64];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", port);

    int ret = port > 0 ? lighthttp_init(&s, addr) : -1;
    TEST_ASSERT(ret == 0, "server initializes before stop", "ret=%d port=%d", ret, port);
    if (ret != 0)
        return;

    int listen_fd = s.listen_fd;
    lighthttp_stop(&s);
    errno = 0;
    int fd_status = fcntl(listen_fd, F_GETFD);
    TEST_ASSERT(s.listen_fd == -1 && fd_status == -1 && errno == EBADF,
        "stop closes listening socket", "listen_fd=%d fd_status=%d errno=%d",
        s.listen_fd, fd_status, errno);
}

/* ---- errno contract on lighthttp_init() failure ----
 * The caller logs why the listener could not come up, so errno has to survive
 * the fprintf and close() that run before the failure return. Distinguishing
 * EADDRINUSE from EACCES is what tells "somebody holds the port" apart from
 * "we are not allowed to bind it". */
static void test_init_failure_errno(void)
{
    printf("\nTesting lighthttp_init failure errno:\n");

    /* Hold an ephemeral port so the bind below has to fail. Never a fixed
     * port: a real node's endpoint must not be disturbed. */
    int busy_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(busy_fd >= 0, "open a socket to occupy a port", "errno=%d", errno);
    if (busy_fd < 0)
        return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t addr_len = sizeof(addr);
    int occupied = bind(busy_fd, (struct sockaddr *)&addr, addr_len) == 0 &&
                   getsockname(busy_fd, (struct sockaddr *)&addr, &addr_len) == 0 &&
                   listen(busy_fd, 1) == 0;
    TEST_ASSERT(occupied, "occupy a loopback port", "errno=%d", errno);
    if (!occupied) {
        close(busy_fd);
        return;
    }

    char busy_addr[64];
    snprintf(busy_addr, sizeof(busy_addr), "127.0.0.1:%u", ntohs(addr.sin_port));

    lighthttp_server_t s;
    errno = 0;
    int ret = lighthttp_init(&s, busy_addr);
    int busy_errno = errno;
    TEST_ASSERT(ret == -1 && busy_errno == EADDRINUSE,
        "an occupied port fails with EADDRINUSE",
        "ret=%d errno=%d", ret, busy_errno);
    close(busy_fd);

    errno = 0;
    ret = lighthttp_init(&s, "not-a-port");
    int parse_errno = errno;
    TEST_ASSERT(ret == -1 && parse_errno == EINVAL,
        "an unparsable address fails with EINVAL",
        "ret=%d errno=%d", ret, parse_errno);

    errno = 0;
    ret = lighthttp_init(&s, "999.999.999.999:8080");
    int host_errno = errno;
    TEST_ASSERT(ret == -1 && host_errno == EINVAL,
        "an invalid host fails with EINVAL",
        "ret=%d errno=%d", ret, host_errno);
}

void test_lighthttp(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║              lighthttp Module Unit Tests                  ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    test_conn_timeouts();
    test_buf_basic();
    test_buf_appendf_and_growth();
    test_parse_addr();
    test_parse_request_line();
    test_match();
    test_stop();
    test_init_failure_errno();

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
