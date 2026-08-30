#include "kafka_producer.h"
#include "kafka_producer_wal.h"
#include "kafka_retry_policy.h"
#include "etcd_client.h"
#include "config_snapshot.h"
#include "proto/kafka-events.pb.h"

#include <librdkafka/rdkafka.h>
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ev = fastrg::events::v1;

std::size_t KafkaRetryPolicy::select(const unsigned char *failed_flags,
                                    std::size_t count,
                                    std::size_t *out_indices,
                                    std::size_t max_out) {
    if (failed_flags == nullptr || out_indices == nullptr) return 0;

    std::size_t written = 0;
    for (std::size_t i = 0; i < count && written < max_out; ++i) {
        if (failed_flags[i]) out_indices[written++] = i;
    }
    return written;
}

bool KafkaRetryPolicy::is_due(std::int64_t now_sec, std::int64_t last_sec,
                              std::int64_t interval_sec) {
    if (interval_sec <= 0) return true;
    return (now_sec - last_sec) >= interval_sec;
}

namespace {

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

}  // namespace

std::string kafka_wal_serialize(const std::vector<KafkaWalEvent> &events) {
    Json::Value arr(Json::arrayValue);
    for (const auto &e : events) {
        if (!e.persistent) continue;
        Json::Value j;
        j["seq"]     = (Json::Int64)e.seq;
        j["payload"] = to_hex(e.payload);
        arr.append(j);
    }
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, arr);
}

bool kafka_wal_parse(const std::string &data, std::vector<KafkaWalEvent> &out) {
    out.clear();
    if (data.empty()) return true;   // no file, or an empty one: an empty WAL

    Json::Value arr;
    Json::Reader reader;
    if (!reader.parse(data, arr) || !arr.isArray()) return false;

    for (const auto &j : arr) {
        if (!j.isMember("seq") || !j.isMember("payload")) continue;
        // Only well-formed entries are recovered; the rest are dropped.
        if (!j["seq"].isIntegral() || !j["payload"].isString()) continue;
        KafkaWalEvent e;
        e.seq = j["seq"].asInt64();
        if (!from_hex(j["payload"].asString(), e.payload)) continue;
        e.persistent = true;   // it came out of the file, so it belongs there
        out.push_back(std::move(e));
    }
    return true;
}

void kafka_build_config_apply_result(ev::ConfigApplyResult *out,
    const char *action, BOOL success, const char *err_code, const char *err_msg,
    const char *applied_resource_version, BOOL republished,
    int64_t applied_mod_revision) {
    if (out == nullptr)
        return;

    out->Clear();
    if (action != NULL) out->set_action(action);
    out->set_success(success == TRUE);
    if (err_code != NULL) out->set_error_code(err_code);
    if (err_msg != NULL)  out->set_error_message(err_msg);
    if (applied_resource_version != NULL)
        out->set_applied_resource_version(applied_resource_version);
    out->set_republished(republished == TRUE);
    /* When config apply fails, applied_mod_revision is set to the revision that 
    failed to apply */
    out->set_applied_mod_revision(applied_mod_revision);
}

