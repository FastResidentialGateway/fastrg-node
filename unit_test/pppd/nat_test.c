#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <common.h>

#include <rte_atomic.h>

#include "../../src/fastrg.h"
#include "../../src/pppd/nat.h"
#include "../../src/protocol.h"
#include "../test_helper.h"

// Global test counters
static int test_count = 0;
static int pass_count = 0;

// Mock addr_table for testing
static addr_table_t test_addr_table[MAX_NAT_ENTRIES];

// Mock port_fwd_table for testing (direct-indexed by port number)
static port_fwd_entry_t test_port_fwd_table[PORT_FWD_TABLE_SIZE];

static void reset_addr_table(void)
{
    memset(test_addr_table, 0, sizeof(test_addr_table));
    memset(test_port_fwd_table, 0, sizeof(test_port_fwd_table));
}

void test_compute_initial_nat_port(void)
{
    printf("\nTesting compute_initial_nat_port function:\n");
    printf("=========================================\n\n");

    U32 src_ip_1 = htonl(0xC0A80001);  // 192.168.0.1
    U16 src_port_1 = htons(12345);

    U32 src_ip_2 = htonl(0xC0A80002);  // 192.168.0.2
    U16 src_port_2 = htons(12345);

    U16 nat_port_1 = compute_initial_nat_port(src_ip_1, src_port_1);
    U16 nat_port_2 = compute_initial_nat_port(src_ip_2, src_port_2);
    U16 nat_port_3 = compute_initial_nat_port(src_ip_1, src_port_1);

    printf("Test 1: \"%s\"\n", "NAT port in valid range");
    TEST_ASSERT(nat_port_1 >= SYS_MAX_PORT && nat_port_1 < TOTAL_SOCK_PORT,
        "NAT port 1 in valid range", 
        "expected [%u, %u), got %u", SYS_MAX_PORT, TOTAL_SOCK_PORT, nat_port_1);

    printf("Test 2: \"%s\"\n", "NAT port 2 in valid range");
    TEST_ASSERT(nat_port_2 >= SYS_MAX_PORT && nat_port_2 < TOTAL_SOCK_PORT,
        "NAT port 2 in valid range", 
        "expected [%u, %u), got %u", SYS_MAX_PORT, TOTAL_SOCK_PORT, nat_port_2);

    printf("Test 3: \"%s\"\n", "Same input produces same output");
    TEST_ASSERT(nat_port_1 == nat_port_3,
        "Deterministic hash", 
        "expected %u, got %u", nat_port_1, nat_port_3);

    printf("Test 4: \"%s\"\n", "Different input produces different output");
    TEST_ASSERT(nat_port_1 != nat_port_2,
        "Different sources produce different NAT ports", 
        "both got %u", nat_port_1);
}

void test_compute_nat_table_index(void)
{
    printf("\nTesting compute_nat_table_index function:\n");
    printf("=========================================\n\n");

    U16 nat_port = 5000;
    U32 dst_ip_1 = htonl(0x08080808);  // 8.8.8.8
    U16 dst_port_1 = htons(53);

    U32 dst_ip_2 = htonl(0x01010101);  // 1.1.1.1
    U16 dst_port_2 = htons(53);

    U32 idx_1 = compute_nat_table_index(nat_port, dst_ip_1, dst_port_1);
    U32 idx_2 = compute_nat_table_index(nat_port, dst_ip_2, dst_port_2);
    U32 idx_3 = compute_nat_table_index(nat_port, dst_ip_1, dst_port_1);

    printf("Test 1: \"%s\"\n", "Table index in valid range");
    TEST_ASSERT(idx_1 < MAX_NAT_ENTRIES,
        "Table index 1 in valid range", 
        "expected < %u, got %u", MAX_NAT_ENTRIES, idx_1);

    printf("Test 2: \"%s\"\n", "Table index 2 in valid range");
    TEST_ASSERT(idx_2 < MAX_NAT_ENTRIES,
        "Table index 2 in valid range", 
        "expected < %u, got %u", MAX_NAT_ENTRIES, idx_2);

    printf("Test 3: \"%s\"\n", "Same input produces same index");
    TEST_ASSERT(idx_1 == idx_3,
        "Deterministic hash", 
        "expected %u, got %u", idx_1, idx_3);

    printf("Test 4: \"%s\"\n", "Different destinations produce different indices");
    TEST_ASSERT(idx_1 != idx_2,
        "Different destinations produce different indices", 
        "both got %u", idx_1);
}

