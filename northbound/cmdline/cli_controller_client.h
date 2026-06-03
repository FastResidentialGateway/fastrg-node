#ifndef _CLI_CONTROLLER_CLIENT_H_
#define _CLI_CONTROLLER_CLIENT_H_

/*
 * CLI -> controller client (slice 12, tier 1).
 *
 * Talks to the controller's ConfigService gRPC (controller.proto) for config
 * writes, authenticated with a JWT obtained from the controller's REST
 * /api/login. Config keys are addressed by node_uuid (the managed node).
 */

#include <common.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result of a controller interaction, used to drive the CLI write fallback. */
typedef enum {
    CLI_CTRL_OK = 0,        /* success                                              */
    CLI_CTRL_AUTH = 1,      /* unauthenticated / token expired -> caller re-logins  */
    CLI_CTRL_UNAVAIL = 2,   /* controller unreachable -> caller may fall back        */
    CLI_CTRL_INVALID = 3,   /* validation/precondition error -> show, do NOT fallback */
    CLI_CTRL_ERR = 4        /* other error                                          */
} cli_ctrl_status_t;

/* Configure controller endpoints. grpc_addr = ConfigService (host:port);
 * rest_base = REST base URL for login (e.g. "http://host:8080"); node_uuid =
 * managed node. Any may be NULL/empty to disable that capability. */
void cli_controller_configure(const char *grpc_addr, const char *rest_base, const char *node_uuid);

/* 1 if a controller gRPC address is configured. */
int  cli_controller_configured(void);

/* The configured node UUID (empty string if unset). */
const char *cli_controller_node_uuid(void);

/* Authenticate via REST /api/login and store the JWT for subsequent gRPC calls.
 * CLI_CTRL_OK on success, CLI_CTRL_AUTH on bad credentials (401),
 * CLI_CTRL_UNAVAIL if the REST endpoint is unreachable, else CLI_CTRL_ERR. */
cli_ctrl_status_t cli_controller_login(const char *username, const char *password);

/* 1 if a JWT token is currently held. */
int  cli_controller_has_token(void);

/* ── ConfigService operations (use the stored token) ── */

/* Create or update an HSI config (tries Create, falls back to Update on
 * ALREADY_EXISTS). desire_status is managed by dial/hangup and left untouched. */
cli_ctrl_status_t cli_controller_apply_hsi(unsigned int user_id, unsigned int vlan_id,
    const char *account, const char *password, const char *pool_start, const char *pool_end,
    const char *subnet, const char *gateway);

cli_ctrl_status_t cli_controller_remove_hsi(unsigned int user_id);
cli_ctrl_status_t cli_controller_set_subscriber_count(int count);
cli_ctrl_status_t cli_controller_dial(unsigned int user_id);
cli_ctrl_status_t cli_controller_hangup(unsigned int user_id);
cli_ctrl_status_t cli_controller_add_dns(unsigned int user_id, const char *domain,
    const char *ip, unsigned int ttl);
cli_ctrl_status_t cli_controller_del_dns(unsigned int user_id, const char *domain);

/* Fetch the desired HSI config for a user as a human-readable JSON string into
 * out_buf (for the desire query / diff). */
cli_ctrl_status_t cli_controller_get_hsi(unsigned int user_id, char *out_buf, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* _CLI_CONTROLLER_CLIENT_H_ */
