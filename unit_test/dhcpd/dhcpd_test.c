#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int test_count = 0;
static int pass_count = 0;

#undef BOOT_REQUEST
#undef BOOT_REPLY
#define BOOT_REQUEST    0x1
#define BOOT_REPLY      0x2

typedef struct dhcp_opt {
    U8 opt_type;
    U8 len;
    U8 val[0];
} dhcp_opt_t;

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

/*
 * dhcpd() resolves ccb_id through DHCPD_GET_CCB, so the fixture must be
 * installed at fastrg_ccb->dhcp_ccb[0] rather than passed as a parameter.
 * The original pointer is captured and restored around the suite so later
 * suites (e.g. dnsd's TCP query tests) see the untouched shared ccb.
 */
#define DHCPD_TEST_POOL_LEN 3

static U8 dhcpd_pkt_buf[2048];
static dhcp_ccb_per_lan_user_t dhcpd_pool_users[DHCPD_TEST_POOL_LEN];
static dhcp_ccb_per_lan_user_t *dhcpd_pool_array[DHCPD_TEST_POOL_LEN];
static dhcp_ccb_t dhcpd_ccb;
static FastRG_t *g_dhcpd_fastrg_ccb;
static BOOL dhcpd_env_initialized = FALSE;

static struct rte_ether_hdr *g_eth_hdr;
static vlan_header_t *g_vlan_hdr;
static struct rte_ipv4_hdr *g_ip_hdr;
static struct rte_udp_hdr *g_udp_hdr;
static dhcp_hdr_t *g_dhcp_hdr;

/**
 * @fn dhcpd_env_reset
 *
 * @brief rebuild the dhcpd_ccb + pool fixture: DHCPD_TEST_POOL_LEN free
 *      lan_user_info slots, each in S_DHCP_INIT (mirroring what
 *      dhcp_pool_init_by_user leaves behind), enabled (dhcp_bool=1)
 * @return
 *      void
 */
static void dhcpd_env_reset(void)
{
    if (dhcpd_env_initialized) {
        for (int i = 0; i < DHCPD_TEST_POOL_LEN; i++)
            rte_timer_stop_sync(&dhcpd_pool_users[i].lan_user_info.timer);
    }
    dhcpd_env_initialized = TRUE;

    memset(dhcpd_pool_users, 0, sizeof(dhcpd_pool_users));
    memset(&dhcpd_ccb, 0, sizeof(dhcpd_ccb));

    dhcpd_ccb.dhcp_server_ip = rte_cpu_to_be_32(0xC0A80201); /* 192.168.2.1 */
    dhcpd_ccb.subnet_mask = rte_cpu_to_be_32(0xFFFFFF00);
    dhcpd_ccb.per_lan_user_pool = dhcpd_pool_array;
    dhcpd_ccb.per_lan_user_pool_len = DHCPD_TEST_POOL_LEN;
    dhcpd_ccb.fastrg_ccb = g_dhcpd_fastrg_ccb;
    rte_atomic16_init(&dhcpd_ccb.dhcp_bool);
    rte_atomic16_set(&dhcpd_ccb.dhcp_bool, 1);
    rte_atomic32_init(&dhcpd_ccb.active_count);

    for (int i = 0; i < DHCPD_TEST_POOL_LEN; i++) {
        dhcpd_pool_array[i] = &dhcpd_pool_users[i];
        dhcpd_pool_users[i].dhcp_ccb = &dhcpd_ccb;
        dhcpd_pool_users[i].pool_index = i;
        dhcpd_pool_users[i].ip_pool.ip_addr = rte_cpu_to_be_32(0xC0A80210 + i);
        dhcpd_pool_users[i].lan_user_info.state = S_DHCP_INIT;
        rte_timer_init(&dhcpd_pool_users[i].lan_user_info.timer);
    }

    g_dhcpd_fastrg_ccb->dhcp_ccb[0] = &dhcpd_ccb;
}