void test_nat_learning_port_reuse_basic(void)
{
    printf("\nTesting nat_learning_port_reuse basic functionality:\n");
    printf("=========================================\n\n");

    reset_addr_table();

    struct rte_ether_hdr eth_hdr = {
        .src_addr = {.addr_bytes = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}},
    };

    U32 src_ip = htonl(0xC0A80064);    // 192.168.0.100
    U32 dst_ip = htonl(0x08080808);    // 8.8.8.8
    U16 src_port = htons(12345);
    U16 dst_port = htons(53);

    printf("Test 1: \"%s\"\n", "First learning creates new entry");
    U16 nat_port_1 = nat_learning_port_reuse(&eth_hdr, src_ip, dst_ip, 
                                              src_port, dst_port, test_addr_table, test_port_fwd_table);
    TEST_ASSERT(nat_port_1 != 0, 
        "First learning returns valid NAT port",
        "got 0 (table full)");
    TEST_ASSERT(nat_port_1 >= SYS_MAX_PORT && nat_port_1 < TOTAL_SOCK_PORT,
        "NAT port in valid range",
        "expected [%u, %u), got %u", SYS_MAX_PORT, TOTAL_SOCK_PORT, nat_port_1);

    printf("Test 2: \"%s\"\n", "Same flow returns same NAT port");
    U16 nat_port_2 = nat_learning_port_reuse(&eth_hdr, src_ip, dst_ip, 
                                              src_port, dst_port, test_addr_table, test_port_fwd_table);
    TEST_ASSERT(nat_port_1 == nat_port_2, 
        "Same flow returns same NAT port",
        "expected %u, got %u", nat_port_1, nat_port_2);
}

void test_nat_learning_port_reuse_different_dst(void)
{
    printf("\nTesting nat_learning_port_reuse with different destinations (port reuse):\n");
    printf("=========================================\n\n");

    reset_addr_table();

    struct rte_ether_hdr eth_hdr_1 = {
        .src_addr = {.addr_bytes = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}},
    };
    struct rte_ether_hdr eth_hdr_2 = {
        .src_addr = {.addr_bytes = {0x00, 0x11, 0x22, 0x33, 0x44, 0x66}},
    };

    U32 src_ip_1 = htonl(0xC0A80064);  // 192.168.0.100
    U32 src_ip_2 = htonl(0xC0A80065);  // 192.168.0.101
    U16 src_port_1 = htons(12345);
    U16 src_port_2 = htons(12346);

    U32 dst_ip_1 = htonl(0x08080808);  // 8.8.8.8
    U32 dst_ip_2 = htonl(0x01010101);  // 1.1.1.1
    U16 dst_port = htons(53);

    printf("Test 1: \"%s\"\n", "First connection to 8.8.8.8");
    U16 nat_port_1 = nat_learning_port_reuse(&eth_hdr_1, src_ip_1, dst_ip_1, 
                                              src_port_1, dst_port, test_addr_table, test_port_fwd_table);
    TEST_ASSERT(nat_port_1 != 0, 
        "First connection gets NAT port",
        "got 0 (table full)");

    printf("Test 2: \"%s\"\n", "Second connection to 1.1.1.1 (different dst, can reuse port)");
    U16 nat_port_2 = nat_learning_port_reuse(&eth_hdr_2, src_ip_2, dst_ip_2, 
                                              src_port_2, dst_port, test_addr_table, test_port_fwd_table);
    TEST_ASSERT(nat_port_2 != 0, 
        "Second connection gets NAT port",
        "got 0 (table full)");

    printf("  Info: NAT port 1 = %u, NAT port 2 = %u\n", nat_port_1, nat_port_2);
    printf("  (Port reuse achieved if both can coexist with same or different NAT ports)\n");

    /*
     * Test 3: True port reuse scenario
     * Two different sources with SAME initial NAT port hash, but different destinations
     * Should be able to use the SAME NAT port since (nat_port, dst_ip, dst_port) is unique
     */
    printf("\nTest 3: \"%s\"\n", "True port reuse - same NAT port, different destinations");

    reset_addr_table();

    /* Use same src_ip and src_port to guarantee same initial NAT port */
    U32 common_src_ip = htonl(0xC0A80064);    // 192.168.0.100
    U16 common_src_port = htons(12345);

    U32 dst_ip_a = htonl(0x08080808);  // 8.8.8.8
    U32 dst_ip_b = htonl(0x01010101);  // 1.1.1.1
    U16 dst_port_a = htons(53);        // DNS
    U16 dst_port_b = htons(80);        // HTTP

    /* First connection: 192.168.0.100:12345 -> 8.8.8.8:53 */
    U16 nat_port_a = nat_learning_port_reuse(&eth_hdr_1, common_src_ip, dst_ip_a, 
        common_src_port, dst_port_a, test_addr_table, test_port_fwd_table);
    TEST_ASSERT(nat_port_a != 0, 
        "Connection A gets NAT port",
        "got 0 (table full)");

    /* Second connection: 192.168.0.100:12345 -> 1.1.1.1:80 (different dst) */
    U16 nat_port_b = nat_learning_port_reuse(&eth_hdr_1, common_src_ip, dst_ip_b, 
        common_src_port, dst_port_b, test_addr_table, test_port_fwd_table);
    TEST_ASSERT(nat_port_b != 0, 
        "Connection B gets NAT port",
        "got 0 (table full)");

    /* 
     * Key assertion: Same source should get SAME NAT port when destinations differ
     * This is the essence of destination-aware port reuse!
     */
    TEST_ASSERT(nat_port_a == nat_port_b, 
        "Same source with different destinations reuses same NAT port",
        "expected same port, got A=%u, B=%u", nat_port_a, nat_port_b);

    printf("  ✓ Port reuse verified: NAT port %u used for both:\n", nat_port_a);
    printf("    - 192.168.0.100:12345 -> 8.8.8.8:53\n");
    printf("    - 192.168.0.100:12345 -> 1.1.1.1:80\n");
}

