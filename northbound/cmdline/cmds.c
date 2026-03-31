#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdio.h>
#include <termios.h>
#include <errno.h>
#include <sys/queue.h>

#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_mempool.h>
#include <rte_string_fns.h>
#include <rte_byteorder.h>
#include <getopt.h>

#include <cmdline_rdline.h>
#include <cmdline_parse.h>
#include <cmdline_parse_string.h>
#include <cmdline_parse_num.h>
#include <cmdline_parse_ipaddr.h>
#include <cmdline_socket.h>
#include <cmdline.h>

#include <pppd/pppd.h>
#include <pppd/header.h>
#include <dhcpd/dhcp_codec.h>
#include <init.h>
#include <fastrg.h>
#include <utils.h>

#include <grpc/grpc.h>

#include "../grpc/fastrg_grpc_client.h"

#include "cmds.h"

#define PARSE_DELIMITER	" \f\n\r\t\v"

/**********************************************************/

struct cmd_info_result {
    cmdline_fixed_string_t info_token;
    cmdline_fixed_string_t subsystem;
};

static void cmd_info_parsed(void *parsed_result,
        struct cmdline *cl,
        __attribute__((unused)) void *data)
{
    struct cmd_info_result *res = parsed_result;

    if (strncmp(res->subsystem, "hsi", 3) == 0)
        fastrg_grpc_get_hsi_info();
    else if (strncmp(res->subsystem, "dhcp", 4) == 0)
        fastrg_grpc_get_dhcp_info();

    return;
}

cmdline_parse_token_string_t cmd_info_info_token =
    TOKEN_STRING_INITIALIZER(struct cmd_info_result, info_token, "show");
cmdline_parse_token_string_t cmd_show_subsystem =
    TOKEN_STRING_INITIALIZER(struct cmd_info_result, subsystem, "hsi#dhcp");

cmdline_parse_inst_t cmd_info = {
    .f = cmd_info_parsed,  /* function to call */
    .data = NULL,      /* 2nd arg of func */
    .help_str = "show subscriber info, show <hsi|dhcp>",
    .tokens = {        /* token list, NULL terminated */
            (void *)&cmd_info_info_token,
            (void *)&cmd_show_subsystem,
            NULL,
    },
};

/**********************************************************/

struct cmd_system_result {
    cmdline_fixed_string_t show_token;
    cmdline_fixed_string_t system_token;
    cmdline_fixed_string_t subcmd;
};

static void cmd_system_parsed(void *parsed_result,
        struct cmdline *cl,
        __attribute__((unused)) void *data)
{
    struct cmd_system_result *res = parsed_result;

    if (strncmp(res->subcmd, "info", 4) == 0)
        fastrg_grpc_get_system_info();
    else if (strncmp(res->subcmd, "stats", 5) == 0)
        fastrg_grpc_get_system_stats();
    else if (strncmp(res->subcmd, "xstats", 6) == 0)
        fastrg_grpc_get_system_xstats();

    return;
}

cmdline_parse_token_string_t cmd_system_show_token =
    TOKEN_STRING_INITIALIZER(struct cmd_system_result, show_token, "show");
cmdline_parse_token_string_t cmd_system_system_token =
    TOKEN_STRING_INITIALIZER(struct cmd_system_result, system_token, "system");
cmdline_parse_token_string_t cmd_system_subcmd =
    TOKEN_STRING_INITIALIZER(struct cmd_system_result, subcmd, "info#stats#xstats");

cmdline_parse_inst_t cmd_system = {
    .f = cmd_system_parsed,  /* function to call */
    .data = NULL,      /* 2nd arg of func */
    .help_str = "show system <info|stats|xstats>",
    .tokens = {        /* token list, NULL terminated */
            (void *)&cmd_system_show_token,
            (void *)&cmd_system_system_token,
            (void *)&cmd_system_subcmd,
            NULL,
    },
};

/**********************************************************/

struct cmd_log_result {
    cmdline_fixed_string_t log_token;
};

