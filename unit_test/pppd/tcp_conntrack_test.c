#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <common.h>

#include <rte_atomic.h>

#include "../../src/fastrg.h"
#include "../../src/pppd/tcp_conntrack.h"
#include "../../src/pppd/nat.h"
#include "../test_helper.h"

static int test_count = 0;
static int pass_count = 0;

/* Helper: create a zeroed addr_table_t entry in READY state */
static void init_entry(addr_table_t *entry, U8 initial_state)
{
    memset(entry, 0, sizeof(*entry));
    entry->tcp_state = initial_state;
    entry->tcp_fin_flags = 0;
    rte_atomic16_set(&entry->is_fill, NAT_ENTRY_READY);
    rte_atomic16_set(&entry->is_alive, TCP_TIMEOUT_NONE);
}

/**
 * Test 1: Basic 3-way handshake
 * SYN → SYN_SENT, SYN-ACK → SYN_RECV, ACK → ESTABLISHED
 */
static void test_three_way_handshake(void)
{
    printf("\nTesting TCP 3-way handshake:\n");
    printf("===========================\n\n");

    addr_table_t entry;
    init_entry(&entry, TCP_CONNTRACK_NONE);

    /* SYN from originator */
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_SENT,
        "SYN → SYN_SENT",
        "expected SYN_SENT(%d), got %d", TCP_CONNTRACK_SYN_SENT, entry.tcp_state);

    /* SYN-ACK from responder */
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_RECV,
        "SYN-ACK → SYN_RECV",
        "expected SYN_RECV(%d), got %d", TCP_CONNTRACK_SYN_RECV, entry.tcp_state);

    /* ACK from originator */
    tcp_conntrack_fsm(&entry, RTE_TCP_ACK_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_ESTABLISHED,
        "ACK → ESTABLISHED",
        "expected ESTABLISHED(%d), got %d", TCP_CONNTRACK_ESTABLISHED, entry.tcp_state);
}

/**
 * Test 2: Timeout values per state
 */
static void test_timeout_values(void)
{
    printf("\nTesting timeout values per state:\n");
    printf("=================================\n\n");

    addr_table_t entry;

    /* SYN_SENT timeout */
    init_entry(&entry, TCP_CONNTRACK_NONE);
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG, FALSE);
    TEST_ASSERT(rte_atomic16_read(&entry.is_alive) == TCP_TIMEOUT_SYN_SENT,
        "SYN_SENT timeout",
        "expected %d, got %d", TCP_TIMEOUT_SYN_SENT, rte_atomic16_read(&entry.is_alive));

    /* ESTABLISHED timeout */
    init_entry(&entry, TCP_CONNTRACK_SYN_RECV);
    tcp_conntrack_fsm(&entry, RTE_TCP_ACK_FLAG, FALSE);
    TEST_ASSERT(rte_atomic16_read(&entry.is_alive) == TCP_TIMEOUT_ESTABLISHED,
        "ESTABLISHED timeout",
        "expected %d, got %d", TCP_TIMEOUT_ESTABLISHED, rte_atomic16_read(&entry.is_alive));

    /* CLOSE timeout (via RST) */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    tcp_conntrack_fsm(&entry, RTE_TCP_RST_FLAG, FALSE);
    TEST_ASSERT(rte_atomic16_read(&entry.is_alive) == TCP_TIMEOUT_CLOSE,
        "CLOSE timeout after RST",
        "expected %d, got %d", TCP_TIMEOUT_CLOSE, rte_atomic16_read(&entry.is_alive));

    /* FIN_WAIT timeout */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, FALSE);
    TEST_ASSERT(rte_atomic16_read(&entry.is_alive) == TCP_TIMEOUT_FIN_WAIT,
        "FIN_WAIT timeout",
        "expected %d, got %d", TCP_TIMEOUT_FIN_WAIT, rte_atomic16_read(&entry.is_alive));

    /* CLOSE_WAIT timeout */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, TRUE);
    TEST_ASSERT(rte_atomic16_read(&entry.is_alive) == TCP_TIMEOUT_CLOSE_WAIT,
        "CLOSE_WAIT timeout",
        "expected %d, got %d", TCP_TIMEOUT_CLOSE_WAIT, rte_atomic16_read(&entry.is_alive));
}

/**
 * Test 3: Normal 4-way close
 * ESTABLISHED + FIN(orig) → FIN_WAIT, FIN(reply) → TIME_WAIT
 */
static void test_four_way_close(void)
{
    printf("\nTesting TCP 4-way close:\n");
    printf("========================\n\n");

    addr_table_t entry;
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);

    /* FIN from originator */
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_FIN_WAIT,
        "ESTABLISHED + FIN(orig) → FIN_WAIT",
        "expected FIN_WAIT(%d), got %d", TCP_CONNTRACK_FIN_WAIT, entry.tcp_state);

    /* FIN from responder (simultaneous close from FIN_WAIT) */
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_TIME_WAIT,
        "FIN_WAIT + FIN(reply) → TIME_WAIT",
        "expected TIME_WAIT(%d), got %d", TCP_CONNTRACK_TIME_WAIT, entry.tcp_state);
}

