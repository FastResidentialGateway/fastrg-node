/*
 * dp_flow.c - DPDK rte_flow rules for PPPoE-aware RSS
 *
 * Queue layout
 * ~~~~~~~~~~~~
 *  Queue 0      - Control plane (queue 0).
 *  Queue 1..N-1 - RSS worker queues (5-tuple hash via RETA).
 *
 * Per-port flow configuration
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  Port 0 (LAN-side, plain IP):
 *    eth / vlan / ipv4 / tcp  -> RSS 1..N
 *    eth / vlan / ipv4 / udp  -> RSS 1..N
 *
 *  Port 1 (WAN-side, PPPoE):
 *    Priority 0:
 *      eth(ether_type=0x8863)              -> queue 0   (PPPoE Discovery)
 *      eth / pppoes / ipv4 / tcp           -> RSS 1..N  (matches VLAN-tagged too)
 *      eth / pppoes / ipv4 / udp           -> RSS 1..N
 *      eth / pppoes / ipv6 / tcp           -> RSS 1..N
 *      eth / pppoes / ipv6 / udp           -> RSS 1..N
 *    Priority 1:
 *      eth(ether_type=0x8864)              -> queue 0   (PPPoE Session fallback)
 *
 * DDP note: ICE E810 with COMMS DDP package.
 *   eth / pppoes / L3 / L4 transparently matches VLAN-tagged PPPoE frames;
 *   verified with testpmd that eth/vlan/pppoes/ipv4/tcp and
 *   eth/vlan/pppoes/ipv4/udp are both handled by this pattern.
 *
 * Queue count formula (total = RSS + 1 for queue 0)
 *   cpu_count = 5  ->  1 RSS queue  (total 2 queues)
 *   cpu_count = 7  ->  2 RSS queues (total 3 queues)
 *   cpu_count = 9  ->  3 RSS queues (total 4 queues)
 *   General: rss_count = max(1, 1 + (cpu_count - 5) / 2)
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <rte_flow.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>

#include "fastrg.h"
#include "dbg.h"
#include "dp_flow.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/**
 * @fn make_reta_rss_action 
 * 
 * @brief fill an RSS action with empty queue list.
 *
 * Matches the testpmd form:
 *   actions rss types <type> end key_len 0 queues end / end
 *
 * queue_num=0 / queue=NULL tells the NIC to use the RSS indirection table
 * (RETA) for queue selection.  The RETA is programmed separately by
 * setup_rss_reta() to exclude queue 0, so all RSS traffic lands on
 * queues 1..N.
 *
 * The ice (E810) driver requires this exact form when a PPPOES item appears
 * in the pattern — specifying an explicit queue list causes an
 * RTE_FLOW_ERROR_TYPE_ITEM "Unsupported pattern" failure.
 *
 * @param action
 *      Action array; action[0] will be filled.
 * @param rss_conf
 *      Caller-allocated rte_flow_action_rss.
 * @param hash_types
 *      RTE_ETH_RSS_* bitmask.
 */
static void make_reta_rss_action(struct rte_flow_action *action,
    struct rte_flow_action_rss *rss_conf, uint64_t hash_types)
{
    memset(rss_conf, 0, sizeof(*rss_conf));
    rss_conf->func      = RTE_ETH_HASH_FUNCTION_DEFAULT;
    rss_conf->level     = 0;
    rss_conf->types     = hash_types;
    rss_conf->key_len   = 0;    /* use NIC default key */
    rss_conf->key       = NULL;
    rss_conf->queue_num = 0;    /* empty: let NIC use RETA */
    rss_conf->queue     = NULL;

    action[0].type = RTE_FLOW_ACTION_TYPE_RSS;
    action[0].conf = rss_conf;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;
}

/**
 * @fn setup_rss_reta
 * 
 * @brief Program the RSS indirection table to use queues [1..N-1].
 *
 * Because the rte_flow RSS actions above use queue_num=0 (RETA-based),
 * we must ensure queue 0 is excluded from the RETA so that RSS traffic
 * never lands there.
 *
 * @param port_id
 *      DPDK port identifier.
 * @param total_queues
 *      Total queue count (RSS queues = total_queues - 1).
 * @return
 *      SUCCESS on success, ERROR on failure.
 */
