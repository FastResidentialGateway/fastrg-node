#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 25 — Sustained UDP / ICMP traffic across NAT timeout, plus RSS
# queue-distribution check (Steps 104-106b)
# ---------------------------------------------------------------------------

_P25_IPERF_PORT=5902
_P25_METRICS_PORT=""

# RSS distribution probe (Step 106b)
_P25_RSS_ECHO_PORT=5903
_P25_RSS_FLOWS=16
_P25_RSS_PKTS_PER_FLOW=20
_P25_RSS_SRC_BASE=43000
_P25_RSS_ECHO_PID=/tmp/e2e_rss_echo.pid
_P25_RSS_ECHO_OUT=/tmp/e2e_rss_echo.out
_P25_RSS_ECHO_READY=/tmp/e2e_rss_echo.ready

_p25_stop_echo_server() {
    local _i

    ssh_wan "if [ -s '${_P25_RSS_ECHO_PID}' ]; then
            _pid=\$(cat '${_P25_RSS_ECHO_PID}' 2>/dev/null || true)
            [ -n \"\$_pid\" ] && kill -0 \"\$_pid\" 2>/dev/null && kill -TERM \"\$_pid\" 2>/dev/null || true
        fi" >/dev/null 2>&1 || true
    for _i in $(seq 1 5); do
        if ! ssh_wan "_pid=\$(cat '${_P25_RSS_ECHO_PID}' 2>/dev/null); \
            test -n \"\$_pid\" && kill -0 \"\$_pid\" 2>/dev/null" 2>/dev/null; then
            ssh_wan "rm -f '${_P25_RSS_ECHO_PID}' '${_P25_RSS_ECHO_OUT}' '${_P25_RSS_ECHO_READY}'" \
                >/dev/null 2>&1 || true
            return 0
        fi
        sleep 1
    done
    warn "Phase 25 UDP echo server did not exit within 5s after SIGTERM"
    return 1
}

_p25_snippet() {
    printf '%s' "$1" | tr '\n' ' ' | cut -c 1-500 || true
}

_p25_fetch_metric() {
    local _family="$1"
    local _body

    _body=$(ssh_node \
        "curl -fsS --max-time 5 http://127.0.0.1:${_P25_METRICS_PORT}/metrics" \
        2>/dev/null || true)
    printf '%s\n' "$_body" | awk -v family="$_family" -v uid="$USER_ID" '
        $1 ~ ("^" family "\\{") && $1 ~ ("user_id=\"" uid "\"") { print $2; exit }
    ' || true
}

_p25_is_uint() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

_p25_stop_client() {
    local _i

    ssh_lan "pkill -TERM -f '[i]perf3 -u -c ${WAN_IP} -p ${_P25_IPERF_PORT}' 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    for _i in $(seq 1 5); do
        if ! ssh_lan "pgrep -f '[i]perf3 -u -c ${WAN_IP} -p ${_P25_IPERF_PORT}' >/dev/null" \
            2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    warn "Phase 25 iperf3 client did not exit within 5s after SIGTERM"
    return 1
}

_p25_stop_server() {
    local _i

    ssh_wan "pkill -TERM -f '[i]perf3 -s -B ${WAN_IP} -p ${_P25_IPERF_PORT}' 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    for _i in $(seq 1 5); do
        if ! ssh_wan "pgrep -f '[i]perf3 -s -B ${WAN_IP} -p ${_P25_IPERF_PORT}' >/dev/null" \
            2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    warn "Phase 25 iperf3 server did not exit within 5s after SIGTERM"
    return 1
}

_cleanup_phase25_udp_icmp_traffic() {
    _p25_stop_client || true
    _p25_stop_server || true
    _p25_stop_echo_server || true
    return 0
}

# ---------------------------------------------------------------------------
# Traffic-counter assertions.
#
# rx is counted on the port a packet arrives on and tx on the port it leaves by
# (see the count_rx_packet/count_tx_packet calls in src/dp.c), so a LAN->WAN
# flow must show up as per-subscriber rx on nic_index 0 and tx on nic_index 1,
# and the reverse flow the other way round. The PPPoE session counters follow
# the encapsulation instead: tx on encapsulate, rx on decapsulate.
#
# The containment check compares packets only, never bytes: the NIC byte
# counters and the per-subscriber byte counters measure the frame at different
# points of the encap/decap path, so only the packet counts are comparable.
# _P25_PORT_SKEW allows for the fact that one scrape gathers the NIC port stats
# before the per-subscriber rows, so a few packets can land between the two
# reads while a flow is running.
# ---------------------------------------------------------------------------
_P25_PORT_SKEW=100
_P25_TRAFFIC_ISSUE=""

# Signed delta of one metric row between two scrapes; prints nothing when
# either sample is unreadable.
_p25_delta() {
    local _before="$1" _after="$2" _family="$3"
    local _b="" _a=""

    shift 3
    _b=$(e2e_metric_value "$_before" "$_family" "$@")
    _a=$(e2e_metric_value "$_after" "$_family" "$@")
    e2e_all_uint "$_b" "$_a" || return 0
    printf '%d' $(( _a - _b ))
}

# Same, summed over every row matching the label filters.
_p25_delta_sum() {
    local _before="$1" _after="$2" _family="$3"
    local _b="" _a=""

    shift 3
    _b=$(e2e_metric_sum "$_before" "$_family" "$@")
    _a=$(e2e_metric_sum "$_after" "$_family" "$@")
    e2e_all_uint "$_b" "$_a" || return 0
    printf '%d' $(( _a - _b ))
}

_p25_want_growth() {
    local _label="$1" _delta="$2"

    if ! [[ "$_delta" =~ ^-?[0-9]+$ ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }${_label} unreadable"
    elif [[ "$_delta" -le 0 ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }${_label} delta=${_delta}, expected > 0"
    fi
}

_p25_want_contains() {
    local _label="$1" _port_delta="$2" _user_delta="$3"

    if ! [[ "$_port_delta" =~ ^-?[0-9]+$ && "$_user_delta" =~ ^-?[0-9]+$ ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }${_label} unreadable"
    elif [[ "$_port_delta" -lt $(( _user_delta - _P25_PORT_SKEW )) ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }${_label}: port delta=${_port_delta} < per-subscriber total=${_user_delta}"
    fi
}

# _p25_check_flow BEFORE AFTER RX_NIC TX_NIC SESSION_DIR
_p25_check_flow() {
    local _before="$1" _after="$2" _rx_nic="$3" _tx_nic="$4" _session_dir="$5"

    _P25_TRAFFIC_ISSUE=""

    _p25_want_growth "per_user rx_packets{nic=${_rx_nic},user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_per_user_rx_packets_total "nic_index=${_rx_nic}" "user_id=${USER_ID}")"
    _p25_want_growth "per_user rx_bytes{nic=${_rx_nic},user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_per_user_rx_bytes_total "nic_index=${_rx_nic}" "user_id=${USER_ID}")"
    _p25_want_growth "per_user tx_packets{nic=${_tx_nic},user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_per_user_tx_packets_total "nic_index=${_tx_nic}" "user_id=${USER_ID}")"
    _p25_want_growth "per_user tx_bytes{nic=${_tx_nic},user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_per_user_tx_bytes_total "nic_index=${_tx_nic}" "user_id=${USER_ID}")"

    _p25_want_growth "per_pppoe_session ${_session_dir}_packets{user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" "fastrg_node_per_pppoe_session_${_session_dir}_packets_total" "user_id=${USER_ID}")"
    _p25_want_growth "per_pppoe_session ${_session_dir}_bytes{user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" "fastrg_node_per_pppoe_session_${_session_dir}_bytes_total" "user_id=${USER_ID}")"

    _p25_want_growth "nic rx_packets{nic=${_rx_nic}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_rx_packets_total "nic_index=${_rx_nic}")"
    _p25_want_growth "nic rx_bytes{nic=${_rx_nic}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_rx_bytes_total "nic_index=${_rx_nic}")"
    _p25_want_growth "nic tx_packets{nic=${_tx_nic}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_tx_packets_total "nic_index=${_tx_nic}")"
    _p25_want_growth "nic tx_bytes{nic=${_tx_nic}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_tx_bytes_total "nic_index=${_tx_nic}")"

    _p25_want_contains "nic ${_rx_nic} rx_packets vs per-subscriber rx" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_rx_packets_total "nic_index=${_rx_nic}")" \
        "$(_p25_delta_sum "$_before" "$_after" fastrg_node_per_user_rx_packets_total "nic_index=${_rx_nic}")"
    _p25_want_contains "nic ${_tx_nic} tx_packets vs per-subscriber tx" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_tx_packets_total "nic_index=${_tx_nic}")" \
        "$(_p25_delta_sum "$_before" "$_after" fastrg_node_per_user_tx_packets_total "nic_index=${_tx_nic}")"

    # Dropped counters are cumulative gauges no healthy flow should move; the
    # checkable contract without injecting a fault is that they never regress.
    local _dropped_pkts _dropped_bytes
    _dropped_pkts=$(_p25_delta_sum "$_before" "$_after" fastrg_node_per_user_dropped_packets_total)
    _dropped_bytes=$(_p25_delta_sum "$_before" "$_after" fastrg_node_per_user_dropped_bytes_total)
    if ! [[ "$_dropped_pkts" =~ ^-?[0-9]+$ && "$_dropped_bytes" =~ ^-?[0-9]+$ ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }per_user dropped counters unreadable"
    elif [[ "$_dropped_pkts" -lt 0 || "$_dropped_bytes" -lt 0 ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }per_user dropped counters regressed (packets=${_dropped_pkts}, bytes=${_dropped_bytes})"
    fi

    [[ -z "$_P25_TRAFFIC_ISSUE" ]]
}

_p25_parse_udp_json() {
    printf '%s\n' "$1" | jq -r '
        [(.end.sum.lost_percent // .end.sum_received.lost_percent //
          .end.streams[0].udp.lost_percent // null),
         (.end.sum.bytes // .end.sum_received.bytes // .end.streams[0].udp.bytes // null)] |
        @tsv
    ' 2>/dev/null || true
}

_p25_udp_result_ok() {
    local _lost="$1"
    local _bytes="$2"

    [[ "$_lost" =~ ^[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?$ ]] || return 1
    [[ "$_bytes" =~ ^[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?$ ]] || return 1
    awk -v lost="$_lost" -v bytes="$_bytes" 'BEGIN { exit !(lost < 1 && bytes > 0) }'
}

_p25_ping_loss() {
    printf '%s\n' "$1" | sed -nE \
        's/.* ([0-9]+([.][0-9]+)?)% packet loss.*/\1/p' | tail -1 || true
}

phase25_udp_icmp_traffic() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 25 — Sustained UDP / ICMP Traffic (Steps 104-106b)"
    bold "═══════════════════════════════════════════════════════"

    local _i _server_ready=0
    local _entries_base="" _entries_after="" _alloc_base="" _alloc_after=""
    local _iperf_out="" _iperf_rc=0 _parsed="" _lost="" _bytes="" _client_stopped=0
    local _ping_out="" _ping_retry_out="" _ping_loss="" _entries_delta=""
    local _traffic_before="" _traffic_after=""

    _P25_METRICS_PORT=$(ssh_node \
        "grep 'MetricsListenPort' /etc/fastrg/config.cfg 2>/dev/null" | \
        awk -F'"' '{print $2}' | awk -F: '{print $NF}' || true)

    _cleanup_phase25_udp_icmp_traffic
    ssh_wan "iperf3 -s -B ${WAN_IP} -p ${_P25_IPERF_PORT} -D --forceflush" \
        >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        if ssh_wan "ss -ltn 2>/dev/null | grep -q ':${_P25_IPERF_PORT}'" 2>/dev/null; then
            _server_ready=1
            break
        fi
        sleep 1
    done

    info "Step 104: running 20s UDP LAN→WAN flow at 50 Mbps..."
    _entries_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")
    _alloc_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_alloc_fail_total")
    _traffic_before=$(e2e_metrics_body)
    if [[ $_server_ready -eq 1 ]]; then
        if _iperf_out=$(ssh_lan \
            "iperf3 -u -c ${WAN_IP} -p ${_P25_IPERF_PORT} -t 20 -b 50M -J" 2>&1); then
            _iperf_rc=0
        else
            _iperf_rc=$?
        fi
    else
        _iperf_rc=1
        _iperf_out="iperf3 server did not listen on port ${_P25_IPERF_PORT} within 10s"
    fi
    _entries_after=$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")
    _alloc_after=$(_p25_fetch_metric "fastrg_node_per_user_nat_alloc_fail_total")
    _traffic_after=$(e2e_metrics_body)
    _parsed=$(_p25_parse_udp_json "$_iperf_out")
    IFS=$'\t' read -r _lost _bytes <<< "$_parsed" || true
    _client_stopped=0
    _p25_stop_client && _client_stopped=1
    # LAN->WAN: rx on nic 0, tx on nic 1, PPPoE session tx (encapsulation).
    _p25_check_flow "$_traffic_before" "$_traffic_after" 0 1 tx || true
    if [[ $_iperf_rc -eq 0 && $_client_stopped -eq 1 ]] && \
       _p25_udp_result_ok "$_lost" "$_bytes" && \
       _p25_is_uint "$_entries_base" && _p25_is_uint "$_entries_after" && \
       _p25_is_uint "$_alloc_base" && [[ "$_alloc_after" == "$_alloc_base" ]] && \
       (( _entries_after - _entries_base <= 3 )) && \
       [[ -z "$_P25_TRAFFIC_ISSUE" ]]; then
        pass "Step 104: sustained UDP LAN→WAN" \
            "loss=${_lost}%, bytes=${_bytes}, entries delta=$(( _entries_after - _entries_base )), alloc_fail delta=0; per-subscriber/session/NIC counters advanced in the LAN→WAN direction"
    else
        fail "Step 104: sustained UDP LAN→WAN" \
            "server=${_server_ready} rc=${_iperf_rc} stopped=${_client_stopped}; loss=${_lost:-NA}% bytes=${_bytes:-NA}; entries=${_entries_base:-NA}->${_entries_after:-NA}; alloc_fail=${_alloc_base:-NA}->${_alloc_after:-NA}${_P25_TRAFFIC_ISSUE:+; traffic counters: ${_P25_TRAFFIC_ISSUE}}; output='$(_p25_snippet "$_iperf_out")'"
    fi

    info "Step 105: running 20s reverse UDP WAN→LAN flow at 50 Mbps..."
    _alloc_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_alloc_fail_total")
    _traffic_before=$(e2e_metrics_body)
    if [[ $_server_ready -eq 1 ]]; then
        if _iperf_out=$(ssh_lan \
            "iperf3 -u -c ${WAN_IP} -p ${_P25_IPERF_PORT} -t 20 -b 50M -R -J" 2>&1); then
            _iperf_rc=0
        else
            _iperf_rc=$?
        fi
    else
        _iperf_rc=1
        _iperf_out="iperf3 server did not listen on port ${_P25_IPERF_PORT} within 10s"
    fi
    _alloc_after=$(_p25_fetch_metric "fastrg_node_per_user_nat_alloc_fail_total")
    _traffic_after=$(e2e_metrics_body)
    _parsed=$(_p25_parse_udp_json "$_iperf_out")
    IFS=$'\t' read -r _lost _bytes <<< "$_parsed" || true
    _client_stopped=0
    _p25_stop_client && _client_stopped=1
    local _server_stopped=0
    _p25_stop_server && _server_stopped=1
    # WAN->LAN: rx on nic 1, tx on nic 0, PPPoE session rx (decapsulation).
    _p25_check_flow "$_traffic_before" "$_traffic_after" 1 0 rx || true
    if [[ $_iperf_rc -eq 0 && $_client_stopped -eq 1 && $_server_stopped -eq 1 ]] && \
       _p25_udp_result_ok "$_lost" "$_bytes" && \
       _p25_is_uint "$_alloc_base" && [[ "$_alloc_after" == "$_alloc_base" ]] && \
       [[ -z "$_P25_TRAFFIC_ISSUE" ]]; then
        pass "Step 105: sustained reverse UDP WAN→LAN" \
            "loss=${_lost}%, bytes=${_bytes}, alloc_fail delta=0; full 20s reverse flow completed; per-subscriber/session/NIC counters advanced in the WAN→LAN direction"
    else
        fail "Step 105: sustained reverse UDP WAN→LAN" \
            "server=${_server_ready}/${_server_stopped} rc=${_iperf_rc} stopped=${_client_stopped}; loss=${_lost:-NA}% bytes=${_bytes:-NA}; alloc_fail=${_alloc_base:-NA}->${_alloc_after:-NA}${_P25_TRAFFIC_ISSUE:+; traffic counters: ${_P25_TRAFFIC_ISSUE}}; output='$(_p25_snippet "$_iperf_out")'"
    fi

    info "Step 106: running approximately 15s of ICMP echo traffic..."
    _entries_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")
    _ping_out=$(ssh_lan "ping -c 150 -i 0.1 -W 2 ${WAN_IP}" 2>&1 || true)
    _ping_loss=$(_p25_ping_loss "$_ping_out")
    if [[ "$_ping_loss" != "0" && "$_ping_loss" != "0.0" ]]; then
        info "Step 106: first ping reported ${_ping_loss:-unparseable}% loss; retrying once..."
        _ping_retry_out=$(ssh_lan "ping -c 150 -i 0.1 -W 2 ${WAN_IP}" 2>&1 || true)
        _ping_loss=$(_p25_ping_loss "$_ping_retry_out")
    fi
    _entries_after=$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")
    if _p25_is_uint "$_entries_base" && _p25_is_uint "$_entries_after"; then
        _entries_delta=$(( _entries_after - _entries_base ))
    fi
    if [[ "$_ping_loss" == "0" || "$_ping_loss" == "0.0" ]] && \
       [[ "$_entries_delta" =~ ^-?[0-9]+$ ]] && (( _entries_delta <= 1 )); then
        pass "Step 106: sustained ICMP echo" \
            "loss=${_ping_loss}%, entries delta=${_entries_delta}; ident mapping survived beyond 10s"
    else
        fail "Step 106: sustained ICMP echo" \
            "loss=${_ping_loss:-NA}%; entries=${_entries_base:-NA}->${_entries_after:-NA}; first='$(_p25_snippet "$_ping_out")' retry='$(_p25_snippet "$_ping_retry_out")'"
    fi

    # ------------------------------------------------------------------
    # Step 106b — RSS spreads PPPoE session traffic across the data lcores.
    #
    # Each data lcore polls exactly one RSS queue, so the per-lcore traffic
    # rows expose the per-queue distribution. A WAN-side UDP echo server
    # reflects >= 16 LAN flows with distinct source ports; each flow gets a
    # distinct NAT external port, so the echoed (downstream) packets are >= 16
    # distinct inner tuples that WAN RSS must spread over queues 1..N. The
    # assertions:
    #   - at least 2 wan_data lcores saw their WAN rx grow (with 16 flows and
    #     uniform 2-queue hashing, all-on-one-queue happens with probability
    #     ~2^-15, so the check is deterministic in practice);
    #   - the wan_ctrl lcore (queue 0) did not absorb the bulk of the session
    #     data (its delta stays below a quarter of the data-lcore total).
    # A collapse to a single queue or to queue 0 — the silent RSS/DDP
    # degradation this step exists for — fails both.
    # ------------------------------------------------------------------
    info "Step 106b: probing RSS distribution with ${_P25_RSS_FLOWS} UDP echo flows..."
    local _rss_issue="" _rss_before="" _rss_after="" _rss_client_out=""
    local _wd_lcores="" _wc_lcores="" _wd_count=0 _wd_hit=0 _wd_sum=0 _wc_delta=0
    local _lid="" _delta="" _flows_ok="" _replies="" _echo_ready=0

    _rss_before=$(e2e_metrics_body)
    _wd_lcores=$(e2e_metric_label_values "$_rss_before" \
        fastrg_node_lcore_rx_packets_total lcore_id "role=wan_data" "nic_index=1")
    _wc_lcores=$(e2e_metric_label_values "$_rss_before" \
        fastrg_node_lcore_rx_packets_total lcore_id "role=wan_ctrl" "nic_index=1")
    _wd_count=$(printf '%s\n' "$_wd_lcores" | grep -c . || true)
    if [[ "$_wd_count" -lt 2 ]]; then
        _rss_issue="expected >= 2 wan_data lcore rows, found ${_wd_count} (RSS datapath inactive or single data queue; the spread assertion needs a redesign, not a waiver)"
    fi

    if [[ -z "$_rss_issue" ]]; then
        ssh_wan "rm -f '${_P25_RSS_ECHO_PID}' '${_P25_RSS_ECHO_OUT}' '${_P25_RSS_ECHO_READY}';
nohup python3 -u - '${WAN_IP}' '${_P25_RSS_ECHO_PORT}' '${_P25_RSS_ECHO_READY}' \
    >'${_P25_RSS_ECHO_OUT}' 2>&1 <<'PY' &
import socket
import sys
import time

bind_ip, port, ready_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
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
echo \$! >'${_P25_RSS_ECHO_PID}'" >/dev/null 2>&1 || true
        for _i in $(seq 1 5); do
            if ssh_wan "test -s '${_P25_RSS_ECHO_READY}'" 2>/dev/null; then
                _echo_ready=1
                break
            fi
            sleep 1
        done
        [[ $_echo_ready -eq 1 ]] || _rss_issue="UDP echo server did not become ready within 5s"
    fi

    if [[ -z "$_rss_issue" ]]; then
        _rss_client_out=$(ssh_lan "python3 -u - '${WAN_IP}' '${_P25_RSS_ECHO_PORT}' \
            '${_P25_RSS_FLOWS}' '${_P25_RSS_SRC_BASE}' '${_P25_RSS_PKTS_PER_FLOW}' <<'PY'
import socket
import sys
import time

wan_ip = sys.argv[1]
echo_port = int(sys.argv[2])
flows = int(sys.argv[3])
src_base = int(sys.argv[4])
pkts = int(sys.argv[5])

socks = []
for i in range(flows):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', src_base + i))
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
            sock.sendto(b'rss-probe-flow-%d' % i, (wan_ip, echo_port))
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
PY" 2>&1 || true)
        _rss_after=$(e2e_metrics_body)
        _flows_ok=$(printf '%s\n' "$_rss_client_out" | \
            sed -nE 's/.*flows_with_reply=([0-9]+).*/\1/p' | tail -1 || true)
        _replies=$(printf '%s\n' "$_rss_client_out" | \
            sed -nE 's/.*replies=([0-9]+).*/\1/p' | tail -1 || true)
        if ! e2e_is_uint "$_flows_ok" || (( _flows_ok < 12 )); then
            _rss_issue="echo traffic did not run end-to-end (flows_with_reply='${_flows_ok:-NA}' of ${_P25_RSS_FLOWS}, replies='${_replies:-NA}'); fix the probe path before judging RSS; client output='$(_p25_snippet "$_rss_client_out")'"
        fi
    fi

    if [[ -z "$_rss_issue" ]]; then
        _wd_hit=0
        _wd_sum=0
        for _lid in $_wd_lcores; do
            _delta=$(_p25_delta "$_rss_before" "$_rss_after" \
                fastrg_node_lcore_rx_packets_total "lcore_id=${_lid}" "nic_index=1")
            if ! [[ "$_delta" =~ ^-?[0-9]+$ ]]; then
                _rss_issue="${_rss_issue:+${_rss_issue}; }wan_data lcore ${_lid} rx row unreadable"
                continue
            fi
            (( _delta > 0 )) && _wd_hit=$(( _wd_hit + 1 ))
            _wd_sum=$(( _wd_sum + _delta ))
        done
        _wc_delta=0
        for _lid in $_wc_lcores; do
            _delta=$(_p25_delta "$_rss_before" "$_rss_after" \
                fastrg_node_lcore_rx_packets_total "lcore_id=${_lid}" "nic_index=1")
            [[ "$_delta" =~ ^-?[0-9]+$ ]] && _wc_delta=$(( _wc_delta + _delta ))
        done
        if (( _wd_hit < 2 )); then
            _rss_issue="${_rss_issue:+${_rss_issue}; }only ${_wd_hit}/${_wd_count} wan_data lcore(s) received session traffic (data-lcore rx deltas sum=${_wd_sum}) — RSS is not spreading"
        fi
        if (( _wc_delta * 4 > _wd_sum )); then
            _rss_issue="${_rss_issue:+${_rss_issue}; }wan_ctrl rx delta=${_wc_delta} is not clearly below the data-lcore total=${_wd_sum} — session data is landing on queue 0"
        fi
    fi

    _p25_stop_echo_server || true
    if [[ -z "$_rss_issue" ]]; then
        pass "Step 106b: RSS queue distribution" \
            "${_wd_hit}/${_wd_count} wan_data lcores took rx (deltas sum=${_wd_sum}); wan_ctrl delta=${_wc_delta}; flows_with_reply=${_flows_ok}/${_P25_RSS_FLOWS}"
    else
        fail "Step 106b: RSS queue distribution" "$_rss_issue"
    fi

    _cleanup_phase25_udp_icmp_traffic
    return 0
}
