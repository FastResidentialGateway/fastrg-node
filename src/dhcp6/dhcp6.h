#ifndef _DHCP6_H_
#define _DHCP6_H_

#include <common.h>

#include <rte_timer.h>

#include "../pppd/pppd.h"

#define DHCP6_S_IDLE        0
#define DHCP6_S_SOLICITING  1
#define DHCP6_S_REQUESTING  2
#define DHCP6_S_BOUND       3
#define DHCP6_S_RENEWING    4

#define DHCP6_SOLICIT       1
#define DHCP6_ADVERTISE     2
#define DHCP6_REQUEST       3
#define DHCP6_RENEW         5
#define DHCP6_REPLY         7

#define DHCP6_OPT_CLIENTID    1
#define DHCP6_OPT_SERVERID    2
#define DHCP6_OPT_STATUS      13
#define DHCP6_OPT_DNS_SERVERS 23
#define DHCP6_OPT_IA_PD       25
#define DHCP6_OPT_IAPREFIX    26

#define DHCP6_CLIENT_PORT       546
#define DHCP6_SERVER_PORT       547
#define DHCP6_STATUS_NO_PREFIX  6
#define DHCP6_PD_PLEN           56
#define DHCP6_LAN_PLEN          64
#define DHCP6_DEFAULT_T1        21600
#define DHCP6_DUID_LL_LEN       10
#define DHCP6_PACKET_MAX_LEN    512

void dhcp6_pd_start(ppp_ccb_t *ppp_ccb);
void dhcp6_pd_stop(ppp_ccb_t *ppp_ccb);
void dhcp6_wan_input(ppp_ccb_t *ppp_ccb, U8 *ipv6_pkt, U16 len);
void dhcp6_timer_cb(struct rte_timer *tim, void *arg);

/* Codec/test seams used by the production state machine and byte fixtures. */
STATUS dhcp6_build_packet(ppp_ccb_t *ppp_ccb, U8 msg_type, U8 *buffer,
    U16 *packet_len);
STATUS dhcp6_process_message(ppp_ccb_t *ppp_ccb, const U8 *dhcp, U16 len);
void dhcp6_derive_lan_prefix(const U8 pd_prefix[16], U8 pd_plen,
    U8 lan_prefix[16]);

#endif
