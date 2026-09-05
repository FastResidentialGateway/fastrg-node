#include <stdlib.h>
#include <string.h>

#include <common.h>

#include <rte_lcore.h>
#include <rte_ring.h>
#include <rte_timer.h>

#include "../src/northbound.h"
#include "../src/cli_request.h"
#include "../src/fastrg.h"
#include "../src/dhcpd/dhcpd.h"
#include "../src/pppd/pppd.h"
#include "../src/pppd/nat.h"
#include "../src/pppd/ipv6_firewall.h"
#include "../src/dnsd/dns_static.h"
#include "test_helper.h"

// Global test counters
static int test_count = 0;
static int pass_count = 0;

/** Verify that port_fwd_table[eport] is active and matches dip_str:dport. */
static BOOL table_entry_matches(port_fwd_entry_t *table, U16 eport,
    const char *dip_str, U16 dport)
{
    port_fwd_entry_t *e = &table[eport];
    if (rte_atomic16_read(&e->is_active) == 0)
        return FALSE;

    U32 dip_be;
    /* use inet_pton path via a simple manual parse */
    unsigned a, b, c, d;
    if (sscanf(dip_str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return FALSE;
    dip_be = rte_cpu_to_be_32((a << 24) | (b << 16) | (c << 8) | d);

    return (e->dip == dip_be && rte_be_to_cpu_16(e->iport) == dport) ? TRUE : FALSE;
}

/* -----------------------------------------------------------------------
 * reconcile_port_mapping unit tests
 * --------------------------------------------------------------------- */

void test_reconcile_port_mapping(FastRG_t *fastrg_ccb)
{
    printf("\nTesting reconcile_port_mapping function:\n");
    printf("=========================================\n\n");

    /* -----------------------------------------------------------------
     * Test 1: NULL fastrg_ccb  →  ERROR
     * --------------------------------------------------------------- */
    printf("Test 1: \"NULL fastrg_ccb returns ERROR\"\n");
    {
        port_mapping_t m = {.eport = 1000, .dip = "192.168.1.1", .dport = 80};
        STATUS r = reconcile_port_mapping(NULL, 0, &m, 1);
        TEST_ASSERT(r == ERROR, "NULL fastrg_ccb returns ERROR", "got SUCCESS");
    }

    /* -----------------------------------------------------------------
     * Test 2: invalid ccb_id (no ppp_ccb set up)  →  ERROR
     * --------------------------------------------------------------- */
    printf("Test 2: \"no ppp_ccb set up → is_valid_ccb_id fails → ERROR\"\n");
    {
        STATUS r = reconcile_port_mapping(fastrg_ccb, 1, NULL, 0);
        TEST_ASSERT(r == ERROR, "uninitialized ccb returns ERROR", "got SUCCESS");
    }

    /* -----------------------------------------------------------------
     * Test 3: empty mappings, empty local table  →  SUCCESS, 0 changes
     * --------------------------------------------------------------- */
    printf("Test 3: \"empty mappings + empty local → SUCCESS, no changes\"\n");
    {
        STATUS r = reconcile_port_mapping(fastrg_ccb, 0, NULL, 0);
        TEST_ASSERT(r == SUCCESS, "empty reconcile returns SUCCESS", "got ERROR");
        /* Verify no entry is active */
        ppp_ccb_t *ppp = fastrg_ccb->ppp_ccb[0];
        BOOL any_active = FALSE;
        for(U32 ep=0; ep<PORT_FWD_TABLE_SIZE; ep++) {
            if (rte_atomic16_read(&ppp->port_fwd_table[ep].is_active))
                any_active = TRUE;
        }
        TEST_ASSERT(!any_active, "no port-fwd entry active after empty reconcile",
            "found active entry");
    }

    /* -----------------------------------------------------------------
     * Test 4: etcd has 2 entries, local is empty  →  both entries added
     * --------------------------------------------------------------- */
    printf("Test 4: \"etcd has entries, local empty → entries added\"\n");
    {
        port_mapping_t mappings[] = {
            {.eport = 8080, .dip = "192.168.1.100", .dport = 80},
            {.eport = 9090, .dip = "10.0.0.1",      .dport = 9000},
        };
        STATUS r = reconcile_port_mapping(fastrg_ccb, 0, mappings, 2);
        TEST_ASSERT(r == SUCCESS, "add-only reconcile returns SUCCESS", "got ERROR");
        ppp_ccb_t *ppp = fastrg_ccb->ppp_ccb[0];
        TEST_ASSERT(table_entry_matches(ppp->port_fwd_table, 8080, "192.168.1.100", 80),
            "eport 8080 added correctly", "entry missing or wrong");
        TEST_ASSERT(table_entry_matches(ppp->port_fwd_table, 9090, "10.0.0.1", 9000),
            "eport 9090 added correctly", "entry missing or wrong");
    }

    /* -----------------------------------------------------------------
     * Test 5: local has entries, etcd is empty  →  all entries removed
     * --------------------------------------------------------------- */
    printf("Test 5: \"local has entries, etcd empty → entries removed\"\n");
    {
        ppp_ccb_t *ppp = fastrg_ccb->ppp_ccb[0];
        /* Pre-populate local table */
        port_mapping_t pre[] = {
            {.eport = 1234, .dip = "192.168.2.10", .dport = 22},
        };
        reconcile_port_mapping(fastrg_ccb, 0, pre, 1);  /* add first */

        STATUS r = reconcile_port_mapping(fastrg_ccb, 0, NULL, 0);  /* now remove */
        TEST_ASSERT(r == SUCCESS, "remove-only reconcile returns SUCCESS", "got ERROR");
        TEST_ASSERT(rte_atomic16_read(&ppp->port_fwd_table[1234].is_active) == 0,
            "eport 1234 removed after empty reconcile", "still active");
    }

    /* -----------------------------------------------------------------
     * Test 6: exact match  →  SUCCESS, entry unchanged (no-op)
     * --------------------------------------------------------------- */
    printf("Test 6: \"exact match → no-op\"\n");
    {
        port_mapping_t m = {.eport = 443, .dip = "172.16.0.5", .dport = 8443};
        reconcile_port_mapping(fastrg_ccb, 0, &m, 1);          /* initial add */
        STATUS r = reconcile_port_mapping(fastrg_ccb, 0, &m, 1); /* reconcile same */
        TEST_ASSERT(r == SUCCESS, "exact-match reconcile returns SUCCESS", "got ERROR");
        ppp_ccb_t *ppp = fastrg_ccb->ppp_ccb[0];
        TEST_ASSERT(table_entry_matches(ppp->port_fwd_table, 443, "172.16.0.5", 8443),
            "eport 443 still correct after no-op", "entry wrong");
    }

    /* -----------------------------------------------------------------
     * Test 7: local has stale entry not in etcd  →  stale entry removed,
     *         new entry added
     * --------------------------------------------------------------- */
    printf("Test 7: \"stale local entry removed, new etcd entry added\"\n");
    {
        port_mapping_t old_m = {.eport = 5000, .dip = "10.0.0.2", .dport = 5001};
        reconcile_port_mapping(fastrg_ccb, 0, &old_m, 1);     /* seed local */

        port_mapping_t new_m = {.eport = 6000, .dip = "10.0.0.3", .dport = 6001};
        STATUS r = reconcile_port_mapping(fastrg_ccb, 0, &new_m, 1);
        TEST_ASSERT(r == SUCCESS, "stale+new reconcile returns SUCCESS", "got ERROR");
        ppp_ccb_t *ppp = fastrg_ccb->ppp_ccb[0];
        TEST_ASSERT(rte_atomic16_read(&ppp->port_fwd_table[5000].is_active) == 0,
            "stale eport 5000 removed", "still active");
        TEST_ASSERT(table_entry_matches(ppp->port_fwd_table, 6000, "10.0.0.3", 6001),
            "new eport 6000 added", "missing or wrong");
    }

    /* -----------------------------------------------------------------
     * Test 8: same eport with updated dip  →  entry updated  (bug fix)
     * --------------------------------------------------------------- */
    printf("Test 8: \"updated dip for same eport → entry updated (bug fix)\"\n");
    {
        port_mapping_t old_m = {.eport = 8080, .dip = "192.168.1.100", .dport = 9000};
        reconcile_port_mapping(fastrg_ccb, 0, &old_m, 1);     /* seed */

        /* etcd now says eport 8080 → 192.168.1.200:9000 (dip changed) */
        port_mapping_t new_m = {.eport = 8080, .dip = "192.168.1.200", .dport = 9000};
        STATUS r = reconcile_port_mapping(fastrg_ccb, 0, &new_m, 1);
        TEST_ASSERT(r == SUCCESS, "dip-update reconcile returns SUCCESS", "got ERROR");
        ppp_ccb_t *ppp = fastrg_ccb->ppp_ccb[0];
        TEST_ASSERT(table_entry_matches(ppp->port_fwd_table, 8080, "192.168.1.200", 9000),
            "eport 8080 updated to new dip", "still has old dip");
    }

    /* -----------------------------------------------------------------
     * Test 9: same eport with updated dport  →  entry updated  (bug fix)
     * --------------------------------------------------------------- */
    printf("Test 9: \"updated dport for same eport → entry updated (bug fix)\"\n");
    {
        port_mapping_t old_m = {.eport = 7070, .dip = "10.1.1.1", .dport = 7071};
        reconcile_port_mapping(fastrg_ccb, 0, &old_m, 1);     /* seed */

        /* etcd now says eport 7070 → 10.1.1.1:9999 (dport changed) */
        port_mapping_t new_m = {.eport = 7070, .dip = "10.1.1.1", .dport = 9999};
        STATUS r = reconcile_port_mapping(fastrg_ccb, 0, &new_m, 1);
        TEST_ASSERT(r == SUCCESS, "dport-update reconcile returns SUCCESS", "got ERROR");
        ppp_ccb_t *ppp = fastrg_ccb->ppp_ccb[0];
        TEST_ASSERT(table_entry_matches(ppp->port_fwd_table, 7070, "10.1.1.1", 9999),
            "eport 7070 updated to new dport", "still has old dport");
    }

    /* -----------------------------------------------------------------
     * Test 10: invalid dip string in mappings  →  ERROR, that entry skipped
     * --------------------------------------------------------------- */
    printf("Test 10: \"invalid dip in mapping → ERROR returned\"\n");
    {
        port_mapping_t bad = {.eport = 1111, .dip = "not.an.ip", .dport = 80};
        STATUS r = reconcile_port_mapping(fastrg_ccb, 0, &bad, 1);
        TEST_ASSERT(r == ERROR, "invalid dip returns ERROR", "got SUCCESS");
        ppp_ccb_t *ppp = fastrg_ccb->ppp_ccb[0];
        TEST_ASSERT(rte_atomic16_read(&ppp->port_fwd_table[1111].is_active) == 0,
            "bad-dip entry not inserted", "entry was inserted");
    }

    /* -----------------------------------------------------------------
     * Test 11: mixed — some add, some remove, some update, 1 invalid
     * --------------------------------------------------------------- */
    printf("Test 11: \"mixed: add + remove + update + invalid dip\"\n");
    {
        /* Seed local with two entries */
        port_mapping_t seed[] = {
            {.eport = 2000, .dip = "10.0.1.1", .dport = 2001},  /* will be removed */
            {.eport = 3000, .dip = "10.0.2.1", .dport = 3001},  /* will be updated */
        };
        reconcile_port_mapping(fastrg_ccb, 0, seed, 2);

        /* New etcd state: eport 2000 gone, 3000 updated, 4000 new, 5000 invalid */
        port_mapping_t etcd[] = {
            {.eport = 3000, .dip = "10.0.2.99", .dport = 3001},  /* update */
            {.eport = 4000, .dip = "10.0.3.1",  .dport = 4001},  /* add    */
            {.eport = 5000, .dip = "bad",        .dport = 5001},  /* error  */
        };
        STATUS r = reconcile_port_mapping(fastrg_ccb, 0, etcd, 3);
        TEST_ASSERT(r == ERROR, "mixed reconcile with invalid dip returns ERROR", "got SUCCESS");

        ppp_ccb_t *ppp = fastrg_ccb->ppp_ccb[0];
        TEST_ASSERT(rte_atomic16_read(&ppp->port_fwd_table[2000].is_active) == 0,
            "eport 2000 removed (not in etcd)", "still active");
        TEST_ASSERT(table_entry_matches(ppp->port_fwd_table, 3000, "10.0.2.99", 3001),
            "eport 3000 updated correctly", "wrong value");
        TEST_ASSERT(table_entry_matches(ppp->port_fwd_table, 4000, "10.0.3.1", 4001),
            "eport 4000 added correctly", "missing");
        TEST_ASSERT(rte_atomic16_read(&ppp->port_fwd_table[5000].is_active) == 0,
            "bad-dip eport 5000 not inserted", "was inserted");
    }

    printf("\n  All reconcile_port_mapping tests done.\n");
}

/* -----------------------------------------------------------------------
 * remove_hsi_config unit tests
 * --------------------------------------------------------------------- */

/*
 * remove_hsi_config() resolves both control blocks through PPPD_GET_CCB /
 * DHCPD_GET_CCB, so the fixture is installed at index 0 of the shared ccb and
 * the original pointers are restored at the end. ppp_cleanup_config_by_user()
 * flushes the NAT and IPv6-firewall tables, hence the real DPDK objects here.
 */
#define NB_REMOVE_VLAN_ID       1234
#define NB_REMOVE_NAT_ID        998
#define NB_REMOVE_FIREWALL_ID   60004

static ppp_ccb_t nb_remove_ppp_ccb;
static dhcp_ccb_t nb_remove_dhcp_ccb;
static dhcp_ccb_per_lan_user_t nb_remove_pool_user;
static dhcp_ccb_per_lan_user_t *nb_remove_pool_array[1];

void test_remove_hsi_config(FastRG_t *fastrg_ccb)
{
    printf("\nTesting remove_hsi_config function:\n");
    printf("=========================================\n\n");

    ppp_ccb_t *orig_ppp_ccb = fastrg_ccb->ppp_ccb[0];
    dhcp_ccb_t *orig_dhcp_ccb = fastrg_ccb->dhcp_ccb[0];
    rte_atomic16_t *orig_vlan_map = fastrg_ccb->vlan_userid_map;

    /* The shared mock leaves the VLAN map unallocated; remove_hsi_config
     * releases the subscriber's VLAN through it. */
    rte_atomic16_t *vlan_map = fastrg_calloc(rte_atomic16_t, MAX_VLAN_ID,
        sizeof(rte_atomic16_t), 0);
    TEST_ASSERT(vlan_map != NULL, "allocate the fixture VLAN map", "out of memory");
    if (vlan_map == NULL)
        return;
    for(U16 i=0; i<MAX_VLAN_ID; i++)
        rte_atomic16_set(&vlan_map[i], INVALID_CCB_ID);
    fastrg_ccb->vlan_userid_map = vlan_map;

    memset(&nb_remove_ppp_ccb, 0, sizeof(nb_remove_ppp_ccb));
    memset(&nb_remove_dhcp_ccb, 0, sizeof(nb_remove_dhcp_ccb));
    memset(&nb_remove_pool_user, 0, sizeof(nb_remove_pool_user));

    nb_remove_ppp_ccb.fastrg_ccb = fastrg_ccb;
    nb_remove_dhcp_ccb.fastrg_ccb = fastrg_ccb;
    nb_remove_pool_array[0] = &nb_remove_pool_user;
    nb_remove_pool_user.dhcp_ccb = &nb_remove_dhcp_ccb;
    nb_remove_dhcp_ccb.per_lan_user_pool = nb_remove_pool_array;
    rte_timer_init(&nb_remove_pool_user.lan_user_info.timer);

    if (nat_table_init(&nb_remove_ppp_ccb, NB_REMOVE_NAT_ID, fastrg_ccb->ppp_ccb_rcu) != SUCCESS ||
            ipv6_firewall_table_init(&nb_remove_ppp_ccb, NB_REMOVE_FIREWALL_ID,
                fastrg_ccb->ppp_ccb_rcu) != SUCCESS) {
        TEST_ASSERT(FALSE, "build the remove_hsi_config fixture",
            "NAT or IPv6 firewall table creation failed");
        fastrg_ccb->vlan_userid_map = orig_vlan_map;
        fastrg_mfree(vlan_map);
        return;
    }

    fastrg_ccb->ppp_ccb[0] = &nb_remove_ppp_ccb;
    fastrg_ccb->dhcp_ccb[0] = &nb_remove_dhcp_ccb;

    dns_static_init(&nb_remove_dhcp_ccb.dns_state.static_table);
    dns_static_add(&nb_remove_dhcp_ccb.dns_state.static_table, "static.fastrg.org",
        rte_cpu_to_be_32(0xC0000277), 60);

    TEST_ASSERT(remove_hsi_config(fastrg_ccb, -1) == ERROR,
        "an invalid ccb id returns ERROR", "got SUCCESS");

    /* vlan_id 0 means the subscriber was never configured. */
    TEST_ASSERT(remove_hsi_config(fastrg_ccb, 0) == ERROR,
        "an unconfigured subscriber returns ERROR", "got SUCCESS");

    rte_atomic16_set(&nb_remove_ppp_ccb.vlan_id, NB_REMOVE_VLAN_ID);
    rte_atomic16_set(&nb_remove_ppp_ccb.ppp_bool, 1);
    TEST_ASSERT(remove_hsi_config(fastrg_ccb, 0) == ERROR,
        "a live PPPoE session blocks the removal", "got SUCCESS");
    rte_atomic16_set(&nb_remove_ppp_ccb.ppp_bool, 0);

    rte_atomic16_set(&nb_remove_dhcp_ccb.dhcp_bool, 1);
    TEST_ASSERT(remove_hsi_config(fastrg_ccb, 0) == ERROR,
        "a running DHCP server blocks the removal", "got SUCCESS");
    rte_atomic16_set(&nb_remove_dhcp_ccb.dhcp_bool, 0);

    TEST_ASSERT(dns_static_get_count(&nb_remove_dhcp_ccb.dns_state.static_table) == 1,
        "a refused removal keeps the static DNS records", "count %u",
        dns_static_get_count(&nb_remove_dhcp_ccb.dns_state.static_table));

    TEST_ASSERT(remove_hsi_config(fastrg_ccb, 0) == SUCCESS,
        "removing an idle subscriber returns SUCCESS", "got ERROR");
    TEST_ASSERT(rte_atomic16_read(&nb_remove_ppp_ccb.vlan_id) == 0,
        "the VLAN binding is cleared", "vlan %d",
        rte_atomic16_read(&nb_remove_ppp_ccb.vlan_id));
    TEST_ASSERT(dns_static_get_count(&nb_remove_dhcp_ccb.dns_state.static_table) == 0 &&
        dns_static_lookup(&nb_remove_dhcp_ccb.dns_state.static_table,
            "static.fastrg.org") == NULL,
        "static DNS records are cleared with the HSI config", "count %u",
        dns_static_get_count(&nb_remove_dhcp_ccb.dns_state.static_table));

    fastrg_ccb->ppp_ccb[0] = orig_ppp_ccb;
    fastrg_ccb->dhcp_ccb[0] = orig_dhcp_ccb;
    fastrg_ccb->vlan_userid_map = orig_vlan_map;
    fastrg_mfree(vlan_map);
}

void test_fastrg_gen_cli_request(FastRG_t *fastrg_ccb)
{
    printf("\nTesting fastrg_gen_cli_request function:\n");
    printf("========================================\n\n");

    /* The shared fixture has no rings, so this case brings its own and hands
     * the CCB back untouched. */
    struct rte_ring *saved_cp_q = fastrg_ccb->cp_q;
    struct rte_ring *saved_free_mail_ring = fastrg_ccb->free_mail_ring;
    struct rte_ring *cp_q = rte_ring_create("nb_test_cp_q", 16, rte_socket_id(), 0);
    struct rte_ring *free_mail_ring = rte_ring_create("nb_test_free_mail", 16, rte_socket_id(), 0);
    tFastRG_MBX mail_slot;

    TEST_ASSERT(cp_q != NULL && free_mail_ring != NULL, "create the test mail rings",
        "cp_q %p free_mail_ring %p", (void *)cp_q, (void *)free_mail_ring);
    if (cp_q == NULL || free_mail_ring == NULL)
        goto out;

    memset(&mail_slot, 0, sizeof(mail_slot));
    fastrg_ccb->cp_q = cp_q;
    fastrg_ccb->free_mail_ring = free_mail_ring;
    rte_ring_enqueue(free_mail_ring, &mail_slot);

    snat_fwd_req_t req = {.eport = 8080, .iport = 80, .dip = "192.168.1.100"};
    STATUS ret = fastrg_gen_cli_request(fastrg_ccb, EV_NORTHBOUND_PPPoE,
        PPPoE_CMD_SNAT_SET, 0, &req, 5);
    TEST_ASSERT(ret == SUCCESS, "posting a CLI request returns SUCCESS", "got ERROR");

    tFastRG_MBX *posted = NULL;
    TEST_ASSERT(rte_ring_dequeue(cp_q, (void **)&posted) == 0 && posted == &mail_slot,
        "the request lands on cp_q", "dequeue failed or wrong slot");
    TEST_ASSERT(posted->type == EV_NORTHBOUND_PPPoE &&
        posted->northbound_msg.cmd == PPPoE_CMD_SNAT_SET &&
        posted->northbound_msg.ccb_id == 0 &&
        posted->northbound_msg.seq == 5 &&
        posted->northbound_msg.payload == &req,
        "the mail carries cmd, ccb id, seq and payload", "seq %u cmd %u",
        posted->northbound_msg.seq, posted->northbound_msg.cmd);

    /* The same path with no waiter: fire-and-forget events carry seq 0. */
    rte_ring_enqueue(free_mail_ring, posted);
    ret = fastrg_gen_northbound_event(fastrg_ccb, EV_NORTHBOUND_DHCP, DHCP_CMD_ENABLE, 0, NULL);
    TEST_ASSERT(ret == SUCCESS, "posting a fire-and-forget event returns SUCCESS", "got ERROR");
    TEST_ASSERT(rte_ring_dequeue(cp_q, (void **)&posted) == 0 &&
        posted->northbound_msg.seq == 0,
        "a fire-and-forget event carries seq 0", "seq %u", posted->northbound_msg.seq);

    /* No free mail slot left: the caller keeps the payload. */
    ret = fastrg_gen_cli_request(fastrg_ccb, EV_NORTHBOUND_PPPoE,
        PPPoE_CMD_SNAT_SET, 0, &req, 6);
    TEST_ASSERT(ret == ERROR, "an empty free mail ring fails the post", "got SUCCESS");

    fastrg_ccb->cp_q = saved_cp_q;
    fastrg_ccb->free_mail_ring = saved_free_mail_ring;

out:
    if (cp_q != NULL)
        rte_ring_free(cp_q);
    if (free_mail_ring != NULL)
        rte_ring_free(free_mail_ring);
}

void test_northbound(FastRG_t *fastrg_ccb, U32 *total_tests, U32 *total_pass)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║           Northbound Unit Tests                             ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_count = 0;
    pass_count = 0;

    test_reconcile_port_mapping(fastrg_ccb);
    test_fastrg_gen_cli_request(fastrg_ccb);
    test_remove_hsi_config(fastrg_ccb);

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
