#include <stdlib.h>
#include <assert.h>

#include <common.h>

#include <rte_atomic.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "../../src/fastrg.h"
#include "../../src/init.h"
#include "../../src/pppd/codec.h"
#include "../../src/protocol.h"
#include "../test_helper.h"

// Global test counters
static int test_count = 0;
static int pass_count = 0;

void test_build_padi(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_padi function:\n");
    printf("=========================================\n\n");

    U8 buffer[80] = { 0 };
    U16 mulen;

    ppp_ccb_t s_ppp_ccb_1 = {
        .pppoe_phase = {
            .timer_counter = 0,
            .max_retransmit = 10,
        },
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .fastrg_ccb = fastrg_ccb,
    };
    U8 pkt_1[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x9c, 0x69, 0xb4, 0x61, 
    0x16, 0xdd, 0x81, 0x00, 0x00, 0x02, 0x88, 0x63, 0x11, 0x09, 0x00, 0x00, 0x00, 
    0x04, 0x01, 0x01, 0x00, 0x00};

    printf("Test 1: \"%s\"\n", "build_padi() result");
    TEST_ASSERT(build_padi(buffer, &mulen, &s_ppp_ccb_1) == SUCCESS, 
        "build_padi success", "build_padi failed");
    TEST_ASSERT(mulen == sizeof(pkt_1), "build_padi packet length", 
        "expected length %zu, got %u", sizeof(pkt_1), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_1, mulen) == 0, "build_padi packet content", 
        "packet content mismatch");

    // TODO: move timeout test to pppoe_send_pkt()
    /*memset(buffer, 0, sizeof(buffer));
    s_ppp_ccb_1.pppoe_phase.timer_counter = 10;
    assert(build_padi(buffer, &mulen, &s_ppp_ccb_1) == ERROR);*/
}

void test_build_padr(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_padr function:\n");
    printf("=========================================\n\n");

    U8 buffer[80] = { 0 };
    U16 mulen = 0;
    struct rte_ether_hdr eth_hdr = {
        .ether_type = htons(VLAN),
    };
    vlan_header_t vlan_header = {
        .tci_union.tci_value = htons(0002),
        .next_proto = htons(ETH_P_PPP_DIS),
    };
    pppoe_header_t pppoe_header = {
        .code = PADO,
        .ver_type = 0x11,
        .session_id = htons(0x000a),
        .length = htons(0x002c),
    };

    ppp_ccb_t s_ppp_ccb_1 = {
        .pppoe_phase = {
            .timer_counter = 0,
            .max_retransmit = 10,
        },
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .eth_hdr = eth_hdr,
        .vlan_header = vlan_header,
        .pppoe_header = pppoe_header,
        .fastrg_ccb = fastrg_ccb,
    };
    U8 pppoe_header_tag[44] = {0x01, 0x03, 0x00, 0x04, 0xdb, 0xce, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x08, 0x4d, 0x69, 0x6b, 0x72, 
    0x6f, 0x54, 0x69, 0x6b, 0x01, 0x01, 0x00, 0x10, 0x76, 0x72, 0x67, 0x5f, 
    0x73, 0x65, 0x72, 0x76, 0x65, 0x72};
    s_ppp_ccb_1.pppoe_phase.pppoe_header_tag = (pppoe_header_tag_t *)pppoe_header_tag;
    char pkt_1[] = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 0x61, 
    0x16, 0xdd, 0x81, 0x00, 0x00, 0x02, 0x88, 0x63, 0x11, 0x19, 0x00, 0x0a, 0x00, 
    0x0c, 0x01, 0x01, 0x00, 0x00, 0x01, 0x03, 0x00, 0x04, 0xdb, 0xce, 0x00, 0x00};


    printf("Test 1: \"%s\"\n", "build_padr() result");
    TEST_ASSERT(build_padr(buffer, &mulen, &s_ppp_ccb_1) == SUCCESS, 
        "build_padr success", "build_padr failed");
    TEST_ASSERT(mulen == sizeof(pkt_1), "build_padr packet length", 
        "expected length %zu, got %u", sizeof(pkt_1), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_1, mulen) == 0, "build_padr packet content", 
        "packet content mismatch");

    // TODO: move timeout test to pppoe_send_pkt()
    /*memset(buffer, 0, sizeof(buffer));
    s_ppp_ccb_1.pppoe_phase.timer_counter = 10;
    assert(build_padr(buffer, &mulen, &s_ppp_ccb_1) == ERROR);*/
}

void test_build_padt(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_padt function:\n");
    printf("=========================================\n\n");

    U8 buffer[80] = { 0 };
    U16 mulen = 0;
    struct rte_ether_hdr eth_hdr = {
        .ether_type = htons(VLAN),
    };
    vlan_header_t vlan_header = {
        .tci_union.tci_value = htons(0002),
        .next_proto = htons(ETH_P_PPP_DIS),
    };
    pppoe_header_t pppoe_header = {
        .code = PADS,
        .ver_type = 0x11,
        .session_id = htons(0x000a),
        .length = htons(0x002c),
    };

    ppp_ccb_t s_ppp_ccb_1 = {
        .pppoe_phase = {
            .timer_counter = 0,
            .max_retransmit = 10,
        },
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id = htons(0x000a),
        .eth_hdr = eth_hdr,
        .vlan_header = vlan_header,
        .pppoe_header = pppoe_header,
        .fastrg_ccb = fastrg_ccb,
    };
    char pkt_1[] = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 0x61, 
    0x16, 0xdd, 0x81, 0x00, 0x00, 0x02, 0x88, 0x63, 0x11, 0xa7, 0x00, 0x0a, 0x00, 
    0x00};

    printf("Test 1: \"%s\"\n", "build_padt() result");
    build_padt(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_1), "build_padt packet length", 
        "expected length %zu, got %u", sizeof(pkt_1), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_1, mulen) == 0, "build_padt packet content", 
        "packet content mismatch");
}

void test_build_config_request(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_config_request function:\n");
    printf("=========================================\n\n");

    U8 buffer[80] = { 0 };
    U16 mulen = 0;

    ppp_ccb_t s_ppp_ccb[] = {
        {
            .ppp_phase = {{
                .timer_counter = 0,
                .max_retransmit = 10,
            },{
                .timer_counter = 0,
                .max_retransmit = 10,
            },},
            .user_num = 1,
            .vlan_id = {
                .cnt = 2,
            },
            .PPP_dst_mac = (struct rte_ether_addr){
                .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
            },
            .session_id = htons(0x000a),
            .cp = 0,
            .magic_num = htonl(0x01020304),
            .identifier = 0xfd,
            .hsi_ipv4 = 0x0,
            .fastrg_ccb = fastrg_ccb,
        },
    };

    U8 pkt_lcp[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x10, /* ppp protocol */0xc0, 0x21, /* ppp hdr */
    0x01, 0xfe, 0x00, 0x0e, /* ppp option */0x01, 0x04, 0x05, 0xd0, 0x05, 0x06, 0x01, 
    0x02, 0x03, 0x04};
    U8 pkt_ipcp_1[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x0c, /* ppp protocol */0x80, 0x21, /* ppp hdr */
    0x01, 0xff, 0x00, 0x0a, /* ppp option */0x03, 0x06, 0x00, 0x00, 0x00, 0x00};
    U8 pkt_ipcp_2[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x0c, /* ppp protocol */0x80, 0x21, /* ppp hdr */
    0x01, 0x01, 0x00, 0x0a, /* ppp option */0x03, 0x06, 0xc0, 0xa8, 0xc8, 0x01};

    for(int i=0; i<sizeof(s_ppp_ccb)/sizeof(s_ppp_ccb[0]); i++) {
        /* test LCP */
        printf("Test %d: \"%s\"\n", i * 3 + 1, "build_config_request() LCP result");
        build_config_request(buffer, &mulen, &s_ppp_ccb[0]);
        TEST_ASSERT(mulen == sizeof(pkt_lcp), "build_config_request packet length", 
            "expected length %zu, got %u", sizeof(pkt_lcp), mulen);
        TEST_ASSERT(memcmp(buffer, pkt_lcp, sizeof(pkt_lcp)) == 0, "build_config_request packet content", 
            "packet content mismatch");
        /* test IPCP */
        printf("Test %d: \"%s\"\n", i * 3 + 2, "build_config_request() IPCP result");
        s_ppp_ccb[0].cp = 1;
        memset(buffer, 0, sizeof(buffer));
        build_config_request(buffer, &mulen, &s_ppp_ccb[0]);
        TEST_ASSERT(mulen == sizeof(pkt_ipcp_1), "build_config_request packet length", 
            "expected length %zu, got %u", sizeof(pkt_ipcp_1), mulen);
        TEST_ASSERT(memcmp(buffer, pkt_ipcp_1, sizeof(pkt_ipcp_1)) == 0, "build_config_request packet content", 
            "packet content mismatch");
        s_ppp_ccb[0].hsi_ipv4 = htonl(0xc0a8c801);
        memset(buffer, 0, sizeof(buffer));

        printf("Test %d: \"%s\"\n", i * 3 + 3, "build_config_request() IPCP result with IP");
        build_config_request(buffer, &mulen, &s_ppp_ccb[0]);
        TEST_ASSERT(mulen == sizeof(pkt_ipcp_2), "build_config_request packet length", 
            "expected length %zu, got %u", sizeof(pkt_ipcp_2), mulen);
        TEST_ASSERT(memcmp(buffer, pkt_ipcp_2, sizeof(pkt_ipcp_2)) == 0, "build_config_request packet content", 
            "packet content mismatch");
    }
}