namespace {

rd_kafka_t       *g_rk = nullptr;
std::string       g_node_uuid;
std::atomic<bool> g_ready{false};
std::atomic<unsigned long> g_dropped{0};   // messages dropped (WAL bound exceeded)

constexpr const char *KAFKA_TOPIC     = "fastrg.node.events";
// Durable log of unconfirmed events; replayed on startup.
constexpr const char *KAFKA_QUEUE_PATH = "/etc/fastrg/kafka_queue.json";
// Bound the WAL so a long broker outage cannot grow it without limit.
constexpr size_t      MAX_WAL_EVENTS   = 100000;

std::mutex                 g_wal_mutex;   // guards g_pending
std::vector<KafkaWalEvent> g_pending;     // events appended but not yet confirmed
std::atomic<int64_t>       g_seq{0};      // last assigned seq

// WAL writes since start; only runtime errors reach the disk.
std::atomic<unsigned long> g_wal_writes{0};
// Guards the WAL file. Lock order: this one first, then g_wal_mutex; never reversed.
// These two locks ensure the processing of snapshot data written to disk won't stuck 
// writing snapshot data to RAM.
std::mutex                 g_wal_file_mutex;

// How often the poller re-produces events the broker never took.
constexpr int64_t          RETRY_INTERVAL_SEC = 30;

std::thread                g_poll_thread; // serves delivery reports while idle
std::atomic<bool>          g_poll_run{false};

// Which snapshot entry an offline-edit event speaks for.
struct OfflineEditAck {
    snapshot_kind_t kind;
    std::string     user_id;
    uint64_t        edit_seq;   // guards against clearing a newer edit
};

std::mutex                             g_offline_ack_mutex;
std::map<int64_t, OfflineEditAck>      g_offline_acks;   // event seq -> entry

// Remember that this event, once delivered, clears that snapshot entry.
void register_offline_ack(int64_t seq, snapshot_kind_t kind, const char *user_id,
                          uint64_t edit_seq) {
    if (seq <= 0 || user_id == nullptr) return;
    std::lock_guard<std::mutex> lk(g_offline_ack_mutex);
    g_offline_acks[seq] = OfflineEditAck{kind, std::string(user_id), edit_seq};
}

// Hand back a delivered event's binding. Caller must hold no lock.
bool take_offline_ack(int64_t seq, OfflineEditAck &out) {
    std::lock_guard<std::mutex> lk(g_offline_ack_mutex);
    auto it = g_offline_acks.find(seq);
    if (it == g_offline_acks.end()) return false;
    out = it->second;
    g_offline_acks.erase(it);
    return true;
}

// Drop an evicted event's binding; the entry stays dirty for the next tick.
void forget_offline_ack(int64_t seq) {
    std::lock_guard<std::mutex> lk(g_offline_ack_mutex);
    g_offline_acks.erase(seq);
}

int64_t now_unix() { return (int64_t)std::time(nullptr); }

// Write the durable subset out atomically (tmp + rename).
// Caller must not hold g_wal_mutex.
void persist_wal() {
    std::lock_guard<std::mutex> flk(g_wal_file_mutex);

    std::string data;
    size_t      count = 0;
    std::unique_lock<std::mutex> lk(g_wal_mutex);
    data = kafka_wal_serialize(g_pending);
    for (const auto &e : g_pending)
        if (e.persistent) count++;
    lk.unlock();   /* release before the disk write */

    std::string tmp = std::string(KAFKA_QUEUE_PATH) + ".tmp";
    std::ofstream ofs(tmp, std::ios::trunc | std::ios::binary);
    if (!ofs) {
        std::fprintf(stderr, "[kafka] failed to open WAL temp file %s\n", tmp.c_str());
        return;
    }
    ofs << data;
    ofs.flush();
    ofs.close();
    if (std::rename(tmp.c_str(), KAFKA_QUEUE_PATH) != 0) {
        std::fprintf(stderr, "[kafka] failed to rename WAL to %s\n", KAFKA_QUEUE_PATH);
        return;
    }
    std::fprintf(stderr, "[kafka] WAL written (%zu event(s), %lu write(s) since start)\n",
        count, ++g_wal_writes);
}

// Load the WAL into g_pending and restore the seq counter.
void load_wal() {
    std::string data;
    std::ifstream ifs(KAFKA_QUEUE_PATH, std::ios::binary);
    if (ifs) {                                // no file = empty WAL
        std::stringstream buf;
        buf << ifs.rdbuf();
        data = buf.str();
    }

    std::vector<KafkaWalEvent> events;
    if (!kafka_wal_parse(data, events))
        std::fprintf(stderr, "[kafka] corrupt WAL %s, ignoring\n", KAFKA_QUEUE_PATH);

    int64_t maxseq = 0;
    for (const auto &e : events)
        if (e.seq > maxseq) maxseq = e.seq;

    std::lock_guard<std::mutex> lk(g_wal_mutex);
    g_pending = std::move(events);
    g_seq.store(maxseq);
}

// Arm the retry flag for one event. Caller must not hold g_wal_mutex.
void mark_delivery_failed(int64_t seq) {
    std::lock_guard<std::mutex> lk(g_wal_mutex);
    for (auto &e : g_pending) {
        if (e.seq == seq) { e.delivery_failed = true; return; }
    }
}

// Drop a confirmed event and report whether it was durable.
// Caller must not hold g_wal_mutex.
bool take_delivered(int64_t seq) {
    std::lock_guard<std::mutex> lk(g_wal_mutex);
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        if (it->seq == seq) {
            bool was_durable = it->persistent;
            g_pending.erase(it);
            return was_durable;
        }
    }
    return false;
}

