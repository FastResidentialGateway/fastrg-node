#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <rte_lcore.h>
#include <rte_rcu_qsbr.h>

#include "../src/fastrg.h"
#include "../src/dp.h"
#include "../src/dhcpd/dhcpd.h"
#include "../src/metrics.h"
#include "../src/pppd/header.h"
#include "../src/pppd/pppd.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

static const char *metric_families[] = {
    "fastrg_node_start_time_seconds",
    "fastrg_node_restart_total",
    "fastrg_node_rx_packets_total",
    "fastrg_node_tx_packets_total",
    "fastrg_node_rx_bytes_total",
    "fastrg_node_tx_bytes_total",
    "fastrg_node_rx_errors_total",
    "fastrg_node_tx_errors_total",
    "fastrg_node_rx_dropped_total",
    "fastrg_node_per_user_rx_packets_total",
    "fastrg_node_per_user_rx_bytes_total",
    "fastrg_node_per_user_tx_packets_total",
    "fastrg_node_per_user_tx_bytes_total",
    "fastrg_node_per_user_dropped_packets_total",
    "fastrg_node_per_user_dropped_bytes_total",
    "fastrg_node_unknown_user_rx_packets_total",
    "fastrg_node_unknown_user_rx_bytes_total",
    "fastrg_node_unknown_user_tx_packets_total",
    "fastrg_node_unknown_user_tx_bytes_total",
    "fastrg_node_unknown_user_dropped_packets_total",
    "fastrg_node_unknown_user_dropped_bytes_total",
    "fastrg_node_total_pppoe_data_sessions",
    "fastrg_node_total_pppoe_ipcp_sessions",
    "fastrg_node_total_pppoe_auth_sessions",
    "fastrg_node_total_pppoe_lcp_sessions",
    "fastrg_node_total_pppoe_init_sessions",
    "fastrg_node_total_pppoe_terminated_sessions",
    "fastrg_node_total_pppoe_not_configured_sessions",
    "fastrg_node_total_pppoe_error_sessions",
    "fastrg_node_per_pppoe_session_rx_packets_total",
    "fastrg_node_per_pppoe_session_rx_bytes_total",
    "fastrg_node_per_pppoe_session_tx_packets_total",
    "fastrg_node_per_pppoe_session_tx_bytes_total",
    "fastrg_node_per_user_nat_entries_used",
    "fastrg_node_per_user_nat_alloc_fail_total",
    "fastrg_node_per_user_nat_gc_reclaimed_total",
    "fastrg_node_per_user_dhcp_cur_lease_count",
    "fastrg_node_per_user_dhcp_max_lease_count",
    "fastrg_node_total_running_dhcp_server",
    "fastrg_node_total_stopped_dhcp_server",
    "fastrg_node_total_not_configured_dhcp_server",
    "fastrg_node_lcore_busy_cycles_total",
    "fastrg_node_lcore_total_cycles_total",
    "fastrg_node_lcore_rx_packets_total",
    "fastrg_node_lcore_tx_packets_total",
    "fastrg_node_heap_total_bytes",
    "fastrg_node_heap_used_bytes",
    "fastrg_node_heap_free_bytes",
    "fastrg_node_heap_largest_free_block_bytes",
    "fastrg_node_mempool_size",
    "fastrg_node_mempool_avail_count",
    "fastrg_node_mempool_in_use_count",
    "fastrg_node_hugepage_pinned_bytes",
    "fastrg_nic_link_up",
    "fastrg_nic_link_speed_mbps",
    "fastrg_nic_link_flaps_total",
    "fastrg_nic_info",
};

static int count_type_lines(const char *text, const char *family)
{
    char expected[160];
    const char *line = text;
    int count = 0;

    snprintf(expected, sizeof(expected), "# TYPE %s ", family);
    while (line != NULL && *line != '\0') {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len >= strlen(expected) && strncmp(line, expected, strlen(expected)) == 0)
            count++;
        line = end ? end + 1 : NULL;
    }
    return count;
}

