/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DP_IPV6.H

     IPv6 forwarding helpers shared by every data-plane RX loop.

     Classification is split from packet rewriting so both halves are plain
     functions a unit test can drive with a hand-built frame: the classifiers
     only read the packet and return a verdict, the transforms apply that
     verdict to the mbuf.

     Routed forwarding, no NAT66: addresses are never rewritten, so L4
     checksums stay valid across both directions and IPv6 (which has no
     header checksum) needs no checksum work for the hop-limit decrement.
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DP_IPV6_H_
#define _DP_IPV6_H_

#include <string.h>

#include <common.h>

#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_hash_crc.h>
#include <rte_ip6.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "protocol.h"
#include "init.h"
#include "dp.h"
#include "dhcp6/dhcp6.h"
#include "nd6/nd6.h"
#include "pppd/pppd.h"
#include "pppd/header.h"

/** Ethernet + VLAN bytes in front of the L3 header on both ports. */
#define IPV6_L2_LEN         (U16)(sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t))

/** PPPoE header plus the 2-byte PPP protocol field. */
#define IPV6_PPPOE_HDR_LEN  (U16)(sizeof(pppoe_header_t) + sizeof(ppp_payload_t))

/** Largest IPv6 packet a PPPoE session carries over a 1500-byte Ethernet MTU;
 *  also the MTU value reported in the ICMPv6 Packet Too Big we return. */
#define IPV6_PPPOE_MTU      (U16)(ETH_MTU - IPV6_PPPOE_HDR_LEN)

/** RFC 8200 minimum link MTU. An ICMPv6 error may not exceed it. */
#define IPV6_MIN_MTU        1280

/** Hop limit of the ICMPv6 errors this node originates. */
#define IPV6_ERROR_HOP_LIMIT 64

#define ICMP6_PACKET_TOO_BIG 2
#define ICMP6_PTB_HDR_LEN    8

/** Upper bound on an ICMPv6 Packet Too Big frame: L2 plus a 1280-byte packet. */
#define IPV6_PTB_MAX_LEN    (U16)(IPV6_L2_LEN + IPV6_MIN_MTU)

/**
 * Verdict for an IPv6 frame received on the LAN port.
 *   DROP        : free it
 *   TO_CP       : neighbor discovery, hand the frame to the control plane
 *   PASSTHROUGH : not addressed to our LAN MAC, forward raw to the WAN
 *   TOO_BIG     : does not fit the PPPoE MTU, answer with Packet Too Big
 *   FORWARD     : encapsulate and send to the WAN
 */
typedef enum {
    IPV6_LAN_DROP = 0,
    IPV6_LAN_TO_CP,
    IPV6_LAN_PASSTHROUGH,
    IPV6_LAN_TOO_BIG,
    IPV6_LAN_FORWARD,
} ipv6_lan_verdict_t;

/**
 * Verdict for a PPPoE-encapsulated IPv6 frame received on the WAN port.
 *   DROP    : free it
 *   TO_CP   : DHCPv6 or ICMPv6 for this node, hand the whole frame over
 *   FORWARD : decapsulate and deliver to a LAN host
 */
typedef enum {
    IPV6_WAN_DROP = 0,
    IPV6_WAN_TO_CP,
    IPV6_WAN_FORWARD,
} ipv6_wan_verdict_t;

/** Outcome of resolving and rewriting a WAN->LAN packet. */
typedef enum {
    IPV6_FWD_OK = 0,
    IPV6_FWD_HOP_LIMIT,     /**< hop limit exhausted */
    IPV6_FWD_NEIGHBOR_MISS, /**< destination not in the neighbor cache */
} ipv6_forward_result_t;

static __always_inline BOOL ipv6_addr_is_multicast(const U8 addr[16])
{
    return addr[0] == 0xff;
}

static __always_inline BOOL ipv6_addr_is_link_local(const U8 addr[16])
{
    return addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80;
}

/* True for ::, ::1 and IPv4-mapped addresses — none of them is routable. */
static __always_inline BOOL ipv6_addr_high64_is_zero(const U8 addr[16])
{
    U8 bits = 0;

    for(U8 i=0; i<8; i++)
        bits |= addr[i];
    return bits == 0;
}

