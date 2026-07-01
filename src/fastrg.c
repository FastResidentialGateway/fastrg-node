#define _GNU_SOURCE
#include <sys/signalfd.h>
#include <stdatomic.h>
#include <grpc/grpc.h>

#include <common.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_timer.h>
#include <rte_ether.h>
#include <rte_memcpy.h>
#include <rte_flow.h>
#include <rte_atomic.h>
#include <rte_pdump.h>
#include <rte_trace.h>
#include <rte_distributor.h>
#include <rte_errno.h>

#include "pppd/fsm.h"
#include "dp.h"
#include "dbg.h"
#include "init.h"
#include "dp_flow.h"
#include "dhcpd/dhcpd.h"
#include "dnsd/dnsd.h"
#include <ip_codec.h>
#include "config.h"
#include "controller.h"
#include "etcd_integration.h"
#include "kafka_producer.h"
#include "utils.h"
#include "pdump_capture.h"
#include "../northbound/grpc/fastrg_grpc_server.h"

#define BURST_SIZE        32

#define LINK_DOWN_TIMEOUT 10 /* seconds */

rte_atomic16_t stop_flag = RTE_ATOMIC16_INIT(0);
rte_atomic16_t start_flag = RTE_ATOMIC16_INIT(0);

FastRG_t                fastrg_ccb;

void fastrg_force_terminate_hsi(ppp_ccb_t *ppp_ccb)
{
    exit_ppp(ppp_ccb);
}

STATUS fastrg_add_subscriber_stats(FastRG_t *fastrg_ccb, U16 extra_count)
{
    if (extra_count == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, 
            "extra_count is 0, nothing to do");
        return SUCCESS;
    }

    /* Early-return only when all needed stats slots were already allocated. */
    U16 stats_new_count_check = fastrg_ccb->user_count + extra_count;
    if (fastrg_ccb->per_subscriber_stats_len >= stats_new_count_check) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "subscriber stats up to %u already allocated, reusing", stats_new_count_check);
        return SUCCESS;
    }

    if (!rte_atomic16_cmpset((volatile uint16_t *)&fastrg_ccb->per_subscriber_stats_updating.cnt, 0, 1)) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "Another resize operation is in progress for per_subscriber_stats");
        return ERROR;
    }

    U16 old_user_count = fastrg_ccb->user_count;
    U16 new_user_count = old_user_count + extra_count;
    /* Per-lcore × per-port new/old arrays (only EAL-lcore rows are touched). */
    struct per_ccb_stats *new_stats[RTE_MAX_LCORE][PORT_AMOUNT] = {{NULL}};
    struct per_ccb_stats *old_stats[RTE_MAX_LCORE][PORT_AMOUNT] = {{NULL}};
    unsigned int lcore_id;

    // Step 1: Allocate all new stats arrays first (calloc => new entries are zero)
    RTE_LCORE_FOREACH(lcore_id) {
        for(int i=0; i<PORT_AMOUNT; i++) {
            old_stats[lcore_id][i] = fastrg_ccb->per_subscriber_stats[lcore_id][i];

            new_stats[lcore_id][i] = fastrg_calloc(struct per_ccb_stats,
                (new_user_count + 1), sizeof(struct per_ccb_stats), 0);
            if (new_stats[lcore_id][i] == NULL) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                    "Cannot allocate memory for per_subscriber_stats[%u][%d]", lcore_id, i);
                // Cleanup already allocated new arrays
                unsigned int lc2;
                RTE_LCORE_FOREACH(lc2)
                    for(int j=0; j<PORT_AMOUNT; j++)
                        if (new_stats[lc2][j] != NULL)
                            fastrg_mfree(new_stats[lc2][j]);
                rte_atomic16_clear(&fastrg_ccb->per_subscriber_stats_updating);
                return ERROR;
            }

            // Preserve existing per-user counts; calloc already zeroed the rest
            // (the new user slots and the unknown-user slot at [new_user_count]).
            if (old_stats[lcore_id][i] != NULL)
                rte_memcpy(new_stats[lcore_id][i], old_stats[lcore_id][i],
                    old_user_count * sizeof(struct per_ccb_stats));
        }
    }

    // Step 2: Atomically swap all pointers
    RTE_LCORE_FOREACH(lcore_id)
        for(int i=0; i<PORT_AMOUNT; i++)
            __atomic_store_n(&fastrg_ccb->per_subscriber_stats[lcore_id][i],
                new_stats[lcore_id][i], __ATOMIC_RELEASE);

    // Step 3: Wait for RCU grace period before freeing old memory
    rte_rcu_qsbr_synchronize(fastrg_ccb->per_subscriber_stats_rcu, RTE_QSBR_THRID_INVALID);

    RTE_LCORE_FOREACH(lcore_id)
        for(int i=0; i<PORT_AMOUNT; i++)
            if (old_stats[lcore_id][i] != NULL)
                fastrg_mfree(old_stats[lcore_id][i]);

    rte_atomic16_clear(&fastrg_ccb->per_subscriber_stats_updating);
    fastrg_ccb->per_subscriber_stats_len = new_user_count;

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
        "%u subscriber stats entries added", extra_count);

    return SUCCESS;
}