static void cmd_log_parsed(__attribute__((unused)) void *parsed_result,
        struct cmdline *cl,
        __attribute__((unused)) void *data)
{
#if 0
    char log_buf[256];

    while (fgets(log_buf, 256, fastrg_ccb->fp) != NULL)
        cmdline_printf(cl, "%s", log_buf);
#endif
    cmdline_printf(cl, "\n");
}

cmdline_parse_token_string_t cmd_log_log_token =
    TOKEN_STRING_INITIALIZER(struct cmd_log_result, log_token, "log");

cmdline_parse_inst_t cmd_log = {
    .f = cmd_log_parsed,  /* function to call */
    .data = NULL,      /* 2nd arg of func */
    .help_str = "show FastRG log file",
    .tokens = {        /* token list, NULL terminated */
            (void *)&cmd_log_log_token,
            NULL,
    },
};

/**********************************************************/

struct cmd_quit_result {
    cmdline_fixed_string_t quit;
};

static void cmd_quit_parsed(__attribute__((unused)) void *parsed_result,
                __attribute__((unused)) struct cmdline *cl,
                __attribute__((unused)) void *data)
{
    cmdline_stdin_exit(cl);
    exit(0);
}

cmdline_parse_token_string_t cmd_quit_quit =
    TOKEN_STRING_INITIALIZER(struct cmd_quit_result, quit, "quit#exit");

cmdline_parse_inst_t cmd_quit = {
    .f = cmd_quit_parsed,  /* function to call */
    .data = NULL,      /* 2nd arg of func */
    .help_str = "close the application",
    .tokens = {        /* token list, NULL terminated */
        (void *)&cmd_quit_quit,
        NULL,
    },
};

/**********************************************************/

struct cmd_help_result {
    cmdline_fixed_string_t help;
};

static void cmd_help_parsed(__attribute__((unused)) void *parsed_result,
                struct cmdline *cl,
                __attribute__((unused)) void *data)
{
    cmdline_printf(cl,"usage: \n"
                      "show <hsi|dhcp> to show information\n"
                      "show system <info|stats|xstats> to show system info/stats/xstats\n"
                      "config <add|del> user <id> pppoe-dhcp vlan <id> account <account> password <password> pool <start~end> subnet <mask> gateway <ip> to add/update/del PPPoE/DHCP configuration\n"
                      "configure SNAT port forwarding: config <add|del> user <id> snat eport <port> dip <ip> iport <port>\n"
                      "show user <id> nat port-forwarding to show port forwarding entries\n"
                      "show user <id> arp-table [count] to show subscriber ARP table (default: 100 entries)\n"
                      "exec hsi <start|stop> <user id | all> to start/stop HSI (PPPoE + DHCP)\n"
                      "exec pppoe <start|stop> <user id | all> to start/stop PPPoE only\n"
                      "exec dhcp-server <start|stop> <user id | all> to start/stop DHCP server only\n"
                      "help to show usage commands\n"
                      "quit/exit to quit FastRG CLI\n");
}

cmdline_parse_token_string_t cmd_help_help =
    TOKEN_STRING_INITIALIZER(struct cmd_help_result, help, "help");

cmdline_parse_inst_t cmd_help = {
    .f = cmd_help_parsed,  /* function to call */
    .data = NULL,      /* 2nd arg of func */
    .help_str = "show help",
    .tokens = {        /* token list, NULL terminated */
        (void *)&cmd_help_help,
        NULL,
    },
};

/**********************************************************/

