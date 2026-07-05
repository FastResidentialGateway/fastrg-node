#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <common.h>

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_icmp.h>

#include "../src/dp_codec.h"
#include "../src/dp.h"
#include "../src/protocol.h"
#include "../src/fastrg.h"
#include "test_helper.h"

// Global test counters
static int test_count = 0;
static int pass_count = 0;

void test_build_icmp_unreach(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_icmp_unreach function:\n");
    printf("=========================================\n\n");

    /* Use the existing dhcp_ccb[0] allocation from init_ccb instead of a local
     * stack variable — avoids a dangling pointer after this function returns. */
    dhcp_ccb_t *dhcp_ccb = (dhcp_ccb_t *)fastrg_ccb->dhcp_ccb[0];
    dhcp_ccb->dhcp_server_ip = 0x0101A8C0;  // 192.168.1.1
    dhcp_ccb->fastrg_ccb = fastrg_ccb;

    // Setup original Ethernet header
    struct rte_ether_hdr eth_hdr = {
        .src_addr = {
            .addr_bytes = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}
        },
        .dst_addr = {
            .addr_bytes = {0x9c, 0x69, 0xb4, 0x61, 0x16, 0xdc}
        },
        .ether_type = rte_cpu_to_be_16(VLAN)
    };

    // Setup original VLAN header
    vlan_header_t old_vlan_hdr = {
        .tci_union.tci_value = rte_cpu_to_be_16(100),  // VLAN ID 100
        .next_proto = rte_cpu_to_be_16(FRAME_TYPE_IP)
    };

    // Setup original IP header (10.1.2.3 -> 192.168.1.100)
    struct rte_ipv4_hdr ip_hdr = {
        .version_ihl = 0x45,
        .type_of_service = 0,
        .total_length = rte_cpu_to_be_16(60),  // 20 (IP) + 40 (data)
        .packet_id = rte_cpu_to_be_16(0x1234),
        .fragment_offset = 0,
        .time_to_live = 64,
        .next_proto_id = IPPROTO_TCP,
        .hdr_checksum = 0,
        .src_addr = 0x0302010A,  // 10.1.2.3 in little-endian
        .dst_addr = 0x6401A8C0   // 192.168.1.100 in little-endian
    };

    U16 ccb_id = 0;

    printf("Test 1: \"%s\"\n", "build_icmp_unreach() basic functionality");

    // Allocate buffer for mock mbuf
    U8 pkt_buffer[512] = {0};
    struct rte_mbuf mock_pkt = {
        .buf_addr = pkt_buffer,
        .data_off = 0,
        .pkt_len = 0,
        .data_len = 0
    };

    // Call the function
    build_icmp_unreach(fastrg_ccb, &mock_pkt, ccb_id, &eth_hdr, old_vlan_hdr, &ip_hdr);

    // Verify the result
    struct rte_ether_hdr *new_eth_hdr = (struct rte_ether_hdr *)pkt_buffer;
    vlan_header_t *new_vlan_hdr = (vlan_header_t *)(new_eth_hdr + 1);
    struct rte_ipv4_hdr *new_ip_hdr = (struct rte_ipv4_hdr *)(new_vlan_hdr + 1);
    struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)(new_ip_hdr + 1);

    // Test Ethernet header
    TEST_ASSERT(memcmp(&new_eth_hdr->dst_addr, &eth_hdr.src_addr, RTE_ETHER_ADDR_LEN) == 0,
        "ICMP Ethernet dst_addr", "dst_addr should be original src_addr");
    TEST_ASSERT(memcmp(&new_eth_hdr->src_addr, &fastrg_ccb->nic_info.hsi_lan_mac, RTE_ETHER_ADDR_LEN) == 0,
        "ICMP Ethernet src_addr", "src_addr should be LAN MAC");
    TEST_ASSERT(new_eth_hdr->ether_type == rte_cpu_to_be_16(VLAN),
        "ICMP Ethernet type", "ether_type should be VLAN");

    // Test VLAN header
    TEST_ASSERT(new_vlan_hdr->tci_union.tci_value == old_vlan_hdr.tci_union.tci_value,
        "ICMP VLAN TCI", "VLAN TCI should match original");

    // Test IP header
    TEST_ASSERT(new_ip_hdr->dst_addr == ip_hdr.src_addr,
        "ICMP IP dst_addr", "IP dst should be original src (0x%08x vs 0x%08x)",
        new_ip_hdr->dst_addr, ip_hdr.src_addr);
    TEST_ASSERT(new_ip_hdr->src_addr == dhcp_ccb->dhcp_server_ip,
        "ICMP IP src_addr", "IP src should be DHCP server IP (0x%08x vs 0x%08x)",
        new_ip_hdr->src_addr, dhcp_ccb->dhcp_server_ip);
    TEST_ASSERT(new_ip_hdr->next_proto_id == IPPROTO_ICMP,
        "ICMP IP protocol", "IP protocol should be ICMP");

    // Test ICMP header
    TEST_ASSERT(icmp_hdr->icmp_type == ICMP_UNREACHABLE,
        "ICMP type", "ICMP type should be UNREACHABLE (3), got %u", icmp_hdr->icmp_type);
    TEST_ASSERT(icmp_hdr->icmp_code == ICMP_FRAG_NEED_DF_SET,
        "ICMP code", "ICMP code should be FRAG_NEED_DF_SET (4), got %u", icmp_hdr->icmp_code);
    TEST_ASSERT(icmp_hdr->icmp_ident == 0,
        "ICMP ident", "ICMP ident should be 0");

    U16 expected_mtu = ETH_MTU - sizeof(struct rte_ipv4_hdr) - 
        sizeof(vlan_header_t) - sizeof(pppoe_header_t) - sizeof(ppp_payload_t);
    TEST_ASSERT(rte_be_to_cpu_16(icmp_hdr->icmp_seq_nb) == expected_mtu,
        "ICMP MTU", "ICMP MTU should be %u, got %u", 
        expected_mtu, rte_be_to_cpu_16(icmp_hdr->icmp_seq_nb));

    // Test that original IP header is copied after ICMP header
    struct rte_ipv4_hdr *copied_ip_hdr = (struct rte_ipv4_hdr *)(icmp_hdr + 1);
    TEST_ASSERT(memcmp(copied_ip_hdr, &ip_hdr, sizeof(struct rte_ipv4_hdr)) == 0,
        "ICMP payload IP header", "Original IP header should be copied");

    // Test packet length
    U16 expected_total_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_icmp_hdr) + 
        sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);
    TEST_ASSERT(mock_pkt.pkt_len == expected_total_len,
        "ICMP packet length", "Packet length should be %u, got %u",
        expected_total_len, mock_pkt.pkt_len);
    TEST_ASSERT(mock_pkt.data_len == expected_total_len,
        "ICMP data length", "Data length should be %u, got %u",
        expected_total_len, mock_pkt.data_len);

    printf("Test 2: \"%s\"\n", "build_icmp_unreach() with short original packet");

    // Test with original packet shorter than 8 bytes of data
    struct rte_ipv4_hdr short_ip_hdr = ip_hdr;
    short_ip_hdr.total_length = rte_cpu_to_be_16(24);  // 20 (IP) + 4 (data)

    memset(pkt_buffer, 0, sizeof(pkt_buffer));
    mock_pkt.pkt_len = 0;
    mock_pkt.data_len = 0;

    build_icmp_unreach(fastrg_ccb, &mock_pkt, ccb_id, &eth_hdr, old_vlan_hdr, &short_ip_hdr);

    new_ip_hdr = (struct rte_ipv4_hdr *)((vlan_header_t *)((struct rte_ether_hdr *)pkt_buffer + 1) + 1);

    // The ICMP payload should always include full IP header + 8 bytes (or less if data is shorter)
    U16 expected_ip_total = sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_icmp_hdr) + 
        sizeof(struct rte_ipv4_hdr) + ICMP_UNREACH_DATA_PAYLOAD_LEN;
    TEST_ASSERT(rte_be_to_cpu_16(new_ip_hdr->total_length) == expected_ip_total,
        "ICMP with short packet IP total length", 
        "IP total length should be %u, got %u", 
        expected_ip_total, rte_be_to_cpu_16(new_ip_hdr->total_length));

    printf("Test 3: \"%s\"\n", "build_icmp_unreach() checksum verification");

    memset(pkt_buffer, 0, sizeof(pkt_buffer));
    mock_pkt.pkt_len = 0;
    mock_pkt.data_len = 0;

    build_icmp_unreach(fastrg_ccb, &mock_pkt, ccb_id, &eth_hdr, old_vlan_hdr, &ip_hdr);

    new_ip_hdr = (struct rte_ipv4_hdr *)((vlan_header_t *)((struct rte_ether_hdr *)pkt_buffer + 1) + 1);
    icmp_hdr = (struct rte_icmp_hdr *)(new_ip_hdr + 1);

    // Verify IP checksum
    U16 saved_ip_cksum = new_ip_hdr->hdr_checksum;
    new_ip_hdr->hdr_checksum = 0;
    U16 calculated_ip_cksum = rte_ipv4_cksum(new_ip_hdr);
    new_ip_hdr->hdr_checksum = saved_ip_cksum;

    TEST_ASSERT(saved_ip_cksum == calculated_ip_cksum,
        "ICMP IP checksum", "IP checksum should be 0x%04x, got 0x%04x",
        calculated_ip_cksum, saved_ip_cksum);

    // Verify ICMP checksum
    U16 saved_icmp_cksum = icmp_hdr->icmp_cksum;
    icmp_hdr->icmp_cksum = 0;
    U16 calculated_icmp_cksum = (U16)~rte_raw_cksum((const void *)icmp_hdr,
        sizeof(struct rte_icmp_hdr) + sizeof(struct rte_ipv4_hdr) + 8);
    icmp_hdr->icmp_cksum = saved_icmp_cksum;

    TEST_ASSERT(saved_icmp_cksum == calculated_icmp_cksum,
        "ICMP checksum", "ICMP checksum should be 0x%04x, got 0x%04x",
        calculated_icmp_cksum, saved_icmp_cksum);
}

