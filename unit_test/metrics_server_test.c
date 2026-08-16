#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <rte_atomic.h>

#include "../src/fastrg.h"
#include "../src/init.h"
#include "../src/lighthttp.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

/* The metrics thread reads the listen address out of the CCB and stores its
 * server state back into it, so a zeroed CCB with that one field set is enough
 * to drive it. The configured port is never used here: every case asks the
 * kernel for a free one, so a real node's endpoint is never disturbed. */
static void init_metrics_ccb(FastRG_t *ccb, char *ip_port)
{
    memset(ccb, 0, sizeof(*ccb));
    ccb->metrics_ip_port = ip_port;
    ccb->metrics_server.listen_fd = -1;
    rte_atomic16_init(&ccb->metrics_stop_requested);
}

/* Listen on a kernel-chosen loopback port and report which one. Keeping the
 * socket open is what makes a second bind on that port fail. */
static int occupy_loopback_port(U16 *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t addr_len = sizeof(addr);
    if (bind(fd, (struct sockaddr *)&addr, addr_len) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }

    *port = ntohs(addr.sin_port);
    return fd;
}

/* A port number nobody is listening on: take one the kernel just handed out and
 * give it straight back. lighthttp cannot ask for port 0 itself — its address
 * parser rejects it. */
static int pick_free_loopback_port(U16 *port)
{
    int fd = occupy_loopback_port(port);

    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

static void test_bind_failure_is_reported(void)
{
    printf("\nTesting metrics server bind failure reporting:\n");

    U16 port = 0;
    int busy_fd = occupy_loopback_port(&port);
    TEST_ASSERT(busy_fd >= 0, "occupy a loopback port for the bind clash",
        "errno=%d", errno);
    if (busy_fd < 0)
        return;

    char ip_port[64];
    snprintf(ip_port, sizeof(ip_port), "127.0.0.1:%u", port);

    FastRG_t ccb;
    init_metrics_ccb(&ccb, ip_port);

    pthread_t metrics_thread;
    int create_ret = pthread_create(&metrics_thread, NULL, metrics_server_run, &ccb);
    TEST_ASSERT(create_ret == 0, "start metrics thread on the busy port",
        "ret=%d", create_ret);
    if (create_ret != 0) {
        close(busy_fd);
        return;
    }

    int ready_ret = metrics_server_wait_ready();
    TEST_ASSERT(ready_ret != 0, "wait_ready reports the failed bind",
        "ret=%d", ready_ret);

    int join_ret = pthread_join(metrics_thread, NULL);
    TEST_ASSERT(join_ret == 0, "metrics thread exits after a failed bind",
        "ret=%d", join_ret);

    close(busy_fd);
}

static void test_startup_success_is_reported(void)
{
    printf("\nTesting metrics server startup success reporting:\n");

    U16 port = 0;
    TEST_ASSERT(pick_free_loopback_port(&port) == 0, "find a free loopback port",
        "errno=%d", errno);
    if (port == 0)
        return;

    char ip_port[64];
    snprintf(ip_port, sizeof(ip_port), "127.0.0.1:%u", port);

    FastRG_t ccb;
    init_metrics_ccb(&ccb, ip_port);

    pthread_t metrics_thread;
    int create_ret = pthread_create(&metrics_thread, NULL, metrics_server_run, &ccb);
    TEST_ASSERT(create_ret == 0, "start metrics thread on a free port",
        "ret=%d", create_ret);
    if (create_ret != 0)
        return;

    int ready_ret = metrics_server_wait_ready();
    TEST_ASSERT(ready_ret == 0, "wait_ready reports the successful bind",
        "ret=%d", ready_ret);

    /* Same stop handshake the shutdown path uses: raise the flag first so a
     * thread still on its way into serve() sees it, then close the listener. */
    rte_atomic16_set(&ccb.metrics_stop_requested, 1);
    lighthttp_stop(&ccb.metrics_server);
    int join_ret = pthread_join(metrics_thread, NULL);
    TEST_ASSERT(join_ret == 0, "metrics thread exits after being stopped",
        "ret=%d", join_ret);
}

/* A failed launch must leave the latch armed for the next one, or a later
 * successful start would read a stale verdict. */
static void test_latch_rearms_between_launches(void)
{
    printf("\nTesting metrics startup latch re-arming:\n");

    U16 port = 0;
    int busy_fd = occupy_loopback_port(&port);
    TEST_ASSERT(busy_fd >= 0, "occupy a loopback port again", "errno=%d", errno);
    if (busy_fd < 0)
        return;

    char busy_ip_port[64];
    snprintf(busy_ip_port, sizeof(busy_ip_port), "127.0.0.1:%u", port);

    FastRG_t busy_ccb;
    init_metrics_ccb(&busy_ccb, busy_ip_port);
    pthread_t busy_thread;
    if (pthread_create(&busy_thread, NULL, metrics_server_run, &busy_ccb) != 0) {
        close(busy_fd);
        return;
    }
    TEST_ASSERT(metrics_server_wait_ready() != 0, "first launch reports failure", NULL);
    pthread_join(busy_thread, NULL);
    close(busy_fd);

    U16 free_port = 0;
    if (pick_free_loopback_port(&free_port) != 0 || free_port == 0)
        return;

    char free_ip_port[64];
    snprintf(free_ip_port, sizeof(free_ip_port), "127.0.0.1:%u", free_port);

    FastRG_t ok_ccb;
    init_metrics_ccb(&ok_ccb, free_ip_port);
    pthread_t ok_thread;
    if (pthread_create(&ok_thread, NULL, metrics_server_run, &ok_ccb) != 0)
        return;

    TEST_ASSERT(metrics_server_wait_ready() == 0,
        "the next launch reports its own success, not the previous failure", NULL);

    rte_atomic16_set(&ok_ccb.metrics_stop_requested, 1);
    lighthttp_stop(&ok_ccb.metrics_server);
    pthread_join(ok_thread, NULL);
}

void test_metrics_server(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║           Metrics Server Startup Unit Tests               ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    test_bind_failure_is_reported();
    test_startup_success_is_reported();
    test_latch_rearms_between_launches();

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
