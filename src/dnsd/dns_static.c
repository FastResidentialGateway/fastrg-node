/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  DNS_STATIC.C

  Static DNS record management implementation.

  Designed by THE on Apr 2026
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#include <string.h>
#include <ctype.h>

#include "dns_static.h"

#define DNS_STATIC_DEFAULT_TTL  3600

static int static_domain_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return (*a == *b);
}

STATUS dns_static_init(dns_static_table_t *table)
{
    if (table == NULL)
        return ERROR;
    memset(table, 0, sizeof(*table));
    return SUCCESS;
}

void dns_static_cleanup(dns_static_table_t *table)
{
    if (table == NULL)
        return;
    memset(table, 0, sizeof(*table));
}

STATUS dns_static_add(dns_static_table_t *table, const char *domain,
    U32 ip_addr, U32 ttl)
{
    if (table == NULL || domain == NULL)
        return ERROR;

    if (ttl == 0)
        ttl = DNS_STATIC_DEFAULT_TTL;

    /* check for existing entry — update if found */
    for(U32 i=0; i<DNS_STATIC_MAX_RECORDS; i++) {
        if (table->records[i].active &&
            static_domain_eq(table->records[i].domain, domain)) {
            table->records[i].ip_addr = ip_addr;
            table->records[i].ttl = ttl;
            return SUCCESS;
        }
    }

    /* find empty slot */
    for(U32 i=0; i<DNS_STATIC_MAX_RECORDS; i++) {
        if (!table->records[i].active) {
            strncpy(table->records[i].domain, domain, DNS_MAX_DOMAIN_LEN);
            table->records[i].domain[DNS_MAX_DOMAIN_LEN] = '\0';
            dns_domain_to_lower(table->records[i].domain);
            table->records[i].ip_addr = ip_addr;
            table->records[i].ttl = ttl;
            table->records[i].active = TRUE;
            table->count++;
            return SUCCESS;
        }
    }

    return ERROR;  /* table full */
}

STATUS dns_static_remove(dns_static_table_t *table, const char *domain)
{
    if (table == NULL || domain == NULL)
        return ERROR;

    for(U32 i=0; i<DNS_STATIC_MAX_RECORDS; i++) {
        if (table->records[i].active &&
                static_domain_eq(table->records[i].domain, domain)) {
            memset(&table->records[i], 0, sizeof(dns_static_record_t));
            table->count--;
            return SUCCESS;
        }
    }

    return ERROR;
}

dns_static_record_t *dns_static_lookup(dns_static_table_t *table, const char *domain)
{
    if (table == NULL || domain == NULL)
        return NULL;

    for(U32 i=0; i<DNS_STATIC_MAX_RECORDS; i++) {
        if (table->records[i].active &&
            static_domain_eq(table->records[i].domain, domain)) {
            return &table->records[i];
        }
    }

    return NULL;
}

U32 dns_static_get_count(const dns_static_table_t *table)
{
    if (table == NULL)
        return 0;
    return table->count;
}