void test_build_config_ack(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_config_ack function:\n");
    printf("=========================================\n\n");

    U8 buffer[80] = { 0 };
    U16 mulen = 0;

    ppp_ccb_t s_ppp_ccb_1 = {
        .ppp_phase = {{
            .timer_counter = 0,
            .max_retransmit = 10,
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(LCP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = CONFIG_REQUEST,
                .identifier = 0x01,
                .length = htons(0x0012),
            },
            .ppp_options = (ppp_options_t *)(U8 []){
                0x03, 0x04, 0xc0, 0x23, 0x01, 0x04, 0x05, 0xd0, 0x05, 0x06, 0x01, 0x02, 0x03, 0x04
            }, // MRU, AUTH, MAGIC NUMBER
        },{
            .timer_counter = 0,
            .max_retransmit = 10,
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(IPCP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = CONFIG_REQUEST,
                .identifier = 0x01,
                .length = htons(0x000a),
            },
            .ppp_options = (ppp_options_t *)(U8 []){
                0x03, 0x06, 0xc0, 0xa8, 0xc8, 0x01
            }, // IP_ADDRESS
        },},
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id = htons(0x000a),
        .cp = 0,
        .eth_hdr = (struct rte_ether_hdr) {
            .ether_type = htons(VLAN),
        },
        .vlan_header = (vlan_header_t) {
            .tci_union.tci_value = htons(0002),
            .next_proto = htons(ETH_P_PPP_SES),
        },
        .pppoe_header = (pppoe_header_t) {
            .code = 0,
            .ver_type = 0x11,
            .session_id = htons(0x000a),
            .length = htons(0x0014),
        },
        .fastrg_ccb = fastrg_ccb,
    };

    char pkt_lcp[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x14, /* ppp protocol */0xc0, 0x21, /* ppp hdr*/
    0x02, 0x01, 0x00, 0x12, /* ppp option */0x03, 0x04, 0xc0, 0x23, 0x01, 0x04, 0x05, 
    0xd0, 0x05, 0x06, 0x01, 0x02, 0x03, 0x04};
    char pkt_ipcp[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x0c, /* ppp protocol */0x80, 0x21, /* ppp hdr*/
    0x02, 0x01, 0x00, 0x0a, /* ppp option */0x03, 0x06, 0xc0, 0xa8, 0xc8, 0x01};

    /* test LCP */
    printf("Test 1: \"%s\"\n", "build_config_ack() LCP result");
    build_config_ack(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_lcp), "build_config_ack packet length", 
        "expected length %zu, got %u", sizeof(pkt_lcp), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_lcp, sizeof(pkt_lcp)) == 0, "build_config_ack packet content", 
        "packet content mismatch");

    memset(buffer, 0, sizeof(buffer));
    /* test IPCP */
    printf("Test 2: \"%s\"\n", "build_config_ack() IPCP result");
    s_ppp_ccb_1.cp = 1;
    s_ppp_ccb_1.pppoe_header.length = htons(0x000c);
    build_config_ack(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_ipcp), "build_config_ack packet length", 
        "expected length %zu, got %u", sizeof(pkt_ipcp), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_ipcp, sizeof(pkt_ipcp)) == 0, "build_config_ack packet content", 
        "packet content mismatch");
}

void test_build_terminate_request(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_terminate_request function:\n");
    printf("=========================================\n\n");
    U8 buffer[80] = { 0 };
    U16 mulen = 0;

    ppp_ccb_t s_ppp_ccb_1 = {
        .ppp_phase = {{
            .timer_counter = 0,
            .max_retransmit = 10,
        },{
            .timer_counter = 0,
            .max_retransmit = 10,
        },},
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id = htons(0x000a),
        .cp = 0,
        .fastrg_ccb = fastrg_ccb,
    };

    char pkt_lcp[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x06, /* ppp protocol */0xc0, 0x21, /* ppp hdr*/
    0x05, 0x03, 0x00, 0x04};
    char pkt_ipcp[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x06, /* ppp protocol */0x80, 0x21, /* ppp hdr*/
    0x05, 0x03, 0x00, 0x04};

    /* test LCP */
    printf("Test 1: \"%s\"\n", "build_terminate_request() LCP result");
    build_terminate_request(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_lcp), "build_terminate_request packet length", 
        "expected length %zu, got %u", sizeof(pkt_lcp), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_lcp, 26/* only memcmp to ipcp field */) == 0, 
        "build_terminate_request packet content", "packet content mismatch");
    ppp_header_t *test_ppp_hdr = (ppp_header_t *)(buffer + 26);
    TEST_ASSERT(test_ppp_hdr->code == TERMIN_REQUEST, "build_terminate_request packet code", 
        "expected code %u, got %u", TERMIN_REQUEST, test_ppp_hdr->code);
    TEST_ASSERT(test_ppp_hdr->length == htons(0x0004), "build_terminate_request packet length", 
        "expected length %u, got %u", htons(0x0004), test_ppp_hdr->length);

    /* test IPCP */
    printf("Test 2: \"%s\"\n", "build_terminate_request() IPCP result");
    memset(buffer, 0, sizeof(buffer));
    s_ppp_ccb_1.cp = 1;
    build_terminate_request(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_ipcp), "build_terminate_request packet length", 
        "expected length %zu, got %u", sizeof(pkt_ipcp), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_ipcp, 26/* only memcmp to ipcp field */) == 0, 
        "build_terminate_request packet content", "packet content mismatch");
    test_ppp_hdr = (ppp_header_t *)(buffer + 26);
    TEST_ASSERT(test_ppp_hdr->code == TERMIN_REQUEST, "build_terminate_request packet code", 
        "expected code %u, got %u", TERMIN_REQUEST, test_ppp_hdr->code);
    TEST_ASSERT(test_ppp_hdr->length == htons(0x0004), "build_terminate_request packet length", 
        "expected length %u, got %u", htons(0x0004), test_ppp_hdr->length);
}

