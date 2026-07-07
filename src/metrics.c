/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  METRICS.C

  Prometheus text exposition for a FastRG node. See metrics.h. Runs on the
  lighthttp metrics thread (a plain pthread, not an EAL lcore).

  RCU: ppp_ccb / dhcp_ccb / per_subscriber_stats are resized at runtime and
  protected by QSBR. The metrics thread uses its own dedicated reader id (not
  the lcore-0 slot the gRPC thread borrows). Each gather copies the needed
  values into local arrays inside a short read-side section; formatting and the
  socket write happen after the section closes, so a slow client never holds a
  grace period open against a subscriber-array resize.
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <inttypes.h>
#include <time.h>

#include <rte_ethdev.h>
#include <rte_atomic.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_rcu_qsbr.h>
#include <rte_lcore.h>

#include "fastrg.h"
#include "pppd/pppd.h"
#include "pppd/header.h"
#include "dhcpd/dhcpd.h"
#include "metrics.h"

/*
 * Dedicated RCU reader slot for the metrics thread. Avoids sharing the lcore-0
 * slot the gRPC server thread borrows for non-EAL reads. RTE_MAX_LCORE-1 is
 * well above the lcore ids this app assigns.
 */
#define METRICS_RCU_THREAD_ID (RTE_MAX_LCORE - 1)

#define MAX_MEMPOOLS 64

/* ----- gathered rows (copied out of RCU sections before formatting) ----- */
struct per_user_row {
    uint64_t rxp, rxb, txp, txb, dp, db;
};
struct ppp_row {
    uint32_t user_id;
    uint64_t rxp, rxb, txp, txb;
    uint64_t nat_used, nat_enospc, nat_gc_reclaimed;
};
struct dhcp_row {
    uint32_t user_id;
    uint64_t cur, max;
    int      configured;
};
struct lcore_row {
    uint32_t id;
    const char *role;
    uint64_t busy, total;
};
struct heap_row {
    int socket_id;
    uint64_t total, used, free, largest;
};
struct mp_row {
    char name[RTE_MEMPOOL_NAMESIZE];
    uint32_t size, avail, in_use;
};
struct mp_ctx {
    struct mp_row *rows;
    int n;
    int cap;
};

void metrics_rcu_register(void *arg)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)arg;
    rte_rcu_qsbr_thread_register(fastrg_ccb->per_subscriber_stats_rcu, METRICS_RCU_THREAD_ID);
    rte_rcu_qsbr_thread_register(fastrg_ccb->ppp_ccb_rcu, METRICS_RCU_THREAD_ID);
    rte_rcu_qsbr_thread_register(fastrg_ccb->dhcp_ccb_rcu, METRICS_RCU_THREAD_ID);
}

