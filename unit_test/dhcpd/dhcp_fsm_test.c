#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <common.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_byteorder.h>
#include <rte_lcore.h>
#include <rte_timer.h>

#include "../../src/fastrg.h"
#include "../../src/dhcpd/dhcp_fsm.h"
#include "../../src/dhcpd/dhcp_codec.h"
#include "../../src/dhcpd/dhcpd.h"
#include "../../src/protocol.h"
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

#define FSM_TEST_POOL_LEN 2

static U8 fsm_pkt_buf[2048];
static dhcp_ccb_per_lan_user_t fsm_pool_users[FSM_TEST_POOL_LEN];
static dhcp_ccb_per_lan_user_t *fsm_pool_array[FSM_TEST_POOL_LEN];
static dhcp_ccb_t fsm_dhcp_ccb;
static FastRG_t *g_fastrg_ccb;
static BOOL fsm_env_initialized = FALSE;

static const struct rte_ether_addr fsm_client_mac = {
    .addr_bytes = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
};

/**
 * @fn fsm_env_reset
 *
 * @brief rebuild the packet/pool fixture and place lan user 0 in the given
 *      DHCP FSM state, mirroring the layout dhcp_decode leaves behind
 * @param state
 *      initial DHCP_STATE for lan user 0
 * @return
 *      dhcp_hdr pointer inside the packet buffer, for reply inspection
 */
static dhcp_hdr_t *fsm_env_reset(U8 state)
{
    /* Detach timers from the lcore skiplist before wiping their memory,
     * otherwise a pending timer armed by a previous case leaves a dangling
     * node behind. */
    if (fsm_env_initialized) {
        for(int i=0; i<FSM_TEST_POOL_LEN; i++)
            rte_timer_stop_sync(&fsm_pool_users[i].lan_user_info.timer);
    }
    fsm_env_initialized = TRUE;

    memset(fsm_pkt_buf, 0, sizeof(fsm_pkt_buf));
    memset(fsm_pool_users, 0, sizeof(fsm_pool_users));
    memset(&fsm_dhcp_ccb, 0, sizeof(fsm_dhcp_ccb));

    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)fsm_pkt_buf;
    rte_ether_addr_copy(&fsm_client_mac, &eth_hdr->src_addr);
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
    dhcp_hdr->mac_addr = eth_hdr->src_addr;
    dhcp_hdr->ur_client_ip = rte_cpu_to_be_32(0xC0A802AE);
    dhcp_hdr->magic_cookie = rte_cpu_to_be_32(DHCP_MAGIC_COOKIE);

    fsm_dhcp_ccb.eth_hdr = eth_hdr;
    fsm_dhcp_ccb.vlan_hdr = vlan_hdr;
    fsm_dhcp_ccb.ip_hdr = ip_hdr;
    fsm_dhcp_ccb.udp_hdr = udp_hdr;
    fsm_dhcp_ccb.dhcp_server_ip = rte_cpu_to_be_32(0xC0A80201);
    fsm_dhcp_ccb.subnet_mask = rte_cpu_to_be_32(0xFFFFFF00);
    fsm_dhcp_ccb.per_lan_user_pool = fsm_pool_array;
    fsm_dhcp_ccb.per_lan_user_pool_len = FSM_TEST_POOL_LEN;
    fsm_dhcp_ccb.fastrg_ccb = g_fastrg_ccb;

    for(int i=0; i<FSM_TEST_POOL_LEN; i++) {
        fsm_pool_array[i] = &fsm_pool_users[i];
        fsm_pool_users[i].dhcp_ccb = &fsm_dhcp_ccb;
        fsm_pool_users[i].pool_index = i;
        fsm_pool_users[i].ip_pool.ip_addr = rte_cpu_to_be_32(0xC0A802AE + i);
        fsm_pool_users[i].ip_pool.used = FALSE;
        rte_timer_init(&fsm_pool_users[i].lan_user_info.timer);
    }
    fsm_pool_users[0].dhcp_hdr = dhcp_hdr;
    fsm_pool_users[0].lan_user_info.state = state;
    fsm_pool_users[0].lan_user_info.lan_user_used = TRUE;
    rte_ether_addr_copy(&fsm_client_mac, &fsm_pool_users[0].lan_user_info.mac_addr);

    return dhcp_hdr;
}

/* First option in the reply is always DHCP_MSG_TYPE — return its value */
static U8 fsm_reply_msg_type(dhcp_hdr_t *dhcp_hdr)
{
    dhcp_opt_t *opt = (dhcp_opt_t *)(dhcp_hdr + 1);
    if (opt->opt_type != DHCP_MSG_TYPE || opt->len != 1)
        return 0;
    return opt->val[0];
}

