/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DP_CODEC.H

  Designed by THE on JAN 21, 2021
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DP_CODEC_H_
#define _DP_CODEC_H_

#include <common.h>

#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_icmp.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_ip.h>
#include <rte_ethdev.h>

#include "protocol.h"
#include "init.h"
#include "pppd/nat.h"
#include "dhcpd/dhcpd.h"
#include "mac_table.h"
#include "dp.h"

/* Queue 0 is for control/fallback, queues 1..N are RSS data queues.
 * Each thread uses its own queue ID for TX to avoid contention. */

static inline void build_icmp_unreach(FastRG_t *fastrg_ccb, struct rte_mbuf *pkt, 
    U16 ccb_id, struct rte_ether_hdr *eth_hdr, vlan_header_t old_vlan_hdr, 
    struct rte_ipv4_hdr *ip_hdr)
{
    vlan_header_t *vlan_header;
    struct rte_ether_hdr *new_eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
#ifndef UNIT_TEST
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
#else
    dhcp_ccb_t *dhcp_ccb = (dhcp_ccb_t *)fastrg_ccb->dhcp_ccb[0];
#endif

    rte_ether_addr_copy(&eth_hdr->src_addr, &new_eth_hdr->dst_addr);
    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &new_eth_hdr->src_addr);
    new_eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    vlan_header = (vlan_header_t *)(new_eth_hdr + 1);
    *vlan_header = old_vlan_hdr;

    struct rte_ipv4_hdr *new_ip_hdr = (struct rte_ipv4_hdr *)(vlan_header + 1);
    *new_ip_hdr = *ip_hdr;
    new_ip_hdr->dst_addr = ip_hdr->src_addr;
    new_ip_hdr->src_addr = dhcp_ccb->dhcp_server_ip;
    new_ip_hdr->packet_id = 0;
    new_ip_hdr->next_proto_id = IPPROTO_ICMP;

    struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)(new_ip_hdr + 1);
    icmp_hdr->icmp_type	= ICMP_UNREACHABLE;
    icmp_hdr->icmp_code = ICMP_FRAG_NEED_DF_SET;
    icmp_hdr->icmp_ident = 0; //unsed field
    icmp_hdr->icmp_seq_nb = rte_cpu_to_be_16(ETH_MTU - sizeof(struct rte_ipv4_hdr) - 
        sizeof(vlan_header_t) - sizeof(pppoe_header_t) - sizeof(ppp_payload_t)); // MTU size is mentioned here 

    U16 orig_ip_total_len = rte_be_to_cpu_16(ip_hdr->total_length);
    U16 orig_data_len = RTE_MIN(orig_ip_total_len - sizeof(struct rte_ipv4_hdr), 
        ICMP_UNREACH_DATA_PAYLOAD_LEN);
    U16 icmp_payload_len = sizeof(struct rte_ipv4_hdr) + orig_data_len;

    rte_memcpy((char *)(icmp_hdr + 1), (char *)ip_hdr, icmp_payload_len);

    new_ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + 
        sizeof(struct rte_icmp_hdr) + sizeof(struct rte_ipv4_hdr) + 8);

    icmp_hdr->icmp_cksum = 0;
    icmp_hdr->icmp_cksum = (U16)~rte_raw_cksum((const void *)icmp_hdr, 
        sizeof(struct rte_icmp_hdr) + sizeof(struct rte_ipv4_hdr) + 8);

    new_ip_hdr->hdr_checksum = 0;
    new_ip_hdr->hdr_checksum = rte_ipv4_cksum(new_ip_hdr);

    pkt->pkt_len = pkt->data_len = rte_be_to_cpu_16(new_ip_hdr->total_length) + 
        sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t);
    //pkt->ol_flags |= PKT_TX_IPV4 | PKT_TX_IP_CKSUM;
    //pkt->l2_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t);
}

static inline STATUS insert_pppoes_hdr(FastRG_t *fastrg_ccb, struct rte_mbuf **pkt, 
    U16 ccb_id, U16 vlan_hdr_tci)
{
    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)rte_pktmbuf_prepend((*pkt), 
        sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + 
        sizeof(pppoe_header_t) + sizeof(ppp_payload_t));
    if (unlikely(eth_hdr == NULL))
        return ERROR;
    /* for PPPoE */
    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_wan_src_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&ppp_ccb->PPP_dst_mac, &eth_hdr->dst_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);
    vlan_header_t *vlan_header = (vlan_header_t *)(eth_hdr + 1);
    vlan_header->tci_union.tci_value = vlan_hdr_tci;	
    vlan_header->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);

    pppoe_header_t *pppoe_header = (pppoe_header_t *)(vlan_header + 1);
    pppoe_header->ver_type = VER_TYPE;
    pppoe_header->code = 0;
    pppoe_header->session_id = ppp_ccb->session_id;
    pppoe_header->length = rte_cpu_to_be_16((*pkt)->data_len - 
        (U16)(sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + 
        sizeof(pppoe_header_t)));
    *(U16 *)(pppoe_header + 1) = rte_cpu_to_be_16(PPP_IP_PROTOCOL);

    return SUCCESS;
}