/**
 * Test 4: RST handling — any state + RST → CLOSE
 */
static void test_rst_handling(void)
{
    printf("\nTesting RST handling:\n");
    printf("=====================\n\n");

    addr_table_t entry;
    U8 states[] = {
        TCP_CONNTRACK_SYN_SENT,
        TCP_CONNTRACK_SYN_RECV,
        TCP_CONNTRACK_ESTABLISHED,
        TCP_CONNTRACK_FIN_WAIT,
        TCP_CONNTRACK_CLOSE_WAIT,
        TCP_CONNTRACK_LAST_ACK,
        TCP_CONNTRACK_TIME_WAIT,
    };

    for (unsigned j = 0; j < sizeof(states) / sizeof(states[0]); j++) {
        init_entry(&entry, states[j]);
        tcp_conntrack_fsm(&entry, RTE_TCP_RST_FLAG, FALSE);
        char test_name[64];
        snprintf(test_name, sizeof(test_name), "RST from %s → CLOSE",
            tcp_conntrack_state2str(states[j]));
        TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_CLOSE, test_name,
            "expected CLOSE(%d), got %d", TCP_CONNTRACK_CLOSE, entry.tcp_state);
    }
}

/**
 * Test 5: Invalid event in state — state should not change
 */
static void test_invalid_event(void)
{
    printf("\nTesting invalid event handling:\n");
    printf("==============================\n\n");

    addr_table_t entry;

    /* SYN_SENT + FIN should have no transition (FIN not valid in SYN_SENT) */
    init_entry(&entry, TCP_CONNTRACK_SYN_SENT);
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_SENT,
        "SYN_SENT + FIN → no change",
        "expected SYN_SENT(%d), got %d", TCP_CONNTRACK_SYN_SENT, entry.tcp_state);

    /* SYN_SENT + ACK should have no transition */
    init_entry(&entry, TCP_CONNTRACK_SYN_SENT);
    tcp_conntrack_fsm(&entry, RTE_TCP_ACK_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_SENT,
        "SYN_SENT + ACK → no change",
        "expected SYN_SENT(%d), got %d", TCP_CONNTRACK_SYN_SENT, entry.tcp_state);
}

/**
 * Test 6: State string conversion
 */
static void test_state2str(void)
{
    printf("\nTesting state string conversion:\n");
    printf("================================\n\n");

    TEST_ASSERT(strcmp(tcp_conntrack_state2str(TCP_CONNTRACK_NONE), "NONE") == 0,
        "NONE state string", NULL);
    TEST_ASSERT(strcmp(tcp_conntrack_state2str(TCP_CONNTRACK_SYN_SENT), "SYN_SENT") == 0,
        "SYN_SENT state string", NULL);
    TEST_ASSERT(strcmp(tcp_conntrack_state2str(TCP_CONNTRACK_ESTABLISHED), "ESTABLISHED") == 0,
        "ESTABLISHED state string", NULL);
    TEST_ASSERT(strcmp(tcp_conntrack_state2str(TCP_CONNTRACK_TIME_WAIT), "TIME_WAIT") == 0,
        "TIME_WAIT state string", NULL);
    TEST_ASSERT(strcmp(tcp_conntrack_state2str(TCP_CONNTRACK_CLOSE), "CLOSE") == 0,
        "CLOSE state string", NULL);
    TEST_ASSERT(strcmp(tcp_conntrack_state2str(TCP_CONNTRACK_INVLD), "UNKNOWN") == 0,
        "INVLD state string → UNKNOWN", NULL);
    TEST_ASSERT(strcmp(tcp_conntrack_state2str(0xFF), "UNKNOWN") == 0,
        "Out-of-range state → UNKNOWN", NULL);
}

/**
 * Test 7: Event extraction from TCP flags (direction-aware)
 */
