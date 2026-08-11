#ifndef _ND6_H_
#define _ND6_H_

#include <common.h>

#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_rcu_qsbr.h>
#include <rte_timer.h>

#include "../mac_table.h"
#include "../pppd/pppd.h"

#define ND6_TABLE_ENTRIES       1024
#define ND6_RA_INTERVAL_SEC     30
#define ND6_PACKET_MAX_LEN      256

/* Aging: an entry idle for ND6_NEIGHBOR_TTL_SEC is probed once with a unicast
 * Neighbor Solicitation; if the following scan still finds it idle the entry
 * is deleted. ND6_AGE_SCAN_SEC is the sweep period, so an unreachable
 * neighbor is reclaimed within TTL + 2 sweeps. */
#define ND6_NEIGHBOR_TTL_SEC    300
#define ND6_AGE_SCAN_SEC        60

#define ND6_ICMP_RS             133
#define ND6_ICMP_RA             134
#define ND6_ICMP_NS             135
#define ND6_ICMP_NA             136

#define ND6_OPT_SLLA            1
#define ND6_OPT_TLLA            2
#define ND6_OPT_PREFIX_INFO     3
#define ND6_OPT_RDNSS           25

#define ND6_NA_FLAG_ROUTER      UINT32_C(0x80000000)
#define ND6_NA_FLAG_SOLICITED   UINT32_C(0x40000000)
#define ND6_NA_FLAG_OVERRIDE    UINT32_C(0x20000000)

typedef struct nd6_table {
    struct rte_hash *hash;        /* key = 16-byte IPv6 address */
    struct rte_rcu_qsbr *rcu;     /* QSBR driving the hash defer queue; NULL = none */
    U16 generation;               /* RELAXED atomic generation bump */
    U64 learn_fail;
    /* Aging metadata, indexed by the hash key position. Only ctrl_thread ever
     * reads or writes these arrays (learn, aging scan and reset all run
     * there), so they need no atomics; data-plane readers only touch the
     * hash itself. */
    U32 slot_count;               /* number of positions the hash can return */
    U64 *last_seen;               /* cycle stamp of the last learn per position */
    U8 *probed;                   /* 1 = a unicast NS probe is outstanding */
} nd6_table_t;

/**
 * @fn nd6_table_alloc
 *
 * @brief Create a subscriber's neighbor cache: a fixed-capacity lock-free
 *        rte_hash plus its aging metadata. The hash is attached to the shared
 *        QSBR variable in deferred-reclaim mode, so a deleted key's slot is
 *        recycled only after every registered data lcore has crossed a
 *        quiescent state.
 *
 * @param ccb_id
 *      Subscriber index; makes the rte_hash name unique process-wide
 * @param rcu
 *      QSBR variable the data lcores report quiescent states on
 * @return
 *      Table handle on success, NULL on failure
 */
nd6_table_t *nd6_table_alloc(U16 ccb_id, struct rte_rcu_qsbr *rcu);
void nd6_table_free(nd6_table_t *table);
void nd6_table_reset(nd6_table_t *table);

/**
 * @brief Learn an IPv6-to-MAC mapping. Only ctrl_thread may call this writer.
 */
void nd6_table_learn(nd6_table_t *table, const U8 ipv6[16],
    const struct rte_ether_addr *mac);

/**
 * @brief Lock-free lookup for data-plane readers.
 */
static __always_inline STATUS nd6_table_lookup(nd6_table_t *table,
    const U8 ipv6[16], struct rte_ether_addr *mac_out)
{
    void *data;

    if (unlikely(table == NULL || ipv6 == NULL || mac_out == NULL))
        return ERROR;
    if (rte_hash_lookup_data(table->hash, ipv6, &data) < 0)
        return ERROR;
    U16 generation = mac_table_unpack((uintptr_t)data, mac_out);
    if (unlikely(generation !=
            __atomic_load_n(&table->generation, __ATOMIC_RELAXED)))
        return ERROR;
    return SUCCESS;
}

void nd6_lan_input(FastRG_t *fastrg_ccb, U16 ccb_id, U8 *pkt, U16 len);

