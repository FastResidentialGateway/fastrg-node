#include <stdint.h>

#include <common.h>

#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_byteorder.h>

#include "dhcp_fsm.h"
#include "../dbg.h"
#include "../pppd/pppd.h"

#define DHCP_OPTIONS_BUFFER_SIZE 256

typedef struct dhcp_opt {
    U8 opt_type;
    U8 len;
    U8 val[0];
}dhcp_opt_t;

typedef struct dhcp_hdr {
    U8 msg_type;                          // op: Message op code / message type
    U8 hwr_type;                          // htype: Hardware address type
    U8 hwr_addr_len;                      // hlen: Hardware address length
    U8 hops;                              // hops: Client sets to zero
    U32 transaction_id;                   // xid: Transaction ID
    U16 sec_elapsed;                      // secs: Seconds elapsed
    U16 bootp_flag;                       // flags: Flags
    U32 client_ip;                        // ciaddr: Client IP address
    U32 ur_client_ip;                     // yiaddr: 'Your' (client) IP address
    U32 next_server_ip;                   // siaddr: IP address of next server
    U32 relay_agent_ip;                   // giaddr: Relay agent IP address
    struct rte_ether_addr mac_addr;       // chaddr: Client hardware address (16 bytes)
    unsigned char mac_addr_padding[10];   // chaddr padding
    unsigned char server_name[64];        // sname: Server host name (optional)
    unsigned char file_name[128];         // file: Boot file name (optional)
    U32 magic_cookie;                     // magic cookie: 0x63825363 (RFC 1497)
    dhcp_opt_t opt_ptr[0];                // options: Variable length options field
}dhcp_hdr_t;

static inline BOOL is_client_in_pool(dhcp_ccb_t *dhcp_ccb, 
    struct rte_ether_addr *mac_addr, int cur_tmp_pool_index)
{
    int i;
    for(i=cur_tmp_pool_index; i<dhcp_ccb->per_lan_user_pool_len; i++) {
        if (dhcp_ccb->per_lan_user_pool[i]->ip_pool.used == TRUE || 
                rte_is_same_ether_addr(mac_addr, &dhcp_ccb->per_lan_user_pool[i]->ip_pool.mac_addr)) {
            return TRUE;
        }
    }

    /* If we don't find it, find from 0 */
    for(int j=0; j<i; j++) {
        if (dhcp_ccb->per_lan_user_pool[j]->ip_pool.used == TRUE || 
                rte_is_same_ether_addr(mac_addr, &dhcp_ccb->per_lan_user_pool[j]->ip_pool.mac_addr)) {
            return TRUE;
        }
    }
    return FALSE;
}

static inline BOOL check_and_set_single_pool_entry(dhcp_ccb_t *dhcp_ccb, 
    int index, struct rte_ether_addr *mac_addr, int *cur_tmp_pool_index)
{
    dhcp_ccb_per_lan_user_t *entry = dhcp_ccb->per_lan_user_pool[index];
    
    if (entry->ip_pool.used == FALSE) {
        entry->ip_pool.used = TRUE;
        rte_ether_addr_copy(mac_addr, &entry->ip_pool.mac_addr);
        *cur_tmp_pool_index = index;
        return TRUE;
    } else if (rte_is_same_ether_addr(mac_addr, &entry->ip_pool.mac_addr)) {
        *cur_tmp_pool_index = index;
        return TRUE;
    }
    
    return FALSE;
}

STATUS check_pool(dhcp_ccb_t *dhcp_ccb, dhcp_ccb_per_lan_user_t *per_lan_user, 
    int *cur_tmp_pool_index,struct rte_ether_addr mac_addr)
{
    int i;

    if (per_lan_user->ip_pool.used == FALSE) {
        rte_ether_addr_copy(&mac_addr, &per_lan_user->ip_pool.mac_addr);
        per_lan_user->ip_pool.used = TRUE;
        return SUCCESS;
    } else if (rte_is_same_ether_addr(&mac_addr, &per_lan_user->ip_pool.mac_addr)) {
        return SUCCESS;
    }

    for(i=*cur_tmp_pool_index; i<dhcp_ccb->per_lan_user_pool_len; i++) {
        if (check_and_set_single_pool_entry(dhcp_ccb, i, &mac_addr, cur_tmp_pool_index) == TRUE)
            return SUCCESS;
    }

    /* If we don't find it, find from 0 */
    for(int j=0; j<i; j++) {
        if (check_and_set_single_pool_entry(dhcp_ccb, j, &mac_addr, cur_tmp_pool_index) == TRUE)
            return SUCCESS;
    }

    FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, (U8 *)dhcp_ccb, DHCPLOGMSG,
        "subscriber %u: IP pool exhausted\n", dhcp_ccb->ccb_id);

    return ERROR;
}

