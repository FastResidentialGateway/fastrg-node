#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 20 — NAT idle expiry and amortized GC metrics (Steps 79-81)
# ---------------------------------------------------------------------------

_P20_LAN_IPERF_PID=/tmp/e2e_nat_gc_iperf.pid
_P20_LAN_IPERF_OUT=/tmp/e2e_nat_gc_iperf.out
_P20_WAN_IPERF_PID=/tmp/e2e_nat_gc_server.pid
_P20_WAN_IPERF_OUT=/tmp/e2e_nat_gc_server.out
_P20_LAN_LISTENER_PID=/tmp/e2e_nat_expiry_listener.pid
_P20_LAN_LISTENER_OUT=/tmp/e2e_nat_expiry_listener.out
_P20_LAN_LISTENER_READY=/tmp/e2e_nat_expiry_listener.ready
_P20_LAN_LISTENER_RECV=/tmp/e2e_nat_expiry_listener.recv
_P20_WAN_TCPDUMP_PID=/tmp/e2e_nat_expiry_tcpdump.pid
_P20_WAN_TCPDUMP_OUT=/tmp/e2e_nat_expiry_tcpdump.out

_p20_stop_remote_pid() {
    local _ssh_fn="$1"
    local _pid_file="$2"
    local _label="$3"

    if "$_ssh_fn" "if [ -s '${_pid_file}' ]; then
            _pid=\$(cat '${_pid_file}' 2>/dev/null || true)
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

    warn "${_label} did not exit within 5s after SIGTERM"
    return 1
}

_cleanup_phase20_nat_expiry() {
    _p20_stop_remote_pid ssh_lan "$_P20_LAN_IPERF_PID" "Phase 20 iperf3 client" || true
    _p20_stop_remote_pid ssh_wan "$_P20_WAN_IPERF_PID" "Phase 20 iperf3 server" || true
    _p20_stop_remote_pid ssh_lan "$_P20_LAN_LISTENER_PID" "Phase 20 UDP listener" || true
    _p20_stop_remote_pid ssh_wan "$_P20_WAN_TCPDUMP_PID" "Phase 20 tcpdump" || true

    ssh_lan "rm -f '${_P20_LAN_IPERF_PID}' '${_P20_LAN_IPERF_OUT}' \
        '${_P20_LAN_LISTENER_PID}' '${_P20_LAN_LISTENER_OUT}' \
        '${_P20_LAN_LISTENER_READY}' '${_P20_LAN_LISTENER_RECV}'" >/dev/null 2>&1 || true
    ssh_wan "rm -f '${_P20_WAN_IPERF_PID}' '${_P20_WAN_IPERF_OUT}' \
        '${_P20_WAN_TCPDUMP_PID}' '${_P20_WAN_TCPDUMP_OUT}'" >/dev/null 2>&1 || true
    return 0
}

_p20_fetch_metrics() {
    ssh_node "curl -fsS --max-time 5 http://127.0.0.1:${_P20_METRICS_PORT}/metrics" 2>/dev/null || true
}

_p20_metric_from_body() {
    local _body="$1"
    local _family="$2"

    printf '%s\n' "$_body" | awk -v family="$_family" -v uid="$USER_ID" '
        $1 ~ ("^" family "\\{") && $1 ~ ("user_id=\\\"" uid "\\\"") { print $2; exit }
    ' || true
}

_p20_metrics_are_uints() {
    local _value
    for _value in "$@"; do
        [[ "$_value" =~ ^[0-9]+$ ]] || return 1
    done
    return 0
}