static __always_inline BOOL ipv6_addr_in_lan_prefix(const ppp_ccb_t *ppp_ccb,
    const U8 addr[16])
{
    return memcmp(addr, ppp_ccb->hsi_ipv6_lan_prefix, 8) == 0;
}

/**
 * @fn compute_flow_tag6
 *
 * @brief IPv6 counterpart of compute_flow_tag: a per-direction 5-tuple hash
 *        used as the rte_distributor tag, so every packet of a flow lands on
 *        one worker and stays in order.
 *
 * @param src_addr
 *      16-byte source address
 * @param dst_addr
 *      16-byte destination address
 * @param src_port
 *      L4 source port as it appears in the packet
 * @param dst_port
 *      L4 destination port as it appears in the packet
 * @param proto
 *      L4 protocol number
 * @return
 *      Flow tag
 */
static __always_inline U32 compute_flow_tag6(const void *src_addr,
    const void *dst_addr, U16 src_port, U16 dst_port, U8 proto)
{
    U32 h = rte_hash_crc(src_addr, 16, 0);

    h = rte_hash_crc(dst_addr, 16, h);
    h = rte_hash_crc_4byte(((U32)src_port << 16) | dst_port, h);
    h = rte_hash_crc_1byte(proto, h);
    return h;
}

/**
 * @fn ipv6_flow_tag
 *
 * @brief Derive a distributor flow tag from a TCP or UDP IPv6 packet.
 *
 * @param ip6
 *      IPv6 header of the packet
 * @param[out] tag
 *      Flow tag, written only when the function returns TRUE
 * @return
 *      TRUE when the packet is TCP or UDP and carries a readable port pair
 */
static __always_inline BOOL ipv6_flow_tag(const struct rte_ipv6_hdr *ip6,
    U32 *tag)
{
    const U16 *ports;

    if (ip6->proto != IPPROTO_TCP && ip6->proto != IPPROTO_UDP)
        return FALSE;
    if (rte_be_to_cpu_16(ip6->payload_len) < 4)
        return FALSE;
    ports = (const U16 *)(ip6 + 1);
    *tag = compute_flow_tag6(&ip6->src_addr, &ip6->dst_addr, ports[0],
        ports[1], ip6->proto);
    return TRUE;
}

/**
 * @fn ipv6_lan_classify
 *
 * @brief Decide what to do with an IPv6 frame received on the LAN port.
 *        Read-only: the frame is left untouched.
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @param ppp_ccb
 *      Subscriber control block
 * @param eth_hdr
 *      Ethernet header of the frame
 * @param ip6
 *      IPv6 header of the frame
 * @param pkt_len
 *      Total frame length in bytes
 * @return
 *      The verdict to apply
 */
static __always_inline ipv6_lan_verdict_t ipv6_lan_classify(
    FastRG_t *fastrg_ccb, ppp_ccb_t *ppp_ccb,
    const struct rte_ether_hdr *eth_hdr, const struct rte_ipv6_hdr *ip6,
    U32 pkt_len)
{
    U16 payload_len;
    const U8 *src_ip = (const U8 *)&ip6->src_addr;
    const U8 *dst_ip = (const U8 *)&ip6->dst_addr;

    if (unlikely(pkt_len < (U32)IPV6_L2_LEN + sizeof(*ip6)))
        return IPV6_LAN_DROP;
    payload_len = rte_be_to_cpu_16(ip6->payload_len);
    if (unlikely(rte_ipv6_check_version(ip6) != 0 ||
            pkt_len != (U32)IPV6_L2_LEN + sizeof(*ip6) + payload_len))
        return IPV6_LAN_DROP;

    /* Neighbor discovery reaches the control plane whatever the forwarding
     * gate says: RA replies and neighbor cache learning are what make
     * forwarding possible in the first place. */
    if (ip6->proto == IPPROTO_ICMPV6 && payload_len >= ICMP6_PTB_HDR_LEN) {
        U8 icmp6_type = *((const U8 *)(ip6 + 1));

        if (icmp6_type == ND6_ICMP_RS || icmp6_type == ND6_ICMP_NS ||
                icmp6_type == ND6_ICMP_NA)
            return IPV6_LAN_TO_CP;
    }

    if (!pppd_ipv6_dp_gate_open(ppp_ccb))
        return IPV6_LAN_DROP;

    /* Not addressed to the gateway MAC: forward the frame raw, the same way
     * the IPv4 paths pass non-gateway traffic straight through. */
    if (unlikely(!rte_is_same_ether_addr(&eth_hdr->dst_addr,
            &fastrg_ccb->nic_info.hsi_lan_mac)))
        return IPV6_LAN_PASSTHROUGH;

    /* Anti-spoofing. One prefix comparison also rejects link-local,
     * unspecified and multicast sources. */
    if (!ipv6_addr_in_lan_prefix(ppp_ccb, src_ip))
        return IPV6_LAN_DROP;

    /* Destinations that must never leave through the WAN: on-link traffic
     * (hosts talk to each other directly over L2), and anything without a
     * routable destination. */
    if (ipv6_addr_is_multicast(dst_ip) || ipv6_addr_is_link_local(dst_ip) ||
            ipv6_addr_in_lan_prefix(ppp_ccb, dst_ip) ||
            ipv6_addr_high64_is_zero(dst_ip))
        return IPV6_LAN_DROP;

    /* Forwarding would produce hop limit 0, so this node is the last hop.
     * No ICMPv6 Time Exceeded is generated. */
    if (ip6->hop_limits <= 1)
        return IPV6_LAN_DROP;

    if (unlikely(sizeof(*ip6) + payload_len > IPV6_PPPOE_MTU))
        return IPV6_LAN_TOO_BIG;

    return IPV6_LAN_FORWARD;
}

