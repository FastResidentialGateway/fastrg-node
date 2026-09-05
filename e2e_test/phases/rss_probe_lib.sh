#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Shared RSS distribution probe.
#
# Each data lcore polls exactly one RSS queue, so the per-lcore traffic rows
# expose the per-queue distribution. The probe puts a known number of distinct
# UDP flows through the subscriber's session and reports how the WAN rx landed
# across the lcores; the verdict predicate below decides what that means.
#
# This file defines no phase function. It lives in phases/ so the existing
# "scp phases/*.sh" upload in run_e2e_test.sh carries it to the runner, and is
# sourced after metrics_lib.sh because every reading here goes through it.
#
# Both address families use the same constants, the same client and the same
# assertions: the only thing an IPv4 and an IPv6 run may legitimately differ in
# is how the node hashes the inner header, which is exactly what is measured.
# ---------------------------------------------------------------------------

_E2E_RSS_ECHO_PORT=5903
_E2E_RSS_FLOWS=16
_E2E_RSS_PKTS_PER_FLOW=20
_E2E_RSS_SRC_BASE=43000
_E2E_RSS_ECHO_PID=/tmp/e2e_rss_echo.pid
_E2E_RSS_ECHO_OUT=/tmp/e2e_rss_echo.out
_E2E_RSS_ECHO_READY=/tmp/e2e_rss_echo.ready