DHCP_EVENT_TYPE decode_request(dhcp_ccb_per_lan_user_t *per_lan_user, int *cur_tmp_pool_index)
{
    dhcp_opt_t *opt_ptr = (dhcp_opt_t *)(per_lan_user->dhcp_hdr + 1);
    dhcp_opt_t *cur = opt_ptr;
    dhcp_ccb_t *dhcp_ccb = per_lan_user->dhcp_ccb;
    U16 dhcp_field_len = rte_be_to_cpu_16(dhcp_ccb->udp_hdr->dgram_len) - 
        sizeof(struct rte_udp_hdr) - sizeof(dhcp_hdr_t);
    struct rte_ether_addr mac_addr;

    /* per_lan_user->dhcp_hdr->ur_client_ip now is Yiaddr in previous DHCP offer,
    so we don't need to change it if it is a SELECTING DHCP request. At this point, 
    per_lan_user->dhcp_hdr->client_ip is 0 and it does not contain DHCP_REQUEST_IP option */

    rte_ether_addr_copy(&dhcp_ccb->eth_hdr->src_addr, &mac_addr);
    /* Ciaddr is non 0 means it is a DHCP renew or rebind request */
    if (per_lan_user->dhcp_hdr->client_ip != RTE_IPV4_ANY) {
        /* If the subscriber's ip pool is changed, we need to check if the ip address 
            is still valid */
        if ((per_lan_user->dhcp_hdr->client_ip & dhcp_ccb->subnet_mask) != 
                (dhcp_ccb->dhcp_server_ip & dhcp_ccb->subnet_mask))
            return E_BAD_REQUEST;
        per_lan_user->dhcp_hdr->ur_client_ip = per_lan_user->dhcp_hdr->client_ip;
        /* In DHCP ack, Ciaddr should be 0 */
        per_lan_user->dhcp_hdr->client_ip = 0;
        per_lan_user->lan_user_info.timeout_secs = LEASE_TIMEOUT;
        U16 cur_opt_len = 0;
        /* Check it is Renewal or Rebind */
        for(; cur->opt_type!=DHCP_END && cur_opt_len<dhcp_field_len; 
                cur=(dhcp_opt_t *)(((U8 *)(cur+1))+cur->len)) {
            if ((cur_opt_len + cur->len + sizeof(dhcp_opt_t)) > dhcp_field_len) {
                FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, (U8 *)dhcp_ccb, DHCPLOGMSG,
                    "subscriber %u: Malformed DHCP options\n", dhcp_ccb->ccb_id);
                return ERROR;
            }
            
            if (cur->opt_type == DHCP_SERVER_ID) {
                if (cur->len != sizeof(U32)) {
                    FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, (U8 *)dhcp_ccb, DHCPLOGMSG,
                        "subscriber %u: Malformed DHCP Server ID option\n", dhcp_ccb->ccb_id);
                    return ERROR;
                }
                U32 server_id;
                rte_memcpy(&server_id, cur->val, sizeof(U32));
                if (server_id != dhcp_ccb->dhcp_server_ip) {
                    return ERROR;
                } else {
                    /* It is Renewal */
                    per_lan_user->lan_user_info.timeout_secs = LEASE_TIMEOUT;
                    break;
                }
            }
            if (per_lan_user->dhcp_ccb->ip_hdr->dst_addr == RTE_IPV4_BROADCAST) {
                /* It is Rebind */
                per_lan_user->lan_user_info.timeout_secs = LEASE_TIMEOUT;
                break;
            }
            cur_opt_len += (cur->len + sizeof(dhcp_opt_t));
        }
    } else {
        U16 cur_opt_len = 0;
        for(; cur->opt_type!=DHCP_END && cur_opt_len<dhcp_field_len; 
                cur=(dhcp_opt_t *)(((U8 *)(cur+1))+cur->len)) {
            if ((cur_opt_len + cur->len + sizeof(dhcp_opt_t)) > dhcp_field_len) {
                FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, (U8 *)dhcp_ccb, DHCPLOGMSG, 
                    "subscriber %u: Malformed DHCP options\n", dhcp_ccb->ccb_id);
                return ERROR;
            }

            if (cur->opt_type == DHCP_CLIENT_ID) {
                /* Option 61 is client identifier with mac address */
                /* Option 61 format is | Type(1Byte) | Length(1Byte) | HW type(1Byte) | MAC Address | */
                if (cur->len < (sizeof(U8) + sizeof(struct rte_ether_addr))) {
                    FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, (U8 *)dhcp_ccb, 
                        DHCPLOGMSG, "subscriber %u: Malformed DHCP Client ID option\n", 
                        dhcp_ccb->ccb_id);
                    return ERROR;
                }
                if (cur->val[0] != DHCP_HW_TYPE_ETHERNET) {
                    FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, (U8 *)dhcp_ccb, 
                        DHCPLOGMSG, "subscriber %u: Unsupported Client ID hardware type 0x%02x (expected 0x01)\n",
                        dhcp_ccb->ccb_id, cur->val[0]);
                    return ERROR;
                }
                rte_ether_addr_copy((struct rte_ether_addr *)(cur->val+1), &mac_addr);
            } else if (cur->opt_type == DHCP_REQUEST_IP) {
                if (cur->len != sizeof(U32)) {
                    FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, 
                        (U8 *)dhcp_ccb, DHCPLOGMSG,
                        "subscriber %u: Malformed DHCP Request IP option\n", dhcp_ccb->ccb_id);
                    return ERROR;
                }
                /* This means DHCP client hopes to request specific IP */
                rte_memcpy(&per_lan_user->dhcp_hdr->ur_client_ip, cur->val, sizeof(U32));
                /* If the subscriber's ip pool is changed, we need to check if the ip address 
                    is still valid */
                if ((per_lan_user->dhcp_hdr->ur_client_ip & dhcp_ccb->subnet_mask) != 
                        (dhcp_ccb->dhcp_server_ip & dhcp_ccb->subnet_mask))
                    return E_BAD_REQUEST;
            } else if (cur->opt_type == DHCP_SERVER_ID) {
                if (cur->len != sizeof(U32)) {
                    FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, 
                        (U8 *)dhcp_ccb, DHCPLOGMSG,
                        "subscriber %u: Malformed DHCP Server ID option\n", dhcp_ccb->ccb_id);
                    return ERROR;
                }
                U32 server_id;
                rte_memcpy(&server_id, cur->val, sizeof(U32));
                if (server_id != dhcp_ccb->dhcp_server_ip)
                    return ERROR;
            }
            cur_opt_len += (cur->len + sizeof(dhcp_opt_t));
        }
    }

    return check_pool(dhcp_ccb, per_lan_user, cur_tmp_pool_index, mac_addr) 
        == SUCCESS ? E_GOOD_REQUEST : E_BAD_REQUEST;
}