phase20_nat_expiry() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 20 — NAT idle expiry + GC metrics (Steps 79-81)"
    bold "═══════════════════════════════════════════════════════"

    local _flow_count=20
    local _flow_src_base=41000
    local _flow_dst_port=40077
    local _body=""
    local _entries_base="" _gc_base="" _alloc_base=""
    local _entries_cur="" _gc_cur="" _alloc_cur=""
    local _entries_peak=0
    local _step77_ok=0
    local _i

    _P20_METRICS_PORT=$(ssh_node \
        "grep 'MetricsListenPort' /etc/fastrg/config.cfg 2>/dev/null" | \
        awk -F'"' '{print $2}' | awk -F: '{print $NF}' || true)
    _body=$(_p20_fetch_metrics)
    _entries_base=$(_p20_metric_from_body "$_body" "fastrg_node_per_user_nat_entries_used")
    _gc_base=$(_p20_metric_from_body "$_body" "fastrg_node_per_user_nat_gc_reclaimed_total")
    _alloc_base=$(_p20_metric_from_body "$_body" "fastrg_node_per_user_nat_alloc_fail_total")

    info "Step 79: creating ${_flow_count} UDP mappings with distinct LAN source ports..."
    if _p20_metrics_are_uints "$_entries_base" "$_gc_base" "$_alloc_base" && \
       ssh_lan "python3 - <<'PY'
import socket

for port in range(${_flow_src_base}, ${_flow_src_base} + ${_flow_count}):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', port))
    sock.sendto(b'nat-gc-idle', ('${WAN_IP}', ${_flow_dst_port}))
    sock.close()