# UDP echo server on the WAN host. FAMILY is 4 or 6, BIND_ADDR the address in
# that family. Returns non-zero when it does not report ready within 5s.
_e2e_rss_start_server() {
    local _family="$1" _bind="$2" _i

    ssh_wan "rm -f '${_E2E_RSS_ECHO_PID}' '${_E2E_RSS_ECHO_OUT}' '${_E2E_RSS_ECHO_READY}';
nohup python3 -u - '${_family}' '${_bind}' '${_E2E_RSS_ECHO_PORT}' '${_E2E_RSS_ECHO_READY}' \
    >'${_E2E_RSS_ECHO_OUT}' 2>&1 <<'PY' &
import socket
import sys
import time

family_arg, bind_ip = sys.argv[1], sys.argv[2]
port, ready_path = int(sys.argv[3]), sys.argv[4]
family = socket.AF_INET6 if family_arg == '6' else socket.AF_INET
sock = socket.socket(family, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind((bind_ip, port))
sock.settimeout(0.5)
with open(ready_path, 'w', encoding='ascii') as ready:
    ready.write('ready\n')
deadline = time.monotonic() + 60
while time.monotonic() < deadline:
    try:
        payload, peer = sock.recvfrom(2048)
    except socket.timeout:
        continue
    sock.sendto(payload, peer)
PY
echo \$! >'${_E2E_RSS_ECHO_PID}'" >/dev/null 2>&1 || true
    for _i in $(seq 1 5); do
        if ssh_wan "test -s '${_E2E_RSS_ECHO_READY}'" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# Send _E2E_RSS_FLOWS flows from the LAN host to the echo server, each from its
# own source port, and print "flows_with_reply=N replies=N sent=N".
_e2e_rss_client() {
    local _family="$1" _server="$2"

    ssh_lan "python3 -u - '${_family}' '${_server}' '${_E2E_RSS_ECHO_PORT}' \
        '${_E2E_RSS_FLOWS}' '${_E2E_RSS_SRC_BASE}' '${_E2E_RSS_PKTS_PER_FLOW}' <<'PY'
import socket
import sys
import time

family_arg, server = sys.argv[1], sys.argv[2]
echo_port = int(sys.argv[3])
flows = int(sys.argv[4])
src_base = int(sys.argv[5])
pkts = int(sys.argv[6])

family = socket.AF_INET6 if family_arg == '6' else socket.AF_INET
bind_ip = '::' if family_arg == '6' else '0.0.0.0'

socks = []
for i in range(flows):
    sock = socket.socket(family, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((bind_ip, src_base + i))
    sock.setblocking(False)
    socks.append(sock)

def drain(got):
    for i, sock in enumerate(socks):
        while True:
            try:
                sock.recvfrom(2048)
                got[i] += 1
            except (BlockingIOError, OSError):
                break

got = [0] * flows
sent = 0
for _round in range(pkts):
    for i, sock in enumerate(socks):
        try:
            sock.sendto(b'rss-probe-flow-%d' % i, (server, echo_port))
            sent += 1
        except OSError:
            pass
    time.sleep(0.05)
    drain(got)
deadline = time.monotonic() + 3
while time.monotonic() < deadline:
    time.sleep(0.1)
    drain(got)
print('flows_with_reply=%d replies=%d sent=%d'
      % (sum(1 for count in got if count > 0), sum(got), sent))
PY" 2>&1 || true
}

# Idempotent; safe to call when no server was ever started.
_e2e_rss_stop_server() {
    local _i

    ssh_wan "if [ -s '${_E2E_RSS_ECHO_PID}' ]; then
            _pid=\$(cat '${_E2E_RSS_ECHO_PID}' 2>/dev/null || true)
            [ -n \"\$_pid\" ] && kill -0 \"\$_pid\" 2>/dev/null && kill -TERM \"\$_pid\" 2>/dev/null || true
        fi" >/dev/null 2>&1 || true
    for _i in $(seq 1 5); do
        if ! ssh_wan "_pid=\$(cat '${_E2E_RSS_ECHO_PID}' 2>/dev/null); \
            test -n \"\$_pid\" && kill -0 \"\$_pid\" 2>/dev/null" 2>/dev/null; then
            ssh_wan "rm -f '${_E2E_RSS_ECHO_PID}' '${_E2E_RSS_ECHO_OUT}' '${_E2E_RSS_ECHO_READY}'" \
                >/dev/null 2>&1 || true
            return 0
        fi
        sleep 1
    done
    warn "RSS probe UDP echo server did not exit within 5s after SIGTERM"
    return 1
}

# Signed WAN rx delta of one lcore between two scrapes; prints nothing when
# either sample is unreadable.
_e2e_rss_delta() {
    local _before="$1" _after="$2" _lcore="$3" _b="" _a=""

    _b=$(e2e_metric_value "$_before" fastrg_node_lcore_rx_packets_total \
        "lcore_id=${_lcore}" "nic_index=1")
    _a=$(e2e_metric_value "$_after" fastrg_node_lcore_rx_packets_total \
        "lcore_id=${_lcore}" "nic_index=1")
    e2e_all_uint "$_b" "$_a" || return 0
    printf '%d' $(( _a - _b ))
}

# e2e_rss_probe FAMILY SERVER_ADDR — run the probe and print one line:
#   wd_hit=<n> wd_count=<n> wd_sum=<n> wc_delta=<n> flows_ok=<n>
# wd_* describe the wan_data lcores (how many rows exist, how many grew, by how
# much in total), wc_delta the wan_ctrl lcore that owns queue 0, flows_ok how
# many of the flows got an echo back. A field reads "err" when the counters
# behind it could not be read, which the verdict below refuses to judge.
e2e_rss_probe() {
    local _family="$1" _server="$2"
    local _before="" _after="" _client_out="" _ready=0
    local _wd_lcores="" _wc_lcores="" _lid="" _delta=""
    local _wd_count=0 _wd_hit=0 _wd_sum=0 _wc_delta=0 _flows_ok=0 _unreadable=0

    _before=$(e2e_metrics_body)
    _wd_lcores=$(e2e_metric_label_values "$_before" \
        fastrg_node_lcore_rx_packets_total lcore_id "role=wan_data" "nic_index=1")
    _wc_lcores=$(e2e_metric_label_values "$_before" \
        fastrg_node_lcore_rx_packets_total lcore_id "role=wan_ctrl" "nic_index=1")
    _wd_count=$(printf '%s\n' "$_wd_lcores" | grep -c . || true)

    # Fewer than two data queues makes the spread question unanswerable, so the
    # traffic is not even generated: the verdict says no_rows and the step says
    # so instead of reporting a spread it never measured.
    if [[ "$_wd_count" -ge 2 ]]; then
        if _e2e_rss_start_server "$_family" "$_server"; then
            _ready=1
        fi
    fi

    if [[ "$_ready" -eq 1 ]]; then
        _client_out=$(_e2e_rss_client "$_family" "$_server")
        _after=$(e2e_metrics_body)
        _flows_ok=$(printf '%s\n' "$_client_out" | \
            sed -nE 's/.*flows_with_reply=([0-9]+).*/\1/p' | tail -1 || true)
        e2e_is_uint "$_flows_ok" || _flows_ok=0

        for _lid in $_wd_lcores; do
            _delta=$(_e2e_rss_delta "$_before" "$_after" "$_lid")
            if ! [[ "$_delta" =~ ^-?[0-9]+$ ]]; then
                _unreadable=1
                continue
            fi
            (( _delta > 0 )) && _wd_hit=$(( _wd_hit + 1 ))
            _wd_sum=$(( _wd_sum + _delta ))
        done
        for _lid in $_wc_lcores; do
            _delta=$(_e2e_rss_delta "$_before" "$_after" "$_lid")
            [[ "$_delta" =~ ^-?[0-9]+$ ]] && _wc_delta=$(( _wc_delta + _delta ))
        done
    fi

    _e2e_rss_stop_server || true
    if [[ "$_unreadable" -eq 1 ]]; then
        printf 'wd_hit=err wd_count=%s wd_sum=err wc_delta=%s flows_ok=%s' \
            "$_wd_count" "$_wc_delta" "$_flows_ok"
        return 0
    fi
    printf 'wd_hit=%s wd_count=%s wd_sum=%s wc_delta=%s flows_ok=%s' \
        "$_wd_hit" "$_wd_count" "$_wd_sum" "$_wc_delta" "$_flows_ok"
}

# e2e_rss_field LINE KEY — one field out of an e2e_rss_probe line; "err" when
# the field is absent, so a truncated line can never read as a number.
e2e_rss_field() {
    printf '%s\n' "$1" | awk -v key="$2" '
        {
            for (i = 1; i <= NF; i++) {
                split($i, kv, "=")
                if (kv[1] == key) { print kv[2]; found = 1; exit }
            }
        }
        END { if (!found) print "err" }
    ' || true
}

# e2e_rss_spread_verdict WD_HIT WD_COUNT WD_SUM WC_DELTA FLOWS_OK FLOWS
#
# Prints pass | no_rows | probe_broken | single_queue | queue0 | err.
#   no_rows       fewer than 2 data queues exist, so there is nothing to spread
#                 over and the assertion needs a redesign, not a waiver
#   probe_broken  under three quarters of the flows were echoed back, so the
#                 traffic never ran end to end and says nothing about RSS
#   single_queue  fewer than 2 data lcores took any of it (with 16 flows and
#                 uniform 2-queue hashing that is a ~2^-15 accident)
#   queue0        the control lcore absorbed a quarter or more of the total
#   err           a reading that could not be taken; never judged as a spread
e2e_rss_spread_verdict() {
    local _hit="${1:-}" _count="${2:-}" _sum="${3:-}"
    local _wc="${4:-}" _flows_ok="${5:-}" _flows="${6:-}"

    if ! e2e_all_uint "$_hit" "$_count" "$_sum" "$_wc" "$_flows_ok" "$_flows"; then
        printf 'err'
        return 1
    fi
    if (( _count < 2 )); then
        printf 'no_rows'
        return 1
    fi
    if (( _flows_ok * 4 < _flows * 3 )); then
        printf 'probe_broken'
        return 1
    fi
    if (( _hit < 2 )); then
        printf 'single_queue'
        return 1
    fi
    if (( _wc * 4 > _sum )); then
        printf 'queue0'
        return 1
    fi
    printf 'pass'
    return 0
}

local_validation_register rss_spread_verdict e2e_rss_spread_verdict \
    rss_spread_good \
    rss_spread_no_rows \
    rss_spread_probe_broken \
    rss_spread_single_queue \
    rss_spread_queue0 \
    rss_spread_unreadable_rows \
    rss_spread_empty_input
