#ifndef _CLI_DISPATCH_H_
#define _CLI_DISPATCH_H_

/*
 * CLI config-write fallback dispatcher (slice 12).
 *
 * Each write tries, in order:
 *   tier 1  controller ConfigService gRPC   (if -c/-n set and logged in)
 *   tier 2  direct etcd CAS write            (if -e/-n set)   [slice 12b-3]
 *   tier 3  node gRPC                        (node applies, or queues if offline)
 * A tier is skipped when not configured; the chain stops on success or a
 * definitive error (validation / auth), and only falls through when the current
 * tier is unreachable.
 */

#include <common.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void cli_dispatch_apply_hsi(U16 user_id, U16 vlan_id, char *account, char *password,
    char *pool_start, char *pool_end, char *subnet, char *gateway);
void cli_dispatch_remove_hsi(U16 user_id);
void cli_dispatch_set_subscriber_count(U16 count);
void cli_dispatch_connect(U16 user_id);
void cli_dispatch_disconnect(U16 user_id, bool force);
void cli_dispatch_add_dns(U16 user_id, char *domain, char *ip, U32 ttl);
void cli_dispatch_del_dns(U16 user_id, char *domain);
void cli_dispatch_snat_set(U16 user_id, U16 eport, char *dip, U16 iport);
void cli_dispatch_snat_unset(U16 user_id, U16 eport);
void cli_dispatch_set_dns_proxy(U16 user_id, bool enable);
void cli_dispatch_set_tcp_conntrack(U16 user_id, bool enable);

/* Fetch the DESIRED config for a user (controller -> etcd). Writes a
 * human-readable summary to buf and returns the source ("controller"/"etcd"/"none"). */
const char *cli_dispatch_get_desire(U16 user_id, char *buf, U32 len);

#ifdef __cplusplus
}
#endif

#endif /* _CLI_DISPATCH_H_ */