void test_build_config_nak_rej(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_config_nak_rej function:\n");
    printf("=========================================\n\n");

    U8 buffer[80] = {0};
    U16 mulen = 0;

    ppp_ccb_t s_ppp_ccb_1 = {
        .ppp_phase = {{
            .timer_counter = 0,
            .max_retransmit = 10,
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(LCP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = CONFIG_REJECT,
                .identifier = 0x01,
                .length = htons(0x0008),
            },
            .ppp_options = (ppp_options_t *)(U8 []){
                0x03, 0x04, 0xc0, 0x23
            }, // CHAP
        },{
            .timer_counter = 0,
            .max_retransmit = 10,
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(IPCP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = CONFIG_REJECT,
                .identifier = 0x01,
                .length = htons(0x000a),
            },
            .ppp_options = (ppp_options_t *)(U8 []){
                0x83, 0x06, 0x00, 0x00, 0x00, 0x00
            }, // Secondary DNS
        },},
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id = htons(0x000a),
        .cp = 0,
        .eth_hdr = (struct rte_ether_hdr) {
            .ether_type = htons(VLAN),
        },
        .vlan_header = (vlan_header_t) {
            .tci_union.tci_value = htons(0002),
            .next_proto = htons(ETH_P_PPP_SES),
        },
        .pppoe_header = (pppoe_header_t) {
            .code = 0,
            .ver_type = 0x11,
            .session_id = htons(0x000a),
            .length = htons(0x000a),
        },
        .fastrg_ccb = fastrg_ccb,
    };
    ppp_ccb_t s_ppp_ccb_2 = {
        .ppp_phase = {{
            .timer_counter = 0,
            .max_retransmit = 10,
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(LCP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = CONFIG_NAK,
                .identifier = 0x01,
                .length = htons(0x0008),
            },
            .ppp_options = (ppp_options_t *)(U8 []){
                0x01, 0x04, 0x05, 0xd0
            }, // MRU
        },{
            .timer_counter = 0,
            .max_retransmit = 10,
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(IPCP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = CONFIG_NAK,
                .identifier = 0x01,
                .length = htons(0x0010),
            },
            .ppp_options = (ppp_options_t *)(U8 []){
                0xc0, 0xa8, 0xc8, 0xfe, 0x81, 0x06, 0xc0, 0xa8, 0x0a, 0x01
            }, // IP and Primary DNS
        },},
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id = htons(0x000a),
        .cp = 0,
        .eth_hdr = (struct rte_ether_hdr) {
            .ether_type = htons(VLAN),
        },
        .vlan_header = (vlan_header_t) {
            .tci_union.tci_value = htons(0002),
            .next_proto = htons(ETH_P_PPP_SES),
        },
        .pppoe_header = (pppoe_header_t) {
            .code = 0,
            .ver_type = 0x11,
            .session_id = htons(0x000a),
            .length = htons(0x000a),
        },
        .fastrg_ccb = fastrg_ccb,
    };

    char pkt_lcp_1[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x0a, /* ppp protocol */0xc0, 0x21, /* ppp hdr*/
    0x04, 0x01, 0x00, 0x08, /* ppp option */0x03, 0x04, 0xc0, 0x23};
    char pkt_ipcp_1[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x0c, /* ppp protocol */0x80, 0x21, /* ppp hdr*/
    0x04, 0x01, 0x00, 0x0a, /* ppp option */0x83, 0x06, 0x00, 0x00, 0x00, 0x00};
    char pkt_lcp_2[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x0a, /* ppp protocol */0xc0, 0x21, /* ppp hdr*/
    0x03, 0x01, 0x00, 0x08, /* ppp option */0x01, 0x04, 0x05, 0xd0};
    char pkt_ipcp_2[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x10, /* ppp protocol */0x80, 0x21, /* ppp hdr*/
    0x03, 0x01, 0x00, 0x10, /* ppp option */0xc0, 0xa8, 0xc8, 0xfe, 0x81, 0x06, 0xc0, 
    0xa8, 0x0a, 0x01};

    /* test LCP */
    printf("Test 1: \"%s\"\n", "build_config_nak_rej() LCP result");
    build_config_nak_rej(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_lcp_1), "build_config_nak_rej packet length", 
        "expected length %zu, got %u", sizeof(pkt_lcp_1), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_lcp_1, sizeof(pkt_lcp_1)) == 0, 
        "build_config_nak_rej packet content", "packet content mismatch");

    /* test IPCP */
    printf("Test 2: \"%s\"\n", "build_config_nak_rej() IPCP result");
    s_ppp_ccb_1.cp = 1;
    s_ppp_ccb_1.pppoe_header.length = htons(0x000c);
    memset(buffer, 0, sizeof(buffer));
    build_config_nak_rej(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_ipcp_1), "build_config_nak_rej packet length", 
        "expected length %zu, got %u", sizeof(pkt_ipcp_1), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_ipcp_1, sizeof(pkt_ipcp_1)) == 0, 
        "build_config_nak_rej packet content", "packet content mismatch");

    s_ppp_ccb_2.cp = 0;
    printf("Test 3: \"%s\"\n", "build_config_nak_rej() LCP result");
    memset(buffer, 0, sizeof(buffer));
    build_config_nak_rej(buffer, &mulen, &s_ppp_ccb_2);
    TEST_ASSERT(mulen == sizeof(pkt_lcp_2), "build_config_nak_rej packet length", 
        "expected length %zu, got %u", sizeof(pkt_lcp_2), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_lcp_2, sizeof(pkt_lcp_2)) == 0, 
        "build_config_nak_rej packet content", "packet content mismatch");

    printf("Test 4: \"%s\"\n", "build_config_nak_rej() IPCP result");
    s_ppp_ccb_2.cp = 1;
    s_ppp_ccb_2.pppoe_header.length = htons(0x0010);
    memset(buffer, 0, sizeof(buffer));
    build_config_nak_rej(buffer, &mulen, &s_ppp_ccb_2);
    TEST_ASSERT(mulen == sizeof(pkt_ipcp_2), "build_config_nak_rej packet length", 
        "expected length %zu, got %u", sizeof(pkt_ipcp_2), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_ipcp_2, sizeof(pkt_ipcp_2)) == 0, 
        "build_config_nak_rej packet content", "packet content mismatch");
}

void test_build_terminate_ack(FastRG_t *fastrg_ccb) 
{
    printf("\nTesting build_terminate_ack function:\n");
    printf("=========================================\n\n");

    U8 buffer[80] = { 0 };
    U16 mulen = 0;

    ppp_ccb_t s_ppp_ccb_1 = {
        .ppp_phase = {{
            .timer_counter = 0,
            .max_retransmit = 10,
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(LCP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = TERMIN_REQUEST,
                .identifier = 0x01,
                .length = htons(0x0012),
            },
            /* this field is not used, we juts leave this here to make sure it won't
            be inserted into terminate ack packet */
            .ppp_options = (ppp_options_t *)(U8 []){
                0x03, 0x04, 0xc0, 0x23, 0x01, 0x04, 0x05, 0xd0, 0x05, 0x06, 0x01, 0x02, 0x03, 0x04
            }, // MRU, AUTH, MAGIC NUMBER
        },{
            .timer_counter = 0,
            .max_retransmit = 10,
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(IPCP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = CONFIG_REQUEST,
                .identifier = 0x01,
                .length = htons(0x000a),
            },
            /* this field is not used, we juts leave this here to make sure it won't
            be inserted into terminate ack packet */
            .ppp_options = (ppp_options_t *)(U8 []){
                0x03, 0x06, 0xc0, 0xa8, 0xc8, 0x01
            }, // IP_ADDRESS
        },},
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id = htons(0x000a),
        .cp = 0,
        .eth_hdr = (struct rte_ether_hdr) {
            .ether_type = htons(VLAN),
        },
        .vlan_header = (vlan_header_t) {
            .tci_union.tci_value = htons(0002),
            .next_proto = htons(ETH_P_PPP_SES),
        },
        .pppoe_header = (pppoe_header_t) {
            .code = 0,
            .ver_type = 0x11,
            .session_id = htons(0x000a),
            .length = htons(0x0014),
        },
        .fastrg_ccb = fastrg_ccb,
    };

    char pkt_lcp[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x06, /* ppp protocol */0xc0, 0x21, /* ppp hdr*/
    0x06, 0x01, 0x00, 0x04};
    char pkt_ipcp[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x06, /* ppp protocol */0x80, 0x21, /* ppp hdr*/
    0x06, 0x01, 0x00, 0x04};

    /* test LCP */
    printf("Test 1: \"%s\"\n", "build_terminate_ack() LCP result");
    build_terminate_ack(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_lcp), "build_terminate_ack packet length", 
        "expected length %zu, got %u", sizeof(pkt_lcp), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_lcp, sizeof(pkt_lcp)) == 0, 
        "build_terminate_ack packet content", "packet content mismatch");

    memset(buffer, 0, sizeof(buffer));
    /* test IPCP */
    printf("Test 2: \"%s\"\n", "build_terminate_ack() IPCP result");
    s_ppp_ccb_1.cp = 1;
    build_terminate_ack(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_ipcp), "build_terminate_ack packet length", 
        "expected length %zu, got %u", sizeof(pkt_ipcp), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_ipcp, sizeof(pkt_ipcp)) == 0, 
        "build_terminate_ack packet content", "packet content mismatch");
}

void test_build_echo_reply(FastRG_t *fastrg_ccb) 
{
    printf("\nTesting build_echo_reply function:\n");

    U8 buffer[80] = { 0 };
    U16 mulen = 0;

    ppp_ccb_t s_ppp_ccb_1 = {
        .ppp_phase = {{
            .timer_counter = 0,
            .max_retransmit = 10,
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(LCP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = CONFIG_REQUEST,
                .identifier = 0x01,
                .length = htons(0x0008),
            },
            .ppp_options = (ppp_options_t *)(U8 []){
                0x05, 0x06, 0x07, 0x08
            }, // echo requester's magic number
        },{},},
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id = htons(0x000a),
        .cp = 0,
        .magic_num = htonl(0x01020304),
        .eth_hdr = (struct rte_ether_hdr) {
            .ether_type = htons(VLAN),
        },
        .vlan_header = (vlan_header_t) {
            .tci_union.tci_value = htons(0002),
            .next_proto = htons(ETH_P_PPP_SES),
        },
        .pppoe_header = (pppoe_header_t) {
            .code = 0,
            .ver_type = 0x11,
            .session_id = htons(0x000a),
            .length = htons(0x0014),
        },
        .fastrg_ccb = fastrg_ccb,
    };

    char pkt_lcp_1[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x0a, /* ppp protocol */0xc0, 0x21, /* ppp hdr*/
    0x0a, 0x01, 0x00, 0x08, /* magic number */0x01, 0x02, 0x03, 0x04};

    printf("Test 1: \"%s\"\n", "build_echo_reply() LCP result");
    build_echo_reply(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_lcp_1), "build_echo_reply packet length", 
        "expected length %zu, got %u", sizeof(pkt_lcp_1), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_lcp_1, sizeof(pkt_lcp_1)) == 0, 
        "build_echo_reply packet content", "packet content mismatch");

    char pkt_lcp_2[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x0e, /* ppp protocol */0xc0, 0x21, /* ppp hdr*/
    0x0a, 0x01, 0x00, 0x0c, /* magic number */0x01, 0x02, 0x03, 0x04, /* echo 
    requester's magic number */0x05, 0x06, 0x07, 0x08};
    s_ppp_ccb_1.ppp_phase[0].ppp_hdr.length = htons(ntohs(s_ppp_ccb_1.ppp_phase[0].ppp_hdr.length) + 4);

    printf("Test 2: \"%s\"\n", "build_echo_reply() LCP result with extra data");
    build_echo_reply(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_lcp_2), "build_echo_reply packet length", 
        "expected length %zu, got %u", sizeof(pkt_lcp_2), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_lcp_2, sizeof(pkt_lcp_2)) == 0, 
        "build_echo_reply packet content", "packet content mismatch");
}

