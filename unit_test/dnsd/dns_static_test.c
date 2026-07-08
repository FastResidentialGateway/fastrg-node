#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <common.h>

#include "../../src/dnsd/dns_static.h"
#include "../../src/fastrg.h"
#include "../test_helper.h"

static int test_count = 0;
static int pass_count = 0;

static void test_dns_static_init_cleanup(void)
{
    printf("\nTesting dns_static_init / dns_static_cleanup:\n");
    printf("=========================================\n\n");

    dns_static_table_t table;
    memset(&table, 0xFF, sizeof(table));

    TEST_ASSERT(dns_static_init(NULL) == ERROR, "dns_static_init rejects NULL", "");
    TEST_ASSERT(dns_static_init(&table) == SUCCESS, "dns_static_init returns SUCCESS", "");
    TEST_ASSERT(dns_static_get_count(&table) == 0, "init zeroes count",
        "got %u", dns_static_get_count(&table));
    TEST_ASSERT(dns_static_lookup(&table, "anything") == NULL,
        "init leaves no active records", "");

    dns_static_add(&table, "cleanup.test", htonl(0x0A000001), 60);
    dns_static_cleanup(&table);
    TEST_ASSERT(dns_static_get_count(&table) == 0, "cleanup zeroes count",
        "got %u", dns_static_get_count(&table));
    TEST_ASSERT(dns_static_lookup(&table, "cleanup.test") == NULL,
        "cleanup removes records", "");
    dns_static_cleanup(NULL); /* must not crash */
}

static void test_dns_static_add_lookup(void)
{
    printf("\nTesting dns_static_add / dns_static_lookup:\n");
    printf("=========================================\n\n");

    dns_static_table_t table;
    dns_static_init(&table);
    U32 ip1 = htonl(0xC0A80101); /* 192.168.1.1 */
    U32 ip2 = htonl(0xC0A80102); /* 192.168.1.2 */

    TEST_ASSERT(dns_static_add(NULL, "a.b", ip1, 60) == ERROR,
        "add rejects NULL table", "");
    TEST_ASSERT(dns_static_add(&table, NULL, ip1, 60) == ERROR,
        "add rejects NULL domain", "");
    TEST_ASSERT(dns_static_lookup(NULL, "a.b") == NULL,
        "lookup rejects NULL table", "");
    TEST_ASSERT(dns_static_lookup(&table, NULL) == NULL,
        "lookup rejects NULL domain", "");

    TEST_ASSERT(dns_static_add(&table, "www.fastrg.org", ip1, 120) == SUCCESS,
        "add returns SUCCESS", "");
    TEST_ASSERT(dns_static_get_count(&table) == 1, "count is 1 after add",
        "got %u", dns_static_get_count(&table));

    dns_static_record_t *rec = dns_static_lookup(&table, "www.fastrg.org");
    TEST_ASSERT(rec != NULL, "lookup finds added record", "");
    TEST_ASSERT(rec != NULL && rec->ip_addr == ip1, "record IP matches", "");
    TEST_ASSERT(rec != NULL && rec->ttl == 120, "record TTL matches",
        "got %u", rec ? rec->ttl : 0);

    /* default TTL when 0 is passed */
    dns_static_add(&table, "defttl.fastrg.org", ip2, 0);
    rec = dns_static_lookup(&table, "defttl.fastrg.org");
    TEST_ASSERT(rec != NULL && rec->ttl == 3600, "ttl=0 falls back to default 3600",
        "got %u", rec ? rec->ttl : 0);

    /* update in place: same domain, new IP/TTL, count unchanged */
    U32 before = dns_static_get_count(&table);
    TEST_ASSERT(dns_static_add(&table, "www.fastrg.org", ip2, 300) == SUCCESS,
        "re-add existing domain returns SUCCESS", "");
    TEST_ASSERT(dns_static_get_count(&table) == before,
        "re-add does not bump count", "expected %u, got %u", before, dns_static_get_count(&table));
    rec = dns_static_lookup(&table, "www.fastrg.org");
    TEST_ASSERT(rec != NULL && rec->ip_addr == ip2 && rec->ttl == 300,
        "re-add updates IP and TTL in place", "");
}

