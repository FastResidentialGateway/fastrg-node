#ifndef _CLI_REQUEST_H_
#define _CLI_REQUEST_H_

#include <netinet/in.h>

#include <common.h>

#include "fastrg.h"
#include "dnsd/dns_cache.h"
#include "dnsd/dns_static.h"

/*
 * Standalone CLI requests: the gRPC handlers hand work to the control thread
 * over cp_q and wait for its verdict, because the control thread is the only
 * writer of subscriber CCB state. Everything here belongs to that one path.
 */

/* Verdicts carried by FastRG_t.cli_request_result in standalone mode */
#define CLI_REQUEST_NONE    0
#define CLI_REQUEST_OK      1
#define CLI_REQUEST_FAILED  2

/* cli_request_result layout: sequence number above the two verdict bits. */
#define CLI_REQUEST_VERDICT_BITS 2
#define CLI_REQUEST_VERDICT_MASK 0x3u
/* Largest sequence that still fits above the verdict bits; 0 means no waiter. */
#define CLI_REQUEST_SEQ_MAX      (UINT32_MAX >> CLI_REQUEST_VERDICT_BITS)

/* Commands carried by EV_NORTHBOUND_NODE (node-wide, not per subscriber) */
#define NODE_CMD_SET_USER_COUNT 0

/* CLI-only PPPoE commands. Values continue pppd.h's PPPoE_CMD_* numbering,
 * which stops at PPPoE_CMD_IPV6_CHANGED (3): keep both lists in step. */
#define PPPoE_CMD_APPLY_CONFIG  4
#define PPPoE_CMD_REMOVE_CONFIG 5
#define PPPoE_CMD_SNAT_SET      6
#define PPPoE_CMD_TCP_CONNTRACK_ENABLE  7
#define PPPoE_CMD_TCP_CONNTRACK_DISABLE 8
#define PPPoE_CMD_SNAT_REMOVE   9
#define PPPoE_CMD_IPV6_ENABLE   10
#define PPPoE_CMD_IPV6_DISABLE  11

/* Commands carried by EV_NORTHBOUND_DNS */
#define DNS_CMD_PROXY_ENABLE   0
#define DNS_CMD_PROXY_DISABLE  1
#define DNS_CMD_RECORD_ADD     2
#define DNS_CMD_RECORD_REMOVE  3
#define DNS_CMD_CACHE_FLUSH    4
#define DNS_CMD_CACHE_DUMP     5
#define DNS_CMD_STATIC_DUMP    6

/* Payload of PPPoE_CMD_SNAT_SET and PPPoE_CMD_SNAT_REMOVE.
 * SNAT_REMOVE only fills in eport. */
typedef struct {
    U16  eport;
    U16  iport;
    char dip[INET_ADDRSTRLEN];
} snat_fwd_req_t;

/* Payload of DNS_CMD_RECORD_ADD and DNS_CMD_RECORD_REMOVE.
 * REMOVE only fills in domain. */
typedef struct {
    char domain[DNS_MAX_DOMAIN_LEN + 1];
    U32  ip_addr;   /* IPv4 address in network byte order */
    U32  ttl;
} dns_record_req_t;

/* Payload of DNS_CMD_CACHE_DUMP. */
typedef struct {
    U32 count;                  /* entries actually written */
    dns_cache_entry_t *entries; /* malloc'd by the control thread, freed by the caller */
} dns_cache_dump_t;

/* Payload of DNS_CMD_CACHE_FLUSH. */
typedef struct {
    U32 flushed;    /* entries the flush removed */
} dns_cache_flush_t;

/* Payload of DNS_CMD_STATIC_DUMP. */
typedef struct {
    U32 count;
    dns_static_record_t *records; /* malloc'd by the control thread, freed by the caller */
} dns_static_dump_t;

/**
 * @fn cli_request_result_pack
 *
 * @brief Pack a CLI request sequence and its verdict into one slot value.
 *        Lower 2 bits for verdict(CLI_REQUEST_NONE=0 / OK=1 / FAILED=2), higher 30 bits for seq
 *
 * @param seq
 *      Request sequence (0 = no waiter)
 * @param verdict
 *      CLI_REQUEST_NONE / CLI_REQUEST_OK / CLI_REQUEST_FAILED
 *
 * @return
 *      The packed value to store in FastRG_t.cli_request_result
 */
U32 cli_request_result_pack(U32 seq, U8 verdict);

/**
 * @fn cli_request_publish
 *
 * @brief Publish the control thread's verdict for a CLI request. Releases the
 *        waiting gRPC thread, so the caller must be done with the request.
 *        Anything written before this call — result buffers, CCB fields — is
 *        visible to the waiter, so commands need no barrier of their own.
 *        Does nothing for seq 0, which marks an event nobody waits on.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param seq
 *      Sequence the request was posted with
 * @param ret
 *      Result of handling the request
 *
 * @return
 *      void
 */