/* Builds a fresh eth/vlan/ip/udp/dhcp header chain for client_mac into
 * dhcpd_pkt_buf and points the g_* pointers at it. Options are left empty
 * for the caller to fill via dhcpd_set_options. */
static void mock_dhcpd_build_hdrs(const struct rte_ether_addr *client_mac)
{
    memset(dhcpd_pkt_buf, 0, sizeof(dhcpd_pkt_buf));

    g_eth_hdr = (struct rte_ether_hdr *)dhcpd_pkt_buf;
    rte_ether_addr_copy(client_mac, &g_eth_hdr->src_addr);
    g_eth_hdr->ether_type = rte_cpu_to_be_16(VLAN);

    g_vlan_hdr = (vlan_header_t *)(g_eth_hdr + 1);
    g_vlan_hdr->tci_union.tci_value = rte_cpu_to_be_16(0x0064);
    g_vlan_hdr->next_proto = rte_cpu_to_be_16(ETH_P_IP);

    g_ip_hdr = (struct rte_ipv4_hdr *)(g_vlan_hdr + 1);
    g_ip_hdr->version_ihl = 0x45;
    g_ip_hdr->time_to_live = 64;
    g_ip_hdr->next_proto_id = IPPROTO_UDP;

    g_udp_hdr = (struct rte_udp_hdr *)(g_ip_hdr + 1);

    g_dhcp_hdr = (dhcp_hdr_t *)(g_udp_hdr + 1);
    g_dhcp_hdr->msg_type = BOOT_REQUEST;
    g_dhcp_hdr->hwr_type = 1;
    g_dhcp_hdr->hwr_addr_len = 6;
    g_dhcp_hdr->transaction_id = rte_cpu_to_be_32(0x12345678);
    g_dhcp_hdr->mac_addr = *client_mac;
    g_dhcp_hdr->magic_cookie = rte_cpu_to_be_32(DHCP_MAGIC_COOKIE);
}

/* Writes `opts` right after g_dhcp_hdr and sets udp_hdr->dgram_len to match. */
static void dhcpd_set_options(const U8 *opts, U16 opt_len)
{
    memcpy((U8 *)(g_dhcp_hdr + 1), opts, opt_len);
    g_udp_hdr->dgram_len = rte_cpu_to_be_16(
        sizeof(struct rte_udp_hdr) + sizeof(dhcp_hdr_t) + opt_len);
}

static int dhcpd_call(U16 ccb_id)
{
    return dhcpd(g_dhcpd_fastrg_ccb, NULL, g_eth_hdr, g_vlan_hdr, g_ip_hdr, g_udp_hdr, ccb_id);
}

/* Builds a minimal [DHCP_MSG_TYPE, 1, msg_type][END] packet from `mac` and
 * feeds it into dhcpd() on ccb_id 0 — nothing is transmitted anywhere. */
static int mock_dhcpd_process_client_msg(const struct rte_ether_addr *mac, U8 msg_type)
{
    mock_dhcpd_build_hdrs(mac);
    U8 opts[4] = {DHCP_MSG_TYPE, 1, msg_type, DHCP_END};
    dhcpd_set_options(opts, sizeof(opts));
    return dhcpd_call(0);
}

/* ---- release_lan_user ---- */

