#include <stdlib.h>
#include <string.h>

#include <rte_lcore.h>
#include <rte_rcu_qsbr.h>

#include "../src/fastrg.h"
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

void test_metrics(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    size_t rcu_size = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    struct rte_rcu_qsbr *stats_rcu = NULL;
    unsigned int lcore_id = rte_get_main_lcore();
    char *saved_uuid = fastrg_ccb->node_uuid;
    struct rte_rcu_qsbr *saved_stats_rcu = fastrg_ccb->per_subscriber_stats_rcu;
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

    TEST_ASSERT(posix_memalign((void **)&stats_rcu, RTE_CACHE_LINE_SIZE, rcu_size) == 0,
        "allocate subscriber stats RCU fixture", "allocation failed");
    memset(stats_rcu, 0, rcu_size);
    TEST_ASSERT(rte_rcu_qsbr_init(stats_rcu, RTE_MAX_LCORE) == 0,
        "initialize subscriber stats RCU fixture", "initialization failed");
    fastrg_ccb->per_subscriber_stats_rcu = stats_rcu;
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

    metrics_rcu_register(fastrg_ccb);
    test_contract_and_families(fastrg_ccb);
    test_uuid_escaping(fastrg_ccb);
    test_pppoe_phase_tallies(fastrg_ccb);
    test_per_user_stats(fastrg_ccb);

    fastrg_ccb->node_uuid = saved_uuid;
    free(fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT]);
    free(fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT]);
    free(fastrg_ccb->pppoes_stats[lcore_id]);
    fastrg_ccb->per_subscriber_stats[lcore_id][LAN_PORT] = saved_lan_stats;
    fastrg_ccb->per_subscriber_stats[lcore_id][WAN_PORT] = saved_wan_stats;
    fastrg_ccb->pppoes_stats[lcore_id] = saved_pppoes_stats;
    fastrg_ccb->per_subscriber_stats_rcu = saved_stats_rcu;
    fastrg_ccb->lcore_usage = saved_lcore_usage;
    fastrg_ccb->user_count = saved_user_count;
    fastrg_ccb->ppp_ccb = saved_ppp_ccb;
    fastrg_ccb->dhcp_ccb = saved_dhcp_ccb;
    free(stats_rcu);
    free(test_ppp_ccb);
    free(test_dhcp_ccb);

    printf("\nmetrics tests: %d passed, %d failed\n", pass_count, test_count - pass_count);
    *total_tests += test_count;
    *total_pass += pass_count;
}
