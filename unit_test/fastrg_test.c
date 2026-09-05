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

static void test_fastrg_gen_etcd_event(FastRG_t *fastrg_ccb)
{
    printf("\nTesting fastrg_gen_etcd_event function:\n");
    printf("=======================================\n\n");

    /* The shared fixture has no rings, so this case brings its own and hands
     * the CCB back untouched. cp_q holds 3 entries, enough to fill it. */
    struct rte_ring *saved_cp_q = fastrg_ccb->cp_q;
    struct rte_ring *saved_free_mail_ring = fastrg_ccb->free_mail_ring;
    struct rte_ring *cp_q = rte_ring_create("ev_test_cp_q", 4, rte_socket_id(), 0);
    struct rte_ring *free_mail_ring = rte_ring_create("ev_test_free_mail", 8, rte_socket_id(), 0);
    tFastRG_MBX mail_slot;
    etcd_event_t *ev = NULL;

    TEST_ASSERT(cp_q != NULL && free_mail_ring != NULL, "create the test rings",
        "cp_q %p free_mail_ring %p", (void *)cp_q, (void *)free_mail_ring);
    if (cp_q == NULL || free_mail_ring == NULL)
        goto out;

    memset(&mail_slot, 0, sizeof(mail_slot));
    fastrg_ccb->cp_q = cp_q;
    fastrg_ccb->free_mail_ring = free_mail_ring;
    rte_ring_enqueue(free_mail_ring, &mail_slot);

    ev = fastrg_alloc_etcd_event(ETCD_EVENT_HSI);
    TEST_ASSERT(ev != NULL, "allocate the test etcd event", "");
    if (ev == NULL)
        goto out;

    STATUS ret = fastrg_gen_etcd_event(fastrg_ccb, ev);
    TEST_ASSERT(ret == SUCCESS, "posting an etcd event returns SUCCESS", "got ERROR");
    TEST_ASSERT(rte_ring_count(free_mail_ring) == 0,
        "the post takes a slot out of the mail pool", "count %u",
        rte_ring_count(free_mail_ring));

    tFastRG_MBX *posted = NULL;
    TEST_ASSERT(rte_ring_dequeue(cp_q, (void **)&posted) == 0 && posted == &mail_slot,
        "the event lands on cp_q", "dequeue failed or wrong slot");
    TEST_ASSERT(posted->type == EV_ETCD && posted->etcd_ev == ev,
        "the mail carries EV_ETCD and the event pointer", "type %d etcd_ev %p",
        posted->type, (void *)posted->etcd_ev);

    /* No free mail slot left: the caller keeps the event. */
    ret = fastrg_gen_etcd_event(fastrg_ccb, ev);
    TEST_ASSERT(ret == ERROR, "an empty mail pool fails the post", "got SUCCESS");
    TEST_ASSERT(ev->kind == ETCD_EVENT_HSI,
        "the failed post leaves the event with the caller", "kind %d", ev->kind);

    /* cp_q full: the slot goes back to the pool and the event stays with the
     * caller. */
    rte_ring_enqueue(free_mail_ring, posted);
    while (rte_ring_free_count(cp_q) > 0)
        rte_ring_enqueue(cp_q, &mail_slot);
    ret = fastrg_gen_etcd_event(fastrg_ccb, ev);
    TEST_ASSERT(ret == ERROR, "a full cp_q fails the post", "got SUCCESS");
    TEST_ASSERT(rte_ring_count(free_mail_ring) == 1,
        "the failed post returns the mail slot", "count %u",
        rte_ring_count(free_mail_ring));
    TEST_ASSERT(ev->kind == ETCD_EVENT_HSI,
        "a full cp_q leaves the event with the caller", "kind %d", ev->kind);

    fastrg_ccb->cp_q = saved_cp_q;
    fastrg_ccb->free_mail_ring = saved_free_mail_ring;

out:
    etcd_event_free(ev);
    if (cp_q != NULL)
        rte_ring_free(cp_q);
    if (free_mail_ring != NULL)
        rte_ring_free(free_mail_ring);
}

void test_fastrg(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║                 fastrg Module Unit Tests                  ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    test_user_count_change_keeps_slots(fastrg_ccb);
    test_fastrg_gen_etcd_event(fastrg_ccb);

    printf("\nfastrg tests: %d passed, %d failed\n", pass_count, test_count - pass_count);
    *total_tests += test_count;
    *total_pass += pass_count;
}
