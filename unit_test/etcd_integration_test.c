#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <common.h>

#include <rte_atomic.h>
#include <rte_timer.h>

#include "../northbound/controller/etcd_client.h"
#include "../src/dhcpd/dhcpd.h"
#include "../src/etcd_integration.h"
#include "../src/cli_request.h"
#include "../src/fastrg.h"
#include "../src/pppd/pppd.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

static etcd_event_t *make_sweep_event(const int *present, int count)
{
    etcd_event_t *event = calloc(1, sizeof(*event));
    assert(event != NULL);

    event->kind = ETCD_EVENT_HSI_SWEEP;
    event->event_data.sweep.count = count;
    if (count > 0) {
        event->event_data.sweep.present_ccb_ids = malloc(sizeof(int) * count);
        assert(event->event_data.sweep.present_ccb_ids != NULL);
        memcpy(event->event_data.sweep.present_ccb_ids, present, sizeof(int) * count);
    }

    return event;
}

static FastRG_t *make_sweep_fixture(void)
{
    FastRG_t *fastrg_ccb = calloc(1, sizeof(*fastrg_ccb));
    assert(fastrg_ccb != NULL);

    fastrg_ccb->user_count = 2;
    fastrg_ccb->max_user_count = 2;
    fastrg_ccb->loglvl = (U8)-1;
    fastrg_ccb->vlan_userid_map = calloc(MAX_VLAN_ID, sizeof(*fastrg_ccb->vlan_userid_map));
    assert(fastrg_ccb->vlan_userid_map != NULL);
    assert(pppd_init(fastrg_ccb) == SUCCESS);

    fastrg_ccb->dhcp_ccb = calloc(fastrg_ccb->max_user_count, sizeof(*fastrg_ccb->dhcp_ccb));
    assert(fastrg_ccb->dhcp_ccb != NULL);
    for(int i=0; i<fastrg_ccb->max_user_count; i++) {
        dhcp_ccb_t *dhcp_ccb = calloc(1, sizeof(*dhcp_ccb));
        assert(dhcp_ccb != NULL);
        dhcp_ccb->fastrg_ccb = fastrg_ccb;
        rte_atomic16_init(&dhcp_ccb->dhcp_bool);
        dhcp_ccb->per_lan_user_pool = calloc(1, sizeof(*dhcp_ccb->per_lan_user_pool));
        assert(dhcp_ccb->per_lan_user_pool != NULL);
        dhcp_ccb->per_lan_user_pool[0] = calloc(1, sizeof(*dhcp_ccb->per_lan_user_pool[0]));
        assert(dhcp_ccb->per_lan_user_pool[0] != NULL);
        rte_timer_init(&dhcp_ccb->per_lan_user_pool[0]->lan_user_info.timer);
        dhcp_ccb->per_lan_user_pool_len = 1;
        fastrg_ccb->dhcp_ccb[i] = dhcp_ccb;
    }

    return fastrg_ccb;
}

static void free_sweep_fixture(FastRG_t *fastrg_ccb)
{
    pppd_cleanup_ccb(fastrg_ccb);

    for(int i=0; i<fastrg_ccb->max_user_count; i++) {
        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, i);
        free(dhcp_ccb->per_lan_user_pool[0]);
        free(dhcp_ccb->per_lan_user_pool);
        free(dhcp_ccb);
    }
    free(fastrg_ccb->dhcp_ccb);
    free(fastrg_ccb->vlan_userid_map);
    free(fastrg_ccb);
}

static void set_sweep_user(FastRG_t *fastrg_ccb, int ccb_id, U16 vlan_id, BOOL ppp_active)
{
    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);

    rte_atomic16_set(&ppp_ccb->vlan_id, vlan_id);
    rte_atomic16_set(&ppp_ccb->ppp_bool, ppp_active);
    rte_atomic16_set(&dhcp_ccb->dhcp_bool, 0);
}

