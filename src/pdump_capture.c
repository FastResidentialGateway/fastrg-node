/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  PDUMP_CAPTURE.C

  CLI-driven per-subscriber packet capture. See pdump_capture.h for the design
  overview.
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "pdump_capture.h"

#include "dbg.h"

#ifndef UNIT_TEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <libgen.h>
#include <sys/time.h>

#include <pcap.h>

#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_byteorder.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_bpf.h>

#include "protocol.h"

#define PDUMP_MAX_Q        32
#define PDUMP_RING_SIZE    8192
#define PDUMP_NUM_MBUFS    8191
#define PDUMP_CACHE_SIZE   256
#define PDUMP_SNAPLEN      262144
#define PDUMP_DRAIN_MS     5     /* drain window for in-flight callbacks on stop */
#define PDUMP_MAX_SIZE_MB  2048  /* hard cap on the pcap file size (2GB)         */

/* classic libpcap file headers (little-endian host assumed; magic encodes order) */
struct pcap_file_hdr {
    U32 magic;
    U16 version_major;
    U16 version_minor;
    S32 thiszone;
    U32 sigfigs;
    U32 snaplen;
    U32 network;
} __attribute__((packed));

struct pcap_rec_hdr {
    U32 ts_sec;
    U32 ts_usec;
    U32 incl_len;
    U32 orig_len;
} __attribute__((packed));

/* A compiled BPF filter. All compiled filters live on global_pdump_ctx.filter_list and are
 * freed only at full session teardown (never while callbacks may execute). */
struct cap_filter {
    struct rte_bpf *bpf;
    uint64_t (*jit)(void *);   /* JIT entry, NULL => fall back to rte_bpf_exec */
    struct cap_filter *next;
};

static struct pdump_ctx {
    pthread_mutex_t lock;
    int             initialized;
    FastRG_t       *fastrg_ccb;
    U16             slots;                      /* matrix dimension = max_user_count */

    /* control-plane written, data-plane (callback) read */
    uint8_t        *capture_on[PORT_AMOUNT];    /* [port][ccb] 0/1                  */
    struct cap_filter **filter[PORT_AMOUNT];    /* [port][ccb] -> filter or NULL    */
    rte_atomic32_t  any_active;                 /* total active (port,ccb) entries  */

    int             active_per_port[PORT_AMOUNT];
    const struct rte_eth_rxtx_callback *rx_cb[PORT_AMOUNT][PDUMP_MAX_Q];
    const struct rte_eth_rxtx_callback *tx_cb[PORT_AMOUNT][PDUMP_MAX_Q];
    U16             nb_rx_q[PORT_AMOUNT];
    U16             nb_tx_q[PORT_AMOUNT];

    struct cap_filter *filter_list;             /* all compiled filters (freed at teardown) */

    struct rte_ring    *ring;
    struct rte_mempool *mp;
    FILE               *pcap_fp;
    char                pcap_path[256];
    pthread_t           writer;
    rte_atomic16_t      writer_running;
    int                 writer_started;

    uint64_t            size_limit;   /* max pcap bytes (<= 2GB); 0 = unset      */
    uint64_t            bytes_written;/* pcap bytes written so far (writer-owned) */
    rte_atomic16_t      paused;       /* set when size limit reached: callbacks no-op */
} global_pdump_ctx;

/* ------------------------------------------------------------------ helpers */

/* Derive the subscriber ccb id from the frame's outer VLAN tag, mirroring
 * parse_l2_hdr() in dp.c. Returns INVALID_CCB_ID when the frame is not a
 * mappable subscriber frame. */
static inline U16 pdump_vlan_to_ccb(FastRG_t *fastrg_ccb, struct rte_mbuf *m)
{
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    if (unlikely(eth_hdr->ether_type != rte_cpu_to_be_16(VLAN)))
        return INVALID_CCB_ID;
    if (unlikely(m->data_len < sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t)))
        return INVALID_CCB_ID;

    vlan_header_t *vlan_header = (vlan_header_t *)(rte_pktmbuf_mtod(m, unsigned char *) +
        sizeof(struct rte_ether_hdr));
    U16 vlan_id = rte_be_to_cpu_16(vlan_header->tci_union.tci_value) & 0xFFF;
    if (unlikely(vlan_id < MIN_VLAN_ID || vlan_id > MAX_VLAN_ID))
        return INVALID_CCB_ID;

    U16 ccb_id = rte_atomic16_read(&fastrg_ccb->vlan_userid_map[vlan_id - 1]);
    if (unlikely(ccb_id > fastrg_ccb->user_count - 1))
        return INVALID_CCB_ID;
    return ccb_id;
}

