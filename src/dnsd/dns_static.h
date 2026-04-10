/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DNS_STATIC.H

  Static DNS record management for FastRG DNS proxy.
  Administrator-defined domain→IP mappings with highest resolution priority.

  Designed by THE on Apr 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DNS_STATIC_H_
#define _DNS_STATIC_H_

#include <common.h>
#include "dns_codec.h"

#define DNS_STATIC_MAX_RECORDS  64

/* single static DNS record */
typedef struct dns_static_record {
    char    domain[DNS_MAX_DOMAIN_LEN + 1];
    U32     ip_addr;    /* IPv4 address in network byte order */
    U32     ttl;        /* TTL to return in DNS response */
    BOOL    active;
} dns_static_record_t;

/* per-subscriber static record table */
typedef struct dns_static_table {
    dns_static_record_t records[DNS_STATIC_MAX_RECORDS];
    U32                 count;
} dns_static_table_t;

/**
 * @fn dns_static_init
 *
 * @brief Initialize a static DNS record table
 * @param table
 *      Table to initialize
 * @return SUCCESS on success, ERROR on failure
 */
STATUS dns_static_init(dns_static_table_t *table);

/**
 * @fn dns_static_cleanup
 *
 * @brief Cleanup a static DNS record table
 * @param table
 *      Table to cleanup
 */
void dns_static_cleanup(dns_static_table_t *table);

/**
 * @fn dns_static_add
 *
 * @brief Add a static DNS record
 * @param table
 *      Table to add to
 * @param domain
 *      Domain name
 * @param ip_addr
 *      IPv4 address (network byte order)
 * @param ttl
 *      TTL (0 means use default 3600)
 * @return SUCCESS on success, ERROR on failure (table full or duplicate)
 */
STATUS dns_static_add(dns_static_table_t *table, const char *domain,
    U32 ip_addr, U32 ttl);

/**
 * @fn dns_static_remove
 *
 * @brief Remove a static DNS record by domain name
 * @param table
 *      Table to remove from
 * @param domain
 *      Domain name to remove
 * @return SUCCESS if found and removed, ERROR if not found
 */
STATUS dns_static_remove(dns_static_table_t *table, const char *domain);

/**
 * @fn dns_static_lookup
 *
 * @brief Look up a domain in the static record table
 * @param table
 *      Table to search
 * @param domain
 *      Domain name (case-insensitive)
 * @return Pointer to static record if found, NULL otherwise
 */
dns_static_record_t *dns_static_lookup(dns_static_table_t *table, const char *domain);

/**
 * @fn dns_static_get_count
 *
 * @brief Get number of active static records
 * @param table
 *      Table to query
 * @return Number of active records
 */
U32 dns_static_get_count(const dns_static_table_t *table);

#endif