STATUS pick_ip_from_pool(dhcp_ccb_t *dhcp_ccb, dhcp_ccb_per_lan_user_t *per_lan_user, 
    U32 *ip_addr, struct rte_ether_addr mac_addr)
{
    int i;

    for(i=per_lan_user->pool_index; i<dhcp_ccb->per_lan_user_pool_len; i++) {
        if (rte_is_same_ether_addr(&mac_addr, &dhcp_ccb->per_lan_user_pool[i]->ip_pool.mac_addr)) {
            *ip_addr = dhcp_ccb->per_lan_user_pool[i]->ip_pool.ip_addr;
            return SUCCESS;
        }
        if (dhcp_ccb->per_lan_user_pool[i]->ip_pool.used == FALSE) {
            *ip_addr = dhcp_ccb->per_lan_user_pool[i]->ip_pool.ip_addr;
            rte_ether_addr_copy(&mac_addr, &dhcp_ccb->per_lan_user_pool[i]->ip_pool.mac_addr);
            return SUCCESS;
        }
    }
    for(int j=0; j<i; j++) {
        if (rte_is_same_ether_addr(&mac_addr, &dhcp_ccb->per_lan_user_pool[j]->ip_pool.mac_addr)) {
            *ip_addr = dhcp_ccb->per_lan_user_pool[j]->ip_pool.ip_addr;
            return SUCCESS;
        }
        if (dhcp_ccb->per_lan_user_pool[j]->ip_pool.used == FALSE) {
            *ip_addr = dhcp_ccb->per_lan_user_pool[j]->ip_pool.ip_addr;
            rte_ether_addr_copy(&mac_addr, &dhcp_ccb->per_lan_user_pool[j]->ip_pool.mac_addr);
            return SUCCESS;
        }
    }
    return ERROR;
}

