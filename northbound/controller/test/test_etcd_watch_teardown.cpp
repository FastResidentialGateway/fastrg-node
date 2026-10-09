#include "../etcd_client.h"
#include "../../../src/fastrg.h"

#include <sanitizer/asan_interface.h>

#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

// Teardown test for the etcd client, built with AddressSanitizer: bring the
// client and its watchers up against etcd, stop and free it, then give the
// watchers' detached wait callbacks time to run. A callback that touches the
// freed client shows up as an ASan report. Needs etcd on 127.0.0.1:2379.

// While set, every new thread sleeps THREAD_START_DELAY before it starts.
static std::atomic<bool> delay_new_threads{false};
static std::atomic<int> delayed_threads{0};
static const std::chrono::milliseconds THREAD_START_DELAY(50);

struct DelayedStart {
    void *(*start_routine)(void *);
    void *arg;
};

static void *delayed_thread_start(void *p)
{
    DelayedStart start = *(DelayedStart *)p;
    delete (DelayedStart *)p;
    std::this_thread::sleep_for(THREAD_START_DELAY);
    return start.start_routine(start.arg);
}

extern "C" {
// etcd_client.o calls these from its watcher-thread self-event filter and its
// event hand-off; the real definitions live in src/etcd_integration.c and
// src/fastrg.c. This standalone test links only etcd_client.o, so provide
// minimal stand-ins.
int parse_user_id(const char *user_id_str, int max_count)
{
    if (!user_id_str || user_id_str[0] == '\0')
        return -1;
    char *endptr;
    long val = strtol(user_id_str, &endptr, 10);
    if (endptr == user_id_str || *endptr != '\0')
        return -1;
    int ccb_id = (int)val - 1;
    return (ccb_id < 0 || ccb_id >= max_count) ? -1 : ccb_id;
}

// No control-plane loop here: take ownership and drop the event, which is what
// the loop does once it has dispatched one.
STATUS fastrg_gen_etcd_event(FastRG_t *fastrg_ccb, etcd_event_t *ev)
{
    (void)fastrg_ccb;
    etcd_event_free(ev);
    return SUCCESS;
}

// Keep going after a report so every iteration is counted (needs
// -fsanitize-recover=address); leaks in the gRPC/etcd libraries are out of scope.
const char *__asan_default_options(void)
{
    return "halt_on_error=0:suppress_equal_pcs=0:detect_leaks=0";
}

// Overrides libc's pthread_create for this binary so delay_new_threads can
// hold back the threads etcd-cpp-api starts during teardown.
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
    void *(*start_routine)(void *), void *arg) noexcept
{
    using pthread_create_fn = int (*)(pthread_t *, const pthread_attr_t *,
        void *(*)(void *), void *);
    static pthread_create_fn real_pthread_create =
        (pthread_create_fn)dlsym(RTLD_NEXT, "pthread_create");

    if (!delay_new_threads.load())
        return real_pthread_create(thread, attr, start_routine, arg);

    DelayedStart *start = new DelayedStart{start_routine, arg};
    int ret = real_pthread_create(thread, attr, delayed_thread_start, start);
    if (ret != 0)
        delete start;
    else
        delayed_threads++;
    return ret;
}
}

static const char *ETCD_ENDPOINTS = "http://127.0.0.1:2379";
static const char *TEST_NODE_UUID = "watch-teardown-test-node";
static const int DEFAULT_ITERATIONS = 20;
// Lets the three watch streams come up before they are cancelled.
static const std::chrono::milliseconds WATCH_SETTLE(200);
// Long enough for the detached wait callbacks to have run.
static const std::chrono::milliseconds CALLBACK_GRACE(500);

static FastRG_t test_fastrg_ccb;
static std::atomic<int> asan_reports{0};

static void count_asan_report(const char *report)
{
    (void)report;
    asan_reports++;
}

static void sync_request_callback(const char *node_id, void *user_data)
{
    (void)node_id;
    (void)user_data;
}

// Runs start_watch -> stop_watch -> cleanup the given number of times and
// returns how many iterations drew an ASan report, or -1 when the scenario
// could not be set up.
static int test_etcd_client_cleanup(int iterations)
{
    int caught = 0;

    for (int i = 1; i <= iterations; i++) {
        int before = asan_reports.load();
        int delayed_before = delayed_threads.load();

        if (etcd_client_init(ETCD_ENDPOINTS, &test_fastrg_ccb) != ETCD_SUCCESS) {
            fprintf(stderr, "FAIL: iteration %d: etcd_client_init failed\n", i);
            etcd_client_cleanup();
            return -1;
        }
        // start_watch also returns success when etcd is down (it falls back to
        // background reconnects); only a reachable etcd means the watchers exist.
        if (etcd_client_start_watch(TEST_NODE_UUID, sync_request_callback) != ETCD_SUCCESS ||
                !etcd_client_is_connected()) {
            fprintf(stderr, "FAIL: iteration %d: watchers did not start (is etcd up on %s?)\n",
                i, ETCD_ENDPOINTS);
            etcd_client_stop_watch();
            etcd_client_cleanup();
            return -1;
        }
        std::this_thread::sleep_for(WATCH_SETTLE);

        // Wait callbacks run on detached threads; the delay mimics their scheduling lag under load.
        delay_new_threads = true;
        etcd_client_stop_watch();
        etcd_client_cleanup();
        delay_new_threads = false;
        std::this_thread::sleep_for(CALLBACK_GRACE);

        // No delayed thread means the override is not in effect and the test
        // would pass without exercising the race.
        if (delayed_threads.load() == delayed_before) {
            fprintf(stderr, "FAIL: iteration %d: no thread was delayed during teardown\n", i);
            return -1;
        }

        int reports = asan_reports.load() - before;
        if (reports > 0)
            caught++;
        printf("iteration %d/%d: %d ASan report(s), %d delayed thread(s)\n", i, iterations,
            reports, delayed_threads.load() - delayed_before);
        fflush(stdout);
    }
    return caught;
}

int main(int argc, char **argv)
{
    int iterations = (argc > 1) ? atoi(argv[1]) : DEFAULT_ITERATIONS;
    if (iterations <= 0)
        iterations = DEFAULT_ITERATIONS;

    __asan_set_error_report_callback(count_asan_report);
    test_fastrg_ccb.fp = stdout;
    test_fastrg_ccb.user_count = 8;

    int caught = test_etcd_client_cleanup(iterations);
    if (caught < 0)
        return 1;

    printf("etcd watch teardown: ASan caught %d of %d iterations\n", caught, iterations);
    if (caught > 0) {
        fprintf(stderr, "FAIL: a watcher callback touched the freed etcd client\n");
        return 1;
    }
    printf("PASS: no ASan report across %d iterations\n", iterations);
    return 0;
}
