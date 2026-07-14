#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 25 — Sustained UDP / ICMP traffic across NAT timeout (Steps 103-105)
# ---------------------------------------------------------------------------

_P25_IPERF_PORT=5902
_P25_METRICS_PORT=""

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
    return 0
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
    bold " Phase 25 — Sustained UDP / ICMP Traffic (Steps 103-105)"
    bold "═══════════════════════════════════════════════════════"

    local _i _server_ready=0
    local _entries_base="" _entries_after="" _alloc_base="" _alloc_after=""
    local _iperf_out="" _iperf_rc=0 _parsed="" _lost="" _bytes="" _client_stopped=0
    local _ping_out="" _ping_retry_out="" _ping_loss="" _entries_delta=""

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

    info "Step 103: running 20s UDP LAN→WAN flow at 50 Mbps..."
    _entries_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")
    _alloc_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_alloc_fail_total")
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
    _parsed=$(_p25_parse_udp_json "$_iperf_out")
    IFS=$'\t' read -r _lost _bytes <<< "$_parsed" || true
    _client_stopped=0
    _p25_stop_client && _client_stopped=1
    if [[ $_iperf_rc -eq 0 && $_client_stopped -eq 1 ]] && \
       _p25_udp_result_ok "$_lost" "$_bytes" && \
       _p25_is_uint "$_entries_base" && _p25_is_uint "$_entries_after" && \
       _p25_is_uint "$_alloc_base" && [[ "$_alloc_after" == "$_alloc_base" ]] && \
       (( _entries_after - _entries_base <= 3 )); then
        pass "Step 103: sustained UDP LAN→WAN" \
            "loss=${_lost}%, bytes=${_bytes}, entries delta=$(( _entries_after - _entries_base )), alloc_fail delta=0"
    else
        fail "Step 103: sustained UDP LAN→WAN" \
            "server=${_server_ready} rc=${_iperf_rc} stopped=${_client_stopped}; loss=${_lost:-NA}% bytes=${_bytes:-NA}; entries=${_entries_base:-NA}->${_entries_after:-NA}; alloc_fail=${_alloc_base:-NA}->${_alloc_after:-NA}; output='$(_p25_snippet "$_iperf_out")'"
    fi

    info "Step 104: running 20s reverse UDP WAN→LAN flow at 50 Mbps..."
    _alloc_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_alloc_fail_total")
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
    _parsed=$(_p25_parse_udp_json "$_iperf_out")
    IFS=$'\t' read -r _lost _bytes <<< "$_parsed" || true
    _client_stopped=0
    _p25_stop_client && _client_stopped=1
    local _server_stopped=0
    _p25_stop_server && _server_stopped=1
    if [[ $_iperf_rc -eq 0 && $_client_stopped -eq 1 && $_server_stopped -eq 1 ]] && \
       _p25_udp_result_ok "$_lost" "$_bytes" && \
       _p25_is_uint "$_alloc_base" && [[ "$_alloc_after" == "$_alloc_base" ]]; then
        pass "Step 104: sustained reverse UDP WAN→LAN" \
            "loss=${_lost}%, bytes=${_bytes}, alloc_fail delta=0; full 20s reverse flow completed"
    else
        fail "Step 104: sustained reverse UDP WAN→LAN" \
            "server=${_server_ready}/${_server_stopped} rc=${_iperf_rc} stopped=${_client_stopped}; loss=${_lost:-NA}% bytes=${_bytes:-NA}; alloc_fail=${_alloc_base:-NA}->${_alloc_after:-NA}; output='$(_p25_snippet "$_iperf_out")'"
    fi

    info "Step 105: running approximately 15s of ICMP echo traffic..."
    _entries_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")
    _ping_out=$(ssh_lan "ping -c 150 -i 0.1 -W 2 ${WAN_IP}" 2>&1 || true)
    _ping_loss=$(_p25_ping_loss "$_ping_out")
    if [[ "$_ping_loss" != "0" && "$_ping_loss" != "0.0" ]]; then
        info "Step 105: first ping reported ${_ping_loss:-unparseable}% loss; retrying once..."
        _ping_retry_out=$(ssh_lan "ping -c 150 -i 0.1 -W 2 ${WAN_IP}" 2>&1 || true)
        _ping_loss=$(_p25_ping_loss "$_ping_retry_out")
    fi
    _entries_after=$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")
    if _p25_is_uint "$_entries_base" && _p25_is_uint "$_entries_after"; then
        _entries_delta=$(( _entries_after - _entries_base ))
    fi
    if [[ "$_ping_loss" == "0" || "$_ping_loss" == "0.0" ]] && \
       [[ "$_entries_delta" =~ ^-?[0-9]+$ ]] && (( _entries_delta <= 1 )); then
        pass "Step 105: sustained ICMP echo" \
            "loss=${_ping_loss}%, entries delta=${_entries_delta}; ident mapping survived beyond 10s"
    else
        fail "Step 105: sustained ICMP echo" \
            "loss=${_ping_loss:-NA}%; entries=${_entries_base:-NA}->${_entries_after:-NA}; first='$(_p25_snippet "$_ping_out")' retry='$(_p25_snippet "$_ping_retry_out")'"
    fi

    _cleanup_phase25_udp_icmp_traffic
    return 0
}