// Re-produce every buffered event after (re)start.
void replay_pending() {
    std::vector<KafkaWalEvent> snap;
    {
        std::lock_guard<std::mutex> lk(g_wal_mutex);
        snap = g_pending;
    }
    if (snap.empty()) return;
    std::fprintf(stderr, "[kafka] replaying %zu buffered event(s) from WAL\n", snap.size());
    for (const auto &e : snap) {
        rd_kafka_resp_err_t err = rd_kafka_producev(
            g_rk,
            RD_KAFKA_V_TOPIC(KAFKA_TOPIC),
            RD_KAFKA_V_KEY(g_node_uuid.data(), g_node_uuid.size()),
            RD_KAFKA_V_VALUE(const_cast<char *>(e.payload.data()), e.payload.size()),
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
            RD_KAFKA_V_OPAQUE((void *)(intptr_t)e.seq),
            RD_KAFKA_V_END);
        if (err) {
            // Nothing enqueued, so no delivery report; arm the flag instead.
            std::fprintf(stderr, "[kafka] replay enqueue deferred (seq=%lld): %s\n",
                (long long)e.seq, rd_kafka_err2str(err));
            mark_delivery_failed(e.seq);
        }
    }
    rd_kafka_poll(g_rk, 0);
}

// Delivery-report callback; runs on rd_kafka_poll / flush.
void dr_msg_cb(rd_kafka_t *, const rd_kafka_message_t *rkmessage, void *) {
    int64_t seq = (int64_t)(intptr_t)rkmessage->_private;
    if (rkmessage->err) {
        std::fprintf(stderr, "[kafka] delivery failed (seq=%lld): %s\n",
            (long long)seq, rd_kafka_err2str(rkmessage->err));
        if (seq > 0) mark_delivery_failed(seq);
        return;   // the background poller retries it
    }
    if (seq <= 0) return;

    bool was_durable = take_delivered(seq);

    if (was_durable)
        persist_wal();

    OfflineEditAck ack;
    if (take_offline_ack(seq, ack))
        config_snapshot_clear_dirty(ack.kind, ack.user_id.c_str(), ack.edit_seq);
}

// Serialize, buffer and produce a NodeEvent; never blocks.
int64_t produce_event(const ev::NodeEvent &evt) {
    if (!g_ready.load() || g_rk == nullptr)
        return 0;

    std::string payload;
    if (!evt.SerializeToString(&payload)) {
        std::fprintf(stderr, "[kafka] failed to serialize NodeEvent\n");
        return 0;
    }

    bool durable = (evt.payload_case() == ev::NodeEvent::kRuntimeError);

    int64_t seq = ++g_seq;
    int64_t evicted = 0;
    bool    evicted_durable = false;
    {
        std::lock_guard<std::mutex> lk(g_wal_mutex);
        KafkaWalEvent e;
        e.seq        = seq;
        e.payload    = payload;
        e.persistent = durable;
        g_pending.push_back(std::move(e));
        // Bound the buffer: drop the oldest past the cap.
        if (g_pending.size() > MAX_WAL_EVENTS) {
            evicted         = g_pending.front().seq;
            evicted_durable = g_pending.front().persistent;
            g_pending.erase(g_pending.begin());
            unsigned long n = ++g_dropped;
            if ((n & (n - 1)) == 0)
                std::fprintf(stderr, "[kafka] WAL full, dropped oldest; total dropped=%lu\n", n);
        }
    }
    // An evicted event is never confirmed; drop its binding.
    if (evicted != 0)
        forget_offline_ack(evicted);

    if (durable || evicted_durable)
        persist_wal();

    rd_kafka_resp_err_t err = rd_kafka_producev(
        g_rk,
        RD_KAFKA_V_TOPIC(KAFKA_TOPIC),
        RD_KAFKA_V_KEY(g_node_uuid.data(), g_node_uuid.size()),
        RD_KAFKA_V_VALUE(const_cast<char *>(payload.data()), payload.size()),
        RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),   // librdkafka copies the value
        RD_KAFKA_V_OPAQUE((void *)(intptr_t)seq),   // correlate the delivery report
        RD_KAFKA_V_END);

    if (err) {
        // Local queue full: no delivery report, so arm the retry flag here.
        std::fprintf(stderr, "[kafka] enqueue deferred (seq=%lld): %s\n",
            (long long)seq, rd_kafka_err2str(err));
        mark_delivery_failed(seq);
    }

    // Serve delivery reports without blocking.
    rd_kafka_poll(g_rk, 0);
    return seq;
}