STATUS fastrg_disable_subscriber_stats(FastRG_t *fastrg_ccb, U16 disable_count, U16 old_count)
{
    if (disable_count > old_count) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "Invalid disabling count %u", disable_count);
        return ERROR;
    }

    if (disable_count == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, 
            "disable_count is 0, nothing to do");
        return SUCCESS;
    }

    // Reset the disabled range [old_count-disable_count, old_count) on every
    // per-lcore row in place (no realloc; each lcore owns its own row).
    unsigned int lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        for(int i=0; i<PORT_AMOUNT; i++) {
            struct per_ccb_stats *s = fastrg_ccb->per_subscriber_stats[lcore_id][i];
            if (s == NULL)
                continue;
            memset(&s[old_count - disable_count], 0,
                disable_count * sizeof(struct per_ccb_stats));
        }
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
        "%u subscriber stats entries disabled (not freed)", disable_count);

    return SUCCESS;
}

STATUS fastrg_remove_subscriber_stats(FastRG_t *fastrg_ccb, U16 remove_count, U16 old_count)
{
    if (remove_count > old_count) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "Invalid remove count %u (old_count: %u)", remove_count, old_count);
        return ERROR;
    }

    if (remove_count == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, 
            "remove_count is 0, nothing to do");
        return SUCCESS;
    }

    if (!rte_atomic16_cmpset((volatile uint16_t *)&fastrg_ccb->per_subscriber_stats_updating.cnt, 0, 1)) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "Another resize operation is in progress for per_subscriber_stats");
        return ERROR;
    }

    U16 new_user_count = old_count - remove_count;
    struct per_ccb_stats *new_stats[RTE_MAX_LCORE][PORT_AMOUNT] = {{NULL}};
    struct per_ccb_stats *old_stats[RTE_MAX_LCORE][PORT_AMOUNT] = {{NULL}};
    unsigned int lcore_id;

    // Step 1: Allocate new smaller arrays (or leave NULL if new_user_count == 0)
    if (new_user_count > 0) {
        RTE_LCORE_FOREACH(lcore_id) {
            for(int i=0; i<PORT_AMOUNT; i++) {
                old_stats[lcore_id][i] = fastrg_ccb->per_subscriber_stats[lcore_id][i];

                new_stats[lcore_id][i] = fastrg_calloc(struct per_ccb_stats,
                    (new_user_count + 1), sizeof(struct per_ccb_stats), 0);
                if (new_stats[lcore_id][i] == NULL) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
                        "Cannot allocate memory for per_subscriber_stats[%u][%d]", lcore_id, i);
                    // Cleanup already allocated arrays
                    unsigned int lc2;
                    RTE_LCORE_FOREACH(lc2)
                        for(int j=0; j<PORT_AMOUNT; j++)
                            if (new_stats[lc2][j] != NULL)
                                fastrg_mfree(new_stats[lc2][j]);
                    rte_atomic16_clear(&fastrg_ccb->per_subscriber_stats_updating);
                    return ERROR;
                }

                // Copy retained data
                if (old_stats[lcore_id][i] != NULL)
                    rte_memcpy(new_stats[lcore_id][i], old_stats[lcore_id][i],
                        (new_user_count + 1) * sizeof(struct per_ccb_stats));
            }
        }
    } else {
        // If removing all users, just save old pointers
        RTE_LCORE_FOREACH(lcore_id)
            for(int i=0; i<PORT_AMOUNT; i++)
                old_stats[lcore_id][i] = fastrg_ccb->per_subscriber_stats[lcore_id][i];
    }

    // Step 2: Atomically swap all pointers
    RTE_LCORE_FOREACH(lcore_id)
        for(int i=0; i<PORT_AMOUNT; i++)
            __atomic_store_n(&fastrg_ccb->per_subscriber_stats[lcore_id][i],
                new_stats[lcore_id][i], __ATOMIC_RELEASE);

    // Step 3: Wait for RCU grace period before freeing old memory
    rte_rcu_qsbr_synchronize(fastrg_ccb->per_subscriber_stats_rcu, RTE_QSBR_THRID_INVALID);

    RTE_LCORE_FOREACH(lcore_id)
        for(int i=0; i<PORT_AMOUNT; i++)
            if (old_stats[lcore_id][i] != NULL)
                fastrg_mfree(old_stats[lcore_id][i]);

    rte_atomic16_clear(&fastrg_ccb->per_subscriber_stats_updating);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
        "%u subscriber stats entries removed", remove_count);

    return SUCCESS;
}

void fastrg_cleanup_subscriber_stats(FastRG_t *fastrg_ccb, U16 total_count)
{
    if (fastrg_ccb == NULL)
        return;

    // Free every per-lcore x per-port stats array
    if (total_count > 0) {
        unsigned int lcore_id;
        RTE_LCORE_FOREACH(lcore_id) {
            for(int i=0; i<PORT_AMOUNT; i++) {
                if (fastrg_ccb->per_subscriber_stats[lcore_id][i] != NULL) {
                    fastrg_mfree(fastrg_ccb->per_subscriber_stats[lcore_id][i]);
                    fastrg_ccb->per_subscriber_stats[lcore_id][i] = NULL;
                }
            }
        }
    }

    if (fastrg_ccb->per_subscriber_stats_rcu != NULL) {
        fastrg_mfree(fastrg_ccb->per_subscriber_stats_rcu);
        fastrg_ccb->per_subscriber_stats_rcu = NULL;
    }
}

STATUS fastrg_modify_subscriber_count(FastRG_t *fastrg_ccb, U16 new_count, 
    U16 old_count)
{
    STATUS ret = SUCCESS;

    if (new_count <= 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Invalid user count: %d", new_count);
        return ERROR;
    }

    if (new_count == old_count) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "User count unchanged: %d", old_count);
        return SUCCESS;
    }

    if (new_count > old_count) {
        // Add new entries using RCU-safe function
        U16 extra_count = new_count - old_count;
        ret = fastrg_add_subscriber_stats(fastrg_ccb, extra_count);
        if (ret != SUCCESS) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                "Failed to add %u subscriber stats entries", extra_count);
        }
    } else {
        // Remove entries using RCU-safe function
        U16 remove_count = old_count - new_count;
        ret = fastrg_remove_subscriber_stats(fastrg_ccb, remove_count, old_count);
        if (ret != SUCCESS) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                "Failed to remove %u subscriber stats entries", remove_count);
        }
    }

    return ret;
}