static void test_dns_static_case_insensitive(void)
{
    printf("\nTesting dns_static case-insensitive matching:\n");
    printf("=========================================\n\n");

    dns_static_table_t table;
    dns_static_init(&table);
    U32 ip = htonl(0x01020304);

    dns_static_add(&table, "WWW.Example.COM", ip, 60);
    dns_static_record_t *rec = dns_static_lookup(&table, "www.example.com");
    TEST_ASSERT(rec != NULL, "mixed-case add found by lowercase lookup", "");
    TEST_ASSERT(rec != NULL && strcmp(rec->domain, "www.example.com") == 0,
        "stored domain is lowercased", "got '%s'", rec ? rec->domain : "(null)");
    TEST_ASSERT(dns_static_lookup(&table, "WWW.EXAMPLE.COM") != NULL,
        "uppercase lookup matches", "");

    /* update path must also match case-insensitively (no duplicate slot) */
    dns_static_add(&table, "www.EXAMPLE.com", htonl(0x05060708), 90);
    TEST_ASSERT(dns_static_get_count(&table) == 1,
        "case-variant re-add updates instead of duplicating",
        "got %u", dns_static_get_count(&table));

    /* remove must match case-insensitively too */
    TEST_ASSERT(dns_static_remove(&table, "Www.Example.Com") == SUCCESS,
        "case-variant remove succeeds", "");
    TEST_ASSERT(dns_static_get_count(&table) == 0, "count back to 0",
        "got %u", dns_static_get_count(&table));

    /* prefix must not match: "a.b" vs "a.bc" */
    dns_static_add(&table, "a.bc", ip, 60);
    TEST_ASSERT(dns_static_lookup(&table, "a.b") == NULL,
        "prefix does not match longer stored domain", "");
    dns_static_add(&table, "a.b", ip, 60);
    TEST_ASSERT(dns_static_lookup(&table, "a.bcd") == NULL,
        "longer query does not match shorter stored domain", "");
}

static void test_dns_static_remove(void)
{
    printf("\nTesting dns_static_remove:\n");
    printf("=========================================\n\n");

    dns_static_table_t table;
    dns_static_init(&table);
    U32 ip = htonl(0x0A0A0A0A);

    TEST_ASSERT(dns_static_remove(NULL, "a.b") == ERROR, "remove rejects NULL table", "");
    TEST_ASSERT(dns_static_remove(&table, NULL) == ERROR, "remove rejects NULL domain", "");
    TEST_ASSERT(dns_static_remove(&table, "ghost.fastrg.org") == ERROR,
        "remove of nonexistent domain returns ERROR", "");

    dns_static_add(&table, "keep.fastrg.org", ip, 60);
    dns_static_add(&table, "drop.fastrg.org", ip, 60);
    TEST_ASSERT(dns_static_remove(&table, "drop.fastrg.org") == SUCCESS,
        "remove returns SUCCESS", "");
    TEST_ASSERT(dns_static_lookup(&table, "drop.fastrg.org") == NULL,
        "removed domain no longer found", "");
    TEST_ASSERT(dns_static_lookup(&table, "keep.fastrg.org") != NULL,
        "other record survives remove", "");
    TEST_ASSERT(dns_static_get_count(&table) == 1, "count decremented",
        "got %u", dns_static_get_count(&table));
    TEST_ASSERT(dns_static_remove(&table, "drop.fastrg.org") == ERROR,
        "double remove returns ERROR", "");
}

