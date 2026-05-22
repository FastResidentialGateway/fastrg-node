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
    rte_atomic64_set(&entry->expire_at,
        fastrg_get_cur_cycles() + (U64)TCP_TIMEOUT_NONE * fastrg_get_cycles_in_sec());
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

    /* SYN from LAN */
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_SENT,
        "SYN → SYN_SENT",
        "expected SYN_SENT(%d), got %d", TCP_CONNTRACK_SYN_SENT, entry.tcp_state);

    /* SYN-ACK from WAN */
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_RECV,
        "SYN-ACK → SYN_RECV",
        "expected SYN_RECV(%d), got %d", TCP_CONNTRACK_SYN_RECV, entry.tcp_state);

    /* ACK from LAN */
    tcp_conntrack_fsm(&entry, RTE_TCP_ACK_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_ESTABLISHED,
        "ACK → ESTABLISHED",
        "expected ESTABLISHED(%d), got %d", TCP_CONNTRACK_ESTABLISHED, entry.tcp_state);
}

/*
 * Helper: verify expire_at was set to approximately (now + expected_secs * hz).
 * Returns 1 if in range, 0 otherwise.
 */
static int expire_at_approx(U64 before_cycles, U64 expire_at, U64 expected_secs)
{
    U64 hz = fastrg_get_cycles_in_sec();
    U64 tolerance = hz / 1000; /* 1 ms tolerance */
    U64 expected = (U64)expected_secs * hz;
    return expire_at >= before_cycles + expected &&
           expire_at <= before_cycles + expected + tolerance;
}

/**
 * Test 2: Timeout values per state
 */
static void test_timeout_values(void)
{
    printf("\nTesting timeout values per state:\n");
    printf("=================================\n\n");

    addr_table_t entry;
    U64 before;

    /* SYN_SENT timeout */
    init_entry(&entry, TCP_CONNTRACK_NONE);
    before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG, FALSE);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_SYN_SENT),
        "SYN_SENT timeout",
        "expire_at not set to ~%d seconds", TCP_TIMEOUT_SYN_SENT);

    /* ESTABLISHED timeout */
    init_entry(&entry, TCP_CONNTRACK_SYN_RECV);
    before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_ACK_FLAG, FALSE);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_ESTABLISHED),
        "ESTABLISHED timeout",
        "expire_at not set to ~%d seconds", TCP_TIMEOUT_ESTABLISHED);

    /* CLOSE timeout (via RST) */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_RST_FLAG, FALSE);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_CLOSE),
        "CLOSE timeout after RST",
        "expire_at not set to ~%d seconds", TCP_TIMEOUT_CLOSE);

    /* FIN_WAIT timeout */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, FALSE);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_FIN_WAIT),
        "FIN_WAIT timeout",
        "expire_at not set to ~%d seconds", TCP_TIMEOUT_FIN_WAIT);

    /* CLOSE_WAIT timeout */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, TRUE);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_CLOSE_WAIT),
        "CLOSE_WAIT timeout",
        "expire_at not set to ~%d seconds", TCP_TIMEOUT_CLOSE_WAIT);
}

/**
 * Test 3: Normal 4-way close
 * ESTABLISHED + FIN(LAN side) → FIN_WAIT, FIN(WAN side) → TIME_WAIT
 */
