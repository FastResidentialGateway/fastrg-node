#include "../etcd_client.h"
#include "../../../src/fastrg.h"

#include <etcd/Client.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// etcd_client.o calls these from its watcher-thread self-event filter; the real
// definitions live in src/etcd_integration.c. This standalone test links only
// etcd_client.o, so provide minimal stand-ins.
extern "C" {
int parse_user_id(const char *user_id_str, int max_count)
{
    (void)max_count;
    if (!user_id_str || user_id_str[0] == '\0')
        return -1;
    char *endptr;
    long val = strtol(user_id_str, &endptr, 10);
    if (endptr == user_id_str || *endptr != '\0')
        return -1;
    int ccb_id = (int)val - 1;
    return ccb_id < 0 ? -1 : ccb_id;
}

BOOL etcd_is_self_event(etcd_action_type_t action, U16 ccb_id, int64_t revision)
{
    (void)action;
    (void)ccb_id;
    (void)revision;
    return FALSE;
}

void etcd_mark_pending_event(etcd_action_type_t action, U16 ccb_id)
{
    (void)action;
    (void)ccb_id;
}

void etcd_confirm_pending_event(etcd_action_type_t action, U16 ccb_id, int64_t revision)
{
    (void)action;
    (void)ccb_id;
    (void)revision;
}

void etcd_remove_event(etcd_action_type_t action, U16 ccb_id)
{
    (void)action;
    (void)ccb_id;
}
}

struct CasContext {
    etcd::Client *client;
    std::string key;
    std::string value_prefix;
    std::string outside_prefix;
    int calls;
    int conflicts_to_inject;
    bool create_on_first_call;
    bool abort_mutation;
    std::vector<std::string> current_values;
};

static int failures = 0;

template <typename Expected, typename Actual>
static void expect_equal(const std::string& assertion, const Expected& expected, const Actual& actual)
{
    if (expected == actual)
        return;

    std::cerr << "FAIL: " << assertion << ", expected=" << expected << ", actual=" << actual << std::endl;
    failures++;
}

static void expect_true(const std::string& assertion, bool actual)
{
    if (actual)
        return;

    std::cerr << "FAIL: " << assertion << ", expected=true, actual=false" << std::endl;
    failures++;
}

static STATUS mutate_value(const char *current_json, char **out_value, void *user_data)
{
    CasContext *ctx = static_cast<CasContext *>(user_data);
    ctx->calls++;
    ctx->current_values.emplace_back(current_json ? current_json : "<NULL>");

    if (ctx->abort_mutation)
        return ERROR;

    if (ctx->calls <= ctx->conflicts_to_inject) {
        std::string outside_value = ctx->outside_prefix + std::to_string(ctx->calls);
        etcd::Response response = ctx->create_on_first_call && ctx->calls == 1
            ? ctx->client->add(ctx->key, outside_value).get()
            : ctx->client->set(ctx->key, outside_value).get();
        if (response.error_code() != 0)
            return ERROR;
    }

    std::string new_value = ctx->value_prefix + std::to_string(ctx->calls);
    *out_value = strdup(new_value.c_str());
    return *out_value ? SUCCESS : ERROR;
}

static void remove_key(etcd::Client& client, const std::string& key)
{
    client.rm(key).get();
}

static std::string get_value(etcd::Client& client, const std::string& key, const std::string& assertion)
{
    etcd::Response response = client.get(key).get();
    expect_equal(assertion + " error code", 0, response.error_code());
    return response.error_code() == 0 ? response.value().as_string() : "";
}

static std::string current_value_at(const CasContext& ctx, size_t index)
{
    return index < ctx.current_values.size() ? ctx.current_values[index] : "<MISSING>";
}

static void test_create_if_absent(etcd::Client& client)
{
    std::cout << "Case 1: create-if-absent without conflict" << std::endl;
    const std::string key = "unit_test/cas/create_if_absent";
    remove_key(client, key);
    CasContext ctx{&client, key, "case1-cas-", "case1-outside-", 0, 0, false, false, {}};
    int64_t revision = 0;

    etcd_status_t status = etcd_client_cas_put(key.c_str(), mutate_value, &ctx, &revision);

    expect_equal("case 1 status", ETCD_SUCCESS, status);
    expect_equal("case 1 mutate calls", 1, ctx.calls);
    expect_equal("case 1 initial value", std::string("<NULL>"), current_value_at(ctx, 0));
    expect_true("case 1 revision is positive", revision > 0);
    expect_equal("case 1 final value", std::string("case1-cas-1"), get_value(client, key, "case 1 get"));
    remove_key(client, key);
}

