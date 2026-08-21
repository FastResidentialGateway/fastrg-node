#include "kafka_producer.h"
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
    // Set when an attempt to hand this event to the broker failed. In memory
    // only: a restart replays the whole WAL anyway, so persisting it would add
    // a field to the file format for no gain.
    bool        delivery_failed = false;
};

std::mutex                 g_wal_mutex;   // guards g_pending + the WAL file
std::vector<PendingEvent>  g_pending;     // events appended but not yet confirmed
std::atomic<int64_t>       g_seq{0};      // last assigned seq

// How often the background poller re-produces events the broker never took.
// Long enough that a broker outage does not turn into a produce loop.
constexpr int64_t          RETRY_INTERVAL_SEC = 30;

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

// Arm the retry flag for one event, so the background poller produces it again.
// Every caller reaches this without holding the WAL lock, which is what makes
// taking it here safe.
void mark_delivery_failed(int64_t seq) {
    std::lock_guard<std::mutex> lk(g_wal_mutex);
    for (auto &e : g_pending) {
        if (e.seq == seq) { e.delivery_failed = true; return; }
    }
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
        rd_kafka_resp_err_t err = rd_kafka_producev(
            g_rk,
            RD_KAFKA_V_TOPIC(KAFKA_TOPIC),
            RD_KAFKA_V_KEY(g_node_uuid.data(), g_node_uuid.size()),
            RD_KAFKA_V_VALUE(const_cast<char *>(e.payload.data()), e.payload.size()),
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
            RD_KAFKA_V_OPAQUE((void *)(intptr_t)e.seq),
            RD_KAFKA_V_END);
        if (err) {
            // A full local queue is likely here: a WAL at its cap holds as many
            // events as the queue accepts. Nothing was enqueued, so no delivery
            // report will arrive; arm the flag so the poller comes back to it.
            std::fprintf(stderr, "[kafka] replay enqueue deferred (seq=%lld): %s\n",
                (long long)e.seq, rd_kafka_err2str(err));
            mark_delivery_failed(e.seq);
        }
    }
    rd_kafka_poll(g_rk, 0);
}

// Delivery-report callback (runs on rd_kafka_poll / flush). On success, drop the
// confirmed event from the WAL; on failure keep it and arm its retry flag, so
// the background poller produces it again without waiting for a restart.
void dr_msg_cb(rd_kafka_t *, const rd_kafka_message_t *rkmessage, void *) {
    int64_t seq = (int64_t)(intptr_t)rkmessage->_private;
    if (rkmessage->err) {
        std::fprintf(stderr, "[kafka] delivery failed (seq=%lld): %s\n",
            (long long)seq, rd_kafka_err2str(rkmessage->err));
        if (seq > 0) mark_delivery_failed(seq);
        return;   // the background poller retries it
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
        // Local queue full: no delivery report will ever arrive for this event,
        // so arm the retry flag here instead. Do not block waiting for space.
        std::fprintf(stderr, "[kafka] enqueue deferred (seq=%lld): %s\n",
            (long long)seq, rd_kafka_err2str(err));
        mark_delivery_failed(seq);
    }

    // Serve delivery reports without blocking.
    rd_kafka_poll(g_rk, 0);
}

// Hand the failed events back to the broker. The flag is cleared before the
// attempt and re-armed by whichever path reports the failure -- the delivery
// report when the broker refuses it, or the enqueue below when it never gets
// that far -- so the event stays in the WAL until it is confirmed.
void retry_failed_deliveries() {
    std::vector<PendingEvent> batch;
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
            // The queue is most likely still full from the outage this retry is
            // recovering from. The flag was cleared above, so re-arm it here or
            // the event would sit in the WAL with nothing left to pick it up.
            std::fprintf(stderr, "[kafka] retry enqueue deferred (seq=%lld): %s\n",
                (long long)e.seq, rd_kafka_err2str(err));
            mark_delivery_failed(e.seq);
        }
    }
    rd_kafka_poll(g_rk, 0);
}

// Background poller: serve delivery reports continuously so that confirmations
// (and WAL pruning) happen even when no new events are being produced, and
// retry what the broker never took so an outage does not strand events until
// the next restart.
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
    g_poll_run.store(false);
    if (g_poll_thread.joinable())
        g_poll_thread.join();
    if (!g_rk)
        return;

    /* Cancel all queued and in-flight messages immediately so flush returns
     * quickly even when the broker is unreachable. */
    rd_kafka_purge(g_rk, RD_KAFKA_PURGE_F_QUEUE |
                         RD_KAFKA_PURGE_F_INFLIGHT |
                         RD_KAFKA_PURGE_F_NON_BLOCKING);
    rd_kafka_flush(g_rk, 1000);

    /* rd_kafka_destroy() blocks until all internal threads exit.  When the
     * broker is unreachable the reconnect-backoff thread may sleep for up to
     * reconnect.backoff.max.ms before it notices the terminate signal, so
     * shutdown must never be gated on it unconditionally.  In the normal case
     * destroy finishes almost immediately, though, and a detached thread racing
     * process exit (atexit handlers, static destructors) can crash during
     * teardown.  Wait up to 2 seconds for the common fast path and only fall
     * back to detaching when destroy is actually stuck — the process is
     * exiting and the OS cleans up any leftover handle. */
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
    /* An unset field arrives as an empty string, which the controller reads as
     * "not reported" and stores as NULL. */
    if (hsi_ipv6)           p->set_hsi_ipv6(hsi_ipv6);
    if (hsi_ipv6_pd_prefix) p->set_hsi_ipv6_pd_prefix(hsi_ipv6_pd_prefix);
    if (hsi_ipv6_dns)       p->set_hsi_ipv6_dns(hsi_ipv6_dns);
    produce_event(evt);
}