static void test_four_way_close(void)
{
    printf("\nTesting TCP 4-way close:\n");
    printf("========================\n\n");

    addr_table_t entry;
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);

    /* FIN from LAN */
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_FIN_WAIT,
        "ESTABLISHED + FIN(LAN side) → FIN_WAIT",
        "expected FIN_WAIT(%d), got %d", TCP_CONNTRACK_FIN_WAIT, entry.tcp_state);

    /* FIN from WAN (simultaneous close from FIN_WAIT) */
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_TIME_WAIT,
        "FIN_WAIT + FIN(WAN side) → TIME_WAIT",
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
    /* FIN direction-aware: is_reply=FALSE (LAN side) → TCP_EV_FIN_LAN */
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_FIN_FLAG, FALSE) == TCP_EV_FIN_LAN,
        "FIN flag (LAN side) → TCP_EV_FIN_LAN", NULL);
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG, FALSE) == TCP_EV_FIN_LAN,
        "FIN+ACK flags (LAN side) → TCP_EV_FIN_LAN", NULL);
    /* FIN direction-aware: is_reply=TRUE (WAN side) → TCP_EV_FIN_WAN */
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_FIN_FLAG, TRUE) == TCP_EV_FIN_WAN,
        "FIN flag (WAN side) → TCP_EV_FIN_WAN", NULL);
    TEST_ASSERT(tcp_flags_to_event(RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG, TRUE) == TCP_EV_FIN_WAN,
        "FIN+ACK flags (WAN side) → TCP_EV_FIN_WAN", NULL);
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

    /* FIN from LAN → FIN_WAIT */
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_FIN_WAIT,
        "First FIN (LAN side) → FIN_WAIT", NULL);
    TEST_ASSERT(entry.tcp_fin_flags & TCP_FIN_FLAG_LAN,
        "LAN side FIN flag set", NULL);

    /* FIN from WAN → TIME_WAIT (simultaneous close) */
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_TIME_WAIT,
        "Second FIN (WAN side) → TIME_WAIT",
        "expected TIME_WAIT(%d), got %d", TCP_CONNTRACK_TIME_WAIT, entry.tcp_state);
    TEST_ASSERT(entry.tcp_fin_flags & TCP_FIN_FLAG_WAN,
        "WAN side FIN flag set", NULL);
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
    /* simulate a partially elapsed timeout (5 seconds remaining) */
    rte_atomic64_set(&entry.expire_at,
        fastrg_get_cur_cycles() + 5ULL * fastrg_get_cycles_in_sec());

    U64 before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_SENT,
        "SYN retransmit stays SYN_SENT",
        "expected SYN_SENT(%d), got %d", TCP_CONNTRACK_SYN_SENT, entry.tcp_state);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_SYN_SENT),
        "SYN retransmit refreshes timeout",
        "expire_at not reset to ~%d seconds", TCP_TIMEOUT_SYN_SENT);
}

/**
 * Test 10: Direction-aware FIN from ESTABLISHED
 * FIN from WAN side → CLOSE_WAIT (not FIN_WAIT)
 */
static void test_direction_aware_fin(void)
{
    printf("\nTesting direction-aware FIN transitions:\n");
    printf("=========================================\n\n");

    addr_table_t entry;

    /* FIN from WAN side in ESTABLISHED → CLOSE_WAIT */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_CLOSE_WAIT,
        "ESTABLISHED + FIN(WAN side) → CLOSE_WAIT",
        "expected CLOSE_WAIT(%d), got %d", TCP_CONNTRACK_CLOSE_WAIT, entry.tcp_state);

    /* Then FIN from LAN → LAST_ACK */
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_LAST_ACK,
        "CLOSE_WAIT + FIN(LAN side) → LAST_ACK",
        "expected LAST_ACK(%d), got %d", TCP_CONNTRACK_LAST_ACK, entry.tcp_state);

    /* Then ACK → TIME_WAIT */
    tcp_conntrack_fsm(&entry, RTE_TCP_ACK_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_TIME_WAIT,
        "LAST_ACK + ACK → TIME_WAIT",
        "expected TIME_WAIT(%d), got %d", TCP_CONNTRACK_TIME_WAIT, entry.tcp_state);
}

/**
 * Test 11: New connection reuse from CLOSE state
 * CLOSE + SYN → SYN_SENT (fin_flags cleared)
 * CLOSE + SYN_ACK → SYN_RECV (passive open, fin_flags cleared)
 */
