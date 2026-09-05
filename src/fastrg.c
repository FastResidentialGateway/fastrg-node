#define _GNU_SOURCE
#include <sys/signalfd.h>
#include <stdatomic.h>
#include <grpc/grpc.h>

#include <common.h>
#include <ip_codec.h>

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

#include "etcd_event.h"
#include "pppd/fsm.h"
#include "dp.h"
#include "dbg.h"
#include "init.h"
#include "dp_flow.h"
#include "dhcpd/dhcpd.h"
#include "dnsd/dnsd.h"
#include "nd6/nd6.h"
#include "config.h"
#include "controller.h"
#include "etcd_integration.h"
#include "northbound.h"
#include "cli_request.h"
#include "kafka_producer.h"
#include "config_snapshot.h"
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
            struct per_ccb_stats *row = fastrg_ccb->per_subscriber_stats[lcore_id][i];
            if (row == NULL)
                continue;
            memset(&row[old_count - disable_count], 0,
                disable_count * sizeof(struct per_ccb_stats));
        }
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, 
        "%u subscriber stats entries disabled (not freed)", disable_count);

    return SUCCESS;
}

void fastrg_cleanup_subscriber_stats(FastRG_t *fastrg_ccb)
{
    if (fastrg_ccb == NULL)
        return;

    // Free every per-lcore x per-port stats row
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

/* ---- Per-lcore PPPoE session stats: same scheme as per_subscriber_stats
 * above, single-dimension. Rows are fixed at max_user_count+1
 * entries. ---- */

STATUS fastrg_disable_pppoes_stats(FastRG_t *fastrg_ccb, U16 disable_count, U16 old_count)
{
    if (disable_count > old_count) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Invalid disabling count %u", disable_count);
        return ERROR;
    }
    if (disable_count == 0) {
        FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL, "disable_count is 0, nothing to do");
        return SUCCESS;
    }

    // Reset the disabled range [old_count-disable_count, old_count) in place.
    unsigned int lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        struct pppoes_lcore_stats *row = fastrg_ccb->pppoes_stats[lcore_id];
        if (row == NULL)
            continue;
        memset(&row[old_count - disable_count], 0,
            disable_count * sizeof(struct pppoes_lcore_stats));
    }

    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
        "%u pppoes stats entries disabled (not freed)", disable_count);
    return SUCCESS;
}

void fastrg_cleanup_pppoes_stats(FastRG_t *fastrg_ccb)
{
    if (fastrg_ccb == NULL)
        return;

    unsigned int lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        if (fastrg_ccb->pppoes_stats[lcore_id] != NULL) {
            fastrg_mfree(fastrg_ccb->pppoes_stats[lcore_id]);
            fastrg_ccb->pppoes_stats[lcore_id] = NULL;
        }
    }
}