STATUS fastrg_gen_northbound_event(FastRG_t *fastrg_ccb, fastrg_event_type_t event_type,
    U8 cmd_type, U16 ccb_id)
{
    /* Try to get a free mail slot from free_mail_ring */
    tFastRG_MBX *slot = NULL;
    fastrg_event_northbound_msg_t *northbound_msg;

    /* Get a free mail slot */
    if (rte_ring_dequeue(fastrg_ccb->free_mail_ring, (void **)&slot) == 0) {
        /* Deep copy packet data to slot's refp buffer to avoid data buffer being overwritten by rx_burst */
        northbound_msg = (fastrg_event_northbound_msg_t *)slot->refp;
        northbound_msg->cmd = cmd_type;
        northbound_msg->ccb_id = ccb_id;
        slot->type = event_type;
        slot->len = sizeof(fastrg_event_northbound_msg_t);
        /* cp_q is full: return slot to free_mail_ring */
        if (rte_ring_enqueue(fastrg_ccb->cp_q, slot) != 0) {
            rte_ring_enqueue(fastrg_ccb->free_mail_ring, slot);
            return ERROR;
        }
        return SUCCESS;
    }
    return ERROR;
}

void link_disconnect(__attribute__((unused)) struct rte_timer *tim, FastRG_t *fastrg_ccb)
{
    for(int i=0; i<fastrg_ccb->user_count; i++)
        fastrg_gen_northbound_event(fastrg_ccb, EV_NORTHBOUND_PPPoE, PPPoE_CMD_FORCE_DISABLE, i);
}

/***************************************************************
 * fastrg_loop : 
 *
 * purpose: Main event loop.
 ***************************************************************/