static void test_close_new_connection(void)
{
    printf("\nTesting new connection from CLOSE state:\n");
    printf("=========================================\n\n");

    addr_table_t entry;

    /* CLOSE + SYN → SYN_SENT, fin_flags cleared */
    init_entry(&entry, TCP_CONNTRACK_CLOSE);
    entry.tcp_fin_flags = TCP_FIN_FLAG_LAN | TCP_FIN_FLAG_WAN;
    U64 before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_SENT,
        "CLOSE + SYN → SYN_SENT",
        "expected SYN_SENT(%d), got %d", TCP_CONNTRACK_SYN_SENT, entry.tcp_state);
    TEST_ASSERT(entry.tcp_fin_flags == 0,
        "CLOSE + SYN clears fin_flags",
        "expected 0, got %d", entry.tcp_fin_flags);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_SYN_SENT),
        "CLOSE + SYN sets SYN_SENT timeout",
        "expire_at not set to ~%d seconds", TCP_TIMEOUT_SYN_SENT);

    /* CLOSE + SYN_ACK has no transition: SNAT entries are generated at LAN → WAN, so a
     * SYN_ACK in CLOSE has no legitimate path; state stays CLOSE, fin_flags untouched. */
    init_entry(&entry, TCP_CONNTRACK_CLOSE);
    entry.tcp_fin_flags = TCP_FIN_FLAG_LAN | TCP_FIN_FLAG_WAN;
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_CLOSE,
        "CLOSE + SYN_ACK → no transition (stays CLOSE)",
        "expected CLOSE(%d), got %d", TCP_CONNTRACK_CLOSE, entry.tcp_state);
    TEST_ASSERT(entry.tcp_fin_flags ==
            (TCP_FIN_FLAG_LAN | TCP_FIN_FLAG_WAN),
        "CLOSE + SYN_ACK does not touch fin_flags",
        "expected 0x%02X, got 0x%02X",
        TCP_FIN_FLAG_LAN | TCP_FIN_FLAG_WAN, entry.tcp_fin_flags);
}

/**
 * Test 12: New connection reuse from TIME_WAIT state
 * TIME_WAIT + SYN → SYN_SENT (fin_flags cleared)
 */
static void test_time_wait_new_connection(void)
{
    printf("\nTesting new connection from TIME_WAIT state:\n");
    printf("=============================================\n\n");

    addr_table_t entry;
    init_entry(&entry, TCP_CONNTRACK_TIME_WAIT);
    entry.tcp_fin_flags = TCP_FIN_FLAG_LAN | TCP_FIN_FLAG_WAN;

    U64 before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_SENT,
        "TIME_WAIT + SYN → SYN_SENT",
        "expected SYN_SENT(%d), got %d", TCP_CONNTRACK_SYN_SENT, entry.tcp_state);
    TEST_ASSERT(entry.tcp_fin_flags == 0,
        "TIME_WAIT + SYN clears fin_flags",
        "expected 0, got %d", entry.tcp_fin_flags);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_SYN_SENT),
        "TIME_WAIT + SYN sets SYN_SENT timeout",
        "expire_at not set to ~%d seconds", TCP_TIMEOUT_SYN_SENT);
}

/**
 * Test 13: New connection reuse from ESTABLISHED state
 * ESTABLISHED + SYN     → SYN_SENT   (client opened a fresh flow on a reused 4-tuple)
 * ESTABLISHED + SYN_ACK → ESTABLISHED (server retransmit; stay put, refresh timeout)
 */
static void test_established_port_reuse(void)
{
    printf("\nTesting port reuse from ESTABLISHED state:\n");
    printf("==========================================\n\n");

    addr_table_t entry;

    /* ESTABLISHED + SYN → SYN_SENT, fin_flags cleared */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    entry.tcp_fin_flags = TCP_FIN_FLAG_LAN | TCP_FIN_FLAG_WAN;
    U64 before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_SENT,
        "ESTABLISHED + SYN → SYN_SENT",
        "expected SYN_SENT(%d), got %d", TCP_CONNTRACK_SYN_SENT, entry.tcp_state);
    TEST_ASSERT(entry.tcp_fin_flags == 0,
        "ESTABLISHED + SYN clears fin_flags",
        "expected 0, got %d", entry.tcp_fin_flags);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_SYN_SENT),
        "ESTABLISHED + SYN sets SYN_SENT timeout",
        "expire_at not set to ~%d seconds", TCP_TIMEOUT_SYN_SENT);

    /* ESTABLISHED + SYN_ACK → ESTABLISHED (server retransmit, no state regression) */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    entry.tcp_fin_flags = TCP_FIN_FLAG_LAN | TCP_FIN_FLAG_WAN;
    before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_ESTABLISHED,
        "ESTABLISHED + SYN_ACK → ESTABLISHED (server retransmit, no regression)",
        "expected ESTABLISHED(%d), got %d", TCP_CONNTRACK_ESTABLISHED, entry.tcp_state);
    TEST_ASSERT(entry.tcp_fin_flags == (TCP_FIN_FLAG_LAN | TCP_FIN_FLAG_WAN),
        "ESTABLISHED + SYN_ACK does not touch fin_flags",
        "expected 0x%02X, got 0x%02X",
        TCP_FIN_FLAG_LAN | TCP_FIN_FLAG_WAN, entry.tcp_fin_flags);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_ESTABLISHED),
        "ESTABLISHED + SYN_ACK refreshes ESTABLISHED timeout",
        "expire_at not set to ~%d seconds", TCP_TIMEOUT_ESTABLISHED);
}

