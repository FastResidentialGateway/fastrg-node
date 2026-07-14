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
    get_dns_cache <user_id>                                 - GetDnsCache → JSON
    flush_dns_cache <user_id>                               - FlushDnsCache → JSON
    get_arp_table <user_id> [max_count]                     - GetArpTable → JSON
    get_system_xstats                                      - GetFastrgSystemXStats → JSON
    get_node_status                                        - GetNodeStatus → JSON
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
    set_snat_config <user_id> <eport> <dip> <iport>         - SetSnatConfig → JSON
    remove_snat_config <user_id> <eport>                    - RemoveSnatConfig → JSON

Requirements:
    - python3 (stdlib only)
    - grpcurl binary: looked up in this script's directory first, then PATH
    - fastrg_node.proto: must be in the same directory as this script
"""

import sys
import os
import json
import re
import ssl
import subprocess
import argparse
import urllib.request
import urllib.error

_DIR = os.path.dirname(os.path.abspath(__file__))
TIMEOUT_SEC = 10

# ---------------------------------------------------------------------------
# Controller config (writes go through the controller; reads stay on the node).
# Provided by run_e2e_test.sh via environment.
# ---------------------------------------------------------------------------
CONTROLLER_REST = os.environ.get("CONTROLLER_REST", "")        # e.g. https://192.168.10.212:28443
CONTROLLER_USER = os.environ.get("CONTROLLER_USER", "admin")
CONTROLLER_PASS = os.environ.get("CONTROLLER_PASS", "admin")
NODE_UUID       = os.environ.get("NODE_UUID", "")
_TOKEN_CACHE    = "/tmp/.fastrg_e2e_ctrl_token"
_SSL_CTX        = ssl.create_default_context()
_SSL_CTX.check_hostname = False
_SSL_CTX.verify_mode = ssl.CERT_NONE


def _ctrl_token(force=False):
    """Return a controller JWT (raw token, no Bearer prefix), cached on disk."""
    if not force and os.path.exists(_TOKEN_CACHE):
        try:
            with open(_TOKEN_CACHE) as f:
                t = f.read().strip()
            if t:
                return t
        except OSError:
            # Cache read failures are non-fatal; fall back to controller login below.
            pass
    body = json.dumps({"username": CONTROLLER_USER, "password": CONTROLLER_PASS}).encode()
    req = urllib.request.Request(CONTROLLER_REST + "/api/login", data=body,
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=TIMEOUT_SEC, context=_SSL_CTX) as r:
        tok = json.loads(r.read()).get("token", "")
    if not tok:
        raise RuntimeError("controller login returned no token")
    with open(_TOKEN_CACHE, "w") as f:
        f.write(tok)
    return tok


def _ctrl(method, path, body=None, _retry=True):
    """Call the controller REST API with the cached token. Returns parsed JSON ({} if empty)."""
    if not CONTROLLER_REST or not NODE_UUID:
        raise RuntimeError("CONTROLLER_REST / NODE_UUID not set in environment")
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(CONTROLLER_REST + path, data=data, method=method,
                                 headers={"Content-Type": "application/json",
                                          "Authorization": _ctrl_token()})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_SEC, context=_SSL_CTX) as r:
            raw = r.read().decode()
            return json.loads(raw) if raw.strip() else {}
    except urllib.error.HTTPError as e:
        # Token may have expired — refresh once and retry.
        if e.code in (401, 403) and _retry:
            _ctrl_token(force=True)
            return _ctrl(method, path, body, _retry=False)
        detail = e.read().decode(errors="replace")
        raise RuntimeError(f"controller {method} {path} -> HTTP {e.code}: {detail}")


def _ctrl_get_hsi(user_id):
    """Fetch a user's HSI config (the flat .config object) from the controller."""
    resp = _ctrl("GET", f"/api/config/{NODE_UUID}/hsi/{user_id}")
    return resp.get("config", resp)

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


