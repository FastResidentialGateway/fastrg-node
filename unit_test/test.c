#include <sys/resource.h>

#include <common.h>

#include <rte_rcu_qsbr.h>

#include "../src/fastrg.h"
#include "../src/pppd/codec.h"
#include "../src/pppd/fsm.h"
#include "../src/dhcpd/dhcpd.h"
#include "../src/dbg.h"
#include "test.h"

void free_ccb(FastRG_t *ccb)
{
    for(int i=0; i<ccb->user_count; i++) {
        if (ccb->ppp_ccb[i]) fastrg_mfree(ccb->ppp_ccb[i]);
        if (ccb->dhcp_ccb[i]) fastrg_mfree(ccb->dhcp_ccb[i]);
    }
    if (ccb->ppp_ccb) fastrg_mfree(ccb->ppp_ccb);
    if (ccb->dhcp_ccb) fastrg_mfree(ccb->dhcp_ccb);
    fastrg_mfree(ccb);
}

FastRG_t *init_ccb(int user_count)
{
    FastRG_t *ccb = fastrg_malloc(FastRG_t, user_count * sizeof(FastRG_t), 0);
    if (ccb == NULL) {
        puts("Failed to allocate memory for FastRG CCB");
        return NULL;
    }

    ccb->fp = NULL,
    ccb->nic_info = (struct nic_info) {
        .hsi_wan_src_mac = {
            .addr_bytes = {0x9c, 0x69, 0xb4, 0x61, 0x16, 0xdd},
        },
        .hsi_lan_mac = {
            .addr_bytes = {0x9c, 0x69, 0xb4, 0x61, 0x16, 0xdc},
        },
    };
    ccb->user_count = user_count;

    /* ---- ppp_ccb_rcu -------------------------------------------------- */
    size_t rcu_sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    struct rte_rcu_qsbr *ppp_rcu = NULL;
    if (posix_memalign((void **)&ppp_rcu, RTE_CACHE_LINE_SIZE, rcu_sz) != 0)
        goto err;
    memset(ppp_rcu, 0, rcu_sz);
    if (rte_rcu_qsbr_init(ppp_rcu, RTE_MAX_LCORE) != 0)
        goto err;
    rte_rcu_qsbr_thread_register(ppp_rcu, 0);
    ccb->ppp_ccb_rcu = ppp_rcu;

    /* ---- dhcp_ccb_rcu ------------------------------------------------- */
    struct rte_rcu_qsbr *dhcp_rcu = NULL;
    if (posix_memalign((void **)&dhcp_rcu, RTE_CACHE_LINE_SIZE, rcu_sz) != 0)
        goto err;
    memset(dhcp_rcu, 0, rcu_sz);
    if (rte_rcu_qsbr_init(dhcp_rcu, RTE_MAX_LCORE) != 0)
        goto err;
    rte_rcu_qsbr_thread_register(dhcp_rcu, 0);
    ccb->dhcp_ccb_rcu = dhcp_rcu;

    /* ---- ppp_ccb pointer array + individual CCBs --------------------- */
    ccb->ppp_ccb = fastrg_calloc(ppp_ccb_t *, user_count, sizeof(ppp_ccb_t *), 0);
    if (ccb->ppp_ccb == NULL) goto err;
    for(int i=0; i<user_count; i++) {
        ppp_ccb_t *p = fastrg_calloc(ppp_ccb_t, 1, sizeof(ppp_ccb_t), 0);
        if (p == NULL) goto err;
        p->hsi_primary_dns = rte_cpu_to_be_32(0x08080808);
        p->hsi_secondary_dns = rte_cpu_to_be_32(0x01010101);
        ccb->ppp_ccb[i] = p;
    }

    /* ---- dhcp_ccb pointer array + individual CCBs -------------------- */
    ccb->dhcp_ccb = fastrg_calloc(dhcp_ccb_t *, user_count, sizeof(dhcp_ccb_t *), 0);
    if (ccb->dhcp_ccb == NULL) goto err;
    for(int i=0; i<user_count; i++) {
        dhcp_ccb_t *d = fastrg_calloc(dhcp_ccb_t, 1, sizeof(dhcp_ccb_t), 0);
        if (d == NULL) goto err;
        ccb->dhcp_ccb[i] = d;
    }

    ccb->loglvl = -1;
    dbg_init((void *)ccb);

    return ccb;

err:
    free_ccb(ccb);
    return NULL;
}

int main()
{
    struct rlimit rlim;
    int ret = 0;

    /* Disable stdout buffering so output is visible in CI even if the process crashes */
    setbuf(stdout, NULL);

    // Set ulimit to unlimited for core dumps and file descriptors
    rlim.rlim_cur = RLIM_INFINITY;
    rlim.rlim_max = RLIM_INFINITY;

    if (setrlimit(RLIMIT_CORE, &rlim) == 0)
        printf("Set core dump size to unlimited\n");

    if (setrlimit(RLIMIT_NOFILE, &rlim) == 0)
        printf("Set max open files to unlimited\n");

    /* Set stack size to unlimited (equivalent to `ulimit -s unlimited`) */
    if (setrlimit(RLIMIT_STACK, &rlim) == 0)
        printf("Set stack size to unlimited\n");

    signal(SIGCHLD, SIG_IGN);

    puts("====================start unit tests====================\n");
    FastRG_t *fastrg_ccb = init_ccb(1);
    if (fastrg_ccb == NULL) {
        puts("Failed to mock FastRG CCB");
        return 1;
    }
    U32 total_tests = 0;
    U32 total_pass = 0;

    puts("====================test pppd/codec.c====================");
    test_ppp_codec(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test pppd/fsm.c====================");
    test_ppp_fsm(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test dhcpd/dhcp_codec.c====================");
    test_dhcp_codec(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test utils.c====================");
    test_utils(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test avl_tree.c====================");
    test_avl_tree(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test pppd/nat.h====================");
    test_nat(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test etcd_integration.c====================");
    test_etcd_integration(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test dp_codec.h====================");
    test_dp_codec(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test dbg.c====================");
    test_dbg(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test config.c====================");
    test_config(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test northbound.c====================");
    test_northbound(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test dnsd/dns_codec.c====================");
    test_dns_codec(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test dnsd/dns_cache.c====================");
    test_dns_cache(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test dnsd/dnsd.c====================");
    test_dnsd(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    puts("====================test pppd/tcp_conntrack.c====================");
    test_tcp_conntrack(fastrg_ccb, &total_tests, &total_pass);
    puts("ok!");

    printf("\n====================Unit Test Summary====================\n\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║  All Test Summary                                          ║\n");
    printf("╠════════════════════════════════════════════════════════════╣\n");
    printf("║  Total Tests:  %3d                                         ║\n", total_tests);
    printf("║  Passed:       %3d                                         ║\n", total_pass);
    printf("║  Failed:       %3d                                         ║\n", total_tests - total_pass);
    printf("║  Success Rate: %3d%%                                        ║\n", 
           total_tests > 0 ? (total_pass * 100 / total_tests) : 0);
    printf("╚════════════════════════════════════════════════════════════╝\n");
    if (total_tests == total_pass) {
        printf("\nAll %u tests passed successfully\n\n", total_tests);
        ret = 0;
    } else {
        printf("\n%d/%d tests failed\n\n", total_tests - total_pass, total_tests);
        ret = 1;
    }

    puts("====================end of unit tests====================");

    return ret;
}
