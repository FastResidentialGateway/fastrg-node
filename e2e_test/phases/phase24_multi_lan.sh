#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 24 — Multiple virtual LAN devices (Steps 98-100)
# ---------------------------------------------------------------------------

_P24_REMOTE_DHCP=/tmp/fastrg_multi_lan_dhcp_client.py
_P24_REMOTE_DEVICE=/tmp/fastrg_lan_device_sim.py
_P24_WAN_LISTENER_PID=/tmp/e2e_multi_lan_listener.pid
_P24_WAN_LISTENER_OUT=/tmp/e2e_multi_lan_listener.out
_P24_WAN_LISTENER_READY=/tmp/e2e_multi_lan_listener.ready
_P24_WAN_LISTENER_PORTS=/tmp/e2e_multi_lan_listener.ports
_P24_MACS=(02:e2:e2:00:00:21 02:e2:e2:00:00:22 02:e2:e2:00:00:23)
_P24_LEASE_IPS=("" "" "")
_P24_SERVER_IDS=("" "" "")
_P24_SERVER_MACS=("" "" "")
_P24_LAN_IFACE=""
_P24_BASELINE_LEASE_COUNT=""
_P24_BASELINE_IPS="[]"
_P24_METRICS_PORT=""

_p24_snippet() {
    printf '%s' "$1" | tr '\n' ' ' | cut -c 1-500 || true
}

