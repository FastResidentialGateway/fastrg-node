#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <common.h>

#include "../src/etcd_integration.h"
#include "../src/fastrg.h"
#include "test_helper.h"

// Global test counters
static int test_count = 0;
static int pass_count = 0;

void test_etcd_mark_pending_event()
{
    printf("\nTesting etcd_mark_pending_event function:\n");
    printf("=========================================\n\n");

    // Test 1: Mark a pending event
    printf("Test 1: Mark a pending event\n");
    etcd_mark_pending_event(HSI_ACTION_CREATE, 1);
    // Verify by checking if confirm works (if mark didn't work, confirm would fail silently)
    etcd_confirm_pending_event(HSI_ACTION_CREATE, 1, 100);
    BOOL is_self = etcd_is_self_event(HSI_ACTION_CREATE, 1, 100);
    TEST_ASSERT(is_self == TRUE, "Mark and confirm creates self-event", "got %d", is_self);

    // Test 2: Confirm event that was not marked (should be a no-op)
    printf("Test 2: Confirm event that was not marked\n");
    etcd_confirm_pending_event(HSI_ACTION_UPDATE, 99, 9999);
    BOOL is_self_nonexistent = etcd_is_self_event(HSI_ACTION_UPDATE, 99, 9999);
    TEST_ASSERT(is_self_nonexistent == FALSE, "Non-marked event is not self-event", "got %d", is_self_nonexistent);

    // Test 3: Mark multiple events with different actions
    printf("Test 3: Mark multiple events with different actions\n");
    etcd_mark_pending_event(HSI_ACTION_UPDATE, 2);
    etcd_mark_pending_event(HSI_ACTION_DELETE, 3);
    etcd_confirm_pending_event(HSI_ACTION_UPDATE, 2, 200);
    etcd_confirm_pending_event(HSI_ACTION_DELETE, 3, 300);

    BOOL is_self_update = etcd_is_self_event(HSI_ACTION_UPDATE, 2, 200);
    BOOL is_self_delete = etcd_is_self_event(HSI_ACTION_DELETE, 3, 300);
    TEST_ASSERT(is_self_update == TRUE && is_self_delete == TRUE, 
        "Multiple events are marked correctly", "got update=%d, delete=%d", is_self_update, is_self_delete);

    // Test 4: Mark same event multiple times (reference counting)
    printf("Test 4: Mark same event multiple times (reference counting)\n");
    etcd_mark_pending_event(HSI_ACTION_UPDATE, 10);
    etcd_mark_pending_event(HSI_ACTION_UPDATE, 10);
    etcd_confirm_pending_event(HSI_ACTION_UPDATE, 10, 1001);
    etcd_confirm_pending_event(HSI_ACTION_UPDATE, 10, 1002);

    BOOL first_match = etcd_is_self_event(HSI_ACTION_UPDATE, 10, 1001);
    BOOL second_match = etcd_is_self_event(HSI_ACTION_UPDATE, 10, 1002);
    TEST_ASSERT(first_match == TRUE && second_match == TRUE, 
        "Reference counting works for multiple marks", "got first=%d, second=%d", first_match, second_match);

    // Test 5: Mark same event multiple times (reference counting) but not executed in sequence
    printf("Test 5: Mark same event multiple times (reference counting) but not executed in sequence\n");
    etcd_mark_pending_event(HSI_ACTION_UPDATE, 10);
    etcd_mark_pending_event(HSI_ACTION_UPDATE, 10);
    etcd_confirm_pending_event(HSI_ACTION_UPDATE, 10, 1001);
    first_match = etcd_is_self_event(HSI_ACTION_UPDATE, 10, 1001);
    etcd_confirm_pending_event(HSI_ACTION_UPDATE, 10, 1002);

    second_match = etcd_is_self_event(HSI_ACTION_UPDATE, 10, 1002);
    TEST_ASSERT(first_match == TRUE && second_match == TRUE, 
        "Reference counting works for multiple marks", "got first=%d, second=%d", first_match, second_match);

    // Test 5: Mark and confirm with different revisions
    printf("Test 5: Mark and confirm with different revisions\n");
    etcd_mark_pending_event(HSI_ACTION_UPDATE, 41);
    etcd_confirm_pending_event(HSI_ACTION_UPDATE, 41, 4100);
    BOOL is_not_self = etcd_is_self_event(HSI_ACTION_UPDATE, 41, 9999);
    TEST_ASSERT(is_not_self == FALSE, "Non-matching revision is not self-event", "got %d", is_not_self);
    // Clean up remaining event
    etcd_remove_event(HSI_ACTION_UPDATE, 41);

    // Test 6: Self-event removes from tracking after match
    printf("Test 6: Self-event removes from tracking after match\n");
    etcd_mark_pending_event(HSI_ACTION_DELETE, 42);
    etcd_confirm_pending_event(HSI_ACTION_DELETE, 42, 4200);
    BOOL first_check = etcd_is_self_event(HSI_ACTION_DELETE, 42, 4200);
    BOOL second_check = etcd_is_self_event(HSI_ACTION_DELETE, 42, 4200);
    TEST_ASSERT(first_check == TRUE && second_check == FALSE, 
        "Self-event is removed after first match", "got first=%d, second=%d", first_check, second_check);

    // Test 7: Different action types are distinguished
    printf("Test 7: Different action types are distinguished\n");
    etcd_mark_pending_event(HSI_ACTION_CREATE, 50);
    etcd_confirm_pending_event(HSI_ACTION_CREATE, 50, 5000);
    BOOL wrong_action = etcd_is_self_event(HSI_ACTION_UPDATE, 50, 5000);
    BOOL correct_action = etcd_is_self_event(HSI_ACTION_CREATE, 50, 5000);
    TEST_ASSERT(wrong_action == FALSE && correct_action == TRUE, 
        "Different actions are tracked separately", "got wrong_action=%d, correct_action=%d", wrong_action, correct_action);
}