/**
 * @fn ipv6_wan_classify
 *
 * @brief Decide what to do with a PPPoE-encapsulated IPv6 frame received on
 *        the WAN port. Read-only: the frame is left untouched.
 *
 * @param ppp_ccb
 *      Subscriber control block
 * @param ip6
 *      Inner IPv6 header of the frame
 * @param pkt_len
 *      Total frame length in bytes, PPPoE header included
 * @return
 *      The verdict to apply
 */
static __always_inline ipv6_wan_verdict_t ipv6_wan_classify(ppp_ccb_t *ppp_ccb,
    const struct rte_ipv6_hdr *ip6, U32 pkt_len)
{
    U32 ip6_len;
    U16 payload_len;
    const U8 *dst_ip = (const U8 *)&ip6->dst_addr;

    if (unlikely(pkt_len < (U32)IPV6_L2_LEN + IPV6_PPPOE_HDR_LEN +
            sizeof(*ip6)))
        return IPV6_WAN_DROP;
    ip6_len = pkt_len - IPV6_L2_LEN - IPV6_PPPOE_HDR_LEN;
    payload_len = rte_be_to_cpu_16(ip6->payload_len);
    if (unlikely(rte_ipv6_check_version(ip6) != 0 ||
            ip6_len != sizeof(*ip6) + payload_len))
        return IPV6_WAN_DROP;

    /* Only the LAN /64 carries assigned hosts. The rest of the delegated /56
     * stays unallocated, so those destinations fall through to the drop
     * below instead of being forwarded or queued to the control plane. */
    if (ipv6_addr_in_lan_prefix(ppp_ccb, dst_ip))
        return IPV6_WAN_FORWARD;

    if (ip6->proto == IPPROTO_UDP &&
            payload_len >= sizeof(struct rte_udp_hdr)) {
        const struct rte_udp_hdr *udp = (const struct rte_udp_hdr *)(ip6 + 1);

        if (udp->dst_port == rte_cpu_to_be_16(DHCP6_CLIENT_PORT))
            return IPV6_WAN_TO_CP;
    }

    /* BRAS-side neighbor discovery and diagnostics aimed at our own WAN
     * link-local address. */
    if (ip6->proto == IPPROTO_ICMPV6 && (ipv6_addr_is_link_local(dst_ip) ||
            ipv6_addr_is_multicast(dst_ip)))
        return IPV6_WAN_TO_CP;

    return IPV6_WAN_DROP;
}

