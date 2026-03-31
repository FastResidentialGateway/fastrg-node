#ifndef _NAT_H_
#define _NAT_H_

#include <stdint.h>
#include <inttypes.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_memcpy.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_arp.h>
#include <rte_icmp.h>
#include <rte_flow.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_timer.h>
#include <rte_atomic.h>
#include <rte_memcpy.h>
#include <rte_hash_crc.h>

#include "pppd.h"

#define NAT_ENTRY_TIMEOUT_TICKS 10

#define MAX_L4_PORT_NUM 0xffff
#define SYS_MAX_PORT 1000
#define NAT_PORT_RANGE (TOTAL_SOCK_PORT - SYS_MAX_PORT)

#define NAT_ENTRY_FREE       0
#define NAT_ENTRY_FILLING    1
#define NAT_ENTRY_READY      2

void nat_rule_timer(__attribute__((unused)) struct rte_timer *tim, ppp_ccb_t *s_ppp_ccb);

/**
 * @fn compute_nat_table_index
 * 
 * @brief Compute NAT table index by using nat_port(calculated in 
 *        compute_initial_nat_port), dst_ip, dst_port
 *        Table index is SEPARATE from NAT port - this enables true port reuse
 * 
 * @param nat_port 
 *        NAT port calculated in compute_initial_nat_port() in network byte order
 * @param dst_ip
 *        Destination IP in network byte order
 * @param dst_port
 *        Destination port in network byte order
 * 
 * @return Hash value in range 0 to MAX_NAT_ENTRIES - 1
 */
static inline U32 compute_nat_table_index(U16 nat_port, U32 dst_ip, U16 dst_port)
{
    U32 hash;
    hash = rte_hash_crc_4byte((U32)rte_be_to_cpu_16(nat_port), 0);
    hash = rte_hash_crc_4byte(dst_ip, hash);
    hash = rte_hash_crc_4byte((U32)dst_port, hash);
    return hash % MAX_NAT_ENTRIES;
}

/**
 * @fn compute_initial_nat_port
 * 
 * @brief Compute initial NAT port from source info
 * 
 * @param src_ip
 *        Source IP in network byte order
 * @param src_port
 *        Source port in network byte order
 * 
 * @return NAT port in range SYS_MAX_PORT to TOTAL_SOCK_PORT - 1 in network byte order
 */
static inline U16 compute_initial_nat_port(U32 src_ip, U16 src_port)
{
    U32 hash;
    hash = rte_hash_crc_4byte(src_ip, 0);
    hash = rte_hash_crc_4byte((U32)src_port, hash);
    return rte_cpu_to_be_16(((U16)hash % NAT_PORT_RANGE) + SYS_MAX_PORT);
}

/**
 * @fn nat_entry_matches_key
 * 
 * @brief Check if NAT entry matches key
 * 
 * @param entry
 *       Pointer to NAT entry
 * @param nat_port
 *       NAT port in network byte order
 * @param dst_ip
 *       Destination IP in network byte order
 * @param dst_port
 *       Destination port in network byte order
 * 
 * @return 1 if matches, 0 otherwise
 */
static inline int nat_entry_matches_key(addr_table_t *entry, 
    U16 nat_port, U32 dst_ip, U16 dst_port)
{
    return (entry->nat_port == nat_port &&
            entry->dst_ip == dst_ip && 
            entry->dst_port == dst_port);
}

/**
 * @fn nat_entry_same_flow
 * 
 * @brief Check if entry is for the same flow (exact 5-tuple match)
 * 
 * @param entry
 *       Pointer to NAT entry
 * @param nat_port
 *       NAT port in network byte order
 * @param src_ip
 *       Source IP in network byte order
 * @param src_port
 *       Source port in network byte order
 * @param dst_ip
 *       Destination IP in network byte order
 * @param dst_port
 *       Destination port in network byte order
 * 
 * @return 1 if matches, 0 otherwise
 */
static inline int nat_entry_same_flow(addr_table_t *entry, U16 nat_port, 
    U32 src_ip, U16 src_port, U32 dst_ip, U16 dst_port)
{
    return (entry->nat_port == nat_port &&
            entry->src_ip == src_ip &&
            entry->src_port == src_port &&
            entry->dst_ip == dst_ip &&
            entry->dst_port == dst_port);
}