/* Common capture path shared by the RX and TX callbacks. Non-destructive: the
 * original mbufs are left untouched; matching frames are deep-copied (the
 * data-plane mutates packets in place, so a shared clone would be corrupted)
 * into the capture pool and queued for the writer. */
static void pdump_do_capture(U16 port, struct rte_mbuf **pkts, U16 nb)
{
    FastRG_t *fastrg_ccb = global_pdump_ctx.fastrg_ccb;

    /* Size limit reached: stop copying entirely (the file is already capped). */
    if (unlikely(rte_atomic16_read(&global_pdump_ctx.paused)))
        return;

    for(U16 i=0; i<nb; i++) {
        struct rte_mbuf *m = pkts[i];
        U16 ccb_id = pdump_vlan_to_ccb(fastrg_ccb, m);
        if (ccb_id == INVALID_CCB_ID)
            continue;
        if (!global_pdump_ctx.capture_on[port][ccb_id])
            continue;

        struct cap_filter *f = global_pdump_ctx.filter[port][ccb_id];
        if (f != NULL) {
            uint64_t rc = f->jit ? f->jit(m) : rte_bpf_exec(f->bpf, m);
            if (rc == 0)
                continue;
        }

        struct rte_mbuf *copy = rte_pktmbuf_copy(m, global_pdump_ctx.mp, 0, UINT32_MAX);
        if (unlikely(copy == NULL))
            continue;

        struct timeval tv;
        gettimeofday(&tv, NULL);
        *(uint64_t *)rte_mbuf_to_priv(copy) =
            (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;

        if (unlikely(rte_ring_enqueue(global_pdump_ctx.ring, copy) != 0))
            rte_pktmbuf_free(copy);
    }
}

static uint16_t pdump_rx_cb(uint16_t port, __rte_unused uint16_t queue,
    struct rte_mbuf *pkts[], uint16_t nb_pkts, __rte_unused uint16_t max_pkts,
    __rte_unused void *user)
{
    pdump_do_capture(port, pkts, nb_pkts);
    return nb_pkts;
}

static uint16_t pdump_tx_cb(uint16_t port, __rte_unused uint16_t queue,
    struct rte_mbuf *pkts[], uint16_t nb_pkts, __rte_unused void *user)
{
    pdump_do_capture(port, pkts, nb_pkts);
    return nb_pkts;
}

/* ---------------------------------------------------------------- pcap I/O */

static STATUS pdump_write_global_hdr(FILE *fp)
{
    struct pcap_file_hdr gh = {
        .magic = 0xa1b2c3d4,
        .version_major = 2,
        .version_minor = 4,
        .thiszone = 0,
        .sigfigs = 0,
        .snaplen = PDUMP_SNAPLEN,
        .network = 1, /* LINKTYPE_ETHERNET */
    };
    if (fwrite(&gh, sizeof(gh), 1, fp) != 1)
        return ERROR;
    return SUCCESS;
}

/* Write one pcap record, honouring the configured size limit. Returns FALSE
 * (and pauses capture) once writing the record would exceed global_pdump_ctx.size_limit. */
static BOOL pdump_write_record(FILE *fp, struct rte_mbuf *m)
{
    uint64_t ts = *(uint64_t *)rte_mbuf_to_priv(m);
    U32 caplen = m->pkt_len > PDUMP_SNAPLEN ? PDUMP_SNAPLEN : m->pkt_len;
    uint64_t reclen = sizeof(struct pcap_rec_hdr) + caplen;

    if (global_pdump_ctx.size_limit && global_pdump_ctx.bytes_written + reclen > global_pdump_ctx.size_limit) {
        if (rte_atomic16_read(&global_pdump_ctx.paused) == 0) {
            rte_atomic16_set(&global_pdump_ctx.paused, 1);  /* stop callbacks from copying more */
            fflush(fp);
            FastRG_LOG(INFO, global_pdump_ctx.fastrg_ccb->fp, NULL, NULL,
                "pdump size limit (%lu bytes) reached, capture paused: %s",
                (unsigned long)global_pdump_ctx.size_limit, global_pdump_ctx.pcap_path);
        }
        return FALSE;
    }

    struct pcap_rec_hdr rh = {
        .ts_sec = (U32)(ts / 1000000ULL),
        .ts_usec = (U32)(ts % 1000000ULL),
        .incl_len = caplen,
        .orig_len = m->pkt_len,
    };
    if (fwrite(&rh, sizeof(rh), 1, fp) != 1)
        return FALSE;
    global_pdump_ctx.bytes_written += sizeof(rh);

    U32 remaining = caplen;
    for(struct rte_mbuf *seg=m; seg!=NULL && remaining>0; seg=seg->next) {
        U32 seg_len = seg->data_len < remaining ? seg->data_len : remaining;
        if (fwrite(rte_pktmbuf_mtod(seg, void *), 1, seg_len, fp) != seg_len)
            return FALSE;
        remaining -= seg_len;
    }
    global_pdump_ctx.bytes_written += caplen;
    return TRUE;
}

/* Single consumer: drains the ring, writes records and frees the copies. */
static void *pdump_writer_thread(__rte_unused void *arg)
{
    struct rte_mbuf *bufs[64];

    while (rte_atomic16_read(&global_pdump_ctx.writer_running)) {
        unsigned n = rte_ring_dequeue_burst(global_pdump_ctx.ring, (void **)bufs, 64, NULL);
        if (n == 0) {
            rte_delay_us_block(200);
            continue;
        }
        for(int i=0; i<n; i++) {
            pdump_write_record(global_pdump_ctx.pcap_fp, bufs[i]);
            rte_pktmbuf_free(bufs[i]);
        }
        fflush(global_pdump_ctx.pcap_fp);
    }

    /* Final drain of whatever is left in the ring after stop. */
    unsigned n;
    while ((n = rte_ring_dequeue_burst(global_pdump_ctx.ring, (void **)bufs, 64, NULL)) > 0) {
        for(int i=0; i<n; i++) {
            pdump_write_record(global_pdump_ctx.pcap_fp, bufs[i]);
            rte_pktmbuf_free(bufs[i]);
        }
    }
    fflush(global_pdump_ctx.pcap_fp);
    return NULL;
}

/* ------------------------------------------------------------ filter compile */

static struct cap_filter *pdump_compile_filter(const char *expr, char *err, U32 err_len)
{
    pcap_t *pc = pcap_open_dead(DLT_EN10MB, PDUMP_SNAPLEN);
    if (pc == NULL) {
        if (err) snprintf(err, err_len, "pcap_open_dead failed");
        return NULL;
    }

    struct bpf_program prog;
    if (pcap_compile(pc, &prog, expr, 1, PCAP_NETMASK_UNKNOWN) != 0) {
        if (err) snprintf(err, err_len, "invalid filter: %s", pcap_geterr(pc));
        pcap_close(pc);
        return NULL;
    }

    struct rte_bpf_prm *prm = rte_bpf_convert(&prog);
    pcap_freecode(&prog);
    pcap_close(pc);
    if (prm == NULL) {
        if (err) snprintf(err, err_len, "rte_bpf_convert failed (rte_errno=%d)", rte_errno);
        return NULL;
    }

    struct rte_bpf *bpf = rte_bpf_load(prm);
    rte_free(prm);
    if (bpf == NULL) {
        if (err) snprintf(err, err_len, "rte_bpf_load failed (rte_errno=%d)", rte_errno);
        return NULL;
    }

    struct cap_filter *f = calloc(1, sizeof(*f));
    if (f == NULL) {
        rte_bpf_destroy(bpf);
        if (err) snprintf(err, err_len, "out of memory");
        return NULL;
    }
    f->bpf = bpf;

    struct rte_bpf_jit jit;
    if (rte_bpf_get_jit(bpf, &jit) == 0 && jit.func != NULL)
        f->jit = jit.func;

    /* Track for teardown-time free. */
    f->next = global_pdump_ctx.filter_list;
    global_pdump_ctx.filter_list = f;
    return f;
}

/* ---------------------------------------------------- callback (un)registration */

static void pdump_register_port(U16 port)
{
    struct rte_eth_dev_info info;
    if (rte_eth_dev_info_get(port, &info) != 0)
        return;

    global_pdump_ctx.nb_rx_q[port] = info.nb_rx_queues < PDUMP_MAX_Q ? info.nb_rx_queues : PDUMP_MAX_Q;
    global_pdump_ctx.nb_tx_q[port] = info.nb_tx_queues < PDUMP_MAX_Q ? info.nb_tx_queues : PDUMP_MAX_Q;

    for(U16 q=0; q<global_pdump_ctx.nb_rx_q[port]; q++)
        global_pdump_ctx.rx_cb[port][q] = rte_eth_add_rx_callback(port, q, pdump_rx_cb, NULL);
    for(U16 q=0; q<global_pdump_ctx.nb_tx_q[port]; q++)
        global_pdump_ctx.tx_cb[port][q] = rte_eth_add_tx_callback(port, q, pdump_tx_cb, NULL);
}

static void pdump_unregister_port(U16 port)
{
    for(U16 q=0; q<global_pdump_ctx.nb_rx_q[port]; q++) {
        if (global_pdump_ctx.rx_cb[port][q]) {
            rte_eth_remove_rx_callback(port, q, global_pdump_ctx.rx_cb[port][q]);
            global_pdump_ctx.rx_cb[port][q] = NULL;
        }
    }
    for(U16 q=0; q<global_pdump_ctx.nb_tx_q[port]; q++) {
        if (global_pdump_ctx.tx_cb[port][q]) {
            rte_eth_remove_tx_callback(port, q, global_pdump_ctx.tx_cb[port][q]);
            global_pdump_ctx.tx_cb[port][q] = NULL;
        }
    }
}

/* --------------------------------------------------------- session lifecycle */

/* Create ring/mempool/file and start the writer thread. Called on the
 * 0 -> non-zero transition of any_active. */
static STATUS pdump_capture_open(FastRG_t *fastrg_ccb, char *err, U32 err_len)
{
    const char *dir = "/var/log/fastrg";
    char dir_buf[256];
    if (fastrg_ccb->log_path && fastrg_ccb->log_path[0]) {
        snprintf(dir_buf, sizeof(dir_buf), "%s", fastrg_ccb->log_path);
        dir = dirname(dir_buf);
    }
    snprintf(global_pdump_ctx.pcap_path, sizeof(global_pdump_ctx.pcap_path), "%s/pdump-%lu.pcap",
        dir, (unsigned long)time(NULL));

    global_pdump_ctx.mp = rte_pktmbuf_pool_create("pdump_cap_pool", PDUMP_NUM_MBUFS,
        PDUMP_CACHE_SIZE, RTE_ALIGN(sizeof(uint64_t), RTE_MBUF_PRIV_ALIGN),
        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (global_pdump_ctx.mp == NULL) {
        if (err) snprintf(err, err_len, "cannot create capture mempool: %s", rte_strerror(rte_errno));
        return ERROR;
    }

    global_pdump_ctx.ring = rte_ring_create("pdump_cap_ring", PDUMP_RING_SIZE, rte_socket_id(),
        RING_F_SC_DEQ);
    if (global_pdump_ctx.ring == NULL) {
        if (err) snprintf(err, err_len, "cannot create capture ring: %s", rte_strerror(rte_errno));
        goto err_mp;
    }

    global_pdump_ctx.pcap_fp = fopen(global_pdump_ctx.pcap_path, "wb");
    if (global_pdump_ctx.pcap_fp == NULL) {
        if (err) snprintf(err, err_len, "cannot open %s", global_pdump_ctx.pcap_path);
        goto err_ring;
    }
    if (pdump_write_global_hdr(global_pdump_ctx.pcap_fp) == ERROR) {
        if (err) snprintf(err, err_len, "cannot write pcap header");
        goto err_file;
    }
    fflush(global_pdump_ctx.pcap_fp);
    global_pdump_ctx.bytes_written = sizeof(struct pcap_file_hdr);
    rte_atomic16_set(&global_pdump_ctx.paused, 0);

    rte_atomic16_set(&global_pdump_ctx.writer_running, 1);
    /* Joinable, pinned to the main lcore CPU so the writer never steals a
     * data-plane core. We keep the tid so pdump_capture_teardown() can join it,
     * which guarantees every queued frame is written before the file closes. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(rte_lcore_to_cpu_id(rte_get_main_lcore()), &cpuset);
    pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
    int prc = pthread_create(&global_pdump_ctx.writer, &attr, pdump_writer_thread, fastrg_ccb);
    pthread_attr_destroy(&attr);
    if (prc != 0) {
        if (err) snprintf(err, err_len, "cannot start writer thread");
        rte_atomic16_set(&global_pdump_ctx.writer_running, 0);
        goto err_file;
    }
    pthread_setname_np(global_pdump_ctx.writer, "fastrg_pdump");
    global_pdump_ctx.writer_started = 1;
    return SUCCESS;

err_file:
    fclose(global_pdump_ctx.pcap_fp);
    global_pdump_ctx.pcap_fp = NULL;
err_ring:
    rte_ring_free(global_pdump_ctx.ring);
    global_pdump_ctx.ring = NULL;
err_mp:
    rte_mempool_free(global_pdump_ctx.mp);
    global_pdump_ctx.mp = NULL;
    return ERROR;
}

/* Tear down the writer/ring/mempool/file and free all compiled filters. Called
 * on the non-zero -> 0 transition of any_active. Callbacks for every port have
 * already been removed by the caller. */
static void pdump_capture_teardown(FastRG_t *fastrg_ccb)
{
    /* Let any in-flight callback (already past the registration check) finish
     * enqueuing before the writer stops draining the ring it feeds. */
    rte_delay_ms(PDUMP_DRAIN_MS);

    if (global_pdump_ctx.writer_started) {
        rte_atomic16_set(&global_pdump_ctx.writer_running, 0);
        pthread_join(global_pdump_ctx.writer, NULL);  /* writer drains the ring before exiting */
        global_pdump_ctx.writer_started = 0;
    }

    if (global_pdump_ctx.pcap_fp) {
        fflush(global_pdump_ctx.pcap_fp);
        fclose(global_pdump_ctx.pcap_fp);
        global_pdump_ctx.pcap_fp = NULL;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "pdump capture written to %s", global_pdump_ctx.pcap_path);
    }
    if (global_pdump_ctx.ring) {
        rte_ring_free(global_pdump_ctx.ring);
        global_pdump_ctx.ring = NULL;
    }
    if (global_pdump_ctx.mp) {
        rte_mempool_free(global_pdump_ctx.mp);
        global_pdump_ctx.mp = NULL;
    }

    /* Free every compiled filter and clear the matrix pointers. */
    struct cap_filter *f = global_pdump_ctx.filter_list;
    while (f) {
        struct cap_filter *nx = f->next;
        rte_bpf_destroy(f->bpf);
        free(f);
        f = nx;
    }
    global_pdump_ctx.filter_list = NULL;
    for(int p=0; p<PORT_AMOUNT; p++)
        if (global_pdump_ctx.filter[p])
            memset(global_pdump_ctx.filter[p], 0, global_pdump_ctx.slots * sizeof(struct cap_filter *));
}

/* --------------------------------------------------------------- direction map */

/* Fill ports[] with the port ids implied by dir. Returns the count. */
static int pdump_dir_ports(int dir, U16 ports[PORT_AMOUNT])
{
    switch (dir) {
    case PDUMP_DIR_WAN: ports[0] = WAN_PORT; return 1;
    case PDUMP_DIR_LAN: ports[0] = LAN_PORT; return 1;
    case PDUMP_DIR_ALL: ports[0] = WAN_PORT; ports[1] = LAN_PORT; return 2;
    default:            return 0;
    }
}

/* ------------------------------------------------------------------- public API */

STATUS fastrg_pdump_capture_init(FastRG_t *fastrg_ccb)
{
    memset(&global_pdump_ctx, 0, sizeof(global_pdump_ctx));
    pthread_mutex_init(&global_pdump_ctx.lock, NULL);
    global_pdump_ctx.fastrg_ccb = fastrg_ccb;
    global_pdump_ctx.slots = fastrg_ccb->max_user_count;
    rte_atomic32_init(&global_pdump_ctx.any_active);
    rte_atomic16_init(&global_pdump_ctx.writer_running);

    for(int p=0; p<PORT_AMOUNT; p++) {
        global_pdump_ctx.capture_on[p] = calloc(global_pdump_ctx.slots, sizeof(uint8_t));
        global_pdump_ctx.filter[p] = calloc(global_pdump_ctx.slots, sizeof(struct cap_filter *));
        if (global_pdump_ctx.capture_on[p] == NULL || global_pdump_ctx.filter[p] == NULL) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "pdump capture init: out of memory");
            return ERROR;
        }
    }
    global_pdump_ctx.initialized = 1;
    return SUCCESS;
}

