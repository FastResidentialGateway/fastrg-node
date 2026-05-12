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

#include "pppd/fsm.h"
#include "dp.h"
#include "dbg.h"
#include "init.h"
#include "dp_flow.h"
#include "dhcpd/dhcpd.h"
#include "dnsd/dnsd.h"
#include <ip_codec.h>
#include "config.h"
#include "timer.h"
#include "controller.h"
#include "etcd_integration.h"
#include "utils.h"
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

    if (fastrg_ccb->per_subscriber_stats_len > fastrg_ccb->user_count) {
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, PPPLOGMSG, "we have unused ccb in mempool, no need to add more");
        return SUCCESS;
    }

    if (!rte_atomic16_cmpset((volatile uint16_t *)&fastrg_ccb->per_subscriber_stats_updating.cnt, 0, 1)) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
            "Another resize operation is in progress for per_subscriber_stats");
        return ERROR;
    }

    U16 old_user_count = fastrg_ccb->user_count;
    U16 new_user_count = old_user_count + extra_count;
    struct per_ccb_stats *new_stats[PORT_AMOUNT] = {NULL};
    struct per_ccb_stats *old_stats[PORT_AMOUNT];

    // Step 1: Allocate all new stats arrays first
    for(int i=0; i<PORT_AMOUNT; i++) {
        old_stats[i] = fastrg_ccb->per_subscriber_stats[i];

        new_stats[i] = fastrg_malloc(struct per_ccb_stats, 
            (new_user_count + 1) * sizeof(struct per_ccb_stats), 0);
        if (new_stats[i] == NULL) {
            FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                "Cannot allocate memory for per_subscriber_stats[%d]", i);
            // Cleanup already allocated new arrays
            for(int j=0; j<i; j++)
                fastrg_mfree(new_stats[j]);
            rte_atomic16_clear(&fastrg_ccb->per_subscriber_stats_updating);
            return ERROR;
        }

        // Copy old data if exists
        if (old_stats[i] != NULL) {
            rte_memcpy(new_stats[i], old_stats[i], 
                (old_user_count + 1) * sizeof(struct per_ccb_stats));
        }

        // Initialize new entries
        for(int j=old_user_count; j<new_user_count+1; j++) {
            rte_atomic64_init(&new_stats[i][j].rx_packets);
            rte_atomic64_init(&new_stats[i][j].rx_bytes);
            rte_atomic64_init(&new_stats[i][j].tx_packets);
            rte_atomic64_init(&new_stats[i][j].tx_bytes);
            rte_atomic64_init(&new_stats[i][j].dropped_packets);
            rte_atomic64_init(&new_stats[i][j].dropped_bytes);
        }
    }

    // Step 2: Atomically swap all pointers
    for(int i=0; i<PORT_AMOUNT; i++)
        __atomic_store_n(&fastrg_ccb->per_subscriber_stats[i], new_stats[i], __ATOMIC_RELEASE);

    // Step 3: Wait for RCU grace period before freeing old memory
    rte_rcu_qsbr_synchronize(fastrg_ccb->per_subscriber_stats_rcu, RTE_QSBR_THRID_INVALID);

    for(int i=0; i<PORT_AMOUNT; i++) {
        if (old_stats[i] != NULL)
            fastrg_mfree(old_stats[i]);
    }

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

    struct per_ccb_stats *old_stats[PORT_AMOUNT];
    for(int i=0; i<PORT_AMOUNT; i++) {
        old_stats[i] = fastrg_ccb->per_subscriber_stats[i];

        // Initialize new entries
        for(int j=old_count-disable_count; j<old_count; j++) {
            rte_atomic64_init(&old_stats[i][j].rx_packets);
            rte_atomic64_init(&old_stats[i][j].rx_bytes);
            rte_atomic64_init(&old_stats[i][j].tx_packets);
            rte_atomic64_init(&old_stats[i][j].tx_bytes);
            rte_atomic64_init(&old_stats[i][j].dropped_packets);
            rte_atomic64_init(&old_stats[i][j].dropped_bytes);
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
    struct per_ccb_stats *new_stats[PORT_AMOUNT] = {NULL};
    struct per_ccb_stats *old_stats[PORT_AMOUNT];

    // Step 1: Allocate new smaller arrays (or set to NULL if new_user_count == 0)
    if (new_user_count > 0) {
        for(int i=0; i<PORT_AMOUNT; i++) {
            old_stats[i] = fastrg_ccb->per_subscriber_stats[i];

            new_stats[i] = fastrg_malloc(struct per_ccb_stats, 
                (new_user_count + 1) * sizeof(struct per_ccb_stats), 0);
            if (new_stats[i] == NULL) {
                FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, 
                    "Cannot allocate memory for per_subscriber_stats[%d]", i);
                // Cleanup already allocated arrays
                for(int j=0; j<i; j++)
                    fastrg_mfree(new_stats[j]);
                rte_atomic16_clear(&fastrg_ccb->per_subscriber_stats_updating);
                return ERROR;
            }

            // Copy retained data
            if (old_stats[i] != NULL) {
                rte_memcpy(new_stats[i], old_stats[i], 
                    (new_user_count + 1) * sizeof(struct per_ccb_stats));
            }
        }
    } else {
        // If removing all users, just save old pointers
        for(int i=0; i<PORT_AMOUNT; i++)
            old_stats[i] = fastrg_ccb->per_subscriber_stats[i];
    }

    // Step 2: Atomically swap all pointers
    for(int i=0; i<PORT_AMOUNT; i++)
        __atomic_store_n(&fastrg_ccb->per_subscriber_stats[i], new_stats[i], __ATOMIC_RELEASE);

    // Step 3: Wait for RCU grace period before freeing old memory
    rte_rcu_qsbr_synchronize(fastrg_ccb->per_subscriber_stats_rcu, RTE_QSBR_THRID_INVALID);

    for(int i=0; i<PORT_AMOUNT; i++) {
        if (old_stats[i] != NULL)
            fastrg_mfree(old_stats[i]);
    }

    rte_atomic16_clear(&fastrg_ccb->per_subscriber_stats_updating);

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
        "%u subscriber stats entries removed", remove_count);

    return SUCCESS;
}