def get_dns_cache(node_addr, user_id):
    resp = _grpcurl(node_addr, 'GetDnsCache', {'user_id': user_id})
    return {
        "user_id":       resp.get('user_id', user_id),
        "total_entries": resp.get('total_entries', 0),
        "entries":       resp.get('entries', []),
    }


def flush_dns_cache(node_addr, user_id):
    resp = _grpcurl(node_addr, 'FlushDnsCache', {'user_id': user_id})
    return {
        "status":        resp.get('status', ''),
        "flushed_count": resp.get('flushed_count', 0),
    }


def get_arp_table(node_addr, user_id, max_count=None):
    data = {'user_id': int(user_id)}
    if max_count is not None:
        data['max_count'] = int(max_count)
    resp = _grpcurl(node_addr, 'GetArpTable', data)
    return {
        "user_id":    resp.get('user_id', user_id),
        "total_count": resp.get('total_count', 0),
        "entries":    resp.get('entries', []),
    }


def get_system_xstats(node_addr):
    resp = _grpcurl(node_addr, 'GetFastrgSystemXStats')
    return {"nic_xstats": resp.get('nic_xstats', [])}


def get_node_status(node_addr):
    resp = _grpcurl(node_addr, 'GetNodeStatus')
    return {
        "node_os_version": resp.get('node_os_version', ''),
        "node_uptime":    resp.get('node_uptime', 0),
        "node_ip_info":   resp.get('node_ip_info', ''),
        "healthy":        resp.get('healthy', False),
    }


def apply_config(node_addr, user_id, vlan_id, pppoe_account, pppoe_password,
                 dhcp_pool_start, dhcp_pool_end, dhcp_subnet_mask, dhcp_gateway):
    # Writes go through the controller (SSOT). Create, fall back to update if it
    # already exists. desire_status is managed by connect/disconnect, not here.
    cfg = {
        'user_id':       str(user_id),
        'vlan_id':       str(vlan_id),
        'account_name':  pppoe_account,
        'password':      pppoe_password,
        'dhcp_addr_pool': f"{dhcp_pool_start}-{dhcp_pool_end}",
        'dhcp_subnet':   dhcp_subnet_mask,
        'dhcp_gateway':  dhcp_gateway,
    }
    try:
        _ctrl("POST", f"/api/config/{NODE_UUID}/hsi", cfg)
    except RuntimeError as e:
        if "exist" in str(e).lower() or "409" in str(e):
            _ctrl("PUT", f"/api/config/{NODE_UUID}/hsi/{user_id}", cfg)
        else:
            raise
    return {"status": "Configuration successful"}


def remove_config(node_addr, user_id):
    _ctrl("DELETE", f"/api/config/{NODE_UUID}/hsi/{user_id}")
    return {"status": "Configuration removal successful"}


def connect_hsi(node_addr, user_id):
    _ctrl("POST", "/api/pppoe/dial", {"node_id": NODE_UUID, "user_id": str(user_id)})
    return {"status": "dial requested"}


def disconnect_hsi(node_addr, user_id):
    _ctrl("POST", "/api/pppoe/hangup", {"node_id": NODE_UUID, "user_id": str(user_id)})
    return {"status": "hangup requested"}


def start_dhcp_server(node_addr, user_id):
    resp = _grpcurl(node_addr, 'DhcpServerStart', {'user_id': int(user_id)})
    return {"status": resp.get("status", "")}


def stop_dhcp_server(node_addr, user_id):
    resp = _grpcurl(node_addr, 'DhcpServerStop', {'user_id': int(user_id)})
    return {"status": resp.get("status", "")}


def add_dns_record(node_addr, user_id, domain, ip, ttl):
    _ctrl("POST", f"/api/config/{NODE_UUID}/dns/{user_id}",
          {"domain": domain, "ip": ip, "ttl": int(ttl)})
    return {"status": "dns record added"}