static void test_contract_and_families(FastRG_t *fastrg_ccb)
{
    lighthttp_buf_t out = {0};
    const char *content_type = NULL;
    char *saved_uuid = fastrg_ccb->node_uuid;
    char uuid[] = "metrics-test-node";

    fastrg_ccb->node_uuid = uuid;
    int status = metrics_build(&out, &content_type, fastrg_ccb);

    TEST_ASSERT(status == 200, "metrics_build returns HTTP 200", "status=%d", status);
    TEST_ASSERT(content_type != NULL &&
        strcmp(content_type, "text/plain; version=0.0.4; charset=utf-8") == 0,
        "Prometheus content type", "content_type=%s", content_type ? content_type : "(null)");
    TEST_ASSERT(out.data != NULL && out.len > 0, "metrics exposition is non-empty", "len=%zu", out.len);
    TEST_ASSERT(strstr(out.data,
        "fastrg_node_start_time_seconds{node_uuid=\"metrics-test-node\"}") != NULL,
        "node UUID label is emitted", "missing known node_uuid label");

    for (size_t i = 0; i < sizeof(metric_families) / sizeof(metric_families[0]); i++) {
        int count = count_type_lines(out.data, metric_families[i]);
        TEST_ASSERT(count == 1, "metric family has exactly one TYPE line",
            "family=%s count=%d", metric_families[i], count);
    }

    fastrg_ccb->node_uuid = saved_uuid;
    lighthttp_buf_free(&out);
}

static void test_uuid_escaping(FastRG_t *fastrg_ccb)
{
    lighthttp_buf_t out = {0};
    const char *content_type = NULL;
    char *saved_uuid = fastrg_ccb->node_uuid;
    char uuid[] = "quote\"slash\\line\nend";

    fastrg_ccb->node_uuid = uuid;
    metrics_build(&out, &content_type, fastrg_ccb);
    TEST_ASSERT(strstr(out.data, "node_uuid=\"quote\\\"slash\\\\line\\nend\"") != NULL,
        "node UUID label escapes quote, backslash, and newline", "escaped label missing");

    fastrg_ccb->node_uuid = saved_uuid;
    lighthttp_buf_free(&out);
}

static void assert_phase_value(FastRG_t *fastrg_ccb, const char *family, uint64_t expected)
{
    lighthttp_buf_t out = {0};
    const char *content_type = NULL;
    char sample[192];

    metrics_build(&out, &content_type, fastrg_ccb);
    snprintf(sample, sizeof(sample), "%s{node_uuid=\"\"} %lu\n", family, (unsigned long)expected);
    TEST_ASSERT(strstr(out.data, sample) != NULL, "PPPoE phase gauge has expected value",
        "sample=%s", sample);
    lighthttp_buf_free(&out);
}

static void test_pppoe_phase_tallies(FastRG_t *fastrg_ccb)
{
    ppp_ccb_t *saved_ccb = fastrg_ccb->ppp_ccb[0];
    int saved_phase = saved_ccb->phase;
    char *saved_uuid = fastrg_ccb->node_uuid;

    fastrg_ccb->node_uuid = NULL;
    saved_ccb->phase = DATA_PHASE;
    assert_phase_value(fastrg_ccb, "fastrg_node_total_pppoe_data_sessions", 1);

    saved_ccb->phase = IPCP_PHASE;
    assert_phase_value(fastrg_ccb, "fastrg_node_total_pppoe_ipcp_sessions", 1);

    fastrg_ccb->ppp_ccb[0] = NULL;
    assert_phase_value(fastrg_ccb, "fastrg_node_total_pppoe_not_configured_sessions", 1);

    fastrg_ccb->ppp_ccb[0] = saved_ccb;
    saved_ccb->phase = saved_phase;
    fastrg_ccb->node_uuid = saved_uuid;
}

static void test_snapshot_persist_gauge(FastRG_t *fastrg_ccb)
{
    lighthttp_buf_t out = {0};
    const char *content_type = NULL;
    char *saved_uuid = fastrg_ccb->node_uuid;

    /* With no snapshot ever persisted (and no failure recorded),
     * config_snapshot_persist_ok() reports TRUE, so the gauge must read 1. */
    fastrg_ccb->node_uuid = NULL;
    metrics_build(&out, &content_type, fastrg_ccb);
    TEST_ASSERT(strstr(out.data, "# TYPE fastrg_node_snapshot_persist_ok gauge\n") != NULL,
        "snapshot persist gauge family is declared", "missing TYPE line");
    TEST_ASSERT(strstr(out.data, "fastrg_node_snapshot_persist_ok{node_uuid=\"\"} 1\n") != NULL,
        "snapshot persist gauge reads 1 when persisting never failed", "sample missing or not 1");

    fastrg_ccb->node_uuid = saved_uuid;
    lighthttp_buf_free(&out);
}