int fastrg_loop(FastRG_t *fastrg_ccb)
{
    tFastRG_MBX         *mail[RING_BURST_SIZE];
    U16                 burst_size;
    fastrg_event_type_t recv_type;
    uint64_t prev_tsc = fastrg_get_cur_cycles(), cur_tsc = 0, diff_tsc = 0;
    uint64_t timer_resolution_cycles = fastrg_get_cycles_in_sec() / 10; /* check every 100ms */

    fastrg_ccb->lcore_usage[rte_lcore_id()].role = "ctrl";
    while(rte_atomic16_read(&stop_flag) == 0) {
        uint64_t _t0 = fastrg_get_cur_cycles();
        burst_size = rte_ring_dequeue_burst(fastrg_ccb->cp_q, (void **)mail, RING_BURST_SIZE, NULL);
        for(int i=0; i<burst_size; i++) {
            recv_type = mail[i]->type;
            switch(recv_type) {
            case EV_NORTHBOUND_PPPoE: {
                /* process cli command */
                fastrg_event_northbound_msg_t *pppoe_msg = (fastrg_event_northbound_msg_t *)mail[i]->refp;
                ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, pppoe_msg->ccb_id);
                if (pppoe_msg->cmd == PPPoE_CMD_DISABLE) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %d pppoe is terminating\n", pppoe_msg->ccb_id + 1);
                    if (ppp_disconnect(ppp_ccb) == SUCCESS) {
                        fastrg_ccb->cur_user--;
                        /* PPPoE "disconnecting" transition → controller via Kafka. */
                        char uid[8];
                        snprintf(uid, sizeof(uid), "%u", pppoe_msg->ccb_id + 1);
                        kafka_report_pppoe_state(uid, KAFKA_PPPOE_DISCONNECTING, NULL, NULL, NULL);
                    }
                } else if (pppoe_msg->cmd == PPPoE_CMD_ENABLE) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %d pppoe is spawning\n", pppoe_msg->ccb_id + 1);
                    if (ppp_connect(ppp_ccb) == SUCCESS) {
                        fastrg_ccb->cur_user++;
                        /* PPPoE "connecting" transition → controller via Kafka. */
                        char uid[8];
                        snprintf(uid, sizeof(uid), "%u", pppoe_msg->ccb_id + 1);
                        kafka_report_pppoe_state(uid, KAFKA_PPPOE_CONNECTING, NULL, NULL, NULL);
                    }
                } else if (pppoe_msg->cmd == PPPoE_CMD_FORCE_DISABLE) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %d pppoe is force terminating\n", pppoe_msg->ccb_id + 1);
                    fastrg_force_terminate_hsi(ppp_ccb);
                }
                rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                break;
            }
            case EV_NORTHBOUND_DHCP: {
                fastrg_event_northbound_msg_t *dhcp_msg = (fastrg_event_northbound_msg_t *)mail[i]->refp;
                dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, dhcp_msg->ccb_id);
                if (dhcp_msg->cmd == DHCP_CMD_DISABLE) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %d dhcp server is terminating\n", dhcp_msg->ccb_id + 1);
                    rte_atomic16_cmpset((uint16_t *)&dhcp_ccb->dhcp_bool.cnt, 1, 0);
                    FastRG_LOG(INFO, fastrg_ccb->fp, dhcp_ccb, DHCPLOGMSG, "User %d dhcp server is terminated\n", dhcp_msg->ccb_id + 1);
                } else if (dhcp_msg->cmd == DHCP_CMD_ENABLE) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %d dhcp server is spawning\n", dhcp_msg->ccb_id + 1);
                    rte_atomic16_cmpset((uint16_t *)&dhcp_ccb->dhcp_bool.cnt, 0, 1);
                    FastRG_LOG(INFO, fastrg_ccb->fp, dhcp_ccb, DHCPLOGMSG, "User %d dhcp server is spawned\n", dhcp_msg->ccb_id + 1);
                }
                rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                break;
            }
            case EV_LINK: {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Recv Link Up/Down event");
                U16 link_port = *(U16 *)&(mail[i]->refp[1]);
                /* Update per-port link state cache for Prometheus metrics. Count a flap
                 * whenever the state actually transitions (caught even if it toggles
                 * faster than the scrape interval). */
                if (link_port < PORT_AMOUNT) {
                    U8 new_up = (mail[i]->refp[0] == LINK_UP) ? 1 : 0;
                    U8 old_up = __atomic_exchange_n(&fastrg_ccb->nic_link_up[link_port],
                        new_up, __ATOMIC_RELAXED);
                    if (old_up != new_up)
                        __atomic_fetch_add(&fastrg_ccb->nic_link_flaps[link_port], 1ULL,
                            __ATOMIC_RELAXED);
                    if (new_up) {
                        struct rte_eth_link lnk = {0};
                        if (rte_eth_link_get_nowait(link_port, &lnk) == 0)
                            __atomic_store_n(&fastrg_ccb->nic_link_speed[link_port],
                                lnk.link_speed, __ATOMIC_RELAXED);
                    } else {
                        __atomic_store_n(&fastrg_ccb->nic_link_speed[link_port], 0u,
                            __ATOMIC_RELAXED);
                    }
                }
                if (link_port == 1) {
                    if (mail[i]->refp[0] == LINK_DOWN) {
                        rte_timer_reset(&fastrg_ccb->link, 
                            LINK_DOWN_TIMEOUT * fastrg_get_cycles_in_sec(), // 10 seconds
                            SINGLE, fastrg_ccb->lcore.timer_thread, 
                            (rte_timer_cb_t)link_disconnect, fastrg_ccb);
                    } else if (mail[i]->refp[0] == LINK_UP) {
                        rte_timer_stop(&fastrg_ccb->link);
                    }
                }
                /* Link event type still uses fastrg_mfree (dynamically allocated) */
                fastrg_mfree(mail[i]);
                break;
            }
            case EV_DP_PPPoE: {
                /* recv pppoe packet from data plane — mbuf pointer in mail */
                U8 *pkt_data = rte_pktmbuf_mtod(mail[i]->mbuf, U8 *);
                if (ppp_process(fastrg_ccb, pkt_data, mail[i]->len) == ERROR) {
                    rte_pktmbuf_free(mail[i]->mbuf);
                    rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                    continue;
                }
                rte_pktmbuf_free(mail[i]->mbuf);
                rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                break;
            }
            case EV_DP_DHCP: {
                U16 ccb_id = mail[i]->ccb_id;
                U8 *pkt_data = rte_pktmbuf_mtod(mail[i]->mbuf, U8 *);
                struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
                vlan_header_t *vlan_header = (vlan_header_t *)(eth_hdr + 1);
                struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(vlan_header + 1);
                struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
                int ret = dhcpd(fastrg_ccb, NULL, eth_hdr, vlan_header, ip_hdr, udp_hdr, ccb_id);
                if (ret > 0) {
                    U16 out_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
                        sizeof(struct rte_ipv4_hdr) + rte_be_to_cpu_16(ip_hdr->total_length);
                    lan_ctrl_tx(fastrg_ccb, ccb_id, pkt_data, out_len);
                } else if (ret == 0) {
                    wan_ctrl_tx(fastrg_ccb, ccb_id, pkt_data, mail[i]->len);
                }
                rte_pktmbuf_free(mail[i]->mbuf);
                rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                break;
            }
            case EV_DP_DNS: {
                U16 ccb_id = mail[i]->ccb_id;
                U8 port_id = mail[i]->port_id;
                U8 *dns_pkt_data = rte_pktmbuf_mtod(mail[i]->mbuf, U8 *);
                if (port_id == WAN_PORT) {
                    dnsd_cp_process_wan_udp_response(fastrg_ccb, dns_pkt_data,
                        mail[i]->len, ccb_id);
                } else {
                    struct rte_ether_hdr *dns_eth = (struct rte_ether_hdr *)dns_pkt_data;
                    vlan_header_t *dns_vlan = (vlan_header_t *)(dns_eth + 1);
                    struct rte_ipv4_hdr *dns_ip = (struct rte_ipv4_hdr *)(dns_vlan + 1);
                    if (dns_ip->next_proto_id == PROTO_TYPE_UDP) {
                        dnsd_cp_process_lan_udp_query(fastrg_ccb, dns_pkt_data,
                            mail[i]->len, ccb_id);
                    } else if (dns_ip->next_proto_id == PROTO_TYPE_TCP) {
                        dnsd_cp_process_lan_tcp_query(fastrg_ccb, dns_pkt_data,
                            mail[i]->len, ccb_id);
                    }
                }
                rte_pktmbuf_free(mail[i]->mbuf);
                rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                break;
            }
            default:
                /* Return unknown type slot to free_mail_ring */
                rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
            }
            mail[i] = NULL;
        }

        /* Drain etcd config events. Applying them here (and nowhere else)
         * makes the control-plane loop the single writer of CCB state. */
        etcd_event_t *etcd_evs[RING_BURST_SIZE];
        U16 etcd_burst = rte_ring_dequeue_burst(fastrg_ccb->etcd_event_q,
            (void **)etcd_evs, RING_BURST_SIZE, NULL);
        for(int i=0; i<etcd_burst; i++) {
            etcd_event_dispatch(fastrg_ccb, etcd_evs[i]);
            etcd_event_free(etcd_evs[i]);
        }

        cur_tsc = fastrg_get_cur_cycles();
        diff_tsc = cur_tsc - prev_tsc;
        if (diff_tsc >= timer_resolution_cycles) {
            rte_timer_manage();
            prev_tsc = cur_tsc;
        }

        uint64_t _elapsed = fastrg_get_cur_cycles() - _t0;
        fastrg_ccb->lcore_usage[rte_lcore_id()].total_cycles += _elapsed;
        if (burst_size > 0 || etcd_burst > 0)
            fastrg_ccb->lcore_usage[rte_lcore_id()].busy_cycles += _elapsed;
    }

    return 0;
}