PY"; then
        for _i in $(seq 1 15); do
            _body=$(_p20_fetch_metrics)
            _entries_cur=$(_p20_metric_from_body "$_body" "fastrg_node_per_user_nat_entries_used")
            _alloc_cur=$(_p20_metric_from_body "$_body" "fastrg_node_per_user_nat_alloc_fail_total")
            if _p20_metrics_are_uints "$_entries_cur" "$_alloc_cur" && \
               (( _entries_cur - _entries_base >= _flow_count )); then
                _step77_ok=1
                break
            fi
            sleep 1
        done
    fi

    if [[ $_step77_ok -eq 1 && "$_alloc_cur" == "$_alloc_base" ]]; then
        _entries_peak=$_entries_cur
        pass "Step 79: NAT entry creation metrics" \
            "entries delta=$(( _entries_cur - _entries_base )) (target>=${_flow_count}); alloc_fail delta=0"
    else
        fail "Step 79: NAT entry creation metrics" \
            "baseline(entries/gc/alloc)=${_entries_base:-NA}/${_gc_base:-NA}/${_alloc_base:-NA}; current(entries/alloc)=${_entries_cur:-NA}/${_alloc_cur:-NA}"
        _p20_metrics_are_uints "$_entries_cur" && _entries_peak=$_entries_cur
    fi

    # Keep one TCP NAT mapping active while the UDP mappings remain idle.
    info "Step 80: starting sustained iperf3 traffic while idle UDP mappings expire..."
    local _server_ready=0 _client_ready=0 _gc_ok=0 _throughput_ok=0
    local _client_stopped=0 _server_stopped=0
    local _gc_delta=0 _entry_drop=0 _iperf_out=""

    ssh_wan "rm -f '${_P20_WAN_IPERF_PID}' '${_P20_WAN_IPERF_OUT}'; \
        nohup iperf3 -s -1 -B '${WAN_IP}' -p '${SRV_PORT}' \
        >'${_P20_WAN_IPERF_OUT}' 2>&1 < /dev/null & echo \$! >'${_P20_WAN_IPERF_PID}'" || true
    for _i in $(seq 1 10); do
        if ssh_wan "ss -ltn 2>/dev/null | grep -q ':${SRV_PORT}'" 2>/dev/null; then
            _server_ready=1
            break
        fi
        sleep 1
    done

    if [[ $_server_ready -eq 1 ]]; then
        ssh_lan "rm -f '${_P20_LAN_IPERF_PID}' '${_P20_LAN_IPERF_OUT}'; \
            nohup iperf3 -c '${WAN_IP}' -p '${SRV_PORT}' -t 45 -i 1 --forceflush \
            >'${_P20_LAN_IPERF_OUT}' 2>&1 < /dev/null & echo \$! >'${_P20_LAN_IPERF_PID}'" || true
        for _i in $(seq 1 12); do
            if ssh_lan "ss -tn 'dport = :${SRV_PORT}' 2>/dev/null | grep -q ESTAB" 2>/dev/null; then
                _client_ready=1
                break
            fi
            sleep 1
        done
    fi

    if _p20_metrics_are_uints "$_entries_base" "$_gc_base"; then
        for _i in $(seq 1 30); do
            _body=$(_p20_fetch_metrics)
            _entries_cur=$(_p20_metric_from_body "$_body" "fastrg_node_per_user_nat_entries_used")
            _gc_cur=$(_p20_metric_from_body "$_body" "fastrg_node_per_user_nat_gc_reclaimed_total")
            if _p20_metrics_are_uints "$_entries_cur" "$_gc_cur"; then
                (( _entries_cur > _entries_peak )) && _entries_peak=$_entries_cur
                _gc_delta=$(( _gc_cur - _gc_base ))
                _entry_drop=$(( _entries_peak - _entries_cur ))
                if (( _entries_cur <= _entries_base + 4 && \
                      _entry_drop >= _flow_count / 2 && \
                      _gc_delta >= _flow_count )); then
                    _gc_ok=1
                    break
                fi
            fi
            sleep 1
        done
    fi

    _p20_stop_remote_pid ssh_lan "$_P20_LAN_IPERF_PID" "Phase 20 iperf3 client" && _client_stopped=1
    _p20_stop_remote_pid ssh_wan "$_P20_WAN_IPERF_PID" "Phase 20 iperf3 server" && _server_stopped=1
    _iperf_out=$(ssh_lan "cat '${_P20_LAN_IPERF_OUT}' 2>/dev/null" || true)
    if [[ $_client_ready -eq 1 ]] && printf '%s\n' "$_iperf_out" | \
        grep -Eq '[1-9][0-9.]* [KMG]bits/sec'; then
        _throughput_ok=1
    fi

    if [[ $_gc_ok -eq 1 && $_throughput_ok -eq 1 && \
          $_client_stopped -eq 1 && $_server_stopped -eq 1 ]]; then
        pass "Step 80: idle GC under mixed traffic" \
            "entries=${_entries_peak}->${_entries_cur} (baseline=${_entries_base}); gc_reclaimed delta=${_gc_delta}; active TCP throughput observed"
    else
        fail "Step 80: idle GC under mixed traffic" \
            "server/client=${_server_ready}/${_client_ready}; entries=${_entries_peak}->${_entries_cur:-NA} baseline=${_entries_base:-NA}; gc_delta=${_gc_delta}; throughput=${_throughput_ok}; stopped=${_client_stopped}/${_server_stopped}"
    fi

    # UDP has no TCP conntrack/SPI gate in decaps_udp.  The reverse key is the
    # allocated NAT port plus the WAN source IP and source port, so the injected
    # packets below deliberately preserve the original remote tuple.
    info "Step 81: checking live UDP inbound control and expired inbound rejection..."
    local _ppp_ip="" _listener_port=42079 _remote_port=40079 _nat_port=""
    local _capture_ready=0 _listener_ready=0 _positive_ok=0 _negative_seen=0
    local _listener_alive=1 _listener_stopped=0 _tcpdump_stopped=0
    local _attempt _cap="" _ppp_ip_re="" _wan_ip_re="" _step79_issue=""
    local _positive_payload="nat-expiry-positive" _negative_payload="nat-expiry-expired"

    _ppp_ip=$(fastrg_grpc get_hsi_info | \
        jq -r ".hsi_infos[] | select(.user_id == ${USER_ID}) | .ip_addr // empty" 2>/dev/null || true)
    [[ -z "$_ppp_ip" ]] && _step79_issue="ppp_ip=unavailable"

    for _attempt in $(seq 1 3); do
        [[ -n "$_step79_issue" ]] && break
        _capture_ready=0
        _listener_ready=0
        _nat_port=""
        ssh_wan "rm -f '${_P20_WAN_TCPDUMP_PID}' '${_P20_WAN_TCPDUMP_OUT}'; \
            nohup tcpdump -l -nn -i '${WAN_NIC}' -c 1 \
            'udp and src host ${_ppp_ip} and dst host ${WAN_IP} and dst port ${_remote_port}' \
            >'${_P20_WAN_TCPDUMP_OUT}' 2>&1 < /dev/null & echo \$! >'${_P20_WAN_TCPDUMP_PID}'" || true
        for _i in $(seq 1 5); do
            if ssh_wan "grep -q 'listening on' '${_P20_WAN_TCPDUMP_OUT}' 2>/dev/null"; then
                _capture_ready=1
                break
            fi
            sleep 1
        done
        if [[ $_capture_ready -ne 1 ]]; then
            _step79_issue="${_step79_issue} tcpdump=not_ready"
            break
        fi

        ssh_lan "rm -f '${_P20_LAN_LISTENER_PID}' '${_P20_LAN_LISTENER_OUT}' \
            '${_P20_LAN_LISTENER_READY}' '${_P20_LAN_LISTENER_RECV}';