def remove_dns_record(node_addr, user_id, domain):
    _ctrl("DELETE", f"/api/config/{NODE_UUID}/dns/{user_id}/{domain}")
    return {"status": "dns record removed"}


def set_subscriber_count(node_addr, subscriber_count):
    _ctrl("PUT", f"/api/nodes/{NODE_UUID}/subscriber-count",
          {"subscriber_count": int(subscriber_count)})
    return {"status": "subscriber count set"}


def _update_hsi_field(user_id, mutate):
    """Read-modify-write a user's HSI config via the controller (for SNAT / toggles)."""
    cfg = _ctrl_get_hsi(user_id)
    mutate(cfg)
    _ctrl("PUT", f"/api/config/{NODE_UUID}/hsi/{user_id}", cfg)


def set_snat_config(node_addr, user_id, eport, dip, iport):
    def mut(cfg):
        pms = [p for p in cfg.get("port-mapping", []) or [] if str(p.get("eport")) != str(eport)]
        pms.append({"index": str(len(pms)), "eport": str(eport), "dip": dip, "dport": str(iport)})
        cfg["port-mapping"] = pms
    _update_hsi_field(user_id, mut)
    return {"status": "snat set"}


def remove_snat_config(node_addr, user_id, eport):
    def mut(cfg):
        cfg["port-mapping"] = [p for p in cfg.get("port-mapping", []) or []
                               if str(p.get("eport")) != str(eport)]
    _update_hsi_field(user_id, mut)
    return {"status": "snat removed"}


def set_dns_proxy(node_addr, user_id, enable):
    _update_hsi_field(user_id, lambda cfg: cfg.__setitem__("dns_proxy_enable", bool(enable)))
    return {"status": "dns_proxy set"}


def set_tcp_conntrack(node_addr, user_id, enable):
    _update_hsi_field(user_id, lambda cfg: cfg.__setitem__("tcp_conntrack_enable", bool(enable)))
    return {"status": "tcp_conntrack set"}


def ctrl_login(node_addr=None):
    """Verify controller login works; return a short token preview ({} on failure)."""
    tok = _ctrl_token(force=True)
    return {"ok": True, "token_preview": tok[:10]}


def ctrl_desire(node_addr, user_id):
    """Desired PPPoE state for a user, read from the controller (etcd-backed)."""
    cfg = _ctrl_get_hsi(user_id)
    return {"user_id": user_id, "desire_status": cfg.get("desire_status", "")}


def ctrl_pppoe(node_addr, user_id):
    """Observed PPPoE status for a user from the controller DB (fed by Kafka)."""
    return _ctrl("GET", f"/api/config/{NODE_UUID}/pppoe/{user_id}")


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


def pdump_start(node_addr, dir_val, subscriber, filter_expr='', size_mb=0):
    data = {'direction': int(dir_val), 'subscriber': int(subscriber)}
    if filter_expr:
        data['filter'] = filter_expr
    if size_mb:
        data['size_limit_mb'] = int(size_mb)
    return _grpcurl(node_addr, 'PdumpStart', data)


