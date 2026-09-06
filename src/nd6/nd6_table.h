/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  ND6_TABLE.H

     Per-subscriber IPv6 neighbor cache for LAN host resolution.

     ctrl_thread is the only writer; data lcores look entries up lock-free
     and deleted key slots are recycled through the RCU defer queue.

  Designed by THE on 2026/09/06
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _ND6_TABLE_H_
#define _ND6_TABLE_H_

#include <common.h>

#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_rcu_qsbr.h>

#include "../mac_table.h"

#define ND6_TABLE_ENTRIES       1024

/* An entry idle past ND6_NEIGHBOR_TTL_SEC is probed once with a unicast
 * Neighbor Solicitation, then deleted at the next sweep if still idle. */
#define ND6_NEIGHBOR_TTL_SEC    300
#define ND6_AGE_SCAN_SEC        60

typedef struct nd6_table {
    struct rte_hash *hash;        /* key = 16-byte IPv6 address */
    struct rte_rcu_qsbr *rcu;     /* Hash defer queue; NULL = no reclaim */
    U16 generation;               /* RELAXED atomic generation bump */
    U64 learn_fail;
    /* Aging metadata indexed by hash key position; ctrl_thread is the only
     * reader and writer, so no atomics are needed. */
    U32 slot_count;               /* number of positions the hash can return */
    U64 *last_seen;               /* cycle stamp of the last learn */
    U8 *probed;                   /* 1 = a unicast NS probe is outstanding */
} nd6_table_t;

/**
 * @fn nd6_table_alloc
 *
 * @brief Create a subscriber's neighbor cache, whose deleted key slots are
 *        recycled only after every data lcore crosses a quiescent state.
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
 * @brief Learn an IPv6-to-MAC mapping; ctrl_thread is the only allowed caller.
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

#endif