int control_plane(FastRG_t *fastrg_ccb)
{
    rte_thread_t thread_id = rte_thread_self();
    rte_thread_set_name(thread_id, "control_plane");
    if (fastrg_loop(fastrg_ccb) == ERROR)
        return -1;
    return 0;
}

STATUS northbound(FastRG_t *fastrg_ccb)
{
    // Initialize controller client
    if (controller_init(fastrg_ccb) != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Controller initialization failed");
        return ERROR;
    }

    BOOL is_standalone = FALSE;
    // Register this node with the controller, if fails, switch to standalone mode
    if (controller_register_this_node(fastrg_ccb) != 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "Node registration with controller failed");
        controller_cleanup(fastrg_ccb);
        is_standalone = TRUE;
    }

    /* Need to set standalone mode before etcd integration */
    fastrg_ccb->is_standalone = is_standalone;

    if (is_standalone == FALSE) {
        /* Start the Kafka telemetry producer (config-apply / PPPoE state / errors).
         * Empty KafkaBrokers disables it; report_* calls then no-op. */
        if (fastrg_ccb->kafka_brokers && fastrg_ccb->kafka_brokers[0] != '\0') {
            if (kafka_producer_init(fastrg_ccb->kafka_brokers, fastrg_ccb->node_uuid) != SUCCESS)
                FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                    "Kafka producer init failed; telemetry disabled");
        }

        // Initialize and start etcd integration
        if (etcd_integration_init(fastrg_ccb) == ERROR) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Etcd integration initialization failed");
            controller_cleanup(fastrg_ccb);
            return ERROR;
        }

        if (etcd_integration_start(fastrg_ccb) == ERROR) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Etcd integration start failed");
            etcd_integration_cleanup(fastrg_ccb);
            controller_cleanup(fastrg_ccb);
            return ERROR;
        }
    }

    unlink(fastrg_ccb->unix_sock_path);

    /* Start the Prometheus /metrics HTTP server so Prometheus can scrape this node
     * directly. Non-fatal on failure — metrics are observability, not core function. */
    if (fastrg_create_pthread("fastrg_metrics",
        metrics_server_run, fastrg_ccb, rte_lcore_id()) != SUCCESS)
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
            "Metrics HTTP server thread failed to start; Prometheus scrape disabled");

    return fastrg_create_pthread("fastrg_grpc",
        fastrg_grpc_server_run, fastrg_ccb, rte_lcore_id());
}

void fastrg_stop()
{
    FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL, "FastRG system stopping...");
    rte_eal_mp_wait_lcore();
    // Cleanup Kafka producer (flush pending telemetry)
    kafka_producer_cleanup();
    // Cleanup etcd integration
    etcd_integration_cleanup(&fastrg_ccb);

    // Cleanup controller client
    controller_cleanup(&fastrg_ccb);

    rte_ring_free(fastrg_ccb.cp_q);
    /* drain any etcd events left unconsumed before freeing the ring */
    etcd_event_t *ev;
    while (rte_ring_dequeue(fastrg_ccb.etcd_event_q, (void **)&ev) == 0)
        etcd_event_free(ev);
    rte_ring_free(fastrg_ccb.etcd_event_q);
    close(fastrg_ccb.unix_sock_fd);
    U16 total_ccbs = fastrg_ccb.user_count;
    fastrg_ccb.user_count = 0;
    pppd_cleanup_ccb(&fastrg_ccb, total_ccbs);
    dhcpd_cleanup_ccb(&fastrg_ccb, total_ccbs);
    /* stop any active CLI capture session and free its state */
    fastrg_pdump_capture_cleanup(&fastrg_ccb);
    #ifdef RTE_LIB_PDUMP
    /*uninitialize packet capture framework */
    rte_pdump_uninit();
    #endif
    //rte_trace_save();
    FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL, "bye!");
    /* DPDK still holds fastrg_ccb.fp as its log stream (set via
     * rte_openlog_stream() during init). rte_eal_cleanup() runs after
     * fastrg_stop() returns and may emit logs (e.g. i40e "Invalid memory"
     * during PCI device uninit). Detach the stream back to stderr before
     * closing the file, otherwise DPDK writes to a closed FILE* and crashes
     * with "free(): invalid pointer" in _IO_free_backup_area. */
    //rte_openlog_stream(stderr);
    if (fastrg_ccb.fp != stdout)
        fclose(fastrg_ccb.fp);
    grpc_shutdown();
    // Free allocated strings
    if (fastrg_ccb.eal_args) free(fastrg_ccb.eal_args);
    if (fastrg_ccb.log_path) free(fastrg_ccb.log_path);
    if (fastrg_ccb.unix_sock_path) free(fastrg_ccb.unix_sock_path);
    if (fastrg_ccb.node_grpc_ip_port) free(fastrg_ccb.node_grpc_ip_port);
    if (fastrg_ccb.controller_address) free(fastrg_ccb.controller_address);
    if (fastrg_ccb.etcd_endpoints) free(fastrg_ccb.etcd_endpoints);
    if (fastrg_ccb.kafka_brokers) free(fastrg_ccb.kafka_brokers);
    if (fastrg_ccb.central_office_location) free(fastrg_ccb.central_office_location);
    if (fastrg_ccb.metrics_ip_port) free(fastrg_ccb.metrics_ip_port);
    if (fastrg_ccb.node_uuid) fastrg_mfree(fastrg_ccb.node_uuid);
    fastrg_mfree(fastrg_ccb.vlan_userid_map);
    fastrg_cleanup_subscriber_stats(&fastrg_ccb, total_ccbs);
}

