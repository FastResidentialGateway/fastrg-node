#include <stdlib.h>
#include <string.h>

#include <common.h>

#include <rte_lcore.h>
#include <rte_ring.h>

#include <arpa/inet.h>

#include "../src/cli_request.h"
#include "../src/dnsd/dns_codec.h"
#include "../src/fastrg.h"
#include "../src/dhcpd/dhcpd.h"
#include "../src/pppd/pppd.h"
#include "test_helper.h"

// Global test counters
static int test_count = 0;
static int pass_count = 0;

void test_cli_request_result_pack(void)
{
    printf("\nTesting cli_request_result_pack function:\n");
    printf("=========================================\n\n");

    U32 packed = cli_request_result_pack(1, CLI_REQUEST_OK);
    TEST_ASSERT((packed >> CLI_REQUEST_VERDICT_BITS) == 1,
        "pack(1, OK) unpacks to seq 1", "got seq %u", packed >> CLI_REQUEST_VERDICT_BITS);
    TEST_ASSERT((packed & CLI_REQUEST_VERDICT_MASK) == CLI_REQUEST_OK,
        "pack(1, OK) unpacks to verdict OK", "got verdict %u", packed & CLI_REQUEST_VERDICT_MASK);

    TEST_ASSERT(cli_request_result_pack(0, CLI_REQUEST_NONE) == 0,
        "pack(0, NONE) is the empty slot value", "got %u",
        cli_request_result_pack(0, CLI_REQUEST_NONE));

    U32 max_packed = cli_request_result_pack(CLI_REQUEST_SEQ_MAX, CLI_REQUEST_FAILED);
    TEST_ASSERT((max_packed >> CLI_REQUEST_VERDICT_BITS) == CLI_REQUEST_SEQ_MAX,
        "the largest seq survives the round trip", "got seq %u",
        max_packed >> CLI_REQUEST_VERDICT_BITS);
    TEST_ASSERT((max_packed & CLI_REQUEST_VERDICT_MASK) == CLI_REQUEST_FAILED,
        "the largest seq does not spill into the verdict bits", "got verdict %u",
        max_packed & CLI_REQUEST_VERDICT_MASK);
}

void test_cli_request_publish(FastRG_t *fastrg_ccb)
{
    printf("\nTesting cli_request_publish function:\n");
    printf("=====================================\n\n");

    rte_atomic32_set(&fastrg_ccb->cli_request_result,
        (int32_t)cli_request_result_pack(0, CLI_REQUEST_NONE));

    cli_request_publish(fastrg_ccb, 7, SUCCESS);
    U32 slot = (U32)rte_atomic32_read(&fastrg_ccb->cli_request_result);
    TEST_ASSERT((slot >> CLI_REQUEST_VERDICT_BITS) == 7 &&
        (slot & CLI_REQUEST_VERDICT_MASK) == CLI_REQUEST_OK,
        "SUCCESS publishes OK under the caller's seq", "slot %u", slot);

    cli_request_publish(fastrg_ccb, 8, ERROR);
    slot = (U32)rte_atomic32_read(&fastrg_ccb->cli_request_result);
    TEST_ASSERT((slot >> CLI_REQUEST_VERDICT_BITS) == 8 &&
        (slot & CLI_REQUEST_VERDICT_MASK) == CLI_REQUEST_FAILED,
        "ERROR publishes FAILED under the caller's seq", "slot %u", slot);

    /* seq 0 marks an event nobody waits on, so the slot must not move. */
    cli_request_publish(fastrg_ccb, 0, SUCCESS);
    slot = (U32)rte_atomic32_read(&fastrg_ccb->cli_request_result);
    TEST_ASSERT((slot >> CLI_REQUEST_VERDICT_BITS) == 8 &&
        (slot & CLI_REQUEST_VERDICT_MASK) == CLI_REQUEST_FAILED,
        "seq 0 publishes nothing", "slot %u", slot);

    rte_atomic32_set(&fastrg_ccb->cli_request_result,
        (int32_t)cli_request_result_pack(0, CLI_REQUEST_NONE));
}