nohup python3 -u - '${_listener_port}' '${WAN_IP}' '${_remote_port}' \
    '${_P20_LAN_LISTENER_READY}' '${_P20_LAN_LISTENER_RECV}' \
    >'${_P20_LAN_LISTENER_OUT}' 2>&1 <<'PY' &
import socket
import sys
import time

listen_port = int(sys.argv[1])
wan_ip = sys.argv[2]
remote_port = int(sys.argv[3])
ready_path = sys.argv[4]
recv_path = sys.argv[5]

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('0.0.0.0', listen_port))
sock.settimeout(0.5)
with open(ready_path, 'w', encoding='ascii') as ready:
    ready.write('ready\n')
sock.sendto(b'nat-expiry-seed', (wan_ip, remote_port))

deadline = time.monotonic() + 45
while time.monotonic() < deadline:
    try:
        payload, _peer = sock.recvfrom(2048)
    except socket.timeout:
        continue
    with open(recv_path, 'a', encoding='ascii') as received:
        received.write(payload.decode('ascii', errors='replace') + '\n')
PY
echo \$! >'${_P20_LAN_LISTENER_PID}'" || true
        for _i in $(seq 1 5); do
            if ssh_lan "test -s '${_P20_LAN_LISTENER_READY}'" 2>/dev/null; then
                _listener_ready=1
                break
            fi
            sleep 1
        done
        if [[ $_listener_ready -ne 1 ]]; then
            _step79_issue="${_step79_issue} listener=not_ready"
            break
        fi

        _ppp_ip_re=${_ppp_ip//./\\.}
        _wan_ip_re=${WAN_IP//./\\.}
        for _i in $(seq 1 12); do
            _cap=$(ssh_wan "cat '${_P20_WAN_TCPDUMP_OUT}' 2>/dev/null" || true)
            _nat_port=$(printf '%s\n' "$_cap" | sed -nE \
                "s/.*${_ppp_ip_re}\.([0-9]+) > ${_wan_ip_re}\.${_remote_port}:.*/\\1/p" | head -1)
            [[ "$_nat_port" =~ ^[0-9]+$ ]] && break
            sleep 1
        done
        _p20_stop_remote_pid ssh_wan "$_P20_WAN_TCPDUMP_PID" "Phase 20 tcpdump" && _tcpdump_stopped=1

        if [[ ! "$_nat_port" =~ ^[0-9]+$ ]]; then
            _step79_issue="${_step79_issue} nat_port=not_captured"
            break
        fi
        if [[ "$_nat_port" == "12345" ]]; then
            info "  NAT selected reserved port-forward eport 12345; retrying with a different tuple..."
            _p20_stop_remote_pid ssh_lan "$_P20_LAN_LISTENER_PID" "Phase 20 UDP listener" || true
            _listener_port=$(( _listener_port + 1 ))
            _remote_port=$(( _remote_port + 1 ))
            continue
        fi
        break
    done

    if [[ "$_nat_port" =~ ^[0-9]+$ && "$_nat_port" != "12345" ]]; then
        local _scapy_base="from scapy.all import Ether,IP,UDP,Raw,sendp"
        ssh_wan "python3 -c \"${_scapy_base}; sendp(Ether(dst='${FASTRG_NODE_WAN_MAC}',src='${WAN_HOST_MAC}')/IP(src='${WAN_IP}',dst='${_ppp_ip}',ttl=64,id=0x4079)/UDP(sport=${_remote_port},dport=${_nat_port})/Raw(load=b'${_positive_payload}'),iface='${WAN_NIC}',verbose=False)\"" || true
        for _i in $(seq 1 5); do
            if ssh_lan "grep -qx '${_positive_payload}' '${_P20_LAN_LISTENER_RECV}' 2>/dev/null"; then
                _positive_ok=1
                break
            fi
            sleep 1
        done

        # Stay silent for 15s (> 10s NAT timeout), polling listener health and
        # metrics so the wait remains bounded and observable despite GC jitter.
        for _i in $(seq 1 15); do
            if ! ssh_lan "_pid=\$(cat '${_P20_LAN_LISTENER_PID}' 2>/dev/null); \
                test -n \"\$_pid\" && kill -0 \"\$_pid\" 2>/dev/null"; then
                _listener_alive=0
                break
            fi
            _body=$(_p20_fetch_metrics)
            sleep 1
        done

        ssh_wan "python3 -c \"${_scapy_base}; sendp(Ether(dst='${FASTRG_NODE_WAN_MAC}',src='${WAN_HOST_MAC}')/IP(src='${WAN_IP}',dst='${_ppp_ip}',ttl=64,id=0x4080)/UDP(sport=${_remote_port},dport=${_nat_port})/Raw(load=b'${_negative_payload}'),iface='${WAN_NIC}',verbose=False)\"" || true
        for _i in $(seq 1 5); do
            sleep 1
            if ssh_lan "grep -qx '${_negative_payload}' '${_P20_LAN_LISTENER_RECV}' 2>/dev/null"; then
                _negative_seen=1
                break
            fi
            if ! ssh_lan "_pid=\$(cat '${_P20_LAN_LISTENER_PID}' 2>/dev/null); \
                test -n \"\$_pid\" && kill -0 \"\$_pid\" 2>/dev/null"; then
                _listener_alive=0
                break
            fi
        done
    fi

    _p20_stop_remote_pid ssh_lan "$_P20_LAN_LISTENER_PID" "Phase 20 UDP listener" && _listener_stopped=1
    if [[ $_positive_ok -eq 1 && $_negative_seen -eq 0 && $_listener_alive -eq 1 && \
          $_listener_stopped -eq 1 && $_tcpdump_stopped -eq 1 && -z "$_step79_issue" ]]; then
        pass "Step 81: expired UDP inbound rejected" \
            "NAT port=${_nat_port}; live tuple reached LAN:${_listener_port}; same tuple rejected after 15s idle"
    else
        fail "Step 81: expired UDP inbound rejected" \
            "${_step79_issue# } nat_port=${_nat_port:-NA}; positive=${_positive_ok}; expired_received=${_negative_seen}; listener_alive/stopped=${_listener_alive}/${_listener_stopped}; tcpdump_stopped=${_tcpdump_stopped}"
    fi

    _cleanup_phase20_nat_expiry
    return 0
}