/**
 * Test 14: Inbound SPI filtering (tcp_conntrack_inbound_valid)
 */
static void test_inbound_spi(void)
{
    printf("\nTesting inbound SPI filtering:\n");
    printf("================================\n\n");

    /* SYN_SENT: only SYN-ACK (expected reply) and RST (abort) are valid from WAN */
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_SYN_SENT,
            RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG) == TRUE,
        "SYN_SENT + SYN-ACK → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_SYN_SENT,
            RTE_TCP_RST_FLAG) == TRUE,
        "SYN_SENT + RST → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_SYN_SENT,
            RTE_TCP_SYN_FLAG) == FALSE,
        "SYN_SENT + SYN → invalid (WAN cannot initiate through SNAT)", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_SYN_SENT,
            RTE_TCP_ACK_FLAG) == FALSE,
        "SYN_SENT + ACK → invalid (no SYN-ACK seen yet)", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_SYN_SENT,
            RTE_TCP_FIN_FLAG) == FALSE,
        "SYN_SENT + FIN → invalid", NULL);

    /* SYN_RECV: SYN-ACK retransmit, ACK, FIN, RST valid */
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_SYN_RECV,
            RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG) == TRUE,
        "SYN_RECV + SYN-ACK → valid (retransmit)", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_SYN_RECV,
            RTE_TCP_ACK_FLAG) == TRUE,
        "SYN_RECV + ACK → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_SYN_RECV,
            RTE_TCP_RST_FLAG) == TRUE,
        "SYN_RECV + RST → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_SYN_RECV,
            RTE_TCP_SYN_FLAG) == FALSE,
        "SYN_RECV + SYN → invalid", NULL);

    /* ESTABLISHED: ACK, FIN (WAN → LAN direction), RST, SYN-ACK valid;
     * bare SYN from WAN is invalid (cannot match an active entry's 4-tuple anyway) */
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_ESTABLISHED,
            RTE_TCP_ACK_FLAG) == TRUE,
        "ESTABLISHED + ACK → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_ESTABLISHED,
            RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG) == TRUE,
        "ESTABLISHED + FIN+ACK (WAN) → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_ESTABLISHED,
            RTE_TCP_RST_FLAG) == TRUE,
        "ESTABLISHED + RST → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_ESTABLISHED,
            RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG) == TRUE,
        "ESTABLISHED + SYN-ACK → valid (handshake retransmit / port reuse)", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_ESTABLISHED,
            RTE_TCP_SYN_FLAG) == FALSE,
        "ESTABLISHED + bare SYN → invalid", NULL);

    /* TIME_WAIT: only ACK (late retransmit) and RST */
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_TIME_WAIT,
            RTE_TCP_ACK_FLAG) == TRUE,
        "TIME_WAIT + ACK → valid (late retransmit)", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_TIME_WAIT,
            RTE_TCP_RST_FLAG) == TRUE,
        "TIME_WAIT + RST → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_TIME_WAIT,
            RTE_TCP_SYN_FLAG) == FALSE,
        "TIME_WAIT + SYN → invalid (WAN cannot initiate through SNAT)", NULL);

    /* NONE: mid-stream pickup — accept ACK / FIN(WAN side) / RST so pre-existing flows
     * (e.g. connections that survived a FastRG restart) keep working; SYN from WAN
     * is still invalid because WAN cannot initiate through SNAT */
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_NONE,
            RTE_TCP_ACK_FLAG) == TRUE,
        "NONE + ACK → valid (mid-stream pickup)", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_NONE,
            RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG) == TRUE,
        "NONE + FIN+ACK (WAN side) → valid (mid-stream pickup)", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_NONE,
            RTE_TCP_RST_FLAG) == TRUE,
        "NONE + RST → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_NONE,
            RTE_TCP_SYN_FLAG) == FALSE,
        "NONE + SYN → invalid (WAN cannot initiate through SNAT)", NULL);

    /* CLOSE: connection fully closed — drop all inbound */
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_CLOSE,
            RTE_TCP_RST_FLAG) == FALSE,
        "CLOSE + RST → invalid (connection dead)", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_CLOSE,
            RTE_TCP_SYN_FLAG) == FALSE,
        "CLOSE + SYN → invalid (WAN cannot initiate through SNAT)", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_CLOSE,
            RTE_TCP_ACK_FLAG) == FALSE,
        "CLOSE + ACK → invalid", NULL);

    /* MID_STREAM: same allowlist as NONE plus SYN-ACK (handshake retransmit
     * straddling the pickup window) */
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_MID_STREAM,
            RTE_TCP_ACK_FLAG) == TRUE,
        "MID_STREAM + ACK → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_MID_STREAM,
            RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG) == TRUE,
        "MID_STREAM + FIN+ACK (WAN side) → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_MID_STREAM,
            RTE_TCP_RST_FLAG) == TRUE,
        "MID_STREAM + RST → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_MID_STREAM,
            RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG) == TRUE,
        "MID_STREAM + SYN-ACK → valid", NULL);
    TEST_ASSERT(tcp_conntrack_inbound_valid(TCP_CONNTRACK_MID_STREAM,
            RTE_TCP_SYN_FLAG) == FALSE,
        "MID_STREAM + SYN → invalid (WAN cannot initiate through SNAT)", NULL);
}

