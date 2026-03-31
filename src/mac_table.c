/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  MAC_TABLE.C

     Per-subscriber MAC table allocation and ARP pending queue
     implementation.

  Designed by THE on 2026/03/29
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#include <stdlib.h>
#include <string.h>

#include <rte_ether.h>
#include <rte_arp.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_ethdev.h>
#include <rte_byteorder.h>
#include <rte_errno.h>

#include "mac_table.h"
#include "init.h"
#include "fastrg.h"
#include "protocol.h"

/* ------------------------------------------------------------------ */
/*  MAC table allocation / free                                       */
/* ------------------------------------------------------------------ */

mac_table_entry_t *mac_table_alloc(void)
{
    /*
     * 255^3 × 8 B ≈ 132 MB — too large for DPDK hugepage memzones in
     * most setups, so we use plain calloc (backed by regular pages).
     * The table is still O(1)-indexed from the data path.
     */
    return fastrg_calloc(mac_table_entry_t, MAC_TABLE_SIZE, 
        sizeof(mac_table_entry_t), 0);
}

void mac_table_free(mac_table_entry_t *table)
{
    if (table != NULL)
        fastrg_mfree(table);
}

/* ------------------------------------------------------------------ */
/*  ARP pending mempool                                               */
/* ------------------------------------------------------------------ */

STATUS arp_pending_init_pool(struct rte_mempool **arp_mp_out)
{
    if (arp_mp_out == NULL)
        return ERROR;

    /* Power-of-2 pool size minus 1 (DPDK requirement) */
    struct rte_mempool *mp = rte_mempool_create(
        "arp_pending_pool",             /* name */
        ARP_PENDING_POOL_SIZE - 1,      /* n (must be 2^k - 1) */
        sizeof(arp_pending_pkt_t),      /* elt_size */
        0,                              /* cache_size */
        0,                              /* private_data_size */
        NULL, NULL,                     /* mp_init, mp_init_arg */
        NULL, NULL,                     /* obj_init, obj_init_arg */
        rte_socket_id(),                /* socket_id */
        0                               /* flags */
    );
    if (mp == NULL)
        return ERROR;

    *arp_mp_out = mp;
    return SUCCESS;
}

void arp_pending_cleanup_pool(struct rte_mempool **arp_mp_ptr)
{
    if (arp_mp_ptr == NULL || *arp_mp_ptr == NULL)
        return;
    rte_mempool_free(*arp_mp_ptr);
    *arp_mp_ptr = NULL;
}

/* ------------------------------------------------------------------ */
/*  ARP pending queue operations  (lock-free rte_ring)                */
/* ------------------------------------------------------------------ */

STATUS arp_pending_init_queue(arp_pending_queue_t *q, int ccb_id)
{
    if (q == NULL)
        return ERROR;

    char name[RTE_RING_NAMESIZE];
    snprintf(name, sizeof(name), "arp_pq_%d", ccb_id);

    /*
     * MPMC: enqueue from data-path thread (lan_data_rx / wan_data_rx),
     *       dequeue from ctrl thread (lan_ctrl_rx on ARP reply).
     * ARP_PENDING_QUEUE_SIZE must be power-of-2 for rte_ring.
     */
    q->ring = rte_ring_create(name,
        ARP_PENDING_QUEUE_SIZE,
        rte_socket_id(), 0);

    if (q->ring == NULL)
        return ERROR;

    return SUCCESS;
}

void arp_pending_cleanup_queue(arp_pending_queue_t *q, struct rte_mempool *mp)
{
    if (q == NULL || q->ring == NULL)
        return;

    /* Drain all remaining entries */
    arp_pending_flush(mp, q);

    rte_ring_free(q->ring);
    q->ring = NULL;
}

STATUS arp_pending_enqueue(struct rte_mempool *mp, arp_pending_queue_t *q,
    struct rte_mbuf *mbuf, U32 target_ip)
{
    if (unlikely(mp == NULL || q == NULL || q->ring == NULL))
        return ERROR;

    arp_pending_pkt_t *entry;
    if (rte_mempool_get(mp, (void **)&entry) != 0)
        return ERROR;    /* mempool exhausted */

    entry->mbuf      = mbuf;
    entry->target_ip = target_ip;

    /* Try enqueue; if ring full, drop oldest and retry */
    if (rte_ring_enqueue(q->ring, entry) != 0) {
        arp_pending_pkt_t *oldest;
        if (rte_ring_dequeue(q->ring, (void **)&oldest) == 0) {
            if (oldest->mbuf != NULL)
                rte_pktmbuf_free(oldest->mbuf);
            rte_mempool_put(mp, oldest);
        }
        /* Retry — guaranteed to succeed after dequeue freed a slot */
        if (rte_ring_enqueue(q->ring, entry) != 0) {
            if (entry->mbuf != NULL)
                rte_pktmbuf_free(entry->mbuf);
            rte_mempool_put(mp, entry);
            return ERROR;
        }
    }