static void test_release_lan_user_direct(void)
{
    printf("\nTesting release_lan_user function:\n");
    printf("=========================================\n\n");

    dhcpd_env_reset();
    dhcp_ccb_per_lan_user_t *slot = &dhcpd_pool_users[0];
    slot->ip_pool.used = TRUE;
    slot->lan_user_info.lan_user_used = TRUE;
    slot->lan_user_info.state = S_DHCP_ACK_SENT;
    struct rte_ether_addr some_mac = {.addr_bytes = {0x02, 1, 2, 3, 4, 5}};
    rte_ether_addr_copy(&some_mac, &slot->lan_user_info.mac_addr);
    rte_timer_reset(&slot->lan_user_info.timer, 1000000, SINGLE,
        g_dhcpd_fastrg_ccb->lcore.ctrl_thread, (rte_timer_cb_t)release_lan_user, slot);
    TEST_ASSERT(rte_timer_pending(&slot->lan_user_info.timer),
        "timer armed before release_lan_user runs", NULL);

    release_lan_user(&slot->lan_user_info.timer, slot);

    TEST_ASSERT(!rte_timer_pending(&slot->lan_user_info.timer),
        "release_lan_user stops the timer", NULL);
    TEST_ASSERT(slot->ip_pool.used == FALSE,
        "release_lan_user clears ip_pool.used", NULL);
    TEST_ASSERT(slot->lan_user_info.lan_user_used == FALSE,
        "release_lan_user clears lan_user_used", NULL);
    TEST_ASSERT(rte_is_zero_ether_addr(&slot->lan_user_info.mac_addr),
        "release_lan_user zeroes the bound MAC", NULL);
    TEST_ASSERT(slot->lan_user_info.state == S_DHCP_INIT,
        "release_lan_user resets state to S_DHCP_INIT", NULL);
}

/* ---- dhcpd() tests ---- */

static void test_dhcpd_ccb_id_out_of_range(void)
{
    printf("\nTesting dhcpd (ccb_id out of range):\n");
    printf("=========================================\n\n");

    dhcpd_env_reset();
    struct rte_ether_addr mac_a = {.addr_bytes = {0x02, 0, 0, 0, 0, 0x01}};
    mock_dhcpd_build_hdrs(&mac_a);
    U8 opts[4] = {DHCP_MSG_TYPE, 1, DHCP_DISCOVER, DHCP_END};
    dhcpd_set_options(opts, sizeof(opts));

    int ret = dhcpd_call(g_dhcpd_fastrg_ccb->user_count); /* one past valid range */
    TEST_ASSERT(ret == -1, "ccb_id >= user_count returns -1", "got %d", ret);
}

static void test_dhcpd_disabled_subscriber(void)
{
    printf("\nTesting dhcpd (subscriber DHCP disabled):\n");
    printf("=========================================\n\n");

    dhcpd_env_reset();
    rte_atomic16_set(&dhcpd_ccb.dhcp_bool, 0);
    struct rte_ether_addr mac_a = {.addr_bytes = {0x02, 0, 0, 0, 0, 0x01}};

    int ret = mock_dhcpd_process_client_msg(&mac_a, DHCP_DISCOVER);
    TEST_ASSERT(ret == -1, "disabled subscriber (dhcp_bool=0) returns -1", "got %d", ret);
    TEST_ASSERT(rte_atomic32_read(&dhcpd_ccb.active_count) == 0,
        "active_count balanced back to 0", "got %d", rte_atomic32_read(&dhcpd_ccb.active_count));
}

static void test_dhcpd_pool_exhausted(void)
{
    printf("\nTesting dhcpd (IP pool exhausted):\n");
    printf("=========================================\n\n");

    dhcpd_env_reset();
    for(int i=0; i<DHCPD_TEST_POOL_LEN; i++) {
        dhcpd_pool_users[i].lan_user_info.lan_user_used = TRUE;
        dhcpd_pool_users[i].lan_user_info.mac_addr.addr_bytes[0] = 0x02;
        dhcpd_pool_users[i].lan_user_info.mac_addr.addr_bytes[5] = (U8)(i + 1);
    }
    struct rte_ether_addr mac_new = {.addr_bytes = {0x02, 0, 0, 0, 0, 0xEE}};

    int ret = mock_dhcpd_process_client_msg(&mac_new, DHCP_DISCOVER);
    TEST_ASSERT(ret == -1,
        "no free/matching lan_user_info entry returns -1 (pool exhausted)", "got %d", ret);
    TEST_ASSERT(rte_atomic32_read(&dhcpd_ccb.active_count) == 0,
        "active_count balanced back to 0", "");
}