// Hand the failed events back to the broker.
void retry_failed_deliveries() {
    std::vector<KafkaWalEvent> batch;
    {
        std::lock_guard<std::mutex> lk(g_wal_mutex);
        std::vector<unsigned char> flags(g_pending.size());
        for (size_t i = 0; i < g_pending.size(); ++i)
            flags[i] = g_pending[i].delivery_failed ? 1u : 0u;

        std::vector<size_t> hits(g_pending.size());
        size_t n = KafkaRetryPolicy::select(flags.data(), flags.size(),
                                            hits.data(), hits.size());
        batch.reserve(n);
        for (size_t k = 0; k < n; ++k) {
            batch.push_back(g_pending[hits[k]]);
            g_pending[hits[k]].delivery_failed = false;
        }
    }
    if (batch.empty()) return;

    std::fprintf(stderr, "[kafka] retrying %zu undelivered event(s)\n", batch.size());
    for (const auto &e : batch) {
        rd_kafka_resp_err_t err = rd_kafka_producev(
            g_rk,
            RD_KAFKA_V_TOPIC(KAFKA_TOPIC),
            RD_KAFKA_V_KEY(g_node_uuid.data(), g_node_uuid.size()),
            RD_KAFKA_V_VALUE(const_cast<char *>(e.payload.data()), e.payload.size()),
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
            RD_KAFKA_V_OPAQUE((void *)(intptr_t)e.seq),
            RD_KAFKA_V_END);
        if (err) {
            // The flag was cleared above; re-arm it or nothing picks it up.
            std::fprintf(stderr, "[kafka] retry enqueue deferred (seq=%lld): %s\n",
                (long long)e.seq, rd_kafka_err2str(err));
            mark_delivery_failed(e.seq);
        }
    }
    rd_kafka_poll(g_rk, 0);
}

