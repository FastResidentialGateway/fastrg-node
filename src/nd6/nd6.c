#include <stdlib.h>
#include <string.h>

#include <common.h>

#include <rte_byteorder.h>
#include <rte_errno.h>
#include <rte_hash_crc.h>
#include <rte_ip6.h>
#include <rte_memcpy.h>

#include "nd6.h"
#include "../dbg.h"
#include "../dp.h"
#include "../protocol.h"
#include "../utils.h"
#include "../pppd/header.h"

#define ND6_ROUTER_LIFETIME_SEC    1800
#define ND6_RDNSS_LIFETIME_SEC     1800
#define ND6_PREFIX_VALID_SEC        86400
#define ND6_PREFIX_PREFERRED_SEC    43200
#define ND6_RA_CUR_HOP_LIMIT        64

/* RFC 4861 MIN_DELAY_BETWEEN_RAS, applied to RS-triggered advertisements. */
#define ND6_RS_RA_MIN_DELAY_SEC     3

typedef struct nd6_icmp_header {
    U8 type;
    U8 code;
    U16 checksum;
    U32 data;
} __rte_packed nd6_icmp_header_t;

typedef struct nd6_ra_header {
    U8 type;
    U8 code;
    U16 checksum;
    U8 current_hop_limit;
    U8 flags;
    U16 router_lifetime;
    U32 reachable_time;
    U32 retrans_timer;
} __rte_packed nd6_ra_header_t;

typedef struct nd6_neighbor_message {
    nd6_icmp_header_t icmp;
    U8 target[16];
} __rte_packed nd6_neighbor_message_t;

typedef struct nd6_ll_option {
    U8 type;
    U8 length;
    U8 mac[RTE_ETHER_ADDR_LEN];
} __rte_packed nd6_ll_option_t;

typedef struct nd6_prefix_option {
    U8 type;
    U8 length;
    U8 prefix_length;
    U8 flags;
    U32 valid_lifetime;
    U32 preferred_lifetime;
    U32 reserved;
    U8 prefix[16];
} __rte_packed nd6_prefix_option_t;

#ifdef UNIT_TEST
static U8 nd6_test_last_tx[ND6_PACKET_MAX_LEN];
static U16 nd6_test_last_tx_len;
static U32 nd6_test_tx_count;
#endif

static BOOL nd6_gate_ready(const ppp_ccb_t *ppp_ccb)
{
    return ppp_ccb != NULL && ppp_ccb->ipv6_enabled != FALSE &&
        ppp_ccb->ipv6cp_up != FALSE && ppp_ccb->dhcp6_pd_ready != FALSE;
}

/* Only link-local sources or addresses inside the advertised LAN /64 may
 * populate the neighbor cache; multicast and off-link sources are discarded
 * before they can consume an entry. Capacity is bounded by the aging sweep
 * (nd6_age_scan_table), which reclaims idle and stale-generation entries, so
 * a host churning addresses can at worst fill its own subscriber's table
 * until the next sweep. */
static BOOL nd6_src_is_on_link(const ppp_ccb_t *ppp_ccb, const U8 src_ip[16])
{
    if (src_ip[0] == 0xff)
        return FALSE;
    if (src_ip[0] == 0xfe && (src_ip[1] & 0xc0) == 0x80)
        return TRUE;
    return memcmp(src_ip, ppp_ccb->hsi_ipv6_lan_prefix, 8) == 0;
}

static BOOL nd6_addr_is_zero(const U8 addr[16])
{
    U8 value = 0;

    for(U8 i=0; i<16; i++)
        value |= addr[i];
    return value == 0;
}

static void nd6_all_nodes(U8 addr[16])
{
    memset(addr, 0, 16);
    addr[0] = 0xff;
    addr[1] = 0x02;
    addr[15] = 0x01;
}

static void nd6_multicast_mac(const U8 addr[16],
    struct rte_ether_addr *mac)
{
    mac->addr_bytes[0] = 0x33;
    mac->addr_bytes[1] = 0x33;
    rte_memcpy(&mac->addr_bytes[2], &addr[12], 4);
}

