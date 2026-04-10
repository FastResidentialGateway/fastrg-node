#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/queue.h>
#include <signal.h>

#include <rte_common.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_timer.h>

#include "fastrg.h"

int timer_loop(__rte_unused void *arg)
{
    rte_thread_t thread_id = rte_thread_self();
    rte_thread_set_name(thread_id, "fastrg_timer");
    uint64_t timer_resolution_cycles = fastrg_get_cycles_in_sec() / 100; /* 10ms */

    uint64_t prev_tsc = 0, cur_tsc, diff_tsc;

    while(rte_atomic16_read(&stop_flag) == 0) {
        cur_tsc = fastrg_get_cur_cycles();
        diff_tsc = cur_tsc - prev_tsc;
        if (diff_tsc > timer_resolution_cycles) {
            rte_timer_manage();
            prev_tsc = cur_tsc;
        }
    }

    return 0;
}