void test_build_echo_request(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_echo_request function:\n");

    U8 buffer[80] = { 0 };
    U16 mulen = 0;

    /* ppp_phase[0] (the "LCP slot") is deliberately left as PAP (0xc023): that is
     * the stale value the slot holds after authentication. The original bug was
     * build_echo_request copying it; the function must IGNORE it and always emit
     * the LCP protocol (0xc021). All headers are built from the authoritative
     * vlan_id / session_id / magic_num, not from stored copies. */
    ppp_ccb_t s_ppp_ccb_1 = {
        .ppp_phase = {{
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(PAP_PROTOCOL),
            },
        },{},},
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id = htons(0x000a),
        .cp = 0,
        .magic_num = htonl(0x01020304),
        .fastrg_ccb = fastrg_ccb,
    };

    /* VLAN-tagged LCP Echo-Request carrying our magic number. The identifier byte
     * (index 27) is randomised by build_echo_request, so it is copied across from
     * the produced buffer before the content comparison. */
    char pkt_lcp[] = {/* mac dst */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31,
        /* mac src */0x9c, 0x69, 0xb4, 0x61, 0x16, 0xdd, /* eth type */0x81, 0x00,
        /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */0x11, 0x00, 0x00, 0x0a, 0x00, 0x0a,
        /* ppp protocol */0xc0, 0x21, /* ppp hdr (code,id,len) */0x09, 0x00, 0x00, 0x08,
        /* magic number */0x01, 0x02, 0x03, 0x04};

    printf("Test 1: \"%s\"\n", "build_echo_request() LCP result (ignores stale PAP slot)");
    build_echo_request(buffer, &mulen, &s_ppp_ccb_1);
    pkt_lcp[27] = buffer[27]; /* identifier is randomised — exclude from compare */

    TEST_ASSERT(mulen == sizeof(pkt_lcp), "build_echo_request packet length",
        "expected length %zu, got %u", sizeof(pkt_lcp), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_lcp, sizeof(pkt_lcp)) == 0,
        "build_echo_request packet content", "packet content mismatch");
    /* Regression guard for the fixed bug: protocol must be LCP (0xc021). */
    TEST_ASSERT(buffer[24] == 0xc0 && buffer[25] == 0x21,
        "build_echo_request emits LCP protocol (not stale PAP)",
        "expected 0xc021, got 0x%02x%02x", buffer[24], buffer[25]);
}

void test_build_auth_request_pap(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_auth_request_pap function:\n");
    printf("=========================================\n\n");

    U8 buffer[80] = { 0 };
    U16 mulen = 0;

    ppp_ccb_t s_ppp_ccb_1 = {
        .ppp_phase = {{
            .timer_counter = 0,
            .max_retransmit = 10,
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(LCP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = CONFIG_REQUEST,
                .identifier = 0x01,
                .length = htons(0x0000),
            },
        },{},},
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id = htons(0x000a),
        .cp = 0,
        .magic_num = htonl(0x01020304),
        .ppp_user_acc = (U8 *)"asdf", // 0x61, 0x73, 0x64, 0x66
        .ppp_passwd = (U8 *)"zxcv", // 0x7a, 0x78, 0x63, 0x76
        .identifier = 0xfe,
        .eth_hdr = (struct rte_ether_hdr) {
            .ether_type = htons(VLAN),
        },
        .vlan_header = (vlan_header_t) {
            .tci_union.tci_value = htons(0002),
            .next_proto = htons(ETH_P_PPP_SES),
        },
        .pppoe_header = (pppoe_header_t) {
            .code = 0,
            .ver_type = 0x11,
            .session_id = htons(0x000a),
            .length = htons(0x0000),
        },
        .fastrg_ccb = fastrg_ccb,
    };

    char pkt_lcp_1[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x10, /* ppp protocol */0xc0, 0x23, /* ppp hdr*/
    0x01, 0xfe, 0x00, 0x0e, /* pap user */0x04, 0x61, 0x73, 0x64, 0x66, /* pap passwd */ 
    0x04, 0x7a, 0x78, 0x63, 0x76};

    printf("Test 1: \"%s\"\n", "build_auth_request_pap() LCP result");
    build_auth_request_pap(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_lcp_1), "build_auth_request_pap packet length", 
        "expected length %zu, got %u", sizeof(pkt_lcp_1), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_lcp_1, sizeof(pkt_lcp_1)) == 0, 
        "build_auth_request_pap packet content", "packet content mismatch");

    char pkt_lcp_2[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x20, /* ppp protocol */0xc0, 0x23, /* ppp hdr*/
    0x01, 0xfe, 0x00, 0x1e, /* pap user */0x08, 0x31, 0x71, 0x61, 0x7a, 0x32, 0x77, 
    0x73, 0x78, /* pap passwd */0x10, 0x33, 0x65, 0x64, 0x63, 0x34, 0x72, 0x66, 0x76, 
    0x35, 0x74, 0x67, 0x62, 0x36, 0x79, 0x68, 0x6e};
    s_ppp_ccb_1.ppp_user_acc = (U8 *)"1qaz2wsx"; // 0x31, 0x71, 0x61, 0x7a, 0x32, 0x77, 0x73, 0x78
    s_ppp_ccb_1.ppp_passwd = (U8 *)"3edc4rfv5tgb6yhn"; // 0x33, 0x65, 0x64, 0x63, 0x34, 0x72, 0x66, 0x76, 0x35, 0x74, 0x67, 0x62, 0x36, 0x79, 0x68, 0x6e

    printf("Test 2: \"%s\"\n", "build_auth_request_pap() LCP result with longer username/password");
    build_auth_request_pap(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_lcp_2), "build_auth_request_pap packet length", 
        "expected length %zu, got %u", sizeof(pkt_lcp_2), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_lcp_2, sizeof(pkt_lcp_2)) == 0, 
        "build_auth_request_pap packet content", "packet content mismatch");
}

void test_build_auth_ack_pap(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_auth_ack_pap function:\n");
    printf("=========================================\n\n");

    U8 buffer[80] = { 0 };
    U16 mulen = 0;

    ppp_ccb_t s_ppp_ccb_1 = {
        .ppp_phase = {{
            .timer_counter = 0,
            .max_retransmit = 10,
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(LCP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = CONFIG_REQUEST,
                .identifier = 0x01,
                .length = htons(0x0000),
            },
        },{},},
        .user_num = 1,
        .vlan_id = {
            .cnt = 2,
        },
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id = htons(0x000a),
        .cp = 0,
        .magic_num = htonl(0x01020304),
        .ppp_user_acc = (U8 *)"asdf", // 0x61, 0x73, 0x64, 0x66
        .ppp_passwd = (U8 *)"zxcv", // 0x7a, 0x78, 0x63, 0x76
        .identifier = 0xfe,
        .eth_hdr = (struct rte_ether_hdr) {
            .ether_type = htons(VLAN),
        },
        .vlan_header = (vlan_header_t) {
            .tci_union.tci_value = htons(0002),
            .next_proto = htons(ETH_P_PPP_SES),
        },
        .pppoe_header = (pppoe_header_t) {
            .code = 0,
            .ver_type = 0x11,
            .session_id = htons(0x000a),
            .length = htons(0x0000),
        },
        .fastrg_ccb = fastrg_ccb,
    };

    char pkt_lcp_1[] = {/* mac */0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31, 0x9c, 0x69, 0xb4, 
    0x61, 0x16, 0xdd, 0x81, 0x00, /* vlan */0x00, 0x02, 0x88, 0x64, /* pppoe hdr */
    0x11, 0x00, 0x00, 0x0a, 0x00, 0x0f, /* ppp protocol */0xc0, 0x23, /* ppp hdr*/
    0x02, 0xfe, 0x00, 0x0d, /* Login ok */0x08, 0x4c, 0x6f, 0x67, 0x69, 0x6e, 0x20, 
    0x6f, 0x6b};

    printf("Test 1: \"%s\"\n", "build_auth_ack_pap() LCP result");
    build_auth_ack_pap(buffer, &mulen, &s_ppp_ccb_1);
    TEST_ASSERT(mulen == sizeof(pkt_lcp_1), "build_auth_ack_pap packet length", 
        "expected length %zu, got %u", sizeof(pkt_lcp_1), mulen);
    TEST_ASSERT(memcmp(buffer, pkt_lcp_1, sizeof(pkt_lcp_1)) == 0, 
        "build_auth_ack_pap packet content", "packet content mismatch");
}