static void test_dns_static_capacity(void)
{
    printf("\nTesting dns_static 64-record capacity:\n");
    printf("=========================================\n\n");

    dns_static_table_t table;
    dns_static_init(&table);
    char domain[64];
    STATUS ret = SUCCESS;

    for (U32 i = 0; i < DNS_STATIC_MAX_RECORDS; i++) {
        snprintf(domain, sizeof(domain), "host%u.fastrg.org", i);
        if (dns_static_add(&table, domain, htonl(0x0A000000 + i), 60) != SUCCESS)
            ret = ERROR;
    }
    TEST_ASSERT(ret == SUCCESS, "all 64 adds succeed", "");
    TEST_ASSERT(dns_static_get_count(&table) == DNS_STATIC_MAX_RECORDS,
        "count reaches DNS_STATIC_MAX_RECORDS", "got %u", dns_static_get_count(&table));

    TEST_ASSERT(dns_static_add(&table, "overflow.fastrg.org", htonl(1), 60) == ERROR,
        "65th add returns ERROR (table full)", "");
    TEST_ASSERT(dns_static_lookup(&table, "overflow.fastrg.org") == NULL,
        "overflow domain was not stored", "");

    /* update of an existing record must still work when full */
    TEST_ASSERT(dns_static_add(&table, "host0.fastrg.org", htonl(0x7F000001), 999) == SUCCESS,
        "in-place update works on a full table", "");

    /* slot reuse after remove */
    TEST_ASSERT(dns_static_remove(&table, "host3.fastrg.org") == SUCCESS,
        "remove from full table", "");
    TEST_ASSERT(dns_static_add(&table, "reused.fastrg.org", htonl(2), 60) == SUCCESS,
        "freed slot is reusable", "");
    TEST_ASSERT(dns_static_get_count(&table) == DNS_STATIC_MAX_RECORDS,
        "count back at max after reuse", "got %u", dns_static_get_count(&table));

    /* every survivor still resolvable (no slot corruption) */
    ret = SUCCESS;
    for (U32 i = 0; i < DNS_STATIC_MAX_RECORDS; i++) {
        if (i == 3)
            continue;
        snprintf(domain, sizeof(domain), "host%u.fastrg.org", i);
        if (dns_static_lookup(&table, domain) == NULL)
            ret = ERROR;
    }
    TEST_ASSERT(ret == SUCCESS, "all surviving records still resolvable", "");
}

static void test_dns_static_long_domain(void)
{
    printf("\nTesting dns_static domain-length truncation:\n");
    printf("=========================================\n\n");

    dns_static_table_t table;
    dns_static_init(&table);

    /* DNS_MAX_DOMAIN_LEN + 10 chars — must be truncated with a terminating NUL */
    char long_domain[DNS_MAX_DOMAIN_LEN + 11];
    memset(long_domain, 'a', sizeof(long_domain) - 1);
    long_domain[sizeof(long_domain) - 1] = '\0';

    TEST_ASSERT(dns_static_add(&table, long_domain, htonl(1), 60) == SUCCESS,
        "over-long domain add returns SUCCESS", "");
    dns_static_record_t *rec = &table.records[0];
    TEST_ASSERT(rec->active && strlen(rec->domain) == DNS_MAX_DOMAIN_LEN,
        "stored domain truncated to DNS_MAX_DOMAIN_LEN",
        "got len %zu", strlen(rec->domain));

    /* exact-length query matches the truncated record */
    char exact[DNS_MAX_DOMAIN_LEN + 1];
    memset(exact, 'a', DNS_MAX_DOMAIN_LEN);
    exact[DNS_MAX_DOMAIN_LEN] = '\0';
    TEST_ASSERT(dns_static_lookup(&table, exact) != NULL,
        "truncated record found by exact-length query", "");
    /* the original over-long query does NOT match the stored truncation */
    TEST_ASSERT(dns_static_lookup(&table, long_domain) == NULL,
        "over-long query does not match truncated record", "");

    TEST_ASSERT(dns_static_get_count(NULL) == 0, "get_count(NULL) is 0", "");
}

void test_dns_static(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;

    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           DNS Static Record Unit Tests                     ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_dns_static_init_cleanup();
    test_dns_static_add_lookup();
    test_dns_static_case_insensitive();
    test_dns_static_remove();
    test_dns_static_capacity();
    test_dns_static_long_domain();

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
