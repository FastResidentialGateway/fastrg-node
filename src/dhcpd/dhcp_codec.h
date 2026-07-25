/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DHCP_CODEC.H

  Designed by THE on MAR 21, 2021
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DHCP_CODEC_H_
#define _DHCP_CODEC_H_

#include <common.h>

#include <rte_udp.h>
#include <rte_timer.h>
#include <rte_ether.h>

#include "../protocol.h"
#include "dhcpd.h"

#define LEASE_TIMEOUT 3600 // 1 hour
#define RENEW_TIMEOUT (LEASE_TIMEOUT / 2)
#define REBIND_TIMEOUT ((LEASE_TIMEOUT * 7) / 8)

#define DHCP_MAGIC_COOKIE 0x63825363
#define DHCP_HW_TYPE_ETHERNET 0x01

S16 dhcp_decode(dhcp_ccb_t *dhcp_ccb, 
    dhcp_ccb_per_lan_user_t *per_lan_user, int *cur_tmp_pool_index,
    struct rte_ether_hdr *eth_hdr, vlan_header_t *vlan_header, 
    struct rte_ipv4_hdr *ip_hdr, struct rte_udp_hdr *udp_hdr);

#define DHCP_SUBNET_MASK     1
#define DHCP_ROUTER          3
#define DHCP_DNS             6
#define DHCP_HOSTNAME        12
#define DHCP_REQUEST_IP      50
#define DHCP_LEASE_TIME      51
#define DHCP_MSG_TYPE        53
#define DHCP_SERVER_ID       54
#define DHCP_PARAMETER_LIST  55
#define DHCP_RENEWAL_VAL     58
#define DHCP_REBIND_TIME_VAL 59
#define DHCP_ISP_ID          60
#define DHCP_CLIENT_ID       61
#define DHCP_END             255

enum {
    DHCP_DISCOVER = 1,
    DHCP_OFFER,
    DHCP_REQUEST,
    DHCP_DECLINE,
    DHCP_ACK,
    DHCP_NAK,
    DHCP_RELEASE,
    DHCP_INFORM,
    DHCP_FORCE_RENEW,
    DHCP_LEASE_QUERY,
    DHCP_LEASE_UNASSIGNED,
    DHCP_LEASE_UNKNOWN,
    DHCP_LEASE_ACTIVE,
};

/**
 * @fn dhcp_select_dns_servers
 *
 * @brief Select the DNS server pair advertised in DHCP replies.
 *
 * @param dns_proxy_enabled
 *      Whether the subscriber DNS proxy is enabled.
 * @param dhcp_server_ip
 *      DHCP server IPv4 address in network byte order.
 * @param dp_started
 *      Whether the subscriber data plane has started.
 * @param hsi_primary_dns
 *      Primary HSI DNS server in network byte order.
 * @param hsi_secondary_dns
 *      Secondary HSI DNS server in network byte order.
 * @param dns_1
 *      Selected primary DNS server output.
 * @param dns_2
 *      Selected secondary DNS server output.
 * @return void
 */
void dhcp_select_dns_servers(BOOL dns_proxy_enabled, U32 dhcp_server_ip,
    BOOL dp_started, U32 hsi_primary_dns, U32 hsi_secondary_dns,
    U32 *dns_1, U32 *dns_2);

STATUS build_dhcp_offer(dhcp_ccb_per_lan_user_t *per_lan_user, struct rte_ether_addr *lan_mac);
STATUS build_dhcp_ack(dhcp_ccb_per_lan_user_t *per_lan_user, struct rte_ether_addr *lan_mac);
STATUS build_dhcp_nak(dhcp_ccb_per_lan_user_t *per_lan_user, struct rte_ether_addr *lan_mac);
STATUS build_dhcp_ack_inform(dhcp_ccb_per_lan_user_t *per_lan_user, 
    struct rte_ether_addr *lan_mac);

#endif