static STATUS setup_rss_reta(FastRG_t *fastrg_ccb, uint16_t port_id, uint16_t total_queues)
{
    struct rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Port %u: rte_eth_dev_info_get failed: %s\n",
            port_id, strerror(-ret));
        return ERROR;
    }

    uint16_t reta_size = dev_info.reta_size;
    if (reta_size == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
            "Port %u: NIC has no RETA, skipping RETA setup\n", port_id);
        return SUCCESS;
    }

    uint16_t rss_cnt = total_queues - 1; /* queues 1..total_queues-1 */
    uint32_t reta_group_cnt = (reta_size + RTE_ETH_RETA_GROUP_SIZE - 1)
                              / RTE_ETH_RETA_GROUP_SIZE;

    struct rte_eth_rss_reta_entry64 *reta =
        fastrg_calloc(struct rte_eth_rss_reta_entry64, reta_group_cnt, 
        sizeof(*reta), 0);
    if (reta == NULL) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Port %u: RETA alloc failed\n", port_id);
        return ERROR;
    }

    /* Fill every RETA entry with queue IDs cycling through 1..rss_cnt */
    for(uint32_t i=0; i<reta_size; i++) {
        uint32_t grp = i / RTE_ETH_RETA_GROUP_SIZE;
        uint32_t pos = i % RTE_ETH_RETA_GROUP_SIZE;
        reta[grp].mask = UINT64_MAX;  /* update all entries in this group */
        reta[grp].reta[pos] = (uint16_t)(1 + (i % rss_cnt));
    }

    ret = rte_eth_dev_rss_reta_update(port_id, reta, reta_size);
    fastrg_mfree(reta);
    if (ret != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Port %u: rte_eth_dev_rss_reta_update failed: %s\n",
            port_id, strerror(-ret));
        return ERROR;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "Port %u: RETA (%u entries) programmed to queues 1..%u\n",
        port_id, reta_size, total_queues - 1);
    return SUCCESS;
}

/**
 * @fn make_queue0_action
 * 
 * @brief Fill an action that steers to queue 0.
 * @param action
 *      Action array; action[0] will be filled.
 * @param q_conf
 *      Caller-allocated rte_flow_action_queue; q_conf->index will be set to 0.
 */
static void make_queue0_action(struct rte_flow_action *action,
    struct rte_flow_action_queue *q_conf)
{
    q_conf->index  = 0;
    action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    action[0].conf = q_conf;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;
}

/**
 * @fn create_flow - validate + create a single flow rule.
 * 
 * @brief Validate and create a single flow rule.
 * @param port_id
 *      DPDK port identifier.
 * @param attr
 *      Flow attributes.
 * @param pattern
 *      Flow pattern.
 * @param action
 *      Flow actions.
 * @param error
 *      Flow error structure.
 * @return
 *      The created flow on success, NULL on failure.
 */
static struct rte_flow *create_flow(uint16_t port_id,
    const struct rte_flow_attr *attr,
    const struct rte_flow_item *pattern,
    const struct rte_flow_action *action,
    struct rte_flow_error *error)
{
    if (rte_flow_validate(port_id, attr, pattern, action, error) != 0)
        return NULL;
    return rte_flow_create(port_id, attr, pattern, action, error);
}

/* ------------------------------------------------------------------ */
/* Flow generators                                                      */
/* ------------------------------------------------------------------ */

/**
 * @fn create_pppoe_discovery_flow
 * 
 * @brief Create a flow for PPPoE Discovery packets.
 * Pattern: eth(ether_type=0x8863) / END
 *
 * Match on outer EtherType = 0x8863 (PPPoE Discovery).
 * Simple single-item pattern that works for QUEUE actions on the ICE PMD.
 * @param fastrg_ccb
 *      FastRG control block.
 * @param port_id
 *      DPDK port identifier.
 * @param error
 *      Flow error structure.
 * @return
 *      The created flow on success, NULL on failure.
 */