struct cmd_config_result {
    cmdline_fixed_string_t 	config;
    cmdline_fixed_string_t 	cmd_str;        /* add/del */
    cmdline_fixed_string_t 	user_str;
    uint16_t 				user_id;
    cmdline_fixed_string_t 	service_type;   /* pppoe-dhcp/snat */
    cmdline_fixed_string_t 	vlan_str;
    uint16_t 				vlan_id;
    cmdline_fixed_string_t 	account_str;
    cmdline_fixed_string_t 	pppoe_account;
    cmdline_fixed_string_t 	password_str;
    cmdline_fixed_string_t 	pppoe_password;
    cmdline_fixed_string_t 	pool_str;
    cmdline_fixed_string_t 	ip_pool_range;  /* format: 192.168.3.2~192.168.3.5 */
    cmdline_fixed_string_t 	subnet_str;
    cmdline_ipaddr_t 		subnet_mask;
    cmdline_fixed_string_t 	gateway_str;
    cmdline_ipaddr_t 		gateway_ip;
    cmdline_fixed_string_t 	eport_str;      /* external port */
    uint16_t 				eport;
    cmdline_fixed_string_t 	dip_str;        /* destination ip */
    cmdline_ipaddr_t 		dip;
    cmdline_fixed_string_t 	iport_str;      /* internal port */
    uint16_t 				iport;
};

static void cmd_config_pppoe_dhcp_parsed(void *parsed_result,
                struct cmdline *cl,
                __attribute__((unused)) void *data)
{
    struct cmd_config_result *res = parsed_result;
    char pool_start[32] = {0};
    char pool_end[32] = {0};
    char subnet_str[32] = {0};
    char gateway_str[32] = {0};
    char *tilde_pos;

    if (strncmp(res->cmd_str, "del", 3) == 0) {
        /* Call gRPC function to remove the configuration */
        fastrg_grpc_remove_config(res->user_id);
        return;
    }

    /* Parse IP pool range (192.168.3.2~192.168.3.5) */
    tilde_pos = strchr(res->ip_pool_range, '~');
    if (tilde_pos == NULL) {
        tilde_pos = strchr(res->ip_pool_range, '-');
        if (tilde_pos == NULL) {
            cmdline_printf(cl, "Invalid IP pool range format. Expected: start_ip~end_ip or start_ip-end_ip\n");
            return;
        }
    }

    /* Extract start and end IP addresses */
    size_t start_len = tilde_pos - res->ip_pool_range;
    strncpy(pool_start, res->ip_pool_range, start_len);
    pool_start[start_len] = '\0';
    strcpy(pool_end, tilde_pos + 1);

    /* Format subnet mask */
    if (res->subnet_mask.family == AF_INET) {
        snprintf(subnet_str, sizeof(subnet_str), "%u.%u.%u.%u",
            (res->subnet_mask.addr.ipv4.s_addr) & 0xFF,
            (res->subnet_mask.addr.ipv4.s_addr >> 8) & 0xFF,
            (res->subnet_mask.addr.ipv4.s_addr >> 16) & 0xFF,
            (res->subnet_mask.addr.ipv4.s_addr >> 24) & 0xFF);
    }

    /* Format gateway IP */
    if (res->gateway_ip.family == AF_INET) {
        snprintf(gateway_str, sizeof(gateway_str), "%u.%u.%u.%u",
            (res->gateway_ip.addr.ipv4.s_addr) & 0xFF,
            (res->gateway_ip.addr.ipv4.s_addr >> 8) & 0xFF,
            (res->gateway_ip.addr.ipv4.s_addr >> 16) & 0xFF,
            (res->gateway_ip.addr.ipv4.s_addr >> 24) & 0xFF);
    }

    cmdline_printf(cl, "Configuration for User %u:\n", res->user_id);
    cmdline_printf(cl, "  VLAN ID: %u\n", res->vlan_id);
    cmdline_printf(cl, "  PPPoE Account: %s\n", res->pppoe_account);
    cmdline_printf(cl, "  PPPoE Password: %s\n", res->pppoe_password);
    cmdline_printf(cl, "  DHCP Pool: %s ~ %s\n", pool_start, pool_end);
    cmdline_printf(cl, "  Subnet Mask: %s\n", subnet_str);
    cmdline_printf(cl, "  Gateway IP: %s\n", gateway_str);

    fastrg_grpc_apply_config(res->user_id, res->vlan_id, 
        res->pppoe_account, res->pppoe_password, pool_start, 
        pool_end, subnet_str, gateway_str);
}