STATUS fastrg_pdump_start(FastRG_t *fastrg_ccb, int dir, U16 subscriber,
    const char *filter, U32 size_limit_mb, char *out_file, U32 out_len,
    char *err, U32 err_len)
{
    U16 ports[PORT_AMOUNT];
    int nports = pdump_dir_ports(dir, ports);

    if (!global_pdump_ctx.initialized) {
        if (err) snprintf(err, err_len, "pdump capture not initialized");
        return ERROR;
    }
    if (nports == 0) {
        if (err) snprintf(err, err_len, "invalid direction");
        return ERROR;
    }
    if (subscriber > fastrg_ccb->user_count) {
        if (err) snprintf(err, err_len, "subscriber %u does not exist", subscriber);
        return ERROR;
    }

    /* Clamp the requested size to (0, 2GB]; 0 (unset) defaults to the 2GB cap. */
    if (size_limit_mb == 0 || size_limit_mb > PDUMP_MAX_SIZE_MB)
        size_limit_mb = PDUMP_MAX_SIZE_MB;

    pthread_mutex_lock(&global_pdump_ctx.lock);

    /* Compile the optional filter once and share it across the affected slots. */
    struct cap_filter *f = NULL;
    if (filter && filter[0]) {
        f = pdump_compile_filter(filter, err, err_len);
        if (f == NULL) {
            pthread_mutex_unlock(&global_pdump_ctx.lock);
            return ERROR;
        }
    }

    int was_idle = (rte_atomic32_read(&global_pdump_ctx.any_active) == 0);
    if (was_idle) {
        /* The size limit belongs to the file, so it is fixed by the first start
         * that opens the session. */
        global_pdump_ctx.size_limit = (uint64_t)size_limit_mb * 1024 * 1024;
        if (pdump_capture_open(fastrg_ccb, err, err_len) == ERROR) {
            pthread_mutex_unlock(&global_pdump_ctx.lock);
            return ERROR;
        }
    } else if (rte_atomic16_read(&global_pdump_ctx.paused)) {
        if (err)
            snprintf(err, err_len,
                "capture paused (size limit reached); stop all captures first to start a new session");
        pthread_mutex_unlock(&global_pdump_ctx.lock);
        return ERROR;
    }

    U16 lo = (subscriber == 0) ? 0 : (subscriber - 1);
    U16 hi = (subscriber == 0) ? (fastrg_ccb->user_count - 1) : (subscriber - 1);

    for(int pi=0; pi<nports; pi++) {
        U16 port = ports[pi];
        int port_was_idle = (global_pdump_ctx.active_per_port[port] == 0);
        for(U16 ccb=lo; ccb<=hi; ccb++) {
            global_pdump_ctx.filter[port][ccb] = f;          /* publish filter before enabling */
            rte_smp_wmb();
            if (global_pdump_ctx.capture_on[port][ccb] == 0) {
                global_pdump_ctx.capture_on[port][ccb] = 1;
                global_pdump_ctx.active_per_port[port]++;
                rte_atomic32_inc(&global_pdump_ctx.any_active);
            }
        }
        if (port_was_idle && global_pdump_ctx.active_per_port[port] > 0)
            pdump_register_port(port);
    }

    if (out_file)
        snprintf(out_file, out_len, "%s", global_pdump_ctx.pcap_path);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "pdump start dir=%d subscriber=%u filter=%s -> %s",
        dir, subscriber, (filter && filter[0]) ? filter : "(none)", global_pdump_ctx.pcap_path);

    pthread_mutex_unlock(&global_pdump_ctx.lock);
    return SUCCESS;
}