static int encaps_udp(FastRG_t *fastrg_ccb, struct rte_mbuf **single_pkt, 
    struct rte_ether_hdr *eth_hdr, vlan_header_t *vlan_header, 
    struct rte_ipv4_hdr *ip_hdr, U16 ccb_id, U16 lan_tx_queue)
{
    struct rte_udp_hdr *udphdr;
    U16                new_port_id;
    int32_t            new_pkt_num = 0;
    U16                vlan_hdr_tci = vlan_header->tci_union.tci_value;

    if (unlikely((*single_pkt)->pkt_len > (ETH_MTU - (sizeof(vlan_header_t) + 
            sizeof(pppoe_header_t) + sizeof(ppp_payload_t))))) {
        struct rte_mbuf *pkt = rte_pktmbuf_alloc(direct_pool[0]);
        if (unlikely(pkt == NULL)) {
            drop_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
            return 0;
        }
        build_icmp_unreach(fastrg_ccb, pkt, ccb_id, eth_hdr, *vlan_header, ip_hdr);
        count_rx_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
        count_tx_packet(fastrg_ccb, pkt, LAN_PORT, ccb_id);
        rte_eth_tx_burst(LAN_PORT, lan_tx_queue, &pkt, 1);
        drop_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
        new_pkt_num = 0;
    } else {
        new_pkt_num = 1;
        ip_hdr->hdr_checksum = 0;
        ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);

        /* for nat */
        udphdr = (struct rte_udp_hdr *)(ip_hdr + 1);
        new_port_id = nat_udp_learning(eth_hdr, ip_hdr, udphdr, ppp_ccb->addr_table, ppp_ccb->port_fwd_table);
        if (unlikely(new_port_id == 0)) {
            drop_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
            return 0;
        }
        ip_hdr->src_addr = ppp_ccb->hsi_ipv4;
        udphdr->src_port = new_port_id;
        ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
        udphdr->dgram_cksum = 0;
        udphdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip_hdr, udphdr);

        if (unlikely(insert_pppoes_hdr(fastrg_ccb, single_pkt, ccb_id, vlan_hdr_tci) == ERROR)) {
            drop_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
            return 0;
        }
        count_rx_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
        count_tx_packet(fastrg_ccb, *single_pkt, WAN_PORT, ccb_id);
        increase_pppoes_tx_count(ppp_ccb, (*single_pkt)->pkt_len);
    }

    return new_pkt_num;
}

static int encaps_tcp(FastRG_t *fastrg_ccb, struct rte_mbuf **single_pkt, 
    struct rte_ether_hdr *eth_hdr, vlan_header_t *vlan_header, 
    struct rte_ipv4_hdr *ip_hdr, U16 ccb_id, U16 lan_tx_queue)
{
    struct rte_tcp_hdr *tcphdr;
    U16                new_port_id;
    int32_t            new_pkt_num = 0;
    U16                vlan_hdr_tci = vlan_header->tci_union.tci_value;

    if (unlikely((*single_pkt)->pkt_len > (ETH_MTU - (sizeof(vlan_header_t) + 
            sizeof(pppoe_header_t) + sizeof(ppp_payload_t))))) {
        #if 0 //TODO: for re-fragmentation, needed to implementation in the future
        struct rte_mbuf  *new_pkt;
        ip_hdr->hdr_checksum = 0;
        tcphdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
        nat_tcp_learning(eth_hdr,ip_hdr,tcphdr,&new_port_id,fastrg_ccb.ppp_ccb[ccb_id].addr_table);
        ori_src_ip = ip_hdr->src_addr;
        ip_hdr->src_addr = fastrg_ccb.ppp_ccb[ccb_id].ipv4;
        tcphdr->src_port = rte_cpu_to_be_16(new_port_id);
        fastrg_ccb.ppp_ccb[ccb_id].addr_table[new_port_id].is_alive = 10;
        ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
        tcphdr->cksum = 0;
        tcphdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr,tcphdr);

