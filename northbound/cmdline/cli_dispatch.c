#include <stdio.h>

#include "cli_dispatch.h"
#include "cli_controller_client.h"
#include "cli_etcd.h"
#include "../grpc/fastrg_grpc_client.h"

/*
 * Decide what to do after a controller (tier 1) attempt.
 * Returns 1 = stop (handled), 0 = fall through to the next tier.
 */
static int after_controller(cli_ctrl_status_t st, const char *op)
{
    switch (st) {
    case CLI_CTRL_OK:
        printf("[controller] %s: OK\n", op);
        return 1;
    case CLI_CTRL_INVALID:
        /* controller already printed the rejection reason */
        return 1;
    case CLI_CTRL_AUTH:
        printf("[controller] not authenticated / token expired — run 'controller login' again\n");
        return 1;
    case CLI_CTRL_UNAVAIL:
        printf("[controller] unreachable, falling back...\n");
        return 0;
    default:
        printf("[controller] %s failed\n", op);
        return 1;
    }
}

/* Returns 1 if tier 1 should be attempted (controller configured + logged in).
 * If configured but not logged in, prints a hint and returns 0-stop via *stop. */
static int controller_ready(int *stop)
{
    *stop = 0;
    if (!cli_controller_configured())
        return 0;                       /* node-direct mode: skip to node tier */
    if (!cli_controller_has_token()) {
        printf("Not logged in to controller — run 'controller login' first.\n");
        *stop = 1;                      /* don't silently fall back when SDN-managed */
        return 0;
    }
    return 1;
}

/* Decide what to do after a tier-2 (etcd) attempt: 1 = stop, 0 = fall through. */
static int after_etcd(cli_etcd_status_t st, const char *op)
{
    switch (st) {
    case CLI_ETCD_OK:
        printf("[etcd] %s: OK\n", op);
        return 1;
    case CLI_ETCD_UNAVAIL:
        printf("[etcd] unreachable, falling back to node...\n");
        return 0;
    default:
        printf("[etcd] %s failed\n", op);
        return 1;
    }
}

void cli_dispatch_apply_hsi(U16 user_id, U16 vlan_id, char *account, char *password,
    char *pool_start, char *pool_end, char *subnet, char *gateway)
{
    int stop = 0;
    if (controller_ready(&stop)) {
        if (after_controller(cli_controller_apply_hsi(user_id, vlan_id, account, password,
                pool_start, pool_end, subnet, gateway), "apply config"))
            return;
    } else if (stop) {
        return;
    }
    if (cli_etcd_configured() &&
            after_etcd(cli_etcd_apply_hsi(user_id, vlan_id, account, password,
                pool_start, pool_end, subnet, gateway), "apply config"))
        return;
    fastrg_grpc_apply_config(user_id, vlan_id, account, password,
        pool_start, pool_end, subnet, gateway);
}

void cli_dispatch_remove_hsi(U16 user_id)
{
    int stop = 0;
    if (controller_ready(&stop)) {
        if (after_controller(cli_controller_remove_hsi(user_id), "remove config"))
            return;
    } else if (stop) {
        return;
    }
    if (cli_etcd_configured() && after_etcd(cli_etcd_remove_hsi(user_id), "remove config"))
        return;
    fastrg_grpc_remove_config(user_id);
}

void cli_dispatch_set_subscriber_count(U16 count)
{
    int stop = 0;
    if (controller_ready(&stop)) {
        if (after_controller(cli_controller_set_subscriber_count((int)count), "set subscriber count"))
            return;
    } else if (stop) {
        return;
    }
    if (cli_etcd_configured() &&
            after_etcd(cli_etcd_set_subscriber_count((int)count), "set subscriber count"))
        return;
    fastrg_grpc_set_subscriber(count);
}