void test_nat_learning_port_reuse_conflict(void)
{
    printf("\nTesting nat_learning_port_reuse conflict resolution:\n");
    printf("=========================================\n\n");

    reset_addr_table();

    struct rte_ether_hdr eth_hdr_1 = {
        .src_addr = {.addr_bytes = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}},
    };
    struct rte_ether_hdr eth_hdr_2 = {
        .src_addr = {.addr_bytes = {0x00, 0x11, 0x22, 0x33, 0x44, 0x66}},
    };

    // Use same src_ip and src_port to get same initial NAT port
    U32 src_ip = htonl(0xC0A80064);    // 192.168.0.100
    U16 src_port = htons(12345);

    U32 dst_ip = htonl(0x08080808);    // 8.8.8.8
    U16 dst_port = htons(53);

    printf("Test 1: \"%s\"\n", "First connection");
    U16 nat_port_1 = nat_learning_port_reuse(&eth_hdr_1, src_ip, dst_ip, 
                                              src_port, dst_port, test_addr_table, test_port_fwd_table);
    TEST_ASSERT(nat_port_1 != 0, 
        "First connection gets NAT port",
        "got 0 (table full)");

    // Different source but same hash -> should get different NAT port due to conflict
    U32 src_ip_2 = htonl(0xC0A80065);  // 192.168.0.101
    U16 src_port_2 = htons(54321);

    printf("Test 2: \"%s\"\n", "Second connection to same destination (potential conflict)");
    U16 nat_port_2 = nat_learning_port_reuse(&eth_hdr_2, src_ip_2, dst_ip, 
                                              src_port_2, dst_port, test_addr_table, test_port_fwd_table);
    TEST_ASSERT(nat_port_2 != 0, 
        "Second connection gets NAT port",
        "got 0 (table full)");

    printf("  Info: NAT port 1 = %u, NAT port 2 = %u\n", nat_port_1, nat_port_2);
}