void test_etcd_remove_event()
{
    printf("\nTesting etcd_remove_event function:\n");
    printf("=====================================\n\n");

    // Test 1: Remove a pending event
    printf("Test 1: Remove a pending event\n");
    etcd_mark_pending_event(HSI_ACTION_UPDATE, 60);
    etcd_confirm_pending_event(HSI_ACTION_UPDATE, 60, 6000);
    etcd_remove_event(HSI_ACTION_UPDATE, 60);
    BOOL is_removed = etcd_is_self_event(HSI_ACTION_UPDATE, 60, 6000);
    TEST_ASSERT(is_removed == FALSE, "Removed event is no longer self-event", "got %d", is_removed);

    // Test 2: Remove non-existent event (should not crash)
    printf("Test 2: Remove non-existent event (should not crash)\n");
    etcd_remove_event(HSI_ACTION_DELETE, 999);
    TEST_ASSERT(TRUE, "Removing non-existent event does not crash", "");

    // Test 3: Remove event with multiple references
    printf("Test 3: Remove event with multiple references\n");
    etcd_mark_pending_event(HSI_ACTION_CREATE, 70);
    etcd_mark_pending_event(HSI_ACTION_CREATE, 70);
    etcd_confirm_pending_event(HSI_ACTION_CREATE, 70, 7001);
    etcd_confirm_pending_event(HSI_ACTION_CREATE, 70, 7002);
    etcd_remove_event(HSI_ACTION_CREATE, 70);
    BOOL first_gone = etcd_is_self_event(HSI_ACTION_CREATE, 70, 7001);
    BOOL second_gone = etcd_is_self_event(HSI_ACTION_CREATE, 70, 7002);
    TEST_ASSERT(first_gone == FALSE && second_gone == FALSE, 
        "Remove clears all references for event", "got first=%d, second=%d", first_gone, second_gone);

    // Test 4: Remove event before confirmation
    printf("Test 4: Remove event before confirmation\n");
    etcd_mark_pending_event(HSI_ACTION_UPDATE, 80);
    etcd_remove_event(HSI_ACTION_UPDATE, 80);
    etcd_confirm_pending_event(HSI_ACTION_UPDATE, 80, 8000); // Should be no-op
    BOOL is_found = etcd_is_self_event(HSI_ACTION_UPDATE, 80, 8000);
    TEST_ASSERT(is_found == FALSE, "Event removed before confirmation is not found", "got %d", is_found);
}

void test_user_count_max_validation(FastRG_t *fastrg_ccb)
{
    printf("\nTesting user_count_changed_callback MaxUserCount validation:\n");
    printf("=========================================\n\n");

    U16 saved_max = fastrg_ccb->max_user_count;
    U16 saved_count = fastrg_ccb->user_count;
    fastrg_ccb->max_user_count = 10;
    fastrg_ccb->user_count = 10;

    user_count_config_t cfg = {0};

    printf("Test 1: count above configured MaxUserCount is rejected\n");
    cfg.user_count = 11;
    STATUS ret = user_count_changed_callback("test-node", &cfg,
        HSI_ACTION_UPDATE, 1, fastrg_ccb);
    TEST_ASSERT(ret == ERROR, "count above MaxUserCount returns ERROR",
        "got %d", ret);
    TEST_ASSERT(fastrg_ccb->user_count == 10,
        "local user_count unchanged after rejection",
        "got %u", fastrg_ccb->user_count);

    printf("Test 2: count equal to MaxUserCount (== current) is accepted\n");
    cfg.user_count = 10;
    ret = user_count_changed_callback("test-node", &cfg,
        HSI_ACTION_UPDATE, 2, fastrg_ccb);
    TEST_ASSERT(ret == SUCCESS, "count at MaxUserCount returns SUCCESS",
        "got %d", ret);

    printf("Test 3: non-positive count is rejected\n");
    cfg.user_count = 0;
    ret = user_count_changed_callback("test-node", &cfg,
        HSI_ACTION_UPDATE, 3, fastrg_ccb);
    TEST_ASSERT(ret == ERROR, "count of zero returns ERROR", "got %d", ret);

    fastrg_ccb->max_user_count = saved_max;
    fastrg_ccb->user_count = saved_count;
}

void test_etcd_integration(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           Etcd Integration Unit Tests                      ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_etcd_mark_pending_event();
    test_etcd_remove_event();
    test_user_count_max_validation(fastrg_ccb);

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