_p24_fetch_metric() {
    local _body
    _body=$(ssh_node \
        "curl -fsS --max-time 5 http://127.0.0.1:${_P24_METRICS_PORT}/metrics" \
        2>/dev/null || true)
    printf '%s\n' "$_body" | awk -v uid="$USER_ID" '
        $1 ~ /^fastrg_node_per_user_dhcp_cur_lease_count\{/ &&
        $1 ~ ("user_id=\"" uid "\"") { print $2; exit }
    ' || true
}

_p24_fetch_nat_metric() {
    local _body
    _body=$(ssh_node \
        "curl -fsS --max-time 5 http://127.0.0.1:${_P24_METRICS_PORT}/metrics" \
        2>/dev/null || true)
    printf '%s\n' "$_body" | awk -v uid="$USER_ID" '
        $1 ~ /^fastrg_node_per_user_nat_entries_used\{/ &&
        $1 ~ ("user_id=\"" uid "\"") { print $2; exit }
    ' || true
}

_p24_get_ips() {
    fastrg_grpc get_dhcp_info | jq -c \
        ".dhcp_infos[] | select(.user_id == ${USER_ID}) | (.inuse_ips // [])" \
        2>/dev/null || true
}

_p24_run_dhcp() {
    local _index="$1"
    shift
    ssh_lan "python3 '${_P24_REMOTE_DHCP}' --interface '${_P24_LAN_IFACE}' \
        --mac '${_P24_MACS[$_index]}' $*" 2>&1
}

_p24_release_device() {
    local _index="$1"
    [[ -z "${_P24_LEASE_IPS[$_index]:-}" || -z "${_P24_SERVER_IDS[$_index]:-}" || \
       -z "${_P24_SERVER_MACS[$_index]:-}" || -z "${_P24_LAN_IFACE:-}" ]] && return 0
    _p24_run_dhcp "$_index" --ip "${_P24_LEASE_IPS[$_index]}" \
        --server-id "${_P24_SERVER_IDS[$_index]}" \
        --server-mac "${_P24_SERVER_MACS[$_index]}" release >/dev/null 2>&1 || true
    return 0
}

_p24_stop_wan_listener() {
    if ssh_wan "if [ -s '${_P24_WAN_LISTENER_PID}' ]; then
            _pid=\$(cat '${_P24_WAN_LISTENER_PID}' 2>/dev/null || true)
            if [ -n \"\$_pid\" ] && kill -0 \"\$_pid\" 2>/dev/null; then
                kill -TERM \"\$_pid\" 2>/dev/null || true
            fi
            _i=0
            while [ -n \"\$_pid\" ] && kill -0 \"\$_pid\" 2>/dev/null && [ \"\$_i\" -lt 5 ]; do
                sleep 1
                _i=\$(( _i + 1 ))
            done
            [ -z \"\$_pid\" ] || ! kill -0 \"\$_pid\" 2>/dev/null
        fi" >/dev/null 2>&1; then
        return 0
    fi
    warn "Phase 24 WAN UDP listener did not exit within 5s after SIGTERM"
    return 1
}

_p24_poll_cleanup() {
    local _i _metric _ips _remaining=0 _index
    for _i in $(seq 1 15); do
        _metric=$(_p24_fetch_metric)
        _ips=$(_p24_get_ips)
        _remaining=0
        for _index in 0 1 2; do
            if [[ -n "${_P24_LEASE_IPS[$_index]:-}" ]] && \
               printf '%s' "$_ips" | jq -e --arg ip "${_P24_LEASE_IPS[$_index]}" \
                   'index($ip) != null' >/dev/null 2>&1; then
                _remaining=1
            fi
        done
        if [[ "$_metric" == "${_P24_BASELINE_LEASE_COUNT}" && $_remaining -eq 0 ]]; then
            return 0
        fi
        sleep 2
    done
    return 1
}

_cleanup_phase24_multi_lan() {
    set +eu
    local _index
    # Release in reverse allocation order.  The DHCP server scans for the
    # first free lan_user slot before later MAC matches, so forward-order
    # release could make a later client's RELEASE land on an earlier slot.
    for _index in 2 1 0; do
        _p24_release_device "$_index" || true
    done
    if [[ "${_P24_BASELINE_LEASE_COUNT:-}" =~ ^[0-9]+$ ]] && \
       [[ -n "${_P24_METRICS_PORT:-}" ]] && _p24_poll_cleanup; then
        info "Cleanup(phase24): verified all virtual leases returned to baseline ${_P24_BASELINE_LEASE_COUNT}."
        _P24_LEASE_IPS=("" "" "")
    elif [[ -n "${_P24_LEASE_IPS[0]:-}${_P24_LEASE_IPS[1]:-}${_P24_LEASE_IPS[2]:-}" ]]; then
        warn "Cleanup(phase24): virtual leases remain after RELEASE polling; DHCP expiry will reclaim them."
    fi

    _p24_stop_wan_listener || true
    ssh_lan "pkill -TERM -f '^python3 ${_P24_REMOTE_DHCP}( |$)' 2>/dev/null || true; \
        pkill -TERM -f '^python3 ${_P24_REMOTE_DEVICE}( |$)' 2>/dev/null || true; \
        rm -f '${_P24_REMOTE_DHCP}' '${_P24_REMOTE_DEVICE}'" >/dev/null 2>&1 || true
    ssh_wan "rm -f '${_P24_WAN_LISTENER_PID}' '${_P24_WAN_LISTENER_OUT}' \
        '${_P24_WAN_LISTENER_READY}' '${_P24_WAN_LISTENER_PORTS}'" >/dev/null 2>&1 || true
    return 0
}

phase24_multi_lan() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 24 — Multiple LAN Devices (Steps 98-100)"
    bold "═══════════════════════════════════════════════════════"

    local _hsi="" _vlan="" _pool="" _pool_start="" _pool_end="" _gateway=""
    local _real_ip="" _expected_count="" _observed_metric="" _observed_ips=""
    local _offer="" _ack="" _offer_ip="" _offer_xid="" _offer_type="" _ack_type="" _ack_ip=""
    local _step98_ok=1 _step99_ok=1 _step100_ok=1 _index _i
    local _arp="" _arp_detail="" _gateway_mac=""
    local _nat_base="" _nat_cur="" _nat_peak="" _nat_delta=0 _received_ports=0
    local _listener_ready=0 _listener_out="" _send_ok=1 _lease_after="" _flow_src_base=43100
    local _flow_count_per_device=5 _listener_port="$SRV_PORT"

    _hsi=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
    _vlan=$(printf '%s' "$_hsi" | jq -r '.config.vlan_id // empty' 2>/dev/null || true)
    _pool=$(printf '%s' "$_hsi" | jq -r '.config.dhcp_addr_pool // empty' 2>/dev/null || true)
    _pool_start="${_pool%-*}"
    _pool_end="${_pool#*-}"
    _gateway=$(printf '%s' "$_hsi" | jq -r '.config.dhcp_gateway // empty' 2>/dev/null || true)
    _P24_LAN_IFACE="vlan${_vlan}"
    _P24_METRICS_PORT=$(ssh_node \
        "grep 'MetricsListenPort' /etc/fastrg/config.cfg 2>/dev/null" | \
        awk -F'"' '{print $2}' | awk -F: '{print $NF}' || true)
    _P24_BASELINE_LEASE_COUNT=$(_p24_fetch_metric)
    _P24_BASELINE_IPS=$(_p24_get_ips)
    _real_ip=$(ssh_lan "ip -4 addr show '${_P24_LAN_IFACE}' 2>/dev/null | \
        sed -nE 's/.*inet ([0-9.]+)\/.*/\1/p' | head -1" 2>/dev/null | tr -d '[:space:]' || true)

    if [[ -z "$_vlan" || -z "$_gateway" || -z "$_pool_start" || -z "$_pool_end" || \
          -z "$_P24_METRICS_PORT" || ! "$_P24_BASELINE_LEASE_COUNT" =~ ^[0-9]+$ || \
          -z "$_real_ip" ]] || ! printf '%s' "$_P24_BASELINE_IPS" | \
            jq -e --arg ip "$_real_ip" 'index($ip) != null' >/dev/null 2>&1; then
        _step98_ok=0
    fi

    if [[ $_step98_ok -eq 1 ]]; then
        scp $SSH_OPTS "${GRPC_CLIENT_DIR}/dhcp_client_sim.py" \
            "root@${LAN_HOST}:${_P24_REMOTE_DHCP}" >/dev/null 2>&1 || _step98_ok=0
        scp $SSH_OPTS "${GRPC_CLIENT_DIR}/lan_device_sim.py" \
            "root@${LAN_HOST}:${_P24_REMOTE_DEVICE}" >/dev/null 2>&1 || _step98_ok=0
        ssh_lan "chmod 700 '${_P24_REMOTE_DHCP}' '${_P24_REMOTE_DEVICE}'" \
            >/dev/null 2>&1 || _step98_ok=0
    fi

    if [[ $_step98_ok -eq 1 ]]; then
        for _index in 0 1 2; do
            _offer=$(_p24_run_dhcp "$_index" --timeout 8 discover) || { _step98_ok=0; break; }
            _offer_type=$(printf '%s' "$_offer" | jq -r '.received // empty' 2>/dev/null || true)
            _offer_ip=$(printf '%s' "$_offer" | jq -r '.yiaddr // empty' 2>/dev/null || true)
            _offer_xid=$(printf '%s' "$_offer" | jq -r '.xid // empty' 2>/dev/null || true)
            _P24_SERVER_IDS[$_index]=$(printf '%s' "$_offer" | jq -r '.server_id // empty' 2>/dev/null || true)
            _P24_SERVER_MACS[$_index]=$(printf '%s' "$_offer" | jq -r '.server_mac // empty' 2>/dev/null || true)
            if [[ "$_offer_type" != "OFFER" || -z "$_offer_ip" || -z "$_offer_xid" || \
                  -z "${_P24_SERVER_IDS[$_index]}" || -z "${_P24_SERVER_MACS[$_index]}" ]]; then
                _step98_ok=0
                break
            fi
            _ack=$(_p24_run_dhcp "$_index" --timeout 8 --xid "$_offer_xid" \
                --ip "$_offer_ip" --server-id "${_P24_SERVER_IDS[$_index]}" request) || {
                _step98_ok=0
                break
            }
            _ack_type=$(printf '%s' "$_ack" | jq -r '.received // empty' 2>/dev/null || true)
            _ack_ip=$(printf '%s' "$_ack" | jq -r '.yiaddr // empty' 2>/dev/null || true)
            [[ "$_ack_type" == "ACK" && "$_ack_ip" == "$_offer_ip" ]] || { _step98_ok=0; break; }
            _P24_LEASE_IPS[$_index]="$_offer_ip"
        done
    fi

    if [[ "$_P24_BASELINE_LEASE_COUNT" =~ ^[0-9]+$ ]]; then
        _expected_count=$(( _P24_BASELINE_LEASE_COUNT + 3 ))
    else
        _expected_count=3
    fi
    if [[ $_step98_ok -eq 1 ]]; then
        for _i in $(seq 1 15); do
            _observed_metric=$(_p24_fetch_metric)
            _observed_ips=$(_p24_get_ips)
            if [[ "$_observed_metric" == "$_expected_count" ]] && \
               P24_IPS="$_observed_ips" P24_BASELINE="$_P24_BASELINE_IPS" python3 - \
                   "$_pool_start" "$_pool_end" "${_P24_LEASE_IPS[@]}" <<'PY'
import ipaddress
import json
import os
import sys

start, end = map(ipaddress.IPv4Address, sys.argv[1:3])
leases = sys.argv[3:]
observed = json.loads(os.environ["P24_IPS"])
baseline = json.loads(os.environ["P24_BASELINE"])
valid = len(leases) == 3 and len(set(leases)) == 3
valid = valid and all(start <= ipaddress.IPv4Address(ip) <= end for ip in leases)
valid = valid and all(ip in observed for ip in leases + baseline)
raise SystemExit(0 if valid else 1)
PY
            then
                break
            fi
            sleep 2
        done
        [[ "$_observed_metric" == "$_expected_count" ]] || _step98_ok=0
        P24_IPS="$_observed_ips" P24_BASELINE="$_P24_BASELINE_IPS" python3 - \
            "$_pool_start" "$_pool_end" "${_P24_LEASE_IPS[@]}" <<'PY' || _step98_ok=0
import ipaddress
import json
import os
import sys

start, end = map(ipaddress.IPv4Address, sys.argv[1:3])
leases = sys.argv[3:]
observed = json.loads(os.environ["P24_IPS"])
baseline = json.loads(os.environ["P24_BASELINE"])
valid = len(leases) == 3 and len(set(leases)) == 3
valid = valid and all(start <= ipaddress.IPv4Address(ip) <= end for ip in leases)
valid = valid and all(ip in observed for ip in leases + baseline)
raise SystemExit(0 if valid else 1)
PY
    fi

    if [[ $_step98_ok -eq 1 ]]; then
        pass "Step 98: multiple LAN DHCP leases" \
            "baseline=${_P24_BASELINE_LEASE_COUNT}/${_P24_BASELINE_IPS}; leases=${_P24_LEASE_IPS[*]}; observed=${_observed_metric}/${_observed_ips}"
    else
        fail "Step 98: multiple LAN DHCP leases" \
            "baseline='${_P24_BASELINE_LEASE_COUNT}' ips='${_P24_BASELINE_IPS}'; iface='${_P24_LAN_IFACE}' pool='${_pool}'; leases='${_P24_LEASE_IPS[*]}'; offer='$(_p24_snippet "$_offer")'; ack='$(_p24_snippet "$_ack")'; observed='${_observed_metric}/${_observed_ips}'"
    fi

    if [[ $_step98_ok -eq 1 ]]; then
        for _index in 0 1 2; do
            ssh_lan "python3 '${_P24_REMOTE_DEVICE}' --interface '${_P24_LAN_IFACE}' \
                --src-mac '${_P24_MACS[$_index]}' --src-ip '${_P24_LEASE_IPS[$_index]}' \
                --target-ip '${_gateway}' arp-request" >/dev/null 2>&1 || _step99_ok=0
        done
        for _i in $(seq 1 15); do
            _arp=$(fastrg_grpc get_arp_table "${USER_ID}" || true)
            if _arp_detail=$(P24_ARP="$_arp" python3 - \
                "${_P24_LEASE_IPS[0]}" "${_P24_MACS[0]}" \
                "${_P24_LEASE_IPS[1]}" "${_P24_MACS[1]}" \
                "${_P24_LEASE_IPS[2]}" "${_P24_MACS[2]}" <<'PY'
import json
import os
import sys

reply = json.loads(os.environ["P24_ARP"])
expected = {sys.argv[i]: sys.argv[i + 1].lower() for i in range(1, len(sys.argv), 2)}
actual = {entry.get("ip"): entry.get("mac", "").lower() for entry in reply.get("entries", [])}
if int(reply.get("total_count", 0)) < 4 or any(actual.get(ip) != mac for ip, mac in expected.items()):
    raise SystemExit(1)
print(f"total_count={reply['total_count']}; mappings=" + ",".join(f"{ip}/{actual[ip]}" for ip in expected))
PY
            ); then
                break
            fi
            sleep 2
        done
        [[ -n "$_arp_detail" ]] || _step99_ok=0
    else
        _step99_ok=0
    fi

    if [[ $_step99_ok -eq 1 ]]; then
        pass "Step 99: MAC table per-device mappings" "$_arp_detail"
    else
        fail "Step 99: MAC table per-device mappings" \
            "Step 98 ready=${_step98_ok}; response='$(_p24_snippet "$_arp")'; expected=${_P24_LEASE_IPS[0]:-NA}/${_P24_MACS[0]},${_P24_LEASE_IPS[1]:-NA}/${_P24_MACS[1]},${_P24_LEASE_IPS[2]:-NA}/${_P24_MACS[2]}"
    fi

    if [[ $_step98_ok -eq 1 ]]; then
        # The DHCP reply Ethernet source is the FastRG LAN gateway MAC.  Use
        # that authoritative value instead of depending on LAN-host neighbor
        # cache state after the synthetic ARP traffic in Step 99.
        _gateway_mac="${_P24_SERVER_MACS[0]}"
        [[ "$_gateway_mac" =~ ^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$ ]] || _step100_ok=0
        _nat_base=$(_p24_fetch_nat_metric)
        [[ "$_nat_base" =~ ^[0-9]+$ ]] || _step100_ok=0
    else
        _step100_ok=0
    fi

    if [[ $_step100_ok -eq 1 ]]; then
        ssh_wan "rm -f '${_P24_WAN_LISTENER_PID}' '${_P24_WAN_LISTENER_OUT}' \
            '${_P24_WAN_LISTENER_READY}' '${_P24_WAN_LISTENER_PORTS}';
nohup python3 -u - '${_listener_port}' '${_P24_WAN_LISTENER_READY}' \
    '${_P24_WAN_LISTENER_PORTS}' >'${_P24_WAN_LISTENER_OUT}' 2>&1 <<'PY' &
import socket
import sys
import time

port = int(sys.argv[1])
ready_path = sys.argv[2]
ports_path = sys.argv[3]
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('0.0.0.0', port))
sock.settimeout(0.5)
with open(ready_path, 'w', encoding='ascii') as ready:
    ready.write('ready\n')
deadline = time.monotonic() + 45
while time.monotonic() < deadline:
    try:
        _payload, peer = sock.recvfrom(2048)
    except socket.timeout:
        continue
    with open(ports_path, 'a', encoding='ascii') as received:
        received.write(f'{peer[1]}\n')
PY
echo \$! >'${_P24_WAN_LISTENER_PID}'" >/dev/null 2>&1 || _step100_ok=0
        for _i in $(seq 1 10); do
            if ssh_wan "test -s '${_P24_WAN_LISTENER_READY}'" 2>/dev/null; then
                _listener_ready=1
                break
            fi
            sleep 1
        done
        [[ $_listener_ready -eq 1 ]] || _step100_ok=0
        if [[ $_listener_ready -ne 1 ]]; then
            _listener_out=$(ssh_wan "cat '${_P24_WAN_LISTENER_OUT}' 2>/dev/null" || true)
        fi
    fi

    if [[ $_step100_ok -eq 1 ]]; then
        for _index in 0 1 2; do
            ssh_lan "python3 '${_P24_REMOTE_DEVICE}' --interface '${_P24_LAN_IFACE}' \
                --src-mac '${_P24_MACS[$_index]}' --src-ip '${_P24_LEASE_IPS[$_index]}' \
                --dst-mac '${_gateway_mac}' --dst-ip '${WAN_IP}' --dst-port '${_listener_port}' \
                --src-port-start '$(( _flow_src_base + _index * 10 ))' \
                --flow-count '${_flow_count_per_device}' --repeat 3 udp" \
                >/dev/null 2>&1 || _send_ok=0
        done
        [[ $_send_ok -eq 1 ]] || _step100_ok=0
    fi

    _nat_peak="${_nat_base:-0}"
    if [[ $_step100_ok -eq 1 ]]; then
        for _i in $(seq 1 15); do
            _nat_cur=$(_p24_fetch_nat_metric)
            if [[ "$_nat_cur" =~ ^[0-9]+$ ]] && (( _nat_cur > _nat_peak )); then
                _nat_peak=$_nat_cur
            fi
            _nat_delta=$(( _nat_peak - _nat_base ))
            _received_ports=$(ssh_wan \
                "sort -nu '${_P24_WAN_LISTENER_PORTS}' 2>/dev/null | wc -l" \
                2>/dev/null | tr -d '[:space:]' || true)
            [[ "$_received_ports" =~ ^[0-9]+$ ]] || _received_ports=0
            if (( _nat_delta >= 15 && _received_ports >= 15 )); then
                break
            fi
            sleep 1
        done
        _lease_after=$(_p24_fetch_metric)
        if (( _nat_delta < 15 || _received_ports < 15 )) || \
           [[ "$_lease_after" != "$_expected_count" ]]; then
            _step100_ok=0
        fi
    fi

    _p24_stop_wan_listener || _step100_ok=0
    if [[ $_step100_ok -eq 1 ]]; then
        pass "Step 100: NAT mappings for multiple source IPs" \
            "entries delta=${_nat_delta} (baseline=${_nat_base}, peak=${_nat_peak}); WAN observed ${_received_ports} distinct source ports; lease count=${_lease_after}"
    else
        fail "Step 100: NAT mappings for multiple source IPs" \
            "Step 98 ready=${_step98_ok}; gateway_mac='${_gateway_mac}'; listener=${_listener_ready} output='$(_p24_snippet "$_listener_out")'; send=${_send_ok}; entries=${_nat_base:-NA}->${_nat_peak:-NA} delta=${_nat_delta}; WAN ports=${_received_ports}; lease=${_lease_after:-NA}/${_expected_count}"
    fi

    _cleanup_phase24_multi_lan
    return 0
}