// Serve delivery reports while idle and retry what the broker refused.
void poll_loop() {
    int64_t last_retry = now_unix();
    while (g_poll_run.load()) {
        if (!g_rk) break;
        rd_kafka_poll(g_rk, 200);
        int64_t now = now_unix();
        if (KafkaRetryPolicy::is_due(now, last_retry, RETRY_INTERVAL_SEC)) {
            last_retry = now;
            retry_failed_deliveries();
        }
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
    // Bound the in-memory queue so an outage cannot grow memory.
    rd_kafka_conf_set(conf, "queue.buffering.max.messages", "100000", errstr, sizeof(errstr));
    rd_kafka_conf_set_dr_msg_cb(conf, dr_msg_cb);

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::fprintf(stderr, "[kafka] failed to create producer: %s\n", errstr);
        // rd_kafka_new owns conf on success and frees it on failure.
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

    // Start the background poller so reports are served while idle.
    g_poll_run.store(true);
    g_poll_thread = std::thread(poll_loop);
    return SUCCESS;
}

void kafka_producer_cleanup(void) {
    if (!g_ready.exchange(false))
        return;
    g_poll_run.store(false);
    if (g_poll_thread.joinable())
        g_poll_thread.join();
    if (!g_rk)
        return;

    /* Purge queued and in-flight messages so flush returns quickly. */
    rd_kafka_purge(g_rk, RD_KAFKA_PURGE_F_QUEUE |
                         RD_KAFKA_PURGE_F_INFLIGHT |
                         RD_KAFKA_PURGE_F_NON_BLOCKING);
    rd_kafka_flush(g_rk, 1000);

    /* destroy blocks until internal threads exit; wait 2s, then detach. */
    rd_kafka_t *rk = g_rk;
    g_rk = nullptr;
    std::packaged_task<void()> destroy_task([rk] { rd_kafka_destroy(rk); });
    std::future<void> destroy_done = destroy_task.get_future();
    std::thread destroy_thread(std::move(destroy_task));
    if (destroy_done.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        destroy_thread.join();
    } else {
        std::fprintf(stderr,
            "[kafka] destroy still blocked after 2s, detaching it for process exit\n");
        destroy_thread.detach();
    }
}

int kafka_producer_is_ready(void) {
    return g_ready.load() ? 1 : 0;
}

void kafka_report_pppoe_state(const char *user_id, kafka_pppoe_phase_t phase,
    const char *hsi_ipv4, const char *hsi_ipv4_gw, const char *err_msg,
    const char *hsi_ipv6, const char *hsi_ipv6_pd_prefix,
    const char *hsi_ipv6_dns) {
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
    /* An unset field arrives empty; the controller stores it as NULL. */
    if (hsi_ipv6)           p->set_hsi_ipv6(hsi_ipv6);
    if (hsi_ipv6_pd_prefix) p->set_hsi_ipv6_pd_prefix(hsi_ipv6_pd_prefix);
    if (hsi_ipv6_dns)       p->set_hsi_ipv6_dns(hsi_ipv6_dns);
    produce_event(evt);
}

void kafka_report_config_apply_result(const char *user_id, const char *action,
    BOOL success, const char *err_code, const char *err_msg,
    const char *applied_resource_version, BOOL republished,
    int64_t applied_mod_revision) {
    if (!g_ready.load())
        return;

    ev::NodeEvent evt;
    fill_envelope(&evt, user_id,
        success == TRUE ? ev::EVENT_TYPE_CONFIG_APPLY_OK : ev::EVENT_TYPE_CONFIG_APPLY_FAIL);
    kafka_build_config_apply_result(evt.mutable_config_apply_result(), action, 
        success, err_code, err_msg, applied_resource_version, republished,
        applied_mod_revision);
    produce_event(evt);
}

int64_t kafka_report_config_offline_edit(kafka_offline_edit_kind_t kind,
    const char *user_id, const char *config_json, const char *resource_version,
    int64_t edited_at, const char *edit_summary) {
    if (!g_ready.load() || !config_json)
        return 0;

    ev::OfflineEditKind pkind;
    switch (kind) {
        case KAFKA_OFFLINE_EDIT_HSI:   pkind = ev::OFFLINE_EDIT_KIND_HSI_CONFIG; break;
        case KAFKA_OFFLINE_EDIT_DNS:   pkind = ev::OFFLINE_EDIT_KIND_DNS_RECORDS; break;
        case KAFKA_OFFLINE_EDIT_COUNT: pkind = ev::OFFLINE_EDIT_KIND_SUBSCRIBER_COUNT; break;
        default: return 0;
    }

    ev::NodeEvent evt;
    fill_envelope(&evt, user_id, ev::EVENT_TYPE_CONFIG_OFFLINE_EDIT);
    ev::ConfigOfflineEdit *o = evt.mutable_config_offline_edit();
    o->set_config_json(config_json);
    if (resource_version) o->set_resource_version(resource_version);
    o->set_edited_at(edited_at);
    if (edit_summary) o->set_edit_summary(edit_summary);
    o->set_kind(pkind);
    return produce_event(evt);
}

int64_t kafka_report_config_offline_delete(kafka_offline_edit_kind_t kind,
    const char *user_id, const char *resource_version, int64_t edited_at,
    const char *edit_summary) {
    if (!g_ready.load())
        return 0;

    ev::OfflineEditKind pkind;
    switch (kind) {
        case KAFKA_OFFLINE_EDIT_HSI:   pkind = ev::OFFLINE_EDIT_KIND_HSI_CONFIG; break;
        case KAFKA_OFFLINE_EDIT_DNS:   pkind = ev::OFFLINE_EDIT_KIND_DNS_RECORDS; break;
        case KAFKA_OFFLINE_EDIT_COUNT: pkind = ev::OFFLINE_EDIT_KIND_SUBSCRIBER_COUNT; break;
        default: return 0;
    }

    ev::NodeEvent evt;
    fill_envelope(&evt, user_id, ev::EVENT_TYPE_CONFIG_OFFLINE_EDIT);
    ev::ConfigOfflineEdit *o = evt.mutable_config_offline_edit();
    // deleted tombstone: config_json stays empty.
    o->set_deleted(true);
    if (resource_version) o->set_resource_version(resource_version);
    o->set_edited_at(edited_at);
    if (edit_summary) o->set_edit_summary(edit_summary);
    o->set_kind(pkind);
    return produce_event(evt);
}

namespace {

// Per-entry state for kafka_report_offline_edits' dirty scan.
struct OfflineReportCtx {
    // false when an entry is still pending; caller must skip any reconcile.
    bool all_ok = true;
};

void offline_report_cb(snapshot_kind_t kind, const char *user_id,
    const char *value_json, const char *resource_version, int64_t edited_at,
    const char *edit_summary, uint64_t edit_seq, void *user_data) {
    OfflineReportCtx *ctx = (OfflineReportCtx *)user_data;

    std::string key;
    kafka_offline_edit_kind_t kkind;
    switch (kind) {
        case SNAPSHOT_KIND_HSI:
            key = "configs/" + g_node_uuid + "/hsi/" + user_id;
            kkind = KAFKA_OFFLINE_EDIT_HSI;
            break;
        case SNAPSHOT_KIND_DNS:
            key = "configs/" + g_node_uuid + "/dns/" + user_id;
            kkind = KAFKA_OFFLINE_EDIT_DNS;
            break;
        case SNAPSHOT_KIND_COUNT:
            key = "user_counts/" + g_node_uuid + "/";
            kkind = KAFKA_OFFLINE_EDIT_COUNT;
            break;
        default:
            return;
    }

    char *current = NULL;
    etcd_status_t st = etcd_client_get_value(key.c_str(), &current);
    if (st != ETCD_SUCCESS && st != ETCD_KEY_NOT_FOUND) {
        // transient read failure: stay dirty, retry on the next tick
        ctx->all_ok = false;
        return;
    }

    // value_json == NULL marks a tombstone (offline delete).
    if (value_json == NULL) {
        if (st == ETCD_KEY_NOT_FOUND) {
            // Already gone in etcd: idempotent, nothing to propose.
            config_snapshot_clear_dirty(kind, user_id, edit_seq);
            free(current);
            return;
        }
        // Stays dirty until the broker confirms; the delivery report clears it.
        register_offline_ack(kafka_report_config_offline_delete(kkind, user_id,
            resource_version, edited_at, edit_summary), kind, user_id, edit_seq);
        std::fprintf(stderr, "[kafka] offline delete reported for %s (last rv=%s)\n",
            key.c_str(), resource_version ? resource_version : "");
        free(current);
        return;
    }

    if (st == ETCD_SUCCESS &&
            config_snapshot_content_equal(value_json, current) == TRUE) {
        // Identical content is never sent, but still compare-and-clear.
        config_snapshot_clear_dirty(kind, user_id, edit_seq);
        free(current);
        return;
    }

    // Key absent in etcd is still reported; arbitration discards it.
    register_offline_ack(kafka_report_config_offline_edit(kkind, user_id, value_json,
        resource_version, edited_at, edit_summary), kind, user_id, edit_seq);
    std::fprintf(stderr, "[kafka] offline edit reported for %s (rv=%s)\n",
        key.c_str(), resource_version ? resource_version : "");
    free(current);
}

void count_dirty_cb(snapshot_kind_t kind, const char *user_id,
    const char *value_json, const char *resource_version, int64_t edited_at,
    const char *edit_summary, uint64_t edit_seq, void *user_data) {
    (void)kind; (void)user_id; (void)value_json; (void)resource_version;
    (void)edited_at; (void)edit_summary; (void)edit_seq;
    (*(int *)user_data)++;
}

}  // namespace

extern "C" BOOL kafka_report_offline_edits(void) {
    int dirty = 0;
    config_snapshot_foreach_dirty(count_dirty_cb, &dirty);
    if (dirty == 0)
        return TRUE;   // nothing pending
    // Skip when etcd is unreachable; entries stay dirty for the watchdog.
    if (!etcd_client_is_connected())
        return FALSE;
    OfflineReportCtx ctx;
    config_snapshot_foreach_dirty(offline_report_cb, &ctx);
    return ctx.all_ok ? TRUE : FALSE;
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
