#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <common.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_byteorder.h>
#include <rte_mempool.h>

#include "../../src/fastrg.h"
#include "../../src/dhcpd/dhcp_fsm.h"
#include "../../src/dhcpd/dhcp_codec.h"
#include "../../src/protocol.h"
#include "../../src/pppd/pppd.h"
#include "../test_helper.h"

// Global test counters
static int test_count = 0;
static int pass_count = 0;

#undef BOOT_REQUEST
#undef BOOT_REPLY
#define BOOT_REQUEST    0x1
#define BOOT_REPLY      0x2

// Mock structure for dhcp_opt
typedef struct dhcp_opt {
    U8 opt_type;
    U8 len;
    U8 val[0];
} dhcp_opt_t;

// Mock structure for dhcp_hdr
typedef struct dhcp_hdr {
    U8 msg_type;
    U8 hwr_type;
    U8 hwr_addr_len;
    U8 hops;
    U32 transaction_id;
    U16 sec_elapsed;
    U16 bootp_flag;
    U32 client_ip;
    U32 ur_client_ip;
    U32 next_server_ip;
    U32 relay_agent_ip;
    struct rte_ether_addr mac_addr;
    unsigned char mac_addr_padding[10];
    unsigned char server_name[64];
    unsigned char file_name[128];
    U32 magic_cookie;
    dhcp_opt_t opt_ptr[0];
} dhcp_hdr_t;

// Forward declarations - actual functions from dhcp_codec.c