void test_build_auth_response_chap(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_auth_response_chap function:\n");
    printf("=========================================\n\n");

    U8 buffer[80] = {0};
    U8 challenge[16] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f
    };
    U8 expected_hash[16];
    U8 expected_dst[] = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31};
    U8 expected_src[] = {0x9c, 0x69, 0xb4, 0x61, 0x16, 0xdd};
    U16 mulen = 0;
    MD5_CTX context;
    ppp_chap_data_t chap_data = {
        .val_size = sizeof(challenge),
        .val = challenge,
        .name = NULL,
    };
    ppp_ccb_t s_ppp_ccb = {
        .ppp_phase = {{
            .ppp_payload = (ppp_payload_t) {
                .ppp_protocol = htons(CHAP_PROTOCOL),
            },
            .ppp_hdr = (ppp_header_t) {
                .code = CHAP_CHALLENGE,
                .identifier = 0x37,
                .length = htons(sizeof(ppp_header_t) + sizeof(U8) + sizeof(challenge)),
            },
        },{},},
        .user_num = 1,
        .ppp_user_acc = (U8 *)"user1",
        .ppp_passwd = (U8 *)"pass1",
        .eth_hdr = (struct rte_ether_hdr) {
            .dst_addr = {.addr_bytes = {0x9c, 0x69, 0xb4, 0x61, 0x16, 0xdd}},
            .src_addr = {.addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31}},
            .ether_type = htons(VLAN),
        },
        .vlan_header = (vlan_header_t) {
            .tci_union.tci_value = htons(2),
            .next_proto = htons(ETH_P_PPP_SES),
        },
        .pppoe_header = (pppoe_header_t) {
            .ver_type = VER_TYPE,
            .code = SESSION_DATA,
            .session_id = htons(0x000a),
        },
        .fastrg_ccb = fastrg_ccb,
    };

    printf("Test 1: \"%s\"\n", "16-byte CHAP challenge response");
    MD5Init(&context);
    MD5Update(&context, &s_ppp_ccb.ppp_phase[0].ppp_hdr.identifier, sizeof(U8));
    MD5Update(&context, (U8 *)"pass1", strlen("pass1"));
    MD5Update(&context, challenge, sizeof(challenge));
    MD5Final(expected_hash, &context);

    build_auth_response_chap(buffer, &mulen, &s_ppp_ccb, &chap_data);

    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buffer;
    vlan_header_t *vlan_hdr = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t *pppoe_hdr = (pppoe_header_t *)(vlan_hdr + 1);
    ppp_payload_t *ppp_payload = (ppp_payload_t *)(pppoe_hdr + 1);
    ppp_header_t *ppp_hdr = (ppp_header_t *)(ppp_payload + 1);
    U8 *response_data = (U8 *)(ppp_hdr + 1);
    U16 expected_ppp_len = sizeof(ppp_header_t) + sizeof(U8) + sizeof(expected_hash) + strlen("user1");

    TEST_ASSERT(memcmp(eth_hdr->dst_addr.addr_bytes, expected_dst, RTE_ETHER_ADDR_LEN) == 0 &&
        memcmp(eth_hdr->src_addr.addr_bytes, expected_src, RTE_ETHER_ADDR_LEN) == 0,
        "CHAP response swaps Ethernet addresses", "Ethernet addresses were not swapped");
    TEST_ASSERT(eth_hdr->ether_type == htons(VLAN) && vlan_hdr->tci_union.tci_value == htons(2) &&
        vlan_hdr->next_proto == htons(ETH_P_PPP_SES),
        "CHAP response preserves VLAN fields", "unexpected Ethernet/VLAN fields");
    TEST_ASSERT(pppoe_hdr->ver_type == VER_TYPE && pppoe_hdr->code == SESSION_DATA &&
        pppoe_hdr->session_id == htons(0x000a),
        "CHAP response preserves PPPoE session fields", "unexpected PPPoE session fields");
    TEST_ASSERT(ppp_payload->ppp_protocol == htons(CHAP_PROTOCOL),
        "CHAP response protocol", "expected 0x%04x, got 0x%04x", CHAP_PROTOCOL,
        ntohs(ppp_payload->ppp_protocol));
    TEST_ASSERT(ppp_hdr->code == CHAP_RESPONSE && ppp_hdr->identifier == 0x37,
        "CHAP response code and identifier", "code=%u identifier=%u", ppp_hdr->code, ppp_hdr->identifier);
    TEST_ASSERT(ntohs(ppp_hdr->length) == expected_ppp_len &&
        ntohs(pppoe_hdr->length) == sizeof(ppp_payload_t) + expected_ppp_len,
        "CHAP response network-order lengths", "PPP=%u PPPoE=%u", ntohs(ppp_hdr->length),
        ntohs(pppoe_hdr->length));
    TEST_ASSERT(response_data[0] == sizeof(expected_hash),
        "CHAP response value size", "expected %zu, got %u", sizeof(expected_hash), response_data[0]);
    TEST_ASSERT(memcmp(response_data + sizeof(U8), expected_hash, sizeof(expected_hash)) == 0,
        "CHAP response MD5 hash", "hash mismatch");
    TEST_ASSERT(memcmp(response_data + sizeof(U8) + sizeof(expected_hash), "user1", strlen("user1")) == 0,
        "CHAP response name without NUL", "name mismatch");
    TEST_ASSERT(mulen == sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t) +
        sizeof(ppp_payload_t) + expected_ppp_len,
        "CHAP response frame length", "expected %zu, got %u",
        sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) + sizeof(pppoe_header_t) +
        sizeof(ppp_payload_t) + expected_ppp_len, mulen);

    printf("Test 2: \"%s\"\n", "8-byte CHAP challenge hashes only its declared value");
    chap_data.val_size = 8;
    MD5Init(&context);
    MD5Update(&context, &s_ppp_ccb.ppp_phase[0].ppp_hdr.identifier, sizeof(U8));
    MD5Update(&context, (U8 *)"pass1", strlen("pass1"));
    MD5Update(&context, challenge, chap_data.val_size);
    MD5Final(expected_hash, &context);
    build_auth_response_chap(buffer, &mulen, &s_ppp_ccb, &chap_data);
    response_data = (U8 *)(ppp_hdr + 1);
    TEST_ASSERT(memcmp(response_data + sizeof(U8), expected_hash, sizeof(expected_hash)) == 0,
        "CHAP response honors 8-byte challenge length", "hash mismatch");
}

void test_build_proto_reject(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_proto_reject function:\n");
    printf("=========================================\n\n");

    U8  buffer[1600] = {0};
    U16 mulen         = 0;

    ppp_ccb_t s_ppp_ccb = {
        .user_num  = 1,
        .vlan_id   = {.cnt = 2},
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id  = htons(0x000a),
        .fastrg_ccb  = fastrg_ccb,
    };

    /* Test 1: MPLSCP reject, no rejected_info */
    printf("Test 1: \"%s\"\n", "build_proto_reject() MPLSCP, no info");
    build_proto_reject(buffer, &mulen, &s_ppp_ccb, MPLSCP_PROTOCOL, NULL, 0);
    /* overhead = eth(14) + vlan(4) + pppoe(6) + ppp_payload(2) + ppp_hdr(4) + rej_proto(2) = 32 */
    TEST_ASSERT(mulen == 32, "build_proto_reject MPLSCP mulen",
        "expected 32, got %u", mulen);
    TEST_ASSERT(memcmp(buffer, s_ppp_ccb.PPP_dst_mac.addr_bytes, 6) == 0,
        "build_proto_reject MPLSCP dst MAC", "dst MAC mismatch");
    TEST_ASSERT(memcmp(buffer + 6, fastrg_ccb->nic_info.hsi_wan_src_mac.addr_bytes, 6) == 0,
        "build_proto_reject MPLSCP src MAC", "src MAC mismatch");
    TEST_ASSERT(*(U16 *)(buffer + 12) == htons(VLAN),
        "build_proto_reject MPLSCP ether_type", "expected 0x8100, got 0x%04x", ntohs(*(U16 *)(buffer + 12)));
    TEST_ASSERT(*(U16 *)(buffer + 14) == htons(0x0002),
        "build_proto_reject MPLSCP vlan TCI", "expected 0x0002, got 0x%04x", ntohs(*(U16 *)(buffer + 14)));
    TEST_ASSERT(*(U16 *)(buffer + 16) == htons(ETH_P_PPP_SES),
        "build_proto_reject MPLSCP next_proto", "expected 0x8864, got 0x%04x", ntohs(*(U16 *)(buffer + 16)));
    TEST_ASSERT(buffer[18] == 0x11 && buffer[19] == 0x00,
        "build_proto_reject MPLSCP pppoe ver/code", "expected ver=0x11 code=0x00");
    TEST_ASSERT(*(U16 *)(buffer + 20) == htons(0x000a),
        "build_proto_reject MPLSCP session_id", "expected 0x000a, got 0x%04x", ntohs(*(U16 *)(buffer + 20)));
    /* pppoe_len = ppp_payload(2) + ppp_hdr(4) + rej_proto(2) + info(0) = 8 */
    TEST_ASSERT(*(U16 *)(buffer + 22) == htons(8),
        "build_proto_reject MPLSCP pppoe length", "expected 8, got %u", ntohs(*(U16 *)(buffer + 22)));
    TEST_ASSERT(*(U16 *)(buffer + 24) == htons(LCP_PROTOCOL),
        "build_proto_reject MPLSCP ppp protocol", "expected LCP 0xc021, got 0x%04x", ntohs(*(U16 *)(buffer + 24)));
    TEST_ASSERT(buffer[26] == PROTO_REJECT,
        "build_proto_reject MPLSCP ppp code", "expected PROTO_REJECT 0x08, got 0x%02x", buffer[26]);
    /* ppp_len = ppp_hdr(4) + rej_proto(2) + info(0) = 6 */
    TEST_ASSERT(*(U16 *)(buffer + 28) == htons(6),
        "build_proto_reject MPLSCP ppp length", "expected 6, got %u", ntohs(*(U16 *)(buffer + 28)));
    TEST_ASSERT(*(U16 *)(buffer + 30) == htons(MPLSCP_PROTOCOL),
        "build_proto_reject MPLSCP rejected_proto", "expected 0x8281, got 0x%04x", ntohs(*(U16 *)(buffer + 30)));

    /* Test 2: IPV6CP reject with 4-byte info */
    printf("Test 2: \"%s\"\n", "build_proto_reject() IPV6CP, 4-byte info");
    U8 rej_info[] = {0xAA, 0xBB, 0xCC, 0xDD};
    memset(buffer, 0, sizeof(buffer));
    mulen = 0;
    build_proto_reject(buffer, &mulen, &s_ppp_ccb, IPV6CP_PROTOCOL, rej_info, sizeof(rej_info));
    /* mulen = 32 (overhead) + 4 (info) = 36 */
    TEST_ASSERT(mulen == 36, "build_proto_reject IPV6CP mulen",
        "expected 36, got %u", mulen);
    /* pppoe_len = 2 + 4 + 2 + 4 = 12 */
    TEST_ASSERT(*(U16 *)(buffer + 22) == htons(12),
        "build_proto_reject IPV6CP pppoe length", "expected 12, got %u", ntohs(*(U16 *)(buffer + 22)));
    TEST_ASSERT(buffer[26] == PROTO_REJECT,
        "build_proto_reject IPV6CP ppp code", "expected PROTO_REJECT 0x08, got 0x%02x", buffer[26]);
    /* ppp_len = 4 + 2 + 4 = 10 */
    TEST_ASSERT(*(U16 *)(buffer + 28) == htons(10),
        "build_proto_reject IPV6CP ppp length", "expected 10, got %u", ntohs(*(U16 *)(buffer + 28)));
    TEST_ASSERT(*(U16 *)(buffer + 30) == htons(IPV6CP_PROTOCOL),
        "build_proto_reject IPV6CP rejected_proto", "expected 0x8057, got 0x%04x", ntohs(*(U16 *)(buffer + 30)));
    TEST_ASSERT(memcmp(buffer + 32, rej_info, sizeof(rej_info)) == 0,
        "build_proto_reject IPV6CP info copied", "rejected_info bytes mismatch");

    /* Test 3: MTU truncation — rejected_info_len > max_info should be clamped */
    printf("Test 3: \"%s\"\n", "build_proto_reject() MTU truncation");
    U8 big_info[2000];
    memset(big_info, 0xAB, sizeof(big_info));
    memset(buffer, 0, sizeof(buffer));
    mulen = 0;
    build_proto_reject(buffer, &mulen, &s_ppp_ccb, MPLSCP_PROTOCOL, big_info, sizeof(big_info));
    /* max_info = ETH_MTU - 32 = 1468; mulen capped at ETH_MTU = 1500 */
    TEST_ASSERT(mulen == ETH_MTU, "build_proto_reject MTU truncated mulen",
        "expected %u (ETH_MTU), got %u", ETH_MTU, mulen);
    /* pppoe_len = 2 + 4 + 2 + 1468 = 1476 */
    TEST_ASSERT(*(U16 *)(buffer + 22) == htons(1476),
        "build_proto_reject MTU truncated pppoe length", "expected 1476, got %u", ntohs(*(U16 *)(buffer + 22)));
    /* ppp_len = 4 + 2 + 1468 = 1474 */
    TEST_ASSERT(*(U16 *)(buffer + 28) == htons(1474),
        "build_proto_reject MTU truncated ppp length", "expected 1474, got %u", ntohs(*(U16 *)(buffer + 28)));
    TEST_ASSERT(buffer[32] == 0xAB,
        "build_proto_reject MTU truncated info first byte", "expected 0xAB, got 0x%02x", buffer[32]);
}