static U8 fsm_state(void)
{
    return fsm_pool_users[0].lan_user_info.state;
}

static BOOL fsm_lan_user_released(void)
{
    return fsm_pool_users[0].ip_pool.used == FALSE &&
        fsm_pool_users[0].lan_user_info.lan_user_used == FALSE &&
        rte_is_zero_ether_addr(&fsm_pool_users[0].lan_user_info.mac_addr) &&
        fsm_pool_users[0].lan_user_info.state == S_DHCP_INIT;
}

static void test_init_state_transitions(void)
{
    printf("\nTesting transitions from S_DHCP_INIT:\n");
    printf("=====================================\n\n");

    dhcp_hdr_t *dhcp_hdr = fsm_env_reset(S_DHCP_INIT);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_DISCOVER) == SUCCESS,
        "INIT + E_DISCOVER returns SUCCESS", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_OFFER_SENT,
        "INIT + E_DISCOVER moves to OFFER_SENT", "got state %u", fsm_state());
    TEST_ASSERT(dhcp_hdr->msg_type == BOOT_REPLY,
        "OFFER reply is a BOOT_REPLY", "got %u", dhcp_hdr->msg_type);
    TEST_ASSERT(fsm_reply_msg_type(dhcp_hdr) == DHCP_OFFER,
        "OFFER reply carries DHCP_OFFER msg type option",
        "got %u", fsm_reply_msg_type(dhcp_hdr));
    TEST_ASSERT(rte_timer_pending(&fsm_pool_users[0].lan_user_info.timer),
        "request wait timer is armed after OFFER", NULL);

    dhcp_hdr = fsm_env_reset(S_DHCP_INIT);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_GOOD_REQUEST) == SUCCESS,
        "INIT + E_GOOD_REQUEST returns SUCCESS", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_ACK_SENT,
        "INIT + E_GOOD_REQUEST moves to ACK_SENT", "got state %u", fsm_state());
    TEST_ASSERT(fsm_reply_msg_type(dhcp_hdr) == DHCP_ACK,
        "ACK reply carries DHCP_ACK msg type option",
        "got %u", fsm_reply_msg_type(dhcp_hdr));
    TEST_ASSERT(rte_timer_pending(&fsm_pool_users[0].lan_user_info.timer),
        "lease timer is armed after ACK", NULL);

    dhcp_hdr = fsm_env_reset(S_DHCP_INIT);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_BAD_REQUEST) == SUCCESS,
        "INIT + E_BAD_REQUEST returns SUCCESS", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_INIT,
        "INIT + E_BAD_REQUEST stays in INIT", "got state %u", fsm_state());
    TEST_ASSERT(fsm_reply_msg_type(dhcp_hdr) == DHCP_NAK,
        "bad REQUEST is answered with DHCP_NAK",
        "got %u", fsm_reply_msg_type(dhcp_hdr));

    dhcp_hdr = fsm_env_reset(S_DHCP_INIT);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_INFORM) == SUCCESS,
        "INIT + E_INFORM returns SUCCESS", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_INIT,
        "INIT + E_INFORM stays in INIT", "got state %u", fsm_state());
    TEST_ASSERT(fsm_reply_msg_type(dhcp_hdr) == DHCP_ACK,
        "INFORM is answered with DHCP_ACK",
        "got %u", fsm_reply_msg_type(dhcp_hdr));
    TEST_ASSERT(dhcp_hdr->ur_client_ip == 0,
        "INFORM ACK does not assign an IP",
        "got 0x%08x", rte_be_to_cpu_32(dhcp_hdr->ur_client_ip));
}

