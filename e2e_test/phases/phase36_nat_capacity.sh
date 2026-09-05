#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 36 — IPv4 NAT near capacity (Steps 152-155)
#
# The rest of the suite proves NAT works with a handful of flows. This phase
# asks what it does with the table nearly full: a LAN-side sprayer holds about
# 90% of the pool open while throughput is compared against the same link's
# idle baseline, and the drain afterwards shows the GC still reclaims at that
# scale.
#
# The pass mark for throughput is half of the baseline this phase measured
# moments earlier, not an absolute number, so it catches disproportionate
# degradation without turning bench-to-bench variation into a flaky test.
#
# Everything this phase starts is written to a PID file and stopped by
# _cleanup_phase36_nat_capacity, which also runs from the top-level EXIT trap.
# ---------------------------------------------------------------------------

_P36_SPRAY_PID=/tmp/e2e_nat_capacity_spray.pid
_P36_SPRAY_OUT=/tmp/e2e_nat_capacity_spray.out

# 4 source ports x 59000 destination ports = 236000 distinct flows, about 90%
# of the 262144-entry pool. The reverse key carries the destination port, so
# one NAT port per source socket serves all of its destinations.
_P36_SRC_BASE=43000
_P36_SRC_COUNT=4
_P36_DST_BASE=6000
_P36_DST_COUNT=59000
_P36_FILL_TARGET=230000
_P36_DRAIN_TARGET=26214     # 10% of the pool
_P36_SWEEP_SEC=6            # under the 10s NAT idle timeout, so entries survive

_P36_BASE_BPS=0
_P36_IPERF_ERR=""
_P36_PING_DETAIL=""

