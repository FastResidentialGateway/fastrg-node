#include "kafka_producer.h"
#include "proto/kafka-events.pb.h"

#include <librdkafka/rdkafka.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace ev = fastrg::events::v1;

namespace {

rd_kafka_t       *g_rk = nullptr;
std::string       g_node_uuid;
std::atomic<bool> g_ready{false};
std::atomic<unsigned long> g_dropped{0};   // messages dropped due to a full local queue

constexpr const char *KAFKA_TOPIC = "fastrg.node.events";

int64_t now_unix() { return (int64_t)std::time(nullptr); }

// Delivery-report callback: log permanent failures (rate-limited by librdkafka's
// own batching). Runs on rd_kafka_poll().
void dr_msg_cb(rd_kafka_t *, const rd_kafka_message_t *rkmessage, void *) {
    if (rkmessage->err) {
        std::fprintf(stderr, "[kafka] delivery failed: %s\n",
            rd_kafka_err2str(rkmessage->err));
    }
}

// Serialize a NodeEvent and produce it non-blocking. Drops on a full queue.
void produce_event(const ev::NodeEvent &evt) {
    if (!g_ready.load() || g_rk == nullptr)
        return;

    std::string payload;
    if (!evt.SerializeToString(&payload)) {
        std::fprintf(stderr, "[kafka] failed to serialize NodeEvent\n");
        return;
    }

    rd_kafka_resp_err_t err = rd_kafka_producev(
        g_rk,
        RD_KAFKA_V_TOPIC(KAFKA_TOPIC),
        RD_KAFKA_V_KEY(g_node_uuid.data(), g_node_uuid.size()),
        RD_KAFKA_V_VALUE(const_cast<char *>(payload.data()), payload.size()),
        RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),   // librdkafka copies; payload can go out of scope
        RD_KAFKA_V_END);

    if (err) {
        // Non-blocking: never wait for queue space; just drop + count.
        unsigned long n = ++g_dropped;
        if ((n & (n - 1)) == 0)   // log on powers of two to avoid spam
            std::fprintf(stderr, "[kafka] produce dropped (%s), total dropped=%lu\n",
                rd_kafka_err2str(err), n);
    }

    // Serve delivery reports / internal events without blocking.
    rd_kafka_poll(g_rk, 0);
}

void fill_envelope(ev::NodeEvent *evt, const char *user_id, ev::EventType type) {
    evt->set_node_uuid(g_node_uuid);
    evt->set_user_id(user_id ? user_id : "0");
    evt->set_type(type);
    evt->set_timestamp(now_unix());
}

}  // namespace

extern "C" {

STATUS kafka_producer_init(const char *brokers, const char *node_uuid) {
    if (g_ready.load())
        return SUCCESS;
    if (!brokers || brokers[0] == '\0' || !node_uuid) {
        std::fprintf(stderr, "[kafka] init skipped: no brokers/node_uuid\n");
        return ERROR;
    }

    GOOGLE_PROTOBUF_VERIFY_VERSION;

    char errstr[512];
    rd_kafka_conf_t *conf = rd_kafka_conf_new();
    if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers, errstr, sizeof(errstr))
            != RD_KAFKA_CONF_OK) {
        std::fprintf(stderr, "[kafka] conf bootstrap.servers: %s\n", errstr);
        rd_kafka_conf_destroy(conf);
        return ERROR;
    }
    // Bound the local queue so a long outage cannot grow memory without limit.
    rd_kafka_conf_set(conf, "queue.buffering.max.messages", "100000", errstr, sizeof(errstr));
    rd_kafka_conf_set_dr_msg_cb(conf, dr_msg_cb);

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::fprintf(stderr, "[kafka] failed to create producer: %s\n", errstr);
        // conf is owned by rd_kafka_new on success; on failure it is freed by it too.
        return ERROR;
    }

    g_rk = rk;
    g_node_uuid = node_uuid;
    g_ready.store(true);
    std::fprintf(stderr, "[kafka] producer initialized (brokers=%s, topic=%s)\n",
        brokers, KAFKA_TOPIC);
    return SUCCESS;
}

void kafka_producer_cleanup(void) {
    if (!g_ready.exchange(false))
        return;
    if (g_rk) {
        rd_kafka_flush(g_rk, 2000);   // best-effort flush, up to 2s
        rd_kafka_destroy(g_rk);
        g_rk = nullptr;
    }
}

int kafka_producer_is_ready(void) {
    return g_ready.load() ? 1 : 0;
}

void kafka_report_pppoe_state(const char *user_id, kafka_pppoe_phase_t phase,
    const char *hsi_ipv4, const char *hsi_ipv4_gw, const char *err_msg) {
    if (!g_ready.load())
        return;

    ev::EventType type;
    ev::PPPoEPhase pphase;
    switch (phase) {
        case KAFKA_PPPOE_CONNECTING:
            type = ev::EVENT_TYPE_PPPOE_CONNECTING;    pphase = ev::PPPOE_PHASE_CONNECTING; break;
        case KAFKA_PPPOE_CONNECTED:
            type = ev::EVENT_TYPE_PPPOE_CONNECTED;     pphase = ev::PPPOE_PHASE_CONNECTED; break;
        case KAFKA_PPPOE_DISCONNECTING:
            type = ev::EVENT_TYPE_PPPOE_DISCONNECTING; pphase = ev::PPPOE_PHASE_DISCONNECTING; break;
        case KAFKA_PPPOE_DISCONNECTED:
            type = ev::EVENT_TYPE_PPPOE_DISCONNECTED;  pphase = ev::PPPOE_PHASE_DISCONNECTED; break;
        default:
            return;
    }

    ev::NodeEvent evt;
    fill_envelope(&evt, user_id, type);
    ev::PPPoEStateChange *p = evt.mutable_pppoe_state_change();
    p->set_phase(pphase);
    if (hsi_ipv4)    p->set_hsi_ipv4(hsi_ipv4);
    if (hsi_ipv4_gw) p->set_hsi_ipv4_gw(hsi_ipv4_gw);
    if (err_msg)     p->set_error_message(err_msg);
    produce_event(evt);
}

void kafka_report_config_apply(const char *user_id, const char *action,
    BOOL success, const char *err_code, const char *err_msg) {
    if (!g_ready.load())
        return;

    ev::NodeEvent evt;
    fill_envelope(&evt, user_id,
        success == TRUE ? ev::EVENT_TYPE_CONFIG_APPLY_OK : ev::EVENT_TYPE_CONFIG_APPLY_FAIL);
    ev::ConfigApplyResult *c = evt.mutable_config_apply_result();
    if (action) c->set_action(action);
    c->set_success(success == TRUE);
    if (err_code) c->set_error_code(err_code);
    if (err_msg)  c->set_error_message(err_msg);
    produce_event(evt);
}

void kafka_report_runtime_error(const char *module, const char *err_code,
    const char *err_msg, const char *context) {
    if (!g_ready.load())
        return;

    ev::NodeEvent evt;
    fill_envelope(&evt, "0", ev::EVENT_TYPE_RUNTIME_ERROR);
    ev::RuntimeError *r = evt.mutable_runtime_error();
    if (module)  r->set_module(module);
    if (err_code) r->set_error_code(err_code);
    if (err_msg)  r->set_error_message(err_msg);
    if (context)  r->set_context(context);
    produce_event(evt);
}

}  // extern "C"