void test_build_dhcp_offer(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_dhcp_offer function:\n");
    printf("=========================================\n\n");

    char res_pkt[] = {/* mac */0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22, 0x33, 
    0x44, 0x55, 0x66, 0x81, 0x00, /* vlan */0x00, 0x64, 0x08, 0x00, /* ip hdr */0x45, 
    0x00, 0x01, 0x32, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0xb3, 0xbb, 0xc0, 0xa8, 0x02, 
    0x01, 0xc0, 0xa8, 0x02, 0xae, /* udp hdr */0x00, 0x43, 0x00, 0x44, 0x01, 0x1e, 
    0x00, 0x00, /* DHCP */0x02, 0x01, 0x06, 0x00, 0x12, 0x34, 0x56, 0x78, 0x00, 0x01, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xa8, 0x02, 0xae, 0xc0, 0xa8, 0x02, 0x01, 
    0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x63, 0x82, 0x53, 0x63, /* DHCP options */0x35, 0x01, 0x02, 0x36, 0x04, 
    0xc0, 0xa8, 0x02, 0x01, 0x01, 0x04, 0xff, 0xff, 0xff, 0x00, 0x33, 0x04, 0x00, 0x00, 
    0x0e, 0x10, 0x03, 0x04, 0xc0, 0xa8, 0x02, 0x01, /* DNS */0x06, 0x08, 0x08, 0x08, 
    0x08, 0x08, 0x01, 0x01, 0x01, 0x01, 0xff};
    U8 recv_buffer[2048] = {0};

    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)recv_buffer;
    eth_hdr->src_addr.addr_bytes[0] = 0xAA;
    eth_hdr->src_addr.addr_bytes[1] = 0xBB;
    eth_hdr->src_addr.addr_bytes[2] = 0xCC;
    eth_hdr->src_addr.addr_bytes[3] = 0xDD;
    eth_hdr->src_addr.addr_bytes[4] = 0xEE;
    eth_hdr->src_addr.addr_bytes[5] = 0xFF;
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    vlan_header_t *vlan_hdr = (vlan_header_t *)(eth_hdr + 1);
    vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(0x0064);
    vlan_hdr->next_proto = rte_cpu_to_be_16(ETH_P_IP);

    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(vlan_hdr + 1);
    ip_hdr->version_ihl = 0x45;
    ip_hdr->fragment_offset = rte_cpu_to_be_16(0x4000);
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = IPPROTO_UDP;

    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);

    dhcp_hdr_t *dhcp_hdr = (dhcp_hdr_t *)(udp_hdr + 1);
    dhcp_hdr->msg_type = BOOT_REQUEST;
    dhcp_hdr->hwr_type = 1;
    dhcp_hdr->hwr_addr_len = 6;
    dhcp_hdr->transaction_id = rte_cpu_to_be_32(0x12345678);
    dhcp_hdr->sec_elapsed = rte_cpu_to_be_16(0x0001);
    dhcp_hdr->mac_addr = eth_hdr->src_addr;
    dhcp_hdr->magic_cookie = rte_cpu_to_be_32(DHCP_MAGIC_COOKIE);

    // Setup IP pool for testing
    dhcp_ccb_per_lan_user_t pool_user = {0};
    pool_user.ip_pool.ip_addr = rte_cpu_to_be_32(0xC0A802AE); // 192.168.2.174
    pool_user.ip_pool.used = FALSE;

    dhcp_ccb_per_lan_user_t *pool_array[1] = {&pool_user};

    dhcp_ccb_t dhcp_ccb = {0};
    dhcp_ccb.eth_hdr = eth_hdr;
    dhcp_ccb.vlan_hdr = vlan_hdr;
    dhcp_ccb.ip_hdr = ip_hdr;
    dhcp_ccb.udp_hdr = udp_hdr;
    dhcp_ccb.dhcp_server_ip = rte_cpu_to_be_32(0xC0A80201);
    dhcp_ccb.subnet_mask = rte_cpu_to_be_32(0xFFFFFF00);
    dhcp_ccb.per_lan_user_pool = pool_array;
    dhcp_ccb.per_lan_user_pool_len = 1;
    dhcp_ccb.fastrg_ccb = fastrg_ccb;

    dhcp_ccb_per_lan_user_t per_lan_user = {0};
    per_lan_user.dhcp_hdr = dhcp_hdr;
    per_lan_user.dhcp_ccb = &dhcp_ccb;

    // Call the actual function
    struct rte_ether_addr lan_mac = {
        .addr_bytes = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}
    };

    printf("Test 1: \"%s\"\n", "build_dhcp_offer() result");

    STATUS result = build_dhcp_offer(&per_lan_user, &lan_mac);
    TEST_ASSERT(result == SUCCESS, "build_dhcp_offer returned SUCCESS", 
        "got ERROR");
    TEST_ASSERT(dhcp_hdr->msg_type == BOOT_REPLY, "DHCP message type set to BOOT_REPLY", 
        "got %d", dhcp_hdr->msg_type);
    TEST_ASSERT(dhcp_hdr->ur_client_ip != 0, "Client IP was assigned to ", 
        "got %d", rte_be_to_cpu_32(dhcp_hdr->ur_client_ip));
    TEST_ASSERT(dhcp_hdr->next_server_ip == dhcp_ccb.dhcp_server_ip, "Next server IP set to DHCP server IP", 
        "got %d", rte_be_to_cpu_32(dhcp_ccb.dhcp_server_ip));
    // Verify MAC addresses were swapped and set
    TEST_ASSERT(rte_is_same_ether_addr(&eth_hdr->src_addr, &lan_mac), "Ethernet dst MAC set to LAN MAC", 
        "got %02x:%02x:%02x:%02x:%02x:%02x",
        eth_hdr->src_addr.addr_bytes[0], eth_hdr->src_addr.addr_bytes[1],
        eth_hdr->src_addr.addr_bytes[2], eth_hdr->src_addr.addr_bytes[3],
        eth_hdr->src_addr.addr_bytes[4], eth_hdr->src_addr.addr_bytes[5]);

    dhcp_opt_t *options = (dhcp_opt_t *)(dhcp_hdr + 1);
    TEST_ASSERT(options->opt_type == DHCP_MSG_TYPE, "DHCP opt type should be DHCP_MSG_TYPE(53)", 
        "got %d", options->opt_type);
    TEST_ASSERT(options->len == 1, "DHCP message type option length should be 1", 
        "got %d", options->len);
    TEST_ASSERT(options->val[0] == DHCP_OFFER, "DHCP opt value should be DHCP_OFFER", 
        "got %d", options->val[0]);

    BOOL pkt_failed = FALSE;
    test_count++;
    for(int i=0; i<sizeof(res_pkt); i++) {
        if (recv_buffer[i] != (U8)res_pkt[i]) {
            printf("  ✗ FAIL: Packet content mismatch at byte %d: expected 0x%02x, got 0x%02x\n",
                i, (U8)res_pkt[i], recv_buffer[i]);
            pkt_failed = TRUE;
        }
    }
    if (!pkt_failed) {
        pass_count++;
        printf("  ✓ PASS: Packet content matches expected result\n");
    } else {
        TEST_ASSERT(FALSE, "Packet content matches expected result", 
            "Packet content mismatch");
    }

    printf("  All build_dhcp_offer tests passed!\n");
}