static void test_parse_user_id_contract(void)
{
    TEST_ASSERT(parse_user_id("1", 0) == -1, "zero bound rejects user 1", "");
    TEST_ASSERT(parse_user_id("3", 0) == -1, "zero bound rejects user 3", "");
    TEST_ASSERT(parse_user_id("1", 2) == 0, "user 1 maps to ccb 0", "");
    TEST_ASSERT(parse_user_id("2", 2) == 1, "user 2 maps to ccb 1", "");
    TEST_ASSERT(parse_user_id("3", 2) == -1, "user above bound is rejected", "");
    TEST_ASSERT(parse_user_id("0", 2) == -1, "user zero is rejected", "");
    TEST_ASSERT(parse_user_id("x", 2) == -1, "non-numeric user is rejected", "");
}

static void test_reconcile_sweep(void)
{
    FastRG_t *fastrg_ccb = make_sweep_fixture();
    ppp_ccb_t *user1 = PPPD_GET_CCB(fastrg_ccb, 0);
    ppp_ccb_t *user2 = PPPD_GET_CCB(fastrg_ccb, 1);

    set_sweep_user(fastrg_ccb, 0, 100, FALSE);
    set_sweep_user(fastrg_ccb, 1, 0, FALSE);
    int user1_present[] = {0};
    etcd_event_t *event = make_sweep_event(user1_present, 1);
    etcd_event_dispatch(fastrg_ccb, event);
    etcd_event_free(event);
    TEST_ASSERT(rte_atomic16_read(&user1->vlan_id) == 100,
        "sweep retains a user present in etcd", "");

    set_sweep_user(fastrg_ccb, 0, 101, FALSE);
    int user2_present[] = {1};
    event = make_sweep_event(user2_present, 1);
    etcd_event_dispatch(fastrg_ccb, event);
    etcd_event_free(event);
    TEST_ASSERT(rte_atomic16_read(&user1->vlan_id) == 0,
        "sweep removes an inactive user absent from etcd", "");

    set_sweep_user(fastrg_ccb, 0, 102, TRUE);
    event = make_sweep_event(user2_present, 1);
    etcd_event_dispatch(fastrg_ccb, event);
    etcd_event_free(event);
    TEST_ASSERT(rte_atomic16_read(&user1->vlan_id) == 102,
        "sweep keeps an active PPPoE user absent from etcd", "");
    rte_atomic16_set(&user1->ppp_bool, 0);

    set_sweep_user(fastrg_ccb, 1, 200, FALSE);
    event = make_sweep_event(NULL, 0);
    etcd_event_dispatch(fastrg_ccb, event);
    etcd_event_free(event);
    TEST_ASSERT(rte_atomic16_read(&user2->vlan_id) == 0,
        "empty present set removes an inactive local user", "");

    free_sweep_fixture(fastrg_ccb);
}

static void test_etcd_event_dispatch(void)
{
    FastRG_t *fastrg_ccb = make_sweep_fixture();
    etcd_event_t *ev = calloc(1, sizeof(*ev));
    assert(ev != NULL);

    /* user 9 is outside the fixture's subscriber count, so the apply fails —
     * the case where a stray verdict would be most visible. */
    ev->kind = ETCD_EVENT_HSI;
    ev->action = HSI_ACTION_UPDATE;
    strncpy(ev->node_id, "node", sizeof(ev->node_id) - 1);
    strncpy(ev->user_id, "9", sizeof(ev->user_id) - 1);
    strncpy(ev->event_data.hsi.config.user_id, "9",
        sizeof(ev->event_data.hsi.config.user_id) - 1);

    rte_atomic32_init(&fastrg_ccb->cli_request_result);
    rte_atomic32_set(&fastrg_ccb->cli_request_result, CLI_REQUEST_NONE);
    etcd_event_dispatch(fastrg_ccb, ev);
    /*
     * The gRPC ApplyConfig waiter is released by the cp_q apply command alone, so an
     * etcd-side HSI event passing through at the same time must not answer for it.
     */
    TEST_ASSERT(rte_atomic32_read(&fastrg_ccb->cli_request_result) == CLI_REQUEST_NONE,
        "an etcd HSI event leaves the CLI apply verdict untouched", "got %d",
        rte_atomic32_read(&fastrg_ccb->cli_request_result));

    etcd_event_free(ev);
    free_sweep_fixture(fastrg_ccb);
}

void test_etcd_integration(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;
    test_count = 0;
    pass_count = 0;

    test_parse_user_id_contract();
    test_reconcile_sweep();
    test_etcd_event_dispatch();

    *total_tests += test_count;
    *total_pass += pass_count;
}
