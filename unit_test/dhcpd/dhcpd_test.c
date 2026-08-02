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

static void test_dhcpd_udp_length_exceeds_packet(void)
{
    printf("\nTesting dhcpd (UDP length exceeds actual packet):\n");
    printf("=========================================\n\n");

    dhcpd_env_reset();
    struct rte_ether_addr mac_a = {.addr_bytes = {0x02, 0, 0, 0, 0, 0x01}};
    mock_dhcpd_build_hdrs(&mac_a);
    U8 opts[4] = {DHCP_MSG_TYPE, 1, DHCP_DISCOVER, DHCP_END};
    dhcpd_set_options(opts, sizeof(opts));

    struct rte_mbuf mock_pkt = {
        .buf_addr = dhcpd_pkt_buf,
        .data_off = 0,
        .pkt_len = (U32)((U8 *)(g_dhcp_hdr + 1) + sizeof(opts) - dhcpd_pkt_buf),
    };
    g_udp_hdr->dgram_len = rte_cpu_to_be_16(
        rte_be_to_cpu_16(g_udp_hdr->dgram_len) + 1);

    int ret = dhcpd(g_dhcpd_fastrg_ccb, &mock_pkt, g_eth_hdr, g_vlan_hdr,
        g_ip_hdr, g_udp_hdr, 0);
    TEST_ASSERT(ret == -1, "oversized UDP datagram returns -1", "got %d", ret);
    TEST_ASSERT(rte_atomic32_read(&dhcpd_ccb.active_count) == 0,
        "oversized UDP datagram balances active_count back to 0", "got %d",
        rte_atomic32_read(&dhcpd_ccb.active_count));
    TEST_ASSERT(dhcpd_pool_users[0].lan_user_info.lan_user_used == FALSE,
        "oversized UDP datagram is rejected before dhcp_decode claims a slot", "");
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


/* ---- fixed-max refactor case (moved from the task-scoped file into the
 * module test file per repo naming convention; additive only) ---- */

#define FM_DHCP_POOL_FIXTURE_LEN 4

static dhcp_ccb_per_lan_user_t fm_pool_users[FM_DHCP_POOL_FIXTURE_LEN];
static dhcp_ccb_per_lan_user_t *fm_pool_array[FM_DHCP_POOL_FIXTURE_LEN];

static void test_dhcp_pool_window_reinit(void)
{
    printf("\nTesting dhcp_pool_init_by_user (window re-init + over-cap reject):\n");
    printf("=========================================\n\n");

    static dhcp_ccb_t fm_dhcp_ccb;
    memset(&fm_dhcp_ccb, 0, sizeof(fm_dhcp_ccb));
    memset(fm_pool_users, 0, sizeof(fm_pool_users));
    fm_dhcp_ccb.fastrg_ccb = g_dhcpd_fastrg_ccb;
    fm_dhcp_ccb.per_lan_user_pool = fm_pool_array;
    for(int i=0; i<FM_DHCP_POOL_FIXTURE_LEN; i++) {
        fm_pool_array[i] = &fm_pool_users[i];
        rte_timer_init(&fm_pool_users[i].lan_user_info.timer);
    }

    /* configure a 3-address window inside the fixture capacity */
    U32 gw = rte_cpu_to_be_32(0xC0A80201);
    U32 start = rte_cpu_to_be_32(0xC0A80210);
    U32 end = rte_cpu_to_be_32(0xC0A80212);
    U32 mask = rte_cpu_to_be_32(0xFFFFFF00);
    TEST_ASSERT(dhcp_pool_init_by_user(&fm_dhcp_ccb, gw, start, end, mask) == SUCCESS,
        "in-capacity pool window is accepted", "");
    TEST_ASSERT(fm_dhcp_ccb.per_lan_user_pool_len == 3,
        "pool window sized to configured range", "got %u",
        fm_dhcp_ccb.per_lan_user_pool_len);
    TEST_ASSERT(fm_pool_users[0].ip_pool.ip_addr == start &&
        fm_pool_users[2].ip_pool.ip_addr == end,
        "window entries carry sequential pool IPs", "");
    TEST_ASSERT(fm_pool_users[0].pool_index == 0 && fm_pool_users[2].pool_index == 2,
        "window entries re-bound to their indices", "");

    /* re-init with a smaller window: lease state is scrubbed over
     * max(old,new), so the trimmed entry is cleaned too */
    fm_pool_users[0].ip_pool.used = TRUE;
    fm_pool_users[0].lan_user_info.lan_user_used = TRUE;
    fm_pool_users[2].ip_pool.used = TRUE;
    U32 start2 = rte_cpu_to_be_32(0xC0A80220);
    U32 end2 = rte_cpu_to_be_32(0xC0A80221);
    TEST_ASSERT(dhcp_pool_init_by_user(&fm_dhcp_ccb, gw, start2, end2, mask) == SUCCESS,
        "shrinking pool window is accepted", "");
    TEST_ASSERT(fm_dhcp_ccb.per_lan_user_pool_len == 2,
        "window shrinks with the new range", "got %u",
        fm_dhcp_ccb.per_lan_user_pool_len);
    TEST_ASSERT(fm_pool_users[0].ip_pool.used == FALSE &&
        fm_pool_users[0].lan_user_info.lan_user_used == FALSE,
        "window re-init clears per-entry lease state", "");
    TEST_ASSERT(fm_pool_users[0].ip_pool.ip_addr == start2,
        "window re-init rewrites entry IPs", "");
    TEST_ASSERT(fm_pool_users[2].ip_pool.used == FALSE &&
        fm_pool_users[2].ip_pool.ip_addr == 0,
        "trimmed beyond-window entry is scrubbed (max(old,new) reset)", "");

    /* over-capacity request (> fixed per-user capacity 1<<17) is rejected
     * without touching the current window */
    U32 huge_start = rte_cpu_to_be_32(0x0A000001);
    U32 huge_end = rte_cpu_to_be_32(0x0A040001); /* 262145 addresses */
    U32 before_len = fm_dhcp_ccb.per_lan_user_pool_len;
    U32 before_ip = fm_pool_users[0].ip_pool.ip_addr;
    TEST_ASSERT(dhcp_pool_init_by_user(&fm_dhcp_ccb, gw, huge_start, huge_end, mask) == ERROR,
        "over-capacity pool request returns ERROR", "");
    TEST_ASSERT(fm_dhcp_ccb.per_lan_user_pool_len == before_len,
        "over-capacity pool request leaves window length untouched", "got %u",
        fm_dhcp_ccb.per_lan_user_pool_len);
    TEST_ASSERT(fm_pool_users[0].ip_pool.ip_addr == before_ip,
        "over-capacity pool request leaves window contents untouched", "");
}

/* ---- pool range validation + .0/.255 reservation (additive only) ---- */

#define RSV_DHCP_POOL_FIXTURE_LEN 8

static dhcp_ccb_per_lan_user_t rsv_pool_users[RSV_DHCP_POOL_FIXTURE_LEN];
static dhcp_ccb_per_lan_user_t *rsv_pool_array[RSV_DHCP_POOL_FIXTURE_LEN];

/**
 * @fn test_dhcp_pool_reserved_entries
 *
 * @brief a pool range crossing an octet boundary must keep its index <-> IP
 *      mapping but flag the network (.0) and broadcast (.255) addresses as
 *      reserved so they are never leased
 * @return
 *      void
 */
static void test_dhcp_pool_reserved_entries(void)
{
    printf("\nTesting dhcp_pool_init_by_user (.0/.255 reservation):\n");
    printf("=========================================\n\n");

    static dhcp_ccb_t rsv_dhcp_ccb;
    memset(&rsv_dhcp_ccb, 0, sizeof(rsv_dhcp_ccb));
    memset(rsv_pool_users, 0, sizeof(rsv_pool_users));
    rsv_dhcp_ccb.fastrg_ccb = g_dhcpd_fastrg_ccb;
    rsv_dhcp_ccb.per_lan_user_pool = rsv_pool_array;
    for(int i=0; i<RSV_DHCP_POOL_FIXTURE_LEN; i++) {
        rsv_pool_array[i] = &rsv_pool_users[i];
        rte_timer_init(&rsv_pool_users[i].lan_user_info.timer);
    }

    /* 192.168.5.253 ~ 192.168.6.4 -> 8 addresses, two of them unusable */
    U32 gw = rte_cpu_to_be_32(0xC0A80501);
    U32 start = rte_cpu_to_be_32(0xC0A805FD);
    U32 end = rte_cpu_to_be_32(0xC0A80604);
    U32 mask = rte_cpu_to_be_32(0xFFFFFF00);

    TEST_ASSERT(dhcp_pool_init_by_user(&rsv_dhcp_ccb, gw, start, end, mask) == SUCCESS,
        "cross-octet pool window is accepted", "");
    TEST_ASSERT(rsv_dhcp_ccb.per_lan_user_pool_len == RSV_DHCP_POOL_FIXTURE_LEN,
        "cross-octet window covers every address in the range", "got %u",
        rsv_dhcp_ccb.per_lan_user_pool_len);
    TEST_ASSERT(rsv_pool_users[0].ip_pool.ip_addr == start &&
        rsv_pool_users[RSV_DHCP_POOL_FIXTURE_LEN - 1].ip_pool.ip_addr == end,
        "cross-octet window keeps the index <-> IP mapping", "");
    TEST_ASSERT(rsv_pool_users[2].ip_pool.ip_addr == rte_cpu_to_be_32(0xC0A805FF) &&
        rsv_pool_users[2].ip_pool.reserved == TRUE,
        "broadcast address 192.168.5.255 is reserved", "");
    TEST_ASSERT(rsv_pool_users[3].ip_pool.ip_addr == rte_cpu_to_be_32(0xC0A80600) &&
        rsv_pool_users[3].ip_pool.reserved == TRUE,
        "network address 192.168.6.0 is reserved", "");

    BOOL others_assignable = TRUE;
    for(int i=0; i<RSV_DHCP_POOL_FIXTURE_LEN; i++) {
        if (i == 2 || i == 3)
            continue;
        if (rsv_pool_users[i].ip_pool.reserved != FALSE)
            others_assignable = FALSE;
    }
    TEST_ASSERT(others_assignable, "every other address in the range stays assignable", "");

    /* a range fully inside one /24 reserves nothing */
    U32 plain_start = rte_cpu_to_be_32(0xC0A80510);
    U32 plain_end = rte_cpu_to_be_32(0xC0A80512);
    TEST_ASSERT(dhcp_pool_init_by_user(&rsv_dhcp_ccb, gw, plain_start, plain_end, mask) == SUCCESS,
        "in-subnet pool window is accepted", "");
    TEST_ASSERT(rsv_pool_users[0].ip_pool.reserved == FALSE &&
        rsv_pool_users[1].ip_pool.reserved == FALSE &&
        rsv_pool_users[2].ip_pool.reserved == FALSE,
        "window re-init clears the reserved flag of a plain range", "");

    for(int i=0; i<RSV_DHCP_POOL_FIXTURE_LEN; i++)
        rte_timer_stop_sync(&rsv_pool_users[i].lan_user_info.timer);
}

/**
 * @fn test_dhcp_validate_pool_range
 *
 * @brief config-apply validation: the pool must be ordered and live inside
 *      the DHCP server subnet, and a rejected pool must leave the running
 *      window untouched
 * @return
 *      void
 */
static void test_dhcp_validate_pool_range(void)
{
    printf("\nTesting dhcp_validate_pool_range:\n");
    printf("=========================================\n\n");

    U32 gw = rte_cpu_to_be_32(0xC0A80401);       /* 192.168.4.1   */
    U32 mask = rte_cpu_to_be_32(0xFFFFFF00);
    U32 ok_start = rte_cpu_to_be_32(0xC0A80402); /* 192.168.4.2   */
    U32 ok_end = rte_cpu_to_be_32(0xC0A804FE);   /* 192.168.4.254 */

    TEST_ASSERT(dhcp_validate_pool_range(gw, ok_start, ok_end, mask) == SUCCESS,
        "range inside the server subnet is accepted", "");
    TEST_ASSERT(dhcp_validate_pool_range(0, 0, 0, 0) == SUCCESS,
        "all-zero pool clearing call is accepted", "");
    TEST_ASSERT(dhcp_validate_pool_range(gw, ok_end, ok_start, mask) == ERROR,
        "reversed range (start > end) is rejected", "");
    TEST_ASSERT(dhcp_validate_pool_range(gw, ok_start,
        rte_cpu_to_be_32(0xC0A80505), mask) == ERROR,
        "range ending outside the server subnet is rejected", "");
    TEST_ASSERT(dhcp_validate_pool_range(gw, rte_cpu_to_be_32(0xC0A80302),
        ok_end, mask) == ERROR,
        "range starting outside the server subnet is rejected", "");

    /* config-apply order: validation runs first, so a rejected pool never
     * reaches dhcp_pool_init_by_user and the current window survives intact */
    static dhcp_ccb_t vr_dhcp_ccb;
    memset(&vr_dhcp_ccb, 0, sizeof(vr_dhcp_ccb));
    memset(rsv_pool_users, 0, sizeof(rsv_pool_users));
    vr_dhcp_ccb.fastrg_ccb = g_dhcpd_fastrg_ccb;
    vr_dhcp_ccb.per_lan_user_pool = rsv_pool_array;
    for(int i=0; i<RSV_DHCP_POOL_FIXTURE_LEN; i++) {
        rsv_pool_array[i] = &rsv_pool_users[i];
        rte_timer_init(&rsv_pool_users[i].lan_user_info.timer);
    }

    U32 live_start = rte_cpu_to_be_32(0xC0A80410);
    U32 live_end = rte_cpu_to_be_32(0xC0A80412);
    dhcp_pool_init_by_user(&vr_dhcp_ccb, gw, live_start, live_end, mask);
    rsv_pool_users[0].ip_pool.used = TRUE;

    U32 bad_start = rte_cpu_to_be_32(0xC0A805FD); /* 192.168.5.253, other /24 */
    U32 bad_end = rte_cpu_to_be_32(0xC0A80604);
    if (dhcp_validate_pool_range(gw, bad_start, bad_end, mask) == SUCCESS)
        dhcp_pool_init_by_user(&vr_dhcp_ccb, gw, bad_start, bad_end, mask);

    TEST_ASSERT(vr_dhcp_ccb.per_lan_user_pool_len == 3,
        "rejected pool leaves the window length untouched", "got %u",
        vr_dhcp_ccb.per_lan_user_pool_len);
    TEST_ASSERT(rsv_pool_users[0].ip_pool.ip_addr == live_start &&
        rsv_pool_users[2].ip_pool.ip_addr == live_end,
        "rejected pool leaves the window contents untouched", "");
    TEST_ASSERT(rsv_pool_users[0].ip_pool.used == TRUE,
        "rejected pool leaves existing lease state untouched", "");

    for(int i=0; i<RSV_DHCP_POOL_FIXTURE_LEN; i++)
        rte_timer_stop_sync(&rsv_pool_users[i].lan_user_info.timer);
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
    test_dhcpd_udp_length_exceeds_packet();
    test_dhcpd_pool_exhausted();
    test_dhcpd_fresh_client_binds_slot();
    test_dhcpd_returning_client_reuses_slot();
    test_dhcpd_decode_failure_releases_slot();
    test_dhcpd_isp_id_event_releases_slot();
    test_dhcpd_fsm_failure_releases_slot();

    test_dhcp_pool_window_reinit();
    test_dhcp_pool_reserved_entries();
    test_dhcp_validate_pool_range();

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