        ip_hdr->fragment_offset = 0;
        //printf("pkt len = %u, data len = %u\n", (*single_pkt)->pkt_len, (*single_pkt)->data_len);
        new_pkt_num = rte_ipv4_fragment_packet((*single_pkt), &new_pkt, 6, IPV4_MTU_DEFAULT - sizeof(vlan_header_t) - sizeof(pppoe_header_t) - sizeof(ppp_payload_t), direct_pool[0], indirect_pool[0]);
        rte_pktmbuf_free((*single_pkt));
        if (unlikely(new_pkt_num < 0)) {
            printf("pkt fragmentation error: %s\n", rte_strerror(new_pkt_num));
            return -1;
        }

        for((*single_pkt)=new_pkt; (*single_pkt)!=NULL; (*single_pkt)=(*single_pkt)->next) {
            ip_hdr = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod((*single_pkt),unsigned char *));
            tcphdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
            struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)rte_pktmbuf_prepend((*single_pkt), (U16)(sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t) + sizeof(ppp_payload_t)));
            if (eth_hdr == NULL) {
                rte_panic("No headroom in mbuf.\n");
            }

            //(*single_pkt)->ol_flags = PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_TCP_CKSUM;
            (*single_pkt)->l2_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t) + sizeof(ppp_payload_t);

            rte_memcpy(eth_hdr->src_addr.addr_bytes,fastrg_ccb.ppp_ccb[ccb_id].src_mac,ETH_ALEN);
            rte_memcpy(eth_hdr->dst_addr.addr_bytes,fastrg_ccb.ppp_ccb[ccb_id].dst_mac,ETH_ALEN);

            //rte_ether_addr_copy(&fastrg_ccb.ppp_ccb[ccb_id].src_mac, &eth_hdr->src_addr);
            //rte_ether_addr_copy(&fastrg_ccb.ppp_ccb[ccb_id].dst_mac, &eth_hdr->dst_addr);
            eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);
            vlan_header = (vlan_header_t *)(eth_hdr + 1);
            vlan_header->tci_union.tci_value = vlan_hdr_tci;
            vlan_header->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);
            pppoe_header = (pppoe_header_t *)(vlan_header + 1);
            pppoe_header->ver_type = VER_TYPE;
            pppoe_header->code = 0;
            pppoe_header->session_id = fastrg_ccb.ppp_ccb[ccb_id].session_id;
            pppoe_header->length = rte_cpu_to_be_16(((*single_pkt)->data_len) - (U16)(sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t)));
            *(U16 *)(pppoe_header + 1) = rte_cpu_to_be_16(PPP_IP_PROTOCOL);
            ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
            tcphdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr,tcphdr);
        }
        (*single_pkt) = new_pkt;
        #else
        struct rte_mbuf *pkt = rte_pktmbuf_alloc(direct_pool[0]);
        if (unlikely(pkt == NULL)) {
            drop_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
            return 0;
        }
        build_icmp_unreach(fastrg_ccb, pkt, ccb_id, eth_hdr, *vlan_header, ip_hdr);
        count_rx_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
        count_tx_packet(fastrg_ccb, pkt, LAN_PORT, ccb_id);
        rte_eth_tx_burst(LAN_PORT, lan_tx_queue, &pkt, 1);
        drop_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
        new_pkt_num = 0;
        #endif
    } else {
        new_pkt_num = 1;
        ip_hdr->hdr_checksum = 0;
        ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);

        /* for nat */
        tcphdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
        new_port_id = nat_tcp_learning(eth_hdr, ip_hdr, tcphdr, ppp_ccb->addr_table, ppp_ccb->port_fwd_table);
        if (unlikely(new_port_id == 0)) {
            drop_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
            return 0;
        }
        ip_hdr->src_addr = ppp_ccb->hsi_ipv4;
        tcphdr->src_port = new_port_id;
        ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
        tcphdr->cksum = 0;
        tcphdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcphdr);

        if (unlikely(insert_pppoes_hdr(fastrg_ccb, single_pkt, ccb_id, vlan_hdr_tci) == ERROR)) {
            drop_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
            return 0;
        }
        count_rx_packet(fastrg_ccb, *single_pkt, LAN_PORT, ccb_id);
        count_tx_packet(fastrg_ccb, *single_pkt, WAN_PORT, ccb_id);
        increase_pppoes_tx_count(ppp_ccb, (*single_pkt)->pkt_len);
    }
    return new_pkt_num;
}

