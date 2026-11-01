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

/* What the HSI apply-result reports would have sent over Kafka. */
static int apply_report_count = 0;
static BOOL apply_report_success = FALSE;
static char apply_report_err_code[32];

/* Linked in place of kafka_report_config_apply_result (-Wl,--wrap). */
void __wrap_kafka_report_config_apply_result(const char *user_id, const char *action,
    BOOL success, const char *err_code, const char *err_msg,
    const char *applied_resource_version, BOOL republished,
    int64_t applied_mod_revision)
{
    apply_report_count++;
    apply_report_success = success;
    snprintf(apply_report_err_code, sizeof(apply_report_err_code), "%s",
        err_code != NULL ? err_code : "");
}

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

/* The sweep fixture with both per-subscriber revision arrays allocated. */
static FastRG_t *make_revision_fixture(void)
{
    FastRG_t *fastrg_ccb = make_sweep_fixture();

    fastrg_ccb->hsi_subscriber_last_revision = calloc(fastrg_ccb->max_user_count, sizeof(S64));
    assert(fastrg_ccb->hsi_subscriber_last_revision != NULL);
    fastrg_ccb->dns_subscriber_last_revision = calloc(fastrg_ccb->max_user_count, sizeof(S64));
    assert(fastrg_ccb->dns_subscriber_last_revision != NULL);

    return fastrg_ccb;
}

static void free_revision_fixture(FastRG_t *fastrg_ccb)
{
    free(fastrg_ccb->hsi_subscriber_last_revision);
    fastrg_ccb->hsi_subscriber_last_revision = NULL;
    free(fastrg_ccb->dns_subscriber_last_revision);
    fastrg_ccb->dns_subscriber_last_revision = NULL;
    free_sweep_fixture(fastrg_ccb);
}

/* An HSI update event for one user. */
static etcd_event_t *make_hsi_event(const char *user_id, BOOL from_reconcile)
{
    etcd_event_t *event = calloc(1, sizeof(*event));
    assert(event != NULL);

    event->kind = ETCD_EVENT_HSI;
    event->action = HSI_ACTION_UPDATE;
    event->from_reconcile = from_reconcile;
    strncpy(event->node_id, "node", sizeof(event->node_id) - 1);
    strncpy(event->user_id, user_id, sizeof(event->user_id) - 1);
    strncpy(event->event_data.hsi.config.user_id, user_id,
        sizeof(event->event_data.hsi.config.user_id) - 1);

    return event;
}

