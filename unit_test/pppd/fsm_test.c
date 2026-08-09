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

static ppp_ccb_t* create_test_ppp_ccb(FastRG_t *fastrg_ccb, U8 cp_id, U8 state) {
    ppp_ccb_t *ccb = (ppp_ccb_t*)calloc(1, sizeof(ppp_ccb_t));
    assert(ccb != NULL);

    ccb->cp_id = cp_id;
    ccb->control_protocol[cp_id].state = state;
    ccb->user_num = 1;
    ccb->session_id = rte_cpu_to_be_16(0x1234);
    ccb->control_protocol[cp_id].timer_counter = 10;
    ccb->control_protocol[cp_id].ppp_payload.ppp_protocol =
        rte_cpu_to_be_16(cp_id == PPP_CP_LCP ? LCP_PROTOCOL :
            (cp_id == PPP_CP_IPCP ? IPCP_PROTOCOL : IPV6CP_PROTOCOL));
    ccb->fastrg_ccb = fastrg_ccb;

    return ccb;
}

static void free_test_ppp_ccb(ppp_ccb_t *ccb) {
    if (ccb) {
        if (ccb->control_protocol[PPP_CP_LCP].ppp_options)
            free(ccb->control_protocol[PPP_CP_LCP].ppp_options);
        if (ccb->control_protocol[PPP_CP_IPCP].ppp_options)
            free(ccb->control_protocol[PPP_CP_IPCP].ppp_options);
        if (ccb->control_protocol[PPP_CP_IPV6CP].ppp_options)
            free(ccb->control_protocol[PPP_CP_IPV6CP].ppp_options);
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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_CLOSED, 
        "State transitions to S_CLOSED", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_STARTING, 
        "State transitions to S_STARTING", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_REQUEST_SENT,
        "State transitions to S_REQUEST_SENT", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_REQUEST_SENT,
        "State transitions to S_REQUEST_SENT", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_ACK_RECEIVED,
        "State transitions to S_ACK_RECEIVED", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_OPENED,
        "State transitions to S_OPENED", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_OPENED,
        "State transitions to S_OPENED", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_CLOSING,
        "State transitions to S_CLOSING", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_CLOSED,
        "State transitions to S_CLOSED", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    U8 initial_state = ccb->control_protocol[PPP_CP_LCP].state;

    // E_RECV_CONFIG_ACK is invalid in INIT state
    STATUS result = PPP_FSM(&timer, ccb, E_RECV_CONFIG_ACK);

    TEST_ASSERT(result == SUCCESS, "PPP_FSM returns SUCCESS (but does nothing)", "got %d", result);
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == initial_state,
        "State unchanged for invalid event", "expected %d, got %d", initial_state, ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->cp_id == PPP_CP_LCP, "CCB indicates LCP phase", "got cp=%d", ccb->cp_id);
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_STARTING,
        "LCP state transitions correctly", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->cp_id == PPP_CP_IPCP, "CCB indicates IPCP phase", "got cp=%d", ccb->cp_id);
    TEST_ASSERT(ccb->control_protocol[PPP_CP_IPCP].state == S_STARTING,
        "IPCP state transitions correctly", "got state %d", ccb->control_protocol[PPP_CP_IPCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_STARTING,
        "Step 1: INIT -> STARTING", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

    // STARTING -> REQUEST_SENT (E_UP)
    PPP_FSM(&timer, ccb, E_UP);
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_REQUEST_SENT,
        "Step 2: STARTING -> REQUEST_SENT", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

    // REQUEST_SENT -> ACK_RECEIVED (E_RECV_CONFIG_ACK)
    PPP_FSM(&timer, ccb, E_RECV_CONFIG_ACK);
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_ACK_RECEIVED,
                "Step 3: REQUEST_SENT -> ACK_RECEIVED", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

    // ACK_RECEIVED -> OPENED (E_RECV_GOOD_CONFIG_REQUEST)
    PPP_FSM(&timer, ccb, E_RECV_GOOD_CONFIG_REQUEST);
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_OPENED,
                "Step 4: ACK_RECEIVED -> OPENED", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_CLOSING,
        "Step 1: OPENED -> CLOSING", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

    // CLOSING -> CLOSED (E_RECV_TERMINATE_ACK)
    PPP_FSM(&timer, ccb, E_RECV_TERMINATE_ACK);
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_CLOSED,
        "Step 2: CLOSING -> CLOSED", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_ACK_SENT,
        "Step 1: REQUEST_SENT -> ACK_SENT", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

    // ACK_SENT -> OPENED (E_RECV_CONFIG_ACK)
    PPP_FSM(&timer, ccb, E_RECV_CONFIG_ACK);
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_OPENED,
        "Step 2: ACK_SENT -> OPENED", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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

    ccb->control_protocol[PPP_CP_LCP].timer_counter = 5;

    PPP_FSM(&timer, ccb, E_OPEN);

    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].timer_counter == 10,
        "Timer counter reset to 10", "got %d", ccb->control_protocol[PPP_CP_LCP].timer_counter);

    free_test_ppp_ccb(ccb);
}