void test_build_dhcp_ack(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_dhcp_ack function:\n");
    printf("=========================================\n\n");

    char res_pkt[] = {/* mac */0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22, 0x33, 
    0x44, 0x55, 0x66, 0x81, 0x00, /* vlan */0x00, 0x64, 0x08, 0x00, /* ip hdr */0x45, 
    0x00, 0x01, 0x32, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0xb3, 0xbb, 0xc0, 0xa8, 0x02, 
    0x01, 0xc0, 0xa8, 0x02, 0xae, /* udp hdr */0x00, 0x43, 0x00, 0x44, 0x01, 0x1e, 
    0x00, 0x00, /* DHCP */0x02, 0x01, 0x06, 0x00, 0x12, 0x34, 0x56, 0x78, 0x00, 0x01, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xa8, 0x02, 0xae, 0xc0, 0xa8, 0x02, 0x01, 
    0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x63, 0x82, 0x53, 0x63, /* DHCP options */0x35, 0x01, 0x05, 0x36, 0x04, 
    0xc0, 0xa8, 0x02, 0x01, 0x01, 0x04, 0xff, 0xff, 0xff, 0x00, 0x33, 0x04, 0x00, 0x00, 
    0x0e, 0x10, 0x03, 0x04, 0xc0, 0xa8, 0x02, 0x01, /* DNS */0x06, 0x08, 0x08, 0x08, 
    0x08, 0x08, 0x01, 0x01, 0x01, 0x01, 0xff};
    U8 recv_buffer[2048] = {0};

    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)recv_buffer;
    eth_hdr->src_addr.addr_bytes[0] = 0xAA;
    eth_hdr->src_addr.addr_bytes[1] = 0xBB;
    eth_hdr->src_addr.addr_bytes[2] = 0xCC;
    eth_hdr->src_addr.addr_bytes[3] = 0xDD;
    eth_hdr->src_addr.addr_bytes[4] = 0xEE;
    eth_hdr->src_addr.addr_bytes[5] = 0xFF;
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    vlan_header_t *vlan_hdr = (vlan_header_t *)(eth_hdr + 1);
    vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(0x0064);
    vlan_hdr->next_proto = rte_cpu_to_be_16(ETH_P_IP);

    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(vlan_hdr + 1);
    ip_hdr->version_ihl = 0x45;
    ip_hdr->fragment_offset = rte_cpu_to_be_16(0x4000);
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = IPPROTO_UDP;

    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);

    dhcp_hdr_t *dhcp_hdr = (dhcp_hdr_t *)(udp_hdr + 1);
    dhcp_hdr->msg_type = BOOT_REQUEST;
    dhcp_hdr->hwr_type = 1;
    dhcp_hdr->hwr_addr_len = 6;
    dhcp_hdr->transaction_id = rte_cpu_to_be_32(0x12345678);
    dhcp_hdr->sec_elapsed = rte_cpu_to_be_16(0x0001);
    dhcp_hdr->mac_addr = eth_hdr->src_addr;
    dhcp_hdr->ur_client_ip = rte_cpu_to_be_32(0xC0A802ae); // Pre-assign an IP
    dhcp_hdr->magic_cookie = rte_cpu_to_be_32(DHCP_MAGIC_COOKIE);

    // Setup IP pool for testing
    dhcp_ccb_per_lan_user_t pool_user = {0};
    pool_user.ip_pool.ip_addr = rte_cpu_to_be_32(0xC0A802ae); // 192.168.2.174
    pool_user.ip_pool.used = FALSE;

    dhcp_ccb_per_lan_user_t *pool_array[1] = {&pool_user};

    dhcp_ccb_t dhcp_ccb = {0};
    dhcp_ccb.eth_hdr = eth_hdr;
    dhcp_ccb.vlan_hdr = vlan_hdr;
    dhcp_ccb.ip_hdr = ip_hdr;
    dhcp_ccb.udp_hdr = udp_hdr;
    dhcp_ccb.dhcp_server_ip = rte_cpu_to_be_32(0xC0A80201);
    dhcp_ccb.subnet_mask = rte_cpu_to_be_32(0xFFFFFF00);
    dhcp_ccb.per_lan_user_pool = pool_array;
    dhcp_ccb.per_lan_user_pool_len = 1;
    dhcp_ccb.fastrg_ccb = fastrg_ccb;

    dhcp_ccb_per_lan_user_t per_lan_user = {0};
    per_lan_user.dhcp_hdr = dhcp_hdr;
    per_lan_user.dhcp_ccb = &dhcp_ccb;
    per_lan_user.lan_user_info.timeout_secs = LEASE_TIMEOUT;

    // Call the actual function
    struct rte_ether_addr lan_mac = {
        .addr_bytes = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}
    };

    printf("Test 1: \"%s\"\n", "build_dhcp_ack() result");
    STATUS result = build_dhcp_ack(&per_lan_user, &lan_mac);
    TEST_ASSERT(result == SUCCESS, "build_dhcp_ack returned SUCCESS", 
        "got ERROR");
    TEST_ASSERT(dhcp_hdr->msg_type == BOOT_REPLY, "DHCP message type set to BOOT_REPLY", 
        "expected BOOT_REPLY, got %u", dhcp_hdr->msg_type);
    TEST_ASSERT(dhcp_hdr->ur_client_ip == rte_cpu_to_be_32(0xC0A802ae), "Client IP preserved", 
        "expected 0x%08x, got 0x%08x", rte_cpu_to_be_32(0xC0A802ae), dhcp_hdr->ur_client_ip);

    dhcp_opt_t *options = (dhcp_opt_t *)(dhcp_hdr + 1);
    TEST_ASSERT(options->opt_type == DHCP_MSG_TYPE, "DHCP option type is DHCP_MSG_TYPE", 
        "got %d", options->opt_type);
    TEST_ASSERT(options->len == 1, "DHCP option length is 1", 
        "got %d", options->len);
    TEST_ASSERT(options->val[0] == DHCP_ACK, "DHCP option value is DHCP_ACK", 
        "got %d", options->val[0]);

    BOOL pkt_failed = FALSE;
    test_count++;
    for(int i=0; i<sizeof(res_pkt); i++) {
        if (recv_buffer[i] != (U8)res_pkt[i]) {
            printf("  ✗ FAIL: Packet content mismatch at byte %d: expected 0x%02x, got 0x%02x\n",
                i, (U8)res_pkt[i], recv_buffer[i]);
            pkt_failed = TRUE;
        }
    }
    if (!pkt_failed) {
        pass_count++;
        printf("  ✓ PASS: Packet content matches expected result\n");
    } else {
        TEST_ASSERT(FALSE, "Packet content matches expected result", 
            "Packet content mismatch");
    }

    printf("  All build_dhcp_ack tests passed!\n");
}