/* A DNS set event for one user; domain NULL makes it an empty set. */
static etcd_event_t *make_dns_set_event(const char *user_id, BOOL from_reconcile,
    S64 revision, const char *domain, const char *ip)
{
    etcd_event_t *event = calloc(1, sizeof(*event));
    assert(event != NULL);

    event->kind = ETCD_EVENT_DNS_SET;
    event->action = HSI_ACTION_UPDATE;
    event->revision = revision;
    event->from_reconcile = from_reconcile;
    strncpy(event->node_id, "node", sizeof(event->node_id) - 1);
    strncpy(event->user_id, user_id, sizeof(event->user_id) - 1);
    if (domain != NULL) {
        dns_record_config_t *record = calloc(1, sizeof(*record));
        assert(record != NULL);
        strncpy(record->domain, domain, sizeof(record->domain) - 1);
        strncpy(record->ip, ip, sizeof(record->ip) - 1);
        record->ttl = 60;
        event->event_data.dns_set.records = record;
        event->event_data.dns_set.count = 1;
    }

    return event;
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

    /* Only a removal that happened records the reconcile revision. */
    fastrg_ccb = make_revision_fixture();
    user1 = PPPD_GET_CCB(fastrg_ccb, 0);
    set_sweep_user(fastrg_ccb, 0, 103, TRUE);
    fastrg_ccb->hsi_subscriber_last_revision[0] = 50;
    event = make_sweep_event(NULL, 0);
    event->event_data.sweep.reconcile_revision = 80;
    etcd_event_dispatch(fastrg_ccb, event);
    etcd_event_free(event);
    TEST_ASSERT(rte_atomic16_read(&user1->vlan_id) == 103 &&
        fastrg_ccb->hsi_subscriber_last_revision[0] == 50,
        "a failed sweep removal leaves the subscriber's revision alone", "got %lld",
        (long long)fastrg_ccb->hsi_subscriber_last_revision[0]);

    rte_atomic16_set(&user1->ppp_bool, 0);
    event = make_sweep_event(NULL, 0);
    event->event_data.sweep.reconcile_revision = 90;
    etcd_event_dispatch(fastrg_ccb, event);
    etcd_event_free(event);
    TEST_ASSERT(rte_atomic16_read(&user1->vlan_id) == 0 &&
        fastrg_ccb->hsi_subscriber_last_revision[0] == 90,
        "a sweep removal records the reconcile revision", "got %lld",
        (long long)fastrg_ccb->hsi_subscriber_last_revision[0]);

    free_revision_fixture(fastrg_ccb);
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

    /* DNS sets: live ones always apply, a reconcile older than the applied set is skipped. */
    fastrg_ccb = make_revision_fixture();
    dns_static_table_t *table = &DHCPD_GET_CCB(fastrg_ccb, 0)->dns_state.static_table;

    ev = make_dns_set_event("1", FALSE, 100, "a.example", "192.0.2.1");
    etcd_event_dispatch(fastrg_ccb, ev);
    etcd_event_free(ev);
    TEST_ASSERT(dns_static_lookup(table, "a.example") != NULL &&
        fastrg_ccb->dns_subscriber_last_revision[0] == 100,
        "a live DNS set applies and records its revision", "got %lld",
        (long long)fastrg_ccb->dns_subscriber_last_revision[0]);

    ev = make_dns_set_event("1", TRUE, 90, NULL, NULL);
    etcd_event_dispatch(fastrg_ccb, ev);
    etcd_event_free(ev);
    TEST_ASSERT(dns_static_lookup(table, "a.example") != NULL &&
        fastrg_ccb->dns_subscriber_last_revision[0] == 100,
        "a reconcile DNS set older than the applied one is skipped", "got %lld",
        (long long)fastrg_ccb->dns_subscriber_last_revision[0]);

    ev = make_dns_set_event("1", TRUE, 100, "b.example", "192.0.2.2");
    etcd_event_dispatch(fastrg_ccb, ev);
    etcd_event_free(ev);
    TEST_ASSERT(dns_static_lookup(table, "a.example") == NULL &&
        dns_static_lookup(table, "b.example") != NULL &&
        fastrg_ccb->dns_subscriber_last_revision[0] == 100,
        "a reconcile DNS set at the applied revision still applies", "got %lld",
        (long long)fastrg_ccb->dns_subscriber_last_revision[0]);

    ev = make_dns_set_event("1", FALSE, 40, NULL, NULL);
    etcd_event_dispatch(fastrg_ccb, ev);
    etcd_event_free(ev);
    TEST_ASSERT(dns_static_get_count(table) == 0 &&
        fastrg_ccb->dns_subscriber_last_revision[0] == 40,
        "a live DNS set applies whatever its revision", "got %lld",
        (long long)fastrg_ccb->dns_subscriber_last_revision[0]);

    free_revision_fixture(fastrg_ccb);

    /* HSI user 9 is beyond the count: a reconcile skips it unreported, a live event reports it. */
    fastrg_ccb = make_sweep_fixture();
    apply_report_count = 0;

    ev = make_hsi_event("9", TRUE);
    etcd_event_dispatch(fastrg_ccb, ev);
    etcd_event_free(ev);
    TEST_ASSERT(apply_report_count == 0,
        "a reconcile HSI event beyond the subscriber count is not reported", "got %d report(s)",
        apply_report_count);

    ev = make_hsi_event("9", FALSE);
    etcd_event_dispatch(fastrg_ccb, ev);
    etcd_event_free(ev);
    TEST_ASSERT(apply_report_count == 1 && apply_report_success == FALSE &&
        strcmp(apply_report_err_code, "apply_failed") == 0,
        "a live HSI event beyond the subscriber count is reported as apply_failed",
        "got %d report(s), success=%d, err_code='%s'", apply_report_count,
        apply_report_success, apply_report_err_code);

    free_sweep_fixture(fastrg_ccb);
}

