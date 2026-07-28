#include <stdio.h>
#include <string.h>

#include "../../northbound/cmdline/ip_pool_range.h"
#include "test.h"
#include "test_helper.h"

#define POOL_BUFFER_SIZE 32
#define GUARD_VALUE 0xA5

static int test_count = 0;
static int pass_count = 0;

struct guarded_pool_buffer {
    unsigned char before;
    char value[POOL_BUFFER_SIZE];
    unsigned char after;
};

static void init_guarded_buffer(struct guarded_pool_buffer *buffer)
{
    memset(buffer, GUARD_VALUE, sizeof(*buffer));
}

static BOOL guarded_buffer_unchanged(const struct guarded_pool_buffer *buffer)
{
    unsigned char expected[POOL_BUFFER_SIZE];

    memset(expected, GUARD_VALUE, sizeof(expected));
    return buffer->before == GUARD_VALUE && buffer->after == GUARD_VALUE &&
        memcmp(buffer->value, expected, sizeof(expected)) == 0;
}

static void test_valid_pool_ranges(void)
{
    char start[POOL_BUFFER_SIZE] = {0};
    char end[POOL_BUFFER_SIZE] = {0};
    STATUS ret;

    ret = cli_parse_ip_pool_range("192.168.3.2~192.168.3.5", start, sizeof(start), end, sizeof(end));
    TEST_ASSERT(ret == SUCCESS && strcmp(start, "192.168.3.2") == 0 && strcmp(end, "192.168.3.5") == 0,
        "tilde-separated IP pool range is parsed", "ret=%d, start=%s, end=%s", ret, start, end);

    memset(start, 0, sizeof(start));
    memset(end, 0, sizeof(end));
    ret = cli_parse_ip_pool_range("192.168.3.2-192.168.3.5", start, sizeof(start), end, sizeof(end));
    TEST_ASSERT(ret == SUCCESS && strcmp(start, "192.168.3.2") == 0 && strcmp(end, "192.168.3.5") == 0,
        "hyphen-separated IP pool range is parsed", "ret=%d, start=%s, end=%s", ret, start, end);
}

static void test_oversized_pool_ranges(void)
{
    struct guarded_pool_buffer start;
    struct guarded_pool_buffer end;
    STATUS ret;

    init_guarded_buffer(&start);
    init_guarded_buffer(&end);
    ret = cli_parse_ip_pool_range("12345678901234567890123456789012~192.168.3.5", start.value,
        sizeof(start.value), end.value, sizeof(end.value));
    TEST_ASSERT(ret == ERROR && guarded_buffer_unchanged(&start) && guarded_buffer_unchanged(&end),
        "32-byte pool start is rejected without writing destinations", "ret=%d", ret);

    init_guarded_buffer(&start);
    init_guarded_buffer(&end);
    ret = cli_parse_ip_pool_range("192.168.3.2~12345678901234567890123456789012", start.value,
        sizeof(start.value), end.value, sizeof(end.value));
    TEST_ASSERT(ret == ERROR && guarded_buffer_unchanged(&start) && guarded_buffer_unchanged(&end),
        "32-byte pool end is rejected without writing destinations", "ret=%d", ret);

    init_guarded_buffer(&start);
    init_guarded_buffer(&end);
    ret = cli_parse_ip_pool_range(
        "12345678901234567890123456789012~12345678901234567890123456789012", start.value,
        sizeof(start.value), end.value, sizeof(end.value));
    TEST_ASSERT(ret == ERROR && guarded_buffer_unchanged(&start) && guarded_buffer_unchanged(&end),
        "oversized pool start and end are rejected without writing destinations", "ret=%d", ret);
}

static void test_missing_pool_range_delimiter(void)
{
    struct guarded_pool_buffer start;
    struct guarded_pool_buffer end;
    STATUS ret;

    init_guarded_buffer(&start);
    init_guarded_buffer(&end);
    ret = cli_parse_ip_pool_range("192.168.3.2", start.value, sizeof(start.value), end.value, sizeof(end.value));
    TEST_ASSERT(ret == ERROR && guarded_buffer_unchanged(&start) && guarded_buffer_unchanged(&end),
        "IP pool range without delimiter is rejected", "ret=%d", ret);
}

void test_cmdline(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    (void)fastrg_ccb;

    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           Command Line Unit Tests                          ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_valid_pool_ranges();
    test_oversized_pool_ranges();
    test_missing_pool_range_delimiter();

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

    *total_tests += test_count;
    *total_pass += pass_count;
}