/* Escape a Prometheus label value ( \, ", \n ) into dst. Returns dst. */
static const char *esc(const char *s, char *dst, size_t dstsz)
{
    size_t j = 0;
    if (s == NULL) {
        if (dstsz)
            dst[0] = '\0';
        return dst;
    }
    for(size_t i=0; s[i]!='\0' && j+2<dstsz; i++) {
        char c = s[i];
        if (c == '\\' || c == '"') {
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c == '\n') {
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    return dst;
}

static void emit_header(lighthttp_buf_t *o, const char *name, const char *type, const char *help)
{
    lighthttp_buf_appendf(o, "# HELP %s %s\n# TYPE %s %s\n", name, help, name, type);
}

static void mp_collect(struct rte_mempool *mp, void *arg)
{
    struct mp_ctx *c = (struct mp_ctx *)arg;
    if (c->n >= c->cap)
        return;
    struct mp_row *r = &c->rows[c->n++];
    snprintf(r->name, sizeof(r->name), "%s", mp->name);
    r->size = mp->size;
    r->avail = rte_mempool_avail_count(mp);
    r->in_use = rte_mempool_in_use_count(mp);
}

static int memseg_sum(const struct rte_memseg_list *msl, const struct rte_memseg *ms, void *arg)
{
    (void)msl;
    *(uint64_t *)arg += ms->len;
    return 0;
}

static void mac_str(const struct rte_ether_addr *m, char *b, size_t bsz)
{
    snprintf(b, bsz, "%02x:%02x:%02x:%02x:%02x:%02x",
        m->addr_bytes[0], m->addr_bytes[1], m->addr_bytes[2],
        m->addr_bytes[3], m->addr_bytes[4], m->addr_bytes[5]);
}

/* per-NIC-counter metric descriptor (offset into struct rte_eth_stats) */
struct nic_metric {
    const char *name, *help;
    size_t off;
};
/* per-subscriber / per-ppp metric descriptor (offset into the gathered row) */
struct row_metric {
    const char *name, *help;
    size_t off;
};

int metrics_build(lighthttp_buf_t *out, const char **content_type, void *arg)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)arg;
    *content_type = "text/plain; version=0.0.4; charset=utf-8";

    char ebuf[256];
    char uuid[64];
    esc(fastrg_ccb->node_uuid ? fastrg_ccb->node_uuid : "", uuid, sizeof(uuid));
    const U16 user_count = fastrg_ccb->user_count;

    /* ---------- gather: NIC port stats ---------- */
    struct rte_eth_stats eth[PORT_AMOUNT];
    int eth_ok[PORT_AMOUNT];
    for(int p=0; p<PORT_AMOUNT; p++) {
        memset(&eth[p], 0, sizeof(eth[p]));
        eth_ok[p] = (rte_eth_stats_get(p, &eth[p]) == 0);
    }

    /* ---------- gather: per-subscriber stats (RCU) ---------- */
    struct per_user_row *per_user[PORT_AMOUNT] = {NULL};
    struct per_user_row unknown_user[PORT_AMOUNT];
    memset(unknown_user, 0, sizeof(unknown_user));
    for(int p=0; p<PORT_AMOUNT; p++) {
        if (user_count > 0)
            per_user[p] = calloc(user_count, sizeof(struct per_user_row));
    }
    rte_rcu_qsbr_thread_online(fastrg_ccb->per_subscriber_stats_rcu, METRICS_RCU_THREAD_ID);
    /* C helper sums each subscriber across lcores; here we only copy into the
     * gathered rows. Index user_count is the unknown-user slot. */
    for(int p=0; p<PORT_AMOUNT; p++) {
        struct per_ccb_stats sum;
        for(U16 i=0; i<user_count && per_user[p] != NULL; i++) {
            fastrg_sum_subscriber_stats(fastrg_ccb, p, i, &sum);
            per_user[p][i].rxp = sum.rx_packets;
            per_user[p][i].rxb = sum.rx_bytes;
            per_user[p][i].txp = sum.tx_packets;
            per_user[p][i].txb = sum.tx_bytes;
            per_user[p][i].dp  = sum.dropped_packets;
            per_user[p][i].db  = sum.dropped_bytes;
        }
        fastrg_sum_subscriber_stats(fastrg_ccb, p, user_count, &sum);
        unknown_user[p].rxp = sum.rx_packets;
        unknown_user[p].rxb = sum.rx_bytes;
        unknown_user[p].txp = sum.tx_packets;
        unknown_user[p].txb = sum.tx_bytes;
        unknown_user[p].dp  = sum.dropped_packets;
        unknown_user[p].db  = sum.dropped_bytes;
    }
    rte_rcu_qsbr_quiescent(fastrg_ccb->per_subscriber_stats_rcu, METRICS_RCU_THREAD_ID);
    rte_rcu_qsbr_thread_offline(fastrg_ccb->per_subscriber_stats_rcu, METRICS_RCU_THREAD_ID);

    /* ---------- gather: PPPoE sessions (RCU) ---------- */
    /* phase tallies: data, ipcp, auth, lcp, init, terminated, not_configured, error */
    uint64_t phase[8] = {0};
    struct ppp_row *ppp = (user_count > 0) ? calloc(user_count, sizeof(struct ppp_row)) : NULL;
    rte_rcu_qsbr_thread_online(fastrg_ccb->ppp_ccb_rcu, METRICS_RCU_THREAD_ID);
    {
        void **a = __atomic_load_n(&fastrg_ccb->ppp_ccb, __ATOMIC_ACQUIRE);
        for(U16 i=0; i<user_count; i++) {
            ppp_ccb_t *c = a ? (ppp_ccb_t *)a[i] : NULL;
            if (ppp)
                ppp[i].user_id = i + 1;
            if (c == NULL) {
                phase[6]++; /* not configured */
                continue;
            }
            switch (c->phase) {
                case DATA_PHASE:     phase[0]++; break;
                case IPCP_PHASE:     phase[1]++; break;
                case AUTH_PHASE:     phase[2]++; break;
                case LCP_PHASE:      phase[3]++; break;
                case PPPOE_PHASE:    phase[4]++; break;
                case END_PHASE:      phase[5]++; break;
                case NOT_CONFIGURED: phase[6]++; break;
                default:             phase[7]++; break;
            }
            if (ppp) {
                /* C helper sums this session across lcores (ccb_id == i), within
                 * the surrounding ppp_ccb_rcu online section. */
                struct pppoes_lcore_stats sum;
                fastrg_sum_pppoes_stats(fastrg_ccb, i, &sum);
                ppp[i].rxp = sum.rx_packets;
                ppp[i].rxb = sum.rx_bytes;
                ppp[i].txp = sum.tx_packets;
                ppp[i].txb = sum.tx_bytes;
                /* NAT pool health: fill is a live gauge derived from the
                 * free ring; the counters are RELAXED single-word reads. */
                if (c->nat_free_ring != NULL)
                    ppp[i].nat_used = MAX_NAT_ENTRIES - rte_ring_count(c->nat_free_ring);
                ppp[i].nat_enospc = __atomic_load_n(&c->nat_enospc, __ATOMIC_RELAXED);
                ppp[i].nat_gc_reclaimed = __atomic_load_n(&c->nat_gc_reclaimed, __ATOMIC_RELAXED);
            }
        }
    }
    rte_rcu_qsbr_quiescent(fastrg_ccb->ppp_ccb_rcu, METRICS_RCU_THREAD_ID);
    rte_rcu_qsbr_thread_offline(fastrg_ccb->ppp_ccb_rcu, METRICS_RCU_THREAD_ID);

    /* ---------- gather: DHCP servers (RCU) ---------- */
    uint64_t dhcp_running = 0, dhcp_stopped = 0, dhcp_notcfg = 0;
    struct dhcp_row *dhcp = (user_count > 0) ? calloc(user_count, sizeof(struct dhcp_row)) : NULL;
    rte_rcu_qsbr_thread_online(fastrg_ccb->dhcp_ccb_rcu, METRICS_RCU_THREAD_ID);
    {
        void **a = __atomic_load_n(&fastrg_ccb->dhcp_ccb, __ATOMIC_ACQUIRE);
        for(U16 i=0; i<user_count; i++) {
            dhcp_ccb_t *c = a ? (dhcp_ccb_t *)a[i] : NULL;
            int configured = 0;
            uint64_t cur = 0, max = 0;
            if (c != NULL && c->per_lan_user_pool != NULL && c->per_lan_user_pool_len > 0 &&
                c->per_lan_user_pool[0]->ip_pool.ip_addr != 0 &&
                c->per_lan_user_pool[c->per_lan_user_pool_len - 1]->ip_pool.ip_addr != 0) {
                configured = 1;
                max = c->per_lan_user_pool_len;
                for(U32 j=0; j<c->per_lan_user_pool_len; j++) {
                    if (c->per_lan_user_pool[j]->ip_pool.used)
                        cur++;
                }
            }
            if (!configured)
                dhcp_notcfg++;
            else if (rte_atomic16_read(&c->dhcp_bool) == 1)
                dhcp_running++;
            else
                dhcp_stopped++;
            if (dhcp) {
                dhcp[i].user_id = i + 1;
                dhcp[i].cur = cur;
                dhcp[i].max = max;
                dhcp[i].configured = configured;
            }
        }
    }
    rte_rcu_qsbr_quiescent(fastrg_ccb->dhcp_ccb_rcu, METRICS_RCU_THREAD_ID);
    rte_rcu_qsbr_thread_offline(fastrg_ccb->dhcp_ccb_rcu, METRICS_RCU_THREAD_ID);

    /* ---------- gather: lcore usage ---------- */
    struct lcore_row lcores[RTE_MAX_LCORE];
    int n_lcores = 0;
    if (fastrg_ccb->lcore_usage) {
        U8 ids[3 + 2 * MAX_DATA_QUEUES];
        int n_ids = 0;
        ids[n_ids++] = fastrg_ccb->lcore.ctrl_thread;
        ids[n_ids++] = fastrg_ccb->lcore.wan_ctrl_thread;
        ids[n_ids++] = fastrg_ccb->lcore.lan_ctrl_thread;
        for(int i=0; i<fastrg_ccb->lcore.num_data_queues; i++) {
            ids[n_ids++] = fastrg_ccb->lcore.wan_data_threads[i];
            ids[n_ids++] = fastrg_ccb->lcore.lan_data_threads[i];
        }
        for(int k=0; k<n_ids && n_lcores<RTE_MAX_LCORE; k++) {
            U8 id = ids[k];
            lcores[n_lcores].id = id;
            lcores[n_lcores].role = __atomic_load_n(&fastrg_ccb->lcore_usage[id].role, __ATOMIC_RELAXED);
            lcores[n_lcores].busy = __atomic_load_n(&fastrg_ccb->lcore_usage[id].busy_cycles, __ATOMIC_RELAXED);
            lcores[n_lcores].total = __atomic_load_n(&fastrg_ccb->lcore_usage[id].total_cycles, __ATOMIC_RELAXED);
            n_lcores++;
        }
    }

    /* ---------- gather: heap / mempool / hugepage ---------- */
    struct heap_row heaps[RTE_MAX_NUMA_NODES];
    int n_heaps = 0;
    for(int sid=0; sid<RTE_MAX_NUMA_NODES; sid++) {
        struct rte_malloc_socket_stats ms;
        if (rte_malloc_get_socket_stats(sid, &ms) == 0) {
            heaps[n_heaps].socket_id = sid;
            heaps[n_heaps].total = ms.heap_totalsz_bytes;
            heaps[n_heaps].used = ms.heap_allocsz_bytes;
            heaps[n_heaps].free = ms.heap_freesz_bytes;
            heaps[n_heaps].largest = ms.greatest_free_size;
            n_heaps++;
        }
    }
    struct mp_row mp_rows[MAX_MEMPOOLS];
    struct mp_ctx mpc = { mp_rows, 0, MAX_MEMPOOLS };
    rte_mempool_walk(mp_collect, &mpc);
    uint64_t hugepage_pinned = 0;
    rte_memseg_walk(memseg_sum, &hugepage_pinned);

    /* ================= emit (one metric family fully before the next) ====== */

    emit_header(out, "fastrg_node_start_time_seconds", "gauge",
        "Unix time (seconds) the fastrg process started; changes on restart.");
    lighthttp_buf_appendf(out, "fastrg_node_start_time_seconds{node_uuid=\"%s\"} %" PRIu64 "\n",
        uuid, fastrg_ccb->node_start_time);

    emit_header(out, "fastrg_node_restart_total", "counter",
        "Cumulative fastrg process start count, persisted across restarts.");
    lighthttp_buf_appendf(out, "fastrg_node_restart_total{node_uuid=\"%s\"} %" PRIu64 "\n",
        uuid, fastrg_ccb->node_restart_total);

    /* ---- NIC port counters ---- */
    static const struct nic_metric nic_metrics[] = {
        {"fastrg_node_rx_packets_total", "Total received packets per NIC port.", offsetof(struct rte_eth_stats, ipackets)},
        {"fastrg_node_tx_packets_total", "Total transmitted packets per NIC port.", offsetof(struct rte_eth_stats, opackets)},
        {"fastrg_node_rx_bytes_total", "Total received bytes per NIC port.", offsetof(struct rte_eth_stats, ibytes)},
        {"fastrg_node_tx_bytes_total", "Total transmitted bytes per NIC port.", offsetof(struct rte_eth_stats, obytes)},
        {"fastrg_node_rx_errors_total", "Total receive errors per NIC port.", offsetof(struct rte_eth_stats, ierrors)},
        {"fastrg_node_tx_errors_total", "Total transmit errors per NIC port.", offsetof(struct rte_eth_stats, oerrors)},
        {"fastrg_node_rx_dropped_total", "RX packets dropped (no mbuf / ring full) per NIC port.", offsetof(struct rte_eth_stats, imissed)},
    };
    for(size_t m=0; m<sizeof(nic_metrics)/sizeof(nic_metrics[0]); m++) {
        emit_header(out, nic_metrics[m].name, "gauge", nic_metrics[m].help);
        for(int p=0; p<PORT_AMOUNT; p++) {
            if (!eth_ok[p])
                continue;
            uint64_t v = *(uint64_t *)((char *)&eth[p] + nic_metrics[m].off);
            lighthttp_buf_appendf(out, "%s{node_uuid=\"%s\",nic_index=\"%d\"} %" PRIu64 "\n",
                nic_metrics[m].name, uuid, p, v);
        }
    }

    /* ---- per-user traffic ---- */
    static const struct row_metric pu_metrics[] = {
        {"fastrg_node_per_user_rx_packets_total", "Per-subscriber received packets.", offsetof(struct per_user_row, rxp)},
        {"fastrg_node_per_user_rx_bytes_total", "Per-subscriber received bytes.", offsetof(struct per_user_row, rxb)},
        {"fastrg_node_per_user_tx_packets_total", "Per-subscriber transmitted packets.", offsetof(struct per_user_row, txp)},
        {"fastrg_node_per_user_tx_bytes_total", "Per-subscriber transmitted bytes.", offsetof(struct per_user_row, txb)},
        {"fastrg_node_per_user_dropped_packets_total", "Per-subscriber dropped packets.", offsetof(struct per_user_row, dp)},
        {"fastrg_node_per_user_dropped_bytes_total", "Per-subscriber dropped bytes.", offsetof(struct per_user_row, db)},
    };
    for(size_t m=0; m<sizeof(pu_metrics)/sizeof(pu_metrics[0]); m++) {
        emit_header(out, pu_metrics[m].name, "gauge", pu_metrics[m].help);
        for(int p=0; p<PORT_AMOUNT; p++) {
            if (per_user[p] == NULL)
                continue;
            for(U16 i=0; i<user_count; i++) {
                uint64_t v = *(uint64_t *)((char *)&per_user[p][i] + pu_metrics[m].off);
                lighthttp_buf_appendf(out,
                    "%s{node_uuid=\"%s\",nic_index=\"%d\",user_id=\"%u\"} %" PRIu64 "\n",
                    pu_metrics[m].name, uuid, p, (unsigned)(i + 1), v);
            }
        }
    }

    /* ---- unknown-user traffic ---- */
    static const struct row_metric uu_metrics[] = {
        {"fastrg_node_unknown_user_rx_packets_total", "Unmapped received packets.", offsetof(struct per_user_row, rxp)},
        {"fastrg_node_unknown_user_rx_bytes_total", "Unmapped received bytes.", offsetof(struct per_user_row, rxb)},
        {"fastrg_node_unknown_user_tx_packets_total", "Unmapped transmitted packets.", offsetof(struct per_user_row, txp)},
        {"fastrg_node_unknown_user_tx_bytes_total", "Unmapped transmitted bytes.", offsetof(struct per_user_row, txb)},
        {"fastrg_node_unknown_user_dropped_packets_total", "Unmapped dropped packets.", offsetof(struct per_user_row, dp)},
        {"fastrg_node_unknown_user_dropped_bytes_total", "Unmapped dropped bytes.", offsetof(struct per_user_row, db)},
    };
    for(size_t m=0; m<sizeof(uu_metrics)/sizeof(uu_metrics[0]); m++) {
        emit_header(out, uu_metrics[m].name, "gauge", uu_metrics[m].help);
        for(int p=0; p<PORT_AMOUNT; p++) {
            uint64_t v = *(uint64_t *)((char *)&unknown_user[p] + uu_metrics[m].off);
            lighthttp_buf_appendf(out, "%s{node_uuid=\"%s\",nic_index=\"%d\"} %" PRIu64 "\n",
                uu_metrics[m].name, uuid, p, v);
        }
    }

    /* ---- PPPoE session phase tallies ---- */
    static const char *pppoe_phase_names[8] = {
        "fastrg_node_total_pppoe_data_sessions",
        "fastrg_node_total_pppoe_ipcp_sessions",
        "fastrg_node_total_pppoe_auth_sessions",
        "fastrg_node_total_pppoe_lcp_sessions",
        "fastrg_node_total_pppoe_init_sessions",
        "fastrg_node_total_pppoe_terminated_sessions",
        "fastrg_node_total_pppoe_not_configured_sessions",
        "fastrg_node_total_pppoe_error_sessions",
    };
    for(int m=0; m<8; m++) {
        emit_header(out, pppoe_phase_names[m], "gauge", "PPPoE sessions in this phase.");
        lighthttp_buf_appendf(out, "%s{node_uuid=\"%s\"} %" PRIu64 "\n",
            pppoe_phase_names[m], uuid, phase[m]);
    }

    /* ---- per-PPPoE-session traffic ---- */
    static const struct row_metric ppp_metrics[] = {
        {"fastrg_node_per_pppoe_session_rx_packets_total", "Per-PPPoE-session received packets.", offsetof(struct ppp_row, rxp)},
        {"fastrg_node_per_pppoe_session_rx_bytes_total", "Per-PPPoE-session received bytes.", offsetof(struct ppp_row, rxb)},
        {"fastrg_node_per_pppoe_session_tx_packets_total", "Per-PPPoE-session transmitted packets.", offsetof(struct ppp_row, txp)},
        {"fastrg_node_per_pppoe_session_tx_bytes_total", "Per-PPPoE-session transmitted bytes.", offsetof(struct ppp_row, txb)},
    };
    for(size_t m=0; m<sizeof(ppp_metrics)/sizeof(ppp_metrics[0]); m++) {
        emit_header(out, ppp_metrics[m].name, "gauge", ppp_metrics[m].help);
        if (ppp == NULL)
            continue;
        for(U16 i=0; i<user_count; i++) {
            uint64_t v = *(uint64_t *)((char *)&ppp[i] + ppp_metrics[m].off);
            lighthttp_buf_appendf(out, "%s{node_uuid=\"%s\",user_id=\"%u\"} %" PRIu64 "\n",
                ppp_metrics[m].name, uuid, (unsigned)ppp[i].user_id, v);
        }
    }

    /* ---- per-user NAT pool health ---- */
    static const struct row_metric nat_metrics[] = {
        {"fastrg_node_per_user_nat_entries_used", "Live NAT mappings held by this subscriber (pool fill).", offsetof(struct ppp_row, nat_used)},
        {"fastrg_node_per_user_nat_alloc_fail_total", "NAT learning failures: ports exhausted, pool dry or hash full.", offsetof(struct ppp_row, nat_enospc)},
        {"fastrg_node_per_user_nat_gc_reclaimed_total", "Expired NAT mappings reclaimed by the amortized GC.", offsetof(struct ppp_row, nat_gc_reclaimed)},
    };
    for(size_t m=0; m<sizeof(nat_metrics)/sizeof(nat_metrics[0]); m++) {
        emit_header(out, nat_metrics[m].name, "gauge", nat_metrics[m].help);
        if (ppp == NULL)
            continue;
        for(U16 i=0; i<user_count; i++) {
            uint64_t v = *(uint64_t *)((char *)&ppp[i] + nat_metrics[m].off);
            lighthttp_buf_appendf(out, "%s{node_uuid=\"%s\",user_id=\"%u\"} %" PRIu64 "\n",
                nat_metrics[m].name, uuid, (unsigned)ppp[i].user_id, v);
        }
    }

    /* ---- per-user DHCP lease counts ---- */
    emit_header(out, "fastrg_node_per_user_dhcp_cur_lease_count", "gauge",
        "Currently leased addresses for this subscriber's DHCP pool.");
    if (dhcp) {
        for(U16 i=0; i<user_count; i++)
            if (dhcp[i].configured)
                lighthttp_buf_appendf(out,
                    "fastrg_node_per_user_dhcp_cur_lease_count{node_uuid=\"%s\",user_id=\"%u\"} %" PRIu64 "\n",
                    uuid, (unsigned)dhcp[i].user_id, dhcp[i].cur);
    }
    emit_header(out, "fastrg_node_per_user_dhcp_max_lease_count", "gauge",
        "Pool capacity (addresses) for this subscriber's DHCP server.");
    if (dhcp) {
        for(U16 i=0; i<user_count; i++)
            if (dhcp[i].configured)
                lighthttp_buf_appendf(out,
                    "fastrg_node_per_user_dhcp_max_lease_count{node_uuid=\"%s\",user_id=\"%u\"} %" PRIu64 "\n",
                    uuid, (unsigned)dhcp[i].user_id, dhcp[i].max);
    }

    /* ---- DHCP server status tallies ---- */
    emit_header(out, "fastrg_node_total_running_dhcp_server", "gauge", "DHCP servers currently running.");
    lighthttp_buf_appendf(out, "fastrg_node_total_running_dhcp_server{node_uuid=\"%s\"} %" PRIu64 "\n", uuid, dhcp_running);
    emit_header(out, "fastrg_node_total_stopped_dhcp_server", "gauge", "DHCP servers configured but stopped.");
    lighthttp_buf_appendf(out, "fastrg_node_total_stopped_dhcp_server{node_uuid=\"%s\"} %" PRIu64 "\n", uuid, dhcp_stopped);
    emit_header(out, "fastrg_node_total_not_configured_dhcp_server", "gauge", "Subscriber slots with no DHCP server configured.");
    lighthttp_buf_appendf(out, "fastrg_node_total_not_configured_dhcp_server{node_uuid=\"%s\"} %" PRIu64 "\n", uuid, dhcp_notcfg);

    /* ---- lcore usage ---- */
    emit_header(out, "fastrg_node_lcore_busy_cycles_total", "gauge",
        "Cumulative TSC cycles a datapath lcore spent processing packets/events.");
    for(int i=0; i<n_lcores; i++)
        lighthttp_buf_appendf(out, "fastrg_node_lcore_busy_cycles_total{node_uuid=\"%s\",lcore_id=\"%u\",role=\"%s\"} %" PRIu64 "\n",
            uuid, lcores[i].id, esc(lcores[i].role ? lcores[i].role : "", ebuf, sizeof(ebuf)), lcores[i].busy);
    emit_header(out, "fastrg_node_lcore_total_cycles_total", "gauge",
        "Cumulative TSC cycles a datapath lcore polled (busy + idle).");
    for(int i=0; i<n_lcores; i++)
        lighthttp_buf_appendf(out, "fastrg_node_lcore_total_cycles_total{node_uuid=\"%s\",lcore_id=\"%u\",role=\"%s\"} %" PRIu64 "\n",
            uuid, lcores[i].id, esc(lcores[i].role ? lcores[i].role : "", ebuf, sizeof(ebuf)), lcores[i].total);

    /* ---- DPDK heap ---- */
    static const struct row_metric heap_metrics[] = {
        {"fastrg_node_heap_total_bytes", "DPDK heap size on hugepages per NUMA socket.", offsetof(struct heap_row, total)},
        {"fastrg_node_heap_used_bytes", "DPDK heap bytes in use per NUMA socket.", offsetof(struct heap_row, used)},
        {"fastrg_node_heap_free_bytes", "DPDK heap bytes free per NUMA socket.", offsetof(struct heap_row, free)},
        {"fastrg_node_heap_largest_free_block_bytes", "Largest contiguous free block (fragmentation gauge).", offsetof(struct heap_row, largest)},
    };
    for(size_t m=0; m<sizeof(heap_metrics)/sizeof(heap_metrics[0]); m++) {
        emit_header(out, heap_metrics[m].name, "gauge", heap_metrics[m].help);
        for(int i=0; i<n_heaps; i++) {
            uint64_t v = *(uint64_t *)((char *)&heaps[i] + heap_metrics[m].off);
            lighthttp_buf_appendf(out, "%s{node_uuid=\"%s\",socket_id=\"%d\"} %" PRIu64 "\n",
                heap_metrics[m].name, uuid, heaps[i].socket_id, v);
        }
    }

    /* ---- DPDK mempool ---- */
    emit_header(out, "fastrg_node_mempool_size", "gauge", "Total elements in the mempool.");
    for(int i=0; i<mpc.n; i++)
        lighthttp_buf_appendf(out, "fastrg_node_mempool_size{node_uuid=\"%s\",pool=\"%s\"} %u\n",
            uuid, esc(mp_rows[i].name, ebuf, sizeof(ebuf)), mp_rows[i].size);
    emit_header(out, "fastrg_node_mempool_avail_count", "gauge", "Free elements in the mempool.");
    for(int i=0; i<mpc.n; i++)
        lighthttp_buf_appendf(out, "fastrg_node_mempool_avail_count{node_uuid=\"%s\",pool=\"%s\"} %u\n",
            uuid, esc(mp_rows[i].name, ebuf, sizeof(ebuf)), mp_rows[i].avail);
    emit_header(out, "fastrg_node_mempool_in_use_count", "gauge", "In-use elements in the mempool.");
    for(int i=0; i<mpc.n; i++)
        lighthttp_buf_appendf(out, "fastrg_node_mempool_in_use_count{node_uuid=\"%s\",pool=\"%s\"} %u\n",
            uuid, esc(mp_rows[i].name, ebuf, sizeof(ebuf)), mp_rows[i].in_use);

    /* ---- hugepage ---- */
    emit_header(out, "fastrg_node_hugepage_pinned_bytes", "gauge", "Hugepage memory locked by DPDK.");
    lighthttp_buf_appendf(out, "fastrg_node_hugepage_pinned_bytes{node_uuid=\"%s\"} %" PRIu64 "\n", uuid, hugepage_pinned);

    /* ---- NIC link state ---- */
    emit_header(out, "fastrg_nic_link_up", "gauge", "NIC link state (1 = up, 0 = down).");
    for(int p=0; p<PORT_AMOUNT; p++)
        lighthttp_buf_appendf(out, "fastrg_nic_link_up{node_uuid=\"%s\",port_id=\"%d\"} %u\n",
            uuid, p, __atomic_load_n(&fastrg_ccb->nic_link_up[p], __ATOMIC_RELAXED));
    emit_header(out, "fastrg_nic_link_speed_mbps", "gauge", "NIC link speed in Mbps (0 when down).");
    for(int p=0; p<PORT_AMOUNT; p++)
        lighthttp_buf_appendf(out, "fastrg_nic_link_speed_mbps{node_uuid=\"%s\",port_id=\"%d\"} %u\n",
            uuid, p, __atomic_load_n(&fastrg_ccb->nic_link_speed[p], __ATOMIC_RELAXED));
    emit_header(out, "fastrg_nic_link_flaps_total", "counter", "Cumulative NIC link up/down transitions.");
    for(int p=0; p<PORT_AMOUNT; p++)
        lighthttp_buf_appendf(out, "fastrg_nic_link_flaps_total{node_uuid=\"%s\",port_id=\"%d\"} %" PRIu64 "\n",
            uuid, p, __atomic_load_n(&fastrg_ccb->nic_link_flaps[p], __ATOMIC_RELAXED));

    /* ---- NIC static info (model / driver / pci / mac) ---- */
    emit_header(out, "fastrg_nic_info", "gauge", "NIC metadata; value is always 1.");
    for(int p=0; p<PORT_AMOUNT; p++) {
        char driver[64] = "unknown", pci[64] = "unknown", mac[32] = "";
        struct rte_eth_dev_info di;
        if (rte_eth_dev_info_get(p, &di) == 0 && di.driver_name)
            snprintf(driver, sizeof(driver), "%s", di.driver_name);
        char nbuf[RTE_ETH_NAME_MAX_LEN];
        if (rte_eth_dev_get_name_by_port(p, nbuf) == 0)
            snprintf(pci, sizeof(pci), "%s", nbuf);
        const struct rte_ether_addr *m =
            (p == 0) ? &fastrg_ccb->nic_info.hsi_lan_mac : &fastrg_ccb->nic_info.hsi_wan_src_mac;
        mac_str(m, mac, sizeof(mac));

        char e_model[256], e_driver[128], e_pci[128], e_mac[64];
        lighthttp_buf_appendf(out,
            "fastrg_nic_info{node_uuid=\"%s\",port_id=\"%d\",model=\"%s\",driver=\"%s\",pci=\"%s\",mac=\"%s\"} 1\n",
            uuid, p,
            esc(fastrg_ccb->nic_info.model[p], e_model, sizeof(e_model)),
            esc(driver, e_driver, sizeof(e_driver)),
            esc(pci, e_pci, sizeof(e_pci)),
            esc(mac, e_mac, sizeof(e_mac)));
    }

    for(int p=0; p<PORT_AMOUNT; p++)
        free(per_user[p]);
    free(ppp);
    free(dhcp);

    return 200;
}
