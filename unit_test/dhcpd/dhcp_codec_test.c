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

/* ==================== dhcp_decode decode-path tests ==================== */

/*
 * Shared fixture for dhcp_decode tests: a full LAN-side DHCP packet
 * (eth + VLAN + IPv4 + UDP + DHCP header) in a stack buffer, plus the
 * dhcp_ccb / per_lan_user / 1-entry IP pool dhcp_decode operates on.
 * Each test writes its own options right after the DHCP header and calls
 * dhcp_decode_ctx_apply, which sets dgram_len from the option length (that is
 * what dhcp_decode derives its option-walk bound from).
 */
typedef struct dhcp_decode_ctx {
    U8 buf[2048];
    struct rte_ether_hdr *eth_hdr;
    vlan_header_t *vlan_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_udp_hdr *udp_hdr;
    dhcp_hdr_t *dhcp_hdr;
    dhcp_ccb_t dhcp_ccb;
    dhcp_ccb_per_lan_user_t per_lan_user;
    dhcp_ccb_per_lan_user_t pool_user;
    dhcp_ccb_per_lan_user_t *pool_array[1];
    int cur_tmp_pool_index;
} dhcp_decode_ctx_t;

static void dhcp_decode_ctx_init(dhcp_decode_ctx_t *f, FastRG_t *fastrg_ccb)
{
    memset(f, 0, sizeof(*f));

    f->eth_hdr = (struct rte_ether_hdr *)f->buf;
    f->eth_hdr->src_addr = (struct rte_ether_addr){
        .addr_bytes = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    f->eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    f->vlan_hdr = (vlan_header_t *)(f->eth_hdr + 1);
    f->vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(0x0064);
    f->vlan_hdr->next_proto = rte_cpu_to_be_16(ETH_P_IP);

    f->ip_hdr = (struct rte_ipv4_hdr *)(f->vlan_hdr + 1);
    f->ip_hdr->version_ihl = 0x45;
    f->ip_hdr->time_to_live = 64;
    f->ip_hdr->next_proto_id = IPPROTO_UDP;

    f->udp_hdr = (struct rte_udp_hdr *)(f->ip_hdr + 1);
    f->udp_hdr->src_port = rte_cpu_to_be_16(DHCP_CLIENT_PORT);
    f->udp_hdr->dst_port = rte_cpu_to_be_16(DHCP_SERVER_PORT);

    f->dhcp_hdr = (dhcp_hdr_t *)(f->udp_hdr + 1);
    f->dhcp_hdr->msg_type = BOOT_REQUEST;
    f->dhcp_hdr->hwr_type = 1;
    f->dhcp_hdr->hwr_addr_len = 6;
    f->dhcp_hdr->transaction_id = rte_cpu_to_be_32(0x12345678);
    f->dhcp_hdr->mac_addr = f->eth_hdr->src_addr;
    f->dhcp_hdr->magic_cookie = rte_cpu_to_be_32(DHCP_MAGIC_COOKIE);

    f->pool_user.ip_pool.ip_addr = rte_cpu_to_be_32(0xC0A80264); /* 192.168.2.100 */
    f->pool_array[0] = &f->pool_user;

    f->dhcp_ccb.dhcp_server_ip = rte_cpu_to_be_32(0xC0A80201); /* 192.168.2.1 */
    f->dhcp_ccb.pool_start = 0xC0A80264; /* 192.168.2.100, host order */
    f->dhcp_ccb.pool_end = 0xC0A80264; /* 192.168.2.100, host order */
    f->dhcp_ccb.subnet_mask = rte_cpu_to_be_32(0xFFFFFF00);
    f->dhcp_ccb.per_lan_user_pool = f->pool_array;
    f->dhcp_ccb.per_lan_user_pool_len = 1;
    f->dhcp_ccb.fastrg_ccb = fastrg_ccb;

    f->per_lan_user.dhcp_ccb = &f->dhcp_ccb;
    rte_timer_init(&f->per_lan_user.lan_user_info.timer);
}

static S16 dhcp_decode_ctx_apply(dhcp_decode_ctx_t *f, const U8 *opts, U16 opt_len)
{
    memcpy((U8 *)(f->dhcp_hdr + 1), opts, opt_len);
    f->udp_hdr->dgram_len = rte_cpu_to_be_16(
        sizeof(struct rte_udp_hdr) + sizeof(dhcp_hdr_t) + opt_len);
    return dhcp_decode(&f->dhcp_ccb, &f->per_lan_user, &f->cur_tmp_pool_index,
        f->eth_hdr, f->vlan_hdr, f->ip_hdr, f->udp_hdr);
}

void test_dhcp_decode(FastRG_t *fastrg_ccb)
{
    printf("\nTesting dhcp_decode function:\n");
    printf("=========================================\n\n");

    dhcp_decode_ctx_t fix;
    S16 event;

    printf("Test 1: \"invalid magic cookie rejected\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    fix.dhcp_hdr->magic_cookie = rte_cpu_to_be_32(0xDEADBEEF);
    const U8 opts_discover[] = {DHCP_MSG_TYPE, 1, DHCP_DISCOVER, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_discover, sizeof(opts_discover));
    TEST_ASSERT(event == ERROR, "bad magic cookie returns ERROR", "got %d", event);

    printf("Test 2: \"DISCOVER decoded to E_DISCOVER\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    event = dhcp_decode_ctx_apply(&fix, opts_discover, sizeof(opts_discover));
    TEST_ASSERT(event == E_DISCOVER, "DISCOVER returns E_DISCOVER", "got %d", event);

    printf("Test 3: \"malformed option overrunning dgram_len rejected\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    /* option claims 200 bytes of value but dgram only carries 4 option bytes */
    const U8 opts_overrun[] = {DHCP_MSG_TYPE, 200, DHCP_DISCOVER, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_overrun, sizeof(opts_overrun));
    TEST_ASSERT(event == ERROR, "overrunning option len returns ERROR", "got %d", event);

    printf("Test 4: \"ISP ID option (60) short-circuits to 0\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    const U8 opts_isp[] = {DHCP_ISP_ID, 3, 'i', 's', 'p',
        DHCP_MSG_TYPE, 1, DHCP_DISCOVER, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_isp, sizeof(opts_isp));
    TEST_ASSERT(event == 0, "ISP ID option returns 0 (no FSM event)", "got %d", event);

    printf("Test 5: \"INFORM decoded to E_INFORM\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    const U8 opts_inform[] = {DHCP_MSG_TYPE, 1, DHCP_INFORM, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_inform, sizeof(opts_inform));
    TEST_ASSERT(event == E_INFORM, "INFORM returns E_INFORM", "got %d", event);

    printf("Test 6: \"RELEASE decoded to E_RELEASE and claims pool slot\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    const U8 opts_release[] = {DHCP_MSG_TYPE, 1, DHCP_RELEASE, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_release, sizeof(opts_release));
    TEST_ASSERT(event == E_RELEASE, "RELEASE returns E_RELEASE", "got %d", event);
    TEST_ASSERT(rte_is_same_ether_addr(&fix.per_lan_user.ip_pool.mac_addr,
        &fix.eth_hdr->src_addr),
        "RELEASE bound client MAC to the pool entry", NULL);

    printf("Test 7: \"valid DECLINE (server-id + in-subnet IP, client in pool)\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    fix.pool_user.ip_pool.used = TRUE; /* is_client_in_pool must find the client */
    U8 opts_decline[] = {DHCP_MSG_TYPE, 1, DHCP_DECLINE,
        DHCP_SERVER_ID, 4, 0, 0, 0, 0,
        DHCP_REQUEST_IP, 4, 0, 0, 0, 0, DHCP_END};
    memcpy(&opts_decline[5], &fix.dhcp_ccb.dhcp_server_ip, 4);
    memcpy(&opts_decline[11], &fix.pool_user.ip_pool.ip_addr, 4);
    event = dhcp_decode_ctx_apply(&fix, opts_decline, sizeof(opts_decline));
    TEST_ASSERT(event == E_DECLINE, "valid DECLINE returns E_DECLINE", "got %d", event);

    printf("Test 8: \"DECLINE without server-id ignored\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    fix.pool_user.ip_pool.used = TRUE;
    U8 opts_decline_no_sid[] = {DHCP_MSG_TYPE, 1, DHCP_DECLINE,
        DHCP_REQUEST_IP, 4, 0, 0, 0, 0, DHCP_END};
    memcpy(&opts_decline_no_sid[5], &fix.pool_user.ip_pool.ip_addr, 4);
    event = dhcp_decode_ctx_apply(&fix, opts_decline_no_sid, sizeof(opts_decline_no_sid));
    TEST_ASSERT(event == -1, "DECLINE without server-id yields no event", "got %d", event);

    printf("Test 9: \"DECLINE for IP outside subnet ignored\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    fix.pool_user.ip_pool.used = TRUE;
    U8 opts_decline_bad_ip[] = {DHCP_MSG_TYPE, 1, DHCP_DECLINE,
        DHCP_SERVER_ID, 4, 0, 0, 0, 0,
        DHCP_REQUEST_IP, 4, 10, 0, 0, 1, DHCP_END};
    memcpy(&opts_decline_bad_ip[5], &fix.dhcp_ccb.dhcp_server_ip, 4);
    event = dhcp_decode_ctx_apply(&fix, opts_decline_bad_ip, sizeof(opts_decline_bad_ip));
    TEST_ASSERT(event == -1, "out-of-subnet DECLINE yields no event", "got %d", event);

    printf("Test 10: \"SELECTING REQUEST with Option 50 in subnet (Option 55 skipped)\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    U8 opts_req_sel[] = {DHCP_MSG_TYPE, 1, DHCP_REQUEST,
        DHCP_CLIENT_ID, 7, DHCP_HW_TYPE_ETHERNET, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
        DHCP_REQUEST_IP, 4, 0, 0, 0, 0,
        DHCP_PARAMETER_LIST, 4, DHCP_SUBNET_MASK, DHCP_ROUTER, DHCP_DNS, DHCP_LEASE_TIME,
        DHCP_END};
    memcpy(&opts_req_sel[14], &fix.pool_user.ip_pool.ip_addr, 4);
    event = dhcp_decode_ctx_apply(&fix, opts_req_sel, sizeof(opts_req_sel));
    TEST_ASSERT(event == E_GOOD_REQUEST, "selecting REQUEST returns E_GOOD_REQUEST",
        "got %d", event);
    TEST_ASSERT(fix.dhcp_hdr->ur_client_ip == fix.pool_user.ip_pool.ip_addr,
        "yiaddr set to the requested IP (Option 50)",
        "got 0x%08x", rte_be_to_cpu_32(fix.dhcp_hdr->ur_client_ip));

    printf("Test 11: \"SELECTING REQUEST with Option 50 outside subnet\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    U8 opts_req_bad_ip[] = {DHCP_MSG_TYPE, 1, DHCP_REQUEST,
        DHCP_REQUEST_IP, 4, 10, 0, 0, 1, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_req_bad_ip, sizeof(opts_req_bad_ip));
    TEST_ASSERT(event == E_BAD_REQUEST, "out-of-subnet Option 50 returns E_BAD_REQUEST",
        "got %d", event);

    printf("Test 12: \"SELECTING REQUEST with malformed Option 50 length\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    const U8 opts_req_short_50[] = {DHCP_MSG_TYPE, 1, DHCP_REQUEST,
        DHCP_REQUEST_IP, 2, 0xC0, 0xA8, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_req_short_50, sizeof(opts_req_short_50));
    TEST_ASSERT(event == -1, "malformed Option 50 length yields no event", "got %d", event);

    printf("Test 13: \"SELECTING REQUEST with malformed Client ID (Option 61)\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    const U8 opts_req_short_61[] = {DHCP_MSG_TYPE, 1, DHCP_REQUEST,
        DHCP_CLIENT_ID, 3, DHCP_HW_TYPE_ETHERNET, 0xAA, 0xBB, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_req_short_61, sizeof(opts_req_short_61));
    TEST_ASSERT(event == -1, "too-short Client ID yields no event", "got %d", event);

    printf("Test 14: \"SELECTING REQUEST with non-Ethernet Client ID hardware type\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    const U8 opts_req_bad_hw[] = {DHCP_MSG_TYPE, 1, DHCP_REQUEST,
        DHCP_CLIENT_ID, 7, 0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_req_bad_hw, sizeof(opts_req_bad_hw));
    TEST_ASSERT(event == -1, "non-Ethernet Client ID hw type yields no event",
        "got %d", event);

    printf("Test 15: \"SELECTING REQUEST with wrong server-id\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    U8 opts_req_wrong_sid[] = {DHCP_MSG_TYPE, 1, DHCP_REQUEST,
        DHCP_SERVER_ID, 4, 0xC0, 0xA8, 0x02, 0xFE, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_req_wrong_sid, sizeof(opts_req_wrong_sid));
    TEST_ASSERT(event == -1, "REQUEST for another server yields no event", "got %d", event);

    printf("Test 16: \"renewal REQUEST (ciaddr set, unicast, matching server-id)\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    U32 leased_ip = rte_cpu_to_be_32(0xC0A80264);
    fix.dhcp_hdr->client_ip = leased_ip;
    fix.ip_hdr->dst_addr = fix.dhcp_ccb.dhcp_server_ip; /* unicast = renewal */
    U8 opts_req_renew[] = {DHCP_MSG_TYPE, 1, DHCP_REQUEST,
        DHCP_SERVER_ID, 4, 0, 0, 0, 0, DHCP_END};
    memcpy(&opts_req_renew[5], &fix.dhcp_ccb.dhcp_server_ip, 4);
    event = dhcp_decode_ctx_apply(&fix, opts_req_renew, sizeof(opts_req_renew));
    TEST_ASSERT(event == E_GOOD_REQUEST, "renewal REQUEST returns E_GOOD_REQUEST",
        "got %d", event);
    TEST_ASSERT(fix.dhcp_hdr->ur_client_ip == leased_ip,
        "yiaddr carries the renewed lease IP",
        "got 0x%08x", rte_be_to_cpu_32(fix.dhcp_hdr->ur_client_ip));
    TEST_ASSERT(fix.dhcp_hdr->client_ip == 0, "ciaddr zeroed for the ACK",
        "got 0x%08x", rte_be_to_cpu_32(fix.dhcp_hdr->client_ip));
    TEST_ASSERT(fix.per_lan_user.lan_user_info.timeout_secs == LEASE_TIMEOUT,
        "lease timeout re-armed to LEASE_TIMEOUT",
        "got %u", fix.per_lan_user.lan_user_info.timeout_secs);

    printf("Test 17: \"renewal REQUEST with ciaddr outside subnet\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    fix.dhcp_hdr->client_ip = rte_cpu_to_be_32(0x0A000001); /* 10.0.0.1 */
    fix.ip_hdr->dst_addr = fix.dhcp_ccb.dhcp_server_ip;
    event = dhcp_decode_ctx_apply(&fix, opts_req_renew, sizeof(opts_req_renew));
    TEST_ASSERT(event == E_BAD_REQUEST, "out-of-subnet ciaddr returns E_BAD_REQUEST",
        "got %d", event);

    printf("Test 18: \"SELECTING REQUEST with Option 50 above pool end\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    U8 opts_req_above_pool[] = {DHCP_MSG_TYPE, 1, DHCP_REQUEST,
        DHCP_REQUEST_IP, 4, 192, 168, 2, 101, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_req_above_pool, sizeof(opts_req_above_pool));
    TEST_ASSERT(event == E_BAD_REQUEST, "above-pool Option 50 returns E_BAD_REQUEST",
        "got %d", event);

    printf("Test 19: \"SELECTING REQUEST with Option 50 below pool start\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    U8 opts_req_below_pool[] = {DHCP_MSG_TYPE, 1, DHCP_REQUEST,
        DHCP_REQUEST_IP, 4, 192, 168, 2, 99, DHCP_END};
    event = dhcp_decode_ctx_apply(&fix, opts_req_below_pool, sizeof(opts_req_below_pool));
    TEST_ASSERT(event == E_BAD_REQUEST, "below-pool Option 50 returns E_BAD_REQUEST",
        "got %d", event);

    printf("Test 20: \"renewal REQUEST with ciaddr outside configured pool\"\n");
    dhcp_decode_ctx_init(&fix, fastrg_ccb);
    fix.dhcp_hdr->client_ip = rte_cpu_to_be_32(0xC0A802C8); /* 192.168.2.200 */
    fix.ip_hdr->dst_addr = fix.dhcp_ccb.dhcp_server_ip;
    event = dhcp_decode_ctx_apply(&fix, opts_req_renew, sizeof(opts_req_renew));
    TEST_ASSERT(event == E_BAD_REQUEST, "out-of-pool ciaddr returns E_BAD_REQUEST",
        "got %d", event);

    printf("  All dhcp_decode tests done.\n");
}

/* ==================== build_dhcp_ack_inform tests ==================== */

void test_build_dhcp_ack_inform(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_dhcp_ack_inform function:\n");
    printf("=========================================\n\n");

    dhcp_decode_ctx_t fix;
    dhcp_decode_ctx_init(&fix, fastrg_ccb);

    /* an INFORM client already owns its IP: ciaddr set, no lease wanted */
    U32 client_ip = rte_cpu_to_be_32(0xC0A802AE); /* 192.168.2.174 */
    fix.dhcp_hdr->client_ip = client_ip;
    fix.dhcp_ccb.eth_hdr = fix.eth_hdr;
    fix.dhcp_ccb.vlan_hdr = fix.vlan_hdr;
    fix.dhcp_ccb.ip_hdr = fix.ip_hdr;
    fix.dhcp_ccb.udp_hdr = fix.udp_hdr;
    fix.per_lan_user.dhcp_hdr = fix.dhcp_hdr;

    struct rte_ether_addr client_mac = fix.eth_hdr->src_addr;
    struct rte_ether_addr lan_mac = {
        .addr_bytes = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};

    printf("Test 1: \"build_dhcp_ack_inform() result\"\n");
    STATUS result = build_dhcp_ack_inform(&fix.per_lan_user, &lan_mac);
    TEST_ASSERT(result == SUCCESS, "build_dhcp_ack_inform returned SUCCESS",
        "got %d", result);
    TEST_ASSERT(fix.dhcp_hdr->msg_type == BOOT_REPLY, "DHCP message type is BOOT_REPLY",
        "got %d", fix.dhcp_hdr->msg_type);
    TEST_ASSERT(fix.dhcp_hdr->ur_client_ip == 0, "yiaddr is 0 (INFORM assigns no lease)",
        "got 0x%08x", rte_be_to_cpu_32(fix.dhcp_hdr->ur_client_ip));
    TEST_ASSERT(fix.ip_hdr->dst_addr == client_ip, "IP dst is the client's own address",
        "got 0x%08x", rte_be_to_cpu_32(fix.ip_hdr->dst_addr));
    TEST_ASSERT(fix.ip_hdr->src_addr == fix.dhcp_ccb.dhcp_server_ip,
        "IP src is the DHCP server address",
        "got 0x%08x", rte_be_to_cpu_32(fix.ip_hdr->src_addr));
    TEST_ASSERT(fix.udp_hdr->src_port == rte_cpu_to_be_16(DHCP_SERVER_PORT) &&
        fix.udp_hdr->dst_port == rte_cpu_to_be_16(DHCP_CLIENT_PORT),
        "UDP ports are 67 -> 68", "got %u -> %u",
        rte_be_to_cpu_16(fix.udp_hdr->src_port), rte_be_to_cpu_16(fix.udp_hdr->dst_port));
    TEST_ASSERT(rte_is_same_ether_addr(&fix.eth_hdr->dst_addr, &client_mac),
        "Ethernet dst is the client MAC", NULL);
    TEST_ASSERT(rte_is_same_ether_addr(&fix.eth_hdr->src_addr, &lan_mac),
        "Ethernet src is the LAN MAC", NULL);

    printf("Test 2: \"ACK(INFORM) option sequence\"\n");
    dhcp_opt_t *opt = (dhcp_opt_t *)(fix.dhcp_hdr + 1);
    TEST_ASSERT(opt->opt_type == DHCP_MSG_TYPE && opt->len == 1 &&
        opt->val[0] == DHCP_ACK,
        "option 53 is DHCP_ACK", "type=%u len=%u val=%u",
        opt->opt_type, opt->len, opt->val[0]);

    opt = (dhcp_opt_t *)((U8 *)(opt + 1) + opt->len);
    TEST_ASSERT(opt->opt_type == DHCP_SERVER_ID && opt->len == 4 &&
        memcmp(opt->val, &fix.dhcp_ccb.dhcp_server_ip, 4) == 0,
        "option 54 carries the server IP", "type=%u len=%u", opt->opt_type, opt->len);

    opt = (dhcp_opt_t *)((U8 *)(opt + 1) + opt->len);
    TEST_ASSERT(opt->opt_type == DHCP_SUBNET_MASK && opt->len == 4 &&
        memcmp(opt->val, &fix.dhcp_ccb.subnet_mask, 4) == 0,
        "option 1 carries the subnet mask", "type=%u len=%u", opt->opt_type, opt->len);

    opt = (dhcp_opt_t *)((U8 *)(opt + 1) + opt->len);
    TEST_ASSERT(opt->opt_type == DHCP_ROUTER && opt->len == 4 &&
        memcmp(opt->val, &fix.dhcp_ccb.dhcp_server_ip, 4) == 0,
        "option 3 carries the gateway (server IP)", "type=%u len=%u",
        opt->opt_type, opt->len);

    opt = (dhcp_opt_t *)((U8 *)(opt + 1) + opt->len);
    const U8 expected_dns[8] = {8, 8, 8, 8, 1, 1, 1, 1}; /* UNIT_TEST stub DNS pair */
    TEST_ASSERT(opt->opt_type == DHCP_DNS && opt->len == 8 &&
        memcmp(opt->val, expected_dns, 8) == 0,
        "option 6 carries both DNS servers", "type=%u len=%u", opt->opt_type, opt->len);

    opt = (dhcp_opt_t *)((U8 *)(opt + 1) + opt->len);
    TEST_ASSERT(*(U8 *)opt == DHCP_END, "options terminated with END",
        "got %u", *(U8 *)opt);

    printf("Test 3: \"ACK(INFORM) length fields\"\n");
    U16 opt_bytes = 3 + 6 + 6 + 6 + 10 + 1;
    U16 expect_dgram = sizeof(struct rte_udp_hdr) + sizeof(dhcp_hdr_t) + opt_bytes;
    TEST_ASSERT(fix.udp_hdr->dgram_len == rte_cpu_to_be_16(expect_dgram),
        "UDP dgram_len covers header + DHCP + options",
        "expect %u got %u", expect_dgram, rte_be_to_cpu_16(fix.udp_hdr->dgram_len));
    TEST_ASSERT(fix.ip_hdr->total_length ==
        rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + expect_dgram),
        "IP total_length covers IP + UDP payload",
        "got %u", rte_be_to_cpu_16(fix.ip_hdr->total_length));

    printf("  All build_dhcp_ack_inform tests done.\n");
}

/* ============== RFC 2131 BROADCAST-flag reply addressing tests ============== */

typedef struct bcast_fix {
    U8 buffer[2048];
    struct rte_ether_hdr *eth_hdr;
    vlan_header_t *vlan_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_udp_hdr *udp_hdr;
    dhcp_hdr_t *dhcp_hdr;
    dhcp_ccb_t dhcp_ccb;
    dhcp_ccb_per_lan_user_t per_lan_user;
    dhcp_ccb_per_lan_user_t pool_user;
    dhcp_ccb_per_lan_user_t *pool_array[1];
    struct rte_ether_addr client_mac;
    struct rte_ether_addr lan_mac;
} bcast_fix_t;

static void setup_bcast_fix(bcast_fix_t *fix, FastRG_t *fastrg_ccb, U16 bootp_flag_be)
{
    memset(fix, 0, sizeof(*fix));

    fix->eth_hdr = (struct rte_ether_hdr *)fix->buffer;
    fix->eth_hdr->src_addr.addr_bytes[0] = 0xAA;
    fix->eth_hdr->src_addr.addr_bytes[1] = 0xBB;
    fix->eth_hdr->src_addr.addr_bytes[2] = 0xCC;
    fix->eth_hdr->src_addr.addr_bytes[3] = 0xDD;
    fix->eth_hdr->src_addr.addr_bytes[4] = 0xEE;
    fix->eth_hdr->src_addr.addr_bytes[5] = 0xFF;
    fix->eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);
    fix->client_mac = fix->eth_hdr->src_addr;

    fix->vlan_hdr = (vlan_header_t *)(fix->eth_hdr + 1);
    fix->vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(0x0064);
    fix->vlan_hdr->next_proto = rte_cpu_to_be_16(ETH_P_IP);

    fix->ip_hdr = (struct rte_ipv4_hdr *)(fix->vlan_hdr + 1);
    fix->ip_hdr->version_ihl = 0x45;
    fix->ip_hdr->time_to_live = 64;
    fix->ip_hdr->next_proto_id = IPPROTO_UDP;

    fix->udp_hdr = (struct rte_udp_hdr *)(fix->ip_hdr + 1);

    fix->dhcp_hdr = (dhcp_hdr_t *)(fix->udp_hdr + 1);
    fix->dhcp_hdr->msg_type = BOOT_REQUEST;
    fix->dhcp_hdr->hwr_type = 1;
    fix->dhcp_hdr->hwr_addr_len = 6;
    fix->dhcp_hdr->transaction_id = rte_cpu_to_be_32(0x12345678);
    fix->dhcp_hdr->bootp_flag = bootp_flag_be;
    fix->dhcp_hdr->mac_addr = fix->client_mac;
    fix->dhcp_hdr->magic_cookie = rte_cpu_to_be_32(DHCP_MAGIC_COOKIE);

    fix->pool_user.ip_pool.ip_addr = rte_cpu_to_be_32(0xC0A802AE);
    fix->pool_user.ip_pool.used = FALSE;
    fix->pool_array[0] = &fix->pool_user;

    fix->dhcp_ccb.eth_hdr = fix->eth_hdr;
    fix->dhcp_ccb.vlan_hdr = fix->vlan_hdr;
    fix->dhcp_ccb.ip_hdr = fix->ip_hdr;
    fix->dhcp_ccb.udp_hdr = fix->udp_hdr;
    fix->dhcp_ccb.dhcp_server_ip = rte_cpu_to_be_32(0xC0A80201);
    fix->dhcp_ccb.subnet_mask = rte_cpu_to_be_32(0xFFFFFF00);
    fix->dhcp_ccb.per_lan_user_pool = fix->pool_array;
    fix->dhcp_ccb.per_lan_user_pool_len = 1;
    fix->dhcp_ccb.fastrg_ccb = fastrg_ccb;

    fix->per_lan_user.dhcp_hdr = fix->dhcp_hdr;
    fix->per_lan_user.dhcp_ccb = &fix->dhcp_ccb;

    fix->lan_mac = (struct rte_ether_addr){
        .addr_bytes = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
}

void test_dhcp_reply_broadcast_flag(FastRG_t *fastrg_ccb)
{
    printf("\nTesting RFC 2131 BROADCAST-flag reply addressing:\n");
    printf("=========================================\n\n");

    bcast_fix_t fix;

    printf("Test 1: \"OFFER with BROADCAST flag is broadcast\"\n");
    setup_bcast_fix(&fix, fastrg_ccb, rte_cpu_to_be_16(0x8000));
    TEST_ASSERT(build_dhcp_offer(&fix.per_lan_user, &fix.lan_mac) == SUCCESS,
        "build_dhcp_offer returned SUCCESS", NULL);
    TEST_ASSERT(rte_is_broadcast_ether_addr(&fix.eth_hdr->dst_addr),
        "Ethernet dst is ff:ff:ff:ff:ff:ff", "got %02x:%02x:...",
        fix.eth_hdr->dst_addr.addr_bytes[0], fix.eth_hdr->dst_addr.addr_bytes[1]);
    TEST_ASSERT(fix.ip_hdr->dst_addr == RTE_IPV4_BROADCAST,
        "IP dst is 255.255.255.255", "got 0x%08x", rte_be_to_cpu_32(fix.ip_hdr->dst_addr));

    printf("Test 2: \"OFFER without BROADCAST flag stays unicast\"\n");
    setup_bcast_fix(&fix, fastrg_ccb, 0);
    TEST_ASSERT(build_dhcp_offer(&fix.per_lan_user, &fix.lan_mac) == SUCCESS,
        "build_dhcp_offer returned SUCCESS", NULL);
    TEST_ASSERT(rte_is_same_ether_addr(&fix.eth_hdr->dst_addr, &fix.client_mac),
        "Ethernet dst is the client MAC", NULL);
    TEST_ASSERT(fix.ip_hdr->dst_addr == rte_cpu_to_be_32(0xC0A802AE),
        "IP dst is the assigned pool address", "got 0x%08x",
        rte_be_to_cpu_32(fix.ip_hdr->dst_addr));

    printf("Test 3: \"ACK with BROADCAST flag is broadcast\"\n");
    setup_bcast_fix(&fix, fastrg_ccb, rte_cpu_to_be_16(0x8000));
    fix.dhcp_hdr->ur_client_ip = rte_cpu_to_be_32(0xC0A802AE);
    TEST_ASSERT(build_dhcp_ack(&fix.per_lan_user, &fix.lan_mac) == SUCCESS,
        "build_dhcp_ack returned SUCCESS", NULL);
    TEST_ASSERT(rte_is_broadcast_ether_addr(&fix.eth_hdr->dst_addr),
        "Ethernet dst is ff:ff:ff:ff:ff:ff", NULL);
    TEST_ASSERT(fix.ip_hdr->dst_addr == RTE_IPV4_BROADCAST,
        "IP dst is 255.255.255.255", "got 0x%08x", rte_be_to_cpu_32(fix.ip_hdr->dst_addr));

    printf("Test 4: \"NAK is broadcast even without the flag\"\n");
    setup_bcast_fix(&fix, fastrg_ccb, 0);
    TEST_ASSERT(build_dhcp_nak(&fix.per_lan_user, &fix.lan_mac) == SUCCESS,
        "build_dhcp_nak returned SUCCESS", NULL);
    TEST_ASSERT(rte_is_broadcast_ether_addr(&fix.eth_hdr->dst_addr),
        "Ethernet dst is ff:ff:ff:ff:ff:ff", NULL);
    TEST_ASSERT(fix.ip_hdr->dst_addr == RTE_IPV4_BROADCAST,
        "IP dst is 255.255.255.255", "got 0x%08x", rte_be_to_cpu_32(fix.ip_hdr->dst_addr));

    printf("  All BROADCAST-flag reply addressing tests passed!\n");
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
    test_dhcp_decode(fastrg_ccb);
    test_build_dhcp_ack_inform(fastrg_ccb);
    test_dhcp_reply_broadcast_flag(fastrg_ccb);

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
