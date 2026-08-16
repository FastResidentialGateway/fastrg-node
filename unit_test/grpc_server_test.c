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

#include "../src/fastrg.h"
#include "../northbound/grpc/fastrg_grpc_server.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

/* Unique paths per case so the two servers never share a socket file.
 * Port 50052 is deliberately avoided: it belongs to a real node. */
#define BIND_FAIL_SOCK "/tmp/fastrg_grpc_bind_fail_test.sock"
#define STARTUP_OK_SOCK "/tmp/fastrg_grpc_startup_ok_test.sock"

/* The server thread only reads the two address strings out of the CCB, so a
 * zeroed CCB with those fields filled in is enough to drive it. */
static void init_grpc_ccb(FastRG_t *ccb, char *unix_sock_path, char *ip_port)
{
    memset(ccb, 0, sizeof(*ccb));
    ccb->unix_sock_path = unix_sock_path;
    ccb->node_grpc_ip_port = ip_port;
}

/* Listen on a free loopback port without SO_REUSEPORT and report which one.
 * The gRPC server sets SO_REUSEPORT, and Linux only lets sockets share a port
 * when every one of them does, so this listener makes its bind fail. */
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

static void test_bind_failure_is_reported(void)
{
    printf("\nTesting gRPC server bind failure reporting:\n");

    U16 port = 0;
    int busy_fd = occupy_loopback_port(&port);
    TEST_ASSERT(busy_fd >= 0, "occupy a loopback port for the bind clash", "errno=%d", errno);
    if (busy_fd < 0)
        return;

    char ip_port[64];
    snprintf(ip_port, sizeof(ip_port), "127.0.0.1:%u", port);
    char unix_sock_path[] = "unix://" BIND_FAIL_SOCK;

    FastRG_t ccb;
    init_grpc_ccb(&ccb, unix_sock_path, ip_port);

    pthread_t server_thread;
    int create_ret = pthread_create(&server_thread, NULL, fastrg_grpc_server_run, &ccb);
    TEST_ASSERT(create_ret == 0, "start gRPC server thread on the busy port", "ret=%d", create_ret);
    if (create_ret != 0) {
        close(busy_fd);
        return;
    }

    int ready_ret = fastrg_grpc_server_wait_ready();
    TEST_ASSERT(ready_ret != 0, "wait_ready reports the failed bind", "ret=%d", ready_ret);

    int join_ret = pthread_join(server_thread, NULL);
    TEST_ASSERT(join_ret == 0, "server thread exits after a failed bind", "ret=%d", join_ret);

    close(busy_fd);
    unlink(BIND_FAIL_SOCK);
}

static void test_startup_success_is_reported(void)
{
    printf("\nTesting gRPC server startup success reporting:\n");

    /* Port 0 lets the kernel pick a free port, so this case never clashes
     * with whatever else is listening on the machine. */
    char ip_port[] = "127.0.0.1:0";
    char unix_sock_path[] = "unix://" STARTUP_OK_SOCK;

    FastRG_t ccb;
    init_grpc_ccb(&ccb, unix_sock_path, ip_port);

    pthread_t server_thread;
    int create_ret = pthread_create(&server_thread, NULL, fastrg_grpc_server_run, &ccb);
    TEST_ASSERT(create_ret == 0, "start gRPC server thread on a free port", "ret=%d", create_ret);
    if (create_ret != 0)
        return;

    int ready_ret = fastrg_grpc_server_wait_ready();
    TEST_ASSERT(ready_ret == 0, "wait_ready reports the successful bind", "ret=%d", ready_ret);

    fastrg_grpc_server_shutdown();
    int join_ret = pthread_join(server_thread, NULL);
    TEST_ASSERT(join_ret == 0, "server thread exits after shutdown", "ret=%d", join_ret);

    unlink(STARTUP_OK_SOCK);
}

void test_grpc_server(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║             gRPC Server Module Unit Tests                 ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    /* The failure case runs first: shutting the successful server down leaves
     * the shutdown request latched for any server started afterwards. */
    test_bind_failure_is_reported();
    test_startup_success_is_reported();

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