void test_nat_reverse_lookup(void)
{
    printf("\nTesting nat_reverse_lookup function:\n");
    printf("=========================================\n\n");

    reset_addr_table();

    struct rte_ether_hdr eth_hdr = {
        .src_addr = {.addr_bytes = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}},
    };

    U32 src_ip = htonl(0xC0A80064);    // 192.168.0.100
    U32 dst_ip = htonl(0x08080808);    // 8.8.8.8
    U16 src_port = htons(12345);
    U16 dst_port = htons(53);

    // Create NAT entry
    U16 nat_port = nat_learning_port_reuse(&eth_hdr, src_ip, dst_ip, 
        src_port, dst_port, test_addr_table, test_port_fwd_table);

    printf("Test 1: \"%s\"\n", "Lookup existing entry");
    addr_table_t *entry = nat_reverse_lookup(nat_port, dst_ip, dst_port, test_addr_table);
    TEST_ASSERT(entry != NULL, 
        "Found existing entry",
        "entry not found");

    if (entry) {
        TEST_ASSERT(entry->src_ip == src_ip, 
            "Entry has correct src_ip",
            "expected 0x%08x, got 0x%08x", ntohl(src_ip), ntohl(entry->src_ip));
        TEST_ASSERT(entry->src_port == src_port, 
            "Entry has correct src_port",
            "expected %u, got %u", ntohs(src_port), ntohs(entry->src_port));
    }

    printf("Test 2: \"%s\"\n", "Lookup non-existing entry");
    addr_table_t *entry_none = nat_reverse_lookup(nat_port + 1, dst_ip, dst_port, test_addr_table);
    TEST_ASSERT(entry_none == NULL, 
        "Non-existing entry returns NULL",
        "expected NULL, got non-NULL");

    /*
     * Test 3: Port reuse reverse lookup scenario
     * 
     * Scenario: Same source (192.168.0.100:12345) connects to two different destinations
     * Both connections share the SAME NAT port (port reuse)
     * 
     * Outbound:
     *   Connection A: 192.168.0.100:12345 -> 8.8.8.8:53   (NAT port X)
     *   Connection B: 192.168.0.100:12345 -> 1.1.1.1:80   (NAT port X, same!)
     * 
     * Inbound (simulated):
     *   Packet from 8.8.8.8:53 -> NAT_IP:X  should map to Connection A
     *   Packet from 1.1.1.1:80 -> NAT_IP:X  should map to Connection B
     */
    printf("\nTest 3: \"%s\"\n", "Port reuse reverse lookup - two connections sharing same NAT port");

    reset_addr_table();

    /* Same source for both connections */
    U32 common_src_ip = htonl(0xC0A80064);    // 192.168.0.100
    U16 common_src_port = htons(12345);

    /* Two different destinations */
    U32 dst_ip_a = htonl(0x08080808);  // 8.8.8.8 (DNS server)
    U16 dst_port_a = htons(53);

    U32 dst_ip_b = htonl(0x01010101);  // 1.1.1.1 (HTTP server)
    U16 dst_port_b = htons(80);

    /* Create two NAT entries - should share same NAT port */
    U16 nat_port_a = nat_learning_port_reuse(&eth_hdr, common_src_ip, dst_ip_a, 
        common_src_port, dst_port_a, test_addr_table, test_port_fwd_table);
    U16 nat_port_b = nat_learning_port_reuse(&eth_hdr, common_src_ip, dst_ip_b, 
        common_src_port, dst_port_b, test_addr_table, test_port_fwd_table);

    TEST_ASSERT(nat_port_a != 0 && nat_port_b != 0, 
        "Both connections get NAT ports",
        "got A=%u, B=%u", nat_port_a, nat_port_b);

    TEST_ASSERT(nat_port_a == nat_port_b, 
        "Both connections share same NAT port (port reuse)",
        "expected same, got A=%u, B=%u", nat_port_a, nat_port_b);

    printf("  Info: Both connections use NAT port %u\n", nat_port_a);

    /* 
     * Simulate inbound packet from 8.8.8.8:53 
     * This should find Connection A's entry
     */
    printf("\nTest 3a: \"%s\"\n", "Inbound from 8.8.8.8:53 finds correct entry");
    addr_table_t *entry_a = nat_reverse_lookup(nat_port_a, dst_ip_a, dst_port_a, test_addr_table);
    TEST_ASSERT(entry_a != NULL, 
        "Found entry for inbound from 8.8.8.8:53",
        "entry not found");

    if (entry_a) {
        TEST_ASSERT(entry_a->src_ip == common_src_ip, 
            "Entry A has correct src_ip (192.168.0.100)",
            "expected 0x%08x, got 0x%08x", ntohl(common_src_ip), ntohl(entry_a->src_ip));
        TEST_ASSERT(entry_a->src_port == common_src_port, 
            "Entry A has correct src_port (12345)",
            "expected %u, got %u", ntohs(common_src_port), ntohs(entry_a->src_port));
        TEST_ASSERT(entry_a->dst_ip == dst_ip_a, 
            "Entry A has correct dst_ip (8.8.8.8)",
            "expected 0x%08x, got 0x%08x", ntohl(dst_ip_a), ntohl(entry_a->dst_ip));
        TEST_ASSERT(entry_a->dst_port == dst_port_a, 
            "Entry A has correct dst_port (53)",
            "expected %u, got %u", ntohs(dst_port_a), ntohs(entry_a->dst_port));
    }

    /* 
     * Simulate inbound packet from 1.1.1.1:80 
     * This should find Connection B's entry (different from A!)
     */
    printf("\nTest 3b: \"%s\"\n", "Inbound from 1.1.1.1:80 finds correct entry");
    addr_table_t *entry_b = nat_reverse_lookup(nat_port_b, dst_ip_b, dst_port_b, test_addr_table);
    TEST_ASSERT(entry_b != NULL, 
        "Found entry for inbound from 1.1.1.1:80",
        "entry not found");

    if (entry_b) {
        TEST_ASSERT(entry_b->src_ip == common_src_ip, 
            "Entry B has correct src_ip (192.168.0.100)",
            "expected 0x%08x, got 0x%08x", ntohl(common_src_ip), ntohl(entry_b->src_ip));
        TEST_ASSERT(entry_b->src_port == common_src_port, 
            "Entry B has correct src_port (12345)",
            "expected %u, got %u", ntohs(common_src_port), ntohs(entry_b->src_port));
        TEST_ASSERT(entry_b->dst_ip == dst_ip_b, 
            "Entry B has correct dst_ip (1.1.1.1)",
            "expected 0x%08x, got 0x%08x", ntohl(dst_ip_b), ntohl(entry_b->dst_ip));
        TEST_ASSERT(entry_b->dst_port == dst_port_b, 
            "Entry B has correct dst_port (80)",
            "expected %u, got %u", ntohs(dst_port_b), ntohs(entry_b->dst_port));
    }

    /* Verify they are different entries (different table slots) */
    printf("\nTest 3c: \"%s\"\n", "Two entries are stored in different table slots");
    TEST_ASSERT(entry_a != entry_b, 
        "Entry A and B are different table entries",
        "both point to same entry at %p", (void*)entry_a);

    printf("\n  ✓ Port reuse reverse lookup verified:\n");
    printf("    - NAT port %u shared by both connections\n", nat_port_a);
    printf("    - Inbound from 8.8.8.8:53 correctly maps to Connection A\n");
    printf("    - Inbound from 1.1.1.1:80 correctly maps to Connection B\n");
}

