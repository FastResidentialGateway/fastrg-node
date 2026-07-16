#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <common.h>

#include <rte_timer.h>
#include <rte_cycles.h>
#include <rte_ether.h>

#include "../../src/fastrg.h"
#include "../../src/pppd/fsm.h"
#include "../../src/pppd/pppd.h"
#include "../../src/pppd/codec.h"
#include "../../src/fastrg.h"
#include "../test_helper.h"

// Global test counters
static int test_count = 0;
static int pass_count = 0;

static ppp_ccb_t* create_test_ppp_ccb(FastRG_t *fastrg_ccb, U8 cp, U8 state) {
    ppp_ccb_t *ccb = (ppp_ccb_t*)calloc(1, sizeof(ppp_ccb_t));
    assert(ccb != NULL);

    ccb->cp = cp;
    ccb->ppp_phase[cp].state = state;
    ccb->user_num = 1;
    ccb->session_id = rte_cpu_to_be_16(0x1234);
    ccb->ppp_phase[cp].timer_counter = 10;
    ccb->ppp_phase[cp].ppp_payload.ppp_protocol = 
        rte_cpu_to_be_16(cp == 0 ? LCP_PROTOCOL : IPCP_PROTOCOL);
    ccb->fastrg_ccb = fastrg_ccb;

    return ccb;
}

static void free_test_ppp_ccb(ppp_ccb_t *ccb) {
    if (ccb) {
        if (ccb->ppp_phase[0].ppp_options)
            free(ccb->ppp_phase[0].ppp_options);
        if (ccb->ppp_phase[1].ppp_options)
            free(ccb->ppp_phase[1].ppp_options);
        free(ccb);
    }
}

// ============================================================================
// Test cases: Basic state transitions
// ============================================================================