static void test_dhcpd_fresh_client_binds_slot(void)
{
    printf("\nTesting dhcpd (fresh client binds a free slot):\n");
    printf("=========================================\n\n");

    dhcpd_env_reset();
    struct rte_ether_addr mac_a = {.addr_bytes = {0x02, 0, 0, 0, 0, 0x01}};

    int ret = mock_dhcpd_process_client_msg(&mac_a, DHCP_DISCOVER);
    TEST_ASSERT(ret == 1, "successful DISCOVER returns 1", "got %d", ret);
    TEST_ASSERT(rte_is_same_ether_addr(&dhcpd_pool_users[0].lan_user_info.mac_addr, &mac_a),
        "first free slot (index 0) bound to the client MAC", "");
    TEST_ASSERT(dhcpd_pool_users[0].lan_user_info.lan_user_used == TRUE,
        "slot marked lan_user_used", "");
    TEST_ASSERT(dhcpd_pool_users[0].lan_user_info.state == S_DHCP_OFFER_SENT,
        "dhcp_fsm advanced state to S_DHCP_OFFER_SENT", "got %u",
        dhcpd_pool_users[0].lan_user_info.state);
    dhcp_opt_t *opt = (dhcp_opt_t *)(g_dhcp_hdr + 1);
    TEST_ASSERT(g_dhcp_hdr->msg_type == BOOT_REPLY && opt->opt_type == DHCP_MSG_TYPE &&
        opt->val[0] == DHCP_OFFER, "reply built in place is a DHCPOFFER", "");
    TEST_ASSERT(rte_atomic32_read(&dhcpd_ccb.active_count) == 0,
        "active_count balanced back to 0", "");
}

static void test_dhcpd_returning_client_reuses_slot(void)
{
    printf("\nTesting dhcpd (returning client reuses its own slot):\n");
    printf("=========================================\n\n");

    dhcpd_env_reset();
    struct rte_ether_addr mac_a = {.addr_bytes = {0x02, 0, 0, 0, 0, 0x01}};

    int ret1 = mock_dhcpd_process_client_msg(&mac_a, DHCP_DISCOVER);
    TEST_ASSERT(ret1 == 1, "first DISCOVER succeeds", "got %d", ret1);

    int ret2 = mock_dhcpd_process_client_msg(&mac_a, DHCP_DISCOVER);
    TEST_ASSERT(ret2 == 1, "second DISCOVER from the same MAC succeeds", "got %d", ret2);
    TEST_ASSERT(rte_is_same_ether_addr(&dhcpd_pool_users[0].lan_user_info.mac_addr, &mac_a),
        "same MAC still resolves to slot 0", "");
    TEST_ASSERT(dhcpd_pool_users[1].lan_user_info.lan_user_used == FALSE,
        "slot 1 was never touched -- no new slot allocated for a returning client", "");
}

static void test_dhcpd_decode_failure_releases_slot(void)
{
    printf("\nTesting dhcpd (dhcp_decode failure releases the claimed slot):\n");
    printf("=========================================\n\n");

    dhcpd_env_reset();
    struct rte_ether_addr mac_a = {.addr_bytes = {0x02, 0, 0, 0, 0, 0x01}};
    mock_dhcpd_build_hdrs(&mac_a);
    g_dhcp_hdr->magic_cookie = rte_cpu_to_be_32(0xDEADBEEF); /* forces dhcp_decode() to ERROR */
    U8 opts[4] = {DHCP_MSG_TYPE, 1, DHCP_DISCOVER, DHCP_END};
    dhcpd_set_options(opts, sizeof(opts));

    int ret = dhcpd_call(0);
    TEST_ASSERT(ret == -1, "dhcp_decode failure (bad magic cookie) returns -1", "got %d", ret);
    TEST_ASSERT(dhcpd_pool_users[0].lan_user_info.lan_user_used == FALSE,
        "slot claimed-then-released back to unused after decode failure", "");
    TEST_ASSERT(rte_is_zero_ether_addr(&dhcpd_pool_users[0].lan_user_info.mac_addr),
        "released slot's MAC is cleared", "");
    TEST_ASSERT(rte_atomic32_read(&dhcpd_ccb.active_count) == 0,
        "active_count balanced back to 0", "");
}