static void test_offer_sent_transitions(void)
{
    printf("\nTesting transitions from S_DHCP_OFFER_SENT:\n");
    printf("===========================================\n\n");

    dhcp_hdr_t *dhcp_hdr = fsm_env_reset(S_DHCP_OFFER_SENT);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_DISCOVER) == SUCCESS,
        "OFFER_SENT + E_DISCOVER returns SUCCESS", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_OFFER_SENT,
        "retransmitted DISCOVER stays in OFFER_SENT", "got state %u", fsm_state());
    TEST_ASSERT(fsm_reply_msg_type(dhcp_hdr) == DHCP_OFFER,
        "retransmitted DISCOVER re-sends DHCP_OFFER",
        "got %u", fsm_reply_msg_type(dhcp_hdr));

    fsm_env_reset(S_DHCP_OFFER_SENT);
    fsm_pool_users[0].ip_pool.used = TRUE;
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_TIMEOUT) == SUCCESS,
        "OFFER_SENT + E_TIMEOUT returns SUCCESS", NULL);
    TEST_ASSERT(fsm_lan_user_released(),
        "request wait timeout releases the lan user back to INIT", NULL);
    TEST_ASSERT(!rte_timer_pending(&fsm_pool_users[0].lan_user_info.timer),
        "timer is stopped after release", NULL);

    dhcp_hdr = fsm_env_reset(S_DHCP_OFFER_SENT);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_GOOD_REQUEST) == SUCCESS,
        "OFFER_SENT + E_GOOD_REQUEST returns SUCCESS", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_ACK_SENT,
        "good REQUEST after OFFER moves to ACK_SENT", "got state %u", fsm_state());
    TEST_ASSERT(fsm_reply_msg_type(dhcp_hdr) == DHCP_ACK,
        "good REQUEST after OFFER is answered with DHCP_ACK",
        "got %u", fsm_reply_msg_type(dhcp_hdr));

    dhcp_hdr = fsm_env_reset(S_DHCP_OFFER_SENT);
    fsm_pool_users[0].ip_pool.used = TRUE;
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_BAD_REQUEST) == SUCCESS,
        "OFFER_SENT + E_BAD_REQUEST returns SUCCESS", NULL);
    TEST_ASSERT(fsm_reply_msg_type(dhcp_hdr) == DHCP_NAK,
        "bad REQUEST after OFFER is answered with DHCP_NAK",
        "got %u", fsm_reply_msg_type(dhcp_hdr));
    TEST_ASSERT(fsm_lan_user_released(),
        "bad REQUEST after OFFER releases the lan user back to INIT", NULL);
}

static void test_ack_sent_transitions(void)
{
    printf("\nTesting transitions from S_DHCP_ACK_SENT:\n");
    printf("=========================================\n\n");

    fsm_env_reset(S_DHCP_ACK_SENT);
    fsm_pool_users[0].ip_pool.used = TRUE;
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_TIMEOUT) == SUCCESS,
        "ACK_SENT + E_TIMEOUT returns SUCCESS", NULL);
    TEST_ASSERT(fsm_lan_user_released(),
        "lease expiry releases the lan user back to INIT", NULL);

    fsm_env_reset(S_DHCP_ACK_SENT);
    fsm_pool_users[0].ip_pool.used = TRUE;
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_RELEASE) == SUCCESS,
        "ACK_SENT + E_RELEASE returns SUCCESS", NULL);
    TEST_ASSERT(fsm_lan_user_released(),
        "client RELEASE frees the lan user back to INIT", NULL);

    dhcp_hdr_t *dhcp_hdr = fsm_env_reset(S_DHCP_ACK_SENT);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_GOOD_REQUEST) == SUCCESS,
        "ACK_SENT + E_GOOD_REQUEST (lease renewal) returns SUCCESS", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_ACK_SENT,
        "lease renewal stays in ACK_SENT", "got state %u", fsm_state());
    TEST_ASSERT(fsm_reply_msg_type(dhcp_hdr) == DHCP_ACK,
        "lease renewal is answered with DHCP_ACK",
        "got %u", fsm_reply_msg_type(dhcp_hdr));
    TEST_ASSERT(rte_timer_pending(&fsm_pool_users[0].lan_user_info.timer),
        "lease timer is re-armed on renewal", NULL);

    dhcp_hdr = fsm_env_reset(S_DHCP_ACK_SENT);
    fsm_pool_users[0].ip_pool.used = TRUE;
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_BAD_REQUEST) == SUCCESS,
        "ACK_SENT + E_BAD_REQUEST returns SUCCESS", NULL);
    TEST_ASSERT(fsm_reply_msg_type(dhcp_hdr) == DHCP_NAK,
        "bad REQUEST after ACK is answered with DHCP_NAK",
        "got %u", fsm_reply_msg_type(dhcp_hdr));
    TEST_ASSERT(fsm_lan_user_released(),
        "bad REQUEST after ACK releases the lan user back to INIT", NULL);

    dhcp_hdr = fsm_env_reset(S_DHCP_ACK_SENT);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_DISCOVER) == SUCCESS,
        "ACK_SENT + E_DISCOVER returns SUCCESS", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_OFFER_SENT,
        "DISCOVER after ACK restarts at OFFER_SENT", "got state %u", fsm_state());
    TEST_ASSERT(fsm_reply_msg_type(dhcp_hdr) == DHCP_OFFER,
        "DISCOVER after ACK is answered with DHCP_OFFER",
        "got %u", fsm_reply_msg_type(dhcp_hdr));

    dhcp_hdr = fsm_env_reset(S_DHCP_ACK_SENT);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_INFORM) == SUCCESS,
        "ACK_SENT + E_INFORM returns SUCCESS", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_ACK_SENT,
        "INFORM after ACK stays in ACK_SENT", "got state %u", fsm_state());
    TEST_ASSERT(fsm_reply_msg_type(dhcp_hdr) == DHCP_ACK,
        "INFORM after ACK is answered with DHCP_ACK",
        "got %u", fsm_reply_msg_type(dhcp_hdr));
}

