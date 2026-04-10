/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DNS_CACHE.C

  DNS cache implementation for FastRG DNS proxy.
  Hash table with TTL-based expiry.

  Designed by THE on Apr 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#include <string.h>
#include <ctype.h>

#include <rte_memcpy.h>

#include "dns_cache.h"
#include "../utils.h"

/* FNV-1a hash for domain + qtype */
static U32 dns_cache_hash(const char *domain, U16 qtype)
{
    U32 hash = 2166136261u;
    for(const char *p=domain; *p; p++) {
        hash ^= (U32)tolower((unsigned char)*p);
        hash *= 16777619u;
    }
    hash ^= (U32)qtype;
    hash *= 16777619u;
    return hash % DNS_CACHE_BUCKET_COUNT;
}

STATUS dns_cache_init(dns_cache_t *cache)
{
    if (cache == NULL)
        return ERROR;
    memset(cache, 0, sizeof(*cache));
    return SUCCESS;
}

void dns_cache_cleanup(dns_cache_t *cache)
{
    if (cache == NULL)
        return;
    dns_cache_flush(cache);
}

static int domain_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return (*a == *b);
}

dns_cache_entry_t *dns_cache_lookup(dns_cache_t *cache, const char *domain, U16 qtype)
{
    if (cache == NULL || domain == NULL)
        return NULL;

    U32 idx = dns_cache_hash(domain, qtype);
    U64 now = fastrg_get_cur_cycles();
    dns_cache_entry_t *entry = cache->buckets[idx];
    dns_cache_entry_t **prev = &cache->buckets[idx];

    while (entry) {
        if (domain_eq(entry->domain, domain) && entry->qtype == qtype) {
            /* check TTL expiry */
            if (now - entry->insert_time >= (U64)entry->ttl * fastrg_get_cycles_in_sec()) {
                /* expired - remove and return NULL */
                *prev = entry->next;
                fastrg_mfree(entry);
                cache->entry_count--;
                return NULL;
            }
            entry->hit_count++;
            return entry;
        }
        prev = &entry->next;
        entry = entry->next;
    }

    return NULL;
}

STATUS dns_cache_insert(dns_cache_t *cache, const char *domain, U16 qtype,
    const U8 *response, U16 response_len, U32 ttl)
{
    if (cache == NULL || domain == NULL || response == NULL || response_len == 0)
        return ERROR;

    if (response_len > DNS_MAX_PACKET_LEN)
        return ERROR;

    if (ttl == 0)
        ttl = 60;  /* minimum TTL: 60 seconds */

    U32 idx = dns_cache_hash(domain, qtype);
    U64 now = fastrg_get_cur_cycles();

    /* check if entry already exists - update it */
    dns_cache_entry_t *entry = cache->buckets[idx];
    while (entry) {
        if (domain_eq(entry->domain, domain) && entry->qtype == qtype) {
            rte_memcpy(entry->response, response, response_len);
            entry->response_len = response_len;
            entry->ttl = ttl;
            entry->insert_time = now;
            return SUCCESS;
        }
        entry = entry->next;
    }

    /* enforce max entries */
    if (cache->entry_count >= DNS_CACHE_MAX_ENTRIES) {
        /* Scan the bucket: evict all timed-out entries first, then the oldest
         * non-expired one if the bucket is still full after the sweep. */
        dns_cache_entry_t *oldest = NULL;
        dns_cache_entry_t **oldest_prev = NULL;
        dns_cache_entry_t **prev = &cache->buckets[idx];
        dns_cache_entry_t *cur = cache->buckets[idx];
        while (cur) {
            if (now - cur->insert_time >= (U64)cur->ttl * fastrg_get_cycles_in_sec()) {
                /* expired: unlink and free in-place; prev stays, cur advances */
                *prev = cur->next;
                /* if oldest was pointing at this expired entry, invalidate it */
                if (oldest == cur) {
                    oldest = NULL;
                    oldest_prev = NULL;
                }
                fastrg_mfree(cur);
                cache->entry_count--;
                cur = *prev;
                continue;
            }
            /* not expired: track the oldest insert_time seen so far */
            if (oldest == NULL || cur->insert_time < oldest->insert_time) {
                oldest = cur;
                oldest_prev = prev;
            }
            prev = &cur->next;
            cur = cur->next;
        }
        /* if still at capacity after sweeping expired entries, evict oldest */
        if (cache->entry_count >= DNS_CACHE_MAX_ENTRIES && oldest != NULL) {
            *oldest_prev = oldest->next;
            fastrg_mfree(oldest);
            cache->entry_count--;
        }
    }

    /* create new entry */
    entry = fastrg_malloc(dns_cache_entry_t, sizeof(dns_cache_entry_t), 0);
    if (entry == NULL)
        return ERROR;

    memset(entry, 0, sizeof(*entry));
    strncpy(entry->domain, domain, DNS_MAX_DOMAIN_LEN);
    entry->domain[DNS_MAX_DOMAIN_LEN] = '\0';
    dns_domain_to_lower(entry->domain);
    entry->qtype = qtype;
    rte_memcpy(entry->response, response, response_len);
    entry->response_len = response_len;
    entry->ttl = ttl;
    entry->insert_time = now;
    entry->hit_count = 0;

    /* insert at head of bucket chain */
    entry->next = cache->buckets[idx];
    cache->buckets[idx] = entry;
    cache->entry_count++;

    return SUCCESS;
}

STATUS dns_cache_remove(dns_cache_t *cache, const char *domain, U16 qtype)
{
    if (cache == NULL || domain == NULL)
        return ERROR;

    U32 idx = dns_cache_hash(domain, qtype);
    dns_cache_entry_t **prev = &cache->buckets[idx];
    dns_cache_entry_t *entry = cache->buckets[idx];

    while (entry) {
        if (domain_eq(entry->domain, domain) && entry->qtype == qtype) {
            *prev = entry->next;
            fastrg_mfree(entry);
            cache->entry_count--;
            return SUCCESS;
        }
        prev = &entry->next;
        entry = entry->next;
    }

    return ERROR;
}

U32 dns_cache_flush(dns_cache_t *cache)
{
    if (cache == NULL)
        return 0;

    U32 flushed = 0;
    for(U32 i=0; i<DNS_CACHE_BUCKET_COUNT; i++) {
        dns_cache_entry_t *entry = cache->buckets[i];
        while (entry) {
            dns_cache_entry_t *next = entry->next;
            fastrg_mfree(entry);
            entry = next;
            flushed++;
        }
        cache->buckets[i] = NULL;
    }
    cache->entry_count = 0;
    return flushed;
}
