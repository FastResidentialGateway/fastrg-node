#!/usr/bin/env python3
"""
FastRG Node gRPC client helper — uses grpcurl subprocess (no grpcio/pb2 required).

Usage:
    fastrg_grpc_client.py --node <host:port> <command> [args...]

Commands:
    get_hsi_info                  - GetFastrgHsiInfo  → JSON
    get_dhcp_info                 - GetFastrgDhcpInfo → JSON
    get_system_info               - GetFastrgSystemInfo → JSON
    get_port_fwd_info <user_id>   - GetPortFwdInfo    → JSON
    get_dns_static <user_id>      - GetDnsStaticRecords → JSON

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
        else:
            print(json.dumps({"error": f"Unknown command: {opts.command}"}), file=sys.stderr)
            sys.exit(1)

        print(json.dumps(result))

    except Exception as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
