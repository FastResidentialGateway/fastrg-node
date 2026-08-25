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
    test_ppp_ccb.control_protocol[PPP_CP_LCP].state = S_INIT;
    test_ppp_ccb.control_protocol[PPP_CP_IPCP].state = S_INIT;
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
    TEST_ASSERT(test_ppp_ccb.cp_id == PPP_CP_LCP, "cp reset to LCP", "");
}

/* ---- PPP_bye state walk-down ---- */

static void test_ppp_bye_end_phase(void)
{
    printf("\nTesting PPP_bye (END_PHASE -> exit_ppp):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = END_PHASE;
    rte_atomic16_set(&test_ppp_ccb.ppp_bool, 1);

    PPP_bye(&test_ppp_ccb);

    TEST_ASSERT(rte_atomic16_read(&test_ppp_ccb.ppp_bool) == 0, "END_PHASE clears ppp_bool via exit_ppp", "");
    TEST_ASSERT(test_ppp_ccb.ppp_processing == FALSE, "ppp_processing left FALSE", "");
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
    test_ppp_ccb.cp_id = PPP_CP_IPCP;
    test_ppp_ccb.control_protocol[PPP_CP_IPCP].state = S_OPENED;

    PPP_bye(&test_ppp_ccb);

    TEST_ASSERT(test_ppp_ccb.ppp_processing == TRUE, "LCP_PHASE marks ppp_processing", "");
    TEST_ASSERT(test_ppp_ccb.cp_id == PPP_CP_LCP, "cp forced back to LCP (0)", "");
    TEST_ASSERT(test_ppp_ccb.control_protocol[PPP_CP_IPCP].state == S_INIT,
        "NCP state force-reset to S_INIT", "got %u", test_ppp_ccb.control_protocol[PPP_CP_IPCP].state);
}

static void test_ppp_bye_data_phase_downgrades_to_lcp(void)
{
    printf("\nTesting PPP_bye (DATA_PHASE downgrades straight to LCP_PHASE):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = DATA_PHASE;
    rte_atomic16_set(&test_ppp_ccb.dp_start_bool, 1);
    test_ppp_ccb.cp_id = PPP_CP_IPCP;

    PPP_bye(&test_ppp_ccb);

    TEST_ASSERT(test_ppp_ccb.phase == LCP_PHASE,
        "DATA_PHASE downgrades to LCP_PHASE (RFC 1661 SS3.7 -- LCP close is sufficient)",
        "got %u", test_ppp_ccb.phase);
    TEST_ASSERT(rte_atomic16_read(&test_ppp_ccb.dp_start_bool) == 0, "dp_start_bool cleared", "");
    TEST_ASSERT(test_ppp_ccb.cp_id == PPP_CP_LCP, "cp forced back to LCP (0)", "");
}

/* ---- exit_ppp ---- */

static void test_exit_ppp_resets_fields(void)
{
    printf("\nTesting exit_ppp (field reset):\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = LCP_PHASE;
    rte_atomic16_set(&test_ppp_ccb.ppp_bool, 1);
    test_ppp_ccb.control_protocol[PPP_CP_LCP].state = S_OPENED;
    test_ppp_ccb.control_protocol[PPP_CP_IPCP].state = S_OPENED;
    test_ppp_ccb.pppoe_phase.active = TRUE;
    test_ppp_ccb.hsi_ipv4 = 0xC0A80101;
    test_ppp_ccb.hsi_ipv4_gw = 0xC0A80001;
    test_ppp_ccb.hsi_primary_dns = 0x08080808;
    test_ppp_ccb.hsi_secondary_dns = 0x01010101;

    exit_ppp(&test_ppp_ccb);

    TEST_ASSERT(rte_atomic16_read(&test_ppp_ccb.ppp_bool) == 0, "ppp_bool cleared", "");
    TEST_ASSERT(test_ppp_ccb.phase == END_PHASE, "phase set to END_PHASE", "");
    TEST_ASSERT(test_ppp_ccb.control_protocol[PPP_CP_LCP].state == S_INIT && test_ppp_ccb.control_protocol[PPP_CP_IPCP].state == S_INIT,
        "both cp states reset to S_INIT", "");
    TEST_ASSERT(test_ppp_ccb.pppoe_phase.active == FALSE, "pppoe_phase.active cleared", "");
    TEST_ASSERT(test_ppp_ccb.hsi_ipv4 == 0 && test_ppp_ccb.hsi_ipv4_gw == 0,
        "assigned IPs cleared", "");
    TEST_ASSERT(test_ppp_ccb.hsi_primary_dns == 0xffffffff && test_ppp_ccb.hsi_secondary_dns == 0xffffffff,
        "DNS reset to unassigned sentinel (0xffffffff)", "");
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


/* ---- fixed-max refactor case (moved from the task-scoped file into the
 * module test file per repo naming convention; additive only) ---- */

static void test_credentials_overwrite(void)
{
    printf("\nTesting ppp_update_config_by_user (credential replacement):\n");
    printf("=========================================\n\n");

    /* Heap fixture: ppp_ccb_t embeds the ~16MB NAT slot pool. */
    ppp_ccb_t *ccb = fastrg_calloc(ppp_ccb_t, 1, sizeof(ppp_ccb_t), 0);
    TEST_ASSERT(ccb != NULL, "allocate credential fixture ccb", "");
    if (ccb == NULL)
        return;
    ccb->fastrg_ccb = g_pppd_fastrg_ccb;
    rte_spinlock_init(&ccb->cred_lock);
    rte_atomic16_init(&ccb->vlan_id);

    TEST_ASSERT(ppp_update_config_by_user(ccb, 100, "user-one", "pass-one") == SUCCESS,
        "initial credential set succeeds", "");
    TEST_ASSERT(ccb->ppp_user_acc != NULL &&
        strcmp((const char *)ccb->ppp_user_acc, "user-one") == 0,
        "account stored", "");
    TEST_ASSERT(ccb->ppp_passwd != NULL &&
        strcmp((const char *)ccb->ppp_passwd, "pass-one") == 0,
        "password stored", "");
    TEST_ASSERT(rte_atomic16_read(&ccb->vlan_id) == 100, "vlan applied", "");

    /* equal-length overwrite */
    TEST_ASSERT(ppp_update_config_by_user(ccb, 101, "user-two", "pass-two") == SUCCESS,
        "equal-length overwrite succeeds", "");
    TEST_ASSERT(strcmp((const char *)ccb->ppp_user_acc, "user-two") == 0 &&
        strcmp((const char *)ccb->ppp_passwd, "pass-two") == 0,
        "equal-length overwrite replaces both strings", "");

    /* longer strings force reallocation (unbounded length support) */
    const char *long_acc = "a-considerably-longer-account-name-with-no-upper-bound";
    const char *long_pwd = "a-considerably-longer-password-value-with-no-upper-bound";
    TEST_ASSERT(ppp_update_config_by_user(ccb, 102, long_acc, long_pwd) == SUCCESS,
        "longer credential replacement succeeds", "");
    TEST_ASSERT(strcmp((const char *)ccb->ppp_user_acc, long_acc) == 0 &&
        strcmp((const char *)ccb->ppp_passwd, long_pwd) == 0,
        "longer credentials stored intact", "");

    /* shrink back */
    TEST_ASSERT(ppp_update_config_by_user(ccb, 103, "u", "p") == SUCCESS,
        "shrinking credential replacement succeeds", "");
    TEST_ASSERT(strcmp((const char *)ccb->ppp_user_acc, "u") == 0 &&
        strcmp((const char *)ccb->ppp_passwd, "p") == 0,
        "short credentials stored intact", "");

    fastrg_mfree(ccb->ppp_user_acc);
    fastrg_mfree(ccb->ppp_passwd);
    fastrg_mfree(ccb);
}

static void test_port_fwd_entry_packing(void)
{
    printf("\nTesting port_fwd_entry_t layout:\n");
    printf("=========================================\n\n");

    /* Regression guard: the port forwarding table is direct-indexed by eport,
     * so every byte of padding costs 64 KiB per subscriber.  A stray
     * __rte_cache_aligned here silently inflated the table from 1 MiB to
     * 4 MiB per ppp_ccb. */
    TEST_ASSERT(sizeof(port_fwd_entry_t) == 16,
        "port_fwd_entry_t stays packed to 16 bytes", "got %zu",
        sizeof(port_fwd_entry_t));
}

/* ---- dp_start_bool gate ---- */

static void test_dp_gate_open_tracks_flag(void)
{
    printf("\nTesting pppd_dp_gate_open:\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();

    /* The read barrier inside the helper cannot be observed from a
     * single-threaded test. What this pins down is the interface every
     * data-plane read point depends on: the helper answers exactly the
     * dp_start_bool state, so a closed gate still means "drop the packet". */
    rte_atomic16_set(&test_ppp_ccb.dp_start_bool, 0);
    TEST_ASSERT(pppd_dp_gate_open(&test_ppp_ccb) == FALSE,
        "closed gate reports FALSE", "");

    rte_atomic16_set(&test_ppp_ccb.dp_start_bool, 1);
    TEST_ASSERT(pppd_dp_gate_open(&test_ppp_ccb) == TRUE,
        "open gate reports TRUE", "");
}

/* ---- IPv6 northbound string formatting ---- */

/* 2001:db8:ab00:: */
static const U8 test_pd_prefix[16] = { 0x20, 0x01, 0x0d, 0xb8, 0xab, 0x00 };
/* 2001:4860:4860::8888 */
static const U8 test_dns1[16] = { 0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0x88, 0x88 };
/* 2606:4700:4700::1111 */
static const U8 test_dns2[16] = { 0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0x11, 0x11 };
static const U8 test_local_iid[8] = { 0x00, 0x11, 0x22, 0xff,
                                      0xfe, 0x33, 0x44, 0x55 };

static void test_ipv6_report_strings_full_lease(void)
{
    char addr[PPPD_IPV6_ADDR_STRLEN];
    char prefix[PPPD_IPV6_PREFIX_STRLEN];
    char dns[PPPD_IPV6_DNS_STRLEN];

    printf("\nTesting pppd_ipv6_report_strings with a full lease:\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    memcpy(test_ppp_ccb.ipv6cp_local_iid, test_local_iid,
        sizeof(test_local_iid));
    memcpy(test_ppp_ccb.hsi_ipv6_pd_prefix, test_pd_prefix,
        sizeof(test_pd_prefix));
    test_ppp_ccb.hsi_ipv6_pd_plen = 56;
    memcpy(test_ppp_ccb.hsi_ipv6_dns[0], test_dns1, sizeof(test_dns1));
    memcpy(test_ppp_ccb.hsi_ipv6_dns[1], test_dns2, sizeof(test_dns2));

    pppd_ipv6_report_strings(&test_ppp_ccb, addr, sizeof(addr), prefix,
        sizeof(prefix), dns, sizeof(dns));

    TEST_ASSERT(strcmp(addr, "fe80::11:22ff:fe33:4455") == 0,
        "WAN address is the IPV6CP interface identifier under fe80::",
        "got %s", addr);
    TEST_ASSERT(strcmp(prefix, "2001:db8:ab00::/56") == 0,
        "delegated prefix is reported in CIDR form", "got %s", prefix);
    TEST_ASSERT(strcmp(dns, "2001:4860:4860::8888,2606:4700:4700::1111") == 0,
        "two DNS servers are comma-separated without spaces", "got %s", dns);
}

static void test_ipv6_report_strings_single_dns(void)
{
    char addr[PPPD_IPV6_ADDR_STRLEN];
    char prefix[PPPD_IPV6_PREFIX_STRLEN];
    char dns[PPPD_IPV6_DNS_STRLEN];

    printf("\nTesting pppd_ipv6_report_strings with one DNS server:\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    memcpy(test_ppp_ccb.ipv6cp_local_iid, test_local_iid,
        sizeof(test_local_iid));
    memcpy(test_ppp_ccb.hsi_ipv6_pd_prefix, test_pd_prefix,
        sizeof(test_pd_prefix));
    test_ppp_ccb.hsi_ipv6_pd_plen = 60;
    memcpy(test_ppp_ccb.hsi_ipv6_dns[0], test_dns1, sizeof(test_dns1));
    /* hsi_ipv6_dns[1] is left all zero -- an unused server slot. */

    pppd_ipv6_report_strings(&test_ppp_ccb, addr, sizeof(addr), prefix,
        sizeof(prefix), dns, sizeof(dns));

    TEST_ASSERT(strcmp(prefix, "2001:db8:ab00::/60") == 0,
        "prefix length is reported as delegated, not assumed to be 56",
        "got %s", prefix);
    TEST_ASSERT(strcmp(dns, "2001:4860:4860::8888") == 0,
        "an unused DNS slot adds no separator", "got %s", dns);
}

static void test_ipv6_report_strings_no_dns(void)
{
    char addr[PPPD_IPV6_ADDR_STRLEN];
    char prefix[PPPD_IPV6_PREFIX_STRLEN];
    char dns[PPPD_IPV6_DNS_STRLEN];

    printf("\nTesting pppd_ipv6_report_strings with no DNS server:\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    memcpy(test_ppp_ccb.ipv6cp_local_iid, test_local_iid,
        sizeof(test_local_iid));
    memcpy(test_ppp_ccb.hsi_ipv6_pd_prefix, test_pd_prefix,
        sizeof(test_pd_prefix));
    test_ppp_ccb.hsi_ipv6_pd_plen = 56;

    pppd_ipv6_report_strings(&test_ppp_ccb, addr, sizeof(addr), prefix,
        sizeof(prefix), dns, sizeof(dns));

    TEST_ASSERT(dns[0] == '\0',
        "a lease without DNS servers reports an empty string", "got %s", dns);
    TEST_ASSERT(strcmp(addr, "fe80::11:22ff:fe33:4455") == 0,
        "address is still reported when DNS is absent", "got %s", addr);
}

/* ---- PPPoE state report built from the control block ---- */

/* Put the fixture in the data phase with a full IPv4 + IPv6 session, which is
 * what every "connected" report is built from. */
static void state_report_ccb_connected(void)
{
    pppd_ccb_reset();
    test_ppp_ccb.phase = DATA_PHASE;
    test_ppp_ccb.hsi_ipv4 = htonl(0x0a000002);    /* 10.0.0.2 */
    test_ppp_ccb.hsi_ipv4_gw = htonl(0x0a000001); /* 10.0.0.1 */
    memcpy(test_ppp_ccb.ipv6cp_local_iid, test_local_iid,
        sizeof(test_local_iid));
    memcpy(test_ppp_ccb.hsi_ipv6_pd_prefix, test_pd_prefix,
        sizeof(test_pd_prefix));
    test_ppp_ccb.hsi_ipv6_pd_plen = 56;
    memcpy(test_ppp_ccb.hsi_ipv6_dns[0], test_dns1, sizeof(test_dns1));
    memcpy(test_ppp_ccb.hsi_ipv6_dns[1], test_dns2, sizeof(test_dns2));
    test_ppp_ccb.ipv6_enabled = TRUE;
    test_ppp_ccb.ipv6cp_up = TRUE;
    test_ppp_ccb.dhcp6_pd_ready = TRUE;
    pppd_ipv6_dp_gate_update(&test_ppp_ccb);
}

static void test_state_report_connected_with_ipv6(void)
{
    ppp_state_report_t report;

    printf("\nTesting ppp_build_state_report in the data phase with IPv6:\n");
    printf("=========================================\n\n");

    state_report_ccb_connected();
    ppp_build_state_report(&test_ppp_ccb, &report);

    TEST_ASSERT(report.phase == PPP_REPORT_CONNECTED,
        "a data-phase session reports the connected phase", "got %d",
        (int)report.phase);
    TEST_ASSERT(strcmp(report.user_id, "1") == 0,
        "the subscriber id is reported as a decimal string", "got %s",
        report.user_id);
    TEST_ASSERT(strcmp(report.ipv4, "10.0.0.2") == 0,
        "the assigned IPv4 address is reported", "got %s", report.ipv4);
    TEST_ASSERT(strcmp(report.ipv4_gw, "10.0.0.1") == 0,
        "the IPv4 gateway is reported", "got %s", report.ipv4_gw);
    TEST_ASSERT(strcmp(report.ipv6_addr, "fe80::11:22ff:fe33:4455") == 0,
        "the WAN IPv6 address is reported", "got %s", report.ipv6_addr);
    TEST_ASSERT(strcmp(report.ipv6_pd_prefix, "2001:db8:ab00::/56") == 0,
        "the delegated prefix is reported", "got %s", report.ipv6_pd_prefix);
    TEST_ASSERT(strcmp(report.ipv6_dns,
            "2001:4860:4860::8888,2606:4700:4700::1111") == 0,
        "the IPv6 DNS servers are reported", "got %s", report.ipv6_dns);
}

static void test_state_report_connected_ipv6_gate_closed(void)
{
    ppp_state_report_t report;

    printf("\nTesting ppp_build_state_report with the IPv6 gate closed:\n");
    printf("=========================================\n\n");

    state_report_ccb_connected();
    /* A lease being renewed closes the gate while the lease fields still hold
     * the previous values. */
    test_ppp_ccb.dhcp6_pd_ready = FALSE;
    pppd_ipv6_dp_gate_update(&test_ppp_ccb);
    ppp_build_state_report(&test_ppp_ccb, &report);

    TEST_ASSERT(report.phase == PPP_REPORT_CONNECTED,
        "IPv4 stays connected while IPv6 is unavailable", "got %d",
        (int)report.phase);
    TEST_ASSERT(strcmp(report.ipv4, "10.0.0.2") == 0,
        "the IPv4 address is still reported", "got %s", report.ipv4);
    TEST_ASSERT(report.ipv6_addr[0] == '\0' &&
            report.ipv6_pd_prefix[0] == '\0' && report.ipv6_dns[0] == '\0',
        "a closed IPv6 gate reports no IPv6 fields",
        "got %s / %s / %s", report.ipv6_addr, report.ipv6_pd_prefix,
        report.ipv6_dns);
}

static void test_state_report_disconnected(void)
{
    ppp_state_report_t report;

    printf("\nTesting ppp_build_state_report after a session ended:\n");
    printf("=========================================\n\n");

    state_report_ccb_connected();
    test_ppp_ccb.phase = END_PHASE;
    ppp_build_state_report(&test_ppp_ccb, &report);

    TEST_ASSERT(report.phase == PPP_REPORT_DISCONNECTED,
        "a subscriber with no session reports the disconnected phase",
        "got %d", (int)report.phase);
    TEST_ASSERT(report.ipv4[0] == '\0' && report.ipv4_gw[0] == '\0',
        "no IPv4 address is reported without a session",
        "got %s / %s", report.ipv4, report.ipv4_gw);
    TEST_ASSERT(report.ipv6_addr[0] == '\0' &&
            report.ipv6_pd_prefix[0] == '\0' && report.ipv6_dns[0] == '\0',
        "no IPv6 fields are reported without a session",
        "got %s / %s / %s", report.ipv6_addr, report.ipv6_pd_prefix,
        report.ipv6_dns);
}

static void test_state_report_not_configured(void)
{
    ppp_state_report_t report;

    printf("\nTesting ppp_build_state_report for an unconfigured subscriber:\n");
    printf("=========================================\n\n");

    pppd_ccb_reset();
    test_ppp_ccb.phase = NOT_CONFIGURED;
    test_ppp_ccb.user_num = 7;
    ppp_build_state_report(&test_ppp_ccb, &report);

    TEST_ASSERT(report.phase == PPP_REPORT_DISCONNECTED,
        "an unconfigured subscriber reports the disconnected phase",
        "got %d", (int)report.phase);
    TEST_ASSERT(strcmp(report.user_id, "7") == 0,
        "the subscriber id follows user_num", "got %s", report.user_id);
}

static void test_state_report_negotiating(void)
{
    ppp_state_report_t report;

    printf("\nTesting ppp_build_state_report while the session is negotiating:\n");
    printf("=========================================\n\n");

    state_report_ccb_connected();
    test_ppp_ccb.phase = IPCP_PHASE;
    ppp_build_state_report(&test_ppp_ccb, &report);

    TEST_ASSERT(report.phase == PPP_REPORT_CONNECTING,
        "a session still negotiating reports the connecting phase", "got %d",
        (int)report.phase);
    TEST_ASSERT(report.ipv4[0] == '\0' && report.ipv6_addr[0] == '\0',
        "no addresses are reported before the session carries data",
        "got %s / %s", report.ipv4, report.ipv6_addr);
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

    test_credentials_overwrite();

    test_port_fwd_entry_packing();

    test_dp_gate_open_tracks_flag();

    test_ipv6_report_strings_full_lease();
    test_ipv6_report_strings_single_dns();
    test_ipv6_report_strings_no_dns();

    test_state_report_connected_with_ipv6();
    test_state_report_connected_ipv6_gate_closed();
    test_state_report_disconnected();
    test_state_report_not_configured();
    test_state_report_negotiating();

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