/**
 * @fn nd6_wan_miss_input
 *
 * @brief Control-plane handler for a WAN->LAN packet whose destination has no
 *        neighbor cache entry. Re-checks the forwarding gate and the
 *        destination prefix, retries the lookup (the address may have been
 *        learned since the data plane escalated) and, when it is still
 *        unresolved, solicits it with a multicast Neighbor Solicitation.
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @param ccb_id
 *      Subscriber index
 * @param pkt
 *      Full PPPoE frame as received on the WAN port
 * @param len
 *      Frame length in bytes
 * @return
 *      void
 */
void nd6_wan_miss_input(FastRG_t *fastrg_ccb, U16 ccb_id, U8 *pkt, U16 len);
void nd6_ra_start(ppp_ccb_t *ppp_ccb);
void nd6_ra_stop(ppp_ccb_t *ppp_ccb);
void nd6_ra_timer_cb(struct rte_timer *tim, void *arg);

/**
 * @fn nd6_age_scan_table
 *
 * @brief Sweep one subscriber's neighbor cache: delete entries left over from
 *        an older generation, probe entries idle for longer than
 *        ND6_NEIGHBOR_TTL_SEC, delete the ones that did not answer the
 *        previous probe, then run one deferred-reclaim pass so freed key slots
 *        become allocatable again.
 *
 * @param ppp_ccb
 *      Subscriber control block (NULL tolerated)
 * @param now
 *      Current cycle stamp used as the aging reference
 * @return
 *      void
 */
void nd6_age_scan_table(ppp_ccb_t *ppp_ccb, U64 now);

/**
 * @fn nd6_age_timer_cb
 *
 * @brief Periodic timer callback that runs nd6_age_scan_table() over every
 *        configured subscriber. Registered on the control-plane lcore.
 *
 * @param tim
 *      Timer being serviced (unused)
 * @param arg
 *      FastRG control block
 * @return
 *      void
 */
void nd6_age_timer_cb(struct rte_timer *tim, void *arg);

void nd6_gateway_link_local(const struct rte_ether_addr *mac, U8 addr[16]);
STATUS nd6_build_ra(ppp_ccb_t *ppp_ccb, U8 *buffer, U16 *packet_len);
STATUS nd6_build_na(ppp_ccb_t *ppp_ccb, const U8 dst_ip[16],
    const struct rte_ether_addr *dst_mac, U32 na_flags, U8 *buffer,
    U16 *packet_len);

/**
 * @fn nd6_build_ns
 *
 * @brief Build a Neighbor Solicitation for target, addressed to dst_ip /
 *        dst_mac. Used both for unicast reachability probes (dst = target)
 *        and for address resolution (dst = the target's solicited-node
 *        multicast address).
 *
 * @param ppp_ccb
 *      Subscriber control block
 * @param target
 *      IPv6 address being solicited
 * @param dst_ip
 *      IPv6 destination of the solicitation
 * @param dst_mac
 *      Ethernet destination of the solicitation
 * @param buffer
 *      Output buffer of at least ND6_PACKET_MAX_LEN bytes
 * @param[out] packet_len
 *      Frame length written
 * @return
 *      SUCCESS on success, ERROR when IPv6 is not ready or arguments are NULL
 */
STATUS nd6_build_ns(ppp_ccb_t *ppp_ccb, const U8 target[16],
    const U8 dst_ip[16], const struct rte_ether_addr *dst_mac, U8 *buffer,
    U16 *packet_len);

/**
 * @fn nd6_solicited_node_addr
 *
 * @brief Derive the solicited-node multicast address ff02::1:ffXX:XXXX of an
 *        IPv6 address.
 *
 * @param target
 *      IPv6 address to derive from
 * @param[out] addr
 *      Resulting multicast address
 * @return
 *      void
 */
void nd6_solicited_node_addr(const U8 target[16], U8 addr[16]);

#ifdef UNIT_TEST
void nd6_test_tx_reset(void);
const U8 *nd6_test_get_last_tx(U16 *packet_len, U32 *tx_count);
#endif

#endif