# Stop a remote process recorded in a PID file, SIGTERM first, and report
# whether it is really gone.
_p36_stop_remote() {
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

# Idempotent: called at the end of phase36 and from the top-level EXIT trap.
_cleanup_phase36_nat_capacity() {
    _p36_stop_remote ssh_lan "$_P36_SPRAY_PID" "Phase 36 NAT sprayer" || true
    ssh_lan "pkill -f 'nat-capacity' 2>/dev/null || true" >/dev/null 2>&1 || true
    ssh_lan "rm -f '${_P36_SPRAY_PID}' '${_P36_SPRAY_OUT}'" >/dev/null 2>&1 || true
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" >/dev/null 2>&1 || true
    return 0
}

# Live NAT pool fill for the primary subscriber. Prints the gauge, or nothing
# when the scrape or the row is missing.
_p36_nat_used() {
    local _body

    _body=$(e2e_metrics_body)
    e2e_metric_value "$_body" "fastrg_node_per_user_nat_entries_used" \
        "user_id=${USER_ID}"
}

_p36_nat_alloc_fail() {
    local _body

    _body=$(e2e_metrics_body)
    e2e_metric_value "$_body" "fastrg_node_per_user_nat_alloc_fail_total" \
        "user_id=${USER_ID}"
}

# Whether ping's loss figure sits within a ceiling, given ping's own output.
# The figure is parsed to a number and compared numerically, because a
# substring match would also accept the "0%" inside "100% packet loss".
# Output carrying no loss field at all means the run produced no result, which
# is a failure rather than a zero.
_p36_ping_loss_ok() {
    local _out="$1" _max_loss="$2"
    local _loss

    _loss=$(printf '%s\n' "$_out" | sed -nE \
        's/.* ([0-9]+([.][0-9]+)?)% packet loss.*/\1/p' | tail -1 || true)
    _P36_PING_DETAIL="${_loss:+${_loss}% packet loss}"
    [[ "$_loss" =~ ^[0-9]+([.][0-9]+)?$ ]] || return 1
    awk -v loss="$_loss" -v max="$_max_loss" 'BEGIN { exit !(loss <= max) }'
}

# LAN to WAN ping: four probes with no loss tolerated by default. Callers
# measuring while the bench is under load pass a larger sample and a loss
# ceiling instead, so a single frame lost on the CRC-damaged LAN path does not
# read as a regression — a 4-probe all-or-nothing check cannot tell those apart.
_p36_ping_ok() {
    local _count="${1:-4}" _max_loss="${2:-0}"
    local _out

    _out=$(ssh_lan "ping -c ${_count} -W 3 ${WAN_IP} 2>&1" || true)
    _p36_ping_loss_ok "$_out" "$_max_loss"
}

# Best of two 5-second iperf3 runs, in bits per second. Prints 0 and leaves the
# reason in _P36_IPERF_ERR when the client produced no usable result.
_p36_iperf_bps() {
    local _i _out _bps _best=0

    _P36_IPERF_ERR=""
    for _i in 1 2; do
        _out=$(ssh_lan "iperf3 -c ${WAN_IP} -p ${SRV_PORT} -t 5 -J 2>&1" || true)
        if [[ -z "$_out" ]]; then
            _P36_IPERF_ERR="no output from the iperf3 client"
            continue
        fi
        _bps=$(printf '%s' "$_out" | jq -r '.end.sum_received.bits_per_second // 0' \
            2>/dev/null || echo 0)
        _bps=$(printf '%.0f' "${_bps}" 2>/dev/null || echo 0)
        if [[ "$_bps" -le 0 ]]; then
            _P36_IPERF_ERR=$(printf '%s' "$_out" | jq -r '.error // empty' 2>/dev/null || true)
            [[ -n "$_P36_IPERF_ERR" ]] || \
                _P36_IPERF_ERR=$(printf '%s' "$_out" | tr '\n' ' ' | cut -c 1-200 || true)
        elif [[ "$_bps" -gt "$_best" ]]; then
            _best=$_bps
        fi
        sleep 1
    done
    printf '%s' "$_best"
}

_p36_mbps() {
    awk "BEGIN {printf \"%.2f\", ${1:-0} / 1000000}"
}

# Start the iperf3 server the throughput steps measure against.
_p36_start_iperf_server() {
    local _i

    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" >/dev/null 2>&1 || true
    sleep 1
    ssh_wan "iperf3 -s -B ${WAN_IP} -p ${SRV_PORT} -D --forceflush >/dev/null 2>&1 || true" \
        >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        sleep 1
        if ssh_wan "ss -ltn 2>/dev/null | grep -q ':${SRV_PORT}'" 2>/dev/null; then
            return 0
        fi
    done
    return 1
}

phase36_nat_capacity() {
    local _i _issue="" _ok=0
    local _used="" _used_before="" _alloc_before="" _alloc_after=""
    local _bps=0 _floor=0 _spray_ready=0 _spray_log=""

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 36 — IPv4 NAT Near Capacity (Steps 152-155)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 152 — idle baseline the later throughput check is measured against
    # ------------------------------------------------------------------
    info "Step 152: measuring the idle NAT baseline..."
    _issue=""
    _used_before=$(_p36_nat_used)
    _alloc_before=$(_p36_nat_alloc_fail)
    e2e_all_uint "$_used_before" "$_alloc_before" || \
        _issue="metrics unreadable (nat_entries_used='${_used_before:-missing}', alloc_fail='${_alloc_before:-missing}')"

    _p36_ping_ok || _issue="${_issue:+${_issue}; }LAN→WAN ping ${_P36_PING_DETAIL:-no response}"

    if ! _p36_start_iperf_server; then
        _issue="${_issue:+${_issue}; }iperf3 server never listened on ${SRV_PORT}"
    fi
    _P36_BASE_BPS=$(_p36_iperf_bps)
    [[ "$_P36_BASE_BPS" -gt 0 ]] || \
        _issue="${_issue:+${_issue}; }no baseline throughput (${_P36_IPERF_ERR:-unknown})"

    if [[ -z "$_issue" ]]; then
        pass "Step 152: NAT capacity baseline" \
            "idle pool ${_used_before} entries, ping 0% loss, baseline $(_p36_mbps "$_P36_BASE_BPS") Mbps"
    else
        fail "Step 152: NAT capacity baseline" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 153 — fill the pool to about 90% and keep it there
    # ------------------------------------------------------------------
    info "Step 153: filling the NAT pool with ~236000 UDP flows..."
    _issue=""
    ssh_lan "rm -f '${_P36_SPRAY_PID}' '${_P36_SPRAY_OUT}';
nohup python3 -u - '${WAN_IP}' '${_P36_SRC_BASE}' '${_P36_SRC_COUNT}' '${_P36_DST_BASE}' '${_P36_DST_COUNT}' '${_P36_SWEEP_SEC}' \
    >'${_P36_SPRAY_OUT}' 2>&1 <<'PY' &
import socket
import sys
import time

wan_ip = sys.argv[1]
src_base, src_count = int(sys.argv[2]), int(sys.argv[3])
dst_base, dst_count = int(sys.argv[4]), int(sys.argv[5])
sweep_sec = float(sys.argv[6])

socks = []
for i in range(src_count):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', src_base + i))
    socks.append(sock)
print('ready with %d source sockets' % len(socks), flush=True)

payload = b'nat-capacity'
while True:
    started = time.monotonic()
    sent = 0
    for sock in socks:
        for port in range(dst_base, dst_base + dst_count):
            try:
                sock.sendto(payload, (wan_ip, port))
                sent += 1
            except OSError:
                time.sleep(0.001)
    print('sweep sent %d in %.1fs' % (sent, time.monotonic() - started), flush=True)
    idle = sweep_sec - (time.monotonic() - started)
    if idle > 0:
        time.sleep(idle)
PY
echo \$! >'${_P36_SPRAY_PID}'" >/dev/null 2>&1 || true

    for _i in $(seq 1 5); do
        if ssh_lan "grep -q 'ready with' '${_P36_SPRAY_OUT}' 2>/dev/null"; then
            _spray_ready=1
            break
        fi
        sleep 1
    done
    [[ $_spray_ready -eq 1 ]] || _issue="sprayer never reported ready"

    _ok=0
    for _i in $(seq 1 8); do
        sleep 5
        _used=$(_p36_nat_used)
        if e2e_is_uint "$_used" && (( _used >= _P36_FILL_TARGET )); then
            _ok=1
            break
        fi
    done
    _alloc_after=$(_p36_nat_alloc_fail)

    if [[ $_ok -eq 1 && -z "$_issue" ]]; then
        pass "Step 153: NAT pool filled to near capacity" \
            "nat_entries_used=${_used} (target ≥ ${_P36_FILL_TARGET}); alloc_fail ${_alloc_before}→${_alloc_after}"
    else
        _spray_log=$(ssh_lan "tail -n 3 '${_P36_SPRAY_OUT}' 2>/dev/null" 2>/dev/null | tr '\n' ' ' | cut -c 1-300 || true)
        fail "Step 153: NAT pool filled to near capacity" \
            "${_issue:+${_issue}; }nat_entries_used='${_used:-missing}' target=${_P36_FILL_TARGET}; alloc_fail='${_alloc_after:-missing}'; sprayer: ${_spray_log:-no output}"
    fi

    # ------------------------------------------------------------------
    # Step 154 — throughput and new-flow setup while the pool stays full
    # ------------------------------------------------------------------
    # No tcpdump anywhere in this step: capturing on the LAN host is known to
    # add packet loss on this bench and would be measured as degradation.
    info "Step 154: measuring throughput while the pool stays near capacity..."
    _issue=""
    # A ping needs a fresh ICMP mapping, so this also shows new flows can still
    # be learned with the pool nearly full. Sampled deep with a loss ceiling:
    # this path drops the occasional CRC-corrupted frame whatever the node is
    # doing, so one lost probe here says nothing about NAT under load.
    local _ping_count=50 _ping_max_loss=2
    _p36_ping_ok "$_ping_count" "$_ping_max_loss" || \
        _issue="LAN→WAN ping ${_P36_PING_DETAIL:-no response} under load"

    _bps=$(_p36_iperf_bps)
    _floor=$(( _P36_BASE_BPS / 2 ))
    if [[ "$_bps" -le 0 ]]; then
        _issue="${_issue:+${_issue}; }no throughput under load (${_P36_IPERF_ERR:-unknown})"
    elif (( _bps < _floor )); then
        _issue="${_issue:+${_issue}; }throughput $(_p36_mbps "$_bps") Mbps is below half the baseline $(_p36_mbps "$_P36_BASE_BPS") Mbps"
    fi
    _used=$(_p36_nat_used)

    if [[ -z "$_issue" ]]; then
        pass "Step 154: throughput holds up near capacity" \
            "$(_p36_mbps "$_bps") Mbps of $(_p36_mbps "$_P36_BASE_BPS") Mbps baseline with ${_used} live mappings, ping ${_P36_PING_DETAIL:-no loss figure} over ${_ping_count} probes (ceiling ${_ping_max_loss}%)"
    else
        fail "Step 154: throughput holds up near capacity" \
            "${_issue}; nat_entries_used='${_used:-missing}'"
    fi

    # ------------------------------------------------------------------
    # Step 155 — stop the sprayer and watch the GC drain the pool
    # ------------------------------------------------------------------
    info "Step 155: stopping the sprayer and waiting for the pool to drain..."
    _issue=""
    _p36_stop_remote ssh_lan "$_P36_SPRAY_PID" "Phase 36 NAT sprayer" || \
        _issue="sprayer did not stop"

    _ok=0
    for _i in $(seq 1 18); do
        sleep 5
        _used=$(_p36_nat_used)
        if e2e_is_uint "$_used" && (( _used < _P36_DRAIN_TARGET )); then
            _ok=1
            break
        fi
    done
    [[ $_ok -eq 1 ]] || \
        _issue="${_issue:+${_issue}; }pool still holds '${_used:-missing}' entries after 90s (target < ${_P36_DRAIN_TARGET})"

    _p36_ping_ok || _issue="${_issue:+${_issue}; }LAN→WAN ping ${_P36_PING_DETAIL:-no response} after the drain"

    if [[ -z "$_issue" ]]; then
        pass "Step 155: the GC drains the pool" \
            "nat_entries_used down to ${_used} (< ${_P36_DRAIN_TARGET}), ${WAN_IP} still reachable"
    else
        fail "Step 155: the GC drains the pool" "$_issue"
    fi

    _cleanup_phase36_nat_capacity
    return 0
}

# Sample-based self-verification of the loss ceiling above; the steps call this
# same function.
local_validation_register p36_ping_loss _p36_ping_loss_ok \
    p36_ping_loss_zero p36_ping_loss_within_ceiling \
    p36_ping_loss_over_ceiling p36_ping_loss_total_loss \
    p36_ping_loss_fractional_over p36_ping_loss_no_output