void test_build_code_reject(FastRG_t *fastrg_ccb)
{
    printf("\nTesting build_code_reject function:\n");
    printf("=========================================\n\n");

    U8  buffer[1600] = {0};
    U16 mulen         = 0;
    U8  lcp_opts[]    = {0xDE, 0xAD};
    U8  big_opts[116];

    ppp_ccb_t s_ppp_ccb = {
        .user_num  = 1,
        .vlan_id   = {.cnt = 2},
        .PPP_dst_mac = (struct rte_ether_addr){
            .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31},
        },
        .session_id  = htons(0x000a),
        .fastrg_ccb  = fastrg_ccb,
    };

    /* Test 1: unknown LCP code — Code-Reject rides on LCP, copies the packet */
    printf("Test 1: \"%s\"\n", "build_code_reject() LCP unknown code");
    s_ppp_ccb.cp = 0;
    s_ppp_ccb.ppp_phase[0].ppp_hdr = (ppp_header_t){.code = 0x0E, .identifier = 0x42, .length = htons(6)};
    s_ppp_ccb.ppp_phase[0].ppp_options = (ppp_options_t *)lcp_opts;
    TEST_ASSERT(build_code_reject(buffer, &s_ppp_ccb, &mulen) == SUCCESS,
        "build_code_reject LCP returns SUCCESS", "");
    /* overhead = eth(14) + vlan(4) + pppoe(6) + ppp_payload(2) + ppp_hdr(4) = 30; + rejected pkt(6) = 36 */
    TEST_ASSERT(mulen == 36, "build_code_reject LCP mulen", "expected 36, got %u", mulen);
    TEST_ASSERT(memcmp(buffer, s_ppp_ccb.PPP_dst_mac.addr_bytes, 6) == 0,
        "build_code_reject LCP dst MAC", "dst MAC mismatch");
    TEST_ASSERT(*(U16 *)(buffer + 16) == htons(ETH_P_PPP_SES),
        "build_code_reject LCP next_proto", "expected 0x8864, got 0x%04x", ntohs(*(U16 *)(buffer + 16)));
    TEST_ASSERT(*(U16 *)(buffer + 20) == htons(0x000a),
        "build_code_reject LCP session_id", "expected 0x000a, got 0x%04x", ntohs(*(U16 *)(buffer + 20)));
    /* pppoe_len = ppp_payload(2) + ppp_hdr(4) + rejected pkt(6) = 12 */
    TEST_ASSERT(*(U16 *)(buffer + 22) == htons(12),
        "build_code_reject LCP pppoe length", "expected 12, got %u", ntohs(*(U16 *)(buffer + 22)));
    TEST_ASSERT(*(U16 *)(buffer + 24) == htons(LCP_PROTOCOL),
        "build_code_reject LCP ppp protocol", "expected LCP 0xc021, got 0x%04x", ntohs(*(U16 *)(buffer + 24)));
    TEST_ASSERT(buffer[26] == CODE_REJECT,
        "build_code_reject LCP ppp code", "expected CODE_REJECT 0x07, got 0x%02x", buffer[26]);
    /* ppp_len = ppp_hdr(4) + rejected pkt(6) = 10 */
    TEST_ASSERT(*(U16 *)(buffer + 28) == htons(10),
        "build_code_reject LCP ppp length", "expected 10, got %u", ntohs(*(U16 *)(buffer + 28)));
    U8 expected_rej[] = {0x0E, 0x42, 0x00, 0x06, 0xDE, 0xAD};
    TEST_ASSERT(memcmp(buffer + 30, expected_rej, sizeof(expected_rej)) == 0,
        "build_code_reject LCP rejected packet copied", "rejected packet bytes mismatch");

    /* Test 2: unknown IPCP code — Code-Reject rides on IPCP */
    printf("Test 2: \"%s\"\n", "build_code_reject() IPCP unknown code");
    memset(buffer, 0, sizeof(buffer));
    mulen = 0;
    s_ppp_ccb.cp = 1;
    s_ppp_ccb.ppp_phase[1].ppp_hdr = (ppp_header_t){.code = 0x0D, .identifier = 0x11, .length = htons(4)};
    s_ppp_ccb.ppp_phase[1].ppp_options = NULL;
    TEST_ASSERT(build_code_reject(buffer, &s_ppp_ccb, &mulen) == SUCCESS,
        "build_code_reject IPCP returns SUCCESS", "");
    TEST_ASSERT(mulen == 34, "build_code_reject IPCP mulen", "expected 34, got %u", mulen);
    TEST_ASSERT(*(U16 *)(buffer + 24) == htons(IPCP_PROTOCOL),
        "build_code_reject IPCP ppp protocol", "expected IPCP 0x8021, got 0x%04x", ntohs(*(U16 *)(buffer + 24)));
    TEST_ASSERT(buffer[26] == CODE_REJECT,
        "build_code_reject IPCP ppp code", "expected CODE_REJECT 0x07, got 0x%02x", buffer[26]);
    U8 expected_ipcp_rej[] = {0x0D, 0x11, 0x00, 0x04};
    TEST_ASSERT(memcmp(buffer + 30, expected_ipcp_rej, sizeof(expected_ipcp_rej)) == 0,
        "build_code_reject IPCP rejected packet copied", "rejected packet bytes mismatch");

    /* Test 3: truncation — rejected packet longer than PPP_MSG_BUF_LEN - 30 is clamped */
    printf("Test 3: \"%s\"\n", "build_code_reject() truncation to PPP_MSG_BUF_LEN");
    memset(big_opts, 0xAB, sizeof(big_opts));
    memset(buffer, 0, sizeof(buffer));
    mulen = 0;
    s_ppp_ccb.cp = 0;
    s_ppp_ccb.ppp_phase[0].ppp_hdr = (ppp_header_t){.code = 0x0E, .identifier = 0x43, .length = htons(120)};
    s_ppp_ccb.ppp_phase[0].ppp_options = (ppp_options_t *)big_opts;
    TEST_ASSERT(build_code_reject(buffer, &s_ppp_ccb, &mulen) == SUCCESS,
        "build_code_reject truncated returns SUCCESS", "");
    /* max_rej = PPP_MSG_BUF_LEN(128) - 30 = 98; mulen = 30 + 98 = 128 */
    TEST_ASSERT(mulen == PPP_MSG_BUF_LEN, "build_code_reject truncated mulen",
        "expected %u (PPP_MSG_BUF_LEN), got %u", PPP_MSG_BUF_LEN, mulen);
    /* ppp_len = 4 + 98 = 102 */
    TEST_ASSERT(*(U16 *)(buffer + 28) == htons(102),
        "build_code_reject truncated ppp length", "expected 102, got %u", ntohs(*(U16 *)(buffer + 28)));
    /* inner Length field of the copied packet keeps its original value per RFC 1661 §5.6 */
    TEST_ASSERT(*(U16 *)(buffer + 32) == htons(120),
        "build_code_reject truncated inner length preserved", "expected 120, got %u", ntohs(*(U16 *)(buffer + 32)));
    TEST_ASSERT(buffer[PPP_MSG_BUF_LEN - 1] == 0xAB && buffer[PPP_MSG_BUF_LEN] == 0x00,
        "build_code_reject truncated copy stops at buffer end", "copy overran PPP_MSG_BUF_LEN");

    /* Test 4: stashed packet shorter than a PPP header → ERROR, nothing to reject */
    printf("Test 4: \"%s\"\n", "build_code_reject() bogus stashed length");
    mulen = 0;
    s_ppp_ccb.ppp_phase[0].ppp_hdr = (ppp_header_t){.code = 0x0E, .identifier = 0x44, .length = htons(2)};
    TEST_ASSERT(build_code_reject(buffer, &s_ppp_ccb, &mulen) == ERROR,
        "build_code_reject short packet returns ERROR", "");
}