int fastrg_start(int argc, char **argv)
{
    fastrg_ccb.fp = stdout; // Temporary log to stdout until config is parsed
    fastrg_ccb.loglvl = LOGERR;// Temporary log level
    dbg_init((void *)&fastrg_ccb);
    int sfd = setup_signalfd();
    if (sfd == -1) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "signal fd setup failed");
        return -1;
    }
    grpc_init();
    fastrg_ccb.eal_args = make_eal_args_string(argc, (const char **)argv);
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "rte initlize fail.\n");
        grpc_shutdown();
        close(sfd);
        return -1;
    }

    /* Parse FastRG app args after EAL args (DPDK consumed argv[0..ret-1]) */
    const char *config_path;
    int app_argc = argc - ret;
    char **app_argv = argv + ret;
    /* Make argv[0] a fake program name so getopt scans the app args */
    if (app_argc > 0)
        app_argv[0] = argv[0];
    if (parse_app_args(app_argc, app_argv, &config_path) != SUCCESS)
        goto err;

    if (rte_eth_dev_count_avail() < 2) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "We need at least 2 eth ports.\n");
        goto err;
    }

    get_all_lcore_id(&fastrg_ccb.lcore, rte_lcore_count());

    if (rte_eth_dev_socket_id(0) > 0 && rte_eth_dev_socket_id(0) != (int)rte_lcore_to_socket_id(fastrg_ccb.lcore.lan_data_threads[0]))
        FastRG_LOG(WARN, fastrg_ccb.fp, NULL, NULL, "LAN port is on remote NUMA node to polling thread.\n\tPerformance will not be optimal.\n");
    if (rte_eth_dev_socket_id(1) > 0 && rte_eth_dev_socket_id(1) != (int)rte_lcore_to_socket_id(fastrg_ccb.lcore.wan_ctrl_thread))
        FastRG_LOG(WARN, fastrg_ccb.fp, NULL, NULL, "WAN port is on remote NUMA node to polling thread.\n\tPerformance will not be optimal.\n");

    /* Read network config */
    struct fastrg_config fastrg_cfg;
    if (parse_config(config_path, &fastrg_ccb, &fastrg_cfg) != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "parse config file error\n");
        goto err;
    }
    FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL, "FastRG log level is %s", loglvl2str(fastrg_ccb.loglvl));

    fastrg_ccb.log_path = strdup(fastrg_cfg.log_path);
    fastrg_ccb.unix_sock_path = strdup(fastrg_cfg.unix_sock_path);
    fastrg_ccb.node_grpc_ip_port = strdup(fastrg_cfg.node_grpc_ip_port);
    fastrg_ccb.controller_address = strdup(fastrg_cfg.controller_address);
    fastrg_ccb.etcd_endpoints = strdup(fastrg_cfg.etcd_endpoints);
    fastrg_ccb.kafka_brokers = strdup(fastrg_cfg.kafka_brokers);
    fastrg_ccb.central_office_location = strdup(fastrg_cfg.central_office_location);
    fastrg_ccb.metrics_ip_port = strdup(fastrg_cfg.metrics_ip_port);
    if (!fastrg_ccb.unix_sock_path || !fastrg_ccb.node_grpc_ip_port ||
        !fastrg_ccb.controller_address || !fastrg_ccb.etcd_endpoints ||
        !fastrg_ccb.kafka_brokers || !fastrg_ccb.central_office_location ||
        !fastrg_ccb.metrics_ip_port) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "Memory allocation failed for config strings");
        goto err;
    }
    fastrg_ccb.enable_ddp = fastrg_cfg.enable_ddp;
    fastrg_ccb.heartbeat_interval = fastrg_cfg.heartbeat_interval;
    fastrg_ccb.fp = fopen(fastrg_cfg.log_path, "w+");
    if (fastrg_ccb.fp) {
        rte_openlog_stream(fastrg_ccb.fp);
    } else {
        FastRG_LOG(WARN, stdout, NULL, NULL, "Failed to open log file %s, using stdout", fastrg_cfg.log_path);
        fastrg_ccb.fp = stdout;
    }

    if (fastrg_ccb.user_count < MIN_USER_COUNT || fastrg_ccb.user_count > MAX_USER_COUNT) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "FastRG system user count configuration failed.\n");
        goto err;
    }

    /* init users and ports info */
    /* vlan 1 is mapped to index 0. However, vlan 1 is not assigned to any user by default, 
    so index 0 is not used */
    fastrg_ccb.vlan_userid_map = fastrg_malloc(rte_atomic16_t, MAX_VLAN_ID * sizeof(rte_atomic16_t), 0);
    for(int i=0; i<MAX_VLAN_ID; i++)
        rte_atomic16_set(&fastrg_ccb.vlan_userid_map[i], INVALID_CCB_ID);

    ret = sys_init(&fastrg_ccb, &fastrg_cfg);
    if (ret) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "System initiation failed: %s", rte_strerror(ret));
        goto err;
    }

    if (pppd_init((void *)&fastrg_ccb) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "PPP initiation failed");
        goto err;
    }

    if (dhcp_init((void *)&fastrg_ccb) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "DHCP initiation failed");
        goto err;
    }
    /* Init the pppoe alive user count */
    fastrg_ccb.cur_user = 0;
    rte_prefetch2(&fastrg_ccb);
    #ifdef RTE_LIB_PDUMP
    /* initialize packet capture framework */
    rte_pdump_init();
    #endif

    /* CLI-driven per-subscriber capture state (exec pdump start/stop) */
    if (fastrg_pdump_capture_init(&fastrg_ccb) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "pdump capture init failed");
        goto err;
    }

    /* Set up the per-port data plane according to the selected mode:
     *   DP_MODE_RSS         : install PPPoE-aware rte_flow rules (ICE/E810, or
     *                         i40e/X710 with DDP). Queue 0 = PPPoE control
     *                         (Discovery + Session w/o 5-tuple); queues 1..N =
     *                         RSS worker queues (5-tuple, inner-header aware).
     *   DP_MODE_DISTRIBUTOR : no rte_flow rules; a single RX queue 0 is polled
     *                         by a distributor RX lcore that fans PPPoE session
     *                         TCP/UDP out to N worker lcores in software.
     * Queue/worker count: N = max(1, (lcore_count - 4) / 2).
     */
    FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL,
        "NIC Vendor ID: 0x%04x, vendor: %s, datapath: %s, DDP enabled: %s\n",
        fastrg_ccb.nic_info.vendor_id,
        fastrg_ccb.nic_info.vendor_name ? fastrg_ccb.nic_info.vendor_name : "unknown",
        fastrg_ccb.datapath_mode == DP_MODE_RSS ? "RSS multi-queue" : "software distributor",
        fastrg_ccb.enable_ddp ? "yes" : "no");

    /* Both data-plane modes share the same lcore budget: main + ctrl + 2 RX +
     * 2N workers, so both require at least 6 even cores. */
    if (rte_lcore_count() < 6 || rte_lcore_count() % 2 != 0) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL,
            "We need at least 6 cores and the lcore count must be even.\n");
        goto err;
    }

    if (fastrg_ccb.datapath_mode == DP_MODE_RSS) {
        struct rte_flow_error flow_error;
        U16 total_q = fastrg_calc_queue_count(rte_lcore_count());
        FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL,
            "Setting up rte_flow rules: %u total queues per port "
            "(queue 0=ctrl, queues 1..%u=RSS)", total_q, total_q - 1);
        for(U16 port_id=0; port_id<rte_eth_dev_count_avail(); port_id++) {
            if (setup_port_flows(&fastrg_ccb, port_id, total_q, &flow_error) != 0) {
                FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL,
                    "Port %u: flow setup failed (type=%d): %s",
                    port_id, flow_error.type,
                    flow_error.message ? flow_error.message : "(no reason)");
                goto err;
            }
        }
    } else {
        FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL,
            "PMD (%s) without PPPoE-aware RSS: using rte_distributor datapath "
            "(single RX queue 0, %u worker(s) per direction)",
            fastrg_ccb.nic_info.vendor_name ? fastrg_ccb.nic_info.vendor_name : "unknown",
            fastrg_ccb.lcore.num_data_queues);
    }

    /* --- Launch fixed threads --- */
    rte_eal_remote_launch((lcore_function_t *)control_plane, (void *)&fastrg_ccb, fastrg_ccb.lcore.ctrl_thread);

    if (fastrg_ccb.datapath_mode == DP_MODE_RSS) {
        /* ICE PMD or i40e+DDP: separate ctrl + data threads with multi-queue RSS */
        rte_eal_remote_launch((lcore_function_t *)wan_ctrl_rx, (void *)&fastrg_ccb, fastrg_ccb.lcore.wan_ctrl_thread);
        rte_eal_remote_launch((lcore_function_t *)lan_ctrl_rx, (void *)&fastrg_ccb, fastrg_ccb.lcore.lan_ctrl_thread);

        /* --- Launch dynamic data threads (one wan_data_rx + one lan_rx per RSS queue) --- */
        static dp_rx_arg_t wan_data_args[MAX_DATA_QUEUES];
        static dp_rx_arg_t lan_data_args[MAX_DATA_QUEUES];
        U16 num_dq = fastrg_ccb.lcore.num_data_queues;
        FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL,
            "Launching %u wan_data_rx + %u lan_data_rx threads (RSS queues 1..%u)",
            num_dq, num_dq, num_dq);
        for(U16 i=0; i<num_dq; i++) {
            U16 queue_id = i + 1;  /* RSS queues start at 1 */
            wan_data_args[i].fastrg_ccb = &fastrg_ccb;
            wan_data_args[i].rx_queue_id = queue_id;
            wan_data_args[i].tx_queue_id = queue_id;
            rte_eal_remote_launch((lcore_function_t *)wan_data_rx,
                (void *)&wan_data_args[i], fastrg_ccb.lcore.wan_data_threads[i]);

            lan_data_args[i].fastrg_ccb = &fastrg_ccb;
            lan_data_args[i].rx_queue_id = queue_id;
            lan_data_args[i].tx_queue_id = queue_id;
            rte_eal_remote_launch((lcore_function_t *)lan_data_rx,
                (void *)&lan_data_args[i], fastrg_ccb.lcore.lan_data_threads[i]);
        }
    } else {
        /* Software distributor: one RX/ctrl lcore per port classifies traffic
         * and fans PPPoE session TCP/UDP out to N worker lcores; queue 0 keeps
         * control plane + inline (IPTV/ARP/ICMP/DHCP/DNS) handling. */
        U16 num_dq = fastrg_ccb.lcore.num_data_queues;
        fastrg_ccb.wan_dist = rte_distributor_create("fastrg_wan_dist",
            rte_socket_id(), num_dq, RTE_DIST_ALG_BURST);
        fastrg_ccb.lan_dist = rte_distributor_create("fastrg_lan_dist",
            rte_socket_id(), num_dq, RTE_DIST_ALG_BURST);
        if (fastrg_ccb.wan_dist == NULL || fastrg_ccb.lan_dist == NULL) {
            FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL,
                "Cannot create rte_distributor instances: %s", rte_strerror(rte_errno));
            goto err;
        }
        rte_eal_remote_launch((lcore_function_t *)wan_dist_rx, (void *)&fastrg_ccb, fastrg_ccb.lcore.wan_ctrl_thread);
        rte_eal_remote_launch((lcore_function_t *)lan_dist_rx, (void *)&fastrg_ccb, fastrg_ccb.lcore.lan_ctrl_thread);

        static dist_worker_arg_t wan_worker_args[MAX_DATA_QUEUES];
        static dist_worker_arg_t lan_worker_args[MAX_DATA_QUEUES];
        FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL,
            "Launching %u wan_dist_worker + %u lan_dist_worker threads", num_dq, num_dq);
        for(U16 i=0; i<num_dq; i++) {
            wan_worker_args[i].fastrg_ccb = &fastrg_ccb;
            wan_worker_args[i].dist = fastrg_ccb.wan_dist;
            wan_worker_args[i].worker_id = i;
            wan_worker_args[i].tx_queue_id = i + 1;
            rte_eal_remote_launch((lcore_function_t *)wan_dist_worker,
                (void *)&wan_worker_args[i], fastrg_ccb.lcore.wan_data_threads[i]);

            lan_worker_args[i].fastrg_ccb = &fastrg_ccb;
            lan_worker_args[i].dist = fastrg_ccb.lan_dist;
            lan_worker_args[i].worker_id = i;
            lan_worker_args[i].tx_queue_id = i + 1;
            rte_eal_remote_launch((lcore_function_t *)lan_dist_worker,
                (void *)&lan_worker_args[i], fastrg_ccb.lcore.lan_data_threads[i]);
        }
    }

    if (northbound(&fastrg_ccb) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "Northbound initialization failed");
        goto err;
    }

    rte_atomic16_set(&start_flag, 1);

    uint64_t timer_resolution_cycles = fastrg_get_cycles_in_sec() / 100; /* 10ms */
    uint64_t prev_tsc = 0;

    while(1) {
        uint64_t cur_tsc = fastrg_get_cur_cycles();
        if (cur_tsc - prev_tsc > timer_resolution_cycles) {
            rte_timer_manage();
            prev_tsc = cur_tsc;
        }

        struct signalfd_siginfo si;
        ssize_t s = read(sfd, &si, sizeof(si));
        if (s < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                usleep(10000); // 10ms — matches timer resolution
                continue;
            }
            FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "signalfd read error: %s", strerror(errno));
            exit(EXIT_FAILURE);
        } else if (s != sizeof(si)) {
            FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "signalfd read unexpected size: %zd", s);
            continue;
        }
        if (s == sizeof(si)) {
            if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) {
                FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL, "Received signal %d", si.ssi_signo);
                rte_atomic16_set(&stop_flag, 1);
                break;
            } else {
                FastRG_LOG(WARN, fastrg_ccb.fp, NULL, NULL, "Received unexpected signal %d", si.ssi_signo);
                continue;
            }
        }
    }

    fastrg_stop();

    close(sfd);

    rte_eal_cleanup();

    return 0;