void nd6_solicited_node_addr(const U8 target[16], U8 addr[16])
{
    memset(addr, 0, 16);
    addr[0] = 0xff;
    addr[1] = 0x02;
    addr[11] = 0x01;
    addr[12] = 0xff;
    addr[13] = target[13];
    addr[14] = target[14];
    addr[15] = target[15];
}

void nd6_gateway_link_local(const struct rte_ether_addr *mac, U8 addr[16])
{
    memset(addr, 0, 16);
    addr[0] = 0xfe;
    addr[1] = 0x80;
    addr[8] = mac->addr_bytes[0] ^ 0x02;
    addr[9] = mac->addr_bytes[1];
    addr[10] = mac->addr_bytes[2];
    addr[11] = 0xff;
    addr[12] = 0xfe;
    addr[13] = mac->addr_bytes[3];
    addr[14] = mac->addr_bytes[4];
    addr[15] = mac->addr_bytes[5];
}

nd6_table_t *nd6_table_alloc(U16 ccb_id, struct rte_rcu_qsbr *rcu)
{
    nd6_table_t *table = fastrg_calloc(nd6_table_t, 1,
        sizeof(nd6_table_t), 0);
    char name[RTE_HASH_NAMESIZE];
    int32_t max_key_id;

    if (table == NULL)
        return NULL;
    snprintf(name, sizeof(name), "nd6_tbl_%u", ccb_id);
    struct rte_hash_parameters params = {
        .name = name,
        .entries = ND6_TABLE_ENTRIES,
        .key_len = 16,
        .hash_func = rte_hash_crc,
        .hash_func_init_val = 0,
        .socket_id = (int)rte_socket_id(),
        /* One ctrl_thread writer publishes packed words to lock-free readers. */
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF,
    };
    table->hash = rte_hash_create(&params);
    if (table->hash == NULL)
        goto err;

    if (rcu != NULL) {
        /* Deferred reclaim, same arrangement as the NAT hashes: a deleted key
         * stops being visible to readers immediately, while its key slot is
         * handed back only after every registered data lcore reports a
         * quiescent state. The data word is a packed value rather than a
         * pointer, so no free callback is needed. */
        struct rte_hash_rcu_config rcu_cfg = {
            .v = rcu,
            .mode = RTE_HASH_QSBR_MODE_DQ,
            /* One sweep can delete every entry, so let one reclaim pass hand
             * all of them back instead of the 16-per-call default that would
             * spread a full table over dozens of sweeps. */
            .max_reclaim_size = ND6_TABLE_ENTRIES,
            .free_key_data_func = NULL,
        };
        if (rte_hash_rcu_qsbr_add(table->hash, &rcu_cfg) != 0)
            goto err;
        table->rcu = rcu;
    }

    max_key_id = rte_hash_max_key_id(table->hash);
    if (max_key_id < 0)
        goto err;
    table->slot_count = (U32)max_key_id + 1;
    table->last_seen = fastrg_calloc(U64, table->slot_count, sizeof(U64), 0);
    table->probed = fastrg_calloc(U8, table->slot_count, sizeof(U8), 0);
    if (table->last_seen == NULL || table->probed == NULL)
        goto err;
    return table;

err:
    nd6_table_free(table);
    return NULL;
}

void nd6_table_free(nd6_table_t *table)
{
    if (table == NULL)
        return;
    if (table->hash != NULL)
        rte_hash_free(table->hash);
    if (table->last_seen != NULL)
        fastrg_mfree(table->last_seen);
    if (table->probed != NULL)
        fastrg_mfree(table->probed);
    fastrg_mfree(table);
}

void nd6_table_reset(nd6_table_t *table)
{
    if (table == NULL)
        return;
    /* A generation bump invalidates every value without restructuring the
     * hash while data-plane readers may be active. The now-stale keys still
     * occupy their slots; the aging sweep deletes them and lets the deferred
     * reclaim hand the slots back. */
    __atomic_fetch_add(&table->generation, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&table->learn_fail, 0, __ATOMIC_RELAXED);
}

