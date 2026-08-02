/*
 * fastrg_test.c — fixed-max prealloc semantics of FastRG_t.
 *
 * The pre-refactor stats-resize cases were removed with the resize code
 * itself (approved 2026-07-31); this file now carries only the fixed-max
 * cases for fastrg.c-level state.
 */
#include <stdlib.h>
#include <string.h>

#include <common.h>

#include <rte_lcore.h>

#include "../src/fastrg.h"
#include "../src/pppd/pppd.h"
#include "../src/dhcpd/dhcpd.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

static void test_user_count_change_keeps_slots(FastRG_t *fastrg_ccb)
{
    printf("\nTesting fixed-max user_count change (slots untouched):\n");
    printf("=========================================\n\n");

    U16 old_user_count = fastrg_ccb->user_count;
    U16 old_max = fastrg_ccb->max_user_count;
    void *slot0_ppp = fastrg_ccb->ppp_ccb[0];
    void *slot0_dhcp = fastrg_ccb->dhcp_ccb[0];

    fastrg_ccb->max_user_count = 1;

    /* Raising the accessible bound only changes the number; the fixed
     * pointer arrays and slot objects stay bit-identical. */
    fastrg_ccb->user_count = 1;
    TEST_ASSERT(PPPD_GET_CCB(fastrg_ccb, 0) == slot0_ppp,
        "raising user_count leaves ppp slot pointer unchanged", "");
    TEST_ASSERT(DHCPD_GET_CCB(fastrg_ccb, 0) == slot0_dhcp,
        "raising user_count leaves dhcp slot pointer unchanged", "");

    /* Lowering the bound must not free or clear the slot either. */
    fastrg_ccb->user_count = 0;
    TEST_ASSERT(fastrg_ccb->ppp_ccb[0] == slot0_ppp,
        "lowering user_count leaves ppp slot pointer unchanged", "");
    TEST_ASSERT(fastrg_ccb->dhcp_ccb[0] == slot0_dhcp,
        "lowering user_count leaves dhcp slot pointer unchanged", "");

    fastrg_ccb->user_count = old_user_count;
    fastrg_ccb->max_user_count = old_max;
}

void test_fastrg(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║                 fastrg Module Unit Tests                  ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    test_user_count_change_keeps_slots(fastrg_ccb);

    printf("\nfastrg tests: %d passed, %d failed\n", pass_count, test_count - pass_count);
    *total_tests += test_count;
    *total_pass += pass_count;
}
