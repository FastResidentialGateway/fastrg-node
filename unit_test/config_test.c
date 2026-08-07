#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "../src/config.h"
#include "../src/fastrg.h"
#include "../src/init.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

void test_parse_config_valid(FastRG_t *fastrg_ccb)
{
    printf("\nTesting parse_config function for valid config:\n");
    printf("=========================================\n\n");

    struct fastrg_config cfg = {0};
    /* parse_config writes user_count into the shared mock ccb; save the
     * capacity too so this case can prove a legacy field does not touch it. */
    U16 saved_user_count = fastrg_ccb->user_count;
    U16 saved_max_user_count = fastrg_ccb->max_user_count;

    /* Create a temporary config file */
    const char *test_config = "/tmp/test_fastrg.conf";
    FILE *fp = fopen(test_config, "w");
    TEST_ASSERT(fp != NULL, "Mock config file", "Create test config file failed");

    fprintf(fp, "MaxUserCount = 100;\n");
    fprintf(fp, "InitUserCount = 10;\n");
    fprintf(fp, "Loglvl = \"INFO\";\n");
    fprintf(fp, "HeartbeatInterval = 60;\n");
    fclose(fp);

    STATUS ret = parse_config(test_config, fastrg_ccb, &cfg);
    TEST_ASSERT(ret == SUCCESS, "Check parse_config return value", 
        "parse_config returns ERROR");
    TEST_ASSERT(fastrg_ccb->max_user_count == saved_max_user_count,
        "legacy MaxUserCount is ignored", "max_user_count changed from %u to %u",
        saved_max_user_count, fastrg_ccb->max_user_count);
    /* user_count is configured after admin set the subscriber count */
    TEST_ASSERT(fastrg_ccb->user_count == 0,
        "user_count should boots at 0", "user_count != 0");
    TEST_ASSERT(cfg.heartbeat_interval == 60, "check heartbeat interval", 
        "HeartbeatInterval != 60");

    fastrg_ccb->user_count = saved_user_count;
    fastrg_ccb->max_user_count = saved_max_user_count;
    unlink(test_config);
    printf("✓ Test passed\n");
}

void test_parse_config_invalid(FastRG_t *fastrg_ccb)
{
    printf("\nTesting parse_config function for invalid config:\n");
    printf("=========================================\n\n");

    struct fastrg_config cfg = {0};
    U16 saved_user_count = fastrg_ccb->user_count;
    U16 saved_max_user_count = fastrg_ccb->max_user_count;

    const char *test_config = "/tmp/test_fastrg_invalid.conf";
    FILE *fp = fopen(test_config, "w");
    TEST_ASSERT(fp != NULL, "Mock config file", 
        "Create test config file failed");

    fprintf(fp, "MaxUserCount = 0;\n");  /* Legacy field, ignored */
    fprintf(fp, "InitUserCount = 0;\n");  /* Malformed config, should be ignored */
    fclose(fp);

    STATUS ret = parse_config(test_config, fastrg_ccb, &cfg);
    TEST_ASSERT(ret == SUCCESS, "legacy MaxUserCount does not reject config",
        "parse_config returns ERROR for ignored MaxUserCount");
    TEST_ASSERT(fastrg_ccb->max_user_count == saved_max_user_count,
        "ignored MaxUserCount leaves capacity unchanged", "max_user_count changed from %u to %u",
        saved_max_user_count, fastrg_ccb->max_user_count);

    fastrg_ccb->user_count = saved_user_count;
    fastrg_ccb->max_user_count = saved_max_user_count;
    unlink(test_config);

    printf("✓ Test passed\n");
}

void test_compute_max_user_count(FastRG_t *fastrg_ccb)
{
    const uint64_t reserve = 512ULL * 1024ULL * 1024ULL;
    const uint64_t per_subscriber = 175ULL * 1024ULL * 1024ULL;
    U16 saved_max_user_count = fastrg_ccb->max_user_count;

    printf("\nTesting hugepage-based subscriber capacity:\n");
    printf("===========================================\n\n");

    fastrg_set_hugepage_free_bytes_for_test(0);
    fastrg_compute_max_user_count(fastrg_ccb);
    TEST_ASSERT(fastrg_ccb->max_user_count == MIN_USER_COUNT,
        "tiny hugepage budget clamps to minimum", "max_user_count=%u", fastrg_ccb->max_user_count);

    fastrg_set_hugepage_free_bytes_for_test(reserve + 10 * per_subscriber + per_subscriber - 1);
    fastrg_compute_max_user_count(fastrg_ccb);
    TEST_ASSERT(fastrg_ccb->max_user_count == 10,
        "middle hugepage budget uses integer division", "max_user_count=%u", fastrg_ccb->max_user_count);

    fastrg_set_hugepage_free_bytes_for_test(reserve + ((uint64_t)MAX_USER_COUNT + 1) * per_subscriber);
    fastrg_compute_max_user_count(fastrg_ccb);
    TEST_ASSERT(fastrg_ccb->max_user_count == MAX_USER_COUNT,
        "large hugepage budget clamps to maximum", "max_user_count=%u", fastrg_ccb->max_user_count);

    fastrg_ccb->max_user_count = saved_max_user_count;
    printf("✓ Test passed\n");
}

void test_config(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║            Configuration Module Unit Tests                ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    test_parse_config_valid(fastrg_ccb);
    test_parse_config_invalid(fastrg_ccb);
    test_compute_max_user_count(fastrg_ccb);

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