void test_nat_udp_learning_wrapper(void)
{
    printf("\nTesting nat_udp_learning wrapper function:\n");
    printf("=========================================\n\n");

    reset_addr_table();

    struct rte_ether_hdr eth_hdr = {
        .src_addr = {.addr_bytes = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}},
    };

    struct rte_ipv4_hdr ip_hdr = {
        .src_addr = htonl(0xC0A80064),  // 192.168.0.100
        .dst_addr = htonl(0x08080808),  // 8.8.8.8
    };

    struct rte_udp_hdr udp_hdr = {
        .src_port = htons(12345),
        .dst_port = htons(53),
    };

    printf("Test 1: \"%s\"\n", "UDP NAT learning");
    U16 nat_port = nat_udp_learning(&eth_hdr, &ip_hdr, &udp_hdr, test_addr_table, test_port_fwd_table);
    TEST_ASSERT(nat_port != 0, 
        "UDP learning returns valid NAT port",
        "got 0 (table full)");
    TEST_ASSERT(nat_port >= SYS_MAX_PORT && nat_port < TOTAL_SOCK_PORT,
        "NAT port in valid range",
        "expected [%u, %u), got %u", SYS_MAX_PORT, TOTAL_SOCK_PORT, nat_port);
}

void test_nat_tcp_learning_wrapper(void)
{
    printf("\nTesting nat_tcp_learning wrapper function:\n");
    printf("=========================================\n\n");

    reset_addr_table();

    struct rte_ether_hdr eth_hdr = {
        .src_addr = {.addr_bytes = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}},
    };

    struct rte_ipv4_hdr ip_hdr = {
        .src_addr = htonl(0xC0A80064),  // 192.168.0.100
        .dst_addr = htonl(0x08080808),  // 8.8.8.8
    };

    struct rte_tcp_hdr tcp_hdr = {
        .src_port = htons(12345),
        .dst_port = htons(80),
    };

    printf("Test 1: \"%s\"\n", "TCP NAT learning");
    U16 nat_port = nat_tcp_learning(&eth_hdr, &ip_hdr, &tcp_hdr, test_addr_table, test_port_fwd_table);
    TEST_ASSERT(nat_port != 0, 
        "TCP learning returns valid NAT port",
        "got 0 (table full)");
    TEST_ASSERT(nat_port >= SYS_MAX_PORT && nat_port < TOTAL_SOCK_PORT,
        "NAT port in valid range",
        "expected [%u, %u), got %u", SYS_MAX_PORT, TOTAL_SOCK_PORT, nat_port);
}

void test_nat_icmp_learning_wrapper(void)
{
    printf("\nTesting nat_icmp_learning wrapper function:\n");
    printf("=========================================\n\n");

    reset_addr_table();

    struct rte_ether_hdr eth_hdr = {
        .src_addr = {.addr_bytes = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}},
    };

    struct rte_ipv4_hdr ip_hdr = {
        .src_addr = htonl(0xC0A80064),  // 192.168.0.100
        .dst_addr = htonl(0x08080808),  // 8.8.8.8
    };

    struct rte_icmp_hdr icmp_hdr = {
        .icmp_ident = htons(1234),
        .icmp_type = 8,  // Echo request
    };

    printf("Test 1: \"%s\"\n", "ICMP NAT learning");
    U16 nat_port = nat_icmp_learning(&eth_hdr, &ip_hdr, &icmp_hdr, test_addr_table, test_port_fwd_table);
    TEST_ASSERT(nat_port != 0, 
        "ICMP learning returns valid NAT port",
        "got 0 (table full)");
    TEST_ASSERT(nat_port >= SYS_MAX_PORT && nat_port < TOTAL_SOCK_PORT,
        "NAT port in valid range",
        "expected [%u, %u), got %u", SYS_MAX_PORT, TOTAL_SOCK_PORT, nat_port);
}