/**
 * @fn nat_learning_port_reuse
 * 
 * @brief NAT learning with TRUE port reuse support
 * 
 * Key insight: table_idx and nat_port are INDEPENDENT
 * - nat_port: the actual port number used in SNAT (can be reused for different dsts)
 * - table_idx: just a hash bucket in the table (for storage only)
 * 
 * Port reuse logic:
 * - Same (nat_port, dst_ip, dst_port) from different sources = CONFLICT, try next nat_port
 * - Same nat_port with different dst = OK, port reuse achieved!
 * 
 * @param eth_hdr
 *        Pointer to Ethernet header (for copying MAC address)
 * @param src_ip
 *        Source IP in network byte order
 * @param dst_ip
 *        Destination IP in network byte order
 * @param src_port
 *        Source port in network byte order
 * @param dst_port
 *        Destination port in network byte order
 * @param addr_table
 *        NAT address table
 * @param port_fwd_table
 *        Port forwarding table (direct-indexed by port number).
 *        If a candidate nat_port is reserved by a port-forward rule,
 *        it will be skipped to avoid conflict.
 * 
 * @return Allocated nat_port in network byte order, or 0 if all ports exhausted
 */
static inline U16 nat_learning_port_reuse(struct rte_ether_hdr *eth_hdr,
    U32 src_ip, U32 dst_ip, U16 src_port, U16 dst_port,
    addr_table_t addr_table[], port_fwd_entry_t port_fwd_table[])
{
    U16 nat_port = compute_initial_nat_port(src_ip, src_port);
    U16 start_nat_port = nat_port;

    do {
        /* Skip ports reserved by static port forwarding rules */
        U16 nat_port_host = rte_be_to_cpu_16(nat_port);
        if (rte_atomic16_read(&port_fwd_table[nat_port_host].is_active) == 1)
            goto next_nat_port;

        /* Compute table index for this (nat_port, dst_ip, dst_port) combination */
        U32 table_idx = compute_nat_table_index(nat_port, 
            dst_ip, dst_port);
        U32 start_idx = table_idx;

        do {
            addr_table_t *entry = &addr_table[table_idx];
            int16_t entry_state = rte_atomic16_read(&entry->is_fill);

            /* Case 1: Empty slot - can use this nat_port */
            if (entry_state == NAT_ENTRY_FREE) {
                if (rte_atomic16_cmpset((volatile uint16_t *)&entry->is_fill, 
                        NAT_ENTRY_FREE, NAT_ENTRY_FILLING)) {
                    rte_ether_addr_copy(&eth_hdr->src_addr, &entry->mac_addr);
                    entry->src_ip = src_ip;
                    entry->dst_ip = dst_ip;
                    entry->src_port = src_port;
                    entry->dst_port = dst_port;
                    entry->nat_port = nat_port;
                    rte_atomic16_set(&entry->is_alive, NAT_ENTRY_TIMEOUT_TICKS);

                    rte_atomic_thread_fence(rte_memory_order_release);
                    rte_atomic16_set(&entry->is_fill, NAT_ENTRY_READY);

                    return entry->nat_port;
                }
                /* This slot is being filled by another CPU, continue to check again */
                continue;
            }

            /* Case 1.5: Entry is being filled - skip it */
            if (entry_state == NAT_ENTRY_FILLING) {
                table_idx++;
                if (table_idx >= MAX_NAT_ENTRIES)
                    table_idx = 0;
                continue;
            }

            /* Case 2: Entry is READY - safe to read */
            if (entry_state == NAT_ENTRY_READY) {
                /* Same flow already exists - return existing nat_port */
                if (nat_entry_same_flow(entry, nat_port, src_ip, src_port, dst_ip, dst_port)) {
                    rte_atomic16_set(&entry->is_alive, NAT_ENTRY_TIMEOUT_TICKS);
                    return entry->nat_port;
                }

                /* Same (nat_port, dst_ip, dst_port) but different source = CONFLICT
                * This means another source already uses this nat_port for this destination
                * Must try a different nat_port */
                if (nat_entry_matches_key(entry, nat_port, dst_ip, dst_port))
                    break;
            }

            /* Case 4: Hash collision (different key, same bucket) - try next slot */
            table_idx++;
            if (table_idx >= MAX_NAT_ENTRIES)
                table_idx = 0;
        } while (table_idx != start_idx);

        /* If we found a conflict or table is full for this nat_port, try next nat_port */
next_nat_port:
        nat_port_host = rte_be_to_cpu_16(nat_port);
        nat_port_host++;
        if (nat_port_host >= TOTAL_SOCK_PORT)
            nat_port_host = SYS_MAX_PORT;
        nat_port = rte_cpu_to_be_16(nat_port_host);

    } while (nat_port != start_nat_port);

    /* All NAT ports exhausted */
    return 0;
}