static void test_max_user_count_gauge(FastRG_t *fastrg_ccb)
{
    lighthttp_buf_t out = {0};
    const char *content_type = NULL;
    char *saved_uuid = fastrg_ccb->node_uuid;
    U16 saved_max_user_count = fastrg_ccb->max_user_count;

    fastrg_ccb->node_uuid = NULL;
    fastrg_ccb->max_user_count = 37;
    metrics_build(&out, &content_type, fastrg_ccb);
    TEST_ASSERT(strstr(out.data, "# TYPE fastrg_node_max_user_count gauge\n") != NULL,
        "max user count gauge family is declared", "missing TYPE line");
    TEST_ASSERT(strstr(out.data, "fastrg_node_max_user_count{node_uuid=\"\"} 37\n") != NULL,
        "max user count gauge exposes effective capacity", "sample missing or not 37");

    fastrg_ccb->max_user_count = saved_max_user_count;
    fastrg_ccb->node_uuid = saved_uuid;
    lighthttp_buf_free(&out);
}

static void test_subscriber_cost_gauge(FastRG_t *fastrg_ccb)
{
    lighthttp_buf_t out = {0};
    const char *content_type = NULL;
    char *saved_uuid = fastrg_ccb->node_uuid;
    U64 saved_cost = fastrg_ccb->subscriber_cost_bytes;

    fastrg_ccb->node_uuid = NULL;
    fastrg_ccb->subscriber_cost_bytes = 123456789;
    metrics_build(&out, &content_type, fastrg_ccb);
    TEST_ASSERT(strstr(out.data, "# TYPE fastrg_node_subscriber_cost_bytes gauge\n") != NULL,
        "subscriber cost gauge family is declared", "missing TYPE line");
    TEST_ASSERT(strstr(out.data, "fastrg_node_subscriber_cost_bytes{node_uuid=\"\"} 123456789\n") != NULL,
        "subscriber cost gauge exposes the measured cost", "sample missing or wrong value");

    fastrg_ccb->subscriber_cost_bytes = saved_cost;
    fastrg_ccb->node_uuid = saved_uuid;
    lighthttp_buf_free(&out);
}

static void test_per_user_stats(FastRG_t *fastrg_ccb)
{
    unsigned int lcore_id = rte_get_main_lcore();
    struct per_ccb_stats *stats = fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT];
    lighthttp_buf_t out = {0};
    const char *content_type = NULL;
    char *saved_uuid = fastrg_ccb->node_uuid;

    fastrg_ccb->node_uuid = NULL;
    stats[0].rx_packets = 101;
    stats[0].rx_bytes = 202;
    stats[0].tx_packets = 303;
    stats[0].tx_bytes = 404;
    stats[0].dropped_packets = 505;
    stats[0].dropped_bytes = 606;

    metrics_build(&out, &content_type, fastrg_ccb);
    TEST_ASSERT(strstr(out.data,
        "fastrg_node_per_user_rx_packets_total{node_uuid=\"\",nic_index=\"1\",user_id=\"1\"} 101\n") != NULL,
        "per-user RX packets are summed", "expected WAN user 1 RX sample");
    TEST_ASSERT(strstr(out.data,
        "fastrg_node_per_user_rx_bytes_total{node_uuid=\"\",nic_index=\"1\",user_id=\"1\"} 202\n") != NULL,
        "per-user RX bytes are summed", "expected WAN user 1 RX bytes sample");
    TEST_ASSERT(strstr(out.data,
        "fastrg_node_per_user_tx_packets_total{node_uuid=\"\",nic_index=\"1\",user_id=\"1\"} 303\n") != NULL,
        "per-user TX packets are summed", "expected WAN user 1 TX sample");
    TEST_ASSERT(strstr(out.data,
        "fastrg_node_per_user_tx_bytes_total{node_uuid=\"\",nic_index=\"1\",user_id=\"1\"} 404\n") != NULL,
        "per-user TX bytes are summed", "expected WAN user 1 TX bytes sample");
    TEST_ASSERT(strstr(out.data,
        "fastrg_node_per_user_dropped_packets_total{node_uuid=\"\",nic_index=\"1\",user_id=\"1\"} 505\n") != NULL,
        "per-user dropped packets are summed", "expected WAN user 1 drop sample");
    TEST_ASSERT(strstr(out.data,
        "fastrg_node_per_user_dropped_bytes_total{node_uuid=\"\",nic_index=\"1\",user_id=\"1\"} 606\n") != NULL,
        "per-user dropped bytes are summed", "expected WAN user 1 drop bytes sample");

    memset(&stats[0], 0, sizeof(stats[0]));
    fastrg_ccb->node_uuid = saved_uuid;
    lighthttp_buf_free(&out);
}

