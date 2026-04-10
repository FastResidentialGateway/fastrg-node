#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <common.h>

#include "../../src/dnsd/dns_cache.h"
#include "../../src/dnsd/dns_static.h"
#include "../../src/fastrg.h"
#include "../test_helper.h"

static int test_count = 0;
static int pass_count = 0;

/* Mirror of the static dns_cache_hash() in dns_cache.c — used only in tests
 * to locate domain names that land in a specific hash bucket. */
static U32 test_dns_cache_bucket(const char *domain, U16 qtype)
{
    U32 hash = 2166136261u;
    for (const char *p = domain; *p; p++) {
        hash ^= (U32)tolower((unsigned char)*p);
        hash *= 16777619u;
    }
    hash ^= (U32)qtype;
    hash *= 16777619u;
    return hash % DNS_CACHE_BUCKET_COUNT;
}

/* ---- Cache tests ---- */

static void test_dns_cache_init_cleanup(void)
{
    printf("\nTesting dns_cache_init / cleanup:\n");
    printf("=========================================\n\n");

    dns_cache_t cache = {0};
    STATUS ret = dns_cache_init(&cache);
    TEST_ASSERT(ret == SUCCESS, "dns_cache_init returns SUCCESS", "");
    TEST_ASSERT(cache.entry_count == 0, "initial entry_count is 0",
        "got %u", cache.entry_count);
    dns_cache_cleanup(&cache);
}

static void test_dns_cache_insert_lookup(void)
{
    printf("\nTesting dns_cache_insert / lookup:\n");
    printf("=========================================\n\n");

    dns_cache_t cache = {0};
    dns_cache_init(&cache);

    /* Build a raw DNS response for caching */
    U8 raw[64];
    memset(raw, 0, sizeof(raw));
    /* Simple: just header bytes, enough for storage */
    raw[0] = 0x00; raw[1] = 0x01; /* ID */

    STATUS ret = dns_cache_insert(&cache, "test.example.com", DNS_TYPE_A,
        raw, 32, 300);
    TEST_ASSERT(ret == SUCCESS, "dns_cache_insert returns SUCCESS", "");
    TEST_ASSERT(cache.entry_count == 1, "entry_count is 1 after insert",
        "got %u", cache.entry_count);

    dns_cache_entry_t *entry = dns_cache_lookup(&cache, "test.example.com", DNS_TYPE_A);
    TEST_ASSERT(entry != NULL, "dns_cache_lookup finds inserted entry", "");
    TEST_ASSERT(entry->ttl == 300, "TTL is 300", "got %u", entry->ttl);

    /* Case-insensitive lookup */
    entry = dns_cache_lookup(&cache, "TEST.EXAMPLE.COM", DNS_TYPE_A);
    TEST_ASSERT(entry != NULL, "case-insensitive lookup works", "");

    /* Miss */
    entry = dns_cache_lookup(&cache, "notfound.com", DNS_TYPE_A);
    TEST_ASSERT(entry == NULL, "lookup for non-existent domain returns NULL", "");

    /* Wrong qtype */
    entry = dns_cache_lookup(&cache, "test.example.com", DNS_TYPE_AAAA);
    TEST_ASSERT(entry == NULL, "lookup with wrong qtype returns NULL", "");

    dns_cache_cleanup(&cache);
}

static void test_dns_cache_flush(void)
{
    printf("\nTesting dns_cache_flush:\n");
    printf("=========================================\n\n");

    dns_cache_t cache = {0};
    dns_cache_init(&cache);

    U8 raw[32] = {0};
    dns_cache_insert(&cache, "a.com", DNS_TYPE_A, raw, 16, 60);
    dns_cache_insert(&cache, "b.com", DNS_TYPE_A, raw, 16, 60);
    dns_cache_insert(&cache, "c.com", DNS_TYPE_A, raw, 16, 60);
    TEST_ASSERT(cache.entry_count == 3, "3 entries after inserts",
        "got %u", cache.entry_count);

    U32 flushed = dns_cache_flush(&cache);
    TEST_ASSERT(flushed == 3, "flush returns 3", "got %u", flushed);
    TEST_ASSERT(cache.entry_count == 0, "entry_count is 0 after flush",
        "got %u", cache.entry_count);

    dns_cache_entry_t *entry = dns_cache_lookup(&cache, "a.com", DNS_TYPE_A);
    TEST_ASSERT(entry == NULL, "lookup after flush returns NULL", "");

    dns_cache_cleanup(&cache);
}