void cli_request_publish(FastRG_t *fastrg_ccb, U32 seq, STATUS ret);

/**
 * @fn cli_request_abandon
 *
 * @brief Mark a CLI request as given up on, so the control thread skips it
 *        instead of applying a change nobody is waiting for.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param seq
 *      Sequence of the request that timed out
 *
 * @return
 *      void
 */
void cli_request_abandon(FastRG_t *fastrg_ccb, U32 seq);

/**
 * @fn cli_request_is_abandoned
 *
 * @brief Test whether this request was given up on, consuming the mark when it
 *        matches. Always FALSE for seq 0, which nobody waits on.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param seq
 *      Sequence the request was posted with
 *
 * @return
 *      TRUE when the caller gave up on this request, FALSE otherwise
 */
BOOL cli_request_is_abandoned(FastRG_t *fastrg_ccb, U32 seq);

/**
 * @fn cli_request_dropped
 *
 * @brief Test whether the control thread should skip this request because its
 *        caller stopped waiting, freeing heap_payload when it should.
 *        A caller-owned dump buffer must not be passed in, because the caller
 *        may still reclaim it. The mark is only visible before the command
 *        runs.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param msg
 *      The northbound mail being handled
 * @param heap_payload
 *      Payload to free when the request is skipped, NULL when there is none
 *
 * @return
 *      TRUE when the request must be skipped, FALSE when it must be handled
 */
BOOL cli_request_dropped(FastRG_t *fastrg_ccb,
    const fastrg_event_northbound_msg_t *msg, void *heap_payload);

/**
 * @fn set_dns_proxy_enable
 *
 * @brief Turn the DNS proxy on or off for a subscriber.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based)
 * @param enable
 *      TRUE to enable the DNS proxy, FALSE to disable it
 *
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS set_dns_proxy_enable(FastRG_t *fastrg_ccb, U16 ccb_id, BOOL enable);

/**
 * @fn set_tcp_conntrack_enable
 *
 * @brief Turn TCP connection tracking on or off for a subscriber.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based)
 * @param enable
 *      TRUE to enable TCP connection tracking, FALSE to disable it
 *
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS set_tcp_conntrack_enable(FastRG_t *fastrg_ccb, U16 ccb_id, BOOL enable);

/**
 * @fn set_ipv6_enable
 *
 * @brief Turn IPv6 on or off for a subscriber, refresh the data-plane IPv6
 *        gate, and queue a redial when the value actually changed.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based)
 * @param enable
 *      TRUE to enable IPv6, FALSE to disable it
 *
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS set_ipv6_enable(FastRG_t *fastrg_ccb, U16 ccb_id, BOOL enable);

/**
 * @fn set_snat_port_fwd
 *
 * @brief Add a static SNAT port forwarding rule for a user.
 *        Maps eport on the WAN PPPoE IP to dip:iport on the LAN.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based)
 * @param eport
 *      External port (host byte order)
 * @param dip
 *      Destination LAN IP string (e.g. "192.168.1.100")
 * @param iport
 *      Internal port (host byte order)
 *
 * @return SUCCESS on success, ERROR on failure
 */
STATUS set_snat_port_fwd(FastRG_t *fastrg_ccb, U16 ccb_id, U16 eport,
    const char *dip, U16 iport);

/**
 * @fn remove_snat_port_fwd
 *
 * @brief Remove a static SNAT port forwarding rule for a user.
 *
 * @param fastrg_ccb
 *      Pointer to FastRG control block
 * @param ccb_id
 *      User ID (0-based)
 * @param eport
 *      External port (host byte order)
 *
 * @return SUCCESS on success, ERROR on failure
 */
STATUS remove_snat_port_fwd(FastRG_t *fastrg_ccb, U16 ccb_id, U16 eport);

/**
 * @fn dns_cache_dump
 *
 * @brief Copy up to max cache entries into out. Runs on the thread that owns
 *        the cache, so the caller gets a snapshot instead of live pointers.
 *        Each copy has next cleared: it is standalone, not part of a chain.
 * @param cache
 *      Cache to walk
 * @param out
 *      Destination array, at least max entries long
 * @param max
 *      Entries out can hold
 * @return Number of entries written
 */
U32 dns_cache_dump(const dns_cache_t *cache, dns_cache_entry_t *out, U32 max);

/**
 * @fn dns_static_dump
 *
 * @brief Copy up to max active records into out.
 * @param table
 *      Table to walk
 * @param out
 *      Destination array, at least max records long
 * @param max_outs
 *      Records out can hold
 * @return Number of records written
 */
U32 dns_static_dump(const dns_static_table_t *table, dns_static_record_t *out, U32 max_outs);

#endif /* _CLI_REQUEST_H_ */