static void cmd_config_snat_parsed(void *parsed_result,
                struct cmdline *cl,
                __attribute__((unused)) void *data)
{
    struct cmd_config_result *res = parsed_result;
    char dip_str[32] = {0};

    if (strncmp(res->cmd_str, "del", 3) == 0) {
        /* Call gRPC function to remove the SNAT configuration */
        fastrg_grpc_hsi_snat_unset(res->user_id, res->eport);
        return;
    }

    /* Format destination IP */
    if (res->dip.family == AF_INET) {
        snprintf(dip_str, sizeof(dip_str), "%u.%u.%u.%u",
            (res->dip.addr.ipv4.s_addr) & 0xFF,
            (res->dip.addr.ipv4.s_addr >> 8) & 0xFF,
            (res->dip.addr.ipv4.s_addr >> 16) & 0xFF,
            (res->dip.addr.ipv4.s_addr >> 24) & 0xFF);
    }

    cmdline_printf(cl, "SNAT Configuration for User %u:\n", res->user_id);
    cmdline_printf(cl, "  External Port: %u\n", res->eport);
    cmdline_printf(cl, "  Destination IP: %s\n", dip_str);
    cmdline_printf(cl, "  Internal Port: %u\n", res->iport);

    /* Call gRPC function to apply SNAT configuration */
    fastrg_grpc_hsi_snat_set(res->user_id, res->eport, dip_str, res->iport);
}

cmdline_parse_token_string_t cmd_config_config =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, config, "config");
cmdline_parse_token_string_t cmd_config_add_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, cmd_str, "add");
cmdline_parse_token_string_t cmd_config_del_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, cmd_str, "del");
cmdline_parse_token_string_t cmd_config_user_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, user_str, "user");
cmdline_parse_token_num_t cmd_config_user_id =
    TOKEN_NUM_INITIALIZER(struct cmd_config_result, user_id, RTE_UINT16);

/* PPPoE/DHCP tokens */
cmdline_parse_token_string_t cmd_config_service_pppoe_dhcp =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, service_type, "pppoe-dhcp");
cmdline_parse_token_string_t cmd_config_vlan_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, vlan_str, "vlan");
cmdline_parse_token_num_t cmd_config_vlan_id =
    TOKEN_NUM_INITIALIZER(struct cmd_config_result, vlan_id, RTE_UINT16);
cmdline_parse_token_string_t cmd_config_account_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, account_str, "account");
cmdline_parse_token_string_t cmd_config_pppoe_account =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, pppoe_account, NULL);
cmdline_parse_token_string_t cmd_config_password_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, password_str, "password");
cmdline_parse_token_string_t cmd_config_pppoe_password =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, pppoe_password, NULL);
cmdline_parse_token_string_t cmd_config_pool_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, pool_str, "pool");
cmdline_parse_token_string_t cmd_config_ip_pool_range =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, ip_pool_range, NULL);
cmdline_parse_token_string_t cmd_config_subnet_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, subnet_str, "subnet");
cmdline_parse_token_ipaddr_t cmd_config_subnet_mask =
    TOKEN_IPV4_INITIALIZER(struct cmd_config_result, subnet_mask);
cmdline_parse_token_string_t cmd_config_gateway_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, gateway_str, "gateway");
cmdline_parse_token_ipaddr_t cmd_config_gateway_ip =
    TOKEN_IPV4_INITIALIZER(struct cmd_config_result, gateway_ip);

/* SNAT tokens */
cmdline_parse_token_string_t cmd_config_service_snat =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, service_type, "snat");
cmdline_parse_token_string_t cmd_config_eport_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, eport_str, "eport");
cmdline_parse_token_num_t cmd_config_eport =
    TOKEN_NUM_INITIALIZER(struct cmd_config_result, eport, RTE_UINT16);
cmdline_parse_token_string_t cmd_config_dip_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, dip_str, "dip");
cmdline_parse_token_ipaddr_t cmd_config_dip =
    TOKEN_IPV4_INITIALIZER(struct cmd_config_result, dip);
