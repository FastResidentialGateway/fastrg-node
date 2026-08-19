#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 37 — IPv6 stateful firewall (Steps 155-169)
#
# IPv6 is routed, not translated, so without a firewall any host on the
# internet can address a LAN host directly. This phase checks the whole
# contract on real hardware: what the LAN asked for comes back, what it did
# not is dropped, forged packets aimed at a live session are rejected, an
# expired session cannot be reopened from the WAN side, and the table still
# performs and drains when it is nearly full.
#
# The bench state this phase touches (IPv6 enable flag, sysctls, WAN address
# and route, the TCP conntrack switch) is undone by
# _cleanup_phase37_ipv6_firewall, which also runs from the top-level EXIT trap.
# ---------------------------------------------------------------------------

# IPv6 defaults built into the dpdk-bras on this bench, same as phase35.
_P37_WAN6_HOST_ADDR="2001:db8:201::11"   # upstream host, past the BRAS
_P37_WAN6_GW="2001:db8:201::1"           # BRAS address on the upstream link
_P37_WAN6_PLEN=64
_P37_PD_ROUTE="2001:db8:6400::/48"       # delegation pool; needs a return route
_P37_LAN_VLAN="vlan3"                    # primary subscriber's LAN interface
_P37_ROUTER6="2001:db8:201::fe"          # stand-in for a router on the path

# Ports this phase owns, kept clear of the ones other phases use.
_P37_ECHO_PORT=40600        # WAN-side UDP echo service
_P37_LATE_PORT=40610        # WAN-side TCP server for the idle-connection check
_P37_REVIVE_REMOTE=40620    # WAN side of the expiry test flow
_P37_REVIVE_LISTEN=42600    # LAN side of the expiry test flow
_P37_SPRAY_SRC=44000
_P37_SPRAY_DST_BASE=6000
_P37_SPRAY_DST_COUNT=59000
_P37_FILL_TARGET=58000
_P37_DRAIN_TARGET=6554      # 10% of the 65536-session table
_P37_SWEEP_SEC=6            # under the 10s idle timeout, so sessions survive

# Bench state to undo.
_P37_IPV6_SET=0
_P37_LAN_SYSCTL_SAVED=""
_P37_WAN_SYSCTL_SAVED=""
_P37_WAN_ADDR_ADDED=0
_P37_WAN_ROUTE_ADDED=0
_P37_CONNTRACK_TOGGLED=0

# Remote scratch files.
_P37_CAP_PID=/tmp/e2e_fw6_tcpdump.pid
_P37_CAP_OUT=/tmp/e2e_fw6_tcpdump.out
_P37_WAN_SRV_PID=/tmp/e2e_fw6_wan_srv.pid
_P37_WAN_SRV_OUT=/tmp/e2e_fw6_wan_srv.out
_P37_LAN_IPERF_PID=/tmp/e2e_fw6_lan_iperf.pid
_P37_LAN_IPERF_OUT=/tmp/e2e_fw6_lan_iperf.out
_P37_LISTENER_PID=/tmp/e2e_fw6_listener.pid
_P37_LISTENER_OUT=/tmp/e2e_fw6_listener.out
_P37_LISTENER_READY=/tmp/e2e_fw6_listener.ready
_P37_LISTENER_RECV=/tmp/e2e_fw6_listener.recv
_P37_SPRAY_PID=/tmp/e2e_fw6_spray.pid
_P37_SPRAY_OUT=/tmp/e2e_fw6_spray.out

# Drill: point the capture at an interface the traffic never crosses. tcpdump
# still starts and still reports silence, so only the positive controls in
# Steps 158/159 can tell this apart from a firewall that really dropped
# everything.
_p37_inject_capture_wrong_interface() {
    _P37_LAN_VLAN=lo
}

_p37_cleanup_capture_wrong_interface() {
    restore_phase_functions phase37_ipv6_firewall.sh
}

case_validation_register fw6_capture_wrong_interface phase37_ipv6_firewall \
    _p37_inject_capture_wrong_interface _p37_cleanup_capture_wrong_interface \
    'Step 15[89]:'

# Drill: let the capture start normally, then kill it inside the window. It
# printed "listening on" before dying, so only the liveness check separates this
# from a window that really was silent.
_p37_inject_capture_dies() {
    sabotage_override_function _p37_start_capture \
        '_p37_start_capture_real "$@" || return 1
         ssh_lan "kill -9 \$(cat '"'"'${_P37_CAP_PID}'"'"') 2>/dev/null || true" >/dev/null 2>&1
         return 0'
}

_p37_cleanup_capture_dies() {
    restore_phase_functions phase37_ipv6_firewall.sh
}

case_validation_register fw6_capture_dies phase37_ipv6_firewall \
    _p37_inject_capture_dies _p37_cleanup_capture_dies \
    'Step 15[89]:'

# Values discovered at runtime.
_P37_PD_PREFIX=""
_P37_LAN6=""
_P37_REDIAL_STAGE=""
_P37_BASE_BPS=0
_P37_IPERF_ERR=""
_P37_PING_DETAIL=""

_p37_user_phase() {
    fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[]? | select(.user_id == ${1}) | .status // empty" \
        2>/dev/null || true
}

_p37_hsi_field() {
    fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[]? | select(.user_id == ${1}) | .${2} // empty" \
        2>/dev/null || true
}

# Hang up and dial again through the controller. IPV6CP only runs while a
# session is coming up, so switching IPv6 on needs a fresh session.
# _P37_REDIAL_STAGE describes what went wrong when this returns non-zero.
_p37_redial() {
    local _uid="$1" _i _phase=""

    _P37_REDIAL_STAGE=""
    fastrg_grpc disconnect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 15); do
        sleep 2
        _phase=$(_p37_user_phase "${_uid}" || true)
        [[ "$_phase" != "Data phase" ]] && break
    done
    [[ "$_phase" == "Data phase" ]] && \
        _P37_REDIAL_STAGE="session never dropped after hangup"

    fastrg_grpc connect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 30); do
        sleep 2
        _phase=$(_p37_user_phase "${_uid}" || true)
        [[ "$_phase" == "Data phase" ]] && return 0
    done
    _P37_REDIAL_STAGE="${_P37_REDIAL_STAGE:+${_P37_REDIAL_STAGE}; }did not return to Data phase (last='${_phase:-missing}')"
    return 1
}