static void test_tcp_flags_to_event(void)
{
    printf("\nTesting tcp_flags_to_event:\n");
    printf("==========================\n\n");

    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_SYN_FLAG, FALSE) == TCP_EV_SYN,
        "SYN flag → TCP_EV_SYN", NULL);
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, FALSE) == TCP_EV_SYN_ACK,
        "SYN+ACK flags → TCP_EV_SYN_ACK", NULL);
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_ACK_FLAG, FALSE) == TCP_EV_ACK,
        "ACK flag → TCP_EV_ACK", NULL);
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_RST_FLAG, FALSE) == TCP_EV_RST,
        "RST flag → TCP_EV_RST", NULL);
    /* RST takes priority over SYN */
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_RST_FLAG | RTE_TCP_SYN_FLAG, FALSE) == TCP_EV_RST,
        "RST+SYN flags → TCP_EV_RST (RST priority)", NULL);
    TEST_ASSERT(tcp_flags_to_event(0, FALSE) == TCP_EV_INVLD,
        "No flags → TCP_EV_INVLD", NULL);
    /* FIN direction-aware: is_reply=FALSE (originator) → TCP_EV_FIN_ORIG */
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_FIN_FLAG, FALSE) == TCP_EV_FIN_ORIG,
        "FIN flag (originator) → TCP_EV_FIN_ORIG", NULL);
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG, FALSE) == TCP_EV_FIN_ORIG,
        "FIN+ACK flags (originator) → TCP_EV_FIN_ORIG", NULL);
    /* FIN direction-aware: is_reply=TRUE (responder) → TCP_EV_FIN_RESP */
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_FIN_FLAG, TRUE) == TCP_EV_FIN_RESP,
        "FIN flag (responder) → TCP_EV_FIN_RESP", NULL);
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG, TRUE) == TCP_EV_FIN_RESP,
        "FIN+ACK flags (responder) → TCP_EV_FIN_RESP", NULL);
}

/**
 * Test 8: Simultaneous close — FIN from both sides in ESTABLISHED
 */
static void test_simultaneous_close(void)
{
    printf("\nTesting simultaneous close:\n");
    printf("===========================\n\n");

    addr_table_t entry;
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);

    /* FIN from originator → FIN_WAIT */
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_FIN_WAIT,
        "First FIN (orig) → FIN_WAIT", NULL);
    TEST_ASSERT(entry.tcp_fin_flags & TCP_FIN_FLAG_ORIGINATOR,
        "Originator FIN flag set", NULL);

    /* FIN from responder → TIME_WAIT (simultaneous close) */
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_TIME_WAIT,
        "Second FIN (reply) → TIME_WAIT",
        "expected TIME_WAIT(%d), got %d", TCP_CONNTRACK_TIME_WAIT, entry.tcp_state);
    TEST_ASSERT(entry.tcp_fin_flags & TCP_FIN_FLAG_RESPONDER,
        "Responder FIN flag set", NULL);
}

/**
 * Test 9: SYN retransmit — SYN in SYN_SENT stays SYN_SENT, timeout refreshed
 */
static void test_syn_retransmit(void)
{
    printf("\nTesting SYN retransmit:\n");
    printf("=======================\n\n");

    addr_table_t entry;
    init_entry(&entry, TCP_CONNTRACK_SYN_SENT);
    rte_atomic16_set(&entry.is_alive, 5); /* simulate partial timeout */

    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_SENT,
        "SYN retransmit stays SYN_SENT",
        "expected SYN_SENT(%d), got %d", TCP_CONNTRACK_SYN_SENT, entry.tcp_state);
    TEST_ASSERT(rte_atomic16_read(&entry.is_alive) == TCP_TIMEOUT_SYN_SENT,
        "SYN retransmit refreshes timeout",
        "expected %d, got %d", TCP_TIMEOUT_SYN_SENT, rte_atomic16_read(&entry.is_alive));
}

/**
 * Test 10: Direction-aware FIN from ESTABLISHED
 * FIN from responder (reply) → CLOSE_WAIT (not FIN_WAIT)
 */
static void test_direction_aware_fin(void)
{
    printf("\nTesting direction-aware FIN transitions:\n");
    printf("=========================================\n\n");

    addr_table_t entry;

    /* FIN from responder in ESTABLISHED → CLOSE_WAIT */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_CLOSE_WAIT,
        "ESTABLISHED + FIN(reply) → CLOSE_WAIT",
        "expected CLOSE_WAIT(%d), got %d", TCP_CONNTRACK_CLOSE_WAIT, entry.tcp_state);

    /* Then FIN from originator → LAST_ACK */
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_LAST_ACK,
        "CLOSE_WAIT + FIN(orig) → LAST_ACK",
        "expected LAST_ACK(%d), got %d", TCP_CONNTRACK_LAST_ACK, entry.tcp_state);

    /* Then ACK → TIME_WAIT */
    tcp_conntrack_fsm(&entry, RTE_TCP_ACK_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_TIME_WAIT,
        "LAST_ACK + ACK → TIME_WAIT",
        "expected TIME_WAIT(%d), got %d", TCP_CONNTRACK_TIME_WAIT, entry.tcp_state);
}

void test_tcp_conntrack(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;

    test_count = 0;
    pass_count = 0;

    test_three_way_handshake();
    test_timeout_values();
    test_four_way_close();
    test_rst_handling();
    test_invalid_event();
    test_state2str();
    test_tcp_flags_to_event();
    test_simultaneous_close();
    test_syn_retransmit();
    test_direction_aware_fin();

    *total_tests += test_count;
    *total_pass += pass_count;
}