def pdump_stop(node_addr, dir_val, subscriber):
    return _grpcurl(node_addr, 'PdumpStop', {'direction': int(dir_val), 'subscriber': int(subscriber)})


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
        elif opts.command == "get_dns_cache":
            if not opts.args:
                print(json.dumps({"error": "get_dns_cache requires <user_id>"}),
                      file=sys.stderr)
                sys.exit(1)
            result = get_dns_cache(opts.node, int(opts.args[0]))
        elif opts.command == "flush_dns_cache":
            if not opts.args:
                print(json.dumps({"error": "flush_dns_cache requires <user_id>"}),
                      file=sys.stderr)
                sys.exit(1)
            result = flush_dns_cache(opts.node, int(opts.args[0]))
        elif opts.command == "get_arp_table":
            if not opts.args:
                print(json.dumps({"error": "get_arp_table requires <user_id> [max_count]"}),
                      file=sys.stderr)
                sys.exit(1)
            max_count = int(opts.args[1]) if len(opts.args) > 1 else None
            result = get_arp_table(opts.node, int(opts.args[0]), max_count)
        elif opts.command == "get_system_xstats":
            result = get_system_xstats(opts.node)
        elif opts.command == "get_node_status":
            result = get_node_status(opts.node)
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
        elif opts.command == "set_snat_config":
            if len(opts.args) < 4:
                print(json.dumps({"error": "set_snat_config requires <user_id> <eport> <dip> <iport>"}),
                      file=sys.stderr)
                sys.exit(1)
            result = set_snat_config(opts.node, opts.args[0], opts.args[1], opts.args[2], opts.args[3])
        elif opts.command == "remove_snat_config":
            if len(opts.args) < 2:
                print(json.dumps({"error": "remove_snat_config requires <user_id> <eport>"}),
                      file=sys.stderr)
                sys.exit(1)
            result = remove_snat_config(opts.node, opts.args[0], opts.args[1])
        elif opts.command == "set_dns_proxy":
            if len(opts.args) < 2:
                print(json.dumps({"error": "set_dns_proxy requires <user_id> <true|false>"}),
                      file=sys.stderr)
                sys.exit(1)
            enable = opts.args[1].lower() not in ("false", "0", "off")
            result = set_dns_proxy(opts.node, opts.args[0], enable)
        elif opts.command == "set_tcp_conntrack":
            if len(opts.args) < 2:
                print(json.dumps({"error": "set_tcp_conntrack requires <user_id> <true|false>"}),
                      file=sys.stderr)
                sys.exit(1)
            enable = opts.args[1].lower() not in ("false", "0", "off")
            result = set_tcp_conntrack(opts.node, opts.args[0], enable)
        elif opts.command == "ctrl_login":
            result = ctrl_login(opts.node)
        elif opts.command == "ctrl_desire":
            if not opts.args:
                print(json.dumps({"error": "ctrl_desire requires <user_id>"}), file=sys.stderr)
                sys.exit(1)
            result = ctrl_desire(opts.node, int(opts.args[0]))
        elif opts.command == "ctrl_pppoe":
            if not opts.args:
                print(json.dumps({"error": "ctrl_pppoe requires <user_id>"}), file=sys.stderr)
                sys.exit(1)
            result = ctrl_pppoe(opts.node, int(opts.args[0]))
        elif opts.command == "get_user_drop_count":
            if not opts.args:
                print(json.dumps({"error": "get_user_drop_count requires <user_id> [port_idx]"}),
                      file=sys.stderr)
                sys.exit(1)
            port_idx = int(opts.args[1]) if len(opts.args) > 1 else 1
            result = get_user_drop_count(opts.node, int(opts.args[0]), port_idx)
        elif opts.command == "pdump_start":
            # args: dir subscriber [filter_expr [size_mb]]
            if len(opts.args) < 2:
                print(json.dumps({"error": "pdump_start requires <dir> <subscriber> [filter] [size_mb]"}),
                      file=sys.stderr)
                sys.exit(1)
            filter_expr = opts.args[2] if len(opts.args) > 2 else ''
            size_mb = int(opts.args[3]) if len(opts.args) > 3 else 0
            result = pdump_start(opts.node, opts.args[0], opts.args[1], filter_expr, size_mb)
        elif opts.command == "pdump_stop":
            if len(opts.args) < 2:
                print(json.dumps({"error": "pdump_stop requires <dir> <subscriber>"}),
                      file=sys.stderr)
                sys.exit(1)
            result = pdump_stop(opts.node, opts.args[0], opts.args[1])
        else:
            print(json.dumps({"error": f"Unknown command: {opts.command}"}), file=sys.stderr)
            sys.exit(1)

        print(json.dumps(result))

    except Exception as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
