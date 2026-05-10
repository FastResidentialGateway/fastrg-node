#!/usr/bin/env python3
"""
FastRG Node gRPC client helper — uses grpcurl subprocess (no grpcio/pb2 required).

Usage:
    fastrg_grpc_client.py --node <host:port> <command> [args...]

Commands:
    get_hsi_info                                            - GetFastrgHsiInfo  → JSON
    get_dhcp_info                                           - GetFastrgDhcpInfo → JSON
    get_system_info                                         - GetFastrgSystemInfo → JSON
    get_user_drop_count <user_id> [port_idx]               - GetFastrgSystemStats, WAN dropped_packets for user
    get_port_fwd_info <user_id>                             - GetPortFwdInfo    → JSON
    get_dns_static <user_id>                                - GetDnsStaticRecords → JSON
    apply_config <uid> <vlan> <acct> <pw> <start> <end> <subnet> <gw>
                                                            - ApplyConfig → JSON
    remove_config <user_id>                                 - RemoveConfig → JSON
    connect_hsi <user_id>                                   - ConnectHsi → JSON
    disconnect_hsi <user_id>                                - DisconnectHsi → JSON
    start_dhcp_server <user_id>                             - DhcpServerStart → JSON
    stop_dhcp_server <user_id>                              - DhcpServerStop → JSON
    add_dns_record <user_id> <domain> <ip> <ttl>            - AddDnsRecord → JSON
    remove_dns_record <user_id> <domain>                    - RemoveDnsRecord → JSON
    set_subscriber_count <count>                            - SetSubscriberCount → JSON

Requirements:
    - python3 (stdlib only)
    - grpcurl binary: looked up in this script's directory first, then PATH
    - fastrg_node.proto: must be in the same directory as this script
"""

import sys
import os
import json
import re
import subprocess
import argparse

_DIR = os.path.dirname(os.path.abspath(__file__))
TIMEOUT_SEC = 10

# ---------------------------------------------------------------------------
# grpcurl helpers
# ---------------------------------------------------------------------------

def _find_grpcurl():
    """Return path to grpcurl binary or raise FileNotFoundError."""
    for candidate in [
        os.path.join(_DIR, 'grpcurl'),
        '/usr/bin/grpcurl',
        '/usr/local/bin/grpcurl',
    ]:
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    import shutil
    found = shutil.which('grpcurl')
    if found:
        return found
    raise FileNotFoundError(
        "grpcurl binary not found. Place it alongside this script or install it in PATH."
    )


def _camel_to_snake(name):
    """Convert camelCase / PascalCase key to snake_case."""
    s = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2', name)
    return re.sub(r'([a-z\d])([A-Z])', r'\1_\2', s).lower()