static STATUS northbound_event_post(FastRG_t *fastrg_ccb, fastrg_event_type_t event_type,
    U8 cmd_type, U16 ccb_id, void *payload, U32 seq)
{
    /* Try to get a free mail slot from free_mail_ring */
    tFastRG_MBX *slot = NULL;
    fastrg_event_northbound_msg_t *northbound_msg;

    /* Get a free mail slot */
    if (rte_ring_dequeue(fastrg_ccb->free_mail_ring, (void **)&slot) == 0) {
        northbound_msg = &slot->northbound_msg;
        northbound_msg->cmd = cmd_type;
        northbound_msg->ccb_id = ccb_id;
        northbound_msg->seq = seq;
        northbound_msg->payload = payload;
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

STATUS fastrg_gen_northbound_event(FastRG_t *fastrg_ccb, fastrg_event_type_t event_type,
    U8 cmd_type, U16 ccb_id, void *payload)
{
    /* seq 0: fire and forget, no caller waits for a verdict. */
    return northbound_event_post(fastrg_ccb, event_type, cmd_type, ccb_id, payload, 0);
}

STATUS fastrg_gen_cli_request(FastRG_t *fastrg_ccb, fastrg_event_type_t event_type,
    U8 cmd_type, U16 ccb_id, void *payload, U32 seq)
{
    return northbound_event_post(fastrg_ccb, event_type, cmd_type, ccb_id, payload, seq);
}

STATUS fastrg_gen_etcd_event(FastRG_t *fastrg_ccb, etcd_event_t *ev)
{
    tFastRG_MBX *slot = NULL;

    if (fastrg_ccb == NULL || ev == NULL)
        return ERROR;
    if (rte_ring_dequeue(fastrg_ccb->free_mail_ring, (void **)&slot) != 0)
        return ERROR;

    slot->type = EV_ETCD;
    slot->etcd_ev = ev;
    slot->len = 0;              /* the event carries its own payload */
    /* cp_q is full: return the slot and leave the event with the caller. */
    if (rte_ring_enqueue(fastrg_ccb->cp_q, slot) != 0) {
        slot->etcd_ev = NULL;
        rte_ring_enqueue(fastrg_ccb->free_mail_ring, slot);
        return ERROR;
    }
    return SUCCESS;
}

void link_disconnect(__attribute__((unused)) struct rte_timer *tim, FastRG_t *fastrg_ccb)
{
    for(int i=0; i<fastrg_ccb->user_count; i++)
        fastrg_gen_northbound_event(fastrg_ccb, EV_NORTHBOUND_PPPoE, PPPoE_CMD_FORCE_DISABLE, i, NULL);
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

    /* Boot loads config straight from the main lcore, so stay parked until it
     * sets start_flag; that keeps CCB writes single-threaded during startup. */
    while(rte_atomic16_read(&start_flag) == 0)
        rte_pause();

    fastrg_ccb->lcore_usage[rte_lcore_id()].role = "ctrl";
    while(rte_atomic16_read(&stop_flag) == 0) {
        uint64_t _t0 = fastrg_get_cur_cycles();
        burst_size = rte_ring_dequeue_burst(fastrg_ccb->cp_q, (void **)mail, RING_BURST_SIZE, NULL);
        for(int i=0; i<burst_size; i++) {
            recv_type = mail[i]->type;
            switch(recv_type) {
            case EV_NORTHBOUND_PPPoE: {
                /* process cli command */
                fastrg_event_northbound_msg_t *pppoe_msg = &mail[i]->northbound_msg;
                /* Commands that validate the ccb id themselves run ahead of the
                 * lookup below, so an out-of-range id still frees the payload and
                 * answers the waiting gRPC thread. */
                if (pppoe_msg->cmd == PPPoE_CMD_APPLY_CONFIG) {
                    hsi_config_t *cfg = (hsi_config_t *)pppoe_msg->payload;
                    STATUS apply_ret;

                    if (cli_request_dropped(fastrg_ccb, pppoe_msg, cfg) == TRUE) {
                        rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                        break;
                    }
                    /* is_update FALSE: a CLI apply always writes the whole config. */
                    apply_ret = apply_hsi_config(fastrg_ccb, pppoe_msg->ccb_id, cfg, FALSE);
                    if (apply_ret == SUCCESS)
                        reconcile_pppoe_desire(fastrg_ccb, pppoe_msg->ccb_id, cfg->desire_status);
                    free(cfg);
                    /* Publishing the verdict releases the waiter, so it comes last. */
                    cli_request_publish(fastrg_ccb, pppoe_msg->seq, apply_ret);
                    rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                    break;
                }
                if (pppoe_msg->cmd == PPPoE_CMD_SNAT_SET ||
                        pppoe_msg->cmd == PPPoE_CMD_SNAT_REMOVE) {
                    snat_fwd_req_t *req = (snat_fwd_req_t *)pppoe_msg->payload;
                    STATUS snat_ret = ERROR;

                    if (cli_request_dropped(fastrg_ccb, pppoe_msg, req) == TRUE) {
                        rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                        break;
                    }
                    if (req != NULL) {
                        snat_ret = (pppoe_msg->cmd == PPPoE_CMD_SNAT_SET) ?
                            set_snat_port_fwd(fastrg_ccb, pppoe_msg->ccb_id, req->eport,
                                req->dip, req->iport) :
                            remove_snat_port_fwd(fastrg_ccb, pppoe_msg->ccb_id, req->eport);
                        free(req);
                    }
                    cli_request_publish(fastrg_ccb, pppoe_msg->seq, snat_ret);
                    rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                    break;
                }
                if (pppoe_msg->cmd == PPPoE_CMD_REMOVE_CONFIG) {
                    if (cli_request_dropped(fastrg_ccb, pppoe_msg, NULL) == TRUE) {
                        rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                        break;
                    }
                    STATUS remove_ret = remove_hsi_config(fastrg_ccb, pppoe_msg->ccb_id);

                    cli_request_publish(fastrg_ccb, pppoe_msg->seq, remove_ret);
                    rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                    break;
                }
                /* pppd_get_ccb() indexes the pointer array without a bound check, so the
                 * range must be validated before the fetch. The array is RCU-protected and
                 * a slot may be transiently NULL while a config change (re)allocates it. */
                if (pppoe_msg->ccb_id >= fastrg_ccb->user_count) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Drop pppoe event with out of range ccb id %d\n",
                        pppoe_msg->ccb_id);
                    cli_request_publish(fastrg_ccb, pppoe_msg->seq, ERROR);
                    rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                    break;
                }
                ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, pppoe_msg->ccb_id);
                if (ppp_ccb == NULL) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Drop pppoe event, user %d ppp ccb is not initialized\n",
                        pppoe_msg->ccb_id + 1);
                    cli_request_publish(fastrg_ccb, pppoe_msg->seq, ERROR);
                    rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                    break;
                }
                if (pppoe_msg->cmd == PPPoE_CMD_DISABLE) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %d pppoe is terminating\n", pppoe_msg->ccb_id + 1);
                    if (ppp_disconnect(ppp_ccb) == SUCCESS) {
                        /* PPPoE "disconnecting" transition → controller via Kafka. */
                        char uid[8];
                        snprintf(uid, sizeof(uid), "%u", pppoe_msg->ccb_id + 1);
                        kafka_report_pppoe_state(uid, KAFKA_PPPOE_DISCONNECTING, NULL, NULL, NULL, NULL, NULL, NULL);
                    }
                } else if (pppoe_msg->cmd == PPPoE_CMD_ENABLE) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %d pppoe is spawning\n", pppoe_msg->ccb_id + 1);
                    if (ppp_connect(ppp_ccb) == SUCCESS) {
                        /* PPPoE "connecting" transition → controller via Kafka. */
                        char uid[8];
                        snprintf(uid, sizeof(uid), "%u", pppoe_msg->ccb_id + 1);
                        kafka_report_pppoe_state(uid, KAFKA_PPPOE_CONNECTING, NULL, NULL, NULL, NULL, NULL, NULL);
                    }
                } else if (pppoe_msg->cmd == PPPoE_CMD_FORCE_DISABLE) {
                    FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "User %d pppoe is force terminating\n", pppoe_msg->ccb_id + 1);
                    fastrg_force_terminate_hsi(ppp_ccb);
                } else if (pppoe_msg->cmd == PPPoE_CMD_IPV6_CHANGED) {
                    if (is_ppp_ipv6_need_redial(TRUE, ppp_ccb->phase,
                            ppp_ccb->ppp_processing) == TRUE) {
                        ppp_ipv6_redial(ppp_ccb);
                    } else {
                        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                            "User %d ipv6_enable changed without redial, phase %u, ppp_processing %u\n",
                            pppoe_msg->ccb_id + 1, ppp_ccb->phase, ppp_ccb->ppp_processing);
                    }
                } else if (pppoe_msg->cmd == PPPoE_CMD_TCP_CONNTRACK_ENABLE ||
                        pppoe_msg->cmd == PPPoE_CMD_TCP_CONNTRACK_DISABLE) {
                    if (cli_request_dropped(fastrg_ccb, pppoe_msg, NULL) == FALSE) {
                        STATUS conntrack_ret = set_tcp_conntrack_enable(fastrg_ccb,
                            pppoe_msg->ccb_id,
                            pppoe_msg->cmd == PPPoE_CMD_TCP_CONNTRACK_ENABLE ? TRUE : FALSE);

                        cli_request_publish(fastrg_ccb, pppoe_msg->seq, conntrack_ret);
                    }
                } else if (pppoe_msg->cmd == PPPoE_CMD_IPV6_ENABLE ||
                        pppoe_msg->cmd == PPPoE_CMD_IPV6_DISABLE) {
                    if (cli_request_dropped(fastrg_ccb, pppoe_msg, NULL) == FALSE) {
                        STATUS ipv6_ret = set_ipv6_enable(fastrg_ccb, pppoe_msg->ccb_id,
                            pppoe_msg->cmd == PPPoE_CMD_IPV6_ENABLE ? TRUE : FALSE);

                        cli_request_publish(fastrg_ccb, pppoe_msg->seq, ipv6_ret);
                    }
                }
                rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                break;
            }
            case EV_NORTHBOUND_DHCP: {
                fastrg_event_northbound_msg_t *dhcp_msg = &mail[i]->northbound_msg;
                /* Same as the PPPoE branch: bound check before the unchecked array index,
                 * then NULL check on the RCU-protected slot. */
                if (dhcp_msg->ccb_id >= fastrg_ccb->user_count) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Drop dhcp event with out of range ccb id %d\n",
                        dhcp_msg->ccb_id);
                    cli_request_publish(fastrg_ccb, dhcp_msg->seq, ERROR);
                    rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                    break;
                }
                dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, dhcp_msg->ccb_id);
                if (dhcp_ccb == NULL) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Drop dhcp event, user %d dhcp ccb is not initialized\n",
                        dhcp_msg->ccb_id + 1);
                    cli_request_publish(fastrg_ccb, dhcp_msg->seq, ERROR);
                    rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                    break;
                }
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
            case EV_NORTHBOUND_DNS: {
                fastrg_event_northbound_msg_t *dns_msg = &mail[i]->northbound_msg;
                /* DNS_CMD_RECORD_ADD and DNS_CMD_RECORD_REMOVE contain payload 
                dns_record_req_t and others don't, so we need to deal with it alone. */
                if (dns_msg->cmd == DNS_CMD_RECORD_ADD ||
                        dns_msg->cmd == DNS_CMD_RECORD_REMOVE) {
                    dns_record_req_t *req = (dns_record_req_t *)dns_msg->payload;
                    STATUS record_ret = ERROR;

                    if (cli_request_dropped(fastrg_ccb, dns_msg, req) == TRUE) {
                        rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                        break;
                    }
                    if (req != NULL) {
                        if (is_valid_ccb_id(fastrg_ccb, dns_msg->ccb_id)) {
                            dns_static_table_t *table =
                                &DHCPD_GET_CCB(fastrg_ccb, dns_msg->ccb_id)->dns_state.static_table;

                            record_ret = (dns_msg->cmd == DNS_CMD_RECORD_ADD) ?
                                dns_static_add(table, req->domain, req->ip_addr, req->ttl) :
                                dns_static_remove(table, req->domain);
                        }
                        free(req);
                    }
                    cli_request_publish(fastrg_ccb, dns_msg->seq, record_ret);
                    rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                    break;
                }
                if (dns_msg->ccb_id >= fastrg_ccb->user_count) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Drop dns event with out of range ccb id %d\n",
                        dns_msg->ccb_id);
                    cli_request_publish(fastrg_ccb, dns_msg->seq, ERROR);
                    rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                    break;
                }
                dhcp_ccb_t *dns_owner_ccb = DHCPD_GET_CCB(fastrg_ccb, dns_msg->ccb_id);
                if (dns_owner_ccb == NULL) {
                    FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Drop dns event, user %d dhcp ccb is not initialized\n",
                        dns_msg->ccb_id + 1);
                    cli_request_publish(fastrg_ccb, dns_msg->seq, ERROR);
                    rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                    break;
                }
                if (dns_msg->cmd == DNS_CMD_PROXY_ENABLE ||
                        dns_msg->cmd == DNS_CMD_PROXY_DISABLE) {
                    if (cli_request_dropped(fastrg_ccb, dns_msg, NULL) == FALSE) {
                        STATUS dns_proxy_ret = set_dns_proxy_enable(fastrg_ccb, dns_msg->ccb_id,
                            dns_msg->cmd == DNS_CMD_PROXY_ENABLE ? TRUE : FALSE);

                        cli_request_publish(fastrg_ccb, dns_msg->seq, dns_proxy_ret);
                    }
                } else if (dns_msg->cmd == DNS_CMD_CACHE_FLUSH) {
                    dns_cache_flush_t *req = (dns_cache_flush_t *)dns_msg->payload;

                    if (cli_request_dropped(fastrg_ccb, dns_msg, NULL) == FALSE) {
                        STATUS flush_ret = ERROR;

                        if (req != NULL) {
                            req->flushed = dns_cache_flush(&dns_owner_ccb->dns_state.cache);
                            flush_ret = SUCCESS;
                        }
                        cli_request_publish(fastrg_ccb, dns_msg->seq, flush_ret);
                    }
                } else if (dns_msg->cmd == DNS_CMD_CACHE_DUMP) {
                    dns_cache_dump_t *dump = (dns_cache_dump_t *)dns_msg->payload;
                    if (cli_request_dropped(fastrg_ccb, dns_msg, NULL) == FALSE) {
                        STATUS dump_ret = ERROR;
                        if (dump != NULL) {
                            /* Size the array to what the cache holds right now, so
                             * caller side doesn't need to guess. */
                            U32 entries = dns_owner_ccb->dns_state.cache.entry_count;

                            dump->entries = entries != 0 ?
                                malloc((size_t)entries * sizeof(*dump->entries)) : NULL;
                            if (entries == 0 || dump->entries != NULL) {
                                dump->count = dns_cache_dump(&dns_owner_ccb->dns_state.cache,
                                    dump->entries, entries);
                                dump_ret = SUCCESS;
                            }
                        }
                        cli_request_publish(fastrg_ccb, dns_msg->seq, dump_ret);
                    }
                } else if (dns_msg->cmd == DNS_CMD_STATIC_DUMP) {
                    dns_static_dump_t *dump = (dns_static_dump_t *)dns_msg->payload;

                    if (cli_request_dropped(fastrg_ccb, dns_msg, NULL) == FALSE) {
                        STATUS dump_ret = ERROR;

                        if (dump != NULL) {
                            U32 records = dns_owner_ccb->dns_state.static_table.count;

                            dump->records = records != 0 ?
                                malloc((size_t)records * sizeof(*dump->records)) : NULL;
                            if (records == 0 || dump->records != NULL) {
                                dump->count = dns_static_dump(
                                    &dns_owner_ccb->dns_state.static_table,
                                    dump->records, records);
                                dump_ret = SUCCESS;
                            }
                        }
                        cli_request_publish(fastrg_ccb, dns_msg->seq, dump_ret);
                    }
                }
                rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                break;
            }
            case EV_NORTHBOUND_NODE: {
                fastrg_event_northbound_msg_t *node_msg = &mail[i]->northbound_msg;
                if (node_msg->cmd == NODE_CMD_SET_USER_COUNT) {
                    user_count_config_t *cfg = (user_count_config_t *)node_msg->payload;
                    STATUS count_ret = ERROR;

                    if (cli_request_dropped(fastrg_ccb, node_msg, cfg) == TRUE) {
                        rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                        break;
                    }
                    if (cfg != NULL) {
                        count_ret = user_count_changed_callback("", cfg, HSI_ACTION_UPDATE,
                            0, fastrg_ccb);
                        free(cfg);
                    }
                    cli_request_publish(fastrg_ccb, node_msg->seq, count_ret);
                }
                rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                break;
            }
            case EV_ETCD: {
                etcd_event_dispatch(fastrg_ccb, mail[i]->etcd_ev);
                etcd_event_free(mail[i]->etcd_ev);
                mail[i]->etcd_ev = NULL;
                rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                break;
            }
            case EV_LINK: {
                FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL, "Recv Link Up/Down event");
                U16 link_port = mail[i]->link.port;
                /* Update per-port link state cache for Prometheus metrics. Count a flap
                 * whenever the state actually transitions (caught even if it toggles
                 * faster than the scrape interval). */
                if (link_port < PORT_AMOUNT) {
                    U8 new_up = (mail[i]->link.up_down == LINK_UP) ? 1 : 0;
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
                    if (mail[i]->link.up_down == LINK_DOWN) {
                        if (rte_timer_reset(&fastrg_ccb->link,
                                LINK_DOWN_TIMEOUT * fastrg_get_cycles_in_sec(), // 10 seconds
                                SINGLE, fastrg_ccb->lcore.timer_thread,
                                (rte_timer_cb_t)link_disconnect, fastrg_ccb) == -1) {
                            /* -1 only when the timer is RUNNING, i.e. link_disconnect
                             * is executing on the timer lcore at this very moment: the
                             * disconnect this rearm would schedule is already happening,
                             * so skipping the redundant rearm loses no protection. */
                            FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
                                "Link-down timer rearm skipped: its disconnect callback is running on another lcore and already handling this link down");
                        }
                    } else if (mail[i]->link.up_down == LINK_UP) {
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
                int ret = dhcpd(fastrg_ccb, mail[i]->mbuf, eth_hdr, vlan_header,
                    ip_hdr, udp_hdr, ccb_id);
                if (ret > 0) {
                    U16 out_len = sizeof(struct rte_ether_hdr) + sizeof(vlan_header_t) +
                        rte_be_to_cpu_16(ip_hdr->total_length);
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
            case EV_DP_ICMP6: {
                U16 ccb_id = mail[i]->ccb_id;
                U8 *pkt_data = rte_pktmbuf_mtod(mail[i]->mbuf, U8 *);

                nd6_lan_input(fastrg_ccb, ccb_id, pkt_data, mail[i]->len);
                rte_pktmbuf_free(mail[i]->mbuf);
                rte_ring_enqueue(fastrg_ccb->free_mail_ring, mail[i]);
                break;
            }
            case EV_DP_ND6_MISS: {
                U16 ccb_id = mail[i]->ccb_id;
                U8 *pkt_data = rte_pktmbuf_mtod(mail[i]->mbuf, U8 *);

                nd6_wan_miss_input(fastrg_ccb, ccb_id, pkt_data, mail[i]->len);
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

        cur_tsc = fastrg_get_cur_cycles();
        diff_tsc = cur_tsc - prev_tsc;
        if (diff_tsc >= timer_resolution_cycles) {
            rte_timer_manage();
            prev_tsc = cur_tsc;
        }

        uint64_t _elapsed = fastrg_get_cur_cycles() - _t0;
        fastrg_ccb->lcore_usage[rte_lcore_id()].total_cycles += _elapsed;
        if (burst_size > 0)
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

static void fastrg_stop_northbound_threads(FastRG_t *fastrg_ccb)
{
    if (fastrg_ccb->grpc_thread_started)
        fastrg_grpc_server_shutdown();

    if (fastrg_ccb->metrics_thread_started) {
        rte_atomic16_set(&fastrg_ccb->metrics_stop_requested, 1);
        lighthttp_stop(&fastrg_ccb->metrics_server);
    }

    if (fastrg_ccb->grpc_thread_started) {
        int ret = pthread_join(fastrg_ccb->grpc_thread, NULL);
        if (ret != 0)
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                "Failed to join gRPC server thread: %s", strerror(ret));
        fastrg_ccb->grpc_thread_started = FALSE;
    }

    if (fastrg_ccb->metrics_thread_started) {
        int ret = pthread_join(fastrg_ccb->metrics_thread, NULL);
        if (ret != 0)
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                "Failed to join metrics server thread: %s", strerror(ret));
        fastrg_ccb->metrics_thread_started = FALSE;
    }
}

STATUS northbound(FastRG_t *fastrg_ccb)
{
    // Initialize controller client
    if (controller_init(fastrg_ccb) != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL, "Controller initialization failed");
        return ERROR;
    }

    unlink(fastrg_ccb->unix_sock_path);

    fastrg_ccb->metrics_thread_started = FALSE;
    fastrg_ccb->grpc_thread_started = FALSE;
    fastrg_ccb->metrics_server.listen_fd = -1;
    rte_atomic16_set(&fastrg_ccb->metrics_stop_requested, 0);

    /* Startup is gated on the /metrics listener coming up; a scrape that fails
     * later is not fatal. */
    if (fastrg_create_pthread("fastrg_metrics",
        metrics_server_run, fastrg_ccb, rte_lcore_id(), &fastrg_ccb->metrics_thread) != SUCCESS) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Metrics HTTP server thread failed to start; shutting down");
        return ERROR;
    }
    fastrg_ccb->metrics_thread_started = TRUE;

    if (metrics_server_wait_ready() != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "Metrics HTTP server failed to start on %s; shutting down",
            fastrg_ccb->metrics_ip_port);
        fastrg_stop_northbound_threads(fastrg_ccb);
        return ERROR;
    }

    if (fastrg_create_pthread("fastrg_grpc",
        fastrg_grpc_server_run, fastrg_ccb, rte_lcore_id(), &fastrg_ccb->grpc_thread) != SUCCESS) {
        fastrg_stop_northbound_threads(fastrg_ccb);
        return ERROR;
    }
    fastrg_ccb->grpc_thread_started = TRUE;

    if (fastrg_grpc_server_wait_ready() != 0) {
        FastRG_LOG(ERR, fastrg_ccb->fp, NULL, NULL,
            "gRPC server failed to start on %s / %s; shutting down",
            fastrg_ccb->unix_sock_path, fastrg_ccb->node_grpc_ip_port);
        fastrg_stop_northbound_threads(fastrg_ccb);
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

        /* Load the persisted config snapshot (SDN-only subsystem: the etcd
         * watch/load paths mirror into it, offline edits accumulate on it and
         * a degraded boot operates from it). MUST precede etcd integration —
         * the load path mirrors etcd values into the snapshot. */
        if (config_snapshot_init() != SUCCESS) {
            FastRG_LOG(WARN, fastrg_ccb->fp, NULL, NULL,
                "Config snapshot file unreadable; starting with an empty snapshot");
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

        U32 startup_events = ppp_report_all_connection_status(fastrg_ccb);
        FastRG_LOG(INFO, fastrg_ccb->fp, NULL, NULL,
            "Startup PPPoE state self-report produced %u event(s)", startup_events);
    }

    return SUCCESS;
}