void test_cli_request_is_abandoned(FastRG_t *fastrg_ccb)
{
    printf("\nTesting cli_request_is_abandoned function:\n");
    printf("==========================================\n\n");

    rte_atomic32_set(&fastrg_ccb->cli_request_abandoned, 0);

    cli_request_abandon(fastrg_ccb, 7);
    TEST_ASSERT(cli_request_is_abandoned(fastrg_ccb, 7) == TRUE,
        "the abandoned seq reads back as abandoned", "got FALSE");
    /* The mark is consumed, so the same seq must not match twice. */
    TEST_ASSERT(cli_request_is_abandoned(fastrg_ccb, 7) == FALSE,
        "a consumed mark does not match again", "got TRUE");

    cli_request_abandon(fastrg_ccb, 7);
    TEST_ASSERT(cli_request_is_abandoned(fastrg_ccb, 8) == FALSE,
        "another seq does not match the mark", "got TRUE");
    TEST_ASSERT(cli_request_is_abandoned(fastrg_ccb, 7) == TRUE,
        "a mismatch leaves the mark in place", "mark was cleared");

    TEST_ASSERT(cli_request_is_abandoned(fastrg_ccb, 0) == FALSE,
        "seq 0 is never abandoned", "got TRUE");

    cli_request_abandon(fastrg_ccb, 9);
    TEST_ASSERT(cli_request_is_abandoned(fastrg_ccb, 7) == FALSE,
        "an older seq does not match the newest mark", "got TRUE");
    TEST_ASSERT(cli_request_is_abandoned(fastrg_ccb, 9) == TRUE,
        "the newest seq matches its mark", "got FALSE");

    /* seq 0 must not be storable, or it would match every fire-and-forget event. */
    cli_request_abandon(fastrg_ccb, 0);
    TEST_ASSERT(rte_atomic32_read(&fastrg_ccb->cli_request_abandoned) == 0,
        "abandoning seq 0 stores nothing", "slot is %d",
        rte_atomic32_read(&fastrg_ccb->cli_request_abandoned));

    rte_atomic32_set(&fastrg_ccb->cli_request_abandoned, 0);
}

void test_set_dns_proxy_enable(FastRG_t *fastrg_ccb)
{
    printf("\nTesting set_dns_proxy_enable function:\n");
    printf("======================================\n\n");

    dhcp_ccb_t *dhcp_ccb = fastrg_ccb->dhcp_ccb[0];
    BOOL saved = dhcp_ccb->dns_state.dns_proxy_enabled;

    TEST_ASSERT(set_dns_proxy_enable(fastrg_ccb, 0, TRUE) == SUCCESS,
        "enabling the DNS proxy returns SUCCESS", "got ERROR");
    TEST_ASSERT(dhcp_ccb->dns_state.dns_proxy_enabled == TRUE,
        "enabling the DNS proxy sets the flag", "flag is FALSE");

    TEST_ASSERT(set_dns_proxy_enable(fastrg_ccb, 0, FALSE) == SUCCESS,
        "disabling the DNS proxy returns SUCCESS", "got ERROR");
    TEST_ASSERT(dhcp_ccb->dns_state.dns_proxy_enabled == FALSE,
        "disabling the DNS proxy clears the flag", "flag is TRUE");

    TEST_ASSERT(set_dns_proxy_enable(fastrg_ccb, 0, TRUE) == SUCCESS,
        "re-enabling the DNS proxy returns SUCCESS", "got ERROR");
    TEST_ASSERT(dhcp_ccb->dns_state.dns_proxy_enabled == TRUE,
        "re-enabling the DNS proxy sets the flag again", "flag is FALSE");

    dhcp_ccb->dns_state.dns_proxy_enabled = FALSE;
    TEST_ASSERT(set_dns_proxy_enable(fastrg_ccb, fastrg_ccb->user_count, TRUE) == ERROR,
        "an out of range ccb id returns ERROR", "got SUCCESS");
    TEST_ASSERT(dhcp_ccb->dns_state.dns_proxy_enabled == FALSE,
        "an out of range ccb id writes nothing", "flag was changed");

    dhcp_ccb->dns_state.dns_proxy_enabled = saved;
}