static void test_dns_cache_update(void)
{
    printf("\nTesting dns_cache update (re-insert):\n");
    printf("=========================================\n\n");

    dns_cache_t cache = {0};
    dns_cache_init(&cache);

    U8 raw1[32] = {1};
    U8 raw2[32] = {2};
    dns_cache_insert(&cache, "update.test", DNS_TYPE_A, raw1, 16, 60);
    dns_cache_insert(&cache, "update.test", DNS_TYPE_A, raw2, 16, 120);

    TEST_ASSERT(cache.entry_count == 1, "re-insert does not duplicate",
        "got %u", cache.entry_count);

    dns_cache_entry_t *entry = dns_cache_lookup(&cache, "update.test", DNS_TYPE_A);
    TEST_ASSERT(entry != NULL, "entry found after re-insert", "");
    TEST_ASSERT(entry->ttl == 120, "TTL updated to 120", "got %u", entry->ttl);

    dns_cache_cleanup(&cache);
}

static void test_dns_cache_eviction(void)
{
    printf("\nTesting dns_cache_insert eviction (max-entries overflow):\n");
    printf("=========================================\n\n");

    dns_cache_t cache = {0};
    dns_cache_init(&cache);

    U8  raw[32] = {0};
    char victim_domain[64];
    char filler_domain[64];

    /* Find a victim domain name that hashes to the same bucket as
     * "newcomer.com" so the eviction logic will see it as a candidate. */
    U32 newcomer_bucket = test_dns_cache_bucket("newcomer.com", DNS_TYPE_A);
    int victim_found = 0;
    for(int i=0; i<100000; i++) {
        snprintf(victim_domain, sizeof(victim_domain), "v%d.test", i);
        if (test_dns_cache_bucket(victim_domain, DNS_TYPE_A) == newcomer_bucket) {
            victim_found = 1;
            break;
        }
    }
    TEST_ASSERT(victim_found, "found a victim domain in newcomer bucket", "");

    /* Step 1: insert victim first — it will have the oldest insert_time. */
    dns_cache_insert(&cache, victim_domain, DNS_TYPE_A, raw, 16, 3600);

    /* Step 2: insert filler entries up to just before we need update1/update2.
     * Total target = DNS_CACHE_MAX_ENTRIES (512).
     * victim(1) + filler(509) + update1(1) + update2(1) = 512. */
    int filler_count = DNS_CACHE_MAX_ENTRIES - 1 - 2; /* 509 */
    for(int i=0; i<filler_count; i++) {
        snprintf(filler_domain, sizeof(filler_domain), "f%03d.test", i);
        STATUS ret = dns_cache_insert(&cache, filler_domain, DNS_TYPE_A, raw, 16, 3600);
        TEST_ASSERT(ret == SUCCESS, "filler insert succeeds", "i=%d", i);
    }

    /* Step 3: insert update1 and update2 (these reach entry_count == 512). */
    dns_cache_insert(&cache, "update1.com", DNS_TYPE_A, raw, 16, 60);
    dns_cache_insert(&cache, "update2.com", DNS_TYPE_A, raw, 16, 60);
    TEST_ASSERT(cache.entry_count == DNS_CACHE_MAX_ENTRIES,
        "entry_count reaches DNS_CACHE_MAX_ENTRIES after filling",
        "got %u", cache.entry_count);

    /* Step 4: re-insert update1 and update2 with a new TTL — this refreshes
     * their insert_time so they are no longer candidates for oldest-eviction. */
    dns_cache_insert(&cache, "update1.com", DNS_TYPE_A, raw, 16, 7200);
    dns_cache_insert(&cache, "update2.com", DNS_TYPE_A, raw, 16, 7200);
    TEST_ASSERT(cache.entry_count == DNS_CACHE_MAX_ENTRIES,
        "re-insert does not change entry_count",
        "got %u", cache.entry_count);

    dns_cache_entry_t *e;
    e = dns_cache_lookup(&cache, "update1.com", DNS_TYPE_A);
    TEST_ASSERT(e != NULL && e->ttl == 7200, "update1 TTL refreshed to 7200", "");
    e = dns_cache_lookup(&cache, "update2.com", DNS_TYPE_A);
    TEST_ASSERT(e != NULL && e->ttl == 7200, "update2 TTL refreshed to 7200", "");

    /* Step 5: insert newcomer — cache is at capacity, eviction fires.
     * The eviction loop scans newcomer's bucket and evicts the oldest
     * entry there, which is victim_domain (inserted first of all). */
    STATUS ret = dns_cache_insert(&cache, "newcomer.com", DNS_TYPE_A, raw, 16, 3600);
    TEST_ASSERT(ret == SUCCESS, "newcomer insert returns SUCCESS", "");

    /* Step 6: verify post-eviction state. */
    TEST_ASSERT(cache.entry_count == DNS_CACHE_MAX_ENTRIES,
        "entry_count stays at DNS_CACHE_MAX_ENTRIES after eviction + insert",
        "got %u", cache.entry_count);

    e = dns_cache_lookup(&cache, "newcomer.com", DNS_TYPE_A);
    TEST_ASSERT(e != NULL, "newcomer.com found in cache after insert", "");

    e = dns_cache_lookup(&cache, victim_domain, DNS_TYPE_A);
    TEST_ASSERT(e == NULL, "victim domain evicted from cache", "");

    e = dns_cache_lookup(&cache, "update1.com", DNS_TYPE_A);
    TEST_ASSERT(e != NULL, "update1.com still present after eviction", "");

    e = dns_cache_lookup(&cache, "update2.com", DNS_TYPE_A);
    TEST_ASSERT(e != NULL, "update2.com still present after eviction", "");

    dns_cache_cleanup(&cache);
}

