#ifndef FastRG_GRPC_SERVER_H
#define FastRG_GRPC_CLIENT_H

#include <common.h>

#ifdef __cplusplus
extern "C" {
#endif

void fastrg_grpc_apply_config(U16 user_id, U16 vlan_id, char *pppoe_account, 
    char *pppoe_password, char *dhcp_pool_start, char *dhcp_pool_end, 
    char *dhcp_subnet_mask, char *dhcp_gateway);
void fastrg_grpc_remove_config(U16 user_id);
void fastrg_grpc_set_subscriber(U16 subscriber_count);
void fastrg_grpc_client_connect(char *server_address);
void fastrg_grpc_hsi_connect(U16 user_id);
void fastrg_grpc_hsi_disconnect(U16 user_id, bool force);
void fastrg_grpc_dhcp_server_start(U16 user_id);
void fastrg_grpc_dhcp_server_stop(U16 user_id);
void fastrg_grpc_get_system_info();
void fastrg_grpc_get_system_stats();
void fastrg_grpc_get_system_xstats();
void fastrg_grpc_get_hsi_info();
void fastrg_grpc_get_dhcp_info();

#ifdef __cplusplus
}
#endif

#endif // FastRG_GRPC_CLIENT_H
