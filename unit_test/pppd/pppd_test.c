#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <common.h>

#include <rte_ether.h>
#include <rte_timer.h>
#include <rte_atomic.h>
#include <rte_ring.h>
#include <rte_lcore.h>
#include <rte_mempool.h>

#include "../../src/fastrg.h"
#include "../../src/init.h"
#include "../../src/pppd/pppd.h"
#include "../../src/pppd/fsm.h"
#include "../../src/pppd/codec.h"
#include "../../src/pppd/header.h"
#include "../../src/dhcpd/dhcpd.h"
#include "../test_helper.h"

static int test_count = 0;
static int pass_count = 0;

/* Not exported via a header (only used by src/init.c's own init sequence),
 * but has external linkage — forward-declare to build the cp_q/free_mail_ring
 * fixture the redial_pending tests need. */
extern STATUS init_ring(FastRG_t *fastrg_ccb);

/*
 * ppp_ccb_t embeds the ~16MB NAT slot pool (addr_table/nat_expire_at) --
 * keep the fixture static instead of burning a stack frame per case, same
 * reasoning as pppd/codec_test.c's decode_ccb.
 *
 * exit_ppp() resolves ccb_id 0 through PPPD_GET_CCB/DHCPD_GET_CCB, so the
 * fixture is installed at fastrg_ccb->ppp_ccb[0]; the original pointer is
 * restored after the suite so later suites see the shared ccb untouched.
 */
static ppp_ccb_t test_ppp_ccb;
static FastRG_t *g_pppd_fastrg_ccb;
static BOOL pppd_env_initialized = FALSE;
static pppoe_header_tag_t *g_padr_tag_buf;

static void pppd_env_init(FastRG_t *fastrg_ccb)
{
    if (direct_pool[0] == NULL) {
        direct_pool[0] = rte_pktmbuf_pool_create("pppd_test_pool", 32, 0, 0,
            RTE_MBUF_DEFAULT_BUF_SIZE, (int)rte_socket_id());
    }
    memset(fastrg_ccb->per_subscriber_stats, 0,
        sizeof(fastrg_ccb->per_subscriber_stats));
    fastrg_rcu_persistent[rte_lcore_id()] = TRUE;
    rte_timer_subsystem_init();
    fastrg_ccb->lcore.ctrl_thread = rte_lcore_id();

    /* fastrg_ccb is malloc'd (not zeroed) by test.c's init_ccb(), so cp_q
     * starts as garbage, not NULL -- a one-shot static flag is the only
     * reliable "already initialized" guard here. */
    static BOOL ring_ready = FALSE;
    if (!ring_ready) {
        init_ring(fastrg_ccb);
        ring_ready = TRUE;
    }

    /* build_padr() walks this as a tag list; a zeroed buffer's first tag
     * reads as {type=END_OF_LIST, length=0}, which terminates the walk
     * immediately -- exactly what an empty PADO tag set looks like. */
    if (g_padr_tag_buf == NULL)
        g_padr_tag_buf = fastrg_calloc(pppoe_header_tag_t, 1,
            PPPoE_TAG_DEFAULT_MAX_LEN, RTE_CACHE_LINE_SIZE);
}

/**
 * @fn pppd_ccb_reset
 *
 * @brief rebuild the ppp_ccb fixture in END_PHASE with fresh timers, and
 *      install it at fastrg_ccb->ppp_ccb[0] (exit_ppp's redial_pending path
 *      resolves ccb_id 0 through PPPD_GET_CCB)
 * @return
 *      void
 */