# Whether a recorded remote pid is still running. A collector that died partway
# through the window leaves a short file that reads exactly like silence.
_p37_remote_pid_alive() {
    local _ssh_fn="$1" _pid_file="$2"

    "$_ssh_fn" "if [ -s '${_pid_file}' ]; then
            _pid=\$(cat '${_pid_file}' 2>/dev/null || true)
            [ -n \"\$_pid\" ] && kill -0 \"\$_pid\" 2>/dev/null
        else
            false
        fi" >/dev/null 2>&1
}

_p37_stop_remote() {
    local _ssh_fn="$1" _pid_file="$2" _label="$3"

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

# Idempotent: called at the end of phase37 and from the top-level EXIT trap.
_cleanup_phase37_ipv6_firewall() {
    _p37_stop_remote ssh_lan "$_P37_SPRAY_PID" "Phase 37 IPv6 sprayer" || true
    _p37_stop_remote ssh_lan "$_P37_LAN_IPERF_PID" "Phase 37 iperf3 client" || true
    _p37_stop_remote ssh_lan "$_P37_LISTENER_PID" "Phase 37 UDP listener" || true
    _p37_stop_remote ssh_lan "$_P37_CAP_PID" "Phase 37 tcpdump" || true
    _p37_stop_remote ssh_wan "$_P37_WAN_SRV_PID" "Phase 37 WAN helper server" || true
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" >/dev/null 2>&1 || true
    ssh_lan "rm -f '${_P37_SPRAY_PID}' '${_P37_SPRAY_OUT}' \
        '${_P37_LAN_IPERF_PID}' '${_P37_LAN_IPERF_OUT}' \
        '${_P37_LISTENER_PID}' '${_P37_LISTENER_OUT}' \
        '${_P37_LISTENER_READY}' '${_P37_LISTENER_RECV}' \
        '${_P37_CAP_PID}' '${_P37_CAP_OUT}'" >/dev/null 2>&1 || true
    ssh_wan "rm -f '${_P37_WAN_SRV_PID}' '${_P37_WAN_SRV_OUT}'" >/dev/null 2>&1 || true

    if [[ "${_P37_CONNTRACK_TOGGLED:-0}" -eq 1 ]]; then
        info "Cleanup(phase37): restoring tcp_conntrack for user ${USER_ID}..."
        fastrg_grpc set_tcp_conntrack "${USER_ID}" true >/dev/null 2>&1 || true
        _P37_CONNTRACK_TOGGLED=0
    fi

    if [[ -n "${_P37_LAN_SYSCTL_SAVED:-}" ]]; then
        info "Cleanup(phase37): restoring ${_P37_LAN_VLAN} IPv6 sysctl on the LAN host..."
        ssh_lan "sysctl -w net.ipv6.conf.${_P37_LAN_VLAN}.disable_ipv6=${_P37_LAN_SYSCTL_SAVED}" \
            >/dev/null 2>&1 || true
        _P37_LAN_SYSCTL_SAVED=""
    fi
    if [[ "${_P37_WAN_ROUTE_ADDED:-0}" -eq 1 ]]; then
        ssh_wan "ip -6 route del ${_P37_PD_ROUTE} via ${_P37_WAN6_GW} dev ${WAN_NIC}" \
            >/dev/null 2>&1 || true
        _P37_WAN_ROUTE_ADDED=0
    fi
    if [[ "${_P37_WAN_ADDR_ADDED:-0}" -eq 1 ]]; then
        ssh_wan "ip -6 addr del ${_P37_WAN6_HOST_ADDR}/${_P37_WAN6_PLEN} dev ${WAN_NIC}" \
            >/dev/null 2>&1 || true
        _P37_WAN_ADDR_ADDED=0
    fi
    if [[ -n "${_P37_WAN_SYSCTL_SAVED:-}" ]]; then
        info "Cleanup(phase37): restoring ${WAN_NIC} IPv6 sysctl on the WAN host..."
        ssh_wan "sysctl -w net.ipv6.conf.${WAN_NIC}.disable_ipv6=${_P37_WAN_SYSCTL_SAVED}" \
            >/dev/null 2>&1 || true
        _P37_WAN_SYSCTL_SAVED=""
    fi
    if [[ "${_P37_IPV6_SET:-0}" -eq 1 ]]; then
        info "Cleanup(phase37): switching ipv6_enable back off for user ${USER_ID}..."
        fastrg_grpc set_ipv6 "${USER_ID}" false >/dev/null 2>&1 || true
        _P37_IPV6_SET=0
    fi
    return 0
}

# One per-user metric of the primary subscriber. Empty when the scrape or the
# row is missing, which every caller treats as a failure rather than a zero.
_p37_metric() {
    local _body

    _body=$(e2e_metrics_body)
    e2e_metric_value "$_body" "$1" "user_id=${USER_ID}"
}

_p37_drop_count() {
    fastrg_grpc get_user_drop_count "${USER_ID}" 1 2>/dev/null | \
        jq -r '.dropped_packets // 0' 2>/dev/null | tr -d '[:space:]' || echo 0
}

# LAN to WAN ping6, anchored so only a literal zero loss field counts.
_p37_ping6_ok() {
    local _out

    _out=$(ssh_lan "ping -6 -c 4 -W 3 ${_P37_WAN6_HOST_ADDR} 2>&1" || true)
    _P37_PING_DETAIL=$(printf '%s' "$_out" | grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
    printf '%s' "$_out" | grep -qE '(^|[ ,])0(\.0+)?% packet loss'
}

# Best of two 5-second iperf3 -6 runs, in bits per second.
_p37_iperf6_bps() {
    local _i _out _bps _best=0

    _P37_IPERF_ERR=""
    for _i in 1 2; do
        _out=$(ssh_lan "iperf3 -6 -c ${_P37_WAN6_HOST_ADDR} -p ${SRV_PORT} -t 5 -J 2>&1" || true)
        if [[ -z "$_out" ]]; then
            _P37_IPERF_ERR="no output from the iperf3 client"
            continue
        fi
        _bps=$(printf '%s' "$_out" | jq -r '.end.sum_received.bits_per_second // 0' \
            2>/dev/null || echo 0)
        _bps=$(printf '%.0f' "${_bps}" 2>/dev/null || echo 0)
        if [[ "$_bps" -le 0 ]]; then
            _P37_IPERF_ERR=$(printf '%s' "$_out" | jq -r '.error // empty' 2>/dev/null || true)
            [[ -n "$_P37_IPERF_ERR" ]] || \
                _P37_IPERF_ERR=$(printf '%s' "$_out" | tr '\n' ' ' | cut -c 1-200 || true)
        elif [[ "$_bps" -gt "$_best" ]]; then
            _best=$_bps
        fi
        sleep 1
    done
    printf '%s' "$_best"
}

_p37_mbps() {
    awk "BEGIN {printf \"%.2f\", ${1:-0} / 1000000}"
}

# Start a capture on the subscriber's LAN interface. Returns non-zero when
# tcpdump never started, so a silent capture is never mistaken for silence.
_p37_start_capture() {
    local _filter="$1" _i

    ssh_lan "rm -f '${_P37_CAP_PID}' '${_P37_CAP_OUT}';
        nohup tcpdump -l -n -i '${_P37_LAN_VLAN}' ${_filter} \
        >'${_P37_CAP_OUT}' 2>&1 < /dev/null & echo \$! >'${_P37_CAP_PID}'" \
        >/dev/null 2>&1 || true
    for _i in $(seq 1 5); do
        if ssh_lan "grep -q 'listening on' '${_P37_CAP_OUT}' 2>/dev/null"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# Matching lines in the running capture. Prints nothing when the capture file
# is missing, which every caller treats as a failure rather than a zero.
_p37_capture_count() {
    ssh_lan "grep -c -- '$1' '${_P37_CAP_OUT}' 2>/dev/null || true" | \
        tr -d '[:space:]'
}

# Send one datagram to the WAN-side echo service from `sport` and report
# whether the reply came back. Prints the raw client output for diagnosis.
_p37_udp_echo() {
    local _sport="$1"

    ssh_lan "python3 - '${_P37_WAN6_HOST_ADDR}' '${_P37_ECHO_PORT}' '${_sport}' <<'PY' 2>&1
import socket
import sys

host, port, sport = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('::', sport))
sock.settimeout(5)
sock.sendto(b'fw6-echo', (host, port))
try:
    payload, _peer = sock.recvfrom(2048)
    print('echo-ok:%s' % payload.decode('ascii', errors='replace'))
except Exception as exc:
    print('echo-fail:%s' % exc)
PY" 2>&1 || true
}

# Run one scapy program on the WAN host. The caller passes the packet
# expression; rc and stderr come back so a broken injection is visible.
_p37_inject() {
    ssh_wan "python3 - <<'PY' 2>&1
from scapy.all import Ether, IPv6, TCP, UDP, ICMPv6PacketTooBig, sendp

eth = Ether(dst='${FASTRG_NODE_WAN_MAC}', src='${WAN_HOST_MAC}')
$1
PY" 2>&1 || true
}

# Start the IPv6 iperf3 server the throughput steps measure against.
_p37_start_iperf6_server() {
    local _i

    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" >/dev/null 2>&1 || true
    sleep 1
    ssh_wan "iperf3 -s -B ${_P37_WAN6_HOST_ADDR} -p ${SRV_PORT} -D --forceflush >/dev/null 2>&1 || true" \
        >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        sleep 1
        if ssh_wan "ss -ltn 2>/dev/null | grep -q ':${SRV_PORT}'" 2>/dev/null; then
            return 0
        fi
    done
    return 1
}

phase37_ipv6_firewall() {
    local _i _issue="" _ok=0
    local _etcd_val="" _flag="" _phase="" _uid=""
    local _addr_line="" _route_line="" _lan_addrs=""
    local _drop_before=0 _drop_after=0 _drop_delta=0
    local _pass_before="" _pass_after="" _drop6_before="" _drop6_after=""
    local _cap_count="" _out="" _sport="" _used="" _bps=0 _floor=0
    local _inject_out="" _spray_log=""
    local _positive_ok=0 _negative_seen=0 _listener_alive=1 _listener_ready=0

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 37 — IPv6 Stateful Firewall (Steps 155-169)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 155 — bring IPv6 up end to end again
    # ------------------------------------------------------------------
    info "Step 155: enabling IPv6, redialling and provisioning both hosts..."
    _issue=""
    _P37_IPV6_SET=1
    fastrg_grpc set_ipv6 "${USER_ID}" true >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        _etcd_val=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
        # No `// empty` here: it would swallow a stored false and read as absent.
        _flag=$(printf '%s' "$_etcd_val" | jq -r '.config.ipv6_enable' 2>/dev/null || true)
        [[ "$_flag" == "true" ]] && break
        sleep 1
    done
    [[ "$_flag" == "true" ]] || _issue="etcd ipv6_enable='${_flag:-missing}'"

    if ! _p37_redial "${USER_ID}"; then
        _issue="${_issue:+${_issue}; }redial failed: ${_P37_REDIAL_STAGE}"
    fi
    for _i in $(seq 1 15); do
        _P37_PD_PREFIX=$(_p37_hsi_field "${USER_ID}" 'ipv6_pd_prefix')
        [[ -n "$_P37_PD_PREFIX" ]] && break
        sleep 2
    done
    [[ -n "$_P37_PD_PREFIX" ]] || _issue="${_issue:+${_issue}; }no delegated prefix within 30s"

    # WAN host: its own address plus a route back into the delegated prefix.
    _P37_WAN_SYSCTL_SAVED=$(ssh_wan "sysctl -n net.ipv6.conf.${WAN_NIC}.disable_ipv6" \
        2>/dev/null | tr -d '[:space:]' || true)
    if [[ "$_P37_WAN_SYSCTL_SAVED" =~ ^[0-9]+$ ]]; then
        ssh_wan "sysctl -w net.ipv6.conf.${WAN_NIC}.disable_ipv6=0" >/dev/null 2>&1 || true
    else
        _P37_WAN_SYSCTL_SAVED=""
        _issue="${_issue:+${_issue}; }cannot read net.ipv6.conf.${WAN_NIC}.disable_ipv6"
    fi
    # The flags go up before the commands run: a partially applied change still
    # has to be undone by cleanup.
    _P37_WAN_ADDR_ADDED=1
    ssh_wan "ip -6 addr add ${_P37_WAN6_HOST_ADDR}/${_P37_WAN6_PLEN} dev ${WAN_NIC}" \
        >/dev/null 2>&1 || true
    _P37_WAN_ROUTE_ADDED=1
    ssh_wan "ip -6 route add ${_P37_PD_ROUTE} via ${_P37_WAN6_GW} dev ${WAN_NIC}" \
        >/dev/null 2>&1 || true
    _addr_line=$(ssh_wan "ip -6 -o addr show dev ${WAN_NIC}" 2>/dev/null | \
        grep -F "${_P37_WAN6_HOST_ADDR}/" || true)
    _route_line=$(ssh_wan "ip -6 route show ${_P37_PD_ROUTE}" 2>/dev/null | \
        grep -F "${_P37_WAN6_GW}" || true)
    [[ -n "$_addr_line" ]]  || _issue="${_issue:+${_issue}; }${_P37_WAN6_HOST_ADDR} not on ${WAN_NIC}"
    [[ -n "$_route_line" ]] || _issue="${_issue:+${_issue}; }no return route for ${_P37_PD_ROUTE}"

    # LAN host: SLAAC address from the advertised prefix.
    _P37_LAN_SYSCTL_SAVED=$(ssh_lan "sysctl -n net.ipv6.conf.${_P37_LAN_VLAN}.disable_ipv6" \
        2>/dev/null | tr -d '[:space:]' || true)
    if [[ "$_P37_LAN_SYSCTL_SAVED" =~ ^[0-9]+$ ]]; then
        ssh_lan "sysctl -w net.ipv6.conf.${_P37_LAN_VLAN}.disable_ipv6=0" >/dev/null 2>&1 || true
    else
        _P37_LAN_SYSCTL_SAVED=""
        _issue="${_issue:+${_issue}; }cannot read net.ipv6.conf.${_P37_LAN_VLAN}.disable_ipv6"
    fi
    for _i in $(seq 1 25); do
        _lan_addrs=$(ssh_lan "ip -6 -o addr show dev ${_P37_LAN_VLAN} scope global" 2>/dev/null | \
            grep -v tentative | awk '{print $4}' | cut -d/ -f1 | tr '\n' ' ' || true)
        [[ -n "${_lan_addrs// /}" ]] && break
        sleep 3
    done
    # Privacy extensions can leave several global addresses on the interface.
    # The one the kernel picks for traffic towards the WAN host is the one the
    # firewall will see, so that is the one the injections must target.
    _P37_LAN6=$(ssh_lan "ip -6 route get ${_P37_WAN6_HOST_ADDR} 2>/dev/null" 2>/dev/null | \
        sed -nE 's/.* src ([0-9a-fA-F:]+).*/\1/p' | head -1 || true)
    [[ -n "$_P37_LAN6" ]] || \
        _issue="${_issue:+${_issue}; }LAN host has no IPv6 source address towards ${_P37_WAN6_HOST_ADDR}"

    if [[ -z "$_issue" ]]; then
        pass "Step 155: IPv6 firewall fixture ready" \
            "delegated ${_P37_PD_PREFIX}, LAN host ${_P37_LAN6}, WAN host ${_P37_WAN6_HOST_ADDR}"
    else
        fail "Step 155: IPv6 firewall fixture ready" \
            "${_issue}; lan addrs='${_lan_addrs:-none}'"
    fi

    # ------------------------------------------------------------------
    # Step 156 — the path is alive, so later drops can be blamed on the firewall
    # ------------------------------------------------------------------
    info "Step 156: ping6 LAN→WAN to prove the path works at all..."
    _ok=0
    for _i in $(seq 1 6); do
        if _p37_ping6_ok; then
            _ok=1
            break
        fi
        sleep 5
    done
    if [[ $_ok -eq 1 ]]; then
        pass "Step 156: LAN→WAN ping6 works" \
            "${_P37_WAN6_HOST_ADDR} reachable with 0% packet loss"
    else
        fail "Step 156: LAN→WAN ping6 works" \
            "${_P37_WAN6_HOST_ADDR} - ${_P37_PING_DETAIL:-no response}; route: $(ssh_lan "ip -6 route get ${_P37_WAN6_HOST_ADDR}" 2>/dev/null | tr '\n' ' ' | cut -c 1-200 || true)"
    fi

    # ------------------------------------------------------------------
    # Step 157 — a reply to something the LAN sent is allowed back in
    # ------------------------------------------------------------------
    info "Step 157: LAN-initiated UDP must get its reply back..."
    ssh_wan "rm -f '${_P37_WAN_SRV_PID}' '${_P37_WAN_SRV_OUT}';
nohup python3 -u - '${_P37_WAN6_HOST_ADDR}' '${_P37_ECHO_PORT}' \
    >'${_P37_WAN_SRV_OUT}' 2>&1 <<'PY' &
import socket
import sys

sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind((sys.argv[1], int(sys.argv[2])))
print('echo server ready', flush=True)
while True:
    payload, peer = sock.recvfrom(2048)
    sock.sendto(payload, peer)
PY
echo \$! >'${_P37_WAN_SRV_PID}'" >/dev/null 2>&1 || true
    _ok=0
    for _i in $(seq 1 5); do
        if ssh_wan "grep -q 'echo server ready' '${_P37_WAN_SRV_OUT}' 2>/dev/null"; then
            _ok=1
            break
        fi
        sleep 1
    done

    _out=""
    [[ $_ok -eq 1 ]] && _out=$(_p37_udp_echo 41600)
    if [[ $_ok -eq 1 ]] && printf '%s' "$_out" | grep -q 'echo-ok'; then
        pass "Step 157: LAN-initiated UDP gets its reply" \
            "datagram from ${_P37_LAN6}:41600 echoed back through the firewall"
    else
        fail "Step 157: LAN-initiated UDP gets its reply" \
            "server_ready=${_ok}; client: $(printf '%s' "$_out" | tr '\n' ' ' | cut -c 1-200 || true); server: $(ssh_wan "tail -n 3 '${_P37_WAN_SRV_OUT}' 2>/dev/null" 2>/dev/null | tr '\n' ' ' | cut -c 1-200 || true)"
    fi

    # ------------------------------------------------------------------
    # Step 158 — an unsolicited inbound ping never reaches the LAN
    # ------------------------------------------------------------------
    info "Step 158: unsolicited inbound ICMPv6 echo must be dropped..."
    _issue=""
    if ! _p37_start_capture "'icmp6'"; then
        _issue="tcpdump did not start on ${_P37_LAN_VLAN}; a silent capture proves nothing"
    fi
    # Control, in this same capture: soliciting an unused link-local neighbour
    # puts a packet on the segment that the node never sees, so it cannot
    # perturb what is being tested while still proving the capture was awake.
    ssh_lan "ping -6 -c 1 -W 1 -I ${_P37_LAN_VLAN} fe80::dead:beef >/dev/null 2>&1 || true" \
        2>/dev/null || true
    _out=$(ssh_wan "ping -6 -c 4 -W 2 ${_P37_LAN6} 2>&1" || true)
    sleep 2
    _p37_remote_pid_alive ssh_lan "$_P37_CAP_PID" || \
        _issue="${_issue:+${_issue}; }tcpdump died mid-capture on ${_P37_LAN_VLAN}; the silent window proves nothing"
    _p37_stop_remote ssh_lan "$_P37_CAP_PID" "Phase 37 tcpdump" || \
        _issue="${_issue:+${_issue}; }tcpdump did not stop"
    _control_count=$(_p37_capture_count 'neighbor solicitation')
    _cap_count=$(_p37_capture_count 'echo request')

    [[ "$_control_count" =~ ^[1-9] ]] || \
        _issue="${_issue:+${_issue}; }no neighbour solicitation in the capture (control='${_control_count:-missing}'); the capture saw nothing at all"

    printf '%s' "$_out" | grep -q '100% packet loss' || \
        _issue="${_issue:+${_issue}; }WAN host saw a reply ($(printf '%s' "$_out" | grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true))"
    [[ "$_cap_count" == "0" ]] || \
        _issue="${_issue:+${_issue}; }${_cap_count} echo request(s) reached ${_P37_LAN_VLAN}"

    if [[ -z "$_issue" ]]; then
        pass "Step 158: unsolicited inbound ping is dropped" \
            "WAN→${_P37_LAN6} 100% loss and no echo request on ${_P37_LAN_VLAN}"
    else
        fail "Step 158: unsolicited inbound ping is dropped" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 159 — an unsolicited inbound TCP SYN never reaches the LAN
    # ------------------------------------------------------------------
    info "Step 159: unsolicited inbound TCP SYN must be dropped..."
    _issue=""
    if ! _p37_start_capture "'ip6 and tcp'"; then
        _issue="tcpdump did not start on ${_P37_LAN_VLAN}; a silent capture proves nothing"
    fi
    # Control, in this same capture: a LAN-initiated SYN to an unrelated port.
    # It opens state for that port only, so it cannot admit the inbound SYN
    # under test, and every SYN in the window is accounted for below.
    ssh_lan "python3 - '${_P37_WAN6_HOST_ADDR}' <<'PY' >/dev/null 2>&1 || true
import socket
import sys

sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
sock.settimeout(2)
try:
    sock.connect((sys.argv[1], 9))
except Exception:
    pass
PY" 2>/dev/null || true
    _out=$(ssh_wan "python3 - '${_P37_LAN6}' <<'PY' 2>&1
import socket
import sys

sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
sock.settimeout(5)
try:
    sock.connect((sys.argv[1], 22))
    print('connect-ok')
except Exception as exc:
    print('connect-fail:%s' % exc)
PY" 2>&1 || true)
    sleep 1
    _p37_remote_pid_alive ssh_lan "$_P37_CAP_PID" || \
        _issue="${_issue:+${_issue}; }tcpdump died mid-capture on ${_P37_LAN_VLAN}; the silent window proves nothing"
    _p37_stop_remote ssh_lan "$_P37_CAP_PID" "Phase 37 tcpdump" || \
        _issue="${_issue:+${_issue}; }tcpdump did not stop"
    _control_count=$(_p37_capture_count '\.9: Flags \[S\]')
    _cap_count=$(_p37_capture_count 'Flags \[S\]')

    printf '%s' "$_out" | grep -q 'connect-fail' || \
        _issue="${_issue:+${_issue}; }WAN host connected (${_out})"
    [[ "$_control_count" =~ ^[1-9] ]] || \
        _issue="${_issue:+${_issue}; }no control SYN in the capture (control='${_control_count:-missing}'); the capture saw nothing at all"
    # Every SYN in the window must be one of the control SYNs; anything above
    # that count is an inbound SYN that got through.
    [[ "$_cap_count" == "$_control_count" ]] || \
        _issue="${_issue:+${_issue}; }${_cap_count} SYN(s) on ${_P37_LAN_VLAN} but only ${_control_count} control SYN(s); an unsolicited SYN reached the LAN"

    if [[ -z "$_issue" ]]; then
        pass "Step 159: unsolicited inbound TCP SYN is dropped" \
            "connect to [${_P37_LAN6}]:22 failed and no SYN on ${_P37_LAN_VLAN}"
    else
        fail "Step 159: unsolicited inbound TCP SYN is dropped" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 160 — an idle TCP connection outlives the flat 10 second timeout
    # ------------------------------------------------------------------
    # A session tracked on the flat UDP timeout would be gone after 10s of
    # silence and the server's late line would be dropped; the conntrack
    # per-state timeout keeps an established connection for 7200s.
    info "Step 160: an idle TCP connection must survive 15s of silence..."
    _issue=""
    ssh_wan "rm -f '${_P37_WAN_SRV_PID}' '${_P37_WAN_SRV_OUT}';
nohup python3 -u - '${_P37_WAN6_HOST_ADDR}' '${_P37_LATE_PORT}' \
    >'${_P37_WAN_SRV_OUT}' 2>&1 <<'PY' &
import socket
import sys
import time

srv = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((sys.argv[1], int(sys.argv[2])))
srv.listen(1)
print('late server ready', flush=True)
conn, _peer = srv.accept()
print('accepted', flush=True)
time.sleep(15)
conn.sendall(b'late-data\n')
print('sent late data', flush=True)
time.sleep(5)
conn.close()
PY
echo \$! >'${_P37_WAN_SRV_PID}'" >/dev/null 2>&1 || true
    _ok=0
    for _i in $(seq 1 5); do
        if ssh_wan "grep -q 'late server ready' '${_P37_WAN_SRV_OUT}' 2>/dev/null"; then
            _ok=1
            break
        fi
        sleep 1
    done
    [[ $_ok -eq 1 ]] || _issue="WAN-side TCP server never listened on ${_P37_LATE_PORT}"

    _out=""
    if [[ -z "$_issue" ]]; then
        _out=$(ssh_lan "timeout 40 python3 - '${_P37_WAN6_HOST_ADDR}' '${_P37_LATE_PORT}' <<'PY' 2>&1
import socket
import sys

sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
sock.settimeout(30)
sock.connect((sys.argv[1], int(sys.argv[2])))
try:
    payload = sock.recv(64)
    print('late-ok:%s' % payload.decode('ascii', errors='replace').strip())
except Exception as exc:
    print('late-fail:%s' % exc)
PY" 2>&1 || true)
        printf '%s' "$_out" | grep -q 'late-ok' || \
            _issue="LAN client did not receive the late data ($(printf '%s' "$_out" | tr '\n' ' ' | cut -c 1-200 || true))"
    fi
    _p37_stop_remote ssh_wan "$_P37_WAN_SRV_PID" "Phase 37 WAN helper server" || true

    if [[ -z "$_issue" ]]; then
        pass "Step 160: idle TCP session survives" \
            "server data after 15s of silence still reached the LAN client"
    else
        fail "Step 160: idle TCP session survives" \
            "${_issue}; server: $(ssh_wan "tail -n 3 '${_P37_WAN_SRV_OUT}' 2>/dev/null" 2>/dev/null | tr '\n' ' ' | cut -c 1-200 || true)"
    fi

    # ------------------------------------------------------------------
    # Step 161 — forged packets aimed at a live TCP session are dropped
    # ------------------------------------------------------------------
    info "Step 161: injecting state-mismatch and out-of-window TCP from the WAN..."
    _issue=""
    if ! _p37_start_iperf6_server; then
        _issue="iperf3 server never listened on ${SRV_PORT}"
    fi
    ssh_lan "rm -f '${_P37_LAN_IPERF_PID}' '${_P37_LAN_IPERF_OUT}';
        nohup iperf3 -6 -c '${_P37_WAN6_HOST_ADDR}' -p '${SRV_PORT}' -t 60 -i 1 --forceflush \
        >'${_P37_LAN_IPERF_OUT}' 2>&1 < /dev/null & echo \$! >'${_P37_LAN_IPERF_PID}'" \
        >/dev/null 2>&1 || true
    _sport=""
    for _i in $(seq 1 12); do
        sleep 1
        _sport=$(ssh_lan "ss -6 -tn 2>/dev/null | grep -F ']:${SRV_PORT}'" 2>/dev/null | \
            awk '{print $4}' | sed -E 's/.*\]:([0-9]+)$/\1/' | head -1 || true)
        [[ "$_sport" =~ ^[0-9]+$ ]] && break
    done
    [[ "$_sport" =~ ^[0-9]+$ ]] || _issue="${_issue:+${_issue}; }could not read the LAN source port of the iperf3 flow"

    # Control first: a forged Packet Too Big quoting this very live session has
    # to be accepted. It moves a counter only the firewall touches, so it
    # proves the injected frames really reach the firewall — without it a drop
    # could just mean the node discarded an unrecognised frame.
    if [[ -z "$_issue" ]]; then
        _pass_before=$(_p37_metric "fastrg_node_per_user_ipv6_firewall_icmp6_err_passed_total")
        _inject_out=$(_p37_inject "sendp(eth / IPv6(src='${_P37_ROUTER6}', dst='${_P37_LAN6}', hlim=64) / ICMPv6PacketTooBig(mtu=1280) / IPv6(src='${_P37_LAN6}', dst='${_P37_WAN6_HOST_ADDR}') / TCP(sport=${_sport}, dport=${SRV_PORT}), iface='${WAN_NIC}', verbose=0)")
        sleep 2
        _pass_after=$(_p37_metric "fastrg_node_per_user_ipv6_firewall_icmp6_err_passed_total")
        if ! e2e_all_uint "$_pass_before" "$_pass_after" || \
           (( _pass_after - _pass_before < 1 )); then
            _issue="injection never reached the firewall (icmp6_err_passed ${_pass_before:-missing}→${_pass_after:-missing}); scapy: $(printf '%s' "$_inject_out" | tr '\n' ' ' | cut -c 1-200 || true)"
        fi
    fi

    if [[ -z "$_issue" ]]; then
        _drop_before=$(_p37_drop_count)
        _inject_out=$(_p37_inject "sendp(eth / IPv6(src='${_P37_WAN6_HOST_ADDR}', dst='${_P37_LAN6}', hlim=64) / TCP(sport=${SRV_PORT}, dport=${_sport}, flags='S', seq=0x12345678), iface='${WAN_NIC}', verbose=0)")
        sleep 2
        _drop_after=$(_p37_drop_count)
        _drop_delta=$(( _drop_after - _drop_before ))
        (( _drop_delta >= 1 )) || \
            _issue="state-mismatch SYN was not dropped (delta=${_drop_delta}); scapy: $(printf '%s' "$_inject_out" | tr '\n' ' ' | cut -c 1-200 || true)"
    fi

    if [[ -z "$_issue" ]]; then
        _drop_before=$(_p37_drop_count)
        _inject_out=$(_p37_inject "sendp(eth / IPv6(src='${_P37_WAN6_HOST_ADDR}', dst='${_P37_LAN6}', hlim=64) / TCP(sport=${SRV_PORT}, dport=${_sport}, flags='A', seq=0x40000000, ack=0xCAFEBABE), iface='${WAN_NIC}', verbose=0)")
        sleep 2
        _drop_after=$(_p37_drop_count)
        _drop_delta=$(( _drop_after - _drop_before ))
        (( _drop_delta >= 1 )) || \
            _issue="out-of-window ACK was not dropped (delta=${_drop_delta}); scapy: $(printf '%s' "$_inject_out" | tr '\n' ' ' | cut -c 1-200 || true)"
    fi

    if [[ -z "$_issue" ]]; then
        pass "Step 161: forged TCP against a live IPv6 session is dropped" \
            "state-mismatch SYN and out-of-window ACK on [${_P37_LAN6}]:${_sport} both dropped"
    else
        fail "Step 161: forged TCP against a live IPv6 session is dropped" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 162 — tcp_conntrack_enabled governs IPv6 exactly as it does IPv4
    # ------------------------------------------------------------------
    info "Step 162: checking the tcp_conntrack switch on the IPv6 path..."
    _issue=""
    if [[ ! "$_sport" =~ ^[0-9]+$ ]]; then
        _issue="no live IPv6 TCP session to inject against"
    else
        _P37_CONNTRACK_TOGGLED=1
        fastrg_grpc set_tcp_conntrack "${USER_ID}" false >/dev/null 2>&1 || true
        sleep 1
        if ! _p37_start_capture "'ip6 and tcp and port ${_sport}'"; then
            _issue="tcpdump did not start on ${_P37_LAN_VLAN}; a silent capture proves nothing"
        fi
        _inject_out=$(_p37_inject "sendp(eth / IPv6(src='${_P37_WAN6_HOST_ADDR}', dst='${_P37_LAN6}', hlim=64) / TCP(sport=${SRV_PORT}, dport=${_sport}, flags='S', seq=0x12345678), iface='${WAN_NIC}', verbose=0)")
        sleep 3
        _p37_stop_remote ssh_lan "$_P37_CAP_PID" "Phase 37 tcpdump" || \
            _issue="${_issue:+${_issue}; }tcpdump did not stop"
        _cap_count=$(_p37_capture_count 'Flags \[S\]')
        if [[ -z "$_issue" ]] && [[ ! "$_cap_count" =~ ^[1-9] ]]; then
            _issue="with conntrack off the SYN did not reach ${_P37_LAN_VLAN} (count=${_cap_count}); scapy: $(printf '%s' "$_inject_out" | tr '\n' ' ' | cut -c 1-200 || true)"
        fi

        fastrg_grpc set_tcp_conntrack "${USER_ID}" true >/dev/null 2>&1 || true
        _P37_CONNTRACK_TOGGLED=0
        sleep 1
        _drop_before=$(_p37_drop_count)
        _inject_out=$(_p37_inject "sendp(eth / IPv6(src='${_P37_WAN6_HOST_ADDR}', dst='${_P37_LAN6}', hlim=64) / TCP(sport=${SRV_PORT}, dport=${_sport}, flags='S', seq=0x12345678), iface='${WAN_NIC}', verbose=0, count=2)")
        sleep 2
        _drop_after=$(_p37_drop_count)
        _drop_delta=$(( _drop_after - _drop_before ))
        (( _drop_delta >= 2 )) || \
            _issue="${_issue:+${_issue}; }with conntrack on the SYNs were not dropped (delta=${_drop_delta}); scapy: $(printf '%s' "$_inject_out" | tr '\n' ' ' | cut -c 1-200 || true)"
    fi

    if [[ -z "$_issue" ]]; then
        pass "Step 162: the tcp_conntrack switch governs IPv6 too" \
            "SYN forwarded to ${_P37_LAN_VLAN} while off, dropped again once back on"
    else
        fail "Step 162: the tcp_conntrack switch governs IPv6 too" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 163 — an ICMPv6 error is judged by the packet it quotes
    # ------------------------------------------------------------------
    # Letting these through is what keeps path MTU discovery working; letting
    # forged ones through would let anybody who knows a LAN address push a host
    # down to the minimum MTU. Both counters make each verdict attributable.
    info "Step 163: forged and genuine ICMPv6 Packet Too Big..."
    _issue=""
    _out=$(_p37_udp_echo 41620)
    printf '%s' "$_out" | grep -q 'echo-ok' || \
        _issue="could not open the UDP session to quote ($(printf '%s' "$_out" | tr '\n' ' ' | cut -c 1-160 || true))"

    if [[ -z "$_issue" ]]; then
        _pass_before=$(_p37_metric "fastrg_node_per_user_ipv6_firewall_icmp6_err_passed_total")
        _inject_out=$(_p37_inject "sendp(eth / IPv6(src='${_P37_ROUTER6}', dst='${_P37_LAN6}', hlim=64) / ICMPv6PacketTooBig(mtu=1280) / IPv6(src='${_P37_LAN6}', dst='${_P37_WAN6_HOST_ADDR}') / UDP(sport=41620, dport=${_P37_ECHO_PORT}), iface='${WAN_NIC}', verbose=0)")
        sleep 2
        _pass_after=$(_p37_metric "fastrg_node_per_user_ipv6_firewall_icmp6_err_passed_total")
        if ! e2e_all_uint "$_pass_before" "$_pass_after" || \
           (( _pass_after - _pass_before < 1 )); then
            _issue="Packet Too Big quoting the live session was not accepted (passed ${_pass_before:-missing}→${_pass_after:-missing}); scapy: $(printf '%s' "$_inject_out" | tr '\n' ' ' | cut -c 1-200 || true)"
        fi
    fi

    if [[ -z "$_issue" ]]; then
        _drop6_before=$(_p37_metric "fastrg_node_per_user_ipv6_firewall_icmp6_err_dropped_total")
        _drop_before=$(_p37_drop_count)
        _inject_out=$(_p37_inject "sendp(eth / IPv6(src='${_P37_ROUTER6}', dst='${_P37_LAN6}', hlim=64) / ICMPv6PacketTooBig(mtu=1280) / IPv6(src='${_P37_LAN6}', dst='${_P37_WAN6_HOST_ADDR}') / UDP(sport=41621, dport=${_P37_ECHO_PORT}), iface='${WAN_NIC}', verbose=0)")
        sleep 2
        _drop6_after=$(_p37_metric "fastrg_node_per_user_ipv6_firewall_icmp6_err_dropped_total")
        _drop_after=$(_p37_drop_count)
        _drop_delta=$(( _drop_after - _drop_before ))
        if ! e2e_all_uint "$_drop6_before" "$_drop6_after" || \
           (( _drop6_after - _drop6_before < 1 )); then
            _issue="Packet Too Big quoting an unknown tuple was not rejected (dropped ${_drop6_before:-missing}→${_drop6_after:-missing}); scapy: $(printf '%s' "$_inject_out" | tr '\n' ' ' | cut -c 1-200 || true)"
        elif (( _drop_delta < 1 )); then
            _issue="the rejected error was not counted as a WAN drop (delta=${_drop_delta})"
        fi
    fi

    if [[ -z "$_issue" ]]; then
        pass "Step 163: ICMPv6 errors are judged by what they quote" \
            "the one quoting the live session passed, the forged one was dropped"
    else
        fail "Step 163: ICMPv6 errors are judged by what they quote" "$_issue"
    fi

    # Release the iperf3 flow that steps 160 and 161 injected against.
    _p37_stop_remote ssh_lan "$_P37_LAN_IPERF_PID" "Phase 37 iperf3 client" || true
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" >/dev/null 2>&1 || true

    # ------------------------------------------------------------------
    # Step 164 — an expired session cannot be reopened from the WAN side
    # ------------------------------------------------------------------
    info "Step 164: an expired IPv6 session must not come back from the WAN..."
    _issue=""
    ssh_lan "rm -f '${_P37_LISTENER_PID}' '${_P37_LISTENER_OUT}' \
        '${_P37_LISTENER_READY}' '${_P37_LISTENER_RECV}';
nohup python3 -u - '${_P37_REVIVE_LISTEN}' '${_P37_WAN6_HOST_ADDR}' '${_P37_REVIVE_REMOTE}' \
    '${_P37_LISTENER_READY}' '${_P37_LISTENER_RECV}' \
    >'${_P37_LISTENER_OUT}' 2>&1 <<'PY' &
import socket
import sys
import time

listen_port = int(sys.argv[1])
wan6 = sys.argv[2]
remote_port = int(sys.argv[3])
ready_path = sys.argv[4]
recv_path = sys.argv[5]

sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('::', listen_port))
sock.settimeout(0.5)
with open(ready_path, 'w', encoding='ascii') as ready:
    ready.write('ready\n')
sock.sendto(b'fw6-expiry-seed', (wan6, remote_port))

deadline = time.monotonic() + 45
while time.monotonic() < deadline:
    try:
        payload, _peer = sock.recvfrom(2048)
    except socket.timeout:
        continue
    with open(recv_path, 'a', encoding='ascii') as received:
        received.write(payload.decode('ascii', errors='replace') + '\n')
PY
echo \$! >'${_P37_LISTENER_PID}'" >/dev/null 2>&1 || true
    for _i in $(seq 1 5); do
        if ssh_lan "test -s '${_P37_LISTENER_READY}'" 2>/dev/null; then
            _listener_ready=1
            break
        fi
        sleep 1
    done
    [[ $_listener_ready -eq 1 ]] || _issue="LAN listener never became ready"

    if [[ -z "$_issue" ]]; then
        _inject_out=$(_p37_inject "sendp(eth / IPv6(src='${_P37_WAN6_HOST_ADDR}', dst='${_P37_LAN6}', hlim=64) / UDP(sport=${_P37_REVIVE_REMOTE}, dport=${_P37_REVIVE_LISTEN}) / b'fw6-expiry-positive', iface='${WAN_NIC}', verbose=0)")
        for _i in $(seq 1 5); do
            if ssh_lan "grep -qx 'fw6-expiry-positive' '${_P37_LISTENER_RECV}' 2>/dev/null"; then
                _positive_ok=1
                break
            fi
            sleep 1
        done
        [[ $_positive_ok -eq 1 ]] || \
            _issue="the live tuple never reached the listener; scapy: $(printf '%s' "$_inject_out" | tr '\n' ' ' | cut -c 1-200 || true)"
    fi

    if [[ -z "$_issue" ]]; then
        # Stay silent past the 10s idle timeout, polling listener health so the
        # wait stays bounded and a dead listener is not read as a pass.
        for _i in $(seq 1 15); do
            sleep 1
            if ! ssh_lan "_pid=\$(cat '${_P37_LISTENER_PID}' 2>/dev/null); \
                test -n \"\$_pid\" && kill -0 \"\$_pid\" 2>/dev/null"; then
                _listener_alive=0
                break
            fi
        done
        [[ $_listener_alive -eq 1 ]] || _issue="the listener died during the idle window"
    fi

    if [[ -z "$_issue" ]]; then
        _inject_out=$(_p37_inject "sendp(eth / IPv6(src='${_P37_WAN6_HOST_ADDR}', dst='${_P37_LAN6}', hlim=64) / UDP(sport=${_P37_REVIVE_REMOTE}, dport=${_P37_REVIVE_LISTEN}) / b'fw6-expiry-expired', iface='${WAN_NIC}', verbose=0)")
        for _i in $(seq 1 5); do
            sleep 1
            if ssh_lan "grep -qx 'fw6-expiry-expired' '${_P37_LISTENER_RECV}' 2>/dev/null"; then
                _negative_seen=1
                break
            fi
        done
        [[ $_negative_seen -eq 0 ]] || \
            _issue="the same tuple still reached the listener after the session expired"
    fi
    _p37_stop_remote ssh_lan "$_P37_LISTENER_PID" "Phase 37 UDP listener" || \
        _issue="${_issue:+${_issue}; }listener did not stop"

    if [[ -z "$_issue" ]]; then
        pass "Step 164: an expired session stays closed" \
            "the live tuple reached [${_P37_LAN6}]:${_P37_REVIVE_LISTEN}, the same tuple was dropped after 15s idle"
    else
        fail "Step 164: an expired session stays closed" \
            "${_issue}; positive=${_positive_ok} negative=${_negative_seen} listener_alive=${_listener_alive}"
    fi

    # ------------------------------------------------------------------
    # Step 165 — idle IPv6 throughput baseline
    # ------------------------------------------------------------------
    info "Step 165: measuring the idle IPv6 throughput baseline..."
    _issue=""
    _p37_start_iperf6_server || _issue="iperf3 server never listened on ${SRV_PORT}"
    _P37_BASE_BPS=$(_p37_iperf6_bps)
    [[ "$_P37_BASE_BPS" -gt 0 ]] || \
        _issue="${_issue:+${_issue}; }no baseline throughput (${_P37_IPERF_ERR:-unknown})"

    if [[ -z "$_issue" ]]; then
        pass "Step 165: IPv6 throughput baseline" \
            "$(_p37_mbps "$_P37_BASE_BPS") Mbps over IPv6 with an idle session table"
    else
        fail "Step 165: IPv6 throughput baseline" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 166 — fill the session table to about 90%
    # ------------------------------------------------------------------
    info "Step 166: filling the IPv6 session table with ~59000 UDP flows..."
    _issue=""
    _ok=0
    ssh_lan "rm -f '${_P37_SPRAY_PID}' '${_P37_SPRAY_OUT}';
nohup python3 -u - '${_P37_WAN6_HOST_ADDR}' '${_P37_SPRAY_SRC}' '${_P37_SPRAY_DST_BASE}' '${_P37_SPRAY_DST_COUNT}' '${_P37_SWEEP_SEC}' \
    >'${_P37_SPRAY_OUT}' 2>&1 <<'PY' &
import socket
import sys
import time

wan6 = sys.argv[1]
src_port = int(sys.argv[2])
dst_base, dst_count = int(sys.argv[3]), int(sys.argv[4])
sweep_sec = float(sys.argv[5])

sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('::', src_port))
print('ready on port %d' % src_port, flush=True)

payload = b'fw6-capacity'
while True:
    started = time.monotonic()
    sent = 0
    for port in range(dst_base, dst_base + dst_count):
        try:
            sock.sendto(payload, (wan6, port))
            sent += 1
        except OSError:
            time.sleep(0.001)
    print('sweep sent %d in %.1fs' % (sent, time.monotonic() - started), flush=True)
    idle = sweep_sec - (time.monotonic() - started)
    if idle > 0:
        time.sleep(idle)
PY
echo \$! >'${_P37_SPRAY_PID}'" >/dev/null 2>&1 || true
    for _i in $(seq 1 5); do
        if ssh_lan "grep -q 'ready on port' '${_P37_SPRAY_OUT}' 2>/dev/null"; then
            _ok=1
            break
        fi
        sleep 1
    done
    [[ $_ok -eq 1 ]] || _issue="sprayer never reported ready"

    _ok=0
    for _i in $(seq 1 8); do
        sleep 5
        _used=$(_p37_metric "fastrg_node_per_user_ipv6_firewall_entries_used")
        if e2e_is_uint "$_used" && (( _used >= _P37_FILL_TARGET )); then
            _ok=1
            break
        fi
    done

    if [[ $_ok -eq 1 && -z "$_issue" ]]; then
        pass "Step 166: IPv6 session table filled to near capacity" \
            "ipv6_firewall_entries_used=${_used} (target ≥ ${_P37_FILL_TARGET})"
    else
        _spray_log=$(ssh_lan "tail -n 3 '${_P37_SPRAY_OUT}' 2>/dev/null" 2>/dev/null | tr '\n' ' ' | cut -c 1-300 || true)
        fail "Step 166: IPv6 session table filled to near capacity" \
            "${_issue:+${_issue}; }ipv6_firewall_entries_used='${_used:-missing}' target=${_P37_FILL_TARGET}; sprayer: ${_spray_log:-no output}"
    fi

    # ------------------------------------------------------------------
    # Step 167 — throughput and new sessions while the table stays full
    # ------------------------------------------------------------------
    # No tcpdump in this step: capturing on the LAN host is known to add packet
    # loss on this bench and would be measured as degradation.
    info "Step 167: measuring IPv6 throughput while the table stays near capacity..."
    _issue=""
    _p37_ping6_ok || _issue="LAN→WAN ping6 ${_P37_PING_DETAIL:-no response} under load"

    _bps=$(_p37_iperf6_bps)
    _floor=$(( _P37_BASE_BPS / 2 ))
    if [[ "$_bps" -le 0 ]]; then
        _issue="${_issue:+${_issue}; }no throughput under load (${_P37_IPERF_ERR:-unknown})"
    elif (( _bps < _floor )); then
        _issue="${_issue:+${_issue}; }throughput $(_p37_mbps "$_bps") Mbps is below half the baseline $(_p37_mbps "$_P37_BASE_BPS") Mbps"
    fi

    # A brand new flow on an unused port has to open a session and get its
    # reply back even with the table nearly full.
    _out=$(_p37_udp_echo 41660)
    printf '%s' "$_out" | grep -q 'echo-ok' || \
        _issue="${_issue:+${_issue}; }a new UDP flow got no reply under load ($(printf '%s' "$_out" | tr '\n' ' ' | cut -c 1-160 || true))"
    _used=$(_p37_metric "fastrg_node_per_user_ipv6_firewall_entries_used")

    if [[ -z "$_issue" ]]; then
        pass "Step 167: the firewall holds up near capacity" \
            "$(_p37_mbps "$_bps") Mbps of $(_p37_mbps "$_P37_BASE_BPS") Mbps baseline with ${_used} live sessions, new flows still open"
    else
        fail "Step 167: the firewall holds up near capacity" \
            "${_issue}; ipv6_firewall_entries_used='${_used:-missing}'"
    fi

    # ------------------------------------------------------------------
    # Step 168 — stop the sprayer and watch the GC drain the table
    # ------------------------------------------------------------------
    info "Step 168: stopping the sprayer and waiting for the table to drain..."
    _issue=""
    _p37_stop_remote ssh_lan "$_P37_SPRAY_PID" "Phase 37 IPv6 sprayer" || \
        _issue="sprayer did not stop"

    _ok=0
    for _i in $(seq 1 18); do
        sleep 5
        _used=$(_p37_metric "fastrg_node_per_user_ipv6_firewall_entries_used")
        if e2e_is_uint "$_used" && (( _used < _P37_DRAIN_TARGET )); then
            _ok=1
            break
        fi
    done
    [[ $_ok -eq 1 ]] || \
        _issue="${_issue:+${_issue}; }table still holds '${_used:-missing}' sessions after 90s (target < ${_P37_DRAIN_TARGET})"

    _p37_ping6_ok || _issue="${_issue:+${_issue}; }LAN→WAN ping6 ${_P37_PING_DETAIL:-no response} after the drain"

    if [[ -z "$_issue" ]]; then
        pass "Step 168: the GC drains the session table" \
            "ipv6_firewall_entries_used down to ${_used} (< ${_P37_DRAIN_TARGET}), IPv6 still forwarding"
    else
        fail "Step 168: the GC drains the session table" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 169 — put the bench back to the canonical IPv4 fixture
    # ------------------------------------------------------------------
    info "Step 169: restoring the bench and rechecking the canonical fixture..."
    _issue=""
    # Guest first: switching IPv6 off there drops the SLAAC address and the RA
    # default route with it.
    _cleanup_phase37_ipv6_firewall

    if ! _p37_redial "${USER_ID}"; then
        _issue="redial to an IPv4-only session failed: ${_P37_REDIAL_STAGE}"
    fi
    for _uid in "${SUB_IDS[@]}"; do
        _phase=$(_p37_user_phase "${_uid}")
        [[ "$_phase" == "Data phase" ]] || \
            _issue="${_issue:+${_issue}; }user ${_uid} status='${_phase:-missing}'"
    done

    _flag=$(_p37_hsi_field "${USER_ID}" 'ipv6_pd_prefix')
    [[ -z "$_flag" ]] || \
        _issue="${_issue:+${_issue}; }gRPC still reports ipv6_pd_prefix='${_flag}'"

    _out=$(ssh_lan "ping -c 4 -W 3 ${WAN_IP} 2>&1" || true)
    # Anchored: only a literal zero loss field counts.
    if ! printf '%s' "$_out" | grep -qE '(^|[ ,])0(\.0+)?% packet loss'; then
        _issue="${_issue:+${_issue}; }LAN→WAN ping $(printf '%s' "$_out" | grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)"
    fi

    if [[ -z "$_issue" ]]; then
        pass "Step 169: fixture restored" \
            "IPv4-only sessions healthy for users ${SUB_IDS[*]}, ${WAN_IP} reachable, IPv6 state cleared"
    else
        fail "Step 169: fixture restored" "$_issue"
    fi

    return 0
}