/* The reader sums one TX queue across lcores, and every queue the port has is
 * published so a quiet one reads 0 instead of vanishing. */
/* Start-up sizes the per-lcore rows from the queues a port really got; this
 * fixture stands in for that, and puts counts on two lcores so a sum has
 * something to add. */
struct tx_queue_fixture {
    unsigned int lcore_id;
    unsigned int other;
    U16          queue_count;
    U16          saved_count[PORT_AMOUNT];
    struct tx_queue_stats *saved_rows[PORT_AMOUNT];
    struct tx_queue_stats *saved_other;
    char        *saved_uuid;
    uint64_t     want_full;
    uint64_t     want_short;
};

static BOOL tx_queue_fixture_install(FastRG_t *fastrg_ccb, struct tx_queue_fixture *fx)
{
    unsigned int l;

    fx->lcore_id = rte_get_main_lcore();
    fx->other = LCORE_ID_ANY;
    fx->queue_count = 4;
    fx->saved_other = NULL;
    fx->want_full = 7;
    fx->want_short = 3;

    /* A second enabled lcore is what makes the sum a sum. Where the test host
     * offers only one, the single-lcore reading is still checked. */
    RTE_LCORE_FOREACH(l) {
        if (l != fx->lcore_id) {
            fx->other = l;
            break;
        }
    }

    for(int p=0; p<PORT_AMOUNT; p++) {
        fx->saved_count[p] = fastrg_ccb->tx_queue_count[p];
        fx->saved_rows[p] = fastrg_ccb->tx_queue_stats[fx->lcore_id][p];
        fastrg_ccb->tx_queue_count[p] = fx->queue_count;
        fastrg_ccb->tx_queue_stats[fx->lcore_id][p] =
            calloc(fx->queue_count, sizeof(struct tx_queue_stats));
    }
    if (fx->other != LCORE_ID_ANY) {
        fx->saved_other = fastrg_ccb->tx_queue_stats[fx->other][WAN_PORT];
        fastrg_ccb->tx_queue_stats[fx->other][WAN_PORT] =
            calloc(fx->queue_count, sizeof(struct tx_queue_stats));
    }
    if (fastrg_ccb->tx_queue_stats[fx->lcore_id][WAN_PORT] == NULL)
        return FALSE;

    fx->saved_uuid = fastrg_ccb->node_uuid;
    fastrg_ccb->node_uuid = NULL;
    fastrg_ccb->tx_queue_stats[fx->lcore_id][WAN_PORT][2].full_packets = 7;
    fastrg_ccb->tx_queue_stats[fx->lcore_id][WAN_PORT][2].short_bursts = 3;
    if (fx->other != LCORE_ID_ANY && fastrg_ccb->tx_queue_stats[fx->other][WAN_PORT] != NULL) {
        fastrg_ccb->tx_queue_stats[fx->other][WAN_PORT][2].full_packets = 5;
        fastrg_ccb->tx_queue_stats[fx->other][WAN_PORT][2].short_bursts = 1;
        fx->want_full = 12;
        fx->want_short = 4;
    }
    return TRUE;
}

static void tx_queue_fixture_restore(FastRG_t *fastrg_ccb, struct tx_queue_fixture *fx)
{
    for(int p=0; p<PORT_AMOUNT; p++) {
        free(fastrg_ccb->tx_queue_stats[fx->lcore_id][p]);
        fastrg_ccb->tx_queue_stats[fx->lcore_id][p] = fx->saved_rows[p];
        fastrg_ccb->tx_queue_count[p] = fx->saved_count[p];
    }
    if (fx->other != LCORE_ID_ANY) {
        free(fastrg_ccb->tx_queue_stats[fx->other][WAN_PORT]);
        fastrg_ccb->tx_queue_stats[fx->other][WAN_PORT] = fx->saved_other;
    }
    fastrg_ccb->node_uuid = fx->saved_uuid;
}