def _convert_keys(obj):
    """Recursively convert all dict keys from camelCase to snake_case."""
    if isinstance(obj, dict):
        return {_camel_to_snake(k): _convert_keys(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_convert_keys(i) for i in obj]
    return obj


def _grpcurl(node_addr, method, data=None):
    """Call grpcurl and return the response as a snake_case-keyed dict."""
    proto_file = os.path.join(_DIR, 'fastrg_node.proto')
    if not os.path.exists(proto_file):
        raise FileNotFoundError(f"proto not found: {proto_file}")

    grpcurl_bin = _find_grpcurl()

    cmd = [
        grpcurl_bin,
        '-plaintext',
        '-proto', 'fastrg_node.proto',
        '-import-path', _DIR,
        '-emit-defaults',
        '-connect-timeout', str(TIMEOUT_SEC),
        '-max-time', str(TIMEOUT_SEC),
    ]
    if data is not None:
        cmd += ['-d', json.dumps(data)]
    cmd += [node_addr, f'fastrgnodeservice.FastrgService/{method}']

    result = subprocess.run(
        cmd, capture_output=True, text=True, timeout=TIMEOUT_SEC + 5
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip())

    raw = json.loads(result.stdout) if result.stdout.strip() else {}
    return _convert_keys(raw)


# ---------------------------------------------------------------------------
# Command implementations
# ---------------------------------------------------------------------------

def get_hsi_info(node_addr):
    resp = _grpcurl(node_addr, 'GetFastrgHsiInfo')
    return {"hsi_infos": resp.get('hsi_infos', [])}


def get_dhcp_info(node_addr):
    resp = _grpcurl(node_addr, 'GetFastrgDhcpInfo')
    return {"dhcp_infos": resp.get('dhcp_infos', [])}


def get_system_info(node_addr):
    resp = _grpcurl(node_addr, 'GetFastrgSystemInfo')
    b = resp.get('base_info', {})
    return {
        "fastrg_version": b.get('fastrg_version', ''),
        "build_date":     b.get('build_date', ''),
        "uptime":         b.get('uptime', 0),
        "dpdk_version":   b.get('dpdk_version', ''),
        "num_users":      b.get('num_users', 0),
    }


def get_port_fwd_info(node_addr, user_id):
    resp = _grpcurl(node_addr, 'GetPortFwdInfo', {'user_id': user_id})
    return {
        "user_id": resp.get('user_id', user_id),
        "entries": resp.get('entries', []),
    }


def get_dns_static(node_addr, user_id):
    resp = _grpcurl(node_addr, 'GetDnsStaticRecords', {'user_id': user_id})
    return {
        "user_id":       resp.get('user_id', user_id),
        "total_entries": resp.get('total_entries', 0),
        "entries":       resp.get('entries', []),
    }


def apply_config(node_addr, user_id, vlan_id, pppoe_account, pppoe_password,
                 dhcp_pool_start, dhcp_pool_end, dhcp_subnet_mask, dhcp_gateway):
    resp = _grpcurl(node_addr, 'ApplyConfig', {
        'user_id':          int(user_id),
        'vlan_id':          int(vlan_id),
        'pppoe_account':    pppoe_account,
        'pppoe_password':   pppoe_password,
        'dhcp_pool_start':  dhcp_pool_start,
        'dhcp_pool_end':    dhcp_pool_end,
        'dhcp_subnet_mask': dhcp_subnet_mask,
        'dhcp_gateway':     dhcp_gateway,
    })
    return {"status": resp.get("status", "")}


def remove_config(node_addr, user_id):
    resp = _grpcurl(node_addr, 'RemoveConfig', {'user_id': int(user_id)})
    return {"status": resp.get("status", "")}


def connect_hsi(node_addr, user_id):
    resp = _grpcurl(node_addr, 'ConnectHsi', {'user_id': int(user_id)})
    return {"status": resp.get("status", "")}


def disconnect_hsi(node_addr, user_id):
    resp = _grpcurl(node_addr, 'DisconnectHsi', {'user_id': int(user_id)})
    return {"status": resp.get("status", "")}


def start_dhcp_server(node_addr, user_id):
    resp = _grpcurl(node_addr, 'DhcpServerStart', {'user_id': int(user_id)})
    return {"status": resp.get("status", "")}


def stop_dhcp_server(node_addr, user_id):
    resp = _grpcurl(node_addr, 'DhcpServerStop', {'user_id': int(user_id)})
    return {"status": resp.get("status", "")}


def add_dns_record(node_addr, user_id, domain, ip, ttl):
    resp = _grpcurl(node_addr, 'AddDnsRecord', {
        'user_id': int(user_id),
        'domain':  domain,
        'ip':      ip,
        'ttl':     int(ttl),
    })
    return {"status": resp.get("status", "")}


def remove_dns_record(node_addr, user_id, domain):
    resp = _grpcurl(node_addr, 'RemoveDnsRecord', {
        'user_id': int(user_id),
        'domain':  domain,
    })
    return {"status": resp.get("status", "")}


def set_subscriber_count(node_addr, subscriber_count):
    resp = _grpcurl(node_addr, 'SetSubscriberCount', {'subscriber_count': int(subscriber_count)})
    return {"status": resp.get("status", "")}


def get_user_drop_count(node_addr, user_id, port_idx=1):
    """Return dropped_packets for user_id on port_idx (1=WAN_PORT, 0=LAN_PORT)."""
    resp = _grpcurl(node_addr, 'GetFastrgSystemStats')
    stats_list = resp.get('stats', [])
    if port_idx >= len(stats_list):
        return {"dropped_packets": 0}
    for u in stats_list[port_idx].get('per_user_stats', []):
        if int(u.get('user_id', -1)) == int(user_id):
            return {"dropped_packets": int(u.get('dropped_packets', 0))}
    return {"dropped_packets": 0}


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="FastRG gRPC CLI helper (grpcurl backend)")
    parser.add_argument("--node", required=True, help="host:port of FastRG gRPC server")
    parser.add_argument("command", help="Command to run")
    parser.add_argument("args", nargs="*", help="Command arguments")
    opts = parser.parse_args()

    try:
        if opts.command == "get_hsi_info":
            result = get_hsi_info(opts.node)
        elif opts.command == "get_dhcp_info":
            result = get_dhcp_info(opts.node)
        elif opts.command == "get_system_info":
            result = get_system_info(opts.node)
        elif opts.command == "get_port_fwd_info":
            if not opts.args:
                print(json.dumps({"error": "get_port_fwd_info requires <user_id>"}),
                      file=sys.stderr)
                sys.exit(1)
            result = get_port_fwd_info(opts.node, int(opts.args[0]))
        elif opts.command == "get_dns_static":
            if not opts.args:
                print(json.dumps({"error": "get_dns_static requires <user_id>"}),
                      file=sys.stderr)
                sys.exit(1)
            result = get_dns_static(opts.node, int(opts.args[0]))
        elif opts.command == "apply_config":
            if len(opts.args) < 8:
                print(json.dumps({"error": "apply_config requires <user_id> <vlan_id> <account> <password> "
                                           "<pool_start> <pool_end> <subnet_mask> <gateway>"}),
                      file=sys.stderr)
                sys.exit(1)
            result = apply_config(opts.node, opts.args[0], opts.args[1], opts.args[2], opts.args[3],
                                  opts.args[4], opts.args[5], opts.args[6], opts.args[7])
        elif opts.command == "remove_config":
            if not opts.args:
                print(json.dumps({"error": "remove_config requires <user_id>"}), file=sys.stderr)
                sys.exit(1)
            result = remove_config(opts.node, opts.args[0])
        elif opts.command == "connect_hsi":
            if not opts.args:
                print(json.dumps({"error": "connect_hsi requires <user_id>"}), file=sys.stderr)
                sys.exit(1)
            result = connect_hsi(opts.node, opts.args[0])
        elif opts.command == "disconnect_hsi":
            if not opts.args:
                print(json.dumps({"error": "disconnect_hsi requires <user_id>"}), file=sys.stderr)
                sys.exit(1)
            result = disconnect_hsi(opts.node, opts.args[0])
        elif opts.command == "start_dhcp_server":
            if not opts.args:
                print(json.dumps({"error": "start_dhcp_server requires <user_id>"}), file=sys.stderr)
                sys.exit(1)
            result = start_dhcp_server(opts.node, opts.args[0])
        elif opts.command == "stop_dhcp_server":
            if not opts.args:
                print(json.dumps({"error": "stop_dhcp_server requires <user_id>"}), file=sys.stderr)
                sys.exit(1)
            result = stop_dhcp_server(opts.node, opts.args[0])
        elif opts.command == "add_dns_record":
            if len(opts.args) < 4:
                print(json.dumps({"error": "add_dns_record requires <user_id> <domain> <ip> <ttl>"}),
                      file=sys.stderr)
                sys.exit(1)
            result = add_dns_record(opts.node, opts.args[0], opts.args[1], opts.args[2], opts.args[3])
        elif opts.command == "remove_dns_record":
            if len(opts.args) < 2:
                print(json.dumps({"error": "remove_dns_record requires <user_id> <domain>"}),
                      file=sys.stderr)
                sys.exit(1)
            result = remove_dns_record(opts.node, opts.args[0], opts.args[1])
        elif opts.command == "set_subscriber_count":
            if not opts.args:
                print(json.dumps({"error": "set_subscriber_count requires <count>"}), file=sys.stderr)
                sys.exit(1)
            result = set_subscriber_count(opts.node, opts.args[0])
        elif opts.command == "get_user_drop_count":
            if not opts.args:
                print(json.dumps({"error": "get_user_drop_count requires <user_id> [port_idx]"}),
                      file=sys.stderr)
                sys.exit(1)
            port_idx = int(opts.args[1]) if len(opts.args) > 1 else 1
            result = get_user_drop_count(opts.node, int(opts.args[0]), port_idx)
        else:
            print(json.dumps({"error": f"Unknown command: {opts.command}"}), file=sys.stderr)
            sys.exit(1)

        print(json.dumps(result))

    except Exception as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
