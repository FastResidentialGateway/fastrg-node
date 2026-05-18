#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "../src/config.h"
#include "../src/fastrg.h"
#include "test_helper.h"

static int test_count = 0;
static int pass_count = 0;

void test_parse_config_valid(FastRG_t *fastrg_ccb)
{
    printf("\nTesting parse_config function for valid config:\n");
    printf("=========================================\n\n");

    struct fastrg_config cfg = {0};

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
    TEST_ASSERT(fastrg_ccb->max_user_count == 100, "check max user count", 
        "MaxUserCount != 100");
    TEST_ASSERT(fastrg_ccb->user_count == 10, "check initial user count", 
        "InitUserCount != 10");
    TEST_ASSERT(cfg.heartbeat_interval == 60, "check heartbeat interval", 
        "HeartbeatInterval != 60");

    unlink(test_config);
    printf("✓ Test passed\n");
}

void test_parse_config_invalid(FastRG_t *fastrg_ccb)
{
    printf("\nTesting parse_config function for invalid config:\n");
    printf("=========================================\n\n");

    struct fastrg_config cfg = {0};

    const char *test_config = "/tmp/test_fastrg_invalid.conf";
    FILE *fp = fopen(test_config, "w");
    TEST_ASSERT(fp != NULL, "Mock config file", 
        "Create test config file failed");

    fprintf(fp, "MaxUserCount = 0;\n");  /* Invalid */
    fprintf(fp, "InitUserCount = 0;\n");  /* Invalid */
    fclose(fp);

    STATUS ret = parse_config(test_config, fastrg_ccb, &cfg);
    TEST_ASSERT(ret == ERROR, "Check parse_config return value", 
        "parse_config returns SUCCESS for invalid MaxUserCount");

    unlink(test_config);

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