static void test_is_hsi_etcd_revision_newest(void)
{
    etcd_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = ETCD_EVENT_HSI;
    ev.action = HSI_ACTION_UPDATE;

    ev.from_reconcile = FALSE;
    ev.revision = 5;
    TEST_ASSERT(is_hsi_etcd_revision_newest(90, &ev) == TRUE,
        "a live watch event applies over a newer applied revision", "");

    ev.from_reconcile = TRUE;
    TEST_ASSERT(is_hsi_etcd_revision_newest(90, &ev) == FALSE,
        "a reconcile older than the applied revision is skipped", "");
    ev.revision = 90;
    TEST_ASSERT(is_hsi_etcd_revision_newest(90, &ev) == TRUE,
        "a reconcile at the applied revision still applies", "");
    ev.revision = 91;
    TEST_ASSERT(is_hsi_etcd_revision_newest(90, &ev) == TRUE,
        "a reconcile newer than the applied revision applies", "");
    ev.revision = 1;
    TEST_ASSERT(is_hsi_etcd_revision_newest(0, &ev) == TRUE,
        "any reconcile applies when no etcd event ever touched the subscriber", "");

    /* A removal records its own revision, so a reconcile that was queued before
     * the delete cannot bring the subscriber back. */
    ev.from_reconcile = TRUE;
    ev.action = HSI_ACTION_UPDATE;
    ev.revision = 40;
    TEST_ASSERT(is_hsi_etcd_revision_newest(50, &ev) == FALSE,
        "a reconcile older than the delete does not re-create the subscriber", "");
    ev.revision = 60;
    TEST_ASSERT(is_hsi_etcd_revision_newest(50, &ev) == TRUE,
        "a reconcile newer than the delete re-creates the subscriber", "");
    ev.from_reconcile = FALSE;
    ev.revision = 40;
    TEST_ASSERT(is_hsi_etcd_revision_newest(50, &ev) == TRUE,
        "a live event after the delete applies whatever its revision", "");

    TEST_ASSERT(is_hsi_etcd_revision_newest(0, NULL) == FALSE, "a NULL event is rejected", "");
}

static void test_is_sweep_may_remove(void)
{
    TEST_ASSERT(is_sweep_may_remove(90, 100) == TRUE,
        "a subscriber older than the reconcile revision is removable", "");
    TEST_ASSERT(is_sweep_may_remove(100, 100) == TRUE,
        "a subscriber at the reconcile revision is removable", "");
    TEST_ASSERT(is_sweep_may_remove(101, 100) == FALSE,
        "a subscriber configured after the reconcile read etcd is kept", "");
    TEST_ASSERT(is_sweep_may_remove(0, 100) == TRUE,
        "a subscriber no etcd event ever touched stays removable", "");
    TEST_ASSERT(is_sweep_may_remove(90, 0) == TRUE,
        "a sweep with no reconcile revision removes as before", "");
}

static void test_etcd_build_present_ccb_ids(void)
{
    FastRG_t *fastrg_ccb = calloc(1, sizeof(*fastrg_ccb));
    const char *ids[] = {"1", "3", "4", "5", "x", "0"};
    int present[6];

    assert(fastrg_ccb != NULL);
    fastrg_ccb->user_count = 2;
    fastrg_ccb->max_user_count = 4;

    int count = etcd_build_present_ccb_ids(fastrg_ccb, ids, 6, present);
    TEST_ASSERT(count == 3 && present[0] == 0 && present[1] == 2 && present[2] == 3,
        "the present list keeps every id below capacity, also those beyond the count",
        "got %d entries", count);
    TEST_ASSERT(etcd_build_present_ccb_ids(fastrg_ccb, ids, 0, present) == 0,
        "no etcd keys give an empty present list", "");
    TEST_ASSERT(etcd_build_present_ccb_ids(NULL, ids, 6, present) == 0,
        "a NULL context gives an empty present list", "");

    free(fastrg_ccb);
}