/**
 * @fn nat_reverse_lookup
 * 
 * @brief Reverse lookup for inbound packets (WAN -> LAN)
 * Find SNAT entry for original SIP and SPORT
 * 
 * @param nat_port
 *        Pkt dst port in network byte order
 * @param remote_ip
 *        Remote source IP in network byte order
 * @param remote_port
 *        Remote source port in network byte order
 * @param addr_table
 *        NAT address table
 * 
 * @return Pointer to matching address table entry, or NULL if not found
 */
static inline addr_table_t *nat_reverse_lookup(U16 nat_port, U32 remote_ip, U16 remote_port,
    addr_table_t addr_table[])
{
    U32 table_idx = compute_nat_table_index(nat_port, remote_ip, remote_port);
    U32 start_idx = table_idx;

    do {
        addr_table_t *entry = &addr_table[table_idx];

        /* Skip non-ready entries */
        if (rte_atomic16_read(&entry->is_fill) != NAT_ENTRY_READY) {
            table_idx++;
            if (table_idx >= MAX_NAT_ENTRIES)
                table_idx = 0;
            continue;
        }

        /* Entry is ready - safe to read */
        rte_atomic_thread_fence(rte_memory_order_acquire);

        if (nat_entry_matches_key(entry, nat_port, remote_ip, remote_port)) {
            rte_atomic16_set(&entry->is_alive, NAT_ENTRY_TIMEOUT_TICKS);
            return entry;
        }

        table_idx++;
        if (table_idx >= MAX_NAT_ENTRIES)
            table_idx = 0;
    } while (table_idx != start_idx);

    return NULL;
}

/**
 * @fn nat_icmp_learning
 * 
 * @brief NAT learning for ICMP packets
 * 
 * @param eth_hdr
 *        Pointer to Ethernet header
 * @param ip_hdr
 *        Pointer to IPv4 header
 * @param icmphdr
 *        Pointer to ICMP header
 * @param addr_table
 *        NAT address table
 * @param port_fwd_table
 *        Port forwarding table for reserved-port check
 * 
 * @return NAT port in network byte order, or 0 if all ports exhausted
 */
static inline U16 nat_icmp_learning(struct rte_ether_hdr *eth_hdr, 
    struct rte_ipv4_hdr *ip_hdr, struct rte_icmp_hdr *icmphdr, 
    addr_table_t addr_table[], port_fwd_entry_t port_fwd_table[])
{
    return nat_learning_port_reuse(eth_hdr,
        ip_hdr->src_addr, ip_hdr->dst_addr,
        icmphdr->icmp_ident, icmphdr->icmp_type,
        addr_table, port_fwd_table);
}

/**
 * @fn nat_udp_learning
 * 
 * @brief NAT learning for UDP packets
 * 
 * @param eth_hdr
 *        Pointer to Ethernet header
 * @param ip_hdr
 *        Pointer to IPv4 header
 * @param udphdr
 *        Pointer to UDP header
 * @param addr_table
 *        NAT address table
 * @param port_fwd_table
 *        Port forwarding table for reserved-port check
 * 
 * @return NAT port in network byte order, or 0 if all ports exhausted
 */
static inline U16 nat_udp_learning(struct rte_ether_hdr *eth_hdr, 
    struct rte_ipv4_hdr *ip_hdr, struct rte_udp_hdr *udphdr, 
    addr_table_t addr_table[], port_fwd_entry_t port_fwd_table[])
{
    return nat_learning_port_reuse(eth_hdr,
        ip_hdr->src_addr, ip_hdr->dst_addr,
        udphdr->src_port, udphdr->dst_port,
        addr_table, port_fwd_table);
}

/**
 * @fn nat_tcp_learning
 * 
 * @brief NAT learning for TCP packets
 * 
 * @param eth_hdr
 *        Pointer to Ethernet header
 * @param ip_hdr
 *        Pointer to IPv4 header
 * @param tcphdr
 *        Pointer to TCP header
 * @param addr_table
 *        NAT address table
 * @param port_fwd_table
 *        Port forwarding table for reserved-port check
 * 
 * @return NAT port in network byte order, or 0 if all ports exhausted
 */