/* Run one deferred-reclaim pass so key slots freed by earlier deletes become
 * allocatable again. No-op when the table was created without an RCU. */
static void nd6_table_reclaim(nd6_table_t *table)
{
    U32 freed, pending, avail;

    if (table->rcu == NULL)
        return;
    rte_hash_rcu_qsbr_dq_reclaim(table->hash, &freed, &pending, &avail);
}

/* Refresh a position's aging state after a learn. ctrl_thread-only, so plain
 * loads and stores are enough. */
static void nd6_table_touch(nd6_table_t *table, int32_t position)
{
    if (position < 0 || (U32)position >= table->slot_count)
        return;
    table->last_seen[position] = fastrg_get_cur_cycles();
    table->probed[position] = 0;
}

void nd6_table_learn(nd6_table_t *table, const U8 ipv6[16],
    const struct rte_ether_addr *mac)
{
    void *data;
    U16 generation;
    uintptr_t packed;
    int32_t position;

    if (table == NULL || ipv6 == NULL || mac == NULL)
        return;
    generation = __atomic_load_n(&table->generation, __ATOMIC_RELAXED);
    packed = mac_table_pack(mac, generation);
    position = rte_hash_lookup_data(table->hash, ipv6, &data);
    if (position >= 0 && (uintptr_t)data == packed) {
        /* Already current: only the idle timer needs refreshing. */
        nd6_table_touch(table, position);
        return;
    }
    if (rte_hash_add_key_data(table->hash, ipv6, (void *)packed) < 0) {
        /* The table may only look full because deleted slots are still
         * waiting on the defer queue; drain what is due and retry once. */
        nd6_table_reclaim(table);
        if (rte_hash_add_key_data(table->hash, ipv6, (void *)packed) < 0) {
            __atomic_fetch_add(&table->learn_fail, 1, __ATOMIC_RELAXED);
            return;
        }
    }
    nd6_table_touch(table, rte_hash_lookup(table->hash, ipv6));
}

static void nd6_build_l2(ppp_ccb_t *ppp_ccb,
    const struct rte_ether_addr *dst_mac, U8 *buffer)
{
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buffer;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);

    rte_ether_addr_copy(dst_mac, &eth->dst_addr);
    rte_ether_addr_copy(&ppp_ccb->fastrg_ccb->nic_info.hsi_lan_mac,
        &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(VLAN);
    vlan->tci_union.tci_value = rte_cpu_to_be_16(
        rte_atomic16_read(&ppp_ccb->vlan_id) & 0x0fff);
    vlan->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IPV6);
}

static struct rte_ipv6_hdr *nd6_build_ip6(ppp_ccb_t *ppp_ccb,
    const U8 dst_ip[16], U16 payload_len, U8 *buffer)
{
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buffer;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(vlan + 1);

    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(6) << 28);
    ip6->payload_len = rte_cpu_to_be_16(payload_len);
    ip6->proto = IPPROTO_ICMPV6;
    ip6->hop_limits = 255;
    nd6_gateway_link_local(&ppp_ccb->fastrg_ccb->nic_info.hsi_lan_mac,
        (U8 *)&ip6->src_addr);
    rte_memcpy(&ip6->dst_addr, dst_ip, 16);
    return ip6;
}