static void test_etcd_plan_dns_reconcile(void)
{
    const char *ids[] = {"1", "3", "5", "x"};
    const BOOL parsed[] = {TRUE, FALSE, TRUE, TRUE};
    etcd_dns_key_action_t actions[4];
    BOOL clear[4];

    /* clear[] is one entry larger than the snapshots below; 0x5a marks untouched. */
    memset(clear, 0x5a, sizeof(clear));
    etcd_plan_dns_reconcile(ids, parsed, 4, 3, actions, clear);
    TEST_ASSERT(actions[0] == ETCD_DNS_KEY_SET && actions[2] == ETCD_DNS_KEY_SKIP &&
        actions[3] == ETCD_DNS_KEY_SKIP,
        "keys within the count snapshot are sent, the others skipped", "");
    TEST_ASSERT(actions[1] == ETCD_DNS_KEY_KEEP && clear[2] == FALSE,
        "an unreadable key keeps its user's records instead of clearing them", "");
    TEST_ASSERT(clear[0] == FALSE && clear[1] == TRUE,
        "only a user etcd has no key for is cleared", "");
    TEST_ASSERT(clear[3] == 0x5a,
        "nothing past the count snapshot is cleared", "");

    memset(clear, 0x5a, sizeof(clear));
    etcd_plan_dns_reconcile(ids, parsed, 4, 2, actions, clear);
    TEST_ASSERT(actions[0] == ETCD_DNS_KEY_SET && actions[1] == ETCD_DNS_KEY_SKIP &&
        clear[0] == FALSE && clear[1] == TRUE && clear[2] == 0x5a,
        "one count snapshot bounds both the keys and the clears", "");

    memset(clear, 0x5a, sizeof(clear));
    etcd_plan_dns_reconcile(ids, parsed, 4, 0, actions, clear);
    TEST_ASSERT(actions[0] == ETCD_DNS_KEY_SKIP && clear[0] == 0x5a,
        "a zero count snapshot sends and clears nothing", "");
}

static void test_etcd_is_reconcile_hsi_outside_count(void)
{
    FastRG_t *fastrg_ccb = calloc(1, sizeof(*fastrg_ccb));
    etcd_event_t ev;

    assert(fastrg_ccb != NULL);
    fastrg_ccb->user_count = 2;
    fastrg_ccb->max_user_count = 4;
    memset(&ev, 0, sizeof(ev));
    ev.kind = ETCD_EVENT_HSI;
    ev.action = HSI_ACTION_UPDATE;

    ev.from_reconcile = TRUE;
    strncpy(ev.user_id, "3", sizeof(ev.user_id) - 1);
    TEST_ASSERT(etcd_is_reconcile_hsi_outside_count(fastrg_ccb, &ev) == TRUE,
        "a reconcile key beyond the subscriber count is outside it", "");
    strncpy(ev.user_id, "2", sizeof(ev.user_id) - 1);
    TEST_ASSERT(etcd_is_reconcile_hsi_outside_count(fastrg_ccb, &ev) == FALSE,
        "a reconcile key within the subscriber count is not", "");

    ev.from_reconcile = FALSE;
    strncpy(ev.user_id, "3", sizeof(ev.user_id) - 1);
    TEST_ASSERT(etcd_is_reconcile_hsi_outside_count(fastrg_ccb, &ev) == FALSE,
        "a live event beyond the subscriber count is not a reconcile residue", "");

    TEST_ASSERT(etcd_is_reconcile_hsi_outside_count(fastrg_ccb, NULL) == FALSE,
        "a NULL event is rejected", "");

    free(fastrg_ccb);
}

void test_etcd_integration(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;
    test_count = 0;
    pass_count = 0;

    test_parse_user_id_contract();
    test_reconcile_sweep();
    test_etcd_event_dispatch();
    test_is_hsi_etcd_revision_newest();
    test_is_sweep_may_remove();
    test_etcd_build_present_ccb_ids();
    test_etcd_plan_dns_reconcile();
    test_etcd_is_reconcile_hsi_outside_count();

    *total_tests += test_count;
    *total_pass += pass_count;
}