static struct rte_flow *create_pppoe_discovery_flow(FastRG_t *fastrg_ccb, 
    uint16_t port_id, struct rte_flow_error *error)
{
    struct rte_flow_attr         attr = { .ingress = 1, .priority = 0 };
    struct rte_flow_item         pattern[2];
    struct rte_flow_action       action[2];
    struct rte_flow_action_queue q_conf;

    struct rte_flow_item_eth eth_spec, eth_mask;

    memset(&eth_spec, 0, sizeof(eth_spec));
    memset(&eth_mask, 0, sizeof(eth_mask));
    eth_spec.hdr.ether_type = rte_cpu_to_be_16(ETH_P_PPP_DIS);
    eth_mask.hdr.ether_type = 0xFFFF;

    memset(pattern, 0, sizeof(pattern));
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[0].spec = &eth_spec;
    pattern[0].mask = &eth_mask;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    make_queue0_action(action, &q_conf);

    struct rte_flow *f = create_flow(port_id, &attr, pattern, action, error);
    if (!f)
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Port %u: PPPoE Discovery flow failed (type=%d): %s\n",
            port_id, error->type,
            error->message ? error->message : "(no reason)");
    return f;
}

/**
 * @fn create_pppoe_session_fallback_flow
 * 
 * @brief Create a flow for PPPoE Session fallback packets.
 * @param fastrg_ccb
 *      FastRG control block.
 * @param port_id
 *      DPDK port identifier.
 * @param error
 *      Flow error structure.
 * @return
 *      The created flow on success, NULL on failure.
 *
 * Pattern: eth(ether_type=0x8864) / END
 *
 * Match on outer EtherType = 0x8864 (PPPoE Session).
 * Simple single-item pattern that works for QUEUE actions on the ICE PMD.
 * Catches LCP/IPCP and any PPPoE Session frames not matched by a P0 rule.
 */
#if 0
static struct rte_flow *create_pppoe_session_fallback_flow(FastRG_t *fastrg_ccb, 
    uint16_t port_id, struct rte_flow_error *error)
{
    struct rte_flow_attr         attr = { .ingress = 1, .priority = 1 };
    struct rte_flow_item         pattern[2];
    struct rte_flow_action       action[2];
    struct rte_flow_action_queue q_conf;

    struct rte_flow_item_eth eth_spec, eth_mask;

    memset(&eth_spec, 0, sizeof(eth_spec));
    memset(&eth_mask, 0, sizeof(eth_mask));
    eth_spec.hdr.ether_type = rte_cpu_to_be_16(ETH_P_PPP_SES);
    eth_mask.hdr.ether_type = 0xFFFF;

    memset(pattern, 0, sizeof(pattern));
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[0].spec = &eth_spec;
    pattern[0].mask = &eth_mask;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    make_queue0_action(action, &q_conf);

    struct rte_flow *f = create_flow(port_id, &attr, pattern, action, error);
    if (!f)
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Port %u: PPPoE Session fallback flow failed (type=%d): %s\n",
            port_id, error->type,
            error->message ? error->message : "(no reason)");
    return f;
}
#endif

/**
 * @fn create_pppoe_session_rss_flow
 * 
 * @brief Create a flow for PPPoE Session packets that steers to RSS queues.
 * eth / pppoes / L3 / L4 --> RSS queues 1..N via RETA, priority 0
 *
 * Verified with testpmd: this pattern transparently matches VLAN-tagged
 * PPPoE Session frames (eth/vlan/pppoes/ipv4/tcp and .../udp) without
 * needing explicit VLAN items.
 *
 * @param fastrg_ccb
 *      FastRG control block.
 * @param port_id
 *      DPDK port identifier.
 * @param l3_type
 *      RTE_FLOW_ITEM_TYPE_IPV4 or RTE_FLOW_ITEM_TYPE_IPV6
 * @param l4_type
 *      RTE_FLOW_ITEM_TYPE_TCP  or RTE_FLOW_ITEM_TYPE_UDP
 * @param hash_type
 *      Corresponding RTE_ETH_RSS_* flag
 * @param error
 *      Flow error structure.
 * @return
 *      The created flow on success, NULL on failure.
 */