err:
    /* Unblock SIGINT/SIGTERM so Ctrl-C works even though sfd is about to be
     * closed and the signal-reading loop never ran. */
    {
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, SIGINT);
        sigaddset(&unblock, SIGTERM);
        sigprocmask(SIG_UNBLOCK, &unblock, NULL);
    }
    /* Stop any data-plane lcores that were already launched. */
    rte_atomic16_set(&stop_flag, 1);
    rte_eal_mp_wait_lcore();
    kafka_producer_cleanup();
    if (fastrg_ccb.fp && fastrg_ccb.fp != stdout) {
        /* Detach DPDK log stream before closing fp; rte_eal_cleanup() below
         * may still log and would otherwise write to a closed FILE*. */
        rte_openlog_stream(stderr);
        fclose(fastrg_ccb.fp);
    }
    if (fastrg_ccb.eal_args) free(fastrg_ccb.eal_args);
    if (fastrg_ccb.log_path) free(fastrg_ccb.log_path);
    if (fastrg_ccb.unix_sock_path) free(fastrg_ccb.unix_sock_path);
    if (fastrg_ccb.node_grpc_ip_port) free(fastrg_ccb.node_grpc_ip_port);
    if (fastrg_ccb.controller_address) free(fastrg_ccb.controller_address);
    if (fastrg_ccb.etcd_endpoints) free(fastrg_ccb.etcd_endpoints);
    if (fastrg_ccb.kafka_brokers) free(fastrg_ccb.kafka_brokers);
    if (fastrg_ccb.central_office_location) free(fastrg_ccb.central_office_location);
    if (fastrg_ccb.metrics_ip_port) free(fastrg_ccb.metrics_ip_port);
    grpc_shutdown();
    close(sfd);
    rte_eal_cleanup();
    return -1;
}
