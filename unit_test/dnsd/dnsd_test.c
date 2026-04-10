#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <common.h>

#include "../../src/dnsd/dnsd.h"
#include "../../src/fastrg.h"
#include "../test_helper.h"

static int test_count = 0;
static int pass_count = 0;

/* ---- dnsd_check_failover tests ---- */

static void test_failover_disabled(void)
{
    printf("\nTesting dnsd_check_failover (proxy disabled):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0x02020202);
    /* dns_proxy_init leaves dns_proxy_enabled = FALSE */
    state.last_response_time = fastrg_get_cur_cycles() -
        (U64)(DNS_FAILOVER_TIMEOUT_SECS + 10) * fastrg_get_cycles_in_sec();

    U32 before = state.active_dns;
    dnsd_check_failover(&state);
    TEST_ASSERT(state.active_dns == before,
        "failover does NOT fire when dns_proxy_enabled is FALSE", "");
    dns_proxy_cleanup(&state);
}

static void test_failover_no_secondary(void)
{
    printf("\nTesting dnsd_check_failover (no secondary DNS):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);
    state.dns_proxy_enabled = TRUE;
    state.last_response_time = fastrg_get_cur_cycles() -
        (U64)(DNS_FAILOVER_TIMEOUT_SECS + 10) * fastrg_get_cycles_in_sec();

    U32 before = state.active_dns;
    dnsd_check_failover(&state);
    TEST_ASSERT(state.active_dns == before,
        "failover does NOT fire when secondary_dns is 0", "");
    dns_proxy_cleanup(&state);
}

static void test_failover_timeout_not_reached(void)
{
    printf("\nTesting dnsd_check_failover (timeout not reached):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0x02020202);
    state.dns_proxy_enabled = TRUE;
    /* last response just now — timeout NOT reached */
    state.last_response_time = fastrg_get_cur_cycles();

    U32 before = state.active_dns;
    dnsd_check_failover(&state);
    TEST_ASSERT(state.active_dns == before,
        "active_dns unchanged when timeout not reached", "");
    dns_proxy_cleanup(&state);
}

static void test_failover_primary_to_secondary(void)
{
    printf("\nTesting dnsd_check_failover (primary -> secondary):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0x02020202);
    state.dns_proxy_enabled = TRUE;
    /* simulate timeout expired */
    state.last_response_time = fastrg_get_cur_cycles() -
        (U64)(DNS_FAILOVER_TIMEOUT_SECS + 1) * fastrg_get_cycles_in_sec();

    dnsd_check_failover(&state);
    TEST_ASSERT(state.active_dns == state.secondary_dns,
        "active_dns switches to secondary after timeout", "");

    /* simulate timeout expired again — should switch back to primary */
    state.last_response_time = fastrg_get_cur_cycles() -
        (U64)(DNS_FAILOVER_TIMEOUT_SECS + 1) * fastrg_get_cycles_in_sec();
    dnsd_check_failover(&state);
    TEST_ASSERT(state.active_dns == state.primary_dns,
        "active_dns switches back to primary on second timeout", "");
    dns_proxy_cleanup(&state);
}

/* ---- find_free_pending tests ---- */

static void test_find_free_pending_all_inactive(void)
{
    printf("\nTesting find_free_pending (all slots inactive):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    dns_pending_query_t *slot = find_free_pending(&state);
    TEST_ASSERT(slot != NULL, "returns a slot from fresh state", "");
    TEST_ASSERT(slot->active == FALSE, "returned slot is inactive", "");
    dns_proxy_cleanup(&state);
}

static void test_find_free_pending_skips_active(void)
{
    printf("\nTesting find_free_pending (first slot active, second free):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    /* Mark slot 0 as active and fresh */
    state.pending[0].active    = TRUE;
    state.pending[0].start_tsc = fastrg_get_cur_cycles();
    state.pending[0].upstream_id = 1;

    dns_pending_query_t *slot = find_free_pending(&state);
    TEST_ASSERT(slot != NULL, "returns a slot", "");
    TEST_ASSERT(slot != &state.pending[0], "does not return the active slot 0", "");
    TEST_ASSERT(slot->active == FALSE, "returned slot is inactive", "");
    dns_proxy_cleanup(&state);
}

static void test_find_free_pending_expired(void)
{
    printf("\nTesting find_free_pending (all active, one expired):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    /* Fill all 32 slots as active with fresh start_tsc */
    U64 now = fastrg_get_cur_cycles();
    for (int i = 0; i < DNS_MAX_PENDING_QUERIES; i++) {
        state.pending[i].active    = TRUE;
        state.pending[i].start_tsc = now;
        state.pending[i].upstream_id = (U16)(i + 1);
    }

    /* Expire slot 5 by backdating its start_tsc */
    state.pending[5].start_tsc = now -
        (U64)(DNS_PENDING_TIMEOUT_SECS + 1) * fastrg_get_cycles_in_sec();

    dns_pending_query_t *slot = find_free_pending(&state);
    TEST_ASSERT(slot != NULL, "returns a slot despite all being active", "");
    TEST_ASSERT(slot == &state.pending[5], "returns the expired slot (slot 5)", "");
    TEST_ASSERT(slot->active == FALSE, "expired slot is marked inactive", "");
    dns_proxy_cleanup(&state);
}

static void test_find_free_pending_full(void)
{
    printf("\nTesting find_free_pending (all slots active and fresh):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    U64 now = fastrg_get_cur_cycles();
    for (int i = 0; i < DNS_MAX_PENDING_QUERIES; i++) {
        state.pending[i].active    = TRUE;
        state.pending[i].start_tsc = now;
        state.pending[i].upstream_id = (U16)(i + 1);
    }

    dns_pending_query_t *slot = find_free_pending(&state);
    TEST_ASSERT(slot == NULL, "returns NULL when all slots are active and fresh", "");
    dns_proxy_cleanup(&state);
}

/* ---- find_pending_by_upstream_id tests ---- */

static void test_find_pending_by_id_hit(void)
{
    printf("\nTesting find_pending_by_upstream_id (match found):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    state.pending[3].active      = TRUE;
    state.pending[3].upstream_id = 42;
    state.pending[3].start_tsc   = fastrg_get_cur_cycles();

    dns_pending_query_t *found = find_pending_by_upstream_id(&state, 42);
    TEST_ASSERT(found != NULL, "returns non-NULL for matching id", "");
    TEST_ASSERT(found == &state.pending[3], "returns correct slot (slot 3)", "");
    TEST_ASSERT(found->upstream_id == 42, "upstream_id matches", "");
    dns_proxy_cleanup(&state);
}

static void test_find_pending_by_id_miss(void)
{
    printf("\nTesting find_pending_by_upstream_id (no match):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    state.pending[0].active      = TRUE;
    state.pending[0].upstream_id = 7;
    state.pending[0].start_tsc   = fastrg_get_cur_cycles();

    dns_pending_query_t *found = find_pending_by_upstream_id(&state, 99);
    TEST_ASSERT(found == NULL, "returns NULL when upstream_id not found", "");
    dns_proxy_cleanup(&state);
}

static void test_find_pending_by_id_expired(void)
{
    printf("\nTesting find_pending_by_upstream_id (match but expired):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    /* Slot with matching id but backdated (timed out) */
    state.pending[1].active      = TRUE;
    state.pending[1].upstream_id = 55;
    state.pending[1].start_tsc   = fastrg_get_cur_cycles() -
        (U64)(DNS_PENDING_TIMEOUT_SECS + 1) * fastrg_get_cycles_in_sec();

    dns_pending_query_t *found = find_pending_by_upstream_id(&state, 55);
    TEST_ASSERT(found == NULL, "returns NULL for timed-out pending query", "");
    TEST_ASSERT(state.pending[1].active == FALSE,
        "expired slot is marked inactive after lookup", "");
    dns_proxy_cleanup(&state);
}

static void test_find_pending_by_id_inactive(void)
{
    printf("\nTesting find_pending_by_upstream_id (slot inactive):\n");
    printf("=========================================\n\n");

    dns_proxy_state_t state = {0};
    dns_proxy_init(&state, 0x01010101, 0);

    /* Slot has matching id but active == FALSE */
    state.pending[2].active      = FALSE;
    state.pending[2].upstream_id = 77;
    state.pending[2].start_tsc   = fastrg_get_cur_cycles();

    dns_pending_query_t *found = find_pending_by_upstream_id(&state, 77);
    TEST_ASSERT(found == NULL, "returns NULL when slot is inactive", "");
    dns_proxy_cleanup(&state);
}

void test_dnsd(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           DNS Daemon Unit Tests                             ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    /* dnsd_check_failover tests */
    test_failover_disabled();
    test_failover_no_secondary();
    test_failover_timeout_not_reached();
    test_failover_primary_to_secondary();

    /* find_free_pending tests */
    test_find_free_pending_all_inactive();
    test_find_free_pending_skips_active();
    test_find_free_pending_expired();
    test_find_free_pending_full();

    /* find_pending_by_upstream_id tests */
    test_find_pending_by_id_hit();
    test_find_pending_by_id_miss();
    test_find_pending_by_id_expired();
    test_find_pending_by_id_inactive();

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