STATUS nd6_build_ra(ppp_ccb_t *ppp_ccb, U8 *buffer, U16 *packet_len)
{
    struct rte_ether_addr dst_mac;
    struct rte_ipv6_hdr *ip6;
    nd6_ra_header_t *ra;
    nd6_prefix_option_t *pio;
    U8 dst_ip[16];
    U8 *cursor;
    U8 dns_count = 0;
    U16 icmp_len;

    if (!nd6_gate_ready(ppp_ccb) || buffer == NULL || packet_len == NULL)
        return ERROR;
    memset(buffer, 0, ND6_PACKET_MAX_LEN);
    nd6_all_nodes(dst_ip);
    nd6_multicast_mac(dst_ip, &dst_mac);
    nd6_build_l2(ppp_ccb, &dst_mac, buffer);

    ra = (nd6_ra_header_t *)(buffer + sizeof(struct rte_ether_hdr) +
        sizeof(vlan_header_t) + sizeof(struct rte_ipv6_hdr));
    cursor = (U8 *)(ra + 1);
    ra->type = ND6_ICMP_RA;
    ra->current_hop_limit = ND6_RA_CUR_HOP_LIMIT;
    ra->router_lifetime = rte_cpu_to_be_16(ND6_ROUTER_LIFETIME_SEC);

    pio = (nd6_prefix_option_t *)cursor;
    pio->type = ND6_OPT_PREFIX_INFO;
    pio->length = sizeof(*pio) / 8;
    pio->prefix_length = 64;
    pio->flags = 0xc0; /* on-link + autonomous address configuration */
    /* IA_PD retains the prefix and T1 but not its preferred/valid lifetimes,
     * so advertisements use the matching BRAS contract constants. */
    pio->valid_lifetime = rte_cpu_to_be_32(ND6_PREFIX_VALID_SEC);
    pio->preferred_lifetime = rte_cpu_to_be_32(ND6_PREFIX_PREFERRED_SEC);
    rte_memcpy(pio->prefix, ppp_ccb->hsi_ipv6_lan_prefix, 16);
    cursor += sizeof(*pio);

    for(U8 i=0; i<2; i++) {
        if (!nd6_addr_is_zero(ppp_ccb->hsi_ipv6_dns[i]))
            dns_count++;
    }
    if (dns_count > 0) {
        U8 *rdnss = cursor;
        rdnss[0] = ND6_OPT_RDNSS;
        rdnss[1] = 1 + 2 * dns_count;
        U32 lifetime = rte_cpu_to_be_32(ND6_RDNSS_LIFETIME_SEC);
        rte_memcpy(&rdnss[4], &lifetime, sizeof(lifetime));
        cursor += 8;
        for(U8 i=0; i<2; i++) {
            if (nd6_addr_is_zero(ppp_ccb->hsi_ipv6_dns[i]))
                continue;
            rte_memcpy(cursor, ppp_ccb->hsi_ipv6_dns[i], 16);
            cursor += 16;
        }
    }

    nd6_ll_option_t *slla = (nd6_ll_option_t *)cursor;
    slla->type = ND6_OPT_SLLA;
    slla->length = 1;
    rte_memcpy(slla->mac,
        ppp_ccb->fastrg_ccb->nic_info.hsi_lan_mac.addr_bytes,
        RTE_ETHER_ADDR_LEN);
    cursor += sizeof(*slla);

    icmp_len = (U16)(cursor - (U8 *)ra);
    ip6 = nd6_build_ip6(ppp_ccb, dst_ip, icmp_len, buffer);
    ra->checksum = 0;
    ra->checksum = rte_ipv6_udptcp_cksum(ip6, ra);
    *packet_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        sizeof(struct rte_ipv6_hdr) + icmp_len;
    return SUCCESS;
}

STATUS nd6_build_na(ppp_ccb_t *ppp_ccb, const U8 dst_ip[16],
    const struct rte_ether_addr *dst_mac, U32 na_flags, U8 *buffer,
    U16 *packet_len)
{
    struct rte_ipv6_hdr *ip6;
    nd6_neighbor_message_t *na;
    nd6_ll_option_t *tlla;
    U16 icmp_len = sizeof(*na) + sizeof(*tlla);

    if (!nd6_gate_ready(ppp_ccb) || dst_ip == NULL || dst_mac == NULL ||
            buffer == NULL || packet_len == NULL)
        return ERROR;
    memset(buffer, 0, ND6_PACKET_MAX_LEN);
    nd6_build_l2(ppp_ccb, dst_mac, buffer);
    ip6 = nd6_build_ip6(ppp_ccb, dst_ip, icmp_len, buffer);
    na = (nd6_neighbor_message_t *)(ip6 + 1);
    na->icmp.type = ND6_ICMP_NA;
    na->icmp.data = rte_cpu_to_be_32(na_flags);
    nd6_gateway_link_local(&ppp_ccb->fastrg_ccb->nic_info.hsi_lan_mac,
        na->target);
    tlla = (nd6_ll_option_t *)(na + 1);
    tlla->type = ND6_OPT_TLLA;
    tlla->length = 1;
    rte_memcpy(tlla->mac,
        ppp_ccb->fastrg_ccb->nic_info.hsi_lan_mac.addr_bytes,
        RTE_ETHER_ADDR_LEN);
    na->icmp.checksum = 0;
    na->icmp.checksum = rte_ipv6_udptcp_cksum(ip6, na);
    *packet_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        sizeof(struct rte_ipv6_hdr) + icmp_len;
    return SUCCESS;
}