cmdline_parse_token_string_t cmd_config_iport_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_result, iport_str, "iport");
cmdline_parse_token_num_t cmd_config_iport =
    TOKEN_NUM_INITIALIZER(struct cmd_config_result, iport, RTE_UINT16);

cmdline_parse_inst_t cmd_config_add_pppoe_dhcp = {
    .f = cmd_config_pppoe_dhcp_parsed,  /* function to call */
    .data = NULL,      /* 2nd arg of func */
    .help_str = "configure PPPoE/DHCP settings: config add user <id> pppoe-dhcp vlan <id> account <account> password <password> pool <start~end> subnet <mask> gateway <ip>",
    .tokens = {        /* token list, NULL terminated */
        (void *)&cmd_config_config,
        (void *)&cmd_config_add_str,
        (void *)&cmd_config_user_str,
        (void *)&cmd_config_user_id,
        (void *)&cmd_config_service_pppoe_dhcp,
        (void *)&cmd_config_vlan_str,
        (void *)&cmd_config_vlan_id,
        (void *)&cmd_config_account_str,
        (void *)&cmd_config_pppoe_account,
        (void *)&cmd_config_password_str,
        (void *)&cmd_config_pppoe_password,
        (void *)&cmd_config_pool_str,
        (void *)&cmd_config_ip_pool_range,
        (void *)&cmd_config_subnet_str,
        (void *)&cmd_config_subnet_mask,
        (void *)&cmd_config_gateway_str,
        (void *)&cmd_config_gateway_ip,
        NULL,
    },
};

cmdline_parse_inst_t cmd_config_add_snat = {
    .f = cmd_config_snat_parsed,  /* function to call */
    .data = NULL,      /* 2nd arg of func */
    .help_str = "configure SNAT port forwarding: config add user <id> snat eport <port> dip <ip> iport <port>",
    .tokens = {        /* token list, NULL terminated */
        (void *)&cmd_config_config,
        (void *)&cmd_config_add_str,
        (void *)&cmd_config_user_str,
        (void *)&cmd_config_user_id,
        (void *)&cmd_config_service_snat,
        (void *)&cmd_config_eport_str,
        (void *)&cmd_config_eport,
        (void *)&cmd_config_dip_str,
        (void *)&cmd_config_dip,
        (void *)&cmd_config_iport_str,
        (void *)&cmd_config_iport,
        NULL,
    },
};

cmdline_parse_inst_t cmd_config_del_snat = {
    .f = cmd_config_snat_parsed,  /* function to call */
    .data = NULL,      /* 2nd arg of func */
    .help_str = "delete user SNAT configuration: config del user <id> snat eport <port>",
    .tokens = {        /* token list, NULL terminated */
        (void *)&cmd_config_config,
        (void *)&cmd_config_del_str,
        (void *)&cmd_config_user_str,
        (void *)&cmd_config_user_id,
        (void *)&cmd_config_service_snat,
        (void *)&cmd_config_eport_str,
        (void *)&cmd_config_eport,
        NULL,
    },
};

cmdline_parse_inst_t cmd_config_del = {
    .f = cmd_config_pppoe_dhcp_parsed,  /* function to call */
    .data = NULL,      /* 2nd arg of func */
    .help_str = "delete user configuration: config del user <id>",
    .tokens = {        /* token list, NULL terminated */
        (void *)&cmd_config_config,
        (void *)&cmd_config_del_str,
        (void *)&cmd_config_user_str,
        (void *)&cmd_config_user_id,
        NULL,
    },
};

/**********************************************************/

struct cmd_config_subscriber_count_result {
    cmdline_fixed_string_t 	config;
    cmdline_fixed_string_t 	cmd_str;        /* set */
    cmdline_fixed_string_t 	subscriber_str;
    uint16_t 				subscriber_count;
};

static void cmd_config_parse_subscriber_count(void *parsed_result,
                struct cmdline *cl,
                __attribute__((unused)) void *data)
{
    struct cmd_config_subscriber_count_result *res = parsed_result;

    cmdline_printf(cl, "Configuration subscriber count to %u\n", res->subscriber_count);

    fastrg_grpc_set_subscriber(res->subscriber_count);
}