void kafka_report_config_apply(const char *user_id, const char *action,
    BOOL success, const char *err_code, const char *err_msg,
    const char *applied_resource_version) {
    if (!g_ready.load())
        return;

    ev::NodeEvent evt;
    fill_envelope(&evt, user_id,
        success == TRUE ? ev::EVENT_TYPE_CONFIG_APPLY_OK : ev::EVENT_TYPE_CONFIG_APPLY_FAIL);
    ev::ConfigApplyResult *c = evt.mutable_config_apply_result();
    if (action != NULL) c->set_action(action);
    c->set_success(success == TRUE);
    if (err_code != NULL) c->set_error_code(err_code);
    if (err_msg != NULL)  c->set_error_message(err_msg);
    if (applied_resource_version != NULL)
        c->set_applied_resource_version(applied_resource_version);
    produce_event(evt);
}

void kafka_report_config_offline_edit(kafka_offline_edit_kind_t kind,
    const char *user_id, const char *config_json, const char *resource_version,
    int64_t edited_at, const char *edit_summary) {
    if (!g_ready.load() || !config_json)
        return;

    ev::OfflineEditKind pkind;
    switch (kind) {
        case KAFKA_OFFLINE_EDIT_HSI:   pkind = ev::OFFLINE_EDIT_KIND_HSI_CONFIG; break;
        case KAFKA_OFFLINE_EDIT_DNS:   pkind = ev::OFFLINE_EDIT_KIND_DNS_RECORDS; break;
        case KAFKA_OFFLINE_EDIT_COUNT: pkind = ev::OFFLINE_EDIT_KIND_SUBSCRIBER_COUNT; break;
        default: return;
    }

    ev::NodeEvent evt;
    fill_envelope(&evt, user_id, ev::EVENT_TYPE_CONFIG_OFFLINE_EDIT);
    ev::ConfigOfflineEdit *o = evt.mutable_config_offline_edit();
    o->set_config_json(config_json);
    if (resource_version) o->set_resource_version(resource_version);
    o->set_edited_at(edited_at);
    if (edit_summary) o->set_edit_summary(edit_summary);
    o->set_kind(pkind);
    produce_event(evt);
}

void kafka_report_config_offline_delete(kafka_offline_edit_kind_t kind,
    const char *user_id, const char *resource_version, int64_t edited_at,
    const char *edit_summary) {
    if (!g_ready.load())
        return;

    ev::OfflineEditKind pkind;
    switch (kind) {
        case KAFKA_OFFLINE_EDIT_HSI:   pkind = ev::OFFLINE_EDIT_KIND_HSI_CONFIG; break;
        case KAFKA_OFFLINE_EDIT_DNS:   pkind = ev::OFFLINE_EDIT_KIND_DNS_RECORDS; break;
        case KAFKA_OFFLINE_EDIT_COUNT: pkind = ev::OFFLINE_EDIT_KIND_SUBSCRIBER_COUNT; break;
        default: return;
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
    produce_event(evt);
}

namespace {

// Per-entry state for kafka_report_offline_edits' dirty scan.
struct OfflineReportCtx {
    // false when any dirty entry could not be handled (transient etcd read
    // failure) and is still pending. The caller must then SKIP any
    // snapshot-refreshing reconcile this round: mirror writes clear the dirty
    // flag unconditionally and would silently swallow the pending proposal
    // (report-before-mirror invariant).
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
        kafka_report_config_offline_delete(kkind, user_id,
            resource_version, edited_at, edit_summary);
        config_snapshot_clear_dirty(kind, user_id, edit_seq);
        std::fprintf(stderr, "[kafka] offline delete reported for %s (last rv=%s)\n",
            key.c_str(), resource_version ? resource_version : "");
        free(current);
        return;
    }

    if (st == ETCD_SUCCESS &&
            config_snapshot_content_equal(value_json, current) == TRUE) {
        // Identical content is never sent. Still compare-and-clear: an edit
        // that landed after the dirty copy must survive this clear too.
        config_snapshot_clear_dirty(kind, user_id, edit_seq);
        free(current);
        return;
    }

    // Key absent in etcd is still reported; the controller's arbitration
    // discards edits whose key was deleted.
    kafka_report_config_offline_edit(kkind, user_id, value_json,
        resource_version, edited_at, edit_summary);
    config_snapshot_clear_dirty(kind, user_id, edit_seq);
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
    // Skip when etcd is unreachable: every per-entry diff read would just
    // burn a connect timeout. Entries stay dirty and the etcd watchdog calls
    // back in after reconnection.
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