static inline U16 nat_tcp_learning(struct rte_ether_hdr *eth_hdr, 
    struct rte_ipv4_hdr *ip_hdr, struct rte_tcp_hdr *tcphdr, 
    addr_table_t addr_table[], port_fwd_entry_t port_fwd_table[])
{
    return nat_learning_port_reuse(eth_hdr,
        ip_hdr->src_addr, ip_hdr->dst_addr,
        tcphdr->src_port, tcphdr->dst_port,
        addr_table, port_fwd_table);
}

/*======================================================================
 * SNAT Port Forwarding Helpers  –  O(1) direct-index implementation
 *
 * Key idea: eport is U16 (0..65535), so we use it directly as the
 * array index.  No hash, no collision, no loop.
 *======================================================================*/

/**
 * @fn port_fwd_lookup_by_eport
 *
 * @brief O(1) lookup: index directly by eport value.
 *
 * @param table
 *      port_fwd_table[PORT_FWD_TABLE_SIZE] inside ppp_ccb
 * @param eport
 *      External port in HOST byte order
 *
 * @return
 *      Pointer to entry if active, NULL otherwise
 */
static inline port_fwd_entry_t *port_fwd_lookup_by_eport(
    port_fwd_entry_t table[], U16 eport)
{
    port_fwd_entry_t *entry = &table[eport];
    if (rte_atomic16_read(&entry->is_active) == 1)
        return entry;
    return NULL;
}

/**
 * @fn port_fwd_add
 *
 * @brief Add / update a static port forwarding entry.
 *        O(1): write directly to table[eport].
 *
 * @param table
 *      Port forwarding table
 * @param eport
 *      External port (host byte order)
 * @param dip
 *      Destination LAN IP (network byte order)
 * @param iport
 *      Internal port (host byte order)
 */
static inline void port_fwd_add(port_fwd_entry_t table[],
    U16 eport, U32 dip, U16 iport)
{
    port_fwd_entry_t *entry = &table[eport];
    if (rte_atomic16_read(&entry->is_active) == 1)
        return; /* Already active, do not overwrite to avoid disrupting existing flow */
    entry->dip = dip;
    entry->iport = iport;
    rte_atomic64_set(&entry->hit_count, 0);
    rte_atomic_thread_fence(rte_memory_order_release);
    rte_atomic16_set(&entry->is_active, 1);
}

/**
 * @fn port_fwd_remove
 *
 * @brief Remove a port forwarding entry.
 *        O(1): clear table[eport] directly.
 *
 * @param table
 *      Port forwarding table
 * @param eport
 *      External port (host byte order)
 *
 * @return
 *      SUCCESS on success, ERROR if entry was not active
 */
static inline STATUS port_fwd_remove(port_fwd_entry_t table[], U16 eport)
{
    port_fwd_entry_t *entry = &table[eport];
    if (rte_atomic16_read(&entry->is_active) == 0)
        return ERROR;
    rte_atomic16_set(&entry->is_active, 0);
    rte_atomic_thread_fence(rte_memory_order_release);
    entry->dip = 0;
    entry->iport = 0;
    rte_atomic64_set(&entry->hit_count, 0);
    return SUCCESS;
}

/**
 * @fn nat_port_fwd_reverse_lookup
 *
 * @brief O(1) inbound port-forward lookup for the data-path hot path.
 *
 * @param table
 *      port_fwd_table inside ppp_ccb
 * @param dst_port
 *      WAN packet destination port, NETWORK byte order
 * @param out_dip
 *      [out] LAN destination IP (network byte order)
 * @param out_iport
 *      [out] LAN internal port (network byte order)
 *
 * @return
 *      SUCCESS if matched, ERROR if not
 */
static inline STATUS nat_port_fwd_reverse_lookup(port_fwd_entry_t table[],
    U16 dst_port, U32 *out_dip, U16 *out_iport)
{
    U16 eport_host = rte_be_to_cpu_16(dst_port);
    port_fwd_entry_t *entry = port_fwd_lookup_by_eport(table, eport_host);
    if (entry == NULL)
        return ERROR;
    *out_dip = entry->dip;
    *out_iport = rte_cpu_to_be_16(entry->iport);
    return SUCCESS;
}

#endif