/**
 * Test 15: MID_STREAM pickup
 * NONE + ACK (LAN side) → MID_STREAM (probationary, 60s timeout)
 */
static void test_mid_stream_pickup(void)
{
    printf("\nTesting MID_STREAM pickup from NONE:\n");
    printf("====================================\n\n");

    addr_table_t entry;
    init_entry(&entry, TCP_CONNTRACK_NONE);

    U64 before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_ACK_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_MID_STREAM,
        "NONE + ACK (LAN side) → MID_STREAM",
        "expected MID_STREAM(%d), got %d", TCP_CONNTRACK_MID_STREAM, entry.tcp_state);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_MID_STREAM),
        "NONE + ACK sets MID_STREAM timeout (60s)",
        "expire_at not set to ~%d seconds", TCP_TIMEOUT_MID_STREAM);
}

/**
 * Test 16: MID_STREAM promotion on WAN-side ACK
 * MID_STREAM + ACK (WAN side) → ESTABLISHED (bidirectional confirmed)
 */
static void test_mid_stream_promotion(void)
{
    printf("\nTesting MID_STREAM promotion on WAN-side ACK:\n");
    printf("===============================================\n\n");

    addr_table_t entry;
    init_entry(&entry, TCP_CONNTRACK_MID_STREAM);

    U64 before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_ACK_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_ESTABLISHED,
        "MID_STREAM + ACK (WAN side) → ESTABLISHED",
        "expected ESTABLISHED(%d), got %d", TCP_CONNTRACK_ESTABLISHED, entry.tcp_state);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_ESTABLISHED),
        "MID_STREAM promotion grants ESTABLISHED timeout (7200s)",
        "expire_at not set to ~%d seconds", TCP_TIMEOUT_ESTABLISHED);
}

/**
 * Test 17: MID_STREAM promotion on PSH+ACK from WAN side
 * PSH is transparent in tcp_flags_to_event — PSH+ACK from WAN side also promotes.
 */
