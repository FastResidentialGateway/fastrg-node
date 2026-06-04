#include "kafka_producer.h"
#include "proto/kafka-events.pb.h"

#include <librdkafka/rdkafka.h>
#include <json/json.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ev = fastrg::events::v1;

namespace {

rd_kafka_t       *g_rk = nullptr;
std::string       g_node_uuid;
std::atomic<bool> g_ready{false};
std::atomic<unsigned long> g_dropped{0};   // messages dropped (WAL bound exceeded)

constexpr const char *KAFKA_TOPIC     = "fastrg.node.events";
// Durable write-ahead log of events not yet confirmed delivered. Survives node
// restarts; replayed on startup so telemetry is not lost across a crash (slice 15).
constexpr const char *KAFKA_QUEUE_PATH = "/etc/fastrg/kafka_queue.json";
// Bound the WAL so a long broker outage cannot grow it without limit.
constexpr size_t      MAX_WAL_EVENTS   = 100000;

struct PendingEvent {
    int64_t     seq;       // local monotonic id, used as the per-message opaque
    std::string payload;   // serialized NodeEvent protobuf bytes
};

std::mutex                 g_wal_mutex;   // guards g_pending + the WAL file
std::vector<PendingEvent>  g_pending;     // events appended but not yet confirmed
std::atomic<int64_t>       g_seq{0};      // last assigned seq

std::thread                g_poll_thread; // serves delivery reports while idle
std::atomic<bool>          g_poll_run{false};

int64_t now_unix() { return (int64_t)std::time(nullptr); }

std::string to_hex(const std::string &in) {
    static const char *h = "0123456789abcdef";
    std::string out;
    out.reserve(in.size() * 2);
    for (unsigned char c : in) { out.push_back(h[c >> 4]); out.push_back(h[c & 0xf]); }
    return out;
}

bool from_hex(const std::string &in, std::string &out) {
    if (in.size() % 2 != 0) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(in.size() / 2);
    for (size_t i = 0; i < in.size(); i += 2) {
        int hi = nib(in[i]), lo = nib(in[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((char)((hi << 4) | lo));
    }
    return true;
}

// Persist the whole pending set atomically (tmp + rename). Caller holds g_wal_mutex.
void persist_wal_locked() {
    Json::Value arr(Json::arrayValue);
    for (const auto &e : g_pending) {
        Json::Value j;
        j["seq"]     = (Json::Int64)e.seq;
        j["payload"] = to_hex(e.payload);
        arr.append(j);
    }
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    std::string data = Json::writeString(w, arr);

    std::string tmp = std::string(KAFKA_QUEUE_PATH) + ".tmp";
    std::ofstream ofs(tmp, std::ios::trunc | std::ios::binary);
    if (!ofs) {
        std::fprintf(stderr, "[kafka] failed to open WAL temp file %s\n", tmp.c_str());
        return;
    }
    ofs << data;
    ofs.flush();
    ofs.close();
    if (std::rename(tmp.c_str(), KAFKA_QUEUE_PATH) != 0)
        std::fprintf(stderr, "[kafka] failed to rename WAL to %s\n", KAFKA_QUEUE_PATH);
}

// Load the WAL into g_pending and restore the seq counter. Called once at init.
void load_wal() {
    std::lock_guard<std::mutex> lk(g_wal_mutex);
    g_pending.clear();
    std::ifstream ifs(KAFKA_QUEUE_PATH, std::ios::binary);
    if (!ifs) return;                         // no file = empty
    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string data = buf.str();
    if (data.empty()) return;

    Json::Value arr;
    Json::Reader reader;
    if (!reader.parse(data, arr) || !arr.isArray()) {
        std::fprintf(stderr, "[kafka] corrupt WAL %s, ignoring\n", KAFKA_QUEUE_PATH);
        return;
    }
    int64_t maxseq = 0;
    for (const auto &j : arr) {
        if (!j.isMember("seq") || !j.isMember("payload")) continue;
        PendingEvent e;
        e.seq = j["seq"].asInt64();
        if (!from_hex(j["payload"].asString(), e.payload)) continue;
        if (e.seq > maxseq) maxseq = e.seq;
        g_pending.push_back(std::move(e));
    }
    g_seq.store(maxseq);
}

// Re-produce every buffered event after (re)start. Called once at init, after the
// producer is ready. Does not re-persist (entries are already in the WAL); a
// delivery report removes each one as it is confirmed.
void replay_pending() {
    std::vector<PendingEvent> snap;
    {
        std::lock_guard<std::mutex> lk(g_wal_mutex);
        snap = g_pending;
    }
    if (snap.empty()) return;
    std::fprintf(stderr, "[kafka] replaying %zu buffered event(s) from WAL\n", snap.size());
    for (const auto &e : snap) {
        rd_kafka_producev(
            g_rk,
            RD_KAFKA_V_TOPIC(KAFKA_TOPIC),
            RD_KAFKA_V_KEY(g_node_uuid.data(), g_node_uuid.size()),
            RD_KAFKA_V_VALUE(const_cast<char *>(e.payload.data()), e.payload.size()),
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
            RD_KAFKA_V_OPAQUE((void *)(intptr_t)e.seq),
            RD_KAFKA_V_END);
    }
    rd_kafka_poll(g_rk, 0);
}

// Delivery-report callback (runs on rd_kafka_poll / flush). On success, drop the
// confirmed event from the WAL; on failure keep it so it is replayed next start.
void dr_msg_cb(rd_kafka_t *, const rd_kafka_message_t *rkmessage, void *) {
    int64_t seq = (int64_t)(intptr_t)rkmessage->_private;
    if (rkmessage->err) {
        std::fprintf(stderr, "[kafka] delivery failed (seq=%lld): %s\n",
            (long long)seq, rd_kafka_err2str(rkmessage->err));
        return;   // keep in WAL for restart replay
    }
    if (seq <= 0) return;
    std::lock_guard<std::mutex> lk(g_wal_mutex);
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        if (it->seq == seq) {
            g_pending.erase(it);
            persist_wal_locked();
            break;
        }
    }
}

// Serialize a NodeEvent, append it to the durable WAL, then produce it. Never
// blocks the data/control plane. Undelivered events persist across restarts.
void produce_event(const ev::NodeEvent &evt) {
    if (!g_ready.load() || g_rk == nullptr)
        return;

    std::string payload;
    if (!evt.SerializeToString(&payload)) {
        std::fprintf(stderr, "[kafka] failed to serialize NodeEvent\n");
        return;
    }

    int64_t seq = ++g_seq;
    {
        std::lock_guard<std::mutex> lk(g_wal_mutex);
        g_pending.push_back({seq, payload});
        // Bound the WAL: drop the oldest if we exceed the cap (telemetry tolerates loss).
        if (g_pending.size() > MAX_WAL_EVENTS) {
            g_pending.erase(g_pending.begin());
            unsigned long n = ++g_dropped;
            if ((n & (n - 1)) == 0)
                std::fprintf(stderr, "[kafka] WAL full, dropped oldest; total dropped=%lu\n", n);
        }
        persist_wal_locked();
    }

    rd_kafka_resp_err_t err = rd_kafka_producev(
        g_rk,
        RD_KAFKA_V_TOPIC(KAFKA_TOPIC),
        RD_KAFKA_V_KEY(g_node_uuid.data(), g_node_uuid.size()),
        RD_KAFKA_V_VALUE(const_cast<char *>(payload.data()), payload.size()),
        RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),   // librdkafka copies the value
        RD_KAFKA_V_OPAQUE((void *)(intptr_t)seq),   // correlate the delivery report
        RD_KAFKA_V_END);

    if (err) {
        // Local queue full: the event stays in the WAL and is replayed on the next
        // start. Do not block waiting for queue space.
        std::fprintf(stderr, "[kafka] enqueue deferred (seq=%lld): %s\n",
            (long long)seq, rd_kafka_err2str(err));
    }

    // Serve delivery reports without blocking.
    rd_kafka_poll(g_rk, 0);
}

// Background poller: serve delivery reports continuously so that confirmations
// (and WAL pruning) happen even when no new events are being produced.
void poll_loop() {
    while (g_poll_run.load()) {
        if (g_rk) rd_kafka_poll(g_rk, 200);
        else      break;
    }
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
    // Bound the in-memory queue so a long outage cannot grow memory without limit.
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

    // Recover any events buffered before a previous restart and re-send them.
    load_wal();
    replay_pending();

    // Start the background poller so delivery reports are served (and the WAL
    // pruned) even when the node is idle and not producing new events.
    g_poll_run.store(true);
    g_poll_thread = std::thread(poll_loop);
    return SUCCESS;
}

void kafka_producer_cleanup(void) {
    if (!g_ready.exchange(false))
        return;
    // Stop the poller before touching g_rk so it cannot poll a destroyed handle.
    g_poll_run.store(false);
    if (g_poll_thread.joinable())
        g_poll_thread.join();
    if (g_rk) {
        rd_kafka_flush(g_rk, 2000);   // best-effort flush; dr_cb prunes the WAL
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
