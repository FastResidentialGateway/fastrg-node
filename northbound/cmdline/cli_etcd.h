#ifndef _CLI_ETCD_H_
#define _CLI_ETCD_H_

/*
 * CLI -> etcd direct writer (slice 12, tier 2).
 *
 * Fallback used when the controller is unreachable: the CLI writes config
 * straight to etcd with a ModRevision CAS, mirroring the shared schema
 * (docs/contracts/etcd-schema.md). Best-effort validation only (the controller
 * config-watch + CAS remain the backstop).
 */

#include <common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLI_ETCD_OK = 0,
    CLI_ETCD_UNAVAIL = 1,   /* etcd unreachable -> caller falls back to node */
    CLI_ETCD_ERR = 2        /* other error (e.g. CAS exhausted, bad config)  */
} cli_etcd_status_t;

/* endpoints = comma-separated host:port; node_uuid identifies the managed node.
 * Either NULL/empty disables this tier. */
void cli_etcd_configure(const char *endpoints, const char *node_uuid);
int  cli_etcd_configured(void);

cli_etcd_status_t cli_etcd_apply_hsi(U16 user_id, U16 vlan_id, const char *account,
    const char *password, const char *pool_start, const char *pool_end,
    const char *subnet, const char *gateway);
cli_etcd_status_t cli_etcd_remove_hsi(U16 user_id);
cli_etcd_status_t cli_etcd_set_subscriber_count(int count);
cli_etcd_status_t cli_etcd_set_desire(U16 user_id, const char *desire_status);
cli_etcd_status_t cli_etcd_add_dns(U16 user_id, const char *domain, const char *ip, U32 ttl);
cli_etcd_status_t cli_etcd_del_dns(U16 user_id, const char *domain);
cli_etcd_status_t cli_etcd_set_dns_proxy(U16 user_id, int enable);
cli_etcd_status_t cli_etcd_set_tcp_conntrack(U16 user_id, int enable);
cli_etcd_status_t cli_etcd_snat_set(U16 user_id, U16 eport, const char *dip, U16 iport);
cli_etcd_status_t cli_etcd_snat_unset(U16 user_id, U16 eport);

#ifdef __cplusplus
}
#endif

#endif /* _CLI_ETCD_H_ */
