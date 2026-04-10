/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DNSD.H

  DNS proxy/relay daemon for FastRG.
  Per-subscriber DNS proxy with cache, static records, and upstream relay.

  Designed by THE on Apr 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DNSD_H_
#define _DNSD_H_

#include <common.h>

#include "../protocol.h"
#include "../fastrg.h"
#include "dns_codec.h"
#include "dns_cache.h"
#include "dns_static.h"

#define DNS_MAX_PENDING_QUERIES     32
#define DNS_PENDING_TIMEOUT_SECS    5
#define DNS_FAILOVER_TIMEOUT_SECS   60
#define DNS_CACHE_EXPIRE_INTERVAL   30  /* seconds between cache expiry sweeps */

/* pending upstream DNS query */
typedef struct dns_pending_query {
    BOOL    active;
    U16     original_id;        /* DNS ID from LAN client */
    U16     upstream_id;        /* DNS ID used in forwarded query */
    U32     client_ip;          /* LAN client IP (network order) */
    U16     client_port;        /* LAN client src port (network order) */
    U8      client_mac[6];      /* LAN client MAC address */
    U16     vlan_tci;           /* VLAN TCI value for response */
    U8      is_tcp;             /* 1 if original query was TCP */
    U16     qtype;              /* query type for cache insertion */
    char    domain[DNS_MAX_DOMAIN_LEN + 1];
    U64     start_tsc;          /* TSC at query forward time, for lazy timeout */
} dns_pending_query_t;

/* per-subscriber DNS proxy state (embedded in dhcp_ccb_t) */
typedef struct dns_proxy_state {
    dns_cache_t         cache;
    dns_static_table_t  static_table;
    dns_pending_query_t pending[DNS_MAX_PENDING_QUERIES];
    U32                 primary_dns;        /* primary upstream DNS (network order) */
    U32                 secondary_dns;      /* secondary upstream DNS (network order) */
    U32                 active_dns;         /* currently active upstream DNS (network order) */
    U64                 last_response_time; /* last successful response from active DNS */
    U16                 next_upstream_id;   /* rolling query ID for upstream queries */
    BOOL                dns_proxy_enabled;  /* per-subscriber DNS proxy enable */
} dns_proxy_state_t;

/**
 * @fn dns_proxy_init
 *
 * @brief Initialize DNS proxy state for a subscriber
 * @param state
 *      DNS proxy state to initialize
 * @param primary_dns
 *      Primary upstream DNS server (network byte order)
 * @param secondary_dns
 *      Secondary upstream DNS server (network byte order)
 * @return SUCCESS on success, ERROR on failure
 */
STATUS dns_proxy_init(dns_proxy_state_t *state, U32 primary_dns, U32 secondary_dns);

/**
 * @fn dns_proxy_cleanup
 *
 * @brief Cleanup DNS proxy state
 * @param state
 *      DNS proxy state to cleanup
 */
void dns_proxy_cleanup(dns_proxy_state_t *state);

/**
 * @fn dnsd_check_failover
 *
 * @brief Check DNS failover condition and switch servers if needed.
 *        Called periodically from timer thread.
 * @param state
 *      DNS proxy state
 */
void dnsd_check_failover(dns_proxy_state_t *state);

dns_pending_query_t *find_free_pending(dns_proxy_state_t *state);
dns_pending_query_t *find_pending_by_upstream_id(dns_proxy_state_t *state, U16 upstream_id);

/**
 * @fn dnsd_cp_process_lan_udp_query
 *
 * @brief Process a DNS query received from LAN via UDP
 *
 * Resolution priority: static record > upstream DNS > cache (fallback)
 *
 * @param fastrg_ccb
 *      FastRG control block
 * @param pkt_data
 *      Packet buf
 * @param pkt_len
 *      Packet length
 * @param ccb_id
 *      Subscriber CCB ID
 * @param tx_queue
 *      TX queue for sending responses/forwarded queries
 * @return 1 if response sent to LAN, 0 if forwarded to WAN, -1 on error
 */
int dnsd_cp_process_lan_udp_query(FastRG_t *fastrg_ccb, U8 *pkt_data, U16 pkt_len, U16 ccb_id);
/**
 * @fn dnsd_cp_process_lan_tcp_query
 *
 * @brief Process a DNS query received from LAN via TCP
 * @param fastrg_ccb
 *      FastRG control block
 * @param pkt_data
 *      Packet buf
 * @param pkt_len
 *      Packet length
 * @param ccb_id
 *      Subscriber CCB ID
 * @return 1 if response sent to LAN, 0 if forwarded to WAN, -1 on error
 */
int dnsd_cp_process_lan_tcp_query(FastRG_t *fastrg_ccb, U8 *pkt_data, U16 pkt_len, U16 ccb_id);
/**
 * @fn dnsd_cp_process_wan_udp_response
 *
 * @brief Process a DNS response received from WAN (upstream DNS server)
 * @param fastrg_ccb
 *      FastRG control block
 * @param pkt_data
 *      Packet buf
 * @param pkt_len
 *      Packet length
 * @param ccb_id
 *      Subscriber CCB ID
 * @return 1 if response sent to LAN client, 0 if no matching pending query, -1 on error
 */
int dnsd_cp_process_wan_udp_response(FastRG_t *fastrg_ccb, U8 *pkt_data, U16 pkt_len, U16 ccb_id);

#endif