/* ==================== PPP_decode_frame decode-path tests ==================== */

/* ppp_ccb_t embeds the 16MB NAT slot pool — keep the decode fixture static
 * instead of burning a stack frame per case. */
static ppp_ccb_t decode_ccb;
static struct per_ccb_stats decode_wan_stats;

/**
 * @fn decode_env_init
 *
 * @brief one-time fixture for PPP_decode_frame tests: back wan_ctrl_tx with a
 *      real mempool (the unconfigured WAN port falls through to DPDK's dummy
 *      tx burst, so the frame is counted then dropped) and hang an observable
 *      per-subscriber stats row for ccb 0 on this lcore
 * @param fastrg_ccb
 *      mock FastRG control block from the test runner
 * @return
 *      void
 */
static void decode_env_init(FastRG_t *fastrg_ccb)
{
    if (direct_pool[0] == NULL) {
        direct_pool[0] = rte_pktmbuf_pool_create("codec_decode_pool", 32, 0, 0,
            RTE_MBUF_DEFAULT_BUF_SIZE, (int)rte_socket_id());
    }

    /* The mock FastRG_t is malloc'd, so the per-lcore stats grid is garbage —
     * wipe it and install one WAN row. The persistent-RCU flag makes the
     * stats getter skip the (unset) stats qsbr on this lcore. */
    memset(fastrg_ccb->per_subscriber_stats, 0,
        sizeof(fastrg_ccb->per_subscriber_stats));
    fastrg_ccb->per_subscriber_stats[rte_lcore_id()][WAN_PORT] = &decode_wan_stats;
    fastrg_rcu_persistent[rte_lcore_id()] = TRUE;
}

static void decode_ccb_reset(FastRG_t *fastrg_ccb, U8 phase)
{
    memset(&decode_ccb, 0, sizeof(decode_ccb));
    decode_ccb.fastrg_ccb = fastrg_ccb;
    decode_ccb.user_num = 1;
    rte_atomic16_set(&decode_ccb.vlan_id, 2);
    decode_ccb.session_id = rte_cpu_to_be_16(0x000a);
    decode_ccb.phase = phase;
    decode_ccb.PPP_dst_mac = (struct rte_ether_addr){
        .addr_bytes = {0x74, 0x4d, 0x28, 0x8d, 0x00, 0x31}
    };
}

/**
 * @fn build_session_frame
 *
 * @brief craft an eth/vlan/PPPoE-session frame carrying one PPP header
 * @param buf
 *      output buffer (must hold 30 + info_len bytes)
 * @param ppp_proto
 *      PPP protocol number in host order (e.g. IPV6CP_PROTOCOL)
 * @param code
 *      PPP header code field
 * @param ppp_hdr_len
 *      value for the PPP header length field, host order (may deliberately
 *      disagree with the real payload for malformed-frame tests)
 * @param info_len
 *      bytes of zero payload appended after the PPP header
 * @return
 *      total frame length
 */
static U16 build_session_frame(U8 *buf, U16 ppp_proto, U8 code,
    U16 ppp_hdr_len, U16 info_len)
{
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)buf;
    vlan_header_t *vlan_hdr = (vlan_header_t *)(eth_hdr + 1);
    pppoe_header_t *pppoe_hdr = (pppoe_header_t *)(vlan_hdr + 1);
    ppp_payload_t *ppp_payload = (ppp_payload_t *)(pppoe_hdr + 1);
    ppp_header_t *ppp_hdr = (ppp_header_t *)(ppp_payload + 1);

    memset(buf, 0, sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        sizeof(pppoe_header_t) + sizeof(ppp_payload_t) +
        sizeof(ppp_header_t) + info_len);
    eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);
    vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(0x0002);
    vlan_hdr->next_proto = rte_cpu_to_be_16(ETH_P_PPP_SES);
    pppoe_hdr->ver_type = VER_TYPE;
    pppoe_hdr->code = SESSION_DATA;
    pppoe_hdr->session_id = rte_cpu_to_be_16(0x000a);
    pppoe_hdr->length = rte_cpu_to_be_16(sizeof(ppp_payload_t) +
        sizeof(ppp_header_t) + info_len);
    ppp_payload->ppp_protocol = rte_cpu_to_be_16(ppp_proto);
    ppp_hdr->code = code;
    ppp_hdr->identifier = 1;
    ppp_hdr->length = rte_cpu_to_be_16(ppp_hdr_len);

    return sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        sizeof(pppoe_header_t) + sizeof(ppp_payload_t) +
        sizeof(ppp_header_t) + info_len;
}

