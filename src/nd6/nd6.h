#ifndef _ND6_H_
#define _ND6_H_

#include <common.h>

#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_timer.h>

#include "../mac_table.h"
#include "../pppd/pppd.h"

#define ND6_TABLE_ENTRIES       1024
#define ND6_RA_INTERVAL_SEC     30
#define ND6_PACKET_MAX_LEN      256

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
    U16 generation;               /* RELAXED atomic generation bump */
    U64 learn_fail;
} nd6_table_t;

nd6_table_t *nd6_table_alloc(U16 ccb_id);
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
void nd6_ra_start(ppp_ccb_t *ppp_ccb);
void nd6_ra_stop(ppp_ccb_t *ppp_ccb);
void nd6_ra_timer_cb(struct rte_timer *tim, void *arg);

void nd6_gateway_link_local(const struct rte_ether_addr *mac, U8 addr[16]);
STATUS nd6_build_ra(ppp_ccb_t *ppp_ccb, U8 *buffer, U16 *packet_len);
STATUS nd6_build_na(ppp_ccb_t *ppp_ccb, const U8 dst_ip[16],
    const struct rte_ether_addr *dst_mac, U32 na_flags, U8 *buffer,
    U16 *packet_len);

#ifdef UNIT_TEST
void nd6_test_tx_reset(void);
const U8 *nd6_test_get_last_tx(U16 *packet_len, U32 *tx_count);
#endif

#endif
