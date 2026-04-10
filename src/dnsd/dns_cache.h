/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DNS_CACHE.H

  DNS cache for FastRG DNS proxy.
  Per-subscriber hash table with TTL-based expiry.

  Designed by THE on Apr 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _DNS_CACHE_H_
#define _DNS_CACHE_H_

#include <common.h>
#include <rte_cycles.h>
#include "dns_codec.h"

#define DNS_CACHE_BUCKET_COUNT  256
#define DNS_CACHE_MAX_ENTRIES   512

/* single cache entry */
typedef struct dns_cache_entry {
    char                    domain[DNS_MAX_DOMAIN_LEN + 1];
    U16                     qtype;          /* DNS_TYPE_A, DNS_TYPE_AAAA, etc. */
    U8                      response[DNS_MAX_PACKET_LEN]; /* raw DNS response */
    U16                     response_len;
    U32                     ttl;            /* original TTL from upstream (seconds) */
    U64                     insert_time;    /* TSC at insert time (fastrg_get_cur_cycles) */
    U32                     hit_count;
    struct dns_cache_entry  *next;          /* hash chain */
} dns_cache_entry_t;

/* per-subscriber cache */
typedef struct dns_cache {
    dns_cache_entry_t   *buckets[DNS_CACHE_BUCKET_COUNT];
    U32                 entry_count;
} dns_cache_t;

/**
 * @fn dns_cache_init
 *
 * @brief Initialize a DNS cache
 * @param cache
 *      Cache to initialize
 * @return SUCCESS on success, ERROR on failure
 */
STATUS dns_cache_init(dns_cache_t *cache);

/**
 * @fn dns_cache_cleanup
 *
 * @brief Free all entries in a DNS cache
 * @param cache
 *      Cache to cleanup
 */
void dns_cache_cleanup(dns_cache_t *cache);

/**
 * @fn dns_cache_lookup
 *
 * @brief Look up a domain in the cache
 * @param cache
 *      Cache to search
 * @param domain
 *      Domain name (case-insensitive)
 * @param qtype
 *      Query type (DNS_TYPE_A, etc.)
 * @return Pointer to cache entry if found and not expired, NULL otherwise
 */
dns_cache_entry_t *dns_cache_lookup(dns_cache_t *cache, const char *domain, U16 qtype);

/**
 * @fn dns_cache_insert
 *
 * @brief Insert or update a DNS response in the cache
 * @param cache
 *      Cache to insert into
 * @param domain
 *      Domain name
 * @param qtype
 *      Query type
 * @param response
 *      Raw DNS response data
 * @param response_len
 *      Length of response data
 * @param ttl
 *      TTL from upstream DNS response
 * @return SUCCESS on success, ERROR on failure
 */
STATUS dns_cache_insert(dns_cache_t *cache, const char *domain, U16 qtype,
    const U8 *response, U16 response_len, U32 ttl);

/**
 * @fn dns_cache_remove
 *
 * @brief Remove a specific domain entry from the cache
 * @param cache
 *      Cache to remove from
 * @param domain
 *      Domain name
 * @param qtype
 *      Query type
 * @return SUCCESS if found and removed, ERROR if not found
 */
STATUS dns_cache_remove(dns_cache_t *cache, const char *domain, U16 qtype);

/**
 * @fn dns_cache_flush
 *
 * @brief Remove all entries from the cache
 * @param cache
 *      Cache to flush
 */
U32 dns_cache_flush(dns_cache_t *cache);

#endif