    return SUCCESS;
}

void arp_pending_drain(struct rte_mempool *mp, arp_pending_queue_t *q,
    U32 resolved_ip, const struct rte_ether_addr *mac,
    struct rte_mbuf **tx_pkts, U16 *tx_count, U16 tx_max)
{
    if (unlikely(mp == NULL || q == NULL || q->ring == NULL))
        return;

    /*
     * Bulk-dequeue all current entries, classify them, then
     * re-enqueue entries that don't match resolved_ip.
     */
    arp_pending_pkt_t *batch[ARP_PENDING_QUEUE_SIZE];
    unsigned int n = rte_ring_dequeue_burst(q->ring,
        (void **)batch, ARP_PENDING_QUEUE_SIZE, NULL);

    for(unsigned int i=0; i<n; i++) {
        arp_pending_pkt_t *e = batch[i];
        if (e->target_ip == resolved_ip && *tx_count < tx_max) {
            /* Set destination MAC in the Ethernet header */
            struct rte_ether_hdr *eth = rte_pktmbuf_mtod(e->mbuf,
                struct rte_ether_hdr *);
            rte_ether_addr_copy(mac, &eth->dst_addr);
            tx_pkts[(*tx_count)++] = e->mbuf;
            rte_mempool_put(mp, e);
        } else {
            /* Re-enqueue unmatched entry */
            if (rte_ring_enqueue(q->ring, e) != 0) {
                /* Ring full after re-enqueue — drop */
                if (e->mbuf != NULL)
                    rte_pktmbuf_free(e->mbuf);
                rte_mempool_put(mp, e);
            }
        }
    }
}

void arp_pending_flush(struct rte_mempool *mp, arp_pending_queue_t *q)
{
    if (q == NULL || q->ring == NULL)
        return;

    arp_pending_pkt_t *e;
    while (rte_ring_dequeue(q->ring, (void **)&e) == 0) {
        if (e != NULL) {
            if (e->mbuf != NULL)
                rte_pktmbuf_free(e->mbuf);
            if (mp != NULL)
                rte_mempool_put(mp, e);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  ARP request generation                                            */
/* ------------------------------------------------------------------ */

STATUS send_arp_request(const struct rte_ether_addr *src_mac, U32 src_ip,
    U32 target_ip, U16 vlan_id, U16 tx_q)
{
    struct rte_mempool *pool = direct_pool[LAN_PORT];
    if (unlikely(pool == NULL))
        return ERROR;

    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    if (unlikely(m == NULL))
        return ERROR;

    /*
     * Packet layout:
     *   [rte_ether_hdr] [vlan_header_t] [rte_arp_hdr]
     *
     * Total = 14 + 4 + 28 = 46 bytes (padded to 60 by NIC if needed).
     */
    size_t pkt_size = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t)
        + sizeof(struct rte_arp_hdr);

    char *pkt = rte_pktmbuf_append(m, pkt_size);
    if (unlikely(pkt == NULL)) {
        rte_pktmbuf_free(m);
        return ERROR;
    }

    /* --- Ethernet header --- */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)pkt;
    memset(eth->dst_addr.addr_bytes, 0xFF, RTE_ETHER_ADDR_LEN); /* broadcast */
    rte_ether_addr_copy(src_mac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(VLAN);

    /* --- VLAN header --- */
    vlan_header_t *vhdr = (vlan_header_t *)(eth + 1);
    vhdr->tci_union.tci_value = rte_cpu_to_be_16(vlan_id);
    vhdr->next_proto = rte_cpu_to_be_16(FRAME_TYPE_ARP);

    /* --- ARP header --- */
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(vhdr + 1);
    arp->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    arp->arp_protocol = rte_cpu_to_be_16(FRAME_TYPE_IP);
    arp->arp_hlen     = RTE_ETHER_ADDR_LEN;
    arp->arp_plen     = 4;
    arp->arp_opcode   = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);

    /* sender = us (gateway) */
    rte_ether_addr_copy(src_mac, &arp->arp_data.arp_sha);
    arp->arp_data.arp_sip = src_ip;

    /* target = the LAN host we want to resolve */
    memset(arp->arp_data.arp_tha.addr_bytes, 0, RTE_ETHER_ADDR_LEN);
    arp->arp_data.arp_tip = target_ip;

    rte_eth_tx_burst(LAN_PORT, tx_q, &m, 1);
    return SUCCESS;
}