void test_fsm_closed_down_event(FastRG_t *fastrg_ccb)
{
    printf("\nTest 19: \"CLOSED -> INIT (E_DOWN)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_CLOSED);
    struct rte_timer timer = {0};

    PPP_FSM(&timer, ccb, E_DOWN);

    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_INIT,
        "State transitions to INIT", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_request_sent_bad_config_request(FastRG_t *fastrg_ccb)
{
    printf("\nTest 20: \"REQUEST_SENT -> REQUEST_SENT (E_RECV_BAD_CONFIG_REQUEST)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_REQUEST_SENT);
    struct rte_timer timer = {0};

    PPP_FSM(&timer, ccb, E_RECV_BAD_CONFIG_REQUEST);

    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_REQUEST_SENT,
        "State remains REQUEST_SENT", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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

    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_REQUEST_SENT,
        "State transitions back to REQUEST_SENT", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
    TEST_ASSERT(ccb->control_protocol[PPP_CP_IPCP].state == S_CLOSED,
        "IPCP state transitions correctly", "got state %d", ccb->control_protocol[PPP_CP_IPCP].state);

    free_test_ppp_ccb(ccb);
}

void test_fsm_stopped_open_event(FastRG_t *fastrg_ccb)
{
    printf("\nTest 24: \"STOPPED -> STOPPED (E_OPEN with restart)\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_STOPPED);
    struct rte_timer timer = {0};

    PPP_FSM(&timer, ccb, E_OPEN);

    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_STOPPED,
        "State remains STOPPED", "got state %d", ccb->control_protocol[PPP_CP_LCP].state);

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
                if (ccb->control_protocol[cp].state != want && state_fails++ == 0) {
                    first_bad_event = ev;
                    first_bad_state = ccb->control_protocol[cp].state;
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
    ccb->control_protocol[PPP_CP_LCP].timer_counter = 7;
    PPP_FSM(&timer, ccb, E_RECV_TERMINATE_ACK);
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_STOPPED &&
        ccb->control_protocol[PPP_CP_LCP].timer_counter == 10,
        "handler row resets timer_counter to 10",
        "state=%u counter=%u", ccb->control_protocol[PPP_CP_LCP].state, ccb->control_protocol[PPP_CP_LCP].timer_counter);
    free_test_ppp_ccb(ccb);

    /* REQUEST_SENT + E_RECV_TERMINATE_ACK stays with no handlers → untouched */
    ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_REQUEST_SENT);
    ccb->control_protocol[PPP_CP_LCP].timer_counter = 7;
    PPP_FSM(&timer, ccb, E_RECV_TERMINATE_ACK);
    TEST_ASSERT(ccb->control_protocol[PPP_CP_LCP].state == S_REQUEST_SENT &&
        ccb->control_protocol[PPP_CP_LCP].timer_counter == 7,
        "handler-less row leaves timer_counter alone",
        "state=%u counter=%u", ccb->control_protocol[PPP_CP_LCP].state, ccb->control_protocol[PPP_CP_LCP].timer_counter);
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

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_OPENED);
    ccb->auth_method = CHAP_PROTOCOL;
    ccb->phase = LCP_PHASE;

    /* PPP_FSM skips action handlers under UNIT_TEST (see the #ifndef in its
     * action loop), so the LCP-up transition is exercised through its public
     * entry point. */
    TEST_ASSERT(lcp_layer_up(ccb) == SUCCESS,
        "CHAP LCP-up returns SUCCESS", "");
    TEST_ASSERT(ccb->phase == AUTH_PHASE,
        "CHAP LCP-up advances to AUTH_PHASE",
        "got phase %u", ccb->phase);

    free_test_ppp_ccb(ccb);
}

/**
 * Test 28: when the peer Configure-Rejected our AUTH option and never demanded
 * authentication itself, LCP-up must skip the AUTH phase and open IPCP.
 */
static void test_fsm_auth_rejected_lcp_up_skips_auth_phase(FastRG_t *fastrg_ccb)
{
    printf("\nTest 28: \"AUTH-rejected LCP up skips AUTH phase\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_OPENED);
    ccb->auth_method = PAP_PROTOCOL;
    ccb->phase = LCP_PHASE;
    ccb->lcp_auth_rejected = TRUE;
    ccb->peer_requires_auth = FALSE;
    ccb->control_protocol[PPP_CP_IPCP].state = S_INIT;

    TEST_ASSERT(lcp_layer_up(ccb) == SUCCESS,
        "AUTH-rejected LCP-up returns SUCCESS", "");
    TEST_ASSERT(ccb->phase == IPCP_PHASE && ccb->cp_id == PPP_CP_IPCP,
        "AUTH-rejected LCP-up opens IPCP directly",
        "got phase %u cp %u", ccb->phase, ccb->cp_id);
    TEST_ASSERT(ccb->control_protocol[PPP_CP_IPCP].state == S_STARTING,
        "AUTH-rejected LCP-up drives the NCP FSM through E_OPEN",
        "got state %u", ccb->control_protocol[PPP_CP_IPCP].state);

    free_test_ppp_ccb(ccb);
}

/**
 * Test 29: the AUTH-phase skip must not trigger when the peer demanded
 * authentication in its own Configure-Request.
 */
static void test_fsm_peer_requires_auth_keeps_auth_phase(FastRG_t *fastrg_ccb)
{
    printf("\nTest 29: \"peer-required auth still enters AUTH phase\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 0, S_OPENED);
    ccb->auth_method = CHAP_PROTOCOL;
    ccb->phase = LCP_PHASE;
    ccb->lcp_auth_rejected = TRUE;
    ccb->peer_requires_auth = TRUE;

    TEST_ASSERT(lcp_layer_up(ccb) == SUCCESS,
        "peer-required-auth LCP-up returns SUCCESS", "");
    TEST_ASSERT(ccb->phase == AUTH_PHASE,
        "peer-required auth still advances to AUTH_PHASE",
        "got phase %u", ccb->phase);

    free_test_ppp_ccb(ccb);
}

/**
 * Test 30: rolling the phase back two steps must clamp at END_PHASE instead
 * of wrapping the unsigned phase field around to a huge value.
 */
static void test_fsm_phase_rollback_underflow_guard(FastRG_t *fastrg_ccb)
{
    printf("\nTest 30: \"phase rollback clamps instead of underflowing\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, 1, S_OPENED);

    /* Normal rollbacks are unchanged */
    ccb->phase = IPCP_PHASE;
    ppp_phase_rollback(ccb);
    TEST_ASSERT(ccb->phase == LCP_PHASE,
        "IPCP_PHASE rolls back to LCP_PHASE", "got phase %u", ccb->phase);

    ccb->phase = DATA_PHASE;
    ppp_phase_rollback(ccb);
    TEST_ASSERT(ccb->phase == AUTH_PHASE,
        "DATA_PHASE rolls back to AUTH_PHASE", "got phase %u", ccb->phase);

    /* Low phases clamp at END_PHASE instead of wrapping around */
    ccb->phase = PPPOE_PHASE;
    ppp_phase_rollback(ccb);
    TEST_ASSERT(ccb->phase == END_PHASE,
        "PPPOE_PHASE clamps to END_PHASE", "got phase %u", ccb->phase);

    ccb->phase = END_PHASE;
    ppp_phase_rollback(ccb);
    TEST_ASSERT(ccb->phase == END_PHASE,
        "END_PHASE stays at END_PHASE without underflow", "got phase %u", ccb->phase);

    free_test_ppp_ccb(ccb);
}

/**
 * Test 31: IPV6CP shares the NCP state table, so every state/event pair must
 * produce the same result as IPCP.
 */
static void test_fsm_ipv6cp_table_matches_ipcp(FastRG_t *fastrg_ccb)
{
    printf("\nTest 31: \"IPV6CP state table matches IPCP\"\n");
    printf("=========================================\n\n");

    struct rte_timer timer = {0};
    int mismatches = 0;
    U8 first_state = 0;
    U16 first_event = 0;

    for (U8 state = 0; state < S_INVLD; state++) {
        for (U16 event = 0; event <= E_UNKNOWN; event++) {
            ppp_ccb_t *ipcp_ccb = create_test_ppp_ccb(fastrg_ccb, 1, state);
            ppp_ccb_t *ipv6cp_ccb = create_test_ppp_ccb(fastrg_ccb, PPP_CP_IPV6CP, state);
            STATUS ipcp_ret = PPP_FSM(&timer, ipcp_ccb, event);
            STATUS ipv6cp_ret = PPP_FSM(&timer, ipv6cp_ccb, event);

            if ((ipcp_ret != ipv6cp_ret ||
                    ipcp_ccb->control_protocol[PPP_CP_IPCP].state != ipv6cp_ccb->control_protocol[PPP_CP_IPV6CP].state) &&
                    mismatches++ == 0) {
                first_state = state;
                first_event = event;
            }
            free_test_ppp_ccb(ipcp_ccb);
            free_test_ppp_ccb(ipv6cp_ccb);
        }
    }

    TEST_ASSERT(mismatches == 0,
        "IPV6CP and IPCP tables have identical transitions",
        "%d mismatches; first state=%u event=%u", mismatches,
        first_state, first_event);
}

/**
 * Test 32: IPV6CP uses the normal NCP kickoff transition.
 */
static void test_fsm_ipv6cp_kickoff(FastRG_t *fastrg_ccb)
{
    printf("\nTest 32: \"IPV6CP INIT + OPEN enters STARTING\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, PPP_CP_IPV6CP, S_INIT);
    struct rte_timer timer = {0};

    TEST_ASSERT(PPP_FSM(&timer, ccb, E_OPEN) == SUCCESS,
        "IPV6CP kickoff returns SUCCESS", "");
    TEST_ASSERT(ccb->control_protocol[PPP_CP_IPV6CP].state == S_STARTING,
        "IPV6CP kickoff enters S_STARTING", "state=%u",
        ccb->control_protocol[PPP_CP_IPV6CP].state);

    free_test_ppp_ccb(ccb);
}

/**
 * Test 33: an exhausted IPV6CP negotiation becomes quiescent without
 * changing IPv4 readiness or the main PPP phase.
 */
static void test_fsm_ipv6cp_timeout_quiescence(FastRG_t *fastrg_ccb)
{
    printf("\nTest 33: \"IPV6CP timeout is IPv4-independent and quiescent\"\n");
    printf("=========================================\n\n");

    ppp_ccb_t *ccb = create_test_ppp_ccb(fastrg_ccb, PPP_CP_IPV6CP, S_REQUEST_SENT);
    struct rte_timer timer = {0};

    ccb->phase = DATA_PHASE;
    ccb->ipv6cp_up = TRUE;
    ccb->config_request_pending[PPP_CP_IPV6CP] = TRUE;
    ccb->identifier[PPP_CP_IPV6CP] = 9;
    rte_atomic16_set(&ccb->dp_start_bool, 1);

    PPP_FSM(&timer, ccb, E_TIMEOUT_COUNTER_EXPIRED);
    ipv6cp_stop(ccb);

    TEST_ASSERT(ccb->control_protocol[PPP_CP_IPV6CP].state == S_STOPPED &&
        ccb->config_request_pending[PPP_CP_IPV6CP] == FALSE && ccb->ipv6cp_up == FALSE,
        "IPV6CP timeout leaves no active negotiation",
        "state=%u pending=%u up=%u", ccb->control_protocol[PPP_CP_IPV6CP].state,
        ccb->config_request_pending[PPP_CP_IPV6CP], ccb->ipv6cp_up);
    TEST_ASSERT(ccb->phase == DATA_PHASE && rte_atomic16_read(&ccb->dp_start_bool) == 1,
        "IPV6CP timeout preserves IPv4 data-plane readiness",
        "phase=%u dp=%d", ccb->phase, rte_atomic16_read(&ccb->dp_start_bool));
    TEST_ASSERT(ccb->identifier[PPP_CP_IPV6CP] == 9,
        "IPV6CP timeout does not generate another Configure-Request",
        "identifier=%u", ccb->identifier[PPP_CP_IPV6CP]);

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
    test_fsm_auth_rejected_lcp_up_skips_auth_phase(fastrg_ccb);
    test_fsm_peer_requires_auth_keeps_auth_phase(fastrg_ccb);
    test_fsm_phase_rollback_underflow_guard(fastrg_ccb);
    test_fsm_ipv6cp_table_matches_ipcp(fastrg_ccb);
    test_fsm_ipv6cp_kickoff(fastrg_ccb);
    test_fsm_ipv6cp_timeout_quiescence(fastrg_ccb);

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