static struct rte_flow *create_pppoe_session_rss_flow(FastRG_t *fastrg_ccb, 
    uint16_t port_id, enum rte_flow_item_type l3_type,
    enum rte_flow_item_type l4_type, uint64_t hash_type, struct rte_flow_error *error)
{
    struct rte_flow_attr       attr = { .ingress = 1, .priority = 0 };
    struct rte_flow_action     action[2];
    struct rte_flow_action_rss rss_conf;
    struct rte_flow_item       pattern[5];

    make_reta_rss_action(action, &rss_conf, hash_type);

    memset(pattern, 0, sizeof(pattern));
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_PPPOES;
    pattern[2].type = l3_type;
    pattern[3].type = l4_type;
    pattern[4].type = RTE_FLOW_ITEM_TYPE_END;

    struct rte_flow *f = create_flow(port_id, &attr, pattern, action, error);
    if (f == NULL)
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Port %u: PPPoE Session RSS flow failed (type=%d): %s\n",
            port_id, error->type,
            error->message ? error->message : "(no reason)");
    return f;
}

/**
 * @fn create_vlan_ip_rss_flow
 * 
 * @brief Create a flow for VLAN-tagged IP packets that steers to RSS queues.
 * eth / vlan / L3 / L4 --> RSS queues 1..N via RETA, priority 0
 *
 * Used for plain IP traffic on VLAN-tagged interfaces (port 0, LAN side).
 *
 * @param fastrg_ccb
 *      FastRG control block.
 * @param port_id
 *      DPDK port identifier.
 * @param l3_type
 *      RTE_FLOW_ITEM_TYPE_IPV4 or RTE_FLOW_ITEM_TYPE_IPV6
 * @param l4_type
 *      RTE_FLOW_ITEM_TYPE_TCP  or RTE_FLOW_ITEM_TYPE_UDP
 * @param hash_type
 *      Corresponding RTE_ETH_RSS_* flag
 * @param error
 *      Flow error structure.
 * @return
 *      The created flow on success, NULL on failure.
 */
static struct rte_flow *create_vlan_ip_rss_flow(FastRG_t *fastrg_ccb, 
    uint16_t port_id, enum rte_flow_item_type l3_type,
    enum rte_flow_item_type l4_type, uint64_t hash_type,
    struct rte_flow_error *error)
{
    struct rte_flow_attr       attr = { .ingress = 1, .priority = 0 };
    struct rte_flow_action     action[2];
    struct rte_flow_action_rss rss_conf;
    struct rte_flow_item       pattern[5];

    make_reta_rss_action(action, &rss_conf, hash_type);

    memset(pattern, 0, sizeof(pattern));
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_VLAN;
    pattern[2].type = l3_type;
    pattern[3].type = l4_type;
    pattern[4].type = RTE_FLOW_ITEM_TYPE_END;

    struct rte_flow *f = create_flow(port_id, &attr, pattern, action, error);
    if (f == NULL)
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Port %u: VLAN IP RSS flow failed (type=%d): %s\n",
            port_id, error->type,
            error->message ? error->message : "(no reason)");
    return f;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int setup_port_flows(FastRG_t *fastrg_ccb, uint16_t port_id, uint16_t total_queues,
    struct rte_flow_error *error)
{
    struct rte_flow *flow;