/**
 * @fn ipv6_build_packet_too_big
 *
 * @brief Build an ICMPv6 Packet Too Big frame answering an oversized LAN
 *        packet. The quoted original is truncated so the whole IPv6 packet
 *        stays within the 1280-byte minimum MTU.
 *
 * @param ppp_ccb
 *      Subscriber control block
 * @param eth_hdr
 *      Ethernet header of the oversized packet
 * @param vlan_hdr
 *      VLAN header of the oversized packet
 * @param orig_ip6
 *      IPv6 header of the oversized packet
 * @param orig_ip6_len
 *      Length of the oversized IPv6 packet in bytes
 * @param buffer
 *      Output buffer of at least IPV6_PTB_MAX_LEN bytes
 * @return
 *      Number of bytes written
 */
static __always_inline U16 ipv6_build_packet_too_big(ppp_ccb_t *ppp_ccb,
    const struct rte_ether_hdr *eth_hdr, const vlan_header_t *vlan_hdr,
    const struct rte_ipv6_hdr *orig_ip6, U16 orig_ip6_len, U8 *buffer)
{
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buffer;
    vlan_header_t *vlan = (vlan_header_t *)(eth + 1);
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(vlan + 1);
    U8 *icmp6 = (U8 *)(ip6 + 1);
    U16 quota = (U16)(IPV6_MIN_MTU - sizeof(*ip6) - ICMP6_PTB_HDR_LEN);
    U16 quote_len = RTE_MIN(orig_ip6_len, quota);
    U16 icmp_len = (U16)(ICMP6_PTB_HDR_LEN + quote_len);
    U32 mtu = rte_cpu_to_be_32(IPV6_PPPOE_MTU);
    U16 cksum;

    rte_ether_addr_copy(&eth_hdr->src_addr, &eth->dst_addr);
    rte_ether_addr_copy(&ppp_ccb->fastrg_ccb->nic_info.hsi_lan_mac,
        &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(VLAN);
    vlan->tci_union.tci_value = vlan_hdr->tci_union.tci_value;
    vlan->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IPV6);

    ip6->vtc_flow = rte_cpu_to_be_32(UINT32_C(6) << 28);
    ip6->payload_len = rte_cpu_to_be_16(icmp_len);
    ip6->proto = IPPROTO_ICMPV6;
    ip6->hop_limits = IPV6_ERROR_HOP_LIMIT;
    nd6_gateway_link_local(&ppp_ccb->fastrg_ccb->nic_info.hsi_lan_mac,
        (U8 *)&ip6->src_addr);
    rte_memcpy(&ip6->dst_addr, &orig_ip6->src_addr, 16);

    memset(icmp6, 0, ICMP6_PTB_HDR_LEN);
    icmp6[0] = ICMP6_PACKET_TOO_BIG;
    rte_memcpy(icmp6 + 4, &mtu, sizeof(mtu));
    rte_memcpy(icmp6 + ICMP6_PTB_HDR_LEN, orig_ip6, quote_len);
    cksum = rte_ipv6_udptcp_cksum(ip6, icmp6);
    rte_memcpy(icmp6 + 2, &cksum, sizeof(cksum));

    return (U16)(IPV6_L2_LEN + sizeof(*ip6) + icmp_len);
}

/**
 * @fn ipv6_send_packet_too_big
 *
 * @brief Answer an oversized LAN packet with ICMPv6 Packet Too Big on the
 *        caller's LAN TX queue and free the oversized packet. Mirrors what
 *        build_icmp_unreach does for IPv4.
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @param ppp_ccb
 *      Subscriber control block
 * @param pkt
 *      Oversized packet; always consumed
 * @param ccb_id
 *      Subscriber index
 * @param eth_hdr
 *      Ethernet header of the oversized packet
 * @param vlan_hdr
 *      VLAN header of the oversized packet
 * @param ip6
 *      IPv6 header of the oversized packet
 * @param lan_tx_queue
 *      LAN TX queue owned by the calling lcore
 * @return
 *      void
 */
static __always_inline void ipv6_send_packet_too_big(FastRG_t *fastrg_ccb,
    ppp_ccb_t *ppp_ccb, struct rte_mbuf *pkt, U16 ccb_id,
    struct rte_ether_hdr *eth_hdr, vlan_header_t *vlan_hdr,
    struct rte_ipv6_hdr *ip6, U16 lan_tx_queue)
{
    struct rte_mbuf *reply = rte_pktmbuf_alloc(direct_pool[0]);
    U16 reply_len;