cmdline_parse_token_string_t cmd_config_subscriber_config =
    TOKEN_STRING_INITIALIZER(struct cmd_config_subscriber_count_result, config, "config");
cmdline_parse_token_string_t cmd_config_set_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_subscriber_count_result, cmd_str, "set");
cmdline_parse_token_string_t cmd_config_subscriber_str =
    TOKEN_STRING_INITIALIZER(struct cmd_config_subscriber_count_result, subscriber_str, "subscriber");
cmdline_parse_token_num_t cmd_config_subscriber_count =
    TOKEN_NUM_INITIALIZER(struct cmd_config_subscriber_count_result, subscriber_count, RTE_UINT16);

cmdline_parse_inst_t cmd_config_set_subscriber = {
    .f = cmd_config_parse_subscriber_count,  /* function to call */
    .data = NULL,      /* 2nd arg of func */
    .help_str = "configure subscriber count: config set subscriber <count>",
    .tokens = {        /* token list, NULL terminated */
        (void *)&cmd_config_subscriber_config,
        (void *)&cmd_config_set_str,
        (void *)&cmd_config_subscriber_str,
        (void *)&cmd_config_subscriber_count,
        NULL,
    },
};

/**********************************************************/

struct cmd_exec_result {
    cmdline_fixed_string_t exec_token;
    cmdline_fixed_string_t subsystem;
    cmdline_fixed_string_t action;
    cmdline_fixed_string_t user_id;
};

static void cmd_exec_parsed(void *parsed_result,
                struct cmdline *cl,
                __attribute__((unused)) void *data)
{
    struct cmd_exec_result *res = parsed_result;
    U16 user_id;

    if (strcmp(res->user_id, "all") == 0) {
        user_id = 0;
    } else {
        user_id = strtoul(res->user_id, NULL, 10);
        if (user_id <= 0) {
            cmdline_printf(cl, "Wrong user id\n");
            return;
        }
    }

    if (strncmp(res->subsystem, "hsi", 3) == 0) {
        if (strcmp(res->action, "start") == 0) {
            fastrg_grpc_hsi_connect(user_id);
            fastrg_grpc_dhcp_server_start(user_id);
        } else {
            fastrg_grpc_hsi_disconnect(user_id, false);
            fastrg_grpc_dhcp_server_stop(user_id);
        }
    } else if (strncmp(res->subsystem, "pppoe", 5) == 0) {
        if (strcmp(res->action, "start") == 0)
            fastrg_grpc_hsi_connect(user_id);
        else
            fastrg_grpc_hsi_disconnect(user_id, false);
    } else if (strncmp(res->subsystem, "dhcp-server", 11) == 0) {
        if (strcmp(res->action, "start") == 0)
            fastrg_grpc_dhcp_server_start(user_id);
        else
            fastrg_grpc_dhcp_server_stop(user_id);
    }
}

cmdline_parse_token_string_t cmd_exec_exec_token =
    TOKEN_STRING_INITIALIZER(struct cmd_exec_result, exec_token, "exec");
cmdline_parse_token_string_t cmd_exec_subsystem =
    TOKEN_STRING_INITIALIZER(struct cmd_exec_result, subsystem, "hsi#pppoe#dhcp-server");
cmdline_parse_token_string_t cmd_exec_action =
    TOKEN_STRING_INITIALIZER(struct cmd_exec_result, action, "start#stop");
cmdline_parse_token_string_t cmd_exec_user_id =
    TOKEN_STRING_INITIALIZER(struct cmd_exec_result, user_id, NULL);

cmdline_parse_inst_t cmd_exec = {
    .f = cmd_exec_parsed,  /* function to call */
    .data = NULL,      /* 2nd arg of func */
    .help_str = "exec <hsi|pppoe|dhcp-server> <start|stop> <user id | all>",
    .tokens = {        /* token list, NULL terminated */
        (void *)&cmd_exec_exec_token,
        (void *)&cmd_exec_subsystem,
        (void *)&cmd_exec_action,
        (void *)&cmd_exec_user_id,
        NULL,
    },
};