static int encaps_icmp(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    struct rte_ether_hdr *eth_hdr, vlan_header_t *vlan_header, 
    struct rte_ipv4_hdr *ip_hdr, struct rte_icmp_hdr *icmphdr, U16 ccb_id)
{
    U16                 new_port_id;
    int32_t             new_pkt_num = 0;
    U16                 vlan_hdr_tci = vlan_header->tci_union.tci_value;
    ppp_ccb_t           *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);

    /* NAT + PPPoE encapsulate ICMP */
    U32 icmp_new_cksum;
    U16 ori_ident = icmphdr->icmp_ident;

    new_port_id = nat_icmp_learning(eth_hdr, ip_hdr, icmphdr, 
        ppp_ccb->addr_table, ppp_ccb->port_fwd_table);
    if (unlikely(new_port_id == 0)) {
        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
        return new_pkt_num;
    }
    ip_hdr->src_addr = ppp_ccb->hsi_ipv4;
    icmphdr->icmp_ident = new_port_id;
    ip_hdr->hdr_checksum = 0;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

    if (((icmp_new_cksum = icmphdr->icmp_cksum + ori_ident - new_port_id) >> 16) != 0)
        icmp_new_cksum = (icmp_new_cksum & 0xFFFF) + (icmp_new_cksum >> 16);
    icmphdr->icmp_cksum = (U16)icmp_new_cksum;

    if (unlikely(insert_pppoes_hdr(fastrg_ccb, &single_pkt, ccb_id, vlan_hdr_tci) == ERROR)) {
        drop_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
        return new_pkt_num;
    }
    count_rx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
    count_tx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
    increase_pppoes_tx_count(ppp_ccb, single_pkt->pkt_len);
    new_pkt_num = 1;

    return new_pkt_num;
}

static int decaps_udp(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    struct rte_ether_hdr *eth_hdr, vlan_header_t *vlan_header, 
    struct rte_ipv4_hdr *ip_hdr, U16 ccb_id, U16 tx_q)
{
    struct rte_udp_hdr *udphdr;
    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);

    udphdr = (struct rte_udp_hdr *)(ip_hdr + 1);
    addr_table_t *entry = nat_reverse_lookup(udphdr->dst_port, ip_hdr->src_addr, 
        udphdr->src_port, ppp_ccb->addr_table);
    if (unlikely(entry == NULL)) {
        /* Fallback: check port forwarding table */
        U32 fwd_dip;
        U16 fwd_iport;
        if (nat_port_fwd_reverse_lookup(ppp_ccb->port_fwd_table,
                udphdr->dst_port, &fwd_dip, &fwd_iport) == SUCCESS) {
            U16 eport_host = rte_be_to_cpu_16(udphdr->dst_port);
            rte_atomic64_inc(&ppp_ccb->port_fwd_table[eport_host].hit_count);
            rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);
            ip_hdr->dst_addr = fwd_dip;
            udphdr->dst_port = fwd_iport;
            ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
            udphdr->dgram_cksum = 0;
            udphdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip_hdr, udphdr);
            count_rx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
            increase_pppoes_rx_count(ppp_ccb, single_pkt->pkt_len);

            /* Look up MAC table for destination host */
            mac_table_entry_t *mac_entry = mac_table_lookup(
                ppp_ccb->mac_table, fwd_dip);
            if (likely(mac_entry != NULL)) {
                rte_ether_addr_copy(&mac_entry->mac, &eth_hdr->dst_addr);
                count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                return 1;
            }
            /* MAC unknown: queue packet and send ARP request */
            dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
            arp_pending_enqueue(fastrg_ccb->arp_pending_mp,
                &ppp_ccb->arp_pq, single_pkt, fwd_dip);
            if (send_arp_request(&fastrg_ccb->nic_info.hsi_lan_mac,
                    dhcp_ccb->dhcp_server_ip, fwd_dip,
                    rte_atomic16_read(&ppp_ccb->vlan_id), tx_q) == SUCCESS)
                count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
            return 0;  /* packet held in pending queue */
        }
        drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
        return 0;
    }
    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&entry->mac_addr, &eth_hdr->dst_addr);
    ip_hdr->dst_addr = entry->src_ip;
    udphdr->dst_port = entry->src_port;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    udphdr->dgram_cksum = 0;
    udphdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip_hdr, udphdr);
    count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
    count_rx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
    increase_pppoes_rx_count(ppp_ccb, single_pkt->pkt_len);

    return 1;
}