static void test_fastrg_sum_tx_queue_stats(FastRG_t *fastrg_ccb)
{
    struct tx_queue_fixture fx;
    struct tx_queue_stats sum;

    TEST_ASSERT(tx_queue_fixture_install(fastrg_ccb, &fx) == TRUE,
        "TX queue counter fixture", "calloc failed");
    if (fastrg_ccb->tx_queue_stats[fx.lcore_id][WAN_PORT] == NULL)
        return;

    fastrg_sum_tx_queue_stats(fastrg_ccb, WAN_PORT, 2, &sum);
    TEST_ASSERT(sum.full_packets == fx.want_full,
        "TX queue refused packets are summed across lcores",
        "expected %" PRIu64 ", got %" PRIu64, fx.want_full, sum.full_packets);
    TEST_ASSERT(sum.short_bursts == fx.want_short,
        "TX queue short bursts are summed across lcores",
        "expected %" PRIu64 ", got %" PRIu64, fx.want_short, sum.short_bursts);

    fastrg_sum_tx_queue_stats(fastrg_ccb, WAN_PORT, 3, &sum);
    TEST_ASSERT(sum.full_packets == 0 && sum.short_bursts == 0,
        "a queue that never dropped reads as zero",
        "got full=%" PRIu64 " short=%" PRIu64, sum.full_packets, sum.short_bursts);

    fastrg_sum_tx_queue_stats(fastrg_ccb, LAN_PORT, 2, &sum);
    TEST_ASSERT(sum.full_packets == 0 && sum.short_bursts == 0,
        "the other port keeps its own counters",
        "got full=%" PRIu64 " short=%" PRIu64, sum.full_packets, sum.short_bursts);

    /* One past the queues this port has: the read must stop rather than run
     * off the end of the row. */
    sum.full_packets = 99; sum.short_bursts = 99; sum.handoff_dropped = 99;
    fastrg_sum_tx_queue_stats(fastrg_ccb, WAN_PORT, fx.queue_count, &sum);
    TEST_ASSERT(sum.full_packets == 0 && sum.short_bursts == 0 && sum.handoff_dropped == 0,
        "a queue the port does not have reads as zero",
        "got full=%" PRIu64 " short=%" PRIu64 " handoff=%" PRIu64,
        sum.full_packets, sum.short_bursts, sum.handoff_dropped);

    tx_queue_fixture_restore(fastrg_ccb, &fx);
}

static void test_metrics_build(FastRG_t *fastrg_ccb)
{
    struct tx_queue_fixture fx;
    lighthttp_buf_t out = {0};
    const char *content_type = NULL;
    char sample[192];

    TEST_ASSERT(tx_queue_fixture_install(fastrg_ccb, &fx) == TRUE,
        "TX queue counter fixture", "calloc failed");
    if (fastrg_ccb->tx_queue_stats[fx.lcore_id][WAN_PORT] == NULL)
        return;

    metrics_build(&out, &content_type, fastrg_ccb);
    TEST_ASSERT(strstr(out.data, "# TYPE fastrg_node_tx_queue_full_total gauge\n") != NULL,
        "TX queue full family is declared", "missing TYPE line");
    snprintf(sample, sizeof(sample),
        "fastrg_node_tx_queue_full_total{node_uuid=\"\",nic_index=\"%d\",queue=\"2\"} %" PRIu64 "\n",
        WAN_PORT, fx.want_full);
    TEST_ASSERT(strstr(out.data, sample) != NULL,
        "TX queue refused packets are summed across lcores", "missing sample=%s", sample);
    snprintf(sample, sizeof(sample),
        "fastrg_node_tx_queue_burst_short_total{node_uuid=\"\",nic_index=\"%d\",queue=\"2\"} %" PRIu64 "\n",
        WAN_PORT, fx.want_short);
    TEST_ASSERT(strstr(out.data, sample) != NULL,
        "TX queue short bursts are summed across lcores", "missing sample=%s", sample);
    snprintf(sample, sizeof(sample),
        "fastrg_node_tx_queue_full_total{node_uuid=\"\",nic_index=\"%d\",queue=\"3\"} 0\n", WAN_PORT);
    TEST_ASSERT(strstr(out.data, sample) != NULL,
        "a queue that never dropped is published as zero", "missing sample=%s", sample);
    snprintf(sample, sizeof(sample),
        "fastrg_node_tx_queue_full_total{node_uuid=\"\",nic_index=\"%d\",queue=\"2\"} 0\n", LAN_PORT);
    TEST_ASSERT(strstr(out.data, sample) != NULL,
        "the other port keeps its own counters", "missing sample=%s", sample);
    snprintf(sample, sizeof(sample),
        "fastrg_node_tx_queue_full_total{node_uuid=\"\",nic_index=\"%d\",queue=\"%u\"}",
        WAN_PORT, fx.queue_count);
    TEST_ASSERT(strstr(out.data, sample) == NULL,
        "queues the port does not have are not published", "found sample=%s", sample);

    tx_queue_fixture_restore(fastrg_ccb, &fx);
    lighthttp_buf_free(&out);
}