/****** SHOW USER NAT PORT-FORWARDING ******/

struct cmd_show_port_fwd_result {
    cmdline_fixed_string_t show_token;
    cmdline_fixed_string_t user_str;
    uint16_t               user_id;
    cmdline_fixed_string_t nat_str;
    cmdline_fixed_string_t port_fwd_str;
};

static void cmd_show_port_fwd_parsed(void *parsed_result,
                struct cmdline *cl,
                __attribute__((unused)) void *data)
{
    struct cmd_show_port_fwd_result *res = parsed_result;
    fastrg_grpc_get_port_fwd_info(res->user_id);
}

cmdline_parse_token_string_t cmd_show_port_fwd_show =
    TOKEN_STRING_INITIALIZER(struct cmd_show_port_fwd_result, show_token, "show");
cmdline_parse_token_string_t cmd_show_port_fwd_user =
    TOKEN_STRING_INITIALIZER(struct cmd_show_port_fwd_result, user_str, "user");
cmdline_parse_token_num_t cmd_show_port_fwd_user_id =
    TOKEN_NUM_INITIALIZER(struct cmd_show_port_fwd_result, user_id, RTE_UINT16);
cmdline_parse_token_string_t cmd_show_port_fwd_nat =
    TOKEN_STRING_INITIALIZER(struct cmd_show_port_fwd_result, nat_str, "nat");
cmdline_parse_token_string_t cmd_show_port_fwd_pfwd =
    TOKEN_STRING_INITIALIZER(struct cmd_show_port_fwd_result, port_fwd_str, "port-forwarding");

cmdline_parse_inst_t cmd_show_port_fwd = {
    .f = cmd_show_port_fwd_parsed,
    .data = NULL,
    .help_str = "show user <id> nat port-forwarding: display port forwarding entries for a subscriber",
    .tokens = {
        (void *)&cmd_show_port_fwd_show,
        (void *)&cmd_show_port_fwd_user,
        (void *)&cmd_show_port_fwd_user_id,
        (void *)&cmd_show_port_fwd_nat,
        (void *)&cmd_show_port_fwd_pfwd,
        NULL,
    },
};

/****** SHOW USER ARP-TABLE ******/

struct cmd_show_arp_table_result {
    cmdline_fixed_string_t show_token;
    cmdline_fixed_string_t user_str;
    uint16_t               user_id;
    cmdline_fixed_string_t arp_table_str;
    uint16_t               count;
};

static void cmd_show_arp_table_parsed(void *parsed_result,
                struct cmdline *cl,
                __attribute__((unused)) void *data)
{
    struct cmd_show_arp_table_result *res = parsed_result;
    fastrg_grpc_get_arp_table(res->user_id, 100);
}

static void cmd_show_arp_table_count_parsed(void *parsed_result,
                struct cmdline *cl,
                __attribute__((unused)) void *data)
{
    struct cmd_show_arp_table_result *res = parsed_result;
    fastrg_grpc_get_arp_table(res->user_id, (U32)res->count);
}

cmdline_parse_token_string_t cmd_show_arp_table_show =
    TOKEN_STRING_INITIALIZER(struct cmd_show_arp_table_result, show_token, "show");
cmdline_parse_token_string_t cmd_show_arp_table_user =
    TOKEN_STRING_INITIALIZER(struct cmd_show_arp_table_result, user_str, "user");
cmdline_parse_token_num_t cmd_show_arp_table_user_id =
    TOKEN_NUM_INITIALIZER(struct cmd_show_arp_table_result, user_id, RTE_UINT16);
cmdline_parse_token_string_t cmd_show_arp_table_arp =
    TOKEN_STRING_INITIALIZER(struct cmd_show_arp_table_result, arp_table_str, "arp-table");