STATUS nd6_build_ns(ppp_ccb_t *ppp_ccb, const U8 target[16],
    const U8 dst_ip[16], const struct rte_ether_addr *dst_mac, U8 *buffer,
    U16 *packet_len)
{
    struct rte_ipv6_hdr *ip6;
    nd6_neighbor_message_t *ns;
    nd6_ll_option_t *slla;
    U16 icmp_len = sizeof(*ns) + sizeof(*slla);

    if (!nd6_gate_ready(ppp_ccb) || target == NULL || dst_ip == NULL ||
            dst_mac == NULL || buffer == NULL || packet_len == NULL)
        return ERROR;
    memset(buffer, 0, ND6_PACKET_MAX_LEN);
    nd6_build_l2(ppp_ccb, dst_mac, buffer);
    ip6 = nd6_build_ip6(ppp_ccb, dst_ip, icmp_len, buffer);
    ns = (nd6_neighbor_message_t *)(ip6 + 1);
    ns->icmp.type = ND6_ICMP_NS;
    rte_memcpy(ns->target, target, 16);
    slla = (nd6_ll_option_t *)(ns + 1);
    slla->type = ND6_OPT_SLLA;
    slla->length = 1;
    rte_memcpy(slla->mac,
        ppp_ccb->fastrg_ccb->nic_info.hsi_lan_mac.addr_bytes,
        RTE_ETHER_ADDR_LEN);
    ns->icmp.checksum = 0;
    ns->icmp.checksum = rte_ipv6_udptcp_cksum(ip6, ns);
    *packet_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        sizeof(struct rte_ipv6_hdr) + icmp_len;
    return SUCCESS;
}

static void nd6_send(FastRG_t *fastrg_ccb, U16 ccb_id, const U8 *packet,
    U16 packet_len)
{
#ifdef UNIT_TEST
    rte_memcpy(nd6_test_last_tx, packet, packet_len);
    nd6_test_last_tx_len = packet_len;
    nd6_test_tx_count++;
#else
    lan_ctrl_tx(fastrg_ccb, ccb_id, (U8 *)packet, packet_len);
#endif
}

static void nd6_send_ra(ppp_ccb_t *ppp_ccb)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U16 packet_len;

    if (nd6_build_ra(ppp_ccb, packet, &packet_len) == SUCCESS)
        nd6_send(ppp_ccb->fastrg_ccb, ppp_ccb->user_num - 1, packet,
            packet_len);
}

static void nd6_send_ns(ppp_ccb_t *ppp_ccb, const U8 target[16],
    const U8 dst_ip[16], const struct rte_ether_addr *dst_mac)
{
    U8 packet[ND6_PACKET_MAX_LEN];
    U16 packet_len;

    if (nd6_build_ns(ppp_ccb, target, dst_ip, dst_mac, packet,
            &packet_len) == SUCCESS)
        nd6_send(ppp_ccb->fastrg_ccb, ppp_ccb->user_num - 1, packet,
            packet_len);
}

static STATUS nd6_parse_ll_options(const U8 *options, U16 len,
    U8 wanted_type, struct rte_ether_addr *mac, BOOL *found)
{
    *found = FALSE;
    while (len > 0) {
        U16 option_len;

        if (len < 2 || options[1] == 0)
            return ERROR;
        option_len = (U16)options[1] * 8;
        if (option_len > len)
            return ERROR;
        if (options[0] == wanted_type) {
            if (*found != FALSE || option_len != sizeof(nd6_ll_option_t))
                return ERROR;
            rte_memcpy(mac->addr_bytes, options + 2, RTE_ETHER_ADDR_LEN);
            *found = TRUE;
        }
        options += option_len;
        len -= option_len;
    }
    return SUCCESS;
}