static void test_dhcpd_isp_id_event_releases_slot(void)
{
    printf("\nTesting dhcpd (ISP ID option short-circuit releases the claimed slot):\n");
    printf("=========================================\n\n");

    dhcpd_env_reset();
    struct rte_ether_addr mac_a = {.addr_bytes = {0x02, 0, 0, 0, 0, 0x01}};
    mock_dhcpd_build_hdrs(&mac_a);
    U8 opts[] = {DHCP_ISP_ID, 3, 'i', 's', 'p', DHCP_MSG_TYPE, 1, DHCP_DISCOVER, DHCP_END};
    dhcpd_set_options(opts, sizeof(opts));

    int ret = dhcpd_call(0);
    TEST_ASSERT(ret == 0, "ISP ID option short-circuit (event 0) returns 0", "got %d", ret);
    TEST_ASSERT(dhcpd_pool_users[0].lan_user_info.lan_user_used == FALSE,
        "slot released back to unused for the no-event case", "");
}

static void test_dhcpd_fsm_failure_releases_slot(void)
{
    printf("\nTesting dhcpd (dhcp_fsm failure releases the claimed slot):\n");
    printf("=========================================\n\n");

    dhcpd_env_reset();
    struct rte_ether_addr mac_a = {.addr_bytes = {0x02, 0, 0, 0, 0, 0x01}};

    /* S_DHCP_INIT has no E_RELEASE row in dhcp_fsm_tbl -> dhcp_fsm() returns ERROR */
    int ret = mock_dhcpd_process_client_msg(&mac_a, DHCP_RELEASE);
    TEST_ASSERT(ret == -1, "dhcp_fsm ERROR (undefined state/event) returns -1", "got %d", ret);
    TEST_ASSERT(dhcpd_pool_users[0].lan_user_info.lan_user_used == FALSE,
        "slot released back to unused after fsm failure", "");
    TEST_ASSERT(dhcpd_pool_users[0].ip_pool.used == FALSE,
        "released slot's ip_pool.used cleared too", "");
    TEST_ASSERT(dhcpd_pool_users[0].lan_user_info.state == S_DHCP_INIT,
        "state reset to S_DHCP_INIT by release_lan_user", "");
}

void test_dhcpd(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           DHCPD Unit Tests                                 ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    g_dhcpd_fastrg_ccb = fastrg_ccb;
    dhcp_ccb_t *orig_ccb0 = fastrg_ccb->dhcp_ccb[0];

    /* Handlers arm rte_timers on lcore.ctrl_thread -- point it at the EAL
     * main lcore and make sure the timer subsystem exists. */
    fastrg_ccb->lcore.ctrl_thread = rte_lcore_id();
    rte_timer_subsystem_init();

    test_release_lan_user_direct();
    test_dhcpd_ccb_id_out_of_range();
    test_dhcpd_disabled_subscriber();
    test_dhcpd_pool_exhausted();
    test_dhcpd_fresh_client_binds_slot();
    test_dhcpd_returning_client_reuses_slot();
    test_dhcpd_decode_failure_releases_slot();
    test_dhcpd_isp_id_event_releases_slot();
    test_dhcpd_fsm_failure_releases_slot();

    /* Leave no armed timer behind for later suites, and restore the shared
     * ccb slot other suites (e.g. dnsd) expect to find untouched. */
    for(int i=0; i<DHCPD_TEST_POOL_LEN; i++)
        rte_timer_stop_sync(&dhcpd_pool_users[i].lan_user_info.timer);
    fastrg_ccb->dhcp_ccb[0] = orig_ccb0;

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