static void test_compare_failed_retry(etcd::Client& client)
{
    std::cout << "Case 2: compare-failed conflict retries successfully" << std::endl;
    const std::string key = "unit_test/cas/compare_failed";
    remove_key(client, key);
    expect_equal("case 2 setup", 0, client.set(key, "case2-initial").get().error_code());
    CasContext ctx{&client, key, "case2-cas-", "case2-outside-", 0, 1, false, false, {}};

    etcd_status_t status = etcd_client_cas_put(key.c_str(), mutate_value, &ctx, nullptr);

    expect_equal("case 2 status", ETCD_SUCCESS, status);
    expect_equal("case 2 mutate calls", 2, ctx.calls);
    expect_equal("case 2 retry current value", std::string("case2-outside-1"), current_value_at(ctx, 1));
    expect_equal("case 2 final value", std::string("case2-cas-2"), get_value(client, key, "case 2 get"));
    remove_key(client, key);
}

static void test_key_already_exists_retry(etcd::Client& client)
{
    std::cout << "Case 3: key-already-exists conflict retries successfully" << std::endl;
    const std::string key = "unit_test/cas/key_already_exists";
    remove_key(client, key);
    CasContext ctx{&client, key, "case3-cas-", "case3-outside-", 0, 1, true, false, {}};

    etcd_status_t status = etcd_client_cas_put(key.c_str(), mutate_value, &ctx, nullptr);

    expect_equal("case 3 status", ETCD_SUCCESS, status);
    expect_equal("case 3 mutate calls", 2, ctx.calls);
    expect_equal("case 3 initial value", std::string("<NULL>"), current_value_at(ctx, 0));
    expect_equal("case 3 retry current value", std::string("case3-outside-1"), current_value_at(ctx, 1));
    expect_equal("case 3 final value", std::string("case3-cas-2"), get_value(client, key, "case 3 get"));
    remove_key(client, key);
}

static void test_retry_exhaustion(etcd::Client& client)
{
    std::cout << "Case 4: conflicts exhaust retries with exponential backoff" << std::endl;
    const std::string key = "unit_test/cas/retry_exhaustion";
    remove_key(client, key);
    expect_equal("case 4 setup", 0, client.set(key, "case4-initial").get().error_code());
    CasContext ctx{&client, key, "case4-cas-", "case4-outside-", 0, 5, false, false, {}};
    auto start = std::chrono::steady_clock::now();

    etcd_status_t status = etcd_client_cas_put(key.c_str(), mutate_value, &ctx, nullptr);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    expect_equal("case 4 status", ETCD_CAS_CONFLICT, status);
    expect_equal("case 4 mutate calls", 5, ctx.calls);
    expect_true("case 4 elapsed time >= 1550ms", elapsed_ms >= 1550);
    expect_true("case 4 elapsed time < 30000ms", elapsed_ms < 30000);
    expect_equal("case 4 final value", std::string("case4-outside-5"), get_value(client, key, "case 4 get"));
    remove_key(client, key);
}

static void test_mutate_abort(etcd::Client& client)
{
    std::cout << "Case 5: mutate error aborts without writing" << std::endl;
    const std::string key = "unit_test/cas/mutate_abort";
    remove_key(client, key);
    expect_equal("case 5 setup", 0, client.set(key, "case5-initial").get().error_code());
    CasContext ctx{&client, key, "case5-cas-", "case5-outside-", 0, 0, false, true, {}};

    etcd_status_t status = etcd_client_cas_put(key.c_str(), mutate_value, &ctx, nullptr);

    expect_equal("case 5 status", ETCD_ERROR, status);
    expect_equal("case 5 mutate calls", 1, ctx.calls);
    expect_equal("case 5 final value", std::string("case5-initial"), get_value(client, key, "case 5 get"));
    remove_key(client, key);
}

int main()
{
    const char *endpoint = "http://127.0.0.1:2379";
    FastRG_t fastrg_ccb{};
    fastrg_ccb.fp = stderr;

    etcd_status_t init_status = etcd_client_init(endpoint, &fastrg_ccb);
    if (init_status != ETCD_SUCCESS) {
        std::cerr << "FAIL: etcd client initialization, expected=" << ETCD_SUCCESS
                  << ", actual=" << init_status << std::endl;
        return 1;
    }

    etcd::Client outside_client(endpoint);
    test_create_if_absent(outside_client);
    test_compare_failed_retry(outside_client);
    test_key_already_exists_retry(outside_client);
    test_retry_exhaustion(outside_client);
    test_mutate_abort(outside_client);
    etcd_client_cleanup();

    if (failures != 0) {
        std::cerr << failures << " CAS assertion(s) failed" << std::endl;
        return 1;
    }

    std::cout << "All etcd CAS put tests passed" << std::endl;
    return 0;
}