void nd6_lan_input(FastRG_t *fastrg_ccb, U16 ccb_id, U8 *pkt, U16 len)
{
    ppp_ccb_t *ppp_ccb;
    struct rte_ether_hdr *eth;
    vlan_header_t *vlan;
    struct rte_ipv6_hdr *ip6;
    nd6_icmp_header_t *icmp;
    U16 l2_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t);
    U16 payload_len;
    U8 *src_ip;

    if (fastrg_ccb == NULL || pkt == NULL || ccb_id >= fastrg_ccb->user_count)
        return;
    ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    if (!nd6_gate_ready(ppp_ccb) || len < l2_len + sizeof(*ip6) +
            sizeof(*icmp))
        return;

    eth = (struct rte_ether_hdr *)pkt;
    vlan = (vlan_header_t *)(eth + 1);
    ip6 = (struct rte_ipv6_hdr *)(vlan + 1);
    payload_len = rte_be_to_cpu_16(ip6->payload_len);
    if (vlan->next_proto != rte_cpu_to_be_16(FRAME_TYPE_IPV6) ||
            rte_ipv6_check_version(ip6) != 0 ||
            ip6->proto != IPPROTO_ICMPV6 || ip6->hop_limits != 255 ||
            payload_len < sizeof(*icmp) ||
            payload_len != len - l2_len - sizeof(*ip6))
        return;
    icmp = (nd6_icmp_header_t *)(ip6 + 1);
    if (icmp->code != 0 ||
            rte_ipv6_udptcp_cksum_verify(ip6, icmp) != 0)
        return;
    src_ip = (U8 *)&ip6->src_addr;

    if (icmp->type == ND6_ICMP_RS) {
        struct rte_ether_addr ignored_mac;
        BOOL ignored_found;
        U64 now;

        if (nd6_parse_ll_options((U8 *)(icmp + 1),
                payload_len - sizeof(*icmp), ND6_OPT_SLLA, &ignored_mac,
                &ignored_found) == ERROR)
            return;
        /* A solicitation flood collapses into one advertisement per window;
         * the periodic timer covers any host whose solicitation was
         * absorbed. */
        now = fastrg_get_cur_cycles();
        if (now - ppp_ccb->last_rs_ra_cycles <
                (U64)ND6_RS_RA_MIN_DELAY_SEC * fastrg_get_cycles_in_sec())
            return;
        ppp_ccb->last_rs_ra_cycles = now;
        nd6_send_ra(ppp_ccb);
        return;
    }

    if (icmp->type == ND6_ICMP_NS) {
        nd6_neighbor_message_t *ns = (nd6_neighbor_message_t *)icmp;
        struct rte_ether_addr learned_mac;
        BOOL has_slla;
        U8 gateway[16];

        if (payload_len < sizeof(*ns) ||
                nd6_parse_ll_options((U8 *)(ns + 1),
                    payload_len - sizeof(*ns), ND6_OPT_SLLA, &learned_mac,
                    &has_slla) == ERROR)
            return;
        nd6_gateway_link_local(&fastrg_ccb->nic_info.hsi_lan_mac, gateway);
        if (nd6_addr_is_zero(src_ip)) {
            /* An unspecified source is Duplicate Address Detection. A probe
             * for the gateway address is a claim attempt and is defended
             * with an unsolicited all-nodes NA; every other target stays
             * unanswered so host DAD can proceed. */
            if (memcmp(ns->target, gateway, sizeof(gateway)) == 0) {
                U8 response[ND6_PACKET_MAX_LEN];
                U8 all_nodes[16];
                struct rte_ether_addr all_nodes_mac;
                U16 response_len;

                nd6_all_nodes(all_nodes);
                nd6_multicast_mac(all_nodes, &all_nodes_mac);
                if (nd6_build_na(ppp_ccb, all_nodes, &all_nodes_mac,
                        ND6_NA_FLAG_ROUTER | ND6_NA_FLAG_OVERRIDE,
                        response, &response_len) == SUCCESS)
                    nd6_send(fastrg_ccb, ccb_id, response, response_len);
            }
            return;
        }
        if (!nd6_src_is_on_link(ppp_ccb, src_ip))
            return;
        if (has_slla)
            nd6_table_learn(ppp_ccb->nd6_table, src_ip, &learned_mac);
        if (memcmp(ns->target, gateway, sizeof(gateway)) == 0) {
            U8 response[ND6_PACKET_MAX_LEN];
            U16 response_len;

            if (nd6_build_na(ppp_ccb, src_ip, &eth->src_addr,
                    ND6_NA_FLAG_ROUTER | ND6_NA_FLAG_SOLICITED |
                    ND6_NA_FLAG_OVERRIDE, response,
                    &response_len) == SUCCESS)
                nd6_send(fastrg_ccb, ccb_id, response, response_len);
        }
        return;
    }

    if (icmp->type == ND6_ICMP_NA) {
        nd6_neighbor_message_t *na = (nd6_neighbor_message_t *)icmp;
        struct rte_ether_addr learned_mac;
        BOOL has_tlla;

        if (payload_len < sizeof(*na) ||
                nd6_parse_ll_options((U8 *)(na + 1),
                    payload_len - sizeof(*na), ND6_OPT_TLLA, &learned_mac,
                    &has_tlla) == ERROR || nd6_addr_is_zero(src_ip) ||
                !nd6_src_is_on_link(ppp_ccb, src_ip))
            return;
        if (has_tlla)
            nd6_table_learn(ppp_ccb->nd6_table, src_ip, &learned_mac);
    }
}