void test_fsm_init_to_closed(FastRG_t *fastrg_ccb)
{
    printf("\nTest 1: \"INIT -> CLOSED (E_UP)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_INIT);
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_UP);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS", "got %d", result);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_CLOSED, 
        "State transitions to S_CLOSED", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_init_to_starting(FastRG_t *fastrg_ccb)
{
    printf("\nTest 2: \"INIT -> STARTING (E_OPEN)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_INIT);
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_OPEN);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS", "got %d", result);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_STARTING, 
        "State transitions to S_STARTING", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_starting_to_request_sent(FastRG_t *fastrg_ccb)
{
    printf("\nTest 3: \"STARTING -> REQUEST_SENT (E_UP)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_STARTING);
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_UP);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS", "got %d", result);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_REQUEST_SENT, 
        "State transitions to S_REQUEST_SENT", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_closed_to_request_sent(FastRG_t *fastrg_ccb)
{
    printf("\nTest 4: \"CLOSED -> REQUEST_SENT (E_OPEN)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_CLOSED);
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_OPEN);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS", "got %d", result);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_REQUEST_SENT, 
        "State transitions to S_REQUEST_SENT", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_request_sent_to_ack_received(FastRG_t *fastrg_ccb)
{
    printf("\nTest 5: \"REQUEST_SENT -> ACK_RECEIVED (E_RECV_CONFIG_ACK)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_REQUEST_SENT);
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_RECV_CONFIG_ACK);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS", "got %d", result);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_ACK_RECEIVED, 
        "State transitions to S_ACK_RECEIVED", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_ack_received_to_opened(FastRG_t *fastrg_ccb)
{
    printf("\nTest 6: \"ACK_RECEIVED -> OPENED (E_RECV_GOOD_CONFIG_REQUEST)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_ACK_RECEIVED);
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_RECV_GOOD_CONFIG_REQUEST);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS", "got %d", result);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_OPENED, 
        "State transitions to S_OPENED", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_ack_sent_to_opened(FastRG_t *fastrg_ccb)
{
    printf("\nTest 7: \"ACK_SENT -> OPENED (E_RECV_CONFIG_ACK)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_ACK_SENT);
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_RECV_CONFIG_ACK);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS", "got %d", result);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_OPENED, 
        "State transitions to S_OPENED", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_opened_to_closing(FastRG_t *fastrg_ccb)
{
    printf("\nTest 8: \"OPENED -> CLOSING (E_CLOSE)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_OPENED);
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_CLOSE);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS", "got %d", result);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_CLOSING, 
        "State transitions to S_CLOSING", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_closing_to_closed(FastRG_t *fastrg_ccb)
{
    printf("\nTest 9: \"CLOSING -> CLOSED (E_RECV_TERMINATE_ACK)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_CLOSING);
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_RECV_TERMINATE_ACK);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS", "got %d", result);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_CLOSED, 
        "State transitions to S_CLOSED", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

// ============================================================================
// Test cases: Error handling
// ============================================================================

void test_fsm_null_ccb(FastRG_t *fastrg_ccb)
{
    printf("\nTest 10: \"NULL CCB handling\"\n");
    printf("=========================================\n\n");

    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, NULL, E_UP);

    TEST_ASSERT(result == ERROR, "PPP_FSM returns ERROR for NULL CCB", "got %d", result);
}

void test_fsm_invalid_state(FastRG_t *fastrg_ccb)
{
    printf("\nTest 11: \"Invalid state handling\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, 99); // Invalid state
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_UP);

    TEST_ASSERT(result == ERROR, "PPP_FSM returns ERROR for invalid state", "got %d", result);

    free_test_ppp_ccb(ccb);
}

void test_fsm_invalid_event_in_valid_state(FastRG_t *fastrg_ccb)
{
    printf("\nTest 12: \"Invalid event in valid state\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_INIT);
    struct rte_timer timer = {0};
    U8 initial_state = ccb->ppp_phase[0].state;

    // E_RECV_CONFIG_ACK is invalid in INIT state
    STATUS result = PPP_FSM(&timer, ccb, E_RECV_CONFIG_ACK);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS (but does nothing)", "got %d", result);
    TEST_ASSERT(ccb->ppp_phase[0].state == initial_state, 
        "State unchanged for invalid event", "expected %d, got %d", initial_state, ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

// ============================================================================
// Test cases: LCP vs IPCP
// ============================================================================

void test_fsm_lcp_phase(FastRG_t *fastrg_ccb)
{
    printf("\nTest 13: \"LCP phase (cp=0)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_INIT); // cp=0 (LCP)
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_OPEN);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS", "got %d", result);
    TEST_ASSERT(ccb->cp == 0, "CCB indicates LCP phase", "got cp=%d", ccb->cp);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_STARTING, 
        "LCP state transitions correctly", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_ipcp_phase(FastRG_t *fastrg_ccb)
{
    printf("\nTest 14: \"IPCP phase (cp=1)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 1, S_INIT); // cp=1 (IPCP)
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_OPEN);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS", "got %d", result);
    TEST_ASSERT(ccb->cp == 1, "CCB indicates IPCP phase", "got cp=%d", ccb->cp);
    TEST_ASSERT(ccb->ppp_phase[1].state == S_STARTING, 
        "IPCP state transitions correctly", "got state %d", ccb->ppp_phase[1].state);

    free_test_ppp_ccb(ccb);
}

// ============================================================================
// Test cases: Complete flows
// ============================================================================

void test_fsm_full_connection_establishment(FastRG_t *fastrg_ccb)
{
    printf("\nTest 15: \"Full connection establishment flow\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_INIT);
    struct rte_timer timer = {0};

    // INIT -> STARTING (E_OPEN)
    PPP_FSM(&timer, ccb, E_OPEN);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_STARTING, 
        "Step 1: INIT -> STARTING", "got state %d", ccb->ppp_phase[0].state);

    // STARTING -> REQUEST_SENT (E_UP)
    PPP_FSM(&timer, ccb, E_UP);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_REQUEST_SENT, 
        "Step 2: STARTING -> REQUEST_SENT", "got state %d", ccb->ppp_phase[0].state);

    // REQUEST_SENT -> ACK_RECEIVED (E_RECV_CONFIG_ACK)
    PPP_FSM(&timer, ccb, E_RECV_CONFIG_ACK);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_ACK_RECEIVED, 
                "Step 3: REQUEST_SENT -> ACK_RECEIVED", "got state %d", ccb->ppp_phase[0].state);

    // ACK_RECEIVED -> OPENED (E_RECV_GOOD_CONFIG_REQUEST)
    PPP_FSM(&timer, ccb, E_RECV_GOOD_CONFIG_REQUEST);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_OPENED, 
                "Step 4: ACK_RECEIVED -> OPENED", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_full_connection_termination(FastRG_t *fastrg_ccb)
{
    printf("\nTest 16: \"Full connection termination flow\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_OPENED);
    struct rte_timer timer = {0};

    // OPENED -> CLOSING (E_CLOSE)
    PPP_FSM(&timer, ccb, E_CLOSE);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_CLOSING, 
        "Step 1: OPENED -> CLOSING", "got state %d", ccb->ppp_phase[0].state);

    // CLOSING -> CLOSED (E_RECV_TERMINATE_ACK)
    PPP_FSM(&timer, ccb, E_RECV_TERMINATE_ACK);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_CLOSED, 
        "Step 2: CLOSING -> CLOSED", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_alternate_path_to_opened(FastRG_t *fastrg_ccb)
{
    printf("\nTest 17: \"Alternate path to OPENED (via ACK_SENT)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_REQUEST_SENT);
    struct rte_timer timer = {0};

    // REQUEST_SENT -> ACK_SENT (E_RECV_GOOD_CONFIG_REQUEST)
    PPP_FSM(&timer, ccb, E_RECV_GOOD_CONFIG_REQUEST);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_ACK_SENT, 
        "Step 1: REQUEST_SENT -> ACK_SENT", "got state %d", ccb->ppp_phase[0].state);

    // ACK_SENT -> OPENED (E_RECV_CONFIG_ACK)
    PPP_FSM(&timer, ccb, E_RECV_CONFIG_ACK);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_OPENED, 
        "Step 2: ACK_SENT -> OPENED", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

// ============================================================================
// Test cases: Special scenarios
// ============================================================================

void test_fsm_timer_counter_reset(FastRG_t *fastrg_ccb)
{
    printf("\nTest 18: \"Timer counter reset\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_INIT);
    struct rte_timer timer = {0};

    ccb->ppp_phase[0].timer_counter = 5;

    PPP_FSM(&timer, ccb, E_OPEN);

    TEST_ASSERT(ccb->ppp_phase[0].timer_counter == 10, 
        "Timer counter reset to 10", "got %d", ccb->ppp_phase[0].timer_counter);

    free_test_ppp_ccb(ccb);
}

void test_fsm_closed_down_event(FastRG_t *fastrg_ccb)
{
    printf("\nTest 19: \"CLOSED -> INIT (E_DOWN)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_CLOSED);
    struct rte_timer timer = {0};

    PPP_FSM(&timer, ccb, E_DOWN);

    TEST_ASSERT(ccb->ppp_phase[0].state == S_INIT, 
        "State transitions to INIT", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_request_sent_bad_config_request(FastRG_t *fastrg_ccb)
{
    printf("\nTest 20: \"REQUEST_SENT -> REQUEST_SENT (E_RECV_BAD_CONFIG_REQUEST)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_REQUEST_SENT);
    struct rte_timer timer = {0};

    PPP_FSM(&timer, ccb, E_RECV_BAD_CONFIG_REQUEST);

    TEST_ASSERT(ccb->ppp_phase[0].state == S_REQUEST_SENT, 
        "State remains REQUEST_SENT", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_ack_received_invalid_ack(FastRG_t *fastrg_ccb)
{
    printf("\nTest 21: \"ACK_RECEIVED -> REQUEST_SENT (invalid CONFIG_ACK)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_ACK_RECEIVED);
    struct rte_timer timer = {0};

    // Receiving CONFIG_ACK in ACK_RECEIVED state is invalid (RFC 1661)
    PPP_FSM(&timer, ccb, E_RECV_CONFIG_ACK);

    TEST_ASSERT(ccb->ppp_phase[0].state == S_REQUEST_SENT, 
        "State transitions back to REQUEST_SENT", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_user_num_zero(FastRG_t *fastrg_ccb)
{
    printf("\nTest 22: \"Handle user_num = 0\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_INIT);
    struct rte_timer timer = {0};

    ccb->user_num = 0;

    STATUS result = PPP_FSM(&timer, ccb, E_UP);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM handles user_num=0", "got %d", result);

    free_test_ppp_ccb(ccb);
}

void test_fsm_max_cp_value(FastRG_t *fastrg_ccb)
{
    printf("\nTest 23: \"Handle max CP value (IPCP)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 1, S_INIT); // cp=1 (IPCP)
    struct rte_timer timer = {0};

    STATUS result = PPP_FSM(&timer, ccb, E_UP);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM handles cp=1", "got %d", result);
    TEST_ASSERT(ccb->ppp_phase[1].state == S_CLOSED, 
        "IPCP state transitions correctly", "got state %d", ccb->ppp_phase[1].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_stopped_open_event(FastRG_t *fastrg_ccb)
{
    printf("\nTest 24: \"STOPPED -> STOPPED (E_OPEN with restart)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_STOPPED);
    struct rte_timer timer = {0};

    PPP_FSM(&timer, ccb, E_OPEN);

    TEST_ASSERT(ccb->ppp_phase[0].state == S_STOPPED, 
        "State remains STOPPED", "got state %d", ccb->ppp_phase[0].state);

    free_test_ppp_ccb(ccb);
}

// ============================================================================
// Test cases: exhaustive table scan (secondary transitions)
// ============================================================================

/**
 * Test 25: exhaustive (cp, state, event) scan of both PPP FSM tables.
 * Mirrors ppp_fsm_tbl_lcp / ppp_fsm_tbl_ncp as an expected-next-state matrix
 * and drives PPP_FSM through every combination — this pins all the secondary
 * transitions the targeted tests above don't touch (STOPPED / STOPPING /
 * CLOSING rows, echo events, unknown code, good/bad code-protocol-reject,
 * timeout counter positive/expired).  Under UNIT_TEST action handlers are
 * compiled out, so next_state and the return value are the observable
 * contract.  Undefined (state, event) combos must return SUCCESS and leave
 * the state unchanged.  The two tables differ only in handler chains, not in
 * next_state, so one matrix serves both cp values.
 */
static void test_fsm_full_table_scan(FastRG_t *fastrg_ccb)
{
    printf("\nTest 25: \"Exhaustive LCP+NCP (state x event) table scan\"\n");
    printf("=========================================\n\n");

    #define STAY 0xFF /* no transition defined: state must not change */
    /* expected[state][event], event order per PPP_EVENT_TYPE (E_UNKNOWN
     * included last — a log-only event that must never move the FSM) */
    static const U8 expected[S_INVLD][E_UNKNOWN + 1] = {
        /*                UP              DOWN         OPEN            CLOSE      TPOS            TEXP       GOOD_CR         BAD_CR          ACK             NAK_REJ         TERM_REQ        TERM_ACK        UNK_CODE   GOOD_REJ        BAD_REJ     ECHO_REQ  ECHO_REP  E_UNKNOWN */
        [S_INIT]         = { S_CLOSED,       STAY,        S_STARTING,     S_INIT,    STAY,           STAY,      STAY,           STAY,           STAY,           STAY,           STAY,           STAY,           STAY,      STAY,           STAY,       STAY,     STAY,     STAY },
        [S_STARTING]     = { S_REQUEST_SENT, STAY,        S_STARTING,     S_INIT,    STAY,           STAY,      STAY,           STAY,           STAY,           STAY,           STAY,           STAY,           STAY,      STAY,           STAY,       STAY,     STAY,     STAY },
        [S_CLOSED]       = { S_CLOSED,       S_INIT,      S_REQUEST_SENT, S_CLOSED,  STAY,           STAY,      S_CLOSED,       S_CLOSED,       S_CLOSED,       S_CLOSED,       S_CLOSED,       S_CLOSED,       S_CLOSED,  S_CLOSED,       S_CLOSED,   S_CLOSED, S_CLOSED, STAY },
        [S_STOPPED]      = { STAY,           S_STARTING,  S_STOPPED,      S_CLOSED,  STAY,           STAY,      S_ACK_SENT,     S_REQUEST_SENT, S_STOPPED,      S_STOPPED,      S_STOPPED,      S_STOPPED,      S_STOPPED, S_STOPPED,      S_STOPPED,  S_STOPPED, S_STOPPED, STAY },
        [S_CLOSING]      = { STAY,           S_INIT,      S_STOPPING,     S_CLOSING, S_CLOSING,      S_CLOSED,  S_CLOSING,      S_CLOSING,      S_CLOSING,      S_CLOSING,      S_CLOSING,      S_CLOSED,       S_CLOSING, S_CLOSING,      S_CLOSED,   S_CLOSING, S_CLOSING, STAY },
        [S_STOPPING]     = { STAY,           S_STARTING,  S_STOPPING,     S_CLOSING, S_STOPPING,     S_STOPPED, S_STOPPING,     S_STOPPING,     S_STOPPING,     S_STOPPING,     S_STOPPING,     S_STOPPED,      S_STOPPING, S_STOPPING,    S_STOPPED,  S_STOPPING, S_STOPPING, STAY },
        [S_REQUEST_SENT] = { STAY,           S_STARTING,  S_REQUEST_SENT, S_CLOSING, S_REQUEST_SENT, S_STOPPED, S_ACK_SENT,     S_REQUEST_SENT, S_ACK_RECEIVED, S_REQUEST_SENT, S_REQUEST_SENT, S_REQUEST_SENT, S_REQUEST_SENT, S_REQUEST_SENT, S_STOPPED, S_REQUEST_SENT, S_REQUEST_SENT, STAY },
        [S_ACK_RECEIVED] = { STAY,           S_STARTING,  S_ACK_RECEIVED, S_CLOSING, S_REQUEST_SENT, S_STOPPED, S_OPENED,       S_ACK_RECEIVED, S_REQUEST_SENT, S_REQUEST_SENT, S_REQUEST_SENT, S_REQUEST_SENT, S_ACK_RECEIVED, S_REQUEST_SENT, S_STOPPED, S_ACK_RECEIVED, S_ACK_RECEIVED, STAY },
        [S_ACK_SENT]     = { STAY,           S_STARTING,  S_ACK_SENT,     S_CLOSING, S_ACK_SENT,     S_STOPPED, S_ACK_SENT,     S_REQUEST_SENT, S_OPENED,       S_ACK_SENT,     S_REQUEST_SENT, S_ACK_SENT,     S_ACK_SENT, S_ACK_SENT,    S_STOPPED,  S_ACK_SENT, S_ACK_SENT, STAY },
        [S_OPENED]       = { STAY,           S_STARTING,  S_OPENED,       S_CLOSING, STAY,           STAY,      S_ACK_SENT,     S_REQUEST_SENT, S_REQUEST_SENT, S_REQUEST_SENT, S_STOPPING,     S_REQUEST_SENT, S_OPENED,  S_OPENED,       S_STOPPING, S_OPENED, S_OPENED, STAY },
    };

    struct rte_timer timer = {0};
    int retval_fails = 0;

    for(U8 cp=0; cp<=1; cp++) {
        for(U8 s=0; s<S_INVLD; s++) {
            int state_fails = 0;
            U16 first_bad_event = 0;
            U8 first_bad_state = 0;
            char label[64];

            for(U16 ev=0; ev<=E_UNKNOWN; ev++) {
                ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, cp, s);
                STATUS ret = PPP_FSM(&timer, ccb, ev);
                U8 want = (expected[s][ev] == STAY) ? s : expected[s][ev];

                if (ret != SUCCESS)
                    retval_fails++;
                if (ccb->ppp_phase[cp].state != want && state_fails++ == 0) {
                    first_bad_event = ev;
                    first_bad_state = ccb->ppp_phase[cp].state;
                }
                free_test_ppp_ccb(ccb);
            }
            snprintf(label, sizeof(label), "%s state %u all events",
                cp == 0 ? "LCP" : "NCP", s);
            TEST_ASSERT(state_fails == 0, label,
                "%d/%d events mismatched; first: event %u -> state %u",
                state_fails, E_UNKNOWN + 1, first_bad_event, first_bad_state);
        }
    }
    TEST_ASSERT(retval_fails == 0,
        "every (cp, state, event) combo returns SUCCESS",
        "%d combos returned non-SUCCESS", retval_fails);
    #undef STAY
}

/**
 * Test 26: timer_counter (the retransmit budget) is reset to 10 ONLY by
 * transitions that carry an action chain — the reset sits inside PPP_FSM's
 * handler loop, so a row with `{ 0 }` handlers must leave the counter
 * untouched.  Test 18 already covers the positive half; this adds the
 * negative half so two future refactors get caught: hoisting the reset out
 * of the loop (unconditional reset per dispatch → the retransmit budget
 * never runs out, retransmission never gives up) and moving it inside the
 * `#ifndef UNIT_TEST` block (silently changes what unit tests can observe).
 */
static void test_fsm_timer_counter_reset_requires_handler(FastRG_t *fastrg_ccb)
{
    printf("\nTest 26: \"retransmit budget reset is bound to action rows\"\n");
    printf("=========================================\n\n");

    struct rte_timer timer = {0};

    /* STOPPING + E_RECV_TERMINATE_ACK → STOPPED has handlers → counter reset */
    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_STOPPING);
    ccb->ppp_phase[0].timer_counter = 7;
    PPP_FSM(&timer, ccb, E_RECV_TERMINATE_ACK);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_STOPPED &&
        ccb->ppp_phase[0].timer_counter == 10,
        "handler row resets timer_counter to 10",
        "state=%u counter=%u", ccb->ppp_phase[0].state, ccb->ppp_phase[0].timer_counter);
    free_test_ppp_ccb(ccb);

    /* REQUEST_SENT + E_RECV_TERMINATE_ACK stays with no handlers → untouched */
    ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_REQUEST_SENT);
    ccb->ppp_phase[0].timer_counter = 7;
    PPP_FSM(&timer, ccb, E_RECV_TERMINATE_ACK);
    TEST_ASSERT(ccb->ppp_phase[0].state == S_REQUEST_SENT &&
        ccb->ppp_phase[0].timer_counter == 7,
        "handler-less row leaves timer_counter alone",
        "state=%u counter=%u", ccb->ppp_phase[0].state, ccb->ppp_phase[0].timer_counter);
    free_test_ppp_ccb(ccb);
}

/**
 * Test 27: CHAP is authenticator-driven, so completing LCP must enter the
 * authentication phase before the server's Challenge arrives.
 */
static void test_fsm_chap_lcp_up_enters_auth_phase(FastRG_t *fastrg_ccb)
{
    printf("\nTest 27: \"CHAP LCP up enters AUTH phase\"\n");
    printf("=========================================\n\n");

    struct rte_timer timer = {0};
    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_OPENED);
    ccb->auth_method = CHAP_PROTOCOL;
    ccb->phase = LCP_PHASE;

    /* PPP_FSM skips action handlers under UNIT_TEST (see the #ifndef in its
     * action loop), so the LCP-up action is called directly; it is exported
     * in fsm.h alongside the other externally used A_* actions. */
    TEST_ASSERT(A_this_layer_up(&timer, ccb) == SUCCESS,
        "CHAP LCP-up action returns SUCCESS", "");
    TEST_ASSERT(ccb->phase == AUTH_PHASE,
        "CHAP LCP-up action advances to AUTH_PHASE",
        "got phase %u", ccb->phase);

    free_test_ppp_ccb(ccb);
}

// ============================================================================
// Main test function
// ============================================================================

void test_ppp_fsm(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           PPPD FSM Unit Tests                            ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    // Run all tests
    test_fsm_init_to_closed(fastrg_ccb);
    test_fsm_init_to_starting(fastrg_ccb);
    test_fsm_starting_to_request_sent(fastrg_ccb);
    test_fsm_closed_to_request_sent(fastrg_ccb);
    test_fsm_request_sent_to_ack_received(fastrg_ccb);
    test_fsm_ack_received_to_opened(fastrg_ccb);
    test_fsm_ack_sent_to_opened(fastrg_ccb);
    test_fsm_opened_to_closing(fastrg_ccb);
    test_fsm_closing_to_closed(fastrg_ccb);

    test_fsm_null_ccb(fastrg_ccb);
    test_fsm_invalid_state(fastrg_ccb);
    test_fsm_invalid_event_in_valid_state(fastrg_ccb);

    test_fsm_lcp_phase(fastrg_ccb);
    test_fsm_ipcp_phase(fastrg_ccb);

    test_fsm_full_connection_establishment(fastrg_ccb);
    test_fsm_full_connection_termination(fastrg_ccb);
    test_fsm_alternate_path_to_opened(fastrg_ccb);

    test_fsm_timer_counter_reset(fastrg_ccb);
    test_fsm_closed_down_event(fastrg_ccb);
    test_fsm_request_sent_bad_config_request(fastrg_ccb);
    test_fsm_ack_received_invalid_ack(fastrg_ccb);
    test_fsm_user_num_zero(fastrg_ccb);
    test_fsm_max_cp_value(fastrg_ccb);
    test_fsm_stopped_open_event(fastrg_ccb);

    test_fsm_full_table_scan(fastrg_ccb);
    test_fsm_timer_counter_reset_requires_handler(fastrg_ccb);
    test_fsm_chap_lcp_up_enters_auth_phase(fastrg_ccb);

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