static void test_mid_stream_promotion_on_psh_ack(void)
{
    printf("\nTesting MID_STREAM promotion on WAN-side PSH+ACK:\n");
    printf("===================================================\n\n");

    addr_table_t entry;
    init_entry(&entry, TCP_CONNTRACK_MID_STREAM);

    /* PSH flag is 0x08 — not exposed as RTE_TCP_PSH_FLAG in older DPDK headers.
     * tcp_flags_to_event ignores PSH/URG/ECE/CWR, so PSH+ACK behaves like ACK. */
    U8 psh_ack = 0x08 | RTE_TCP_ACK_FLAG;
    tcp_conntrack_fsm(&entry, psh_ack, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_ESTABLISHED,
        "MID_STREAM + PSH+ACK (WAN side) → ESTABLISHED",
        "expected ESTABLISHED(%d), got %d", TCP_CONNTRACK_ESTABLISHED, entry.tcp_state);
}

/**
 * Test 18: MID_STREAM does NOT promote on LAN-side ACK
 * MID_STREAM + ACK (LAN side) → MID_STREAM, timeout refreshed to 60s
 */
static void test_mid_stream_no_promote_on_lan_ack(void)
{
    printf("\nTesting MID_STREAM stays on LAN-side ACK:\n");
    printf("============================================\n\n");

    addr_table_t entry;
    init_entry(&entry, TCP_CONNTRACK_MID_STREAM);
    /* Drift the timer forward so we can confirm a refresh actually happened. */
    rte_atomic64_set(&entry.expire_at,
        fastrg_get_cur_cycles() + 5ULL * fastrg_get_cycles_in_sec());

    U64 before = fastrg_get_cur_cycles();
    tcp_conntrack_fsm(&entry, RTE_TCP_ACK_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_MID_STREAM,
        "MID_STREAM + ACK (LAN side) stays MID_STREAM (no promotion)",
        "expected MID_STREAM(%d), got %d", TCP_CONNTRACK_MID_STREAM, entry.tcp_state);
    TEST_ASSERT(expire_at_approx(before, rte_atomic64_read(&entry.expire_at), TCP_TIMEOUT_MID_STREAM),
        "MID_STREAM + ACK (LAN side) refreshes 60s timeout",
        "expire_at not refreshed to ~%d seconds", TCP_TIMEOUT_MID_STREAM);
}

/**
 * Test 19: MID_STREAM transitions for SYN / FIN / RST
 */
static void test_mid_stream_other_transitions(void)
{
    printf("\nTesting MID_STREAM other transitions:\n");
    printf("======================================\n\n");

    addr_table_t entry;

    /* MID_STREAM + SYN → SYN_SENT (port reuse), fin_flags cleared */
    init_entry(&entry, TCP_CONNTRACK_MID_STREAM);
    entry.tcp_fin_flags = TCP_FIN_FLAG_LAN;
    tcp_conntrack_fsm(&entry, RTE_TCP_SYN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_SYN_SENT,
        "MID_STREAM + SYN → SYN_SENT",
        "expected SYN_SENT(%d), got %d", TCP_CONNTRACK_SYN_SENT, entry.tcp_state);
    TEST_ASSERT(entry.tcp_fin_flags == 0,
        "MID_STREAM + SYN clears fin_flags", NULL);

    /* MID_STREAM + FIN (LAN side) → FIN_WAIT */
    init_entry(&entry, TCP_CONNTRACK_MID_STREAM);
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_FIN_WAIT,
        "MID_STREAM + FIN (LAN side) → FIN_WAIT",
        "expected FIN_WAIT(%d), got %d", TCP_CONNTRACK_FIN_WAIT, entry.tcp_state);

    /* MID_STREAM + FIN (WAN side) → CLOSE_WAIT */
    init_entry(&entry, TCP_CONNTRACK_MID_STREAM);
    tcp_conntrack_fsm(&entry, RTE_TCP_FIN_FLAG, TRUE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_CLOSE_WAIT,
        "MID_STREAM + FIN (WAN side) → CLOSE_WAIT",
        "expected CLOSE_WAIT(%d), got %d", TCP_CONNTRACK_CLOSE_WAIT, entry.tcp_state);

    /* MID_STREAM + RST → CLOSE */
    init_entry(&entry, TCP_CONNTRACK_MID_STREAM);
    tcp_conntrack_fsm(&entry, RTE_TCP_RST_FLAG, FALSE);
    TEST_ASSERT(entry.tcp_state == TCP_CONNTRACK_CLOSE,
        "MID_STREAM + RST → CLOSE",
        "expected CLOSE(%d), got %d", TCP_CONNTRACK_CLOSE, entry.tcp_state);
}