void fastrg_cleanup_subscriber_stats(FastRG_t *fastrg_ccb, U16 total_count)
{
    if (fastrg_ccb == NULL)
        return;

    // Free each port's stats array
    if (total_count > 0) {
        for(int i=0; i<PORT_AMOUNT; i++) {
            if (fastrg_ccb->per_subscriber_stats[i] != NULL) {
                fastrg_mfree(fastrg_ccb->per_subscriber_stats[i]);
                fastrg_ccb->per_subscriber_stats[i] = NULL;
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

STATUS fastrg_gen_northbound_event(fastrg_event_type_t event_type, U8 cmd_type,
    U16 ccb_id)
{
    /* Try to get a free mail slot from free_mail_ring */
    tFastRG_MBX *slot = NULL;
    fastrg_event_northbound_msg_t *northbound_msg;

    /* Get a free mail slot */
    if (rte_ring_dequeue(free_mail_ring, (void **)&slot) == 0) {
        /* Deep copy packet data to slot's refp buffer to avoid data buffer being overwritten by rx_burst */
        northbound_msg = (fastrg_event_northbound_msg_t *)slot->refp;
        northbound_msg->cmd = cmd_type;
        northbound_msg->ccb_id = ccb_id;
        slot->type = event_type;
        slot->len = sizeof(fastrg_event_northbound_msg_t);
        /* cp_q is full: return slot to free_mail_ring */
        if (rte_ring_enqueue(cp_q, slot) != 0) {
            rte_ring_enqueue(free_mail_ring, slot);
            return ERROR;
        }
        return SUCCESS;
    }
    return ERROR;
}

void link_disconnect(__attribute__((unused)) struct rte_timer *tim, FastRG_t *fastrg_ccb)
{
    for(int i=0; i<fastrg_ccb->user_count; i++)
        fastrg_gen_northbound_event(EV_NORTHBOUND_PPPoE, PPPoE_CMD_FORCE_DISABLE, i);
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

    while(rte_atomic16_read(&stop_flag) == 0) {
        burst_size = rte_ring_dequeue_burst(cp_q, (void **)mail, RING_BURST_SIZE, NULL);
        for(int i=0; i<burst_size; i++) {
            recv_type = mail[i]->type;
            switch(recv_type) {
            case EV_NORTHBOUND_PPPoE: {
                /* process cli command */
                fastrg_event_northbound_msg_t *pppoe_msg = (fastrg_event_northbound_msg_t *)mail[i]->refp;
                ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, pppoe_msg->ccb_id);
                if (pppoe_msg->cmd == PPPoE_CMD_DISABLE) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %d pppoe is terminating\n", pppoe_msg->ccb_id + 1);
                    if (ppp_disconnect(ppp_ccb) == SUCCESS)
                        fastrg_ccb->cur_user--;
                } else if (pppoe_msg->cmd == PPPoE_CMD_ENABLE) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %d pppoe is spawning\n", pppoe_msg->ccb_id + 1);
                    if (ppp_connect(ppp_ccb) == SUCCESS)
                        fastrg_ccb->cur_user++;
                } else if (pppoe_msg->cmd == PPPoE_CMD_FORCE_DISABLE) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %d pppoe is force terminating\n", pppoe_msg->ccb_id + 1);
                    fastrg_force_terminate_hsi(ppp_ccb);
                }
                rte_ring_enqueue(free_mail_ring, mail[i]);
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
                    FastRG_LOG(INFO, fastrg_ccb->fp, &dhcp_ccb, DHCPLOGMSG, "User %d dhcp server is spawned\n", dhcp_msg->ccb_id + 1);
                }
                rte_ring_enqueue(free_mail_ring, mail[i]);
                break;
            }
            case EV_LINK: {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Recv Link Up/Down event");
                if ((U16)(mail[i]->refp[1]) == 1) {
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
                    rte_ring_enqueue(free_mail_ring, mail[i]);
                    continue;
                }
                rte_pktmbuf_free(mail[i]->mbuf);
                rte_ring_enqueue(free_mail_ring, mail[i]);
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
                rte_ring_enqueue(free_mail_ring, mail[i]);
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
                rte_ring_enqueue(free_mail_ring, mail[i]);
                break;
            }
            default:
                /* Return unknown type slot to free_mail_ring */
                rte_ring_enqueue(free_mail_ring, mail[i]);
            }
            mail[i] = NULL;
        }

        cur_tsc = fastrg_get_cur_cycles();
        diff_tsc = cur_tsc - prev_tsc;
        if (diff_tsc >= timer_resolution_cycles) {
            rte_timer_manage();
            prev_tsc = cur_tsc;
        }
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
    return fastrg_create_pthread("fastrg_grpc", 
        fastrg_grpc_server_run, fastrg_ccb, rte_lcore_id());
}