static void pppd_ccb_reset(void)
{
    if (pppd_env_initialized) {
        rte_timer_stop_sync(&test_ppp_ccb.pppoe);
        rte_timer_stop_sync(&test_ppp_ccb.ppp);
        rte_timer_stop_sync(&test_ppp_ccb.ppp_alive);
    }
    pppd_env_initialized = TRUE;

    memset(&test_ppp_ccb, 0, sizeof(test_ppp_ccb));
    test_ppp_ccb.fastrg_ccb = g_pppd_fastrg_ccb;
    test_ppp_ccb.user_num = 1;
    rte_atomic16_init(&test_ppp_ccb.vlan_id);
    rte_atomic16_set(&test_ppp_ccb.vlan_id, 100);
    rte_timer_init(&test_ppp_ccb.pppoe);
    rte_timer_init(&test_ppp_ccb.ppp);
    rte_timer_init(&test_ppp_ccb.ppp_alive);
    rte_atomic16_init(&test_ppp_ccb.ppp_bool);
    rte_atomic16_init(&test_ppp_ccb.dp_start_bool);
    rte_atomic16_init(&test_ppp_ccb.redial_pending);
    test_ppp_ccb.ppp_phase[0].state = S_INIT;
    test_ppp_ccb.ppp_phase[1].state = S_INIT;
    test_ppp_ccb.phase = END_PHASE;
    test_ppp_ccb.pppoe_phase.pppoe_header_tag = g_padr_tag_buf;

    g_pppd_fastrg_ccb->ppp_ccb[0] = &test_ppp_ccb;
}

/* Drains cp_q back into free_mail_ring, returning how many PPPoE_CMD_ENABLE
 * events for ccb_id 0 were found. */
static int pppd_drain_pppoe_enable_events(void)
{
    tFastRG_MBX *mail = NULL;
    int found = 0;
    while (rte_ring_dequeue(g_pppd_fastrg_ccb->cp_q, (void **)&mail) == 0) {
        if (mail->type == EV_NORTHBOUND_PPPoE) {
            fastrg_event_northbound_msg_t *msg = (fastrg_event_northbound_msg_t *)mail->refp;
            if (msg->cmd == PPPoE_CMD_ENABLE && msg->ccb_id == 0)
                found++;
        }
        rte_ring_enqueue(g_pppd_fastrg_ccb->free_mail_ring, mail);
    }
    return found;
}

/* ---- ppp_connect ---- */