/* Helper: build an rte_tcp_hdr with seq/ack/win/flags in network byte order. */
static struct rte_tcp_hdr build_tcp_hdr(U32 seq, U32 ack, U16 win, U8 flags)
{
    struct rte_tcp_hdr th;
    memset(&th, 0, sizeof(th));
    th.sent_seq  = rte_cpu_to_be_32(seq);
    th.recv_ack  = rte_cpu_to_be_32(ack);
    th.rx_win    = rte_cpu_to_be_16(win);
    th.data_off  = (5 << 4); /* 5 * 4 = 20-byte TCP header, no options */
    th.tcp_flags = flags;
    return th;
}

/**
 * Test 20: TCP sequence/ack window validation
 */
static void test_seq_window_validation(void)
{
    printf("\nTesting tcp_conntrack_seq_valid:\n");
    printf("=================================\n\n");

    addr_table_t entry;
    struct rte_tcp_hdr th;

    /* SYN bypass: any seq/ack passes if SYN flag is set */
    init_entry(&entry, TCP_CONNTRACK_NONE);
    th = build_tcp_hdr(0xDEADBEEF, 0, 65535, RTE_TCP_SYN_FLAG);
    TEST_ASSERT(tcp_conntrack_seq_valid(&entry, &th, FALSE) == TRUE,
        "SYN packet bypasses seq check",
        "SYN should always be valid");

    /* Zero baseline: first non-SYN accepted to seed state */
    init_entry(&entry, TCP_CONNTRACK_MID_STREAM);
    th = build_tcp_hdr(1000, 2000, 65535, RTE_TCP_ACK_FLAG);
    TEST_ASSERT(tcp_conntrack_seq_valid(&entry, &th, TRUE) == TRUE,
        "Zero baseline accepts seed packet",
        "First packet on uninitialised entry should pass");

    /* In-window seq passes */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    /* Set up: LAN side sent up to seq_end=10000 ack=5000 win=65535;
     * WAN side sent up to seq_end=5000  ack=10000 win=65535 */
    entry.max_seq_end_lan = 10000;
    entry.max_ack_lan     = 10000;
    entry.max_win_lan     = 65535;
    entry.max_seq_end_wan = 5000;
    entry.max_ack_wan     = 5000;
    entry.max_win_wan     = 65535;
    /* Inbound (is_reply=TRUE): WAN side seq=5500 (within LAN's expected ack window) */
    th = build_tcp_hdr(5500, 9000, 65535, RTE_TCP_ACK_FLAG);
    TEST_ASSERT(tcp_conntrack_seq_valid(&entry, &th, TRUE) == TRUE,
        "In-window inbound packet is valid", NULL);

    /* Out-of-window seq dropped: seq beyond peer_max_ack + 16 MB slack */
    th = build_tcp_hdr(10000 + 0x01000000, 9000, 65535, RTE_TCP_ACK_FLAG);
    TEST_ASSERT(tcp_conntrack_seq_valid(&entry, &th, TRUE) == FALSE,
        "Out-of-window seq is dropped",
        "Expected drop on far-future seq (>16MB ahead)");

    /* Far-past seq dropped: seq more than 16 MB behind peer_max_ack */
    th = build_tcp_hdr(10000 - 0x01000000 - 1, 9000, 65535, RTE_TCP_ACK_FLAG);
    TEST_ASSERT(tcp_conntrack_seq_valid(&entry, &th, TRUE) == FALSE,
        "Far-past seq is dropped",
        "Expected drop on ancient seq (>16MB behind)");

    /* ACK above peer's max_seq_end + 0xFFFF slack dropped (you can't ack what
     * the peer hasn't sent).  peer_max_seqend = max_seq_end_lan = 10000. */
    th = build_tcp_hdr(5500, 10000 + 0xFFFF + 100, 65535, RTE_TCP_ACK_FLAG);
    TEST_ASSERT(tcp_conntrack_seq_valid(&entry, &th, TRUE) == FALSE,
        "ACK beyond what peer has sent is dropped", NULL);

    /* RST in-window passes */
    th = build_tcp_hdr(5500, 9000, 0, RTE_TCP_RST_FLAG);
    TEST_ASSERT(tcp_conntrack_seq_valid(&entry, &th, TRUE) == TRUE,
        "In-window RST passes", NULL);
}