void test_set_tcp_conntrack_enable(FastRG_t *fastrg_ccb)
{
    printf("\nTesting set_tcp_conntrack_enable function:\n");
    printf("==========================================\n\n");

    ppp_ccb_t *ppp_ccb = fastrg_ccb->ppp_ccb[0];
    BOOL saved = ppp_ccb->tcp_conntrack_enabled;

    TEST_ASSERT(set_tcp_conntrack_enable(fastrg_ccb, 0, TRUE) == SUCCESS,
        "enabling TCP conntrack returns SUCCESS", "got ERROR");
    TEST_ASSERT(ppp_ccb->tcp_conntrack_enabled == TRUE,
        "enabling TCP conntrack sets the flag", "flag is FALSE");

    TEST_ASSERT(set_tcp_conntrack_enable(fastrg_ccb, 0, FALSE) == SUCCESS,
        "disabling TCP conntrack returns SUCCESS", "got ERROR");
    TEST_ASSERT(ppp_ccb->tcp_conntrack_enabled == FALSE,
        "disabling TCP conntrack clears the flag", "flag is TRUE");

    TEST_ASSERT(set_tcp_conntrack_enable(fastrg_ccb, 0, TRUE) == SUCCESS,
        "re-enabling TCP conntrack returns SUCCESS", "got ERROR");
    TEST_ASSERT(ppp_ccb->tcp_conntrack_enabled == TRUE,
        "re-enabling TCP conntrack sets the flag again", "flag is FALSE");

    ppp_ccb->tcp_conntrack_enabled = FALSE;
    TEST_ASSERT(set_tcp_conntrack_enable(fastrg_ccb, fastrg_ccb->user_count, TRUE) == ERROR,
        "an out of range ccb id returns ERROR", "got SUCCESS");
    TEST_ASSERT(ppp_ccb->tcp_conntrack_enabled == FALSE,
        "an out of range ccb id writes nothing", "flag was changed");

    ppp_ccb->tcp_conntrack_enabled = saved;
}