STATUS fastrg_pdump_stop(FastRG_t *fastrg_ccb, int dir, U16 subscriber,
    char *err, U32 err_len)
{
    U16 ports[PORT_AMOUNT];
    int nports = pdump_dir_ports(dir, ports);

    if (!global_pdump_ctx.initialized) {
        if (err) snprintf(err, err_len, "pdump capture not initialized");
        return ERROR;
    }
    if (nports == 0) {
        if (err) snprintf(err, err_len, "invalid direction");
        return ERROR;
    }
    if (subscriber > fastrg_ccb->user_count) {
        if (err) snprintf(err, err_len, "subscriber %u does not exist", subscriber);
        return ERROR;
    }

    pthread_mutex_lock(&global_pdump_ctx.lock);

    if (rte_atomic32_read(&global_pdump_ctx.any_active) == 0) {
        if (err) snprintf(err, err_len, "no capture is running");
        pthread_mutex_unlock(&global_pdump_ctx.lock);
        return ERROR;
    }

    U16 lo = (subscriber == 0) ? 0 : (subscriber - 1);
    U16 hi = (subscriber == 0) ? (fastrg_ccb->user_count - 1) : (subscriber - 1);

    for(int pi=0; pi<nports; pi++) {
        U16 port = ports[pi];
        for(U16 ccb=lo; ccb<=hi; ccb++) {
            if (global_pdump_ctx.capture_on[port][ccb]) {
                global_pdump_ctx.capture_on[port][ccb] = 0;  /* stop matching new frames     */
                rte_smp_wmb();
                global_pdump_ctx.filter[port][ccb] = NULL;   /* object stays on filter_list  */
                global_pdump_ctx.active_per_port[port]--;
                rte_atomic32_dec(&global_pdump_ctx.any_active);
            }
        }
        if (global_pdump_ctx.active_per_port[port] == 0)
            pdump_unregister_port(port);
    }

    if (rte_atomic32_read(&global_pdump_ctx.any_active) == 0)
        pdump_capture_teardown(fastrg_ccb);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "pdump stop dir=%d subscriber=%u", dir, subscriber);

    pthread_mutex_unlock(&global_pdump_ctx.lock);
    return SUCCESS;
}