void test_ppp_decode_frame(FastRG_t *fastrg_ccb)
{
    printf("\nTesting PPP_decode_frame function:\n");
    printf("=========================================\n\n");

    U8 frame[128];
    U16 event = 0;
    U16 frame_len;

    decode_env_init(fastrg_ccb);
    memset(&decode_wan_stats, 0, sizeof(decode_wan_stats));

    /* Test 1: unsupported IPV6CP must be answered with Protocol-Reject */
    printf("Test 1: \"%s\"\n", "IPV6CP replied with Protocol-Reject");
    decode_ccb_reset(fastrg_ccb, DATA_PHASE);
    frame_len = build_session_frame(frame, IPV6CP_PROTOCOL, CONFIG_REQUEST,
        sizeof(ppp_header_t) + 4, 4);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == ERROR,
        "IPV6CP frame short-circuits with ERROR (stateless reply)", NULL);
    TEST_ASSERT(decode_wan_stats.tx_packets == 1,
        "Protocol-Reject was handed to wan_ctrl_tx for IPV6CP",
        "tx_packets=%" PRIu64, decode_wan_stats.tx_packets);

    /* Test 2: unsupported MPLSCP takes the same Protocol-Reject path */
    printf("Test 2: \"%s\"\n", "MPLSCP replied with Protocol-Reject");
    decode_ccb_reset(fastrg_ccb, DATA_PHASE);
    frame_len = build_session_frame(frame, MPLSCP_PROTOCOL, CONFIG_REQUEST,
        sizeof(ppp_header_t), 0);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == ERROR,
        "MPLSCP frame short-circuits with ERROR (stateless reply)", NULL);
    TEST_ASSERT(decode_wan_stats.tx_packets == 2,
        "Protocol-Reject was handed to wan_ctrl_tx for MPLSCP",
        "tx_packets=%" PRIu64, decode_wan_stats.tx_packets);

    /* Test 3: unknown PPP protocol is dropped without any reply */
    printf("Test 3: \"%s\"\n", "unknown PPP protocol dropped silently");
    decode_ccb_reset(fastrg_ccb, DATA_PHASE);
    frame_len = build_session_frame(frame, 0x9999, CONFIG_REQUEST,
        sizeof(ppp_header_t), 0);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == ERROR,
        "unknown PPP protocol returns ERROR", NULL);
    TEST_ASSERT(decode_wan_stats.tx_packets == 2,
        "no reply is sent for an unknown PPP protocol",
        "tx_packets=%" PRIu64, decode_wan_stats.tx_packets);

    /* Test 4: PPP header length below the 4-byte minimum is rejected */
    printf("Test 4: \"%s\"\n", "malformed PPP header length rejected");
    decode_ccb_reset(fastrg_ccb, DATA_PHASE);
    frame_len = build_session_frame(frame, LCP_PROTOCOL, CONFIG_REQUEST,
        sizeof(ppp_header_t) - 2, 0);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == ERROR,
        "PPP header length < sizeof(ppp_header_t) returns ERROR", NULL);

    /* Test 5: frames above ETH_JUMBO are rejected before any parsing */
    printf("Test 5: \"%s\"\n", "oversized frame rejected");
    static U8 jumbo_frame[ETH_JUMBO + 8];
    decode_ccb_reset(fastrg_ccb, DATA_PHASE);
    memset(jumbo_frame, 0, sizeof(jumbo_frame));
    TEST_ASSERT(PPP_decode_frame(jumbo_frame, ETH_JUMBO + 1, &event, &decode_ccb) == ERROR,
        "frame larger than ETH_JUMBO returns ERROR", NULL);

    /* Test 6: NCP/auth phase guards */
    printf("Test 6: \"%s\"\n", "phase guards for IPCP and PAP");
    decode_ccb_reset(fastrg_ccb, LCP_PHASE);
    frame_len = build_session_frame(frame, IPCP_PROTOCOL, CONFIG_REQUEST,
        sizeof(ppp_header_t), 0);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == ERROR,
        "IPCP frame outside IPCP_PHASE returns ERROR", NULL);
    codec_cleanup_ppp_ccb(&decode_ccb);

    decode_ccb_reset(fastrg_ccb, LCP_PHASE);
    frame_len = build_session_frame(frame, PAP_PROTOCOL, PAP_ACK,
        sizeof(ppp_header_t), 0);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == ERROR,
        "PAP frame outside AUTH_PHASE returns ERROR", NULL);

    /* Test 7: auth results accepted in AUTH_PHASE */
    printf("Test 7: \"%s\"\n", "auth results in AUTH_PHASE");
    decode_ccb_reset(fastrg_ccb, AUTH_PHASE);
    frame_len = build_session_frame(frame, PAP_PROTOCOL, PAP_ACK,
        sizeof(ppp_header_t), 0);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == SUCCESS,
        "PAP ACK in AUTH_PHASE returns SUCCESS", NULL);

    decode_ccb_reset(fastrg_ccb, AUTH_PHASE);
    frame_len = build_session_frame(frame, CHAP_PROTOCOL, CHAP_SUCCESS,
        sizeof(ppp_header_t), 0);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == SUCCESS,
        "CHAP Success in AUTH_PHASE returns SUCCESS", NULL);
    TEST_ASSERT(decode_ccb.phase == IPCP_PHASE,
        "CHAP Success advances the session to IPCP_PHASE",
        "got phase %u", decode_ccb.phase);

    decode_ccb_reset(fastrg_ccb, AUTH_PHASE);
    frame_len = build_session_frame(frame, CHAP_PROTOCOL, CHAP_FAILURE,
        sizeof(ppp_header_t), 0);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == SUCCESS,
        "CHAP Failure in AUTH_PHASE returns SUCCESS", NULL);
    TEST_ASSERT(decode_ccb.phase == LCP_PHASE,
        "CHAP Failure drops the session back to LCP_PHASE",
        "got phase %u", decode_ccb.phase);

    /* Test 8: discovery frames never reach PPP payload parsing */
    printf("Test 8: \"%s\"\n", "discovery frame short-circuits");
    decode_ccb_reset(fastrg_ccb, PPPOE_PHASE);
    frame_len = build_session_frame(frame, LCP_PROTOCOL, CONFIG_REQUEST,
        sizeof(ppp_header_t), 0);
    vlan_header_t *vlan_hdr = (vlan_header_t *)(frame + sizeof(struct rte_ether_hdr));
    vlan_hdr->next_proto = rte_cpu_to_be_16(ETH_P_PPP_DIS);
    ((pppoe_header_t *)(vlan_hdr + 1))->code = PADM;
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == ERROR,
        "PPPoE discovery frame (PADM) returns ERROR by design", NULL);
}

void test_ppp_decode_frame_chap(FastRG_t *fastrg_ccb)
{
    printf("\nTesting PPP_decode_frame CHAP dispatch:\n");
    printf("=========================================\n\n");

    U8 frame[128];
    U8 challenge[16] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f
    };
    U16 event = 0;
    U16 frame_len;
    U16 chap_info_len = sizeof(U8) + sizeof(challenge);

    decode_env_init(fastrg_ccb);
    memset(&decode_wan_stats, 0, sizeof(decode_wan_stats));

    printf("Test 1: \"%s\"\n", "valid CHAP Challenge is dispatched and answered");
    decode_ccb_reset(fastrg_ccb, AUTH_PHASE);
    decode_ccb.ppp_user_acc = (U8 *)"user1";
    decode_ccb.ppp_passwd = (U8 *)"pass1";
    frame_len = build_session_frame(frame, CHAP_PROTOCOL, CHAP_CHALLENGE,
        sizeof(ppp_header_t) + chap_info_len, chap_info_len);
    ppp_header_t *ppp_hdr = (ppp_header_t *)(frame + sizeof(struct rte_ether_hdr) +
        sizeof(vlan_header_t) + sizeof(pppoe_header_t) + sizeof(ppp_payload_t));
    U8 *chap_data = (U8 *)(ppp_hdr + 1);
    ppp_hdr->identifier = 0x37;
    chap_data[0] = sizeof(challenge);
    rte_memcpy(chap_data + sizeof(U8), challenge, sizeof(challenge));
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == SUCCESS,
        "valid CHAP Challenge returns SUCCESS", NULL);
    TEST_ASSERT(decode_wan_stats.tx_packets == 1,
        "valid CHAP Challenge sends one response", "tx_packets=%" PRIu64, decode_wan_stats.tx_packets);

    printf("Test 2: \"%s\"\n", "malformed CHAP Challenge value size is rejected");
    decode_ccb_reset(fastrg_ccb, AUTH_PHASE);
    decode_ccb.ppp_user_acc = (U8 *)"user1";
    decode_ccb.ppp_passwd = (U8 *)"pass1";
    frame_len = build_session_frame(frame, CHAP_PROTOCOL, CHAP_CHALLENGE,
        sizeof(ppp_header_t) + chap_info_len, chap_info_len);
    ppp_hdr = (ppp_header_t *)(frame + sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
        sizeof(pppoe_header_t) + sizeof(ppp_payload_t));
    chap_data = (U8 *)(ppp_hdr + 1);
    chap_data[0] = sizeof(challenge) + 1;
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == ERROR,
        "malformed CHAP Challenge returns ERROR", NULL);
    TEST_ASSERT(decode_wan_stats.tx_packets == 1,
        "malformed CHAP Challenge sends no response", "tx_packets=%" PRIu64, decode_wan_stats.tx_packets);

    printf("Test 3: \"%s\"\n", "CHAP frame outside AUTH_PHASE is rejected");
    decode_ccb_reset(fastrg_ccb, LCP_PHASE);
    frame_len = build_session_frame(frame, CHAP_PROTOCOL, CHAP_SUCCESS,
        sizeof(ppp_header_t), 0);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == ERROR,
        "CHAP frame outside AUTH_PHASE returns ERROR", NULL);

    printf("Test 4: \"%s\"\n", "CHAP Success advances AUTH_PHASE to IPCP_PHASE");
    decode_ccb_reset(fastrg_ccb, AUTH_PHASE);
    frame_len = build_session_frame(frame, CHAP_PROTOCOL, CHAP_SUCCESS,
        sizeof(ppp_header_t), 0);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == SUCCESS,
        "CHAP Success in AUTH_PHASE returns SUCCESS", NULL);
    TEST_ASSERT(decode_ccb.phase == IPCP_PHASE,
        "CHAP Success advances AUTH_PHASE to IPCP_PHASE", "got phase %u", decode_ccb.phase);

    printf("Test 5: \"%s\"\n", "CHAP Failure returns AUTH_PHASE to LCP_PHASE");
    decode_ccb_reset(fastrg_ccb, AUTH_PHASE);
    frame_len = build_session_frame(frame, CHAP_PROTOCOL, CHAP_FAILURE,
        sizeof(ppp_header_t), 0);
    TEST_ASSERT(PPP_decode_frame(frame, frame_len, &event, &decode_ccb) == SUCCESS,
        "CHAP Failure in AUTH_PHASE returns SUCCESS", NULL);
    TEST_ASSERT(decode_ccb.phase == LCP_PHASE,
        "CHAP Failure returns AUTH_PHASE to LCP_PHASE", "got phase %u", decode_ccb.phase);
}

void test_ppp_codec(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           PPPD Codec Unit Tests                            ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_build_padi(fastrg_ccb);
    test_build_padr(fastrg_ccb);
    test_build_padt(fastrg_ccb);
    test_build_config_request(fastrg_ccb);
    test_build_config_ack(fastrg_ccb);
    test_build_terminate_request(fastrg_ccb);
    test_build_config_nak_rej(fastrg_ccb);
    test_build_terminate_ack(fastrg_ccb);
    test_build_echo_reply(fastrg_ccb);
    test_build_echo_request(fastrg_ccb);
    test_build_auth_request_pap(fastrg_ccb);
    test_build_auth_ack_pap(fastrg_ccb);
    test_build_auth_response_chap(fastrg_ccb);
    test_build_proto_reject(fastrg_ccb);
    test_build_code_reject(fastrg_ccb);
    test_ppp_decode_frame(fastrg_ccb);
    test_ppp_decode_frame_chap(fastrg_ccb);

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