/**
 * Test 21: tcp_conntrack_seq_update — max() update behaviour
 */
static void test_seq_update(void)
{
    printf("\nTesting tcp_conntrack_seq_update:\n");
    printf("==================================\n\n");

    addr_table_t entry;
    struct rte_tcp_hdr th;

    /* LAN-side update with payload bumps max_seq_end_lan and max_ack_lan */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    th = build_tcp_hdr(1000, 2000, 8192, RTE_TCP_ACK_FLAG);
    tcp_conntrack_seq_update(&entry, &th, 500, FALSE);
    TEST_ASSERT(entry.max_seq_end_lan == 1000 + 500,
        "max_seq_end_lan = seq + payload (no SYN/FIN)",
        "expected %u, got %u", 1500u, entry.max_seq_end_lan);
    TEST_ASSERT(entry.max_ack_lan == 2000,
        "max_ack_lan = ack", NULL);
    TEST_ASSERT(entry.max_win_lan == 8192,
        "max_win_lan = win", NULL);
    TEST_ASSERT(entry.max_seq_end_wan == 0,
        "max_seq_end_wan untouched on LAN side update", NULL);

    /* SYN occupies one byte of seq space — seq_end = seq + 1 */
    init_entry(&entry, TCP_CONNTRACK_NONE);
    th = build_tcp_hdr(1000, 0, 65535, RTE_TCP_SYN_FLAG);
    tcp_conntrack_seq_update(&entry, &th, 0, FALSE);
    TEST_ASSERT(entry.max_seq_end_lan == 1001,
        "SYN consumes 1 byte of seq space",
        "expected 1001, got %u", entry.max_seq_end_lan);

    /* Older seq does NOT decrement max */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    entry.max_seq_end_lan = 5000;
    entry.max_ack_lan     = 4000;
    th = build_tcp_hdr(2000, 3000, 65535, RTE_TCP_ACK_FLAG);
    tcp_conntrack_seq_update(&entry, &th, 100, FALSE);
    TEST_ASSERT(entry.max_seq_end_lan == 5000,
        "max_seq_end_lan is monotonic (older seq ignored)",
        "expected 5000, got %u", entry.max_seq_end_lan);
    TEST_ASSERT(entry.max_ack_lan == 4000,
        "max_ack_lan is monotonic (older ack ignored)", NULL);

    /* WAN-side update bumps fields, not LAN-side */
    init_entry(&entry, TCP_CONNTRACK_ESTABLISHED);
    th = build_tcp_hdr(7000, 8000, 4096, RTE_TCP_ACK_FLAG);
    tcp_conntrack_seq_update(&entry, &th, 200, TRUE);
    TEST_ASSERT(entry.max_seq_end_wan == 7200,
        "WAN-side update bumps max_seq_end_wan",
        "expected 7200, got %u", entry.max_seq_end_wan);
    TEST_ASSERT(entry.max_seq_end_lan == 0,
        "WAN-side update leaves LAN-side fields alone", NULL);
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
    test_close_new_connection();
    test_time_wait_new_connection();
    test_established_port_reuse();
    test_inbound_spi();
    test_mid_stream_pickup();
    test_mid_stream_promotion();
    test_mid_stream_promotion_on_psh_ack();
    test_mid_stream_no_promote_on_lan_ack();
    test_mid_stream_other_transitions();
    test_seq_window_validation();
    test_seq_update();

    *total_tests += test_count;
    *total_pass += pass_count;
}