void cli_dispatch_connect(U16 user_id)
{
    int stop = 0;
    if (controller_ready(&stop)) {
        if (after_controller(cli_controller_dial(user_id), "connect"))
            return;
    } else if (stop) {
        return;
    }
    if (cli_etcd_configured() && after_etcd(cli_etcd_set_desire(user_id, "connect"), "connect"))
        return;
    fastrg_grpc_hsi_connect(user_id);
}

void cli_dispatch_disconnect(U16 user_id, bool force)
{
    int stop = 0;
    if (controller_ready(&stop)) {
        if (after_controller(cli_controller_hangup(user_id), "disconnect"))
            return;
    } else if (stop) {
        return;
    }
    if (cli_etcd_configured() && after_etcd(cli_etcd_set_desire(user_id, "disconnect"), "disconnect"))
        return;
    fastrg_grpc_hsi_disconnect(user_id, force);
}

void cli_dispatch_add_dns(U16 user_id, char *domain, char *ip, U32 ttl)
{
    int stop = 0;
    if (controller_ready(&stop)) {
        if (after_controller(cli_controller_add_dns(user_id, domain, ip, ttl), "add dns record"))
            return;
    } else if (stop) {
        return;
    }
    if (cli_etcd_configured() &&
            after_etcd(cli_etcd_add_dns(user_id, domain, ip, ttl), "add dns record"))
        return;
    fastrg_grpc_add_dns_record(user_id, domain, ip, ttl);
}

void cli_dispatch_del_dns(U16 user_id, char *domain)
{
    int stop = 0;
    if (controller_ready(&stop)) {
        if (after_controller(cli_controller_del_dns(user_id, domain), "remove dns record"))
            return;
    } else if (stop) {
        return;
    }
    if (cli_etcd_configured() && after_etcd(cli_etcd_del_dns(user_id, domain), "remove dns record"))
        return;
    fastrg_grpc_remove_dns_record(user_id, domain);
}

void cli_dispatch_snat_set(U16 user_id, U16 eport, char *dip, U16 iport)
{
    int stop = 0;
    if (controller_ready(&stop)) {
        if (after_controller(cli_controller_snat_set(user_id, eport, dip, iport), "set snat"))
            return;
    } else if (stop) {
        return;
    }
    if (cli_etcd_configured() &&
            after_etcd(cli_etcd_snat_set(user_id, eport, dip, iport), "set snat"))
        return;
    fastrg_grpc_hsi_snat_set(user_id, eport, dip, iport);
}

void cli_dispatch_snat_unset(U16 user_id, U16 eport)
{
    int stop = 0;
    if (controller_ready(&stop)) {
        if (after_controller(cli_controller_snat_unset(user_id, eport), "unset snat"))
            return;
    } else if (stop) {
        return;
    }
    if (cli_etcd_configured() && after_etcd(cli_etcd_snat_unset(user_id, eport), "unset snat"))
        return;
    fastrg_grpc_hsi_snat_unset(user_id, eport);
}

void cli_dispatch_set_dns_proxy(U16 user_id, bool enable)
{
    int stop = 0;
    if (controller_ready(&stop)) {
        if (after_controller(cli_controller_set_dns_proxy(user_id, enable ? 1 : 0), "set dns_proxy"))
            return;
    } else if (stop) {
        return;
    }
    if (cli_etcd_configured() &&
            after_etcd(cli_etcd_set_dns_proxy(user_id, enable ? 1 : 0), "set dns_proxy"))
        return;
    fastrg_grpc_set_dns_proxy(user_id, enable);
}

void cli_dispatch_set_tcp_conntrack(U16 user_id, bool enable)
{
    int stop = 0;
    if (controller_ready(&stop)) {
        if (after_controller(cli_controller_set_tcp_conntrack(user_id, enable ? 1 : 0), "set tcp_conntrack"))
            return;
    } else if (stop) {
        return;
    }
    if (cli_etcd_configured() &&
            after_etcd(cli_etcd_set_tcp_conntrack(user_id, enable ? 1 : 0), "set tcp_conntrack"))
        return;
    fastrg_grpc_set_tcp_conntrack(user_id, enable);
}