static void test_ppp_connect_already_active(void)
{
    printf("\nTesting ppp_connect (already in a PPPoE connection):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = LCP_PHASE;
    STATUS ret = ppp_connect(&test_ppp_ccb);
    TEST_ASSERT(ret == ERROR, "phase > END_PHASE returns ERROR", "got %d", ret);
    TEST_ASSERT(test_ppp_ccb.phase == LCP_PHASE, "phase left unchanged", "got %u", test_ppp_ccb.phase);
}

static void test_ppp_connect_from_end_phase(void)
{
    printf("\nTesting ppp_connect (from END_PHASE):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    STATUS ret = ppp_connect(&test_ppp_ccb);
    TEST_ASSERT(ret == SUCCESS, "connect from END_PHASE returns SUCCESS", "got %d", ret);
    TEST_ASSERT(test_ppp_ccb.phase == PPPOE_PHASE, "phase advances to PPPOE_PHASE", "got %u", test_ppp_ccb.phase);
    TEST_ASSERT(test_ppp_ccb.pppoe_phase.max_retransmit == MAX_RETRAN,
        "max_retransmit set to MAX_RETRAN", "got %u", test_ppp_ccb.pppoe_phase.max_retransmit);
    TEST_ASSERT(test_ppp_ccb.pppoe_phase.timer_counter == 1,
        "timer_counter is 1 after the first PADI send", "got %u", test_ppp_ccb.pppoe_phase.timer_counter);
    TEST_ASSERT(rte_atomic16_read(&test_ppp_ccb.ppp_bool) == 1, "ppp_bool set to 1", "");
    TEST_ASSERT(rte_timer_pending(&test_ppp_ccb.pppoe), "PADI retransmit timer armed", "");
}

/* ---- ppp_disconnect ---- */

static void test_ppp_disconnect_from_end_phase(void)
{
    printf("\nTesting ppp_disconnect (already in END_PHASE):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    STATUS ret = ppp_disconnect(&test_ppp_ccb);
    TEST_ASSERT(ret == ERROR, "disconnect from END_PHASE returns ERROR", "got %d", ret);
}

static void test_ppp_disconnect_while_processing(void)
{
    printf("\nTesting ppp_disconnect (teardown already in progress):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = LCP_PHASE;
    test_ppp_ccb.ppp_processing = TRUE;
    STATUS ret = ppp_disconnect(&test_ppp_ccb);
    TEST_ASSERT(ret == ERROR, "disconnect while ppp_processing returns ERROR", "got %d", ret);
}

static void test_ppp_disconnect_normal(void)
{
    printf("\nTesting ppp_disconnect (normal case):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = LCP_PHASE;
    test_ppp_ccb.ppp_processing = FALSE;
    STATUS ret = ppp_disconnect(&test_ppp_ccb);
    TEST_ASSERT(ret == SUCCESS, "normal disconnect returns SUCCESS", "got %d", ret);
    TEST_ASSERT(test_ppp_ccb.ppp_processing == TRUE,
        "PPP_bye's LCP_PHASE branch marks ppp_processing (delegates to PPP_bye)", "");
    TEST_ASSERT(test_ppp_ccb.cp == 0, "cp reset to LCP", "");
}

/* ---- PPP_bye state walk-down ---- */

static void test_ppp_bye_end_phase(void)
{
    printf("\nTesting PPP_bye (END_PHASE -> exit_ppp):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = END_PHASE;
    rte_atomic16_set(&test_ppp_ccb.ppp_bool, 1);
    U8 before = g_pppd_fastrg_ccb->cur_user;
    g_pppd_fastrg_ccb->cur_user = 5;

    PPP_bye(&test_ppp_ccb);

    TEST_ASSERT(rte_atomic16_read(&test_ppp_ccb.ppp_bool) == 0, "END_PHASE clears ppp_bool via exit_ppp", "");
    TEST_ASSERT(test_ppp_ccb.ppp_processing == FALSE, "ppp_processing left FALSE", "");
    TEST_ASSERT(g_pppd_fastrg_ccb->cur_user == 4, "exit_ppp decrements cur_user", "got %u", g_pppd_fastrg_ccb->cur_user);
    g_pppd_fastrg_ccb->cur_user = before;
}

static void test_ppp_bye_pppoe_phase_recurses_to_end(void)
{
    printf("\nTesting PPP_bye (PPPOE_PHASE recurses down to END_PHASE):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = PPPOE_PHASE;
    rte_atomic16_set(&test_ppp_ccb.ppp_bool, 1);

    PPP_bye(&test_ppp_ccb);

    TEST_ASSERT(test_ppp_ccb.phase == END_PHASE,
        "PPPOE_PHASE decrements then recurses down to END_PHASE", "got %u", test_ppp_ccb.phase);
    TEST_ASSERT(rte_atomic16_read(&test_ppp_ccb.ppp_bool) == 0,
        "recursion reaches exit_ppp, clearing ppp_bool", "");
}

static void test_ppp_bye_lcp_phase(void)
{
    printf("\nTesting PPP_bye (LCP_PHASE -> E_CLOSE, no IPCP terminate needed):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = LCP_PHASE;
    test_ppp_ccb.cp = 1;
    test_ppp_ccb.ppp_phase[1].state = S_OPENED;

    PPP_bye(&test_ppp_ccb);

    TEST_ASSERT(test_ppp_ccb.ppp_processing == TRUE, "LCP_PHASE marks ppp_processing", "");
    TEST_ASSERT(test_ppp_ccb.cp == 0, "cp forced back to LCP (0)", "");
    TEST_ASSERT(test_ppp_ccb.ppp_phase[1].state == S_INIT,
        "NCP state force-reset to S_INIT", "got %u", test_ppp_ccb.ppp_phase[1].state);
}

static void test_ppp_bye_data_phase_downgrades_to_lcp(void)
{
    printf("\nTesting PPP_bye (DATA_PHASE downgrades straight to LCP_PHASE):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = DATA_PHASE;
    rte_atomic16_set(&test_ppp_ccb.dp_start_bool, 1);
    test_ppp_ccb.cp = 1;

    PPP_bye(&test_ppp_ccb);

    TEST_ASSERT(test_ppp_ccb.phase == LCP_PHASE,
        "DATA_PHASE downgrades to LCP_PHASE (RFC 1661 SS3.7 -- LCP close is sufficient)",
        "got %u", test_ppp_ccb.phase);
    TEST_ASSERT(rte_atomic16_read(&test_ppp_ccb.dp_start_bool) == 0, "dp_start_bool cleared", "");
    TEST_ASSERT(test_ppp_ccb.cp == 0, "cp forced back to LCP (0)", "");
}

/* ---- exit_ppp ---- */

static void test_exit_ppp_resets_fields(void)
{
    printf("\nTesting exit_ppp (field reset):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = LCP_PHASE;
    rte_atomic16_set(&test_ppp_ccb.ppp_bool, 1);
    test_ppp_ccb.ppp_phase[0].state = S_OPENED;
    test_ppp_ccb.ppp_phase[1].state = S_OPENED;
    test_ppp_ccb.pppoe_phase.active = TRUE;
    test_ppp_ccb.hsi_ipv4 = 0xC0A80101;
    test_ppp_ccb.hsi_ipv4_gw = 0xC0A80001;
    test_ppp_ccb.hsi_primary_dns = 0x08080808;
    test_ppp_ccb.hsi_secondary_dns = 0x01010101;
    U8 before = g_pppd_fastrg_ccb->cur_user;
    g_pppd_fastrg_ccb->cur_user = 5;

    exit_ppp(&test_ppp_ccb);

    TEST_ASSERT(rte_atomic16_read(&test_ppp_ccb.ppp_bool) == 0, "ppp_bool cleared", "");
    TEST_ASSERT(test_ppp_ccb.phase == END_PHASE, "phase set to END_PHASE", "");
    TEST_ASSERT(test_ppp_ccb.ppp_phase[0].state == S_INIT && test_ppp_ccb.ppp_phase[1].state == S_INIT,
        "both cp states reset to S_INIT", "");
    TEST_ASSERT(test_ppp_ccb.pppoe_phase.active == FALSE, "pppoe_phase.active cleared", "");
    TEST_ASSERT(test_ppp_ccb.hsi_ipv4 == 0 && test_ppp_ccb.hsi_ipv4_gw == 0,
        "assigned IPs cleared", "");
    TEST_ASSERT(test_ppp_ccb.hsi_primary_dns == 0xffffffff && test_ppp_ccb.hsi_secondary_dns == 0xffffffff,
        "DNS reset to unassigned sentinel (0xffffffff)", "");
    TEST_ASSERT(g_pppd_fastrg_ccb->cur_user == 4, "cur_user decremented", "got %u", g_pppd_fastrg_ccb->cur_user);
    g_pppd_fastrg_ccb->cur_user = before;
}

static void test_exit_ppp_redial_pending_honored(void)
{
    printf("\nTesting exit_ppp (redial_pending honored -> re-dial enqueued):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = LCP_PHASE;
    rte_atomic16_set(&test_ppp_ccb.ppp_bool, 1);
    rte_atomic16_set(&test_ppp_ccb.redial_pending, 1);
    rte_atomic16_set(&DHCPD_GET_CCB(g_pppd_fastrg_ccb, 0)->dhcp_bool, 0);
    pppd_drain_pppoe_enable_events(); /* clear anything left by a previous case */

    exit_ppp(&test_ppp_ccb);

    TEST_ASSERT(rte_atomic16_read(&test_ppp_ccb.redial_pending) == 0, "redial_pending cleared", "");
    TEST_ASSERT(pppd_drain_pppoe_enable_events() == 1,
        "teardown completion enqueues exactly one PPPoE_CMD_ENABLE event for ccb 0", "");
}

static void test_exit_ppp_no_redial_when_not_pending(void)
{
    printf("\nTesting exit_ppp (no redial_pending -> no re-dial):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = LCP_PHASE;
    rte_atomic16_set(&test_ppp_ccb.ppp_bool, 1);
    rte_atomic16_set(&test_ppp_ccb.redial_pending, 0);
    pppd_drain_pppoe_enable_events();

    exit_ppp(&test_ppp_ccb);

    TEST_ASSERT(pppd_drain_pppoe_enable_events() == 0,
        "no redial_pending -> no northbound re-dial event enqueued", "");
}

/* ---- PPP_keepalive_cb ---- */

static void test_keepalive_probes_and_increments(void)
{
    printf("\nTesting PPP_keepalive_cb (under the miss threshold, probes the peer):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.echo_miss_count = 0;
    struct rte_timer dummy_tim;
    rte_timer_init(&dummy_tim);

    PPP_keepalive_cb(&dummy_tim, &test_ppp_ccb);

    TEST_ASSERT(test_ppp_ccb.echo_miss_count == 1,
        "echo_miss_count incremented after sending a probe", "got %u", test_ppp_ccb.echo_miss_count);
    TEST_ASSERT(test_ppp_ccb.phase == END_PHASE,
        "session left untouched while under the fail threshold (PPP_bye not called)", "");
}

static void test_keepalive_exceeds_threshold_tears_down(void)
{
    printf("\nTesting PPP_keepalive_cb (LCP_ECHO_MAX_FAIL reached -> teardown, no further probe):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = LCP_PHASE;
    rte_atomic16_set(&test_ppp_ccb.ppp_bool, 1);
    test_ppp_ccb.echo_miss_count = LCP_ECHO_MAX_FAIL;
    struct rte_timer dummy_tim;
    rte_timer_init(&dummy_tim);

    PPP_keepalive_cb(&dummy_tim, &test_ppp_ccb);

    TEST_ASSERT(test_ppp_ccb.ppp_processing == TRUE,
        "peer unresponsive past LCP_ECHO_MAX_FAIL triggers PPP_bye teardown", "");
    TEST_ASSERT(test_ppp_ccb.echo_miss_count == LCP_ECHO_MAX_FAIL,
        "no additional probe sent -- echo_miss_count left unchanged", "got %u", test_ppp_ccb.echo_miss_count);
}

/* ---- pppoe_send_pkt PADI/PADR retransmit budget ---- */

static void test_pppoe_send_pkt_padi_under_budget(void)
{
    printf("\nTesting pppoe_send_pkt (PADI under retransmit budget):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.pppoe_phase.max_retransmit = MAX_RETRAN;
    test_ppp_ccb.pppoe_phase.timer_counter = 0;

    STATUS ret = pppoe_send_pkt(ENCODE_PADI, &test_ppp_ccb);

    TEST_ASSERT(ret == SUCCESS, "PADI send under budget returns SUCCESS", "got %d", ret);
    TEST_ASSERT(test_ppp_ccb.pppoe_phase.timer_counter == 1,
        "timer_counter incremented", "got %u", test_ppp_ccb.pppoe_phase.timer_counter);
    TEST_ASSERT(test_ppp_ccb.phase == END_PHASE, "session not torn down while under budget", "");
}

static void test_pppoe_send_pkt_padi_exhausted(void)
{
    printf("\nTesting pppoe_send_pkt (PADI retransmit budget exhausted):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = PPPOE_PHASE;
    rte_atomic16_set(&test_ppp_ccb.ppp_bool, 1);
    test_ppp_ccb.pppoe_phase.max_retransmit = MAX_RETRAN;
    test_ppp_ccb.pppoe_phase.timer_counter = MAX_RETRAN;

    STATUS ret = pppoe_send_pkt(ENCODE_PADI, &test_ppp_ccb);

    TEST_ASSERT(ret == ERROR, "budget exhausted returns ERROR", "got %d", ret);
    TEST_ASSERT(test_ppp_ccb.phase == END_PHASE, "budget exhaustion tears down via exit_ppp", "got %u", test_ppp_ccb.phase);
    TEST_ASSERT(rte_atomic16_read(&test_ppp_ccb.ppp_bool) == 0, "ppp_bool cleared by exit_ppp", "");
}

static void test_pppoe_send_pkt_padr_under_budget(void)
{
    printf("\nTesting pppoe_send_pkt (PADR under retransmit budget):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.pppoe_phase.max_retransmit = MAX_RETRAN;
    test_ppp_ccb.pppoe_phase.timer_counter = 0;

    STATUS ret = pppoe_send_pkt(ENCODE_PADR, &test_ppp_ccb);

    TEST_ASSERT(ret == SUCCESS, "PADR send under budget returns SUCCESS", "got %d", ret);
    TEST_ASSERT(test_ppp_ccb.pppoe_phase.timer_counter == 1,
        "timer_counter incremented", "got %u", test_ppp_ccb.pppoe_phase.timer_counter);
}

static void test_pppoe_send_pkt_padr_exhausted(void)
{
    printf("\nTesting pppoe_send_pkt (PADR retransmit budget exhausted):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = PPPOE_PHASE;
    rte_atomic16_set(&test_ppp_ccb.ppp_bool, 1);
    test_ppp_ccb.pppoe_phase.max_retransmit = MAX_RETRAN;
    test_ppp_ccb.pppoe_phase.timer_counter = MAX_RETRAN;

    STATUS ret = pppoe_send_pkt(ENCODE_PADR, &test_ppp_ccb);

    TEST_ASSERT(ret == ERROR, "budget exhausted returns ERROR", "got %d", ret);
    TEST_ASSERT(test_ppp_ccb.phase == END_PHASE, "budget exhaustion tears down via exit_ppp", "got %u", test_ppp_ccb.phase);
}

void test_pppd(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           PPPD Unit Tests                                  ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    g_pppd_fastrg_ccb = fastrg_ccb;
    ppp_ccb_t *orig_ccb0 = fastrg_ccb->ppp_ccb[0];
    pppd_env_init(fastrg_ccb);

    test_ppp_connect_already_active();
    test_ppp_connect_from_end_phase();

    test_ppp_disconnect_from_end_phase();
    test_ppp_disconnect_while_processing();
    test_ppp_disconnect_normal();

    test_ppp_bye_end_phase();
    test_ppp_bye_pppoe_phase_recurses_to_end();
    test_ppp_bye_lcp_phase();
    test_ppp_bye_data_phase_downgrades_to_lcp();

    test_exit_ppp_resets_fields();
    test_exit_ppp_redial_pending_honored();
    test_exit_ppp_no_redial_when_not_pending();

    test_keepalive_probes_and_increments();
    test_keepalive_exceeds_threshold_tears_down();

    test_pppoe_send_pkt_padi_under_budget();
    test_pppoe_send_pkt_padi_exhausted();
    test_pppoe_send_pkt_padr_under_budget();
    test_pppoe_send_pkt_padr_exhausted();

    /* Leave no armed timer behind, and restore the shared ccb slot other
     * suites expect to find untouched. */
    rte_timer_stop_sync(&test_ppp_ccb.pppoe);
    rte_timer_stop_sync(&test_ppp_ccb.ppp);
    rte_timer_stop_sync(&test_ppp_ccb.ppp_alive);
    fastrg_ccb->ppp_ccb[0] = orig_ccb0;

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