/* Per-lcore subscriber stats: each data/ctrl lcore writes ONLY its own
 * per_ccb_stats row with a plain += (no atomic); readers sum the rows. Verify
 * the write inlines accumulate correctly and that summing two separate per-lcore
 * rows yields the combined total (the metrics / gRPC reader invariant). */
static void test_per_lcore_stats_sum(FastRG_t *fastrg_ccb)
{
    (void)fastrg_ccb;
    struct per_ccb_stats row_a, row_b;
    memset(&row_a, 0, sizeof(row_a));
    memset(&row_b, 0, sizeof(row_b));

    /* lcore A: 3 rx pkts @1000B + 1 drop @100B; lcore B: 2 rx @500B + 1 tx @200B */
    increase_ccb_rx_count(&row_a, 1000);
    increase_ccb_rx_count(&row_a, 1000);
    increase_ccb_rx_count(&row_a, 1000);
    increase_ccb_drop_count(&row_a, 100);
    increase_ccb_rx_count(&row_b, 500);
    increase_ccb_rx_count(&row_b, 500);
    increase_ccb_tx_count(&row_b, 200);

    TEST_ASSERT(row_a.rx_packets == 3 && row_a.rx_bytes == 3000,
        "per-lcore write: row A rx accumulates", "expect 3 pkts / 3000 B");
    TEST_ASSERT(row_b.rx_packets == 2 && row_b.rx_bytes == 1000,
        "per-lcore write: row B rx accumulates", "expect 2 pkts / 1000 B");
    TEST_ASSERT(row_a.dropped_packets == 1 && row_a.dropped_bytes == 100,
        "per-lcore write: row A drop accumulates", "expect 1 pkt / 100 B");

    /* reader-style sum across the two per-lcore rows for the same subscriber */
    TEST_ASSERT(row_a.rx_packets + row_b.rx_packets == 5,
        "per-lcore sum: rx_packets total", "3 + 2 should be 5");
    TEST_ASSERT(row_a.rx_bytes + row_b.rx_bytes == 4000,
        "per-lcore sum: rx_bytes total", "3000 + 1000 should be 4000");
    TEST_ASSERT(row_a.tx_bytes + row_b.tx_bytes == 200,
        "per-lcore sum: tx_bytes total", "0 + 200 should be 200");
    TEST_ASSERT(row_a.dropped_packets + row_b.dropped_packets == 1,
        "per-lcore sum: dropped_packets total", "1 + 0 should be 1");
}

void test_dp_codec(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           DP Codec Unit Tests                              ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_build_icmp_unreach(fastrg_ccb);
    test_per_lcore_stats_sum(fastrg_ccb);

    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║  Test Summary                                              ║\n");
    printf("╠════════════════════════════════════════════════════════════╣\n");
    printf("║  Total Tests:  %3d                                         ║\n", test_count);
    printf("║  Passed:       %3d                                         ║\n", pass_count);
    printf("║  Failed:       %3d                                         ║\n", test_count - pass_count);
    printf("║  Success Rate: %3d%%                                        ║\n", 
           test_count > 0 ? (pass_count * 100 / test_count) : 0);
    printf("╚════════════════════════════════════════════════════════════╝\n");

    if (pass_count == test_count) {
        printf("\n✓ All tests passed!\n");
    } else {
        printf("\n✗ Some tests failed!\n");
    }

    *total_tests += test_count;
    *total_pass += pass_count;
}