void nd6_ra_start(ppp_ccb_t *ppp_ccb)
{
    if (!nd6_gate_ready(ppp_ccb))
        return;
    nd6_ra_stop(ppp_ccb);
    nd6_send_ra(ppp_ccb);
    rte_timer_reset(&ppp_ccb->ra_timer,
        (U64)ND6_RA_INTERVAL_SEC * fastrg_get_cycles_in_sec(), PERIODICAL,
        ppp_ccb->fastrg_ccb->lcore.ctrl_thread, nd6_ra_timer_cb, ppp_ccb);
}

void nd6_ra_stop(ppp_ccb_t *ppp_ccb)
{
    if (ppp_ccb == NULL)
        return;
    /* PD loss stops future advertisements and replies. No lifetime-zero
     * withdrawal is sent; hosts age state by its advertised lifetimes. */
    rte_timer_stop(&ppp_ccb->ra_timer);
}

void nd6_ra_timer_cb(__attribute__((unused)) struct rte_timer *tim, void *arg)
{
    ppp_ccb_t *ppp_ccb = (ppp_ccb_t *)arg;

    if (!nd6_gate_ready(ppp_ccb)) {
        nd6_ra_stop(ppp_ccb);
        return;
    }
    nd6_send_ra(ppp_ccb);
}

void nd6_age_scan_table(ppp_ccb_t *ppp_ccb, U64 now)
{
    nd6_table_t *table;
    const void *key;
    void *data;
    U32 next = 0;
    int32_t position;
    U16 generation;
    U64 ttl_cycles;

    if (ppp_ccb == NULL || ppp_ccb->nd6_table == NULL)
        return;
    table = ppp_ccb->nd6_table;
    generation = __atomic_load_n(&table->generation, __ATOMIC_RELAXED);
    ttl_cycles = (U64)ND6_NEIGHBOR_TTL_SEC * fastrg_get_cycles_in_sec();
    /* Deleting the entry the iterator just returned is safe: the cursor has
     * already moved past its bucket slot and no other entry is relocated. */
    while ((position = rte_hash_iterate(table->hash, &key, &data,
            &next)) >= 0) {
        struct rte_ether_addr mac;
        U8 entry_ip[16];

        rte_memcpy(entry_ip, key, 16);
        if (mac_table_unpack((uintptr_t)data, &mac) != generation) {
            /* Left over from a table reset or a prefix change: lookups
             * already reject it, so the slot is pure waste. */
            rte_hash_del_key(table->hash, entry_ip);
            continue;
        }
        if ((U32)position >= table->slot_count ||
                table->last_seen[position] + ttl_cycles >= now)
            continue;
        if (table->probed[position] != 0) {
            /* The probe from the previous sweep went unanswered. */
            rte_hash_del_key(table->hash, entry_ip);
            continue;
        }
        /* One unicast reachability probe; an answering host re-learns through
         * nd6_lan_input, which refreshes last_seen and clears probed. */
        table->probed[position] = 1;
        nd6_send_ns(ppp_ccb, entry_ip, entry_ip, &mac);
    }
    /* Hand back the key slots freed by this sweep and by any earlier one
     * whose grace period has since completed. */
    nd6_table_reclaim(table);
}