void test_set_ipv6_enable(FastRG_t *fastrg_ccb)
{
    printf("\nTesting set_ipv6_enable function:\n");
    printf("=================================\n\n");

    /* The redial event goes on cp_q, so this case brings its own rings and
     * hands the CCB back untouched. */
    struct rte_ring *saved_cp_q = fastrg_ccb->cp_q;
    struct rte_ring *saved_free_mail_ring = fastrg_ccb->free_mail_ring;
    struct rte_ring *cp_q = rte_ring_create("nb_test_v6_cp_q", 16, rte_socket_id(), 0);
    struct rte_ring *free_mail_ring = rte_ring_create("nb_test_v6_free_mail", 16, rte_socket_id(), 0);
    tFastRG_MBX mail_slots[4];
    ppp_ccb_t *ppp_ccb = fastrg_ccb->ppp_ccb[0];
    BOOL saved_enabled = ppp_ccb->ipv6_enabled;
    BOOL saved_ipv6cp_up = ppp_ccb->ipv6cp_up;
    BOOL saved_pd_ready = ppp_ccb->dhcp6_pd_ready;
    S16 saved_dp_bool = rte_atomic16_read(&ppp_ccb->ipv6_dp_bool);

    TEST_ASSERT(cp_q != NULL && free_mail_ring != NULL, "create the test mail rings",
        "cp_q %p free_mail_ring %p", (void *)cp_q, (void *)free_mail_ring);
    if (cp_q == NULL || free_mail_ring == NULL)
        goto out;

    memset(mail_slots, 0, sizeof(mail_slots));
    fastrg_ccb->cp_q = cp_q;
    fastrg_ccb->free_mail_ring = free_mail_ring;
    for(U32 i=0; i<RTE_DIM(mail_slots); i++)
        rte_ring_enqueue(free_mail_ring, &mail_slots[i]);

    /* The other two gate inputs are up, so ipv6_dp_bool follows the toggle. */
    ppp_ccb->ipv6_enabled = FALSE;
    ppp_ccb->ipv6cp_up = TRUE;
    ppp_ccb->dhcp6_pd_ready = TRUE;
    rte_atomic16_set(&ppp_ccb->ipv6_dp_bool, 0);

    TEST_ASSERT(set_ipv6_enable(fastrg_ccb, 0, TRUE) == SUCCESS,
        "enabling IPv6 returns SUCCESS", "got ERROR");
    TEST_ASSERT(ppp_ccb->ipv6_enabled == TRUE, "enabling IPv6 sets the flag", "flag is FALSE");
    TEST_ASSERT(rte_atomic16_read(&ppp_ccb->ipv6_dp_bool) == 1,
        "enabling IPv6 opens the data-plane gate", "gate still closed");

    tFastRG_MBX *posted = NULL;
    TEST_ASSERT(rte_ring_dequeue(cp_q, (void **)&posted) == 0 &&
        posted->type == EV_NORTHBOUND_PPPoE &&
        posted->northbound_msg.cmd == PPPoE_CMD_IPV6_CHANGED &&
        posted->northbound_msg.ccb_id == 0,
        "a real change queues the redial event", "no matching event on cp_q");

    /* Enabling again is not a change, so nothing is queued. */
    TEST_ASSERT(set_ipv6_enable(fastrg_ccb, 0, TRUE) == SUCCESS,
        "re-enabling IPv6 returns SUCCESS", "got ERROR");
    TEST_ASSERT(rte_ring_count(cp_q) == 0, "an unchanged value queues nothing",
        "cp_q has %u events", rte_ring_count(cp_q));

    TEST_ASSERT(set_ipv6_enable(fastrg_ccb, 0, FALSE) == SUCCESS,
        "disabling IPv6 returns SUCCESS", "got ERROR");
    TEST_ASSERT(ppp_ccb->ipv6_enabled == FALSE, "disabling IPv6 clears the flag", "flag is TRUE");
    TEST_ASSERT(rte_atomic16_read(&ppp_ccb->ipv6_dp_bool) == 0,
        "disabling IPv6 closes the data-plane gate", "gate still open");

    ppp_ccb->ipv6_enabled = FALSE;
    TEST_ASSERT(set_ipv6_enable(fastrg_ccb, fastrg_ccb->user_count, TRUE) == ERROR,
        "an out of range ccb id returns ERROR", "got SUCCESS");
    TEST_ASSERT(ppp_ccb->ipv6_enabled == FALSE,
        "an out of range ccb id writes nothing", "flag was changed");

    fastrg_ccb->cp_q = saved_cp_q;
    fastrg_ccb->free_mail_ring = saved_free_mail_ring;

out:
    ppp_ccb->ipv6_enabled = saved_enabled;
    ppp_ccb->ipv6cp_up = saved_ipv6cp_up;
    ppp_ccb->dhcp6_pd_ready = saved_pd_ready;
    rte_atomic16_set(&ppp_ccb->ipv6_dp_bool, saved_dp_bool);
    if (cp_q != NULL)
        rte_ring_free(cp_q);
    if (free_mail_ring != NULL)
        rte_ring_free(free_mail_ring);
}

void test_dns_cache_dump(void)
{
    printf("\nTesting dns_cache_dump:\n");
    printf("=========================================\n\n");

    dns_cache_t cache = {0};
    dns_cache_entry_t out[8];
    dns_cache_init(&cache);

    U32 count = dns_cache_dump(&cache, out, 8);
    TEST_ASSERT(count == 0, "an empty cache dumps 0 entries", "got %u", count);

    U8 raw[32] = {0};
    raw[0] = 0xAB;
    dns_cache_insert(&cache, "a.com", DNS_TYPE_A, raw, 16, 60);
    dns_cache_insert(&cache, "b.com", DNS_TYPE_A, raw, 16, 120);
    dns_cache_insert(&cache, "c.com", DNS_TYPE_AAAA, raw, 16, 180);
    TEST_ASSERT(cache.entry_count == 3, "the cache reports 3 entries to size a dump by",
        "got %u", cache.entry_count);

    memset(out, 0, sizeof(out));
    count = dns_cache_dump(&cache, out, cache.entry_count);
    TEST_ASSERT(count == 3, "3 entries dump as 3", "got %u", count);

    /* Bucket order is not the insert order, so match each domain by name. */
    BOOL seen_a = FALSE, seen_b = FALSE, seen_c = FALSE;
    for(U32 i=0; i<count; i++) {
        if (strcmp(out[i].domain, "a.com") == 0)
            seen_a = (out[i].qtype == DNS_TYPE_A && out[i].ttl == 60) ? TRUE : FALSE;
        else if (strcmp(out[i].domain, "b.com") == 0)
            seen_b = (out[i].qtype == DNS_TYPE_A && out[i].ttl == 120) ? TRUE : FALSE;
        else if (strcmp(out[i].domain, "c.com") == 0)
            seen_c = (out[i].qtype == DNS_TYPE_AAAA && out[i].ttl == 180) ? TRUE : FALSE;
    }
    TEST_ASSERT(seen_a == TRUE && seen_b == TRUE && seen_c == TRUE,
        "each dumped entry carries its own domain, qtype and ttl",
        "a=%d b=%d c=%d", seen_a, seen_b, seen_c);
    TEST_ASSERT(out[0].next == NULL && out[1].next == NULL && out[2].next == NULL,
        "every copy is unchained", "a chain pointer survived the copy");
    TEST_ASSERT(out[0].response_len == 16 && out[0].response[0] == 0xAB &&
        out[0].insert_time != 0,
        "the whole entry is copied, response and insert_time included",
        "len=%u byte0=%u", out[0].response_len, out[0].response[0]);

    count = dns_cache_dump(&cache, out, 2);
    TEST_ASSERT(count == 2, "max 2 stops the dump at 2 entries", "got %u", count);

    count = dns_cache_dump(&cache, out, 0);
    TEST_ASSERT(count == 0, "max 0 dumps nothing", "got %u", count);
    count = dns_cache_dump(NULL, out, 8);
    TEST_ASSERT(count == 0, "a NULL cache dumps nothing", "got %u", count);

    dns_cache_cleanup(&cache);
}