static int decaps_tcp(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt, 
    struct rte_ether_hdr *eth_hdr, vlan_header_t *vlan_header, 
    struct rte_ipv4_hdr *ip_hdr, U16 ccb_id, U16 tx_q)
{
    struct rte_tcp_hdr *tcphdr;
    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);

    tcphdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
    addr_table_t *entry = nat_reverse_lookup(tcphdr->dst_port, ip_hdr->src_addr, 
        tcphdr->src_port, ppp_ccb->addr_table);
    if (unlikely(entry == NULL)) {
        /* Fallback: check port forwarding table */
        U32 fwd_dip;
        U16 fwd_iport;
        if (nat_port_fwd_reverse_lookup(ppp_ccb->port_fwd_table,
                tcphdr->dst_port, &fwd_dip, &fwd_iport) == SUCCESS) {
            U16 eport_host = rte_be_to_cpu_16(tcphdr->dst_port);
            rte_atomic64_inc(&ppp_ccb->port_fwd_table[eport_host].hit_count);
            rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);
            ip_hdr->dst_addr = fwd_dip;
            tcphdr->dst_port = fwd_iport;
            ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
            tcphdr->cksum = 0;
            tcphdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcphdr);
            count_rx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
            increase_pppoes_rx_count(ppp_ccb, single_pkt->pkt_len);

            /* Look up MAC table for destination host */
            mac_table_entry_t *mac_entry = mac_table_lookup(
                ppp_ccb->mac_table, fwd_dip);
            if (likely(mac_entry != NULL)) {
                rte_ether_addr_copy(&mac_entry->mac, &eth_hdr->dst_addr);
                count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
                return 1;
            }
            /* MAC unknown: queue packet and send ARP request */
            dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
            arp_pending_enqueue(fastrg_ccb->arp_pending_mp,
                &ppp_ccb->arp_pq, single_pkt, fwd_dip);
            if (send_arp_request(&fastrg_ccb->nic_info.hsi_lan_mac,
                    dhcp_ccb->dhcp_server_ip, fwd_dip,
                    rte_atomic16_read(&ppp_ccb->vlan_id), tx_q) == SUCCESS)
                count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
            return 0;  /* packet held in pending queue */
        }
        drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
        return 0;
    }
    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&entry->mac_addr, &eth_hdr->dst_addr);

    /* SPI: drop packets that are inconsistent with the tracked TCP state */
    if (unlikely(!tcp_conntrack_inbound_valid(entry->tcp_state, tcphdr->tcp_flags))) {
        drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
        return 0;
    }

    tcp_conntrack_fsm(entry, tcphdr->tcp_flags, TRUE);
    ip_hdr->dst_addr = entry->src_ip;
    tcphdr->dst_port = entry->src_port;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    tcphdr->cksum = 0;
    tcphdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcphdr);
    count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
    count_rx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
    increase_pppoes_rx_count(ppp_ccb, single_pkt->pkt_len);

    return 1;
}

static int decaps_icmp(FastRG_t *fastrg_ccb, struct rte_mbuf *single_pkt,
    struct rte_ether_hdr *eth_hdr, struct rte_ipv4_hdr *ip_hdr, U16 ccb_id)
{
    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    struct rte_icmp_hdr *icmphdr = (struct rte_icmp_hdr *)(rte_pktmbuf_mtod(single_pkt,
        unsigned char *) + sizeof(struct rte_ether_hdr) +
        sizeof(vlan_header_t) + sizeof(struct rte_ipv4_hdr));

    if (icmphdr->icmp_type != ICMP_ECHO_REPLY) {
        drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
        return 0;
    }
    addr_table_t *entry = nat_reverse_lookup(icmphdr->icmp_ident,
        ip_hdr->src_addr, ICMP_ECHO_REQUEST, ppp_ccb->addr_table);
    if (entry == NULL) {
        drop_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
        return 0;
    }
    int32_t icmp_cksum_diff = (int32_t)icmphdr->icmp_ident - (int32_t)entry->src_port;
    U32 icmp_new_cksum;

    rte_ether_addr_copy(&fastrg_ccb->nic_info.hsi_lan_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&entry->mac_addr, &eth_hdr->dst_addr);
    ip_hdr->dst_addr = entry->src_ip;
    icmphdr->icmp_ident = entry->src_port;

    if (((icmp_new_cksum = (U32)icmp_cksum_diff + (U32)icmphdr->icmp_cksum) >> 16) != 0)
        icmp_new_cksum = (icmp_new_cksum & 0xFFFF) + (icmp_new_cksum >> 16);
    icmphdr->icmp_cksum = (U16)icmp_new_cksum;
    ip_hdr->hdr_checksum = 0;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    count_tx_packet(fastrg_ccb, single_pkt, LAN_PORT, ccb_id);
    count_rx_packet(fastrg_ccb, single_pkt, WAN_PORT, ccb_id);
    increase_pppoes_rx_count(ppp_ccb, single_pkt->pkt_len);

    return 1;
}

#endif
