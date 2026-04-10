#include <grpc++/grpc++.h>
#include <fstream>
#include <sys/utsname.h>
#include <ifaddrs.h>
#include "fastrg_node_grpc.h"
#include "../controller/etcd_client.h"

#ifdef __cplusplus
extern "C" 
{
#endif

#include <rte_eal.h>
#include <rte_version.h>
#include <rte_ethdev.h>
#include <rte_atomic.h>
#include "../../src/fastrg.h"
#include "../../src/dhcpd/dhcpd.h"
#include "../../src/northbound.h"
#include "../../src/pppd/pppd.h"
#include "../../src/etcd_integration.h"
#include "../../src/dnsd/dnsd.h"
#include "../../src/dnsd/dns_cache.h"
#include "../../src/dnsd/dns_static.h"

#ifdef __cplusplus
}
#endif

using namespace std;
using namespace fastrgnodeservice;

grpc::Status FastRGNodeServiceImpl::ApplyConfig(::grpc::ServerContext* context, const ::fastrgnodeservice::ConfigRequest* request, ::fastrgnodeservice::ConfigReply* response)
{
    uint16_t user_id = request->user_id(), ccb_id = request->user_id() - 1;
    uint16_t vlan_id = request->vlan_id();
    string pppoe_account = request->pppoe_account();
    string pppoe_password = request->pppoe_password();
    string dhcp_pool_start = request->dhcp_pool_start();
    string dhcp_pool_end = request->dhcp_pool_end();
    string dhcp_subnet_mask = request->dhcp_subnet_mask();
    string dhcp_gateway = request->dhcp_gateway();

    cout << "Config called" << endl;

    cout << "User ID: " << user_id << endl;
    cout << "VLAN ID: " << vlan_id << endl;
    cout << "PPPoE Account: " << pppoe_account << endl;
    cout << "PPPoE Password: " << pppoe_password << endl;
    cout << "DHCP Pool Start: " << dhcp_pool_start << endl;
    cout << "DHCP Pool End: " << dhcp_pool_end << endl;
    cout << "DHCP Subnet Mask: " << dhcp_subnet_mask << endl;
    cout << "DHCP Gateway: " << dhcp_gateway << endl;

    if (user_id > fastrg_ccb->user_count) {
        std::string err = "Error! User " + std::to_string(user_id) + " is not exist";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err);
    }

    if (vlan_id > MAX_VLAN_ID || vlan_id < 2) {
        std::string err = "Error! VLAN ID " + std::to_string(vlan_id) + " is invalid";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err);
    }

    hsi_config_t hsi_config = { 0 };
    snprintf(hsi_config.vlan_id, sizeof(hsi_config.vlan_id), "%d", vlan_id);
    snprintf(hsi_config.user_id, sizeof(hsi_config.user_id), "%d", user_id);
    strncpy(hsi_config.account_name, pppoe_account.c_str(), sizeof(hsi_config.account_name) - 1);
    strncpy(hsi_config.password, pppoe_password.c_str(), sizeof(hsi_config.password) - 1);
    snprintf(hsi_config.dhcp_addr_pool, sizeof(hsi_config.dhcp_addr_pool), "%s-%s", dhcp_pool_start.c_str(), dhcp_pool_end.c_str());
    strncpy(hsi_config.dhcp_subnet, dhcp_subnet_mask.c_str(), sizeof(hsi_config.dhcp_subnet) - 1);
    strncpy(hsi_config.dhcp_gateway, dhcp_gateway.c_str(), sizeof(hsi_config.dhcp_gateway) - 1);

    // Write the config to etcd to trigger etcd watcher (only if etcd is initialized)
    if (etcd_client_is_initialized() && fastrg_ccb && fastrg_ccb->node_uuid) {
        std::string user_id_str = std::to_string(user_id);
        etcd_status_t s = etcd_client_put_hsi_config(fastrg_ccb->node_uuid, 
            user_id_str.c_str(), &hsi_config, ENABLE_STATUS_DISABLED, "fastrg-node-grpc");
        if (s != ETCD_SUCCESS) {
            std::string err = "Failed to write configuration to etcd for user " + user_id_str;
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
        cout << "Configuration synced to etcd for user " << user_id_str << endl;
    } else if (!etcd_client_is_initialized()) {
        cout << "Etcd not initialized, directly applying config for user " << user_id << endl;
        if (apply_hsi_config(fastrg_ccb, ccb_id, &hsi_config, FALSE) == ERROR) {
            std::string err = "Error! Failed to apply configuration for user " + std::to_string(user_id);
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
    } else {
        std::string err = "Error! fastrg_ccb or node_uuid is NULL";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    response->set_status("Configuration successful");

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::RemoveConfig(::grpc::ServerContext* context, const ::fastrgnodeservice::ConfigRequest* request, ::fastrgnodeservice::ConfigReply* response)
{
    uint16_t user_id = request->user_id(), ccb_id = request->user_id() - 1;

    cout << "RemoveConfig called" << endl;

    if (user_id > fastrg_ccb->user_count) {
        std::string err = "Error! User " + std::to_string(user_id) + " is not exist";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err);
    }

    // After removing local HSI config, delete the config from etcd (only if etcd is initialized)
    if (etcd_client_is_initialized() && fastrg_ccb && fastrg_ccb->node_uuid) {
        std::string user_id_str = std::to_string(user_id);
        etcd_status_t s = etcd_client_delete_hsi_config(fastrg_ccb->node_uuid, user_id_str.c_str(), NULL);
        if (s != ETCD_SUCCESS) {
            std::string err = "Failed to delete config from etcd for user " + user_id_str;
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
        cout << "Configuration removed from etcd for user " << user_id_str << endl;
    } else if (!etcd_client_is_initialized()) {
        cout << "Etcd not initialized, directly calling deletion for user " << user_id << endl;
        if (remove_hsi_config(fastrg_ccb, ccb_id) == ERROR) {
            std::string err = "Error! Failed to remove configuration for user " + std::to_string(user_id);
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
    } else {
        std::string err = "Error! fastrg_ccb or node_uuid is NULL";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    response->set_status("Configuration removal successful");

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::SetSubscriberCount(::grpc::ServerContext* context, const ::fastrgnodeservice::SetSubscriberCountRequest* request, ::fastrgnodeservice::SetSubscriberCountReply* response)
{
    int subscriber_count = request->subscriber_count();

    cout << "SetSubscriberCount called" << endl;

    if (subscriber_count == 0 || subscriber_count > MAX_USER_COUNT) {
        std::string err = "Error! Subscriber count " + std::to_string(subscriber_count) + " is invalid";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err);
    }

    if (etcd_client_is_initialized() && fastrg_ccb && fastrg_ccb->node_uuid) {
        std::string subscriber_count_str = std::to_string(subscriber_count);
        etcd_status_t s = etcd_client_put_subscriber_count(fastrg_ccb->node_uuid, subscriber_count_str.c_str(), "fastrg-node-grpc");
        if (s != ETCD_SUCCESS) {
            std::string err = "Failed to write subscriber count to etcd: " + subscriber_count_str;
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
        cout << "Subscriber count synced to etcd: " << subscriber_count << endl;
    } else if (!etcd_client_is_initialized()) {
        cout << "Etcd not initialized, directly setting subscriber count to " << subscriber_count << endl;
        // mock a listend etcd event
        user_count_config_t config = {
            .user_count = subscriber_count
        };
        if (user_count_changed_callback("", &config, HSI_ACTION_UPDATE, 0, fastrg_ccb) == ERROR) {
            std::string err = "Error! Failed to set subscriber count to " + std::to_string(subscriber_count);
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
    } else {
        std::string err = "Error! fastrg_ccb or node_uuid is NULL";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }
    cout << "Subscriber count set to " << subscriber_count << endl;

    response->set_status("Subscriber count set successfully");

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::ConnectHsi(::grpc::ServerContext* context, const ::fastrgnodeservice::HsiRequest* request, ::fastrgnodeservice::HsiReply* response)
{
    uint16_t user_id = request->user_id(), ccb_id = request->user_id() - 1;

    if (user_id > fastrg_ccb->user_count) {
        std::string err = "Error! User " + std::to_string(user_id) + " is not exist";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err);
    }

    cout << "ConnectHsi called" << endl;
    if (user_id == 0) {
        for(int i=0; i<fastrg_ccb->user_count; i++) {
            ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, i);
            if (rte_atomic16_read(&ppp_ccb->vlan_id) == 0) {
                cout << "User " << i + 1 << " has no configuration, skip connecting" << endl;
                continue;
            }
            if (rte_atomic16_read(&ppp_ccb->ppp_bool) == 1) {
                cout << "User " << i + 1 << " is already connectiing/connected, skip connecting" << endl;
                continue;
            }
            if (fastrg_gen_northbound_event(EV_NORTHBOUND_PPPoE, PPPoE_CMD_ENABLE, i) == ERROR) {
                cout << "Failed to generate PPPoE enable event for user " << i + 1 << endl;
                continue;
            }
        }
    } else {
        ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
        if (rte_atomic16_read(&ppp_ccb->vlan_id) == 0) {
            cout << "User " << ccb_id + 1 << " has no configuration, skip connecting" << endl;
            std::string err = "Error! User " + std::to_string(user_id) + " has no configuration";
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, err);
        }
        if (rte_atomic16_read(&ppp_ccb->ppp_bool) == 1) {
            cout << "User " << ccb_id + 1 << " is already connectiing/connected, skip connecting" << endl;
            std::string err = "Error! User " + std::to_string(user_id) + " is already connected/connecting by other client";
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, err);
        }
        if (fastrg_gen_northbound_event(EV_NORTHBOUND_PPPoE, PPPoE_CMD_ENABLE, ccb_id) == ERROR) {
            cout << "Failed to generate PPPoE enable event for user " << ccb_id + 1 << endl;
            std::string err = "Failed to generate PPPoE enable event for user " + std::to_string(user_id);
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, err);
        }
    }

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::DisconnectHsi(::grpc::ServerContext* context, const ::fastrgnodeservice::HsiRequest* request, ::fastrgnodeservice::HsiReply* response)
{
    uint16_t user_id = request->user_id(), ccb_id = request->user_id() - 1;
    bool force = request->force();

    if (user_id > fastrg_ccb->user_count) {
        std::string err = "Error! User " + std::to_string(user_id) + " is not exist";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err);
    }

    cout << "DisconnectHsi called" << endl;
    if (user_id == 0) {
        for(int i=0; i<fastrg_ccb->user_count; i++) {
            if (force) {
                if (fastrg_gen_northbound_event(EV_NORTHBOUND_PPPoE, PPPoE_CMD_FORCE_DISABLE, i) == ERROR) {
                    cout << "Failed to generate PPPoE enable event for user " << i + 1 << endl;
                    std::string err = "Failed to generate PPPoE enable event for user " + std::to_string(i + 1);
                    cout << err << endl;
                    continue;
                }
            }
            ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, i);
            if (rte_atomic16_read(&ppp_ccb->vlan_id) == 0) {
                cout << "User " << i + 1 << " has no configuration, skip disconnecting" << endl;
                continue;
            }
            if (rte_atomic16_read(&ppp_ccb->ppp_bool) == 0) {
                cout << "User " << i + 1 << " is already disconnectiing/disconnected, skip disconnecting" << endl;
                continue;
            }
            if (fastrg_gen_northbound_event(EV_NORTHBOUND_PPPoE, PPPoE_CMD_DISABLE, i) == ERROR) {
                cout << "Failed to generate PPPoE disable event for user " << i + 1 << endl;
                continue;
            }
        }
    } else {
        if (force) {
            if (fastrg_gen_northbound_event(EV_NORTHBOUND_PPPoE, PPPoE_CMD_FORCE_DISABLE, ccb_id) == ERROR) {
                cout << "Failed to generate PPPoE enable event for user " << ccb_id + 1 << endl;
                std::string err = "Failed to generate PPPoE enable event for user " + std::to_string(ccb_id + 1);
                cout << err << endl;
                return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, err);
            } else {
                return grpc::Status::OK;
            }
        }
        ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
        if (rte_atomic16_read(&ppp_ccb->vlan_id) == 0) {
            cout << "User " << ccb_id + 1 << " has no configuration, skip disconnecting" << endl;
            std::string err = "Error! User " + std::to_string(user_id) + " has no configuration";
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, err);
        }
        if (rte_atomic16_read(&ppp_ccb->ppp_bool) == 0) {
            cout << "User " << ccb_id + 1 << " is already disconnectiing/disconnected, skip disconnecting" << endl;
            std::string err = "Error! User " + std::to_string(user_id) + " is already disconnected/disconnecting by other client";
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, err);
        }
        if (fastrg_gen_northbound_event(EV_NORTHBOUND_PPPoE, PPPoE_CMD_DISABLE, ccb_id) == ERROR) {
            cout << "Failed to generate PPPoE disable event for user " << ccb_id + 1 << endl;
            std::string err = "Failed to generate PPPoE disable event for user " + std::to_string(user_id);
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, err);
        }
    }

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::DhcpServerStart(::grpc::ServerContext* context, const ::fastrgnodeservice::DhcpServerRequest* request, ::fastrgnodeservice::DhcpServerReply* response)
{
    uint16_t user_id = request->user_id(), ccb_id = request->user_id() - 1;

    if (user_id > fastrg_ccb->user_count) {
        std::string err = "Error! User " + std::to_string(user_id) + " is not exist";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err);
    }

    cout << "DhcpServerStart called" << endl;
    if (user_id == 0) {
        std::string err;
        for(int i=0; i<fastrg_ccb->user_count; i++) {
            dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, i);
            if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 1) {
                cout << "User " << i + 1 << " DHCP server is already enabled, skip enabling" << endl;
                err += "User " + std::to_string(i + 1) + " DHCP server is already enabled\n";
                continue;
            }
            if (dhcp_ccb->dhcp_server_ip == 0) {
                cout << "User " << i + 1 << " DHCP server has not been configured, skip enabling" << endl;
                err += "User " + std::to_string(i + 1) + " DHCP server has not been configured\n";
                continue;
            }
            if (fastrg_gen_northbound_event(EV_NORTHBOUND_DHCP, DHCP_CMD_ENABLE, i) == ERROR) {
                cout << "Failed to generate DHCP enable event for user " << i + 1 << endl;
                err += "Failed to generate DHCP enable event for user " + std::to_string(i + 1) + "\n";
                continue;
            }
        }
        if (!err.empty())
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, err);
    } else {
        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
        if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 1) {
            cout << "User " << ccb_id + 1 << " DHCP server is already enabled, skip enabling" << endl;
            std::string err = "User " + std::to_string(ccb_id + 1) + " DHCP server is already enabled\n";
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, err);
        }
        if (dhcp_ccb->dhcp_server_ip == 0) {
            cout << "User " << ccb_id + 1 << " DHCP server has not been configured, skip enabling" << endl;
            std::string err = "User " + std::to_string(ccb_id + 1) + " DHCP server has not been configured\n";
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, err);
        }
        if (fastrg_gen_northbound_event(EV_NORTHBOUND_DHCP, DHCP_CMD_ENABLE, ccb_id) == ERROR) {
            cout << "Failed to generate DHCP enable event for user " << ccb_id + 1 << endl;
            std::string err = "Failed to generate DHCP enable event for user " + std::to_string(ccb_id + 1) + "\n";
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, err);
        }
    }

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::DhcpServerStop(::grpc::ServerContext* context, const ::fastrgnodeservice::DhcpServerRequest* request, ::fastrgnodeservice::DhcpServerReply* response)
{
    uint16_t user_id = request->user_id(), ccb_id = request->user_id() - 1;

    if (user_id > fastrg_ccb->user_count) {
        std::string err = "Error! User " + std::to_string(user_id) + " is not exist";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err);
    }

    cout << "DhcpServerStop called" << endl;
    if (user_id == 0) {
        std::string err;
        for(int i=0; i<fastrg_ccb->user_count; i++) {
            dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, i);
            if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 0) {
                cout << "User " << i + 1 << " DHCP server is already disabled, skip disabling" << endl;
                err += "User " + std::to_string(i + 1) + " DHCP server is already disabled\n";
                continue;
            }
            if (fastrg_gen_northbound_event(EV_NORTHBOUND_DHCP, DHCP_CMD_DISABLE, i) == ERROR) {
                cout << "Failed to generate DHCP disable event for user " << i + 1 << endl;
                err += "Failed to generate DHCP disable event for user " + std::to_string(i + 1) + "\n";
                continue;
            }
        }
        if (!err.empty())
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, err);
    } else {
        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
        if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 0) {
            cout << "User " << ccb_id + 1 << " DHCP server is already disabled, skip disabling" << endl;
            std::string err = "User " + std::to_string(ccb_id + 1) + " DHCP server is already disabled\n";
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, err);
        }
        if (fastrg_gen_northbound_event(EV_NORTHBOUND_DHCP, DHCP_CMD_DISABLE, ccb_id) == ERROR) {
            cout << "Failed to generate DHCP disable event for user " << ccb_id + 1 << endl;
            std::string err = "Failed to generate DHCP disable event for user " + std::to_string(ccb_id + 1) + "\n";
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, err);
        }
    }

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::SetSnatConfig(::grpc::ServerContext* context, const ::fastrgnodeservice::SnatConfigRequest* request, ::fastrgnodeservice::SnatConfigReply* response)
{
    cout << "SetSnatConfig called" << endl;

    U16 user_id = request->user_id();
    U16 ccb_id = user_id - 1;
    U16 eport = request->eport();
    U16 iport = request->iport();
    std::string dip = request->dip();

    if (etcd_client_is_initialized() && fastrg_ccb && fastrg_ccb->node_uuid) {
        std::string user_id_str = std::to_string(user_id);

        // Read current HSI config from etcd, update port-mapping entry, then write back
        hsi_config_full_t full_config = { 0 };
        etcd_status_t get_s = etcd_client_get_hsi_config(fastrg_ccb->node_uuid,
            user_id_str.c_str(), &full_config);
        if (get_s != ETCD_SUCCESS) {
            std::string err = "Failed to get HSI config from etcd for user " + user_id_str;
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }

        hsi_config_t *cfg = &full_config.config;

        // Update existing entry if same eport found, otherwise append new entry
        bool found = false;
        for(int i=0; cfg->port_mappings!=NULL && i<cfg->port_mapping_count; i++) {
            if (cfg->port_mappings[i].eport == eport) {
                strncpy(cfg->port_mappings[i].dip, dip.c_str(), sizeof(cfg->port_mappings[i].dip) - 1);
                cfg->port_mappings[i].dip[sizeof(cfg->port_mappings[i].dip) - 1] = '\0';
                cfg->port_mappings[i].dport = iport;
                found = true;
                break;
            }
        }
        if (!found) {
            int new_count = cfg->port_mapping_count + 1;
            port_mapping_t *new_mappings = (port_mapping_t *)realloc(cfg->port_mappings,
                new_count * sizeof(port_mapping_t));
            if (new_mappings == NULL) {
                hsi_config_free_port_mappings(cfg);
                std::string err = "Out of memory adding port mapping for user " + user_id_str;
                cout << err << endl;
                return grpc::Status(grpc::StatusCode::INTERNAL, err);
            }
            cfg->port_mappings = new_mappings;
            port_mapping_t *pm = &cfg->port_mappings[cfg->port_mapping_count];
            pm->eport = eport;
            strncpy(pm->dip, dip.c_str(), sizeof(pm->dip) - 1);
            pm->dip[sizeof(pm->dip) - 1] = '\0';
            pm->dport = iport;
            cfg->port_mapping_count = new_count;
        }

        etcd_status_t put_s = etcd_client_put_hsi_config(fastrg_ccb->node_uuid,
            user_id_str.c_str(), cfg, full_config.enable_status, "fastrg-node-grpc");
        hsi_config_free_port_mappings(cfg);

        if (put_s != ETCD_SUCCESS) {
            std::string err = "Failed to update HSI config in etcd for user " + user_id_str;
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
        cout << "SNAT port forward synced to etcd for user " << user_id_str << endl;
    } else if (!etcd_client_is_initialized()) {
        cout << "Etcd not initialized, directly setting SNAT port forward for user " << user_id << endl;
        if (set_snat_port_fwd(fastrg_ccb, ccb_id, eport, dip.c_str(), iport) == ERROR) {
            std::string err = "Error! Failed to set SNAT port forward for user " + std::to_string(user_id) +
                " eport=" + std::to_string(eport) + " dip=" + dip + " iport=" + std::to_string(iport);
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
    } else {
        std::string err = "Error! fastrg_ccb or node_uuid is NULL";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    response->set_status("SNAT port forward set successfully");

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::RemoveSnatConfig(::grpc::ServerContext* context, const ::fastrgnodeservice::SnatConfigRequest* request, ::fastrgnodeservice::SnatConfigReply* response)
{
    cout << "RemoveSnatConfig called" << endl;

    U16 user_id = request->user_id();
    U16 ccb_id = user_id - 1;
    U16 eport = request->eport();

    if (etcd_client_is_initialized() && fastrg_ccb && fastrg_ccb->node_uuid) {
        std::string user_id_str = std::to_string(user_id);

        // Read current HSI config from etcd, remove the matching eport entry, then write back
        hsi_config_full_t full_config = { 0 };
        etcd_status_t get_s = etcd_client_get_hsi_config(fastrg_ccb->node_uuid,
            user_id_str.c_str(), &full_config);
        if (get_s != ETCD_SUCCESS) {
            std::string err = "Failed to get HSI config from etcd for user " + user_id_str;
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }

        hsi_config_t *cfg = &full_config.config;

        // Find and remove the entry with matching eport (shift remaining entries left)
        bool found = false;
        for(int i=0; cfg->port_mappings != NULL && i<cfg->port_mapping_count; i++) {
            if (cfg->port_mappings[i].eport == eport) {
                for(int j=i; j<cfg->port_mapping_count-1; j++)
                    cfg->port_mappings[j] = cfg->port_mappings[j + 1];
                cfg->port_mapping_count--;
                found = true;
                break;
            }
        }
        if (!found) {
            hsi_config_free_port_mappings(cfg);
            std::string err = "SNAT port forward eport=" + std::to_string(eport) +
                " not found for user " + user_id_str;
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::NOT_FOUND, err);
        }

        etcd_status_t put_s = etcd_client_put_hsi_config(fastrg_ccb->node_uuid,
            user_id_str.c_str(), cfg, full_config.enable_status, "fastrg-node-grpc");
        hsi_config_free_port_mappings(cfg);

        if (put_s != ETCD_SUCCESS) {
            std::string err = "Failed to update HSI config in etcd for user " + user_id_str;
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
        cout << "SNAT port forward removed from etcd for user " << user_id_str << endl;
    } else if (!etcd_client_is_initialized()) {
        cout << "Etcd not initialized, directly removing SNAT port forward for user " << user_id << endl;
        if (remove_snat_port_fwd(fastrg_ccb, ccb_id, eport) == ERROR) {
            std::string err = "Error! Failed to remove SNAT port forward for user " + std::to_string(user_id) +
                " eport=" + std::to_string(eport);
            cout << err << endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
    } else {
        std::string err = "Error! fastrg_ccb or node_uuid is NULL";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    response->set_status("SNAT port forward removed successfully");

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::GetPortFwdInfo(::grpc::ServerContext* context, const ::fastrgnodeservice::PortFwdInfoRequest* request, ::fastrgnodeservice::PortFwdInfoReply* response)
{
    cout << "GetPortFwdInfo called" << endl;

    U16 user_id = request->user_id();
    U16 ccb_id = user_id - 1;

    if (user_id == 0 || user_id > fastrg_ccb->user_count) {
        std::string err = "Error! User " + std::to_string(user_id) + " does not exist";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err);
    }

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    response->set_user_id(user_id);

    for(int eport=0; eport<PORT_FWD_TABLE_SIZE; eport++) {
        if (rte_atomic16_read(&ppp_ccb->port_fwd_table[eport].is_active) == 1) {
            PortFwdEntry *entry = response->add_entries();
            entry->set_eport(eport);

            U32 dip = ppp_ccb->port_fwd_table[eport].dip;
            std::string dip_str = std::to_string((dip) & 0xFF) + "." +
                std::to_string((dip >> 8) & 0xFF) + "." +
                std::to_string((dip >> 16) & 0xFF) + "." +
                std::to_string((dip >> 24) & 0xFF);
            entry->set_dip(dip_str);
            entry->set_iport(ppp_ccb->port_fwd_table[eport].iport);
            entry->set_hit_count(rte_atomic64_read(&ppp_ccb->port_fwd_table[eport].hit_count));
        }
    }

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::GetArpTable(::grpc::ServerContext* context, const ::fastrgnodeservice::ArpTableRequest* request, ::fastrgnodeservice::ArpTableReply* response)
{
    cout << "GetArpTable called" << endl;

    U16 user_id = request->user_id();
    U16 ccb_id = user_id - 1;
    U32 max_count = request->max_count();
    if (max_count == 0)
        max_count = 100;

    if (user_id == 0 || user_id > fastrg_ccb->user_count) {
        std::string err = "Error! User " + std::to_string(user_id) + " does not exist";
        cout << err << endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, err);
    }

    ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, ccb_id);
    response->set_user_id(user_id);

    if (ppp_ccb->mac_table == NULL) {
        response->set_total_count(0);
        return grpc::Status::OK;
    }

    U32 total_count = 0;
    U32 returned = 0;

    for(U32 idx=0; idx<MAC_TABLE_SIZE; idx++) {
        if (rte_atomic16_read(&ppp_ccb->mac_table[idx].valid) != 0) {
            total_count++;
            if (returned < max_count) {
                /* Reconstruct IP from index:
                 * idx = B * 255*255 + C * 255 + D
                 * First octet is the network prefix (e.g. 10 for 10.0.0.0/8)
                 */
                U32 d = idx % MAC_TABLE_DIM;
                U32 c = (idx / MAC_TABLE_DIM) % MAC_TABLE_DIM;
                U32 b = idx / (MAC_TABLE_DIM * MAC_TABLE_DIM);

                /* Use the DHCP server IP's first octet as the network prefix */
                dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
                U32 net_ip = rte_be_to_cpu_32(dhcp_ccb->dhcp_server_ip);
                U32 first_octet = (net_ip >> 24) & 0xFF;

                std::string ip_str = std::to_string(first_octet) + "." +
                    std::to_string(b) + "." +
                    std::to_string(c) + "." +
                    std::to_string(d);

                struct rte_ether_addr *mac = &ppp_ccb->mac_table[idx].mac;
                char mac_buf[18];
                snprintf(mac_buf, sizeof(mac_buf),
                    "%02x:%02x:%02x:%02x:%02x:%02x",
                    mac->addr_bytes[0], mac->addr_bytes[1],
                    mac->addr_bytes[2], mac->addr_bytes[3],
                    mac->addr_bytes[4], mac->addr_bytes[5]);

                ArpTableEntry *entry = response->add_entries();
                entry->set_entry_id(idx);
                entry->set_ip(ip_str);
                entry->set_mac(std::string(mac_buf));
                returned++;
            }
        }
    }

    response->set_total_count(total_count);
    return grpc::Status::OK;
}

int getNicInfo(NicDriverInfo *nic_info, uint8_t port_id)
{
    struct rte_eth_dev_info dev_info = {0};
    if (rte_eth_dev_info_get(port_id, &dev_info) != 0) {
		std::string err = "get device info failed";
		return -1;
	}
    nic_info->set_driver_name(std::string(dev_info.driver_name));
    char buf[RTE_ETH_NAME_MAX_LEN];
    if (rte_eth_dev_get_name_by_port(port_id, buf) != 0) {
        std::string err = "get device pci addr failed";
        return -1;
    }
    nic_info->set_pci_addr(std::string(buf));

    return 0;
}

int getNicStats(Statistics *stats, uint8_t port_id, FastRG_t *fastrg_ccb)
{
    struct rte_eth_stats eth_stats = {0};
    if (rte_eth_stats_get(port_id, &eth_stats) != 0) {
        std::string err = "get device stats failed";
        return -1;
    }
    stats->set_rx_packets(eth_stats.ipackets);
    stats->set_tx_packets(eth_stats.opackets);
    stats->set_rx_bytes(eth_stats.ibytes);
    stats->set_tx_bytes(eth_stats.obytes);
    stats->set_rx_errors(eth_stats.ierrors);
    stats->set_tx_errors(eth_stats.oerrors);
    stats->set_rx_dropped(eth_stats.imissed);

    // per user stats - with RCU protection
    unsigned int lcore_id = 0;
    // gRPC usually runs on non-DPDK lcore
    if (unlikely(rte_lcore_id() != LCORE_ID_ANY))
        lcore_id = rte_lcore_id();
    
    rte_rcu_qsbr_thread_online(fastrg_ccb->per_subscriber_stats_rcu, lcore_id);
    struct per_ccb_stats *port_stats = __atomic_load_n(&fastrg_ccb->per_subscriber_stats[port_id], __ATOMIC_ACQUIRE);
    
    if (likely(port_stats != NULL)) {
        for(int i=0; i<fastrg_ccb->user_count; i++) {
            PerUserStatistics* per_user_stats = stats->add_per_user_stats();
            per_user_stats->set_user_id(i + 1);
            per_user_stats->set_rx_packets(rte_atomic64_read(&port_stats[i].rx_packets));
            per_user_stats->set_tx_packets(rte_atomic64_read(&port_stats[i].tx_packets));
            per_user_stats->set_rx_bytes(rte_atomic64_read(&port_stats[i].rx_bytes));
            per_user_stats->set_tx_bytes(rte_atomic64_read(&port_stats[i].tx_bytes));
            per_user_stats->set_dropped_bytes(rte_atomic64_read(&port_stats[i].dropped_bytes));
            per_user_stats->set_dropped_packets(rte_atomic64_read(&port_stats[i].dropped_packets));
        }
        PerUserStatistics* per_user_stats = stats->add_per_user_stats();
        per_user_stats->set_user_id(0); // set unknown user to 0
        per_user_stats->set_rx_packets(rte_atomic64_read(&port_stats[fastrg_ccb->user_count].rx_packets));
        per_user_stats->set_tx_packets(rte_atomic64_read(&port_stats[fastrg_ccb->user_count].tx_packets));
        per_user_stats->set_rx_bytes(rte_atomic64_read(&port_stats[fastrg_ccb->user_count].rx_bytes));
        per_user_stats->set_tx_bytes(rte_atomic64_read(&port_stats[fastrg_ccb->user_count].tx_bytes));
        per_user_stats->set_dropped_bytes(rte_atomic64_read(&port_stats[fastrg_ccb->user_count].dropped_bytes));
        per_user_stats->set_dropped_packets(rte_atomic64_read(&port_stats[fastrg_ccb->user_count].dropped_packets));
    }
    rte_rcu_qsbr_quiescent(fastrg_ccb->per_subscriber_stats_rcu, lcore_id);
    rte_rcu_qsbr_thread_offline(fastrg_ccb->per_subscriber_stats_rcu, lcore_id);

    return 0;
}

int getNicXStats(NicXStats *nic_xstats, uint8_t port_id)
{
    nic_xstats->set_port_id(port_id);

    int eth_stats_len = rte_eth_xstats_get(port_id, NULL, 0);
    if (eth_stats_len < 0) {
        std::string err = "get xstats length failed";
        return -1;
    }
    struct rte_eth_xstat *xstats = fastrg_calloc(struct rte_eth_xstat, 
        eth_stats_len, sizeof(struct rte_eth_xstat), 0);
    if (xstats == NULL) {
        std::string err = "calloc xstats failed";
        return -1;
    }
    int ret = rte_eth_xstats_get(port_id, xstats, eth_stats_len);
    if (ret < 0 || ret > eth_stats_len) {
        std::string err = "get xstats failed";
        fastrg_mfree(xstats);
        return -1;
    }
    rte_eth_xstat_name *xstats_names = fastrg_calloc(struct rte_eth_xstat_name, 
        eth_stats_len, sizeof(struct rte_eth_xstat_name), 0);
    if (xstats_names == NULL) {
        std::string err = "calloc xstats names failed";
        fastrg_mfree(xstats);
        return -1;
    }
    ret = rte_eth_xstats_get_names(port_id, xstats_names, eth_stats_len);
    if (ret < 0 || ret > eth_stats_len) {
        std::string err = "get xstats names failed";
        fastrg_mfree(xstats);
        fastrg_mfree(xstats_names);
        return -1;
    }

    for(int i=0; i<eth_stats_len; i++) {
        XStat *xstat = nic_xstats->add_xstats();
        xstat->set_name(std::string(xstats_names[i].name));
        xstat->set_value(xstats[i].value);
    }

    fastrg_mfree(xstats);
    fastrg_mfree(xstats_names);
    return 0;
}

grpc::Status FastRGNodeServiceImpl::GetFastrgSystemInfo(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::FastrgSystemInfo* response)
{
    FastrgBaseInfo* base_info = response->mutable_base_info();
    base_info->set_fastrg_version(std::string(fastrg_ccb->version));
    base_info->set_build_date(std::string(fastrg_ccb->build_date));
    base_info->set_dpdk_version(std::string(rte_version()));
    base_info->set_dpdk_eal_args(std::string(fastrg_ccb->eal_args));
    base_info->set_num_users(fastrg_ccb->user_count);

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::GetFastrgSystemStats(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::FastrgSystemStatsInfo* response)
{
    uint8_t lan_port_id = 0, wan_port_id = 1;

    NicDriverInfo *lan_nic_info = response->add_nics();
    if (getNicInfo(lan_nic_info, lan_port_id) != 0) {
        std::string err = "get lan device info failed";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }
    lan_nic_info->set_mac_addr(std::string(
        reinterpret_cast<const char*>(fastrg_ccb->nic_info.hsi_lan_mac.addr_bytes), 6));

    NicDriverInfo *wan_nic_info = response->add_nics();
    if (getNicInfo(wan_nic_info, wan_port_id) != 0) {
        std::string err = "get wan device info failed";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }
    wan_nic_info->set_mac_addr(std::string(
        reinterpret_cast<const char*>(fastrg_ccb->nic_info.hsi_wan_src_mac.addr_bytes), 6));

    Statistics *lan_stats = response->add_stats();
    if (getNicStats(lan_stats, lan_port_id, fastrg_ccb) != 0) {
        std::string err = "get lan device stats failed";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }
    Statistics *wan_stats = response->add_stats();
    if (getNicStats(wan_stats, wan_port_id, fastrg_ccb) != 0) {
        std::string err = "get wan device stats failed";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::GetFastrgSystemXStats(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::FastrgSystemXStatsInfo* response)
{
    uint8_t lan_port_id = 0, wan_port_id = 1;

    NicXStats *lan_xstats = response->add_nic_xstats();
    if (getNicXStats(lan_xstats, lan_port_id) != 0) {
        std::string err = "get lan device xstats failed";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    NicXStats *wan_xstats = response->add_nic_xstats();
    if (getNicXStats(wan_xstats, wan_port_id) != 0) {
        std::string err = "get wan device xstats failed";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::GetFastrgHsiInfo(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::FastrgHsiInfo* response) 
{
    for(int i=0; i<fastrg_ccb->user_count; i++) {
        HsiInfo *hsi_info = response->add_hsi_infos();
        ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, i);
        hsi_info->set_user_id(i + 1);
        hsi_info->set_vlan_id(rte_atomic16_read(&ppp_ccb->vlan_id));
        switch (ppp_ccb->phase) {
            case END_PHASE:
                hsi_info->set_status("End phase");
                break;
            case PPPOE_PHASE:
                hsi_info->set_status("PPPoE phase");
                break;
            case LCP_PHASE:
                hsi_info->set_status("LCP phase");
                break;
            case AUTH_PHASE:
                hsi_info->set_status("Auth phase");
                break;
            case IPCP_PHASE:
                hsi_info->set_status("IPCP phase");
                break;
            case DATA_PHASE:
                hsi_info->set_status("Data phase");
                hsi_info->set_account(std::string(reinterpret_cast<const char*>(ppp_ccb->ppp_user_acc)));
                hsi_info->set_password(std::string(reinterpret_cast<const char*>(ppp_ccb->ppp_passwd)));
                hsi_info->set_session_id(rte_be_to_cpu_16(ppp_ccb->session_id));
                hsi_info->set_ip_addr(std::to_string(*(((U8 *)&(ppp_ccb->hsi_ipv4)))) + "." +
                                     std::to_string(*(((U8 *)&(ppp_ccb->hsi_ipv4))+1)) + "." +
                                     std::to_string(*(((U8 *)&(ppp_ccb->hsi_ipv4))+2)) + "." +
                                     std::to_string(*(((U8 *)&(ppp_ccb->hsi_ipv4))+3)));
                hsi_info->set_gateway(std::to_string(*(((U8 *)&(ppp_ccb->hsi_ipv4_gw)))) + "." +
                                     std::to_string(*(((U8 *)&(ppp_ccb->hsi_ipv4_gw))+1)) + "." +
                                     std::to_string(*(((U8 *)&(ppp_ccb->hsi_ipv4_gw))+2)) + "." +
                                     std::to_string(*(((U8 *)&(ppp_ccb->hsi_ipv4_gw))+3)));
                hsi_info->add_dnss(std::to_string(*(((U8 *)&(ppp_ccb->hsi_primary_dns)))) + "." +
                                   std::to_string(*(((U8 *)&(ppp_ccb->hsi_primary_dns))+1)) + "." +
                                   std::to_string(*(((U8 *)&(ppp_ccb->hsi_primary_dns))+2)) + "." +
                                   std::to_string(*(((U8 *)&(ppp_ccb->hsi_primary_dns))+3)));
                hsi_info->add_dnss(std::to_string(*(((U8 *)&(ppp_ccb->hsi_secondary_dns)))) + "." +
                                   std::to_string(*(((U8 *)&(ppp_ccb->hsi_secondary_dns))+1)) + "." +
                                   std::to_string(*(((U8 *)&(ppp_ccb->hsi_secondary_dns))+2)) + "." +
                                   std::to_string(*(((U8 *)&(ppp_ccb->hsi_secondary_dns))+3)));
                break;
            case NOT_CONFIGURED:
                hsi_info->set_status("not configured");
                break;
            default:
                hsi_info->set_status("unknown status");
                break;
        }
        hsi_info->set_pppoes_rx_packets(rte_atomic64_read(&ppp_ccb->pppoes_rx_packets));
        hsi_info->set_pppoes_tx_packets(rte_atomic64_read(&ppp_ccb->pppoes_tx_packets));
        hsi_info->set_pppoes_rx_bytes(rte_atomic64_read(&ppp_ccb->pppoes_rx_bytes));
        hsi_info->set_pppoes_tx_bytes(rte_atomic64_read(&ppp_ccb->pppoes_tx_bytes));
    }

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::GetFastrgDhcpInfo(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::FastrgDhcpInfo* response) 
{
    for(int i=0; i<fastrg_ccb->user_count; i++) {
        DhcpInfo *dhcp_info = response->add_dhcp_infos();
        ppp_ccb_t *ppp_ccb = PPPD_GET_CCB(fastrg_ccb, i);
        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, i);
        if (rte_atomic16_read(&dhcp_ccb->dhcp_bool) == 1) {
            dhcp_info->set_user_id(i + 1);
            dhcp_info->set_status("DHCP server is on");

			for(U8 j=0; j<dhcp_ccb->per_lan_user_pool_len; j++) {
				if (dhcp_ccb->per_lan_user_pool[j]->ip_pool.used) {
                    dhcp_info->add_inuse_ips(std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[j]->ip_pool.ip_addr)))) + "." +
                        std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[j]->ip_pool.ip_addr))+1)) + "." +
                        std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[j]->ip_pool.ip_addr))+2)) + "." +
                        std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[j]->ip_pool.ip_addr))+3)));
				}
			}
		} else {
            dhcp_info->set_user_id(i + 1);
            dhcp_info->set_status("DHCP server is off");
        }

        if (dhcp_ccb->per_lan_user_pool[0]->ip_pool.ip_addr == 0 || 
                dhcp_ccb->per_lan_user_pool[dhcp_ccb->per_lan_user_pool_len - 1]->ip_pool.ip_addr == 0) {
            dhcp_info->set_ip_range("Not Configured");
        } else {
            dhcp_info->set_ip_range(std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[0]->ip_pool.ip_addr)))) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[0]->ip_pool.ip_addr))+1)) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[0]->ip_pool.ip_addr))+2)) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[0]->ip_pool.ip_addr))+3)) + " - " +
                std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[dhcp_ccb->per_lan_user_pool_len - 1]->ip_pool.ip_addr)))) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[dhcp_ccb->per_lan_user_pool_len - 1]->ip_pool.ip_addr))+1)) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[dhcp_ccb->per_lan_user_pool_len - 1]->ip_pool.ip_addr))+2)) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->per_lan_user_pool[dhcp_ccb->per_lan_user_pool_len - 1]->ip_pool.ip_addr))+3)));
        }

        if (dhcp_ccb->subnet_mask == 0) {
            dhcp_info->set_subnet_mask("Not Configured");
        } else {
            dhcp_info->set_subnet_mask(std::to_string(*(((U8 *)&(dhcp_ccb->subnet_mask)))) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->subnet_mask))+1)) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->subnet_mask))+2)) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->subnet_mask))+3)));
        }

        if (dhcp_ccb->dhcp_server_ip == 0) {
            dhcp_info->set_gateway("Not Configured");
        } else {
            dhcp_info->set_gateway(std::to_string(*(((U8 *)&(dhcp_ccb->dhcp_server_ip)))) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->dhcp_server_ip))+1)) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->dhcp_server_ip))+2)) + "." +
                std::to_string(*(((U8 *)&(dhcp_ccb->dhcp_server_ip))+3)));
        }
        
        if (ppp_ccb->hsi_primary_dns == 0) {
            dhcp_info->add_dnss("Not Configured");
        } else {
            dhcp_info->add_dnss(std::to_string(*(((U8 *)&(ppp_ccb->hsi_primary_dns)))) + "." +
                std::to_string(*(((U8 *)&(ppp_ccb->hsi_primary_dns))+1)) + "." +
                std::to_string(*(((U8 *)&(ppp_ccb->hsi_primary_dns))+2)) + "." +
                std::to_string(*(((U8 *)&(ppp_ccb->hsi_primary_dns))+3)));
            if (ppp_ccb->hsi_secondary_dns != 0) {
                dhcp_info->add_dnss(std::to_string(*(((U8 *)&(ppp_ccb->hsi_secondary_dns)))) + "." +
                    std::to_string(*(((U8 *)&(ppp_ccb->hsi_secondary_dns))+1)) + "." +
                    std::to_string(*(((U8 *)&(ppp_ccb->hsi_secondary_dns))+2)) + "." +
                    std::to_string(*(((U8 *)&(ppp_ccb->hsi_secondary_dns))+3)));
            }
        }
    }
    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::GetNodeStatus(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::fastrgnodeservice::NodeStatus* response) 
{
    cout << "GetNodeStatus called" << endl;

    // get os version
    std::string os_name;
    struct utsname buffer;
    if (uname(&buffer) == 0) {
        os_name = std::string(buffer.sysname) + " " + std::string(buffer.release);
    } else {
        std::string err = "Failed to get Linux kernel version";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }
    std::ifstream os_release("/etc/os-release");
    if (os_release.is_open()) {
        std::string line, name, version;
        while (std::getline(os_release, line)) {
            if (line.rfind("PRETTY_NAME=", 0) == 0) {
                std::string value = line.substr(12); // remove PRETTY_NAME="
                if (!value.empty() && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }
                os_name += " " + value;
                break;
            }
        }
        os_release.close();
    } else {
        std::string err = "Failed to open /etc/os-release";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }
    response->set_node_os_version(os_name);

    std::ifstream uptime_file("/proc/uptime");
    if (!uptime_file.is_open()) {
        std::string err = "Failed to get system uptime";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }
    double uptime_seconds;
    uptime_file >> uptime_seconds;
    uptime_file.close();
    response->set_node_uptime(static_cast<int64_t>(uptime_seconds));

    FILE* fp = popen("ip route show default 2>/dev/null", "r");
    std::string def_route;
    if (fp) {
        char buf[256];
        if (fgets(buf, sizeof(buf), fp)) {
            def_route = buf;
        }
        pclose(fp);
    } else {
        std::string err = "Failed to get default route";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    std::string iface;
    if (!def_route.empty()) {
        std::istringstream iss(def_route);
        std::string word;
        while (iss >> word) {
            if (word == "dev") {
                iss >> iface;
                break;
            }
        }
    } else {
        std::string err = "No default route found";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    if (!iface.empty()) {
        struct ifaddrs *ifaddr, *ifa;
        if (getifaddrs(&ifaddr) == 0) {
            for(ifa=ifaddr; ifa!=NULL; ifa=ifa->ifa_next) {
                if (ifa->ifa_addr == NULL) continue;
                if (iface == ifa->ifa_name && ifa->ifa_addr->sa_family == AF_INET) {
                    char ip[INET_ADDRSTRLEN];
                    void* addr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
                    inet_ntop(AF_INET, addr, ip, sizeof(ip));
                    
                    char netmask[INET_ADDRSTRLEN];
                    void* mask = &((struct sockaddr_in*)ifa->ifa_netmask)->sin_addr;
                    inet_ntop(AF_INET, mask, netmask, sizeof(netmask));

                    response->set_node_ip_info(std::string(ip) + "/" + std::string(netmask));
                }
            }
            freeifaddrs(ifaddr);
        }
    } else {
        std::string err = "No default route interface found.";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    response->set_healthy(true);

    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::AddDnsRecord(::grpc::ServerContext* context,
    const ::fastrgnodeservice::DnsRecordRequest* request,
    ::fastrgnodeservice::DnsRecordReply* response)
{
    cout << "AddDnsRecord called" << endl;

    U16 user_id = request->user_id();
    std::string domain = request->domain();
    std::string ip = request->ip();
    U32 ttl = request->ttl();
    if (ttl == 0) ttl = 3600;

    if (etcd_client_is_initialized() && fastrg_ccb && fastrg_ccb->node_uuid) {
        std::string user_id_str = std::to_string(user_id);
        dns_record_config_t record;
        memset(&record, 0, sizeof(record));
        strncpy(record.domain, domain.c_str(), sizeof(record.domain) - 1);
        strncpy(record.ip, ip.c_str(), sizeof(record.ip) - 1);
        record.ttl = ttl;

        etcd_status_t s = etcd_client_put_dns_record(fastrg_ccb->node_uuid,
            user_id_str.c_str(), &record);
        if (s != ETCD_SUCCESS) {
            std::string err = "Failed to write DNS record to etcd for user " + user_id_str;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
    } else {
        /* Etcd not available, apply directly */
        U16 ccb_id = user_id - 1;
        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
        if (!dhcp_ccb) {
            std::string err = "DNS proxy not initialized for user " + std::to_string(user_id);
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
        U32 ip_addr;
        if (inet_pton(AF_INET, ip.c_str(), &ip_addr) != 1) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid IP address");
        }
        dns_static_add(&dhcp_ccb->dns_state.static_table, domain.c_str(), ip_addr, ttl);
    }

    response->set_status("DNS record added successfully");
    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::RemoveDnsRecord(::grpc::ServerContext* context,
    const ::fastrgnodeservice::DnsRecordRequest* request,
    ::fastrgnodeservice::DnsRecordReply* response)
{
    cout << "RemoveDnsRecord called" << endl;

    U16 user_id = request->user_id();
    std::string domain = request->domain();

    if (etcd_client_is_initialized() && fastrg_ccb && fastrg_ccb->node_uuid) {
        std::string user_id_str = std::to_string(user_id);
        etcd_status_t s = etcd_client_delete_dns_record(fastrg_ccb->node_uuid,
            user_id_str.c_str(), domain.c_str());
        if (s != ETCD_SUCCESS) {
            std::string err = "Failed to delete DNS record from etcd for user " + user_id_str;
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
    } else {
        U16 ccb_id = user_id - 1;
        dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
        if (!dhcp_ccb) {
            std::string err = "DNS proxy not initialized for user " + std::to_string(user_id);
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
        dns_static_remove(&dhcp_ccb->dns_state.static_table, domain.c_str());
    }

    response->set_status("DNS record removed successfully");
    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::GetDnsCache(::grpc::ServerContext* context,
    const ::fastrgnodeservice::DnsCacheRequest* request,
    ::fastrgnodeservice::DnsCacheReply* response)
{
    cout << "GetDnsCache called" << endl;

    U16 user_id = request->user_id();
    U16 ccb_id = user_id - 1;
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
    if (!dhcp_ccb) {
        std::string err = "DNS proxy not initialized for user " + std::to_string(user_id);
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    response->set_user_id(user_id);
    dns_cache_t *cache = &dhcp_ccb->dns_state.cache;
    U32 total = 0;
    U64 now = rte_rdtsc();
    U64 cycles_in_sec = fastrg_get_cycles_in_sec();

    for(U32 i=0; i<DNS_CACHE_BUCKET_COUNT; i++) {
        dns_cache_entry_t *entry = cache->buckets[i];
        while (entry) {
            total++;
            DnsCacheEntry* e = response->add_entries();
            e->set_domain(entry->domain);
            e->set_qtype(entry->qtype);
            e->set_ttl(entry->ttl);
            U32 elapsed = (U32)((now - entry->insert_time) / cycles_in_sec);
            U32 remaining = (elapsed < entry->ttl) ? (entry->ttl - elapsed) : 0;
            e->set_remaining_ttl(remaining);
            e->set_hit_count(entry->hit_count);
            entry = entry->next;
        }
    }
    response->set_total_entries(total);
    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::GetDnsStaticRecords(::grpc::ServerContext* context,
    const ::fastrgnodeservice::DnsStaticRequest* request,
    ::fastrgnodeservice::DnsStaticReply* response)
{
    cout << "GetDnsStaticRecords called" << endl;

    U16 user_id = request->user_id();
    U16 ccb_id = user_id - 1;
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
    if (!dhcp_ccb) {
        std::string err = "DNS proxy not initialized for user " + std::to_string(user_id);
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    response->set_user_id(user_id);
    dns_static_table_t *tbl = &dhcp_ccb->dns_state.static_table;
    U32 total = 0;
    char ip_str[32];

    for(U32 i=0; i<DNS_STATIC_MAX_RECORDS; i++) {
        if (!tbl->records[i].active) continue;
        total++;
        DnsStaticEntry* e = response->add_entries();
        e->set_domain(tbl->records[i].domain);
        struct in_addr addr;
        addr.s_addr = tbl->records[i].ip_addr;
        inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
        e->set_ip(ip_str);
        e->set_ttl(tbl->records[i].ttl);
    }
    response->set_total_entries(total);
    return grpc::Status::OK;
}

grpc::Status FastRGNodeServiceImpl::FlushDnsCache(::grpc::ServerContext* context,
    const ::fastrgnodeservice::DnsCacheFlushRequest* request,
    ::fastrgnodeservice::DnsCacheFlushReply* response)
{
    cout << "FlushDnsCache called" << endl;

    U16 user_id = request->user_id();
    U16 ccb_id = user_id - 1;
    dhcp_ccb_t *dhcp_ccb = DHCPD_GET_CCB(fastrg_ccb, ccb_id);
    if (!dhcp_ccb) {
        std::string err = "DNS proxy not initialized for user " + std::to_string(user_id);
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
    }

    U32 flushed = dns_cache_flush(&dhcp_ccb->dns_state.cache);
    response->set_status("ok");
    response->set_flushed_count(flushed);
    return grpc::Status::OK;
}