static void test_lcore_traffic_stats(FastRG_t *fastrg_ccb)
{
    unsigned int lcore_id = rte_get_main_lcore();
    unsigned int other1 = lcore_id + 1, other2 = lcore_id + 2;
    struct per_ccb_stats *stats = fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT];
    struct lcore_usage_counter *usage = calloc(RTE_MAX_LCORE, sizeof(*usage));
    struct lcore_map saved_lcore = fastrg_ccb->lcore;
    struct lcore_usage_counter *saved_usage = fastrg_ccb->lcore_usage;
    struct per_ccb_stats *saved_other1[PORT_AMOUNT], *saved_other2[PORT_AMOUNT];
    char *saved_uuid = fastrg_ccb->node_uuid;
    lighthttp_buf_t out = {0};
    const char *content_type = NULL;
    char sample[192];

    TEST_ASSERT(usage != NULL, "allocate lcore_usage fixture", "calloc failed");
    if (usage == NULL)
        return;

    /* Three distinct fixed-thread ids; the two extra lcores have no stats
     * rows, exercising the NULL-row path. */
    for(int p=0; p<PORT_AMOUNT; p++) {
        saved_other1[p] = fastrg_ccb->per_subscriber_stats[other1][p];
        saved_other2[p] = fastrg_ccb->per_subscriber_stats[other2][p];
        fastrg_ccb->per_subscriber_stats[other1][p] = NULL;
        fastrg_ccb->per_subscriber_stats[other2][p] = NULL;
    }
    fastrg_ccb->node_uuid = NULL;
    usage[lcore_id].role = "ctrl";
    fastrg_ccb->lcore_usage = usage;
    fastrg_ccb->lcore.ctrl_thread = (U8)lcore_id;
    fastrg_ccb->lcore.wan_ctrl_thread = (U8)other1;
    fastrg_ccb->lcore.lan_ctrl_thread = (U8)other2;
    fastrg_ccb->lcore.num_data_queues = 0;

    /* user slot + unknown-user slot (index max_user_count == 1) must both
     * be part of the per-lcore aggregation. */
    stats[0].rx_packets = 7;
    stats[1].rx_packets = 5;
    stats[0].tx_packets = 11;
    stats[1].tx_packets = 3;

    metrics_build(&out, &content_type, fastrg_ccb);

    snprintf(sample, sizeof(sample),
        "fastrg_node_lcore_rx_packets_total{node_uuid=\"\",lcore_id=\"%u\",role=\"ctrl\",nic_index=\"%d\"} 12\n",
        lcore_id, WAN_PORT);
    TEST_ASSERT(strstr(out.data, sample) != NULL,
        "per-lcore RX sums subscribers including the unknown-user slot", "missing sample=%s", sample);
    snprintf(sample, sizeof(sample),
        "fastrg_node_lcore_tx_packets_total{node_uuid=\"\",lcore_id=\"%u\",role=\"ctrl\",nic_index=\"%d\"} 14\n",
        lcore_id, WAN_PORT);
    TEST_ASSERT(strstr(out.data, sample) != NULL,
        "per-lcore TX sums subscribers including the unknown-user slot", "missing sample=%s", sample);
    snprintf(sample, sizeof(sample),
        "fastrg_node_lcore_rx_packets_total{node_uuid=\"\",lcore_id=\"%u\",role=\"ctrl\",nic_index=\"%d\"} 0\n",
        lcore_id, LAN_PORT);
    TEST_ASSERT(strstr(out.data, sample) != NULL,
        "per-lcore RX emits a zero row for the idle port", "missing sample=%s", sample);
    snprintf(sample, sizeof(sample),
        "fastrg_node_lcore_rx_packets_total{node_uuid=\"\",lcore_id=\"%u\",role=\"\",nic_index=\"%d\"} 0\n",
        other1, WAN_PORT);
    TEST_ASSERT(strstr(out.data, sample) != NULL,
        "per-lcore RX emits a zero row for a lcore without stats rows", "missing sample=%s", sample);

    memset(&stats[0], 0, 2 * sizeof(stats[0]));
    for(int p=0; p<PORT_AMOUNT; p++) {
        fastrg_ccb->per_subscriber_stats[other1][p] = saved_other1[p];
        fastrg_ccb->per_subscriber_stats[other2][p] = saved_other2[p];
    }
    fastrg_ccb->lcore = saved_lcore;
    fastrg_ccb->lcore_usage = saved_usage;
    fastrg_ccb->node_uuid = saved_uuid;
    free(usage);
    lighthttp_buf_free(&out);
}