STATUS build_dhcp_offer(dhcp_ccb_per_lan_user_t *per_lan_user, struct rte_ether_addr *lan_mac)
{
    U32 ip_addr;
    dhcp_ccb_t *dhcp_ccb = per_lan_user->dhcp_ccb;
#ifndef UNIT_TEST
    U16 ccb_id = dhcp_ccb->ccb_id;
    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(dhcp_ccb->fastrg_ccb, ccb_id);
    U32 dns_1, dns_2;
    if (dhcp_ccb->dns_state.dns_proxy_enabled) {
        dns_1 = dhcp_ccb->dhcp_server_ip;
        dns_2 = dhcp_ccb->dhcp_server_ip;
    } else if (rte_atomic16_read(&ppp_ccb->dp_start_bool) == 0) {
        dns_1 = rte_cpu_to_be_32(0x08080808);
        dns_2 = rte_cpu_to_be_32(0x01010101);
    } else {
        dns_1 = ppp_ccb->hsi_primary_dns;
        dns_2 = ppp_ccb->hsi_secondary_dns;
    }
#else
    U32 dns_1 = rte_cpu_to_be_32(0x08080808);
    U32 dns_2 = rte_cpu_to_be_32(0x01010101);
#endif

    rte_ether_addr_copy(&dhcp_ccb->eth_hdr->src_addr, &dhcp_ccb->eth_hdr->dst_addr);
    rte_ether_addr_copy(lan_mac, &dhcp_ccb->eth_hdr->src_addr);
    if (pick_ip_from_pool(dhcp_ccb, per_lan_user, &ip_addr, dhcp_ccb->eth_hdr->dst_addr) != SUCCESS)
        return ERROR;
    dhcp_ccb->ip_hdr->packet_id = 0; // dhcp is usually simple and do not fragment, so we can keep it as 0
    dhcp_ccb->ip_hdr->hdr_checksum = 0;
    dhcp_ccb->ip_hdr->src_addr = dhcp_ccb->dhcp_server_ip;
    dhcp_ccb->ip_hdr->dst_addr = ip_addr;
    dhcp_ccb->ip_hdr->total_length = (U16)sizeof(struct rte_ipv4_hdr);

    dhcp_ccb->udp_hdr->src_port = rte_cpu_to_be_16(DHCP_SERVER_PORT);
    dhcp_ccb->udp_hdr->dst_port = rte_cpu_to_be_16(DHCP_CLIENT_PORT);
    dhcp_ccb->udp_hdr->dgram_cksum = 0;
    dhcp_ccb->udp_hdr->dgram_len = sizeof(struct rte_udp_hdr);

    per_lan_user->dhcp_hdr->msg_type = BOOT_REPLY;
    per_lan_user->dhcp_hdr->ur_client_ip = ip_addr;
    per_lan_user->dhcp_hdr->next_server_ip = dhcp_ccb->dhcp_server_ip;
    memset(per_lan_user->dhcp_hdr->server_name, 0, sizeof(per_lan_user->dhcp_hdr->server_name));
    memset(per_lan_user->dhcp_hdr->file_name, 0, sizeof(per_lan_user->dhcp_hdr->file_name));
    per_lan_user->dhcp_hdr->magic_cookie = rte_cpu_to_be_32(DHCP_MAGIC_COOKIE);  // RFC 1497
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_hdr_t);

    unsigned char buf[DHCP_OPTIONS_BUFFER_SIZE] = {0};
    U32 dhcp_opt_len = 0;

    dhcp_opt_t *cur = (dhcp_opt_t *)buf;
    cur->opt_type = DHCP_MSG_TYPE;
    cur->len = sizeof(U8);
    *(cur->val) = DHCP_OFFER;
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_SERVER_ID;
    cur->len = sizeof(dhcp_ccb->dhcp_server_ip);
    rte_memcpy(cur->val, &dhcp_ccb->dhcp_server_ip, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_SUBNET_MASK;
    cur->len = sizeof(U32); // sizeof(255.255.255.0)
    rte_memcpy(cur->val, &dhcp_ccb->subnet_mask, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_LEASE_TIME;
    cur->len = sizeof(U32);
    U32 lease_time = rte_cpu_to_be_32(LEASE_TIMEOUT); //1 hr
    rte_memcpy(cur->val, &lease_time, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_ROUTER;
    cur->len = sizeof(dhcp_ccb->dhcp_server_ip);
    rte_memcpy(cur->val, &dhcp_ccb->dhcp_server_ip, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_DNS;
    cur->len = sizeof(U32) * 2; // 2 DNS servers
    U32 dns[2] = { dns_1, dns_2 };
    rte_memcpy(cur->val, &dns, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    *(U8 *)cur = DHCP_END;
    dhcp_ccb->udp_hdr->dgram_len += sizeof(U8);
    dhcp_opt_len += sizeof(U8);

    rte_memcpy((per_lan_user->dhcp_hdr + 1), buf, dhcp_opt_len);

    dhcp_ccb->ip_hdr->total_length += dhcp_ccb->udp_hdr->dgram_len;

    dhcp_ccb->udp_hdr->dgram_len = rte_cpu_to_be_16(dhcp_ccb->udp_hdr->dgram_len);
    dhcp_ccb->ip_hdr->total_length = rte_cpu_to_be_16(dhcp_ccb->ip_hdr->total_length);
    dhcp_ccb->ip_hdr->hdr_checksum = rte_ipv4_cksum(dhcp_ccb->ip_hdr);

    return SUCCESS;
}

STATUS build_dhcp_ack(dhcp_ccb_per_lan_user_t *per_lan_user, struct rte_ether_addr *lan_mac)
{
    dhcp_ccb_t *dhcp_ccb = per_lan_user->dhcp_ccb;
#ifndef UNIT_TEST
    U16 ccb_id = dhcp_ccb->ccb_id;
    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(dhcp_ccb->fastrg_ccb, ccb_id);
    U32 dns_1, dns_2;
    if (dhcp_ccb->dns_state.dns_proxy_enabled) {
        dns_1 = dhcp_ccb->dhcp_server_ip;
        dns_2 = dhcp_ccb->dhcp_server_ip;
    } else if (rte_atomic16_read(&ppp_ccb->dp_start_bool) == 0) {
        dns_1 = rte_cpu_to_be_32(0x08080808);
        dns_2 = rte_cpu_to_be_32(0x01010101);
    } else {
        dns_1 = ppp_ccb->hsi_primary_dns;
        dns_2 = ppp_ccb->hsi_secondary_dns;
    }
#else
    U32 dns_1 = rte_cpu_to_be_32(0x08080808);
    U32 dns_2 = rte_cpu_to_be_32(0x01010101);
#endif

    rte_ether_addr_copy(&dhcp_ccb->eth_hdr->src_addr, &dhcp_ccb->eth_hdr->dst_addr);
    rte_ether_addr_copy(lan_mac, &dhcp_ccb->eth_hdr->src_addr);

    dhcp_ccb->ip_hdr->packet_id = 0; // dhcp is usually simple and do not fragment, so we can keep it as 0
    dhcp_ccb->ip_hdr->hdr_checksum = 0;
    dhcp_ccb->ip_hdr->src_addr = dhcp_ccb->dhcp_server_ip;
    dhcp_ccb->ip_hdr->dst_addr = per_lan_user->dhcp_hdr->ur_client_ip;
    dhcp_ccb->ip_hdr->total_length = sizeof(struct rte_ipv4_hdr);

    dhcp_ccb->udp_hdr->src_port = rte_cpu_to_be_16(DHCP_SERVER_PORT);
    dhcp_ccb->udp_hdr->dst_port = rte_cpu_to_be_16(DHCP_CLIENT_PORT);
    dhcp_ccb->udp_hdr->dgram_cksum = 0;
    dhcp_ccb->udp_hdr->dgram_len = sizeof(struct rte_udp_hdr);

    per_lan_user->dhcp_hdr->msg_type = BOOT_REPLY;
    per_lan_user->dhcp_hdr->next_server_ip = dhcp_ccb->dhcp_server_ip;
    memset(per_lan_user->dhcp_hdr->server_name, 0, sizeof(per_lan_user->dhcp_hdr->server_name));
    memset(per_lan_user->dhcp_hdr->file_name, 0, sizeof(per_lan_user->dhcp_hdr->file_name));
    per_lan_user->dhcp_hdr->magic_cookie = rte_cpu_to_be_32(DHCP_MAGIC_COOKIE);  // RFC 1497
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_hdr_t);

    unsigned char buf[DHCP_OPTIONS_BUFFER_SIZE] = {0};
    U32 dhcp_opt_len = 0;

    dhcp_opt_t *cur = (dhcp_opt_t *)buf;
    cur->opt_type = DHCP_MSG_TYPE;
    cur->len = sizeof(U8);
    *(cur->val) = DHCP_ACK;
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_SERVER_ID;
    cur->len = sizeof(dhcp_ccb->dhcp_server_ip);
    rte_memcpy(cur->val, &dhcp_ccb->dhcp_server_ip, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_SUBNET_MASK;
    cur->len = sizeof(U32); // sizeof(255.255.255.0)
    rte_memcpy(cur->val, &dhcp_ccb->subnet_mask, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_LEASE_TIME;
    cur->len = sizeof(U32);
    U32 lease_time = rte_cpu_to_be_32(LEASE_TIMEOUT);
    rte_memcpy(cur->val, &lease_time, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_ROUTER;
    cur->len = sizeof(dhcp_ccb->dhcp_server_ip);
    rte_memcpy(cur->val, &dhcp_ccb->dhcp_server_ip, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_DNS;
    cur->len = sizeof(U32) * 2; // 2 DNS servers
    U32 dns[2] = { dns_1, dns_2 };
    rte_memcpy(cur->val, &dns, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    *(U8 *)cur = DHCP_END;
    dhcp_ccb->udp_hdr->dgram_len += sizeof(U8);
    dhcp_opt_len += sizeof(U8);

    rte_memcpy((per_lan_user->dhcp_hdr + 1), buf, dhcp_opt_len);

    dhcp_ccb->ip_hdr->total_length += dhcp_ccb->udp_hdr->dgram_len;

    dhcp_ccb->udp_hdr->dgram_len = rte_cpu_to_be_16(dhcp_ccb->udp_hdr->dgram_len);
    dhcp_ccb->ip_hdr->total_length = rte_cpu_to_be_16(dhcp_ccb->ip_hdr->total_length);
    dhcp_ccb->ip_hdr->hdr_checksum = rte_ipv4_cksum(dhcp_ccb->ip_hdr);

    FastRG_LOG(INFO, dhcp_ccb->log_fp, (U8 *)dhcp_ccb, DHCPLOGMSG, "DHCP ACK built\n");

    return SUCCESS;
}

STATUS build_dhcp_nak(dhcp_ccb_per_lan_user_t *per_lan_user, struct rte_ether_addr *lan_mac)
{
    dhcp_ccb_t *dhcp_ccb = per_lan_user->dhcp_ccb;

    rte_ether_addr_copy(&dhcp_ccb->eth_hdr->src_addr, &dhcp_ccb->eth_hdr->dst_addr);
    rte_ether_addr_copy(lan_mac, &dhcp_ccb->eth_hdr->src_addr);

    dhcp_ccb->ip_hdr->packet_id = 0; // dhcp is usually simple and do not fragment, so we can keep it as 0
    dhcp_ccb->ip_hdr->hdr_checksum = 0;
    dhcp_ccb->ip_hdr->src_addr = dhcp_ccb->dhcp_server_ip;
    dhcp_ccb->ip_hdr->dst_addr = RTE_IPV4_BROADCAST;
    dhcp_ccb->ip_hdr->total_length = sizeof(struct rte_ipv4_hdr);

    dhcp_ccb->udp_hdr->src_port = rte_cpu_to_be_16(DHCP_SERVER_PORT);
    dhcp_ccb->udp_hdr->dst_port = rte_cpu_to_be_16(DHCP_CLIENT_PORT);
    dhcp_ccb->udp_hdr->dgram_cksum = 0;
    dhcp_ccb->udp_hdr->dgram_len = sizeof(struct rte_udp_hdr);

    per_lan_user->dhcp_hdr->client_ip = 0;
    per_lan_user->dhcp_hdr->ur_client_ip = 0;
    per_lan_user->dhcp_hdr->msg_type = BOOT_REPLY;
    per_lan_user->dhcp_hdr->next_server_ip = dhcp_ccb->dhcp_server_ip;
    memset(per_lan_user->dhcp_hdr->server_name, 0, sizeof(per_lan_user->dhcp_hdr->server_name));
    memset(per_lan_user->dhcp_hdr->file_name, 0, sizeof(per_lan_user->dhcp_hdr->file_name));
    per_lan_user->dhcp_hdr->magic_cookie = rte_cpu_to_be_32(DHCP_MAGIC_COOKIE);  // RFC 1497
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_hdr_t);

    unsigned char buf[DHCP_OPTIONS_BUFFER_SIZE] = {0};
    U32 dhcp_opt_len = 0;

    dhcp_opt_t *cur = (dhcp_opt_t *)buf;
    cur->opt_type = DHCP_MSG_TYPE;
    cur->len = sizeof(U8);
    *(cur->val) = DHCP_NAK;
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_SERVER_ID;
    cur->len = sizeof(dhcp_ccb->dhcp_server_ip);
    rte_memcpy(cur->val, &dhcp_ccb->dhcp_server_ip, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_CLIENT_ID;
    cur->len = RTE_ETHER_ADDR_LEN + sizeof(U8);
    cur->val[0] = DHCP_HW_TYPE_ETHERNET; // Ethernet
    rte_ether_addr_copy(&dhcp_ccb->eth_hdr->dst_addr, (struct rte_ether_addr *)(cur->val + 1));
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    *(U8 *)cur = DHCP_END;
    dhcp_ccb->udp_hdr->dgram_len += sizeof(U8);
    dhcp_opt_len += sizeof(U8);

    rte_memcpy((per_lan_user->dhcp_hdr + 1), buf, dhcp_opt_len);

    dhcp_ccb->ip_hdr->total_length += dhcp_ccb->udp_hdr->dgram_len;

    dhcp_ccb->udp_hdr->dgram_len = rte_cpu_to_be_16(dhcp_ccb->udp_hdr->dgram_len);
    dhcp_ccb->ip_hdr->total_length = rte_cpu_to_be_16(dhcp_ccb->ip_hdr->total_length);
    dhcp_ccb->ip_hdr->hdr_checksum = rte_ipv4_cksum(dhcp_ccb->ip_hdr);

    return SUCCESS;
}

STATUS build_dhcp_ack_inform(dhcp_ccb_per_lan_user_t *per_lan_user, 
    struct rte_ether_addr *lan_mac)
{
    dhcp_ccb_t *dhcp_ccb = per_lan_user->dhcp_ccb;
#ifndef UNIT_TEST
    U16 ccb_id = dhcp_ccb->ccb_id;
    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(dhcp_ccb->fastrg_ccb, ccb_id);
    U32 dns_1, dns_2;
    if (dhcp_ccb->dns_state.dns_proxy_enabled) {
        dns_1 = dhcp_ccb->dhcp_server_ip;
        dns_2 = dhcp_ccb->dhcp_server_ip;
    } else {
        dns_1 = ppp_ccb->hsi_primary_dns;
        dns_2 = ppp_ccb->hsi_secondary_dns;
    }
#else
    U32 dns_1 = rte_cpu_to_be_32(0x08080808);
    U32 dns_2 = rte_cpu_to_be_32(0x01010101);
#endif

    rte_ether_addr_copy(&dhcp_ccb->eth_hdr->src_addr, &dhcp_ccb->eth_hdr->dst_addr);
    rte_ether_addr_copy(lan_mac, &dhcp_ccb->eth_hdr->src_addr);

    dhcp_ccb->ip_hdr->packet_id = 0;
    dhcp_ccb->ip_hdr->hdr_checksum = 0;
    dhcp_ccb->ip_hdr->src_addr = dhcp_ccb->dhcp_server_ip;
    dhcp_ccb->ip_hdr->dst_addr = per_lan_user->dhcp_hdr->client_ip;
    dhcp_ccb->ip_hdr->total_length = sizeof(struct rte_ipv4_hdr);

    dhcp_ccb->udp_hdr->src_port = rte_cpu_to_be_16(DHCP_SERVER_PORT);
    dhcp_ccb->udp_hdr->dst_port = rte_cpu_to_be_16(DHCP_CLIENT_PORT);
    dhcp_ccb->udp_hdr->dgram_cksum = 0;
    dhcp_ccb->udp_hdr->dgram_len = sizeof(struct rte_udp_hdr);

    per_lan_user->dhcp_hdr->msg_type = BOOT_REPLY;
    per_lan_user->dhcp_hdr->ur_client_ip = 0;
    per_lan_user->dhcp_hdr->next_server_ip = dhcp_ccb->dhcp_server_ip;
    memset(per_lan_user->dhcp_hdr->server_name, 0, sizeof(per_lan_user->dhcp_hdr->server_name));
    memset(per_lan_user->dhcp_hdr->file_name, 0, sizeof(per_lan_user->dhcp_hdr->file_name));
    per_lan_user->dhcp_hdr->magic_cookie = rte_cpu_to_be_32(DHCP_MAGIC_COOKIE);  // RFC 1497
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_hdr_t);

    unsigned char buf[DHCP_OPTIONS_BUFFER_SIZE] = {0};
    U32 dhcp_opt_len = 0;

    dhcp_opt_t *cur = (dhcp_opt_t *)buf;
    cur->opt_type = DHCP_MSG_TYPE;
    cur->len = sizeof(U8);
    *(cur->val) = DHCP_ACK;
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_SERVER_ID;
    cur->len = sizeof(dhcp_ccb->dhcp_server_ip);
    rte_memcpy(cur->val, &dhcp_ccb->dhcp_server_ip, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_SUBNET_MASK;
    cur->len = sizeof(U32);
    rte_memcpy(cur->val, &dhcp_ccb->subnet_mask, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_ROUTER;
    cur->len = sizeof(dhcp_ccb->dhcp_server_ip);
    rte_memcpy(cur->val, &dhcp_ccb->dhcp_server_ip, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    cur->opt_type = DHCP_DNS;
    cur->len = sizeof(U32) * 2;
    U32 dns[2] = { dns_1, dns_2 };
    rte_memcpy(cur->val, &dns, cur->len);
    dhcp_ccb->udp_hdr->dgram_len += sizeof(dhcp_opt_t) + cur->len;
    dhcp_opt_len += sizeof(dhcp_opt_t) + cur->len;

    cur = (dhcp_opt_t *)(((char *)cur) + sizeof(dhcp_opt_t) + cur->len);
    *(U8 *)cur = DHCP_END;
    dhcp_ccb->udp_hdr->dgram_len += sizeof(U8);
    dhcp_opt_len += sizeof(U8);

    rte_memcpy((per_lan_user->dhcp_hdr + 1), buf, dhcp_opt_len);

    dhcp_ccb->ip_hdr->total_length += dhcp_ccb->udp_hdr->dgram_len;
    dhcp_ccb->udp_hdr->dgram_len = rte_cpu_to_be_16(dhcp_ccb->udp_hdr->dgram_len);
    dhcp_ccb->ip_hdr->total_length = rte_cpu_to_be_16(dhcp_ccb->ip_hdr->total_length);
    dhcp_ccb->ip_hdr->hdr_checksum = rte_ipv4_cksum(dhcp_ccb->ip_hdr);

    FastRG_LOG(INFO, dhcp_ccb->log_fp, (U8 *)dhcp_ccb, DHCPLOGMSG, 
        "DHCP ACK (INFORM) built\n");

    return SUCCESS;
}

BIT16 dhcp_decode(dhcp_ccb_t *dhcp_ccb, 
    dhcp_ccb_per_lan_user_t *per_lan_user, int *cur_tmp_pool_index, 
    struct rte_ether_hdr *eth_hdr, vlan_header_t *vlan_header, 
    struct rte_ipv4_hdr *ip_hdr, struct rte_udp_hdr *udp_hdr)
{
    dhcp_opt_t *cur; 
    BIT16 event = -1;

    dhcp_ccb->eth_hdr = eth_hdr;
    dhcp_ccb->vlan_hdr = vlan_header;
    dhcp_ccb->ip_hdr = ip_hdr;
    dhcp_ccb->udp_hdr = udp_hdr;
    per_lan_user->dhcp_hdr = (dhcp_hdr_t *)(udp_hdr + 1);

    if (rte_be_to_cpu_32(per_lan_user->dhcp_hdr->magic_cookie) != DHCP_MAGIC_COOKIE) {
        FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, 
            (U8 *)dhcp_ccb, DHCPLOGMSG,
            "subscriber %u: Invalid DHCP magic cookie 0x%08x (expected 0x%08x)\n",
            dhcp_ccb->ccb_id, 
            rte_be_to_cpu_32(per_lan_user->dhcp_hdr->magic_cookie),
            DHCP_MAGIC_COOKIE);
        return ERROR;
    }

    cur = (dhcp_opt_t *)(per_lan_user->dhcp_hdr + 1);
    U16 dhcp_field_len = rte_be_to_cpu_16(udp_hdr->dgram_len) - 
        sizeof(struct rte_udp_hdr) - sizeof(dhcp_hdr_t);
    U16 cur_opt_len = 0;

    for(; cur->opt_type!=DHCP_END && cur_opt_len<dhcp_field_len; 
            cur=(dhcp_opt_t *)(((U8 *)(cur+1))+cur->len)) {
        if ((cur_opt_len + cur->len + sizeof(dhcp_opt_t)) > dhcp_field_len) {
            FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, 
                (U8 *)dhcp_ccb, DHCPLOGMSG,
                "subscriber %u: Malformed DHCP options\n", dhcp_ccb->ccb_id);
            return ERROR;
        }

        if (cur->opt_type == DHCP_ISP_ID) {
            return 0;
        } else if (cur->opt_type == DHCP_MSG_TYPE) {
            switch (*(U8 *)(cur+1)) {
            case DHCP_DISCOVER:
                event = E_DISCOVER;
                break;
            case DHCP_REQUEST:
                DHCP_EVENT_TYPE ret = decode_request(per_lan_user, cur_tmp_pool_index);
                event = (ret == ERROR) ? -1 : ret;
                rte_timer_stop(&per_lan_user->lan_user_info.timer);
                break;
            case DHCP_RELEASE:
                if (check_pool(dhcp_ccb, per_lan_user, 
                        cur_tmp_pool_index, eth_hdr->src_addr) == SUCCESS)
                    event = E_RELEASE;
                break;
            case DHCP_DECLINE:
                dhcp_opt_t *decline_cur = (dhcp_opt_t *)(per_lan_user->dhcp_hdr + 1);
                U32 requested_ip = 0;
                BOOL has_server_id = FALSE;
                U16 decline_opt_len = 0;
                
                for(; decline_cur->opt_type != DHCP_END && decline_opt_len < dhcp_field_len;
                    decline_cur = (dhcp_opt_t *)(((U8 *)(decline_cur+1)) + decline_cur->len)) {
                    
                    if (decline_opt_len + sizeof(dhcp_opt_t) + decline_cur->len > dhcp_field_len) {
                        FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, 
                            (U8 *)dhcp_ccb, DHCPLOGMSG,
                            "subscriber %u: Malformed DECLINE options\n", dhcp_ccb->ccb_id);
                        break;
                    }
                    
                    if (decline_cur->opt_type == DHCP_SERVER_ID) {
                        if (decline_cur->len == sizeof(U32)) {
                            U32 server_id;
                            rte_memcpy(&server_id, decline_cur->val, sizeof(U32));
                            if (server_id == dhcp_ccb->dhcp_server_ip)
                                has_server_id = TRUE;
                        }
                    } else if (decline_cur->opt_type == DHCP_REQUEST_IP) {
                        if (decline_cur->len == sizeof(U32))
                            rte_memcpy(&requested_ip, decline_cur->val, sizeof(U32));
                    }
                    
                    decline_opt_len += sizeof(dhcp_opt_t) + decline_cur->len;
                }
                
                if (has_server_id && requested_ip != 0) {
                    if ((requested_ip & dhcp_ccb->subnet_mask) == 
                            (dhcp_ccb->dhcp_server_ip & dhcp_ccb->subnet_mask)) {
                        if (is_client_in_pool(dhcp_ccb, &eth_hdr->src_addr, *cur_tmp_pool_index))
                        event = E_DECLINE;
                    } else {
                        FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, 
                            (U8 *)dhcp_ccb, DHCPLOGMSG,
                            "subscriber %u: DECLINE for IP outside subnet\n", dhcp_ccb->ccb_id);
                    }
                } else {
                    FastRG_LOG(WARN, dhcp_ccb->fastrg_ccb->fp, 
                        (U8 *)dhcp_ccb, DHCPLOGMSG,
                        "subscriber %u: invalid DECLINE (missing server_id=%d or requested_ip=0x%08x)\n",
                        dhcp_ccb->ccb_id, has_server_id, rte_be_to_cpu_32(requested_ip));
                }
                break;
            case DHCP_INFORM:
                event = E_INFORM;
                break;
            default:
                break;
            }
        } else if (cur->opt_type == DHCP_HOSTNAME) {
            // TODO: process hostname if needed
        }
        cur_opt_len += (cur->len + sizeof(dhcp_opt_t));
    }
    return event;
}