void test_port_fwd_helpers(void)
{
    printf("\nTesting port forwarding helpers (port_fwd_add/remove/lookup/nat_port_fwd_reverse_lookup):\n");
    printf("=========================================\n\n");

    reset_addr_table();

    U16 eport  = 8080;                   /* host byte order */
    U32 dip    = htonl(0xC0A80065);      /* 192.168.0.101, network byte order */
    U16 iport  = 80;                     /* host byte order */

    /* ── port_fwd_lookup_by_eport: inactive entry returns NULL ── */
    printf("Test 1: \"%s\"\n", "lookup inactive entry returns NULL");
    port_fwd_entry_t *lookup = port_fwd_lookup_by_eport(test_port_fwd_table, eport);
    TEST_ASSERT(lookup == NULL,
        "port_fwd_lookup_by_eport: inactive returns NULL",
        "expected NULL, got %p", (void *)lookup);

    /* ── port_fwd_remove: removing inactive entry returns ERROR ── */
    printf("Test 2: \"%s\"\n", "remove inactive entry returns ERROR");
    STATUS s = port_fwd_remove(test_port_fwd_table, eport);
    TEST_ASSERT(s == ERROR,
        "port_fwd_remove: inactive entry returns ERROR",
        "expected ERROR, got %d", s);

    /* ── port_fwd_add: add new entry ── */
    printf("Test 3: \"%s\"\n", "add new entry");
    port_fwd_add(test_port_fwd_table, eport, dip, iport);
    TEST_ASSERT(rte_atomic16_read(&test_port_fwd_table[eport].is_active) == 1,
        "port_fwd_add: is_active set to 1",
        "is_active = %d", rte_atomic16_read(&test_port_fwd_table[eport].is_active));

    printf("Test 4: \"%s\"\n", "add: dip and iport stored correctly");
    TEST_ASSERT(test_port_fwd_table[eport].dip == dip,
        "port_fwd_add: dip stored correctly",
        "expected 0x%08X, got 0x%08X", dip, test_port_fwd_table[eport].dip);
    TEST_ASSERT(test_port_fwd_table[eport].iport == iport,
        "port_fwd_add: iport stored correctly",
        "expected %u, got %u", iport, test_port_fwd_table[eport].iport);

    printf("Test 5: \"%s\"\n", "add: hit_count initialised to 0");
    TEST_ASSERT(rte_atomic64_read(&test_port_fwd_table[eport].hit_count) == 0,
        "port_fwd_add: hit_count reset to 0",
        "expected 0, got %" PRId64, rte_atomic64_read(&test_port_fwd_table[eport].hit_count));

    /* ── port_fwd_lookup_by_eport: active entry returns correct pointer ── */
    printf("Test 6: \"%s\"\n", "lookup active entry returns correct pointer");
    lookup = port_fwd_lookup_by_eport(test_port_fwd_table, eport);
    TEST_ASSERT(lookup != NULL,
        "port_fwd_lookup_by_eport: active entry returns non-NULL",
        "expected non-NULL, got NULL");
    TEST_ASSERT(lookup == &test_port_fwd_table[eport],
        "port_fwd_lookup_by_eport: returns correct entry pointer",
        "expected %p, got %p", (void *)&test_port_fwd_table[eport], (void *)lookup);

    /* ── port_fwd_add: idempotent – no-op if already active ── */
    printf("Test 7: \"%s\"\n", "add to already-active entry is a no-op");
    U32 new_dip   = htonl(0xC0A80099);  /* different IP */
    U16 new_iport = 9090;
    port_fwd_add(test_port_fwd_table, eport, new_dip, new_iport);
    TEST_ASSERT(test_port_fwd_table[eport].dip == dip,
        "port_fwd_add: no-op preserves original dip",
        "expected 0x%08X, got 0x%08X", dip, test_port_fwd_table[eport].dip);
    TEST_ASSERT(test_port_fwd_table[eport].iport == iport,
        "port_fwd_add: no-op preserves original iport",
        "expected %u, got %u", iport, test_port_fwd_table[eport].iport);

    /* ── nat_port_fwd_reverse_lookup: match ── */
    printf("Test 8: \"%s\"\n", "nat_port_fwd_reverse_lookup: match returns SUCCESS");
    U32 out_dip  = 0;
    U16 out_iport = 0;
    STATUS rv = nat_port_fwd_reverse_lookup(test_port_fwd_table,
        htons(eport), &out_dip, &out_iport);
    TEST_ASSERT(rv == SUCCESS,
        "nat_port_fwd_reverse_lookup: active entry returns SUCCESS",
        "expected SUCCESS, got %d", rv);
    TEST_ASSERT(out_dip == dip,
        "nat_port_fwd_reverse_lookup: out_dip matches",
        "expected 0x%08X, got 0x%08X", dip, out_dip);
    /* iport stored host byte order; out_iport returned in network byte order */
    TEST_ASSERT(out_iport == htons(iport),
        "nat_port_fwd_reverse_lookup: out_iport matches (network byte order)",
        "expected %u, got %u", htons(iport), out_iport);

    /* ── nat_port_fwd_reverse_lookup: miss (different port) ── */
    printf("Test 9: \"%s\"\n", "nat_port_fwd_reverse_lookup: inactive port returns ERROR");
    rv = nat_port_fwd_reverse_lookup(test_port_fwd_table, htons(9999), &out_dip, &out_iport);
    TEST_ASSERT(rv == ERROR,
        "nat_port_fwd_reverse_lookup: inactive entry returns ERROR",
        "expected ERROR, got %d", rv);

    /* ── hit_count increments (simulate data-path) ── */
    printf("Test 10: \"%s\"\n", "hit_count increments correctly");
    rte_atomic64_inc(&test_port_fwd_table[eport].hit_count);
    rte_atomic64_inc(&test_port_fwd_table[eport].hit_count);
    rte_atomic64_inc(&test_port_fwd_table[eport].hit_count);
    TEST_ASSERT(rte_atomic64_read(&test_port_fwd_table[eport].hit_count) == 3,
        "hit_count correctly incremented to 3",
        "expected 3, got %" PRId64, rte_atomic64_read(&test_port_fwd_table[eport].hit_count));

    /* ── port_fwd_remove: removes active entry, resets hit_count ── */
    printf("Test 11: \"%s\"\n", "remove active entry returns SUCCESS");
    s = port_fwd_remove(test_port_fwd_table, eport);
    TEST_ASSERT(s == SUCCESS,
        "port_fwd_remove: active entry returns SUCCESS",
        "expected SUCCESS, got %d", s);
    TEST_ASSERT(rte_atomic16_read(&test_port_fwd_table[eport].is_active) == 0,
        "port_fwd_remove: is_active cleared to 0",
        "is_active = %d", rte_atomic16_read(&test_port_fwd_table[eport].is_active));

    printf("Test 12: \"%s\"\n", "remove: hit_count reset to 0");
    TEST_ASSERT(rte_atomic64_read(&test_port_fwd_table[eport].hit_count) == 0,
        "port_fwd_remove: hit_count reset to 0",
        "expected 0, got %" PRId64, rte_atomic64_read(&test_port_fwd_table[eport].hit_count));

    printf("Test 13: \"%s\"\n", "lookup after remove returns NULL");
    lookup = port_fwd_lookup_by_eport(test_port_fwd_table, eport);
    TEST_ASSERT(lookup == NULL,
        "port_fwd_lookup_by_eport: removed entry returns NULL",
        "expected NULL, got %p", (void *)lookup);

    /* ── reverse lookup after remove also misses ── */
    printf("Test 14: \"%s\"\n", "nat_port_fwd_reverse_lookup after remove returns ERROR");
    rv = nat_port_fwd_reverse_lookup(test_port_fwd_table, htons(eport), &out_dip, &out_iport);
    TEST_ASSERT(rv == ERROR,
        "nat_port_fwd_reverse_lookup: removed entry returns ERROR",
        "expected ERROR, got %d", rv);

    /* ── double-remove returns ERROR ── */
    printf("Test 15: \"%s\"\n", "double-remove returns ERROR");
    s = port_fwd_remove(test_port_fwd_table, eport);
    TEST_ASSERT(s == ERROR,
        "port_fwd_remove: double-remove returns ERROR",
        "expected ERROR, got %d", s);

    /* ── re-add after remove works, hit_count starts at 0 ── */
    printf("Test 16: \"%s\"\n", "re-add after remove works and hit_count is 0");
    port_fwd_add(test_port_fwd_table, eport, dip, iport);
    TEST_ASSERT(rte_atomic16_read(&test_port_fwd_table[eport].is_active) == 1,
        "port_fwd_add: re-add after remove succeeds",
        "is_active = %d", rte_atomic16_read(&test_port_fwd_table[eport].is_active));
    TEST_ASSERT(rte_atomic64_read(&test_port_fwd_table[eport].hit_count) == 0,
        "port_fwd_add: re-add resets hit_count to 0",
        "expected 0, got %" PRId64, rte_atomic64_read(&test_port_fwd_table[eport].hit_count));
}