static void test_decline_marks_ip_conflicted(void)
{
    printf("\nTesting E_DECLINE IP-conflict quarantine:\n");
    printf("=========================================\n\n");

    fsm_env_reset(S_DHCP_ACK_SENT);
    rte_ether_addr_copy(&fsm_client_mac, &fsm_pool_users[0].ip_pool.mac_addr);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_DECLINE) == SUCCESS,
        "ACK_SENT + E_DECLINE returns SUCCESS", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_INIT,
        "DECLINE moves back to INIT", "got state %u", fsm_state());
    TEST_ASSERT(fsm_pool_users[0].ip_pool.used == TRUE,
        "declined IP stays marked used (quarantined)", NULL);
    TEST_ASSERT(fsm_pool_users[0].lan_user_info.lan_user_used == TRUE,
        "declined pool slot stays reserved", NULL);
    TEST_ASSERT(rte_is_zero_ether_addr(&fsm_pool_users[0].lan_user_info.mac_addr) &&
        rte_is_zero_ether_addr(&fsm_pool_users[0].ip_pool.mac_addr),
        "declined slot MAC bindings are cleared", NULL);
    TEST_ASSERT(rte_timer_pending(&fsm_pool_users[0].lan_user_info.timer),
        "conflict quarantine timer is armed", NULL);
}

static void test_invalid_event_and_state(void)
{
    printf("\nTesting invalid event/state handling:\n");
    printf("=====================================\n\n");

    fsm_env_reset(S_DHCP_INIT);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_RELEASE) == ERROR,
        "INIT + E_RELEASE is rejected", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_INIT,
        "state is unchanged after rejected event", "got state %u", fsm_state());

    fsm_env_reset(S_DHCP_OFFER_SENT);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_DECLINE) == ERROR,
        "OFFER_SENT + E_DECLINE is rejected", NULL);
    TEST_ASSERT(fsm_state() == S_DHCP_OFFER_SENT,
        "state is unchanged after rejected DECLINE", "got state %u", fsm_state());

    fsm_env_reset(S_DHCP_INVLD);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_DISCOVER) == ERROR,
        "unknown state S_DHCP_INVLD is rejected", NULL);

    fsm_env_reset(0);
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_DISCOVER) == ERROR,
        "zeroed (uninitialized) state is rejected", NULL);
}

static void test_handler_failure_propagates(void)
{
    printf("\nTesting handler failure propagation:\n");
    printf("====================================\n\n");

    /* Exhaust the pool: every slot used with a foreign MAC so
     * pick_ip_from_pool finds neither a match nor a free slot. */
    fsm_env_reset(S_DHCP_INIT);
    for(int i=0; i<FSM_TEST_POOL_LEN; i++) {
        fsm_pool_users[i].ip_pool.used = TRUE;
        fsm_pool_users[i].ip_pool.mac_addr.addr_bytes[0] = 0x02;
        fsm_pool_users[i].ip_pool.mac_addr.addr_bytes[5] = (U8)i;
    }
    TEST_ASSERT(dhcp_fsm(&fsm_dhcp_ccb, 0, E_DISCOVER) == ERROR,
        "E_DISCOVER with exhausted IP pool returns ERROR", NULL);
}

void test_dhcp_fsm(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           DHCP FSM Unit Tests                              ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    g_fastrg_ccb = fastrg_ccb;
    /* Handlers arm rte_timers on lcore.ctrl_thread — point it at the EAL
     * main lcore and make sure the timer subsystem exists so armed/stopped
     * assertions are meaningful. */
    fastrg_ccb->lcore.ctrl_thread = rte_lcore_id();
    rte_timer_subsystem_init();

    test_init_state_transitions();
    test_offer_sent_transitions();
    test_ack_sent_transitions();
    test_decline_marks_ip_conflicted();
    test_invalid_event_and_state();
    test_handler_failure_propagates();

    /* Leave no armed timer behind for later suites */
    for(int i=0; i<FSM_TEST_POOL_LEN; i++)
        rte_timer_stop_sync(&fsm_pool_users[i].lan_user_info.timer);

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