void test_build_dhcp_nak(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_dhcp_nak function:\n");
    printf("=========================================\n\n");

    U8 buffer[2048] = {0};

    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    eth_hdr->src_addr.addr_bytes[0] = 0xAA;
    eth_hdr->src_addr.addr_bytes[1] = 0xBB;
    eth_hdr->src_addr.addr_bytes[2] = 0xCC;
    eth_hdr->src_addr.addr_bytes[3] = 0xDD;
    eth_hdr->src_addr.addr_bytes[4] = 0xEE;
    eth_hdr->src_addr.addr_bytes[5] = 0xFF;
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    vlan_header_t *vlan_hdr = (vlan_header_t *)(eth_hdr + 1);
    vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(0x0064);
    vlan_hdr->next_proto = rte_cpu_to_be_16(ETH_P_IP);

    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(vlan_hdr + 1);
    ip_hdr->version_ihl = 0x45;
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = IPPROTO_UDP;

    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);

    dhcp_hdr_t *dhcp_hdr = (dhcp_hdr_t *)(udp_hdr + 1);
    dhcp_hdr->msg_type = BOOT_REQUEST;
    dhcp_hdr->hwr_type = 1;
    dhcp_hdr->hwr_addr_len = 6;
    dhcp_hdr->transaction_id = rte_cpu_to_be_32(0x12345678);
    dhcp_hdr->magic_cookie = rte_cpu_to_be_32(DHCP_MAGIC_COOKIE);

    // Setup IP pool for testing (NAK doesn't need it, but keep structure consistent)
    dhcp_ccb_per_lan_user_t pool_user = {0};
    pool_user.ip_pool.ip_addr = rte_cpu_to_be_32(0xC0A80064);
    pool_user.ip_pool.used = FALSE;

    dhcp_ccb_per_lan_user_t *pool_array[1] = {&pool_user};

    dhcp_ccb_t dhcp_ccb = {0};
    dhcp_ccb.eth_hdr = eth_hdr;
    dhcp_ccb.vlan_hdr = vlan_hdr;
    dhcp_ccb.ip_hdr = ip_hdr;
    dhcp_ccb.udp_hdr = udp_hdr;
    dhcp_ccb.dhcp_server_ip = rte_cpu_to_be_32(0xC0A80001);
    dhcp_ccb.subnet_mask = rte_cpu_to_be_32(0xFFFFFF00);
    dhcp_ccb.per_lan_user_pool = pool_array;
    dhcp_ccb.per_lan_user_pool_len = 1;
    dhcp_ccb.fastrg_ccb = fastrg_ccb;

    dhcp_ccb_per_lan_user_t per_lan_user = {0};
    per_lan_user.dhcp_hdr = dhcp_hdr;
    per_lan_user.dhcp_ccb = &dhcp_ccb;

    // Call the actual function
    struct rte_ether_addr lan_mac = {
        .addr_bytes = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}
    };

    printf("Test 1: \"%s\"\n", "build_dhcp_nak() result");
    STATUS result = build_dhcp_nak(&per_lan_user, &lan_mac);
    TEST_ASSERT(result == SUCCESS, "build_dhcp_nak returned SUCCESS", 
        "build_dhcp_nak returned %d", result);
    TEST_ASSERT(dhcp_hdr->msg_type == BOOT_REPLY, "DHCP message type set to BOOT_REPLY", 
        "expected %d, got %d", BOOT_REPLY, dhcp_hdr->msg_type);
    TEST_ASSERT(dhcp_hdr->ur_client_ip == 0, "Client IP is 0 (no IP assigned for NAK)", 
        "expected 0, got %u", dhcp_hdr->ur_client_ip);

    dhcp_opt_t *options = (dhcp_opt_t *)(dhcp_hdr + 1);
    TEST_ASSERT(options->opt_type == DHCP_MSG_TYPE, "DHCP option type is DHCP_MSG_TYPE", 
        "got %d", options->opt_type);
    TEST_ASSERT(options->len == 1, "DHCP option length is 1", 
        "got %d", options->len);
    TEST_ASSERT(options->val[0] == DHCP_NAK, "DHCP option value is DHCP_NAK", 
        "got %d", options->val[0]);

    printf("  All build_dhcp_nak tests passed!\n");
}

void test_dhcp_codec(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           DHCP Codec Unit Tests                            ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_build_dhcp_offer(fastrg_ccb);
    test_build_dhcp_ack(fastrg_ccb);
    test_build_dhcp_nak(fastrg_ccb);

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