    if (total_queues < 2) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "total_queues=%u, need at least 2\n", total_queues);
        return -1;
    }

    /* Flush stale rules from a previous run to avoid conflict errors. */
    rte_flow_flush(port_id, error);

    if (port_id == 0) {
        /* ---- Port 0: LAN-side plain IP over VLAN --------------------- */

        flow = create_vlan_ip_rss_flow(fastrg_ccb, port_id,
            RTE_FLOW_ITEM_TYPE_IPV4, RTE_FLOW_ITEM_TYPE_TCP,
            RTE_ETH_RSS_NONFRAG_IPV4_TCP, error);
        if (!flow)
            return -1;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Port %u: VLAN IPv4/TCP -> RSS 1..%u [OK]\n",
            port_id, total_queues - 1);

        flow = create_vlan_ip_rss_flow(fastrg_ccb, port_id,
            RTE_FLOW_ITEM_TYPE_IPV4, RTE_FLOW_ITEM_TYPE_UDP,
            RTE_ETH_RSS_NONFRAG_IPV4_UDP, error);
        if (!flow)
            return -1;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Port %u: VLAN IPv4/UDP -> RSS 1..%u [OK]\n",
            port_id, total_queues - 1);

    } else {
        /* ---- Port 1+: WAN-side PPPoE --------------------------------- */

        /* Priority 0: PPPoE Discovery -> queue 0 */
        flow = create_pppoe_discovery_flow(fastrg_ccb, port_id, error);
        if (!flow)
            return -1;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Port %u: PPPoE Discovery (ether_type=0x8863) -> queue 0 [OK]\n",
            port_id);

        /* Priority 0: PPPoE Session + inner IPv4/TCP -> RSS */
        flow = create_pppoe_session_rss_flow(fastrg_ccb, port_id,
            RTE_FLOW_ITEM_TYPE_IPV4, RTE_FLOW_ITEM_TYPE_TCP,
            RTE_ETH_RSS_NONFRAG_IPV4_TCP, error);
        if (!flow)
            return -1;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Port %u: PPPoE Session IPv4/TCP -> RSS 1..%u [OK]\n",
            port_id, total_queues - 1);

        /* Priority 0: PPPoE Session + inner IPv4/UDP -> RSS */
        flow = create_pppoe_session_rss_flow(fastrg_ccb, port_id,
            RTE_FLOW_ITEM_TYPE_IPV4, RTE_FLOW_ITEM_TYPE_UDP,
            RTE_ETH_RSS_NONFRAG_IPV4_UDP, error);
        if (!flow)
            return -1;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Port %u: PPPoE Session IPv4/UDP -> RSS 1..%u [OK]\n",
            port_id, total_queues - 1);

        /* Priority 0: PPPoE Session + inner IPv6/TCP -> RSS */
        flow = create_pppoe_session_rss_flow(fastrg_ccb, port_id,
            RTE_FLOW_ITEM_TYPE_IPV6, RTE_FLOW_ITEM_TYPE_TCP,
            RTE_ETH_RSS_NONFRAG_IPV6_TCP, error);
        if (!flow)
            return -1;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Port %u: PPPoE Session IPv6/TCP -> RSS 1..%u [OK]\n",
            port_id, total_queues - 1);

        /* Priority 0: PPPoE Session + inner IPv6/UDP -> RSS */
        flow = create_pppoe_session_rss_flow(fastrg_ccb, port_id,
            RTE_FLOW_ITEM_TYPE_IPV6, RTE_FLOW_ITEM_TYPE_UDP,
            RTE_ETH_RSS_NONFRAG_IPV6_UDP, error);
        if (!flow)
            return -1;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Port %u: PPPoE Session IPv6/UDP -> RSS 1..%u [OK]\n",
            port_id, total_queues - 1);

        /* ======Note: This flow is invalid for ICE PMD, I keep it here for here because it is 
         * a good reference for PPPoE Session fallback flow without inner IP match.====== */
        #if 0
         /* Priority 1: PPPoE Session fallback -> queue 0 */
        flow = create_pppoe_session_fallback_flow(fastrg_ccb, port_id, error);
        if (!flow)
            return -1;
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Port %u: PPPoE Session fallback (ether_type=0x8864) -> queue 0 [OK]\n",
            port_id);
        #endif
    }

    /* ---- Program RETA to distribute RSS traffic to queues 1..N ------- */
    if (setup_rss_reta(fastrg_ccb, port_id, total_queues) != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Port %u: RETA setup failed\n", port_id);
        return -1;
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "Port %u: flow setup complete, %u total queues "
        "(queue 0=ctrl, queues 1..%u=RSS via RETA)\n",
        port_id, total_queues, total_queues - 1);

    return 0;
}
