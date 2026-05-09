/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  CONFIG.H

  Designed by THE on Sep 15, 2023
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <common.h>

typedef struct FastRG FastRG_t;

#define CONFIG_DIR_PATH "/etc/fastrg/"

struct fastrg_config {
    char unix_sock_path[256];
    char node_grpc_ip_port[256];
    char log_path[256];
    char controller_address[256];
    char etcd_endpoints[512];
    U16 heartbeat_interval;
    char ddp_pkg_path[256]; /* path to i40e DDP package; empty string = disabled */
};

/**
 * @fn parse_config
 * @brief
 *      Parse FastRG configuration file
 * @param config_path
 *      Path to configuration file
 * @param fastrg_ccb
 *      Pointer to FastRG context
 * @param fastrg_cfg
 *      Pointer to fastrg_config struct to fill
 * @return
 *      SUCCESS on success, ERROR on failure
 */
STATUS parse_config(const char *config_path, FastRG_t *fastrg_ccb, struct fastrg_config *fastrg_cfg);

#endif /* _CONFIG_H_ */