void test_metrics(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    unsigned int lcore_id = rte_get_main_lcore();
    char *saved_uuid = fastrg_ccb->node_uuid;
    struct per_ccb_stats *saved_lan_stats = fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT];
    struct per_ccb_stats *saved_wan_stats = fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT];
    struct pppoes_lcore_stats *saved_pppoes_stats = fastrg_ccb->pppoes_stats[lcore_id];
    struct lcore_usage_counter *saved_lcore_usage = fastrg_ccb->lcore_usage;
    U16 saved_user_count = fastrg_ccb->user_count;
    void **saved_ppp_ccb = fastrg_ccb->ppp_ccb;
    void **saved_dhcp_ccb = fastrg_ccb->dhcp_ccb;
    ppp_ccb_t *test_ppp_ccb = calloc(1, sizeof(*test_ppp_ccb));
    dhcp_ccb_t *test_dhcp_ccb = calloc(1, sizeof(*test_dhcp_ccb));
    void *test_ppp_array[] = {test_ppp_ccb};
    void *test_dhcp_array[] = {test_dhcp_ccb};

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║                metrics Module Unit Tests                  ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    fastrg_ccb->lcore_usage = NULL;
    fastrg_ccb->user_count = 1;
    fastrg_ccb->ppp_ccb = test_ppp_array;
    fastrg_ccb->dhcp_ccb = test_dhcp_array;
    fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT] =
        calloc(fastrg_ccb->user_count + 1, sizeof(struct per_ccb_stats));
    fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT] =
        calloc(fastrg_ccb->user_count + 1, sizeof(struct per_ccb_stats));
    fastrg_ccb->pppoes_stats[lcore_id] = calloc(fastrg_ccb->user_count, sizeof(struct pppoes_lcore_stats));
    TEST_ASSERT(fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT] != NULL &&
        fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT] != NULL &&
        fastrg_ccb->pppoes_stats[lcore_id] != NULL && test_ppp_ccb != NULL && test_dhcp_ccb != NULL,
        "allocate metrics stats rows", "allocation failed");

    test_contract_and_families(fastrg_ccb);
    test_uuid_escaping(fastrg_ccb);
    test_pppoe_phase_tallies(fastrg_ccb);
    test_per_user_stats(fastrg_ccb);
    test_snapshot_persist_gauge(fastrg_ccb);
    test_max_user_count_gauge(fastrg_ccb);
    test_subscriber_cost_gauge(fastrg_ccb);
    test_fastrg_sum_tx_queue_stats(fastrg_ccb);
    test_metrics_build(fastrg_ccb);
    test_lcore_traffic_stats(fastrg_ccb);

    fastrg_ccb->node_uuid = saved_uuid;
    free(fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT]);
    free(fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT]);
    free(fastrg_ccb->pppoes_stats[lcore_id]);
    fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT] = saved_lan_stats;
    fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT] = saved_wan_stats;
    fastrg_ccb->pppoes_stats[lcore_id] = saved_pppoes_stats;
    fastrg_ccb->lcore_usage = saved_lcore_usage;
    fastrg_ccb->user_count = saved_user_count;
    fastrg_ccb->ppp_ccb = saved_ppp_ccb;
    fastrg_ccb->dhcp_ccb = saved_dhcp_ccb;
    free(test_ppp_ccb);
    free(test_dhcp_ccb);

    printf("\nmetrics tests: %d passed, %d failed\n", pass_count, test_count - pass_count);
    *total_tests += test_count;
    *total_pass += pass_count;
}
