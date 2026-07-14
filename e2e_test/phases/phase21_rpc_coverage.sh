#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 21 — Read-only RPC coverage (Steps 90-92)
# ---------------------------------------------------------------------------

_p21_response_snippet() {
    printf '%s' "$1" | tr '\n' ' ' | cut -c 1-400 || true
}

phase21_rpc_coverage() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 21 — Read-only RPC Coverage (Steps 90-92)"
    bold "═══════════════════════════════════════════════════════"

    local _hsi_config="" _gateway="" _subnet=""
    local _arp="" _arp_detail="" _arp_snippet=""
    local _invalid_out="" _invalid_snippet="" _invalid_rc=0
    local _arp_positive_ok=0 _arp_negative_ok=0

    _hsi_config=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
    _gateway=$(printf '%s' "$_hsi_config" | jq -r '.config.dhcp_gateway // empty' 2>/dev/null || true)
    _subnet=$(printf '%s' "$_hsi_config" | jq -r '.config.dhcp_subnet // empty' 2>/dev/null || true)
    _arp=$(fastrg_grpc get_arp_table "${USER_ID}" || true)
    if _arp_detail=$(ARP_JSON="$_arp" python3 - "$USER_ID" "$_gateway" "$_subnet" <<'PY'
import ipaddress
import json
import os
import re
import sys

user_id = int(sys.argv[1])
network = ipaddress.IPv4Network(f"{sys.argv[2]}/{sys.argv[3]}", strict=False)
reply = json.loads(os.environ["ARP_JSON"])
entries = reply.get("entries", [])
if int(reply.get("user_id", -1)) != user_id or int(reply.get("total_count", 0)) < 1 or not entries:
    raise SystemExit(1)
mac_re = re.compile(r"^[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}$")
matching = [entry for entry in entries
            if ipaddress.IPv4Address(entry.get("ip", "0.0.0.0")) in network
            and mac_re.fullmatch(entry.get("mac", ""))]
if not matching:
    raise SystemExit(1)
entry = matching[0]
print(f"user_id={user_id}; total_count={reply['total_count']}; entry={entry['ip']}/{entry['mac']}; subnet={network}")
PY
    ); then
        _arp_positive_ok=1
    fi

    _invalid_out=$(python3 "${GRPC_CLIENT_DIR}/fastrg_grpc_client.py" \
        --node "${FASTRG_NODE}:${FASTRG_GRPC_PORT}" get_arp_table 99 2>&1) || _invalid_rc=$?
    if [[ $_invalid_rc -ne 0 ]] && \
       printf '%s' "$_invalid_out" | grep -Fq 'Code: InvalidArgument' && \
       printf '%s' "$_invalid_out" | grep -Fq 'does not exist'; then
        _arp_negative_ok=1
    fi

    if [[ $_arp_positive_ok -eq 1 && $_arp_negative_ok -eq 1 ]]; then
        pass "Step 90: GetArpTable" "${_arp_detail}; invalid user rejected (rc=${_invalid_rc})"
    else
        _arp_snippet=$(_p21_response_snippet "$_arp")
        _invalid_snippet=$(_p21_response_snippet "$_invalid_out")
        fail "Step 90: GetArpTable" \
            "positive=${_arp_positive_ok} response='${_arp_snippet:-empty}'; invalid=${_arp_negative_ok} rc=${_invalid_rc} response='${_invalid_snippet:-empty}'"
    fi

    local _xstats="" _xstats_detail="" _xstats_snippet=""
    _xstats=$(fastrg_grpc get_system_xstats || true)
    if _xstats_detail=$(XSTATS_JSON="$_xstats" python3 - <<'PY'
import json
import os

reply = json.loads(os.environ["XSTATS_JSON"])
nics = reply.get("nic_xstats", [])
if len(nics) != 2 or {int(nic.get("port_id", -1)) for nic in nics} != {0, 1}:
    raise SystemExit(1)
values = {}
counts = {}
for nic in nics:
    port_id = int(nic["port_id"])
    xstats = nic.get("xstats", [])
    if not xstats:
        raise SystemExit(1)
    counts[port_id] = len(xstats)
    matches = [stat for stat in xstats if stat.get("name") == "rx_good_packets"]
    if len(matches) != 1:
        raise SystemExit(1)
    value = int(matches[0]["value"])
    if value < 0:
        raise SystemExit(1)
    values[port_id] = value
if values[0] <= 0:
    raise SystemExit(1)
print(f"ports=0,1; xstats={counts[0]}/{counts[1]}; rx_good_packets={values[0]}/{values[1]}")
PY
    ); then
        pass "Step 91: GetFastrgSystemXStats" "$_xstats_detail"
    else
        _xstats_snippet=$(_p21_response_snippet "$_xstats")
        fail "Step 91: GetFastrgSystemXStats" "response='${_xstats_snippet:-empty}'"
    fi

    local _status="" _status_detail="" _status_snippet=""
    _status=$(fastrg_grpc get_node_status || true)
    if _status_detail=$(STATUS_JSON="$_status" python3 - <<'PY'
import ipaddress
import json
import os

reply = json.loads(os.environ["STATUS_JSON"])
uptime = int(reply.get("node_uptime", 0))
os_version = reply.get("node_os_version", "")
ip_info = reply.get("node_ip_info", "")
if reply.get("healthy") is not True or not (0 < uptime < 10 * 365 * 24 * 60 * 60):
    raise SystemExit(1)
if "Linux" not in os_version or not ip_info:
    raise SystemExit(1)
ipaddress.IPv4Address(ip_info.split("/", 1)[0])
print(f"healthy=true; uptime={uptime}s; os='{os_version}'; ip='{ip_info}'")
PY
    ); then
        pass "Step 92: GetNodeStatus" "$_status_detail"
    else
        _status_snippet=$(_p21_response_snippet "$_status")
        fail "Step 92: GetNodeStatus" "response='${_status_snippet:-empty}'"
    fi
}