void fastrg_stop()
{
    FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL, "FastRG system stopping...");
    rte_eal_mp_wait_lcore();
    /* Must follow rte_eal_mp_wait_lcore(): only then can the callback no
     * longer be running. */
    rte_timer_stop(&fastrg_ccb.nd6_age_timer);
    fastrg_stop_northbound_threads(&fastrg_ccb);
    // Cleanup Kafka producer (flush pending telemetry)
    kafka_producer_cleanup();
    // Cleanup etcd integration
    etcd_integration_cleanup(&fastrg_ccb);
    // Persist and release the config snapshot (init'd in northbound(), SDN mode)
    config_snapshot_cleanup();

    // Cleanup controller client
    if (fastrg_ccb.controller_address)
        controller_cleanup(&fastrg_ccb);

    if (fastrg_ccb.unix_sock_fd > 0)
        close(fastrg_ccb.unix_sock_fd);
    fastrg_ccb.user_count = 0;
    pppd_cleanup_ccb(&fastrg_ccb);
    dhcpd_cleanup_ccb(&fastrg_ccb);
    /* stop any active CLI capture session and free its state */
    fastrg_pdump_capture_cleanup(&fastrg_ccb);
    /* pdump_rcu is owned here, not by sys_cleanup: it must be freed right
     * after fastrg_pdump_capture_cleanup(), whose QSBR grace period is its
     * last reader. sys_cleanup never touches pdump_rcu. */
    if (fastrg_ccb.pdump_rcu != NULL) {
        fastrg_mfree(fastrg_ccb.pdump_rcu);
        fastrg_ccb.pdump_rcu = NULL;
    }
    #ifdef RTE_LIB_PDUMP
    /*uninitialize packet capture framework */
    rte_pdump_uninit();
    #endif
    /* Stop and close both ports before any ring/pool teardown: closing a port
     * makes the driver return every mbuf still sitting in its RX/TX queue
     * descriptors to the mempools, so cleanup_mem (via sys_cleanup below) only
     * frees pools the NIC no longer references. Must come after
     * rte_pdump_uninit so the pdump RX/TX callbacks are detached first, and
     * before the cp_q drain so the LSC callback is unregistered before the
     * drain runs. */
    for(U16 port=0; port<PORT_AMOUNT; port++)
        PORT_CLOSE(&fastrg_ccb, port);
    /* Drain any mail left unconsumed in cp_q; the ring itself is freed by
     * sys_cleanup below. Placed after PORT_CLOSE: the LSC callback (the last
     * possible producer, running on the interrupt thread) is unregistered
     * there, so nothing can enqueue after this drain. EV_DP_* mails own an
     * mbuf that must be freed; EV_ETCD mails own a heap etcd event that only
     * etcd_event_free can release; EV_LINK mails are individually allocated
     * (see lsi_event_callback) and are freed here. Every other mail is a
     * borrowed pool slot: hand it back to free_mail_ring so cleanup_ring (via
     * sys_cleanup) frees every slot in one place. free_mail_ring is sized for
     * the whole pool, so the give-back cannot fail. */
    if (fastrg_ccb.cp_q) {
        tFastRG_MBX *left_mail;
        while (rte_ring_dequeue(fastrg_ccb.cp_q, (void **)&left_mail) == 0) {
            switch (left_mail->type) {
            case EV_DP_PPPoE:
            case EV_DP_DNS:
            case EV_DP_DHCP:
            case EV_DP_ICMP6:
            case EV_DP_ND6_MISS:
                if (left_mail->mbuf)
                    rte_pktmbuf_free(left_mail->mbuf);
                rte_ring_enqueue(fastrg_ccb.free_mail_ring, left_mail);
                break;
            case EV_ETCD:
                etcd_event_free(left_mail->etcd_ev);
                left_mail->etcd_ev = NULL;
                rte_ring_enqueue(fastrg_ccb.free_mail_ring, left_mail);
                break;
            case EV_LINK:
                fastrg_mfree(left_mail);
                break;
            default:
                rte_ring_enqueue(fastrg_ccb.free_mail_ring, left_mail);
                break;
            }
        }
    }
    //rte_trace_save();
    grpc_shutdown();
    if (fastrg_ccb.vlan_userid_map != NULL) {
        fastrg_mfree(fastrg_ccb.vlan_userid_map);
        fastrg_ccb.vlan_userid_map = NULL;
    }
    fastrg_cleanup_subscriber_stats(&fastrg_ccb);
    fastrg_cleanup_pppoes_stats(&fastrg_ccb);
    /* Single-owner teardown of everything sys_init created: the stats rows
     * (already NULL after the two cleanups above, so the guarded loops are
     * no-ops), the arp_pending pool (its per-ccb queues were returned by
     * pppd_cleanup_ccb above), node_uuid, lcore_usage, the applied-revision
     * table, both rings (cp_q / free_mail_ring including every mail slot) and
     * the mbuf pools. Must precede rte_eal_cleanup(): the rings and pools
     * live in EAL hugepage memory. */
    sys_cleanup(&fastrg_ccb);
    // Free allocated memory from config file
    if (fastrg_ccb.eal_args) free(fastrg_ccb.eal_args);
    if (fastrg_ccb.log_path) free(fastrg_ccb.log_path);
    if (fastrg_ccb.unix_sock_path) free(fastrg_ccb.unix_sock_path);
    if (fastrg_ccb.node_grpc_ip_port) free(fastrg_ccb.node_grpc_ip_port);
    if (fastrg_ccb.controller_address) free(fastrg_ccb.controller_address);
    if (fastrg_ccb.etcd_endpoints) free(fastrg_ccb.etcd_endpoints);
    if (fastrg_ccb.kafka_brokers) free(fastrg_ccb.kafka_brokers);
    if (fastrg_ccb.central_office_location) free(fastrg_ccb.central_office_location);
    if (fastrg_ccb.metrics_ip_port) free(fastrg_ccb.metrics_ip_port);
    /* Logged after every teardown step above: a crash during teardown leaves
     * the log without this line, so a missing "bye!" is itself a signal. */
    FastRG_LOG(INFO, fastrg_ccb.fp, NULL, NULL, "bye!");

    rte_eal_cleanup();

    /* Close the log file last so DPDK cleanup logs (e.g. i40e
     * "Invalid memory" during PCI device uninit) are written to the
     * still-open log file. */
    if (fastrg_ccb.fp && fastrg_ccb.fp != stdout)
        fclose(fastrg_ccb.fp);
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
    fastrg_ccb.node_grpc_port = fastrg_cfg.node_grpc_port;
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

    /* init users and ports info */
    /* vlan 1 is mapped to index 0. However, vlan 1 is not assigned to any user by default, 
    so index 0 is not used */
    fastrg_ccb.vlan_userid_map = fastrg_malloc(rte_atomic16_t, MAX_VLAN_ID * sizeof(rte_atomic16_t), 0);
    if (fastrg_ccb.vlan_userid_map == NULL) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "Cannot allocate memory for vlan_userid_map");
        goto err;
    }
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
            U16 wan_tx_q = get_tx_queue_id_for_sender(FASTRG_TX_SENDER_WAN_DATA, i, LAN_PORT, num_dq);
            U16 lan_tx_q = get_tx_queue_id_for_sender(FASTRG_TX_SENDER_LAN_DATA, i, WAN_PORT, num_dq);
            if (wan_tx_q == FASTRG_TX_QUEUE_NONE || lan_tx_q == FASTRG_TX_QUEUE_NONE) {
                FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL,
                    "Data queue %u has no TX queue in the layout", i);
                goto err;
            }
            wan_data_args[i].fastrg_ccb = &fastrg_ccb;
            wan_data_args[i].rx_queue_id = queue_id;
            wan_data_args[i].tx_queue_id = wan_tx_q;
            rte_eal_remote_launch((lcore_function_t *)wan_data_rx,
                (void *)&wan_data_args[i], fastrg_ccb.lcore.wan_data_threads[i]);

            lan_data_args[i].fastrg_ccb = &fastrg_ccb;
            lan_data_args[i].rx_queue_id = queue_id;
            lan_data_args[i].tx_queue_id = lan_tx_q;
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
            U16 wan_tx_q = get_tx_queue_id_for_sender(FASTRG_TX_SENDER_WAN_DATA, i, LAN_PORT, num_dq);
            U16 lan_tx_q = get_tx_queue_id_for_sender(FASTRG_TX_SENDER_LAN_DATA, i, WAN_PORT, num_dq);
            if (wan_tx_q == FASTRG_TX_QUEUE_NONE || lan_tx_q == FASTRG_TX_QUEUE_NONE) {
                FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL,
                    "Worker %u has no TX queue in the layout", i);
                goto err;
            }
            wan_worker_args[i].fastrg_ccb = &fastrg_ccb;
            wan_worker_args[i].dist = fastrg_ccb.wan_dist;
            wan_worker_args[i].worker_id = i;
            wan_worker_args[i].tx_queue_id = wan_tx_q;
            rte_eal_remote_launch((lcore_function_t *)wan_dist_worker,
                (void *)&wan_worker_args[i], fastrg_ccb.lcore.wan_data_threads[i]);

            lan_worker_args[i].fastrg_ccb = &fastrg_ccb;
            lan_worker_args[i].dist = fastrg_ccb.lan_dist;
            lan_worker_args[i].worker_id = i;
            lan_worker_args[i].tx_queue_id = lan_tx_q;
            rte_eal_remote_launch((lcore_function_t *)lan_dist_worker,
                (void *)&lan_worker_args[i], fastrg_ccb.lcore.lan_data_threads[i]);
        }
    }

    if (northbound(&fastrg_ccb) == ERROR) {
        FastRG_LOG(ERR, fastrg_ccb.fp, NULL, NULL, "Northbound initialization failed");
        goto err;
    }

    /* Aging must run on the control-plane lcore: it is the nd6 table's only
     * writer. */
    rte_timer_reset(&fastrg_ccb.nd6_age_timer,
        (U64)ND6_AGE_SCAN_SEC * fastrg_get_cycles_in_sec(), PERIODICAL,
        fastrg_ccb.lcore.ctrl_thread, nd6_age_timer_cb, &fastrg_ccb);

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
    /* Enable start_flag to make sure dp lcores leave pause loop */
    rte_atomic16_set(&start_flag, 1);
    fastrg_stop();
    close(sfd);

    return -1;
}