void fastrg_pdump_capture_cleanup(FastRG_t *fastrg_ccb)
{
    if (!global_pdump_ctx.initialized)
        return;

    pthread_mutex_lock(&global_pdump_ctx.lock);
    if (rte_atomic32_read(&global_pdump_ctx.any_active) > 0) {
        for(int p=0; p<PORT_AMOUNT; p++) {
            if (global_pdump_ctx.active_per_port[p] > 0) {
                memset(global_pdump_ctx.capture_on[p], 0, global_pdump_ctx.slots);
                global_pdump_ctx.active_per_port[p] = 0;
                pdump_unregister_port(p);
            }
        }
        rte_atomic32_set(&global_pdump_ctx.any_active, 0);
        pdump_capture_teardown(fastrg_ccb);
    }
    for(int p=0; p<PORT_AMOUNT; p++) {
        free(global_pdump_ctx.capture_on[p]);
        free(global_pdump_ctx.filter[p]);
        global_pdump_ctx.capture_on[p] = NULL;
        global_pdump_ctx.filter[p] = NULL;
    }
    global_pdump_ctx.initialized = 0;
    pthread_mutex_unlock(&global_pdump_ctx.lock);
    pthread_mutex_destroy(&global_pdump_ctx.lock);
}

#else /* UNIT_TEST: capture relies on real DPDK ethdev/bpf, stub it out. */

STATUS fastrg_pdump_capture_init(FastRG_t *fastrg_ccb) { (void)fastrg_ccb; return SUCCESS; }
STATUS fastrg_pdump_start(FastRG_t *fastrg_ccb, int dir, U16 subscriber,
    const char *filter, U32 size_limit_mb, char *out_file, U32 out_len,
    char *err, U32 err_len)
{
    (void)fastrg_ccb; (void)dir; (void)subscriber; (void)filter;
    (void)size_limit_mb; (void)out_file; (void)out_len;
    if (err) snprintf(err, err_len, "pdump not available in unit test build");
    return ERROR;
}
STATUS fastrg_pdump_stop(FastRG_t *fastrg_ccb, int dir, U16 subscriber, char *err, U32 err_len)
{
    (void)fastrg_ccb; (void)dir; (void)subscriber;
    if (err) snprintf(err, err_len, "pdump not available in unit test build");
    return ERROR;
}
void fastrg_pdump_capture_cleanup(FastRG_t *fastrg_ccb) { (void)fastrg_ccb; }

#endif /* UNIT_TEST */