    count_rx_packet(fastrg_ccb, pkt, LAN_PORT, ccb_id);
    if (unlikely(reply == NULL)) {
        drop_packet(fastrg_ccb, pkt, LAN_PORT, ccb_id);
        return;
    }
    reply_len = ipv6_build_packet_too_big(ppp_ccb, eth_hdr, vlan_hdr, ip6,
        (U16)(pkt->pkt_len - IPV6_L2_LEN), rte_pktmbuf_mtod(reply, U8 *));
    reply->pkt_len = reply_len;
    reply->data_len = reply_len;
    count_tx_packet(fastrg_ccb, reply, LAN_PORT, ccb_id);
    if (rte_eth_tx_burst(LAN_PORT, lan_tx_queue, &reply, 1) == 0)
        drop_packet(fastrg_ccb, reply, LAN_PORT, ccb_id);
    drop_packet(fastrg_ccb, pkt, LAN_PORT, ccb_id);
}

/**
 * @fn ipv6_lan_to_wan_encap
 *
 * @brief Turn a classified LAN IPv6 frame into a PPPoE session frame: one
 *        hop-limit decrement, then the Ethernet and VLAN headers move back
 *        over the 8 bytes that hold the new PPPoE header.
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @param ppp_ccb
 *      Subscriber control block
 * @param pkt
 *      Packet to encapsulate
 * @param ccb_id
 *      Subscriber index
 * @return
 *      void
 */
static __always_inline void ipv6_lan_to_wan_encap(FastRG_t *fastrg_ccb,
    ppp_ccb_t *ppp_ccb, struct rte_mbuf *pkt, U16 ccb_id)
{
    mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(pkt);
    struct rte_ether_hdr *eth_hdr = mbuf_priv->eth_hdr;
    vlan_header_t *vlan_hdr = mbuf_priv->vlan_hdr;
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)
        ((char *)eth_hdr + IPV6_L2_LEN);
    char *cur = (char *)eth_hdr - IPV6_PPPOE_HDR_LEN;
    pppoe_header_t *pppoe_hdr;

    ip6->hop_limits--;
    count_rx_packet(fastrg_ccb, pkt, LAN_PORT, ccb_id);
    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac,
        &eth_hdr->src_addr);
    rte_ether_addr_copy(&ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);
    vlan_hdr->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);
    rte_memcpy(cur, eth_hdr, sizeof(struct rte_ether_hdr));
    rte_memcpy(cur + sizeof(struct rte_ether_hdr), vlan_hdr,
        sizeof(vlan_header_t));
    pppoe_hdr = (pppoe_header_t *)(cur + IPV6_L2_LEN);
    pppoe_hdr->ver_type = VER_TYPE;
    pppoe_hdr->code = 0;
    pppoe_hdr->session_id = ppp_ccb->session_id;
    pppoe_hdr->length = rte_cpu_to_be_16((U16)(pkt->pkt_len - IPV6_L2_LEN +
        sizeof(ppp_payload_t)));
    *(U16 *)(pppoe_hdr + 1) = rte_cpu_to_be_16(PPP_IPV6_PROTOCOL);
    pkt->data_off -= IPV6_PPPOE_HDR_LEN;
    pkt->pkt_len += IPV6_PPPOE_HDR_LEN;
    pkt->data_len += IPV6_PPPOE_HDR_LEN;
    mbuf_priv->eth_hdr = (struct rte_ether_hdr *)cur;
    mbuf_priv->vlan_hdr = (vlan_header_t *)((char *)cur +
        sizeof(struct rte_ether_hdr));
    count_tx_packet(fastrg_ccb, pkt, WAN_PORT, ccb_id);
    increase_pppoes_tx_count(fastrg_ccb, ccb_id, pkt->pkt_len);
}

/**
 * @fn ipv6_wan_to_lan_forward
 *
 * @brief Resolve the LAN host of a PPPoE-encapsulated IPv6 packet, decrement
 *        its hop limit, rewrite the Ethernet header and strip the PPPoE
 *        header. The packet is left untouched unless the result is
 *        IPV6_FWD_OK, so a caller escalating a neighbor miss still holds the
 *        frame exactly as it arrived.
 *
 *        The neighbor cache is never written here: the control plane stays
 *        its single writer.
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @param ppp_ccb
 *      Subscriber control block
 * @param pkt
 *      Packet to decapsulate
 * @param ccb_id
 *      Subscriber index
 * @return
 *      IPV6_FWD_OK when the packet is ready for LAN TX, otherwise the reason
 *      it cannot be forwarded
 */
