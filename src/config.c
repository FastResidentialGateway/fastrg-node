#include <common.h>

#include <libconfig.h>

#include "fastrg.h"
#include "dbg.h"
#include "config.h"

STATUS is_unix_sock_path_valid(char *unix_sock_path)
{
    char *unix_sock_dir = NULL;
    size_t unix_sock_dir_len = 0;
    char dir_path[512];

    if (!unix_sock_path) {
        fprintf(stderr, "unix socket path buffer is NULL\n");
        return ERROR;
    }

    if (parse_unix_sock_path(unix_sock_path, &unix_sock_dir, &unix_sock_dir_len) != SUCCESS) {
        fprintf(stderr, "unix socket path format error\n");
        return ERROR;
    }
    if (unix_sock_dir_len >= sizeof(dir_path)) {
        fprintf(stderr, "unix socket dir path too long\n");
        return ERROR;
    }

    strncpy(dir_path, unix_sock_dir, unix_sock_dir_len);
    dir_path[unix_sock_dir_len] = '\0';

    if (create_dir_if_not_exists(dir_path) != SUCCESS) {
        fprintf(stderr, "create unix socket dir %s failed\n", unix_sock_dir);
        return ERROR;
    }

    return SUCCESS;
}

STATUS parse_config(const char *config_path, FastRG_t *fastrg_ccb, struct fastrg_config *fastrg_cfg) 
{
    config_t cfg;
    int user_count, heartbeat_interval;
    const char *loglvl, *unix_sock_path, *log_path, *node_grpc_port, *controller_address, *etcd_endpoints, *ddp_pkg_path;

    config_init(&cfg);
    if (!config_read_file(&cfg, config_path)) {
        fprintf(stderr, "read config file %s content error: %s:%d - %s\n", 
                config_path, config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        return ERROR;
    }

    if (config_lookup_int(&cfg, "UserCount", &user_count) == CONFIG_FALSE)
        user_count = 1;
    if (user_count < MIN_USER_COUNT || user_count > MAX_USER_COUNT) {
        fprintf(stderr, "user count must be between %d and %d\n", MIN_USER_COUNT, MAX_USER_COUNT);
        config_destroy(&cfg);
        return ERROR;
    }
    fastrg_ccb->user_count = user_count;

    if (config_lookup_string(&cfg, "Loglvl", &loglvl) == CONFIG_FALSE)
        loglvl = "DBG";
    fastrg_ccb->loglvl = logstr2lvl(loglvl);
    if (fastrg_ccb->loglvl == 0) {
        fprintf(stderr, "log level error\n");
        config_destroy(&cfg);
        return ERROR;
    }

    if (config_lookup_string(&cfg, "LogPath", &log_path) == CONFIG_FALSE) {
        log_path = "/var/log/fastrg/fastrg.log";
        printf("log path not found, use default path: %s\n", log_path);
    }
    strncpy(fastrg_cfg->log_path, log_path, sizeof(fastrg_cfg->log_path) - 1);
    fastrg_cfg->log_path[sizeof(fastrg_cfg->log_path) - 1] = '\0';

    if (config_lookup_string(&cfg, "NodeGrpcUnixSocket", &unix_sock_path) == CONFIG_FALSE)
        unix_sock_path = "unix:///var/run/fastrg/fastrg.sock";
    strncpy(fastrg_cfg->unix_sock_path, unix_sock_path, sizeof(fastrg_cfg->unix_sock_path) - 1);
    fastrg_cfg->unix_sock_path[sizeof(fastrg_cfg->unix_sock_path) - 1] = '\0';
    if (is_unix_sock_path_valid(fastrg_cfg->unix_sock_path) != SUCCESS) {
        config_destroy(&cfg);
        return ERROR;
    }

    if (config_lookup_string(&cfg, "NodeGrpcPort", &node_grpc_port) == CONFIG_FALSE)
        node_grpc_port = "50052";  
    int port_num = atoi(node_grpc_port);
    if (port_num <= 0 || port_num > 65535) {
        fprintf(stderr, "Invalid port number: %s (must be 1-65535)\n", node_grpc_port);
        config_destroy(&cfg);
        return ERROR;
    }
    snprintf(fastrg_cfg->node_grpc_ip_port, sizeof(fastrg_cfg->node_grpc_ip_port), "0.0.0.0:%s", node_grpc_port);

    if (config_lookup_string(&cfg, "ControllerAddress", &controller_address) == CONFIG_FALSE)
        controller_address = "127.0.0.1:50051";
    strncpy(fastrg_cfg->controller_address, controller_address, sizeof(fastrg_cfg->controller_address) - 1);
    fastrg_cfg->controller_address[sizeof(fastrg_cfg->controller_address) - 1] = '\0';

    if (config_lookup_int(&cfg, "HeartbeatInterval", &heartbeat_interval) == CONFIG_FALSE)
        heartbeat_interval = 30;
    if (heartbeat_interval <= 0) {
        fprintf(stderr, "heartbeat interval must be positive\n");
        config_destroy(&cfg);
        return ERROR;
    }
    fastrg_cfg->heartbeat_interval = heartbeat_interval;

    if (config_lookup_string(&cfg, "EtcdEndpoints", &etcd_endpoints) == CONFIG_FALSE)
        etcd_endpoints = "127.0.0.1:2379";
    strncpy(fastrg_cfg->etcd_endpoints, etcd_endpoints, sizeof(fastrg_cfg->etcd_endpoints) - 1);
    fastrg_cfg->etcd_endpoints[sizeof(fastrg_cfg->etcd_endpoints) - 1] = '\0';

    if (config_lookup_string(&cfg, "I40eDdpPkgPath", &ddp_pkg_path) == CONFIG_FALSE)
        ddp_pkg_path = "";
    strncpy(fastrg_cfg->ddp_pkg_path, ddp_pkg_path, sizeof(fastrg_cfg->ddp_pkg_path) - 1);
    fastrg_cfg->ddp_pkg_path[sizeof(fastrg_cfg->ddp_pkg_path) - 1] = '\0';

    config_destroy(&cfg);

    return SUCCESS;
}