void nd6_age_timer_cb(__attribute__((unused)) struct rte_timer *tim, void *arg)
{
    FastRG_t *fastrg_ccb = (FastRG_t *)arg;
    U64 now;

    if (fastrg_ccb == NULL)
        return;
    now = fastrg_get_cur_cycles();
    for(U16 i=0; i<fastrg_ccb->user_count; i++)
        nd6_age_scan_table(PPPD_GET_CCB(fastrg_ccb, i), now);
}

void nd6_wan_miss_input(FastRG_t *fastrg_ccb, U16 ccb_id, U8 *pkt, U16 len)
{
    ppp_ccb_t *ppp_ccb;
    vlan_header_t *vlan;
    ppp_payload_t *ppp_payload;
    struct rte_ipv6_hdr *ip6;
    struct rte_ether_addr resolved_mac, solicited_mac;
    U8 solicited[16];
    U8 *dst_ip;
    U16 l2_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t);
    U16 ip6_offset = l2_len + sizeof(pppoe_header_t) + sizeof(ppp_payload_t);

    if (fastrg_ccb == NULL || pkt == NULL || ccb_id >= fastrg_ccb->user_count)
        return;
    ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    if (!nd6_gate_ready(ppp_ccb) || len < ip6_offset + sizeof(*ip6))
        return;

    vlan = (vlan_header_t *)(pkt + sizeof(struct rte_ether_hdr));
    ppp_payload = (ppp_payload_t *)(pkt + l2_len + sizeof(pppoe_header_t));
    if (vlan->next_proto != rte_cpu_to_be_16(ETH_P_PPP_SES) ||
            ppp_payload->ppp_protocol !=
                rte_cpu_to_be_16(PPP_IPV6_PROTOCOL))
        return;

    ip6 = (struct rte_ipv6_hdr *)(pkt + ip6_offset);
    dst_ip = (U8 *)&ip6->dst_addr;
    if (rte_ipv6_check_version(ip6) != 0 ||
            memcmp(dst_ip, ppp_ccb->hsi_ipv6_lan_prefix, 8) != 0)
        return;
    /* The address may have been learned between the data plane's lookup and
     * this handler; only a still-unresolved one needs soliciting. */
    if (nd6_table_lookup(ppp_ccb->nd6_table, dst_ip, &resolved_mac) == SUCCESS)
        return;
    nd6_solicited_node_addr(dst_ip, solicited);
    nd6_multicast_mac(solicited, &solicited_mac);
    nd6_send_ns(ppp_ccb, dst_ip, solicited, &solicited_mac);
}

#ifdef UNIT_TEST
void nd6_test_tx_reset(void)
{
    memset(nd6_test_last_tx, 0, sizeof(nd6_test_last_tx));
    nd6_test_last_tx_len = 0;
    nd6_test_tx_count = 0;
}

const U8 *nd6_test_get_last_tx(U16 *packet_len, U32 *tx_count)
{
    if (packet_len != NULL)
        *packet_len = nd6_test_last_tx_len;
    if (tx_count != NULL)
        *tx_count = nd6_test_tx_count;
    return nd6_test_last_tx;
}
#endif