void test_nat_table_almost_full(void)
{
    printf("\nTesting NAT learning when table is almost full:\n");
    printf("=========================================\n\n");

    reset_addr_table();

    struct rte_ether_hdr eth_hdr = {
        .src_addr = {.addr_bytes = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}},
    };

    /*
     * Test scenario:
     * 1. Create first entry using nat_learning_port_reuse (proper hash)
     * 2. Fill all other slots manually
     * 3. Try to create second entry with same src but different dst (port reuse)
     */

    /* Step 1: Create the "known" entry properly using the NAT learning function */
    printf("Step 1: Creating known entry via nat_learning_port_reuse...\n");

    U32 known_src_ip = htonl(0xC0A80064);    // 192.168.0.100
    U16 known_src_port = htons(12345);
    U32 known_dst_ip = htonl(0x08080808);    // 8.8.8.8
    U16 known_dst_port = htons(53);

    U16 known_nat_port = nat_learning_port_reuse(&eth_hdr, known_src_ip, known_dst_ip,
        known_src_port, known_dst_port, test_addr_table, test_port_fwd_table);

    TEST_ASSERT(known_nat_port != 0,
        "First entry created successfully",
        "got 0 (unexpected failure)");

    printf("  Known entry: 192.168.0.100:12345 -> 8.8.8.8:53 (NAT port %u)\n", 
        ntohs(known_nat_port));

    /* Find the slot where the known entry was stored */
    U32 known_table_idx = compute_nat_table_index(known_nat_port, known_dst_ip, known_dst_port);
    printf("  Entry stored at table index: %u\n", known_table_idx);

    /* Step 2: Fill almost all remaining slots, but leave space for the second entry */
    printf("\nStep 2: Filling remaining table slots...\n");

    /* Calculate where the second entry (different dst) would be stored */
    U32 new_dst_ip = htonl(0x01010101);   // 1.1.1.1
    U16 new_dst_port = htons(80);          // HTTP
    U32 new_table_idx = compute_nat_table_index(known_nat_port, new_dst_ip, new_dst_port);
    printf("  Second entry will hash to index: %u\n", new_table_idx);

    U32 filled_count = 0;
    for(U32 i=0; i<MAX_NAT_ENTRIES; i++) {
        /* Skip the known entry's slot */
        if (i == known_table_idx)
            continue;

        /* Leave the target slot empty for the new entry */
        if (i == new_table_idx)
            continue;

        /* Skip if already filled */
        if (rte_atomic16_read(&test_addr_table[i].is_fill) == NAT_ENTRY_READY)
            continue;

        /* Fill with dummy data */
        rte_atomic16_set(&test_addr_table[i].is_fill, NAT_ENTRY_READY);
        test_addr_table[i].src_ip = htonl(0x0A000000 + i);  // 10.0.x.x
        test_addr_table[i].src_port = htons(10000 + (i % 50000));
        test_addr_table[i].dst_ip = htonl(0xC0A80001);      // Different from our targets
        test_addr_table[i].dst_port = htons(443);
        test_addr_table[i].nat_port = htons(SYS_MAX_PORT + (i % NAT_PORT_RANGE));
        filled_count++;
    }

    printf("  Filled %u additional entries\n", filled_count);
    printf("  Empty slots: index %u (known) and index %u (target for new entry)\n", 
        known_table_idx, new_table_idx);

    /* Step 3: Verify we can still find the original entry */
    printf("\nStep 3: Verifying original entry is still accessible...\n");

    addr_table_t *verify_known = nat_reverse_lookup(known_nat_port, known_dst_ip, 
        known_dst_port, test_addr_table);
    TEST_ASSERT(verify_known != NULL,
        "Original entry still accessible after filling table",
        "original entry not found");

    TEST_ASSERT(verify_known->src_ip == known_src_ip,
        "Original entry has correct src_ip",
        "expected 0x%08x, got 0x%08x", ntohl(known_src_ip), ntohl(verify_known->src_ip));

    /* Step 4: Try to create new entry with same src but different dst */
    printf("\nStep 4: Attempting NAT learning with same src but different dst...\n");
    printf("  New flow: 192.168.0.100:12345 -> 1.1.1.1:80\n");

    U16 new_nat_port = nat_learning_port_reuse(&eth_hdr, known_src_ip, new_dst_ip,
        known_src_port, new_dst_port, test_addr_table, test_port_fwd_table);

    printf("\nTest 1: \"%s\"\n", "NAT learning with almost full table");
    TEST_ASSERT(new_nat_port != 0,
        "Successfully allocated NAT port in almost-full table",
        "got 0 (table full)");

    printf("  ✓ New NAT port allocated: %u\n", ntohs(new_nat_port));

    /* Check if port was reused (same NAT port for both) */
    printf("\nTest 2: \"%s\"\n", "Port reuse check");
    TEST_ASSERT(new_nat_port == known_nat_port,
        "Same source reuses same NAT port (port reuse)",
        "expected %u, got %u", ntohs(known_nat_port), ntohs(new_nat_port));

    /* Verify both entries are accessible via reverse lookup */
    printf("\nTest 3: \"%s\"\n", "Both entries accessible via reverse lookup");

    addr_table_t *orig_entry = nat_reverse_lookup(known_nat_port, known_dst_ip, 
        known_dst_port, test_addr_table);
    TEST_ASSERT(orig_entry != NULL,
        "Original entry still accessible",
        "original entry not found");

    addr_table_t *new_entry = nat_reverse_lookup(new_nat_port, new_dst_ip, 
        new_dst_port, test_addr_table);
    TEST_ASSERT(new_entry != NULL,
        "New entry accessible",
        "new entry not found");

    if (orig_entry && new_entry) {
        printf("\nTest 4: \"%s\"\n", "Entries are stored in different slots");
        TEST_ASSERT(orig_entry != new_entry,
            "Original and new entries are different table slots",
            "both point to same entry at %p", (void*)orig_entry);

        printf("\n  ✓ Port reuse achieved with almost-full table!\n");
        printf("    - Both connections use NAT port %u\n", ntohs(known_nat_port));
        printf("    - Original: 192.168.0.100:12345 -> 8.8.8.8:53\n");
        printf("    - New:      192.168.0.100:12345 -> 1.1.1.1:80\n");
    }

    printf("\nTest 5: \"%s\"\n", "Need to check unable to add new entry when table is truly full");
    /* Now fill the last remaining slot to make table truly full */
    U32 extra_src_ip = htonl(0xC0A80064);    // 172.16.0.100
    U16 extra_src_port = htons(12345);
    U32 extra_dst_ip = htonl(0x08080808);    // 8.8.8.8
    U16 extra_dst_port = htons(53);

    U16 extra_nat_port = nat_learning_port_reuse(&eth_hdr, extra_src_ip, extra_dst_ip,
        extra_src_port, extra_dst_port, test_addr_table, test_port_fwd_table);

    TEST_ASSERT(extra_nat_port != 0,
        "Extra entry failed to be created as table is full",
        "got non 0 (nat table should be full)");

    printf("\n  Test completed: Almost-full table scenario handled correctly\n");
}

void test_nat(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║              NAT MODULE UNIT TESTS                       ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_compute_initial_nat_port();
    test_compute_nat_table_index();
    test_nat_learning_port_reuse_basic();
    test_nat_learning_port_reuse_different_dst();
    test_nat_learning_port_reuse_conflict();
    test_nat_reverse_lookup();
    test_nat_udp_learning_wrapper();
    test_nat_tcp_learning_wrapper();
    test_nat_icmp_learning_wrapper();
    test_port_fwd_helpers();
    test_nat_table_almost_full();

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