static __always_inline ipv6_forward_result_t ipv6_wan_to_lan_forward(
    FastRG_t *fastrg_ccb, ppp_ccb_t *ppp_ccb, struct rte_mbuf *pkt,
    U16 ccb_id)
{
    mbuf_priv_t *mbuf_priv = rte_mbuf_to_priv(pkt);
    struct rte_ether_hdr *eth_hdr = mbuf_priv->eth_hdr;
    vlan_header_t *vlan_hdr = mbuf_priv->vlan_hdr;
    struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)
        ((char *)eth_hdr + IPV6_L2_LEN + IPV6_PPPOE_HDR_LEN);
    struct rte_ether_hdr tmp_eth;
    vlan_header_t tmp_vlan;
    struct rte_ether_addr host_mac;
    char *cur;

    if (unlikely(ip6->hop_limits <= 1))
        return IPV6_FWD_HOP_LIMIT;
    if (unlikely(nd6_table_lookup(ppp_ccb->nd6_table,
            (const U8 *)&ip6->dst_addr, &host_mac) != SUCCESS))
        return IPV6_FWD_NEIGHBOR_MISS;

    ip6->hop_limits--;
    count_rx_packet(fastrg_ccb, pkt, WAN_PORT, ccb_id);
    increase_pppoes_rx_count(fastrg_ccb, ccb_id, pkt->pkt_len);
    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&host_mac, &eth_hdr->dst_addr);
    vlan_hdr->next_proto = rte_cpu_to_be_16(FRAME_TYPE_IPV6);
    rte_memcpy(&tmp_eth, eth_hdr, sizeof(tmp_eth));
    rte_memcpy(&tmp_vlan, vlan_hdr, sizeof(tmp_vlan));
    cur = (char *)eth_hdr + IPV6_PPPOE_HDR_LEN;
    rte_memcpy(cur, &tmp_eth, sizeof(tmp_eth));
    rte_memcpy(cur + sizeof(tmp_eth), &tmp_vlan, sizeof(tmp_vlan));
    pkt->data_off += IPV6_PPPOE_HDR_LEN;
    pkt->pkt_len -= IPV6_PPPOE_HDR_LEN;
    pkt->data_len -= IPV6_PPPOE_HDR_LEN;
    mbuf_priv->eth_hdr = (struct rte_ether_hdr *)cur;
    mbuf_priv->vlan_hdr = (vlan_header_t *)(cur + sizeof(tmp_eth));
    count_tx_packet(fastrg_ccb, pkt, LAN_PORT, ccb_id);
    return IPV6_FWD_OK;
}

/**
 * @fn ipv6_neighbor_miss
 *
 * @brief Hand an unresolvable WAN->LAN packet to the control plane so it can
 *        solicit the destination, at most once per subscriber per second.
 *        Every other packet in the window (and the loser of the race) is
 *        dropped, so traffic aimed at an unresolved address cannot flood the
 *        control-plane ring. The packet is always consumed.
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @param ppp_ccb
 *      Subscriber control block
 * @param pkt
 *      Packet whose destination is unresolved
 * @param ccb_id
 *      Subscriber index
 * @return
 *      void
 */
static __always_inline void ipv6_neighbor_miss(FastRG_t *fastrg_ccb,
    ppp_ccb_t *ppp_ccb, struct rte_mbuf *pkt, U16 ccb_id)
{
    U64 now = fastrg_get_cur_cycles();
    U64 last = __atomic_load_n(&ppp_ccb->nd6_miss_last_cycles,
        __ATOMIC_RELAXED);

    if (now - last < fastrg_get_cycles_in_sec() ||
            !__atomic_compare_exchange_n(&ppp_ccb->nd6_miss_last_cycles,
                &last, now, FALSE, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        drop_packet(fastrg_ccb, pkt, WAN_PORT, ccb_id);
        return;
    }
    send2cp(fastrg_ccb, pkt, EV_DP_ND6_MISS, WAN_PORT);
}

#endif /* _DP_IPV6_H_ */