/* ---- Static record tests ---- */

static void test_static_init_cleanup(void)
{
    printf("\nTesting dns_static_init / cleanup:\n");
    printf("=========================================\n\n");

    dns_static_table_t table = {0};
    STATUS ret = dns_static_init(&table);
    TEST_ASSERT(ret == SUCCESS, "dns_static_init returns SUCCESS", "");
    TEST_ASSERT(dns_static_get_count(&table) == 0, "initial count is 0", "");
    dns_static_cleanup(&table);
}

static void test_static_add_lookup(void)
{
    printf("\nTesting dns_static_add / lookup:\n");
    printf("=========================================\n\n");

    dns_static_table_t table = {0};
    dns_static_init(&table);

    U32 ip = htonl(0xC0A80101); /* 192.168.1.1 */
    STATUS ret = dns_static_add(&table, "mysite.local", ip, 3600);
    TEST_ASSERT(ret == SUCCESS, "dns_static_add returns SUCCESS", "");
    TEST_ASSERT(dns_static_get_count(&table) == 1, "count is 1 after add", "");

    dns_static_record_t *rec = dns_static_lookup(&table, "mysite.local");
    TEST_ASSERT(rec != NULL, "dns_static_lookup finds added record", "");
    TEST_ASSERT(rec->ip_addr == ip, "IP address matches", "");
    TEST_ASSERT(rec->ttl == 3600, "TTL matches", "got %u", rec->ttl);

    /* Case-insensitive */
    rec = dns_static_lookup(&table, "MYSITE.LOCAL");
    TEST_ASSERT(rec != NULL, "case-insensitive lookup works", "");

    /* Miss */
    rec = dns_static_lookup(&table, "notfound.local");
    TEST_ASSERT(rec == NULL, "lookup for non-existent returns NULL", "");

    dns_static_cleanup(&table);
}

static void test_static_remove(void)
{
    printf("\nTesting dns_static_remove:\n");
    printf("=========================================\n\n");

    dns_static_table_t table = {0};
    dns_static_init(&table);

    U32 ip = htonl(0x0A000001); /* 10.0.0.1 */
    dns_static_add(&table, "remove.test", ip, 600);
    TEST_ASSERT(dns_static_get_count(&table) == 1, "count is 1 after add", "");

    STATUS ret = dns_static_remove(&table, "remove.test");
    TEST_ASSERT(ret == SUCCESS, "dns_static_remove returns SUCCESS", "");
    TEST_ASSERT(dns_static_get_count(&table) == 0, "count is 0 after remove", "");

    dns_static_record_t *rec = dns_static_lookup(&table, "remove.test");
    TEST_ASSERT(rec == NULL, "lookup after remove returns NULL", "");

    /* Remove non-existent */
    ret = dns_static_remove(&table, "nonexistent.test");
    TEST_ASSERT(ret != SUCCESS, "remove non-existent returns ERROR", "");

    dns_static_cleanup(&table);
}

static void test_static_duplicate(void)
{
    printf("\nTesting dns_static_add duplicate (update):\n");
    printf("=========================================\n\n");

    dns_static_table_t table = {0};
    dns_static_init(&table);

    U32 ip1 = htonl(0x01020304);
    U32 ip2 = htonl(0x05060708);
    dns_static_add(&table, "dup.test", ip1, 100);
    dns_static_add(&table, "dup.test", ip2, 200);

    TEST_ASSERT(dns_static_get_count(&table) == 1,
        "count stays 1 after duplicate add (update in-place)", "");

    dns_static_record_t *rec = dns_static_lookup(&table, "dup.test");
    TEST_ASSERT(rec != NULL, "record still findable", "");
    /* The second add should update the existing entry */
    TEST_ASSERT(rec->ip_addr == ip2, "IP updated to second value", "");
    TEST_ASSERT(rec->ttl == 200, "TTL updated", "got %u", rec->ttl);

    dns_static_cleanup(&table);
}

void test_dns_cache(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           DNS Cache Unit Tests                             ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    /* Cache tests */
    test_dns_cache_init_cleanup();
    test_dns_cache_insert_lookup();
    test_dns_cache_flush();
    test_dns_cache_update();
    test_dns_cache_eviction();

    /* Static record tests */
    test_static_init_cleanup();
    test_static_add_lookup();
    test_static_remove();
    test_static_duplicate();

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