cmdline_parse_token_num_t cmd_show_arp_table_count_val =
    TOKEN_NUM_INITIALIZER(struct cmd_show_arp_table_result, count, RTE_UINT16);

/* Variant with count */
cmdline_parse_inst_t cmd_show_arp_table_count = {
    .f = cmd_show_arp_table_count_parsed,
    .data = NULL,
    .help_str = "show user <id> arp-table <count>: display ARP table entries for a subscriber (with count)",
    .tokens = {
        (void *)&cmd_show_arp_table_show,
        (void *)&cmd_show_arp_table_user,
        (void *)&cmd_show_arp_table_user_id,
        (void *)&cmd_show_arp_table_arp,
        (void *)&cmd_show_arp_table_count_val,
        NULL,
    },
};

/* Variant without count (defaults to 100) */
cmdline_parse_inst_t cmd_show_arp_table = {
    .f = cmd_show_arp_table_parsed,
    .data = NULL,
    .help_str = "show user <id> arp-table: display ARP table entries for a subscriber (default: 100)",
    .tokens = {
        (void *)&cmd_show_arp_table_show,
        (void *)&cmd_show_arp_table_user,
        (void *)&cmd_show_arp_table_user_id,
        (void *)&cmd_show_arp_table_arp,
        NULL,
    },
};

/****** CONTEXT (list of instruction) */
cmdline_parse_ctx_t ctx[] = {
        (cmdline_parse_inst_t *)&cmd_info,
        (cmdline_parse_inst_t *)&cmd_system,
        (cmdline_parse_inst_t *)&cmd_quit,
        (cmdline_parse_inst_t *)&cmd_help,
        (cmdline_parse_inst_t *)&cmd_config_add_pppoe_dhcp,
        (cmdline_parse_inst_t *)&cmd_config_add_snat,
        (cmdline_parse_inst_t *)&cmd_config_del_snat,
        (cmdline_parse_inst_t *)&cmd_config_del,
        (cmdline_parse_inst_t *)&cmd_config_set_subscriber,
        (cmdline_parse_inst_t *)&cmd_exec,
        (cmdline_parse_inst_t *)&cmd_show_port_fwd,
        (cmdline_parse_inst_t *)&cmd_show_arp_table_count,
        (cmdline_parse_inst_t *)&cmd_show_arp_table,
        (cmdline_parse_inst_t *)&cmd_log,
    NULL,
};

static void print_usage(const char *prog_name)
{
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("Options:\n");
    printf("  -s, --socket <path>    Connect to Unix socket (default: unix:///var/run/fastrg/fastrg.sock)\n");
    printf("  -i, --ip <address>     Connect to IP address (e.g., 127.0.0.1:50051)\n");
    printf("  -h, --help             Show this help message\n");
    printf("\nIf no option is specified, Unix socket connection is used by default.\n");
}

int main(int argc, char **argv)
{
    char *grpc_target = NULL;
    char target_buffer[256];
    int opt;

    static struct option long_options[] = {
        {"socket", required_argument, 0, 's'},
        {"ip", required_argument, 0, 'i'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    /* Parse command line arguments */
    while ((opt = getopt_long(argc, argv, "s:i:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 's':
            /* Unix socket path provided */
            snprintf(target_buffer, sizeof(target_buffer), "unix://%s", optarg);
            grpc_target = target_buffer;
            break;
        case 'i':
            /* IP address provided */
            snprintf(target_buffer, sizeof(target_buffer), "%s", optarg);
            grpc_target = target_buffer;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    /* If no option specified, use default Unix socket */
    if (grpc_target == NULL)
        grpc_target = "unix:///var/run/fastrg/fastrg.sock";

    grpc_init();
    printf("Connecting to gRPC server at: %s\n", grpc_target);
    fastrg_grpc_client_connect(grpc_target);

    struct cmdline *cl = cmdline_stdin_new(ctx, "FastRG> ");
    if (cl == NULL) {
        grpc_shutdown();
        return -1;
    }

    cmdline_interact(cl);

    cmdline_stdin_exit(cl);
    grpc_shutdown();
    return 0;
}