void fastrg_stop()
{
    FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL, "FastRG system stopping...");
    rte_eal_mp_wait_lcore();
    // Cleanup etcd integration
    etcd_integration_cleanup(&fastrg_ccb);

    // Cleanup controller client
    controller_cleanup(&fastrg_ccb);

    rte_ring_free(cp_q);
    close(fastrg_ccb.unix_sock_fd);
    U16 total_ccbs = fastrg_ccb.user_count;
    fastrg_ccb.user_count = 0;
    pppd_cleanup_ccb(&fastrg_ccb, total_ccbs);
    dhcpd_cleanup_ccb(&fastrg_ccb, total_ccbs);
    #ifdef RTE_LIBRTE_PDUMP
    /*uninitialize packet capture framework */
    rte_pdump_uninit();
    #endif
    //rte_trace_save();
    FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL, "bye!");
    fclose(fastrg_ccb.fp);
    grpc_shutdown();
    // Free allocated strings
    if (fastrg_ccb.eal_args) free(fastrg_ccb.eal_args);
    if (fastrg_ccb.unix_sock_path) free(fastrg_ccb.unix_sock_path);
    if (fastrg_ccb.node_grpc_ip_port) free(fastrg_ccb.node_grpc_ip_port);
    if (fastrg_ccb.controller_address) free(fastrg_ccb.controller_address);
    if (fastrg_ccb.etcd_endpoints) free(fastrg_ccb.etcd_endpoints);
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

    if (rte_lcore_count() < 7) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "We need at least 7 cores.\n");
        goto err;
    }
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
    if (parse_config("/etc/fastrg/config.cfg", &fastrg_ccb, &fastrg_cfg) != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "parse config file error\n");
        goto err;
    }
    FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL, "FastRG log level is %s", loglvl2str(fastrg_ccb.loglvl));

    fastrg_ccb.unix_sock_path = strdup(fastrg_cfg.unix_sock_path);
    fastrg_ccb.node_grpc_ip_port = strdup(fastrg_cfg.node_grpc_ip_port);
    fastrg_ccb.controller_address = strdup(fastrg_cfg.controller_address);
    fastrg_ccb.etcd_endpoints = strdup(fastrg_cfg.etcd_endpoints);
    if (!fastrg_ccb.unix_sock_path || !fastrg_ccb.node_grpc_ip_port ||
        !fastrg_ccb.controller_address || !fastrg_ccb.etcd_endpoints) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "Memory allocation failed for config strings");
        goto err;
    }
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
    fastrg_ccb.vlan_userid_map = fastrg_malloc(rte_atomic16_t, MAX_VLAN_ID, 0);
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
    #ifdef RTE_LIBRTE_PDUMP
    /* initialize packet capture framework */
    rte_pdump_init();
    #endif

    /* Install PPPoE-aware RSS flow rules on both ports (ICE PMD, or i40e with
     * DDP loaded).
     * Queue layout per port:
     *   queue 0          : PPPoE control (Discovery + Session w/o 5-tuple)
     *   queues 1 .. N-1  : RSS worker queues (5-tuple, PPPoE inner-header aware)
     * Queue count: 1 + max(1, 1 + (lcore_count - 5) / 2)
     *
     * Other PMDs use a single queue; rte_flow rules are not installed.
     */
    BOOL is_ice_pmd = (fastrg_ccb.nic_info.vendor_id == NIC_VENDOR_ICE);
    BOOL is_i40e_ddp = (fastrg_ccb.nic_info.vendor_id == NIC_VENDOR_I40E &&
                        fastrg_ccb.i40e_ddp_enabled == TRUE);
    printf("NIC Vendor ID: 0x%04x, ICE PMD: %s, i40e DDP: %s, DDP enabled: %s\n",
        fastrg_ccb.nic_info.vendor_id,
        is_ice_pmd ? "yes" : "no",
        is_i40e_ddp ? "yes" : "no",
        fastrg_ccb.i40e_ddp_enabled ? "yes" : "no");
    BOOL use_multiqueue = (is_ice_pmd || is_i40e_ddp);
    if (use_multiqueue) {
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
            "Non-ICE/i40e PMD (%s) detected, using single queue per port, "
            "rte_flow rules skipped",
            fastrg_ccb.nic_info.vendor_name ? fastrg_ccb.nic_info.vendor_name : "unknown");
    }

    /* --- Launch fixed threads --- */
    rte_eal_remote_launch((lcore_function_t *)control_plane, (void *)&fastrg_ccb, fastrg_ccb.lcore.ctrl_thread);
    rte_eal_remote_launch((lcore_function_t *)timer_loop, (void *)&fastrg_ccb, fastrg_ccb.lcore.timer_thread);

    if (use_multiqueue) {
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
        /* Single-queue PMD: combined ctrl+data function per port */
        FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL,
            "Launching combined wan_combined_rx + lan_combined_rx threads (single queue 0)");
        rte_eal_remote_launch((lcore_function_t *)wan_combined_rx, (void *)&fastrg_ccb, fastrg_ccb.lcore.wan_ctrl_thread);
        rte_eal_remote_launch((lcore_function_t *)lan_combined_rx, (void *)&fastrg_ccb, fastrg_ccb.lcore.lan_ctrl_thread);
    }

    if (northbound(&fastrg_ccb) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "Northbound initialization failed");
        goto err;
    }

    rte_atomic16_set(&start_flag, 1);

    while(1) {
        struct signalfd_siginfo si;
        ssize_t s = read(sfd, &si, sizeof(si));
        if (s < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                usleep(100000); // prevent busy waiting
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
    close(sfd);
    if (fastrg_ccb.fp && fastrg_ccb.fp != stdout)
        fclose(fastrg_ccb.fp);
    if (fastrg_ccb.eal_args) free(fastrg_ccb.eal_args);
    if (fastrg_ccb.unix_sock_path) free(fastrg_ccb.unix_sock_path);
    if (fastrg_ccb.node_grpc_ip_port) free(fastrg_ccb.node_grpc_ip_port);
    if (fastrg_ccb.controller_address) free(fastrg_ccb.controller_address);
    if (fastrg_ccb.etcd_endpoints) free(fastrg_ccb.etcd_endpoints);
    grpc_shutdown();
    rte_eal_cleanup();
    return -1;
}