void test_dns_static_dump(void)
{
    printf("\nTesting dns_static_dump:\n");
    printf("=========================================\n\n");

    dns_static_table_t table;
    dns_static_record_t out[8];
    U32 ip1 = htonl(0x0A000001), ip2 = htonl(0x0A000002), ip3 = htonl(0x0A000003);
    dns_static_init(&table);

    U32 count = dns_static_dump(&table, out, 8);
    TEST_ASSERT(count == 0, "an empty table dumps 0 records", "got %u", count);

    dns_static_add(&table, "one.fastrg.org", ip1, 60);
    dns_static_add(&table, "two.fastrg.org", ip2, 120);
    dns_static_add(&table, "three.fastrg.org", ip3, 180);

    memset(out, 0, sizeof(out));
    count = dns_static_dump(&table, out, 8);
    TEST_ASSERT(count == 3, "3 records dump as 3", "got %u", count);
    TEST_ASSERT(strcmp(out[0].domain, "one.fastrg.org") == 0 &&
        out[0].ip_addr == ip1 && out[0].ttl == 60,
        "the first record keeps its domain, ip and ttl", "got %s", out[0].domain);
    TEST_ASSERT(strcmp(out[2].domain, "three.fastrg.org") == 0 && out[2].ip_addr == ip3,
        "the last record keeps its domain and ip", "got %s", out[2].domain);

    /* A removed record leaves an inactive slot the dump has to skip. */
    dns_static_remove(&table, "two.fastrg.org");
    memset(out, 0, sizeof(out));
    count = dns_static_dump(&table, out, 8);
    TEST_ASSERT(count == 2, "an inactive slot is skipped", "got %u", count);
    TEST_ASSERT(strcmp(out[0].domain, "one.fastrg.org") == 0 &&
        strcmp(out[1].domain, "three.fastrg.org") == 0,
        "the surviving records dump without a gap", "got %s and %s",
        out[0].domain, out[1].domain);

    count = dns_static_dump(&table, out, 1);
    TEST_ASSERT(count == 1, "max 1 stops the dump at 1 record", "got %u", count);

    count = dns_static_dump(&table, out, 0);
    TEST_ASSERT(count == 0, "max 0 dumps nothing", "got %u", count);
    count = dns_static_dump(NULL, out, 8);
    TEST_ASSERT(count == 0, "a NULL table dumps nothing", "got %u", count);
}

void test_cli_request(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           CLI Request Unit Tests                           ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_cli_request_result_pack();
    test_cli_request_publish(fastrg_ccb);
    test_cli_request_is_abandoned(fastrg_ccb);
    test_set_dns_proxy_enable(fastrg_ccb);
    test_set_tcp_conntrack_enable(fastrg_ccb);
    test_set_ipv6_enable(fastrg_ccb);
    test_dns_cache_dump();
    test_dns_static_dump();

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
