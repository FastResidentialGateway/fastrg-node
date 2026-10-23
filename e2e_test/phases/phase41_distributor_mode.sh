#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 41 — Software distributor data plane (Steps 185-192)
#
# Restart the node with EnableDDP = false, check that traffic round-trips
# through the distributor workers, then restore the config and the old mode.
# ---------------------------------------------------------------------------

# Mode the node ran before this phase; kept across a drill's re-source.
: "${_P41_ORIG_MODE:=}"
: "${_P41_SHA_BEFORE:=}"

# Last subscriber statuses _p41_wait_sessions read.
_P41_SESSIONS=""

# Drill: the config switch silently does nothing, so the node restarts in RSS.
_p41_inject_switch_skipped() {
    sabotage_override_function e2e_node_ddp_off 'printf "switch skipped"'
}

_p41_cleanup_switch_skipped() {
    restore_phase_functions datapath_mode_lib.sh
    _cleanup_phase41_distributor_mode
}

case_validation_register distributor_switch_skipped phase41_distributor_mode \
    _p41_inject_switch_skipped _p41_cleanup_switch_skipped 'Step 185:'

# Idempotent: called at the end of phase41 and from the top-level EXIT trap.
_cleanup_phase41_distributor_mode() {
    local _restore="" _mode=""

    _e2e_rss_stop_server >/dev/null 2>&1 || true
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" >/dev/null 2>&1 || true

    _restore=$(e2e_node_ddp_restore) || true
    case "$_restore" in
        none) ;;
        restored) info "Cleanup(phase41): /etc/fastrg/config.cfg restored from the saved copy." ;;
        *)
            warn "Cleanup(phase41): could not restore /etc/fastrg/config.cfg; the original is still at /etc/fastrg/config.cfg.e2e-datapath-orig"
            return 0
            ;;
    esac

    # Only a node still running the switched config needs a restart.
    _mode=$(e2e_node_running_datapath_mode || true)
    if [[ "$_mode" == "distributor" && "$_restore" == "restored" ]] || \
       [[ -n "$_P41_ORIG_MODE" && "$_mode" != "err" && "$_mode" != "$_P41_ORIG_MODE" ]]; then
        info "Cleanup(phase41): restarting fastrg on the restored config (now '${_mode}')..."
        if e2e_node_restart_for_datapath; then
            info "Cleanup(phase41): fastrg reports datapath mode '${_E2E_DP_RESTART_MODE}'."
        else
            warn "Cleanup(phase41): restart incomplete: ${_E2E_DP_RESTART_ISSUE}"
        fi
    fi
    return 0
}

# Wait up to 150s for every subscriber to reach Data phase.
_p41_wait_sessions() {
    local _i _hsi _uid _st _all

    for _i in $(seq 1 30); do
        _hsi=$(fastrg_grpc get_hsi_info)
        _all=1
        _P41_SESSIONS=""
        for _uid in "${SUB_IDS[@]}"; do
            _st=$(printf '%s' "$_hsi" | jq -r \
                ".hsi_infos[]? | select(.user_id == ${_uid}) | .status // empty" \
                2>/dev/null || true)
            _P41_SESSIONS="${_P41_SESSIONS:+${_P41_SESSIONS} }user${_uid}='${_st:-unreachable}'"
            [[ "$_st" == "Data phase" ]] || _all=0
        done
        [[ $_all -eq 1 ]] && return 0
        sleep 5
    done
    return 1
}

# RX delta of one lcore role on one port; empty when a scrape lacks the row.
_p41_role_rx_delta() {
    local _before="$1" _after="$2" _role="$3" _nic="$4" _b="" _a=""

    _b=$(e2e_metric_sum "$_before" fastrg_node_lcore_rx_packets_total "role=${_role}" "nic_index=${_nic}")
    _a=$(e2e_metric_sum "$_after" fastrg_node_lcore_rx_packets_total "role=${_role}" "nic_index=${_nic}")
    e2e_all_uint "$_b" "$_a" || return 0
    printf '%d' $(( _a - _b ))
}

# Issue text when either worker role saw no traffic between two scrapes.
_p41_worker_issue() {
    local _before="$1" _after="$2" _wan="" _lan=""

    _wan=$(_p41_role_rx_delta "$_before" "$_after" wan_dist_worker 1)
    _lan=$(_p41_role_rx_delta "$_before" "$_after" lan_dist_worker 0)
    if ! { e2e_counter_increased 0 "$_wan" && e2e_counter_increased 0 "$_lan"; }; then
        printf 'worker rx delta wan_dist_worker=%s lan_dist_worker=%s' "${_wan:-unreadable}" "${_lan:-unreadable}"
    fi
}

# Flows of one UDP echo run that got a reply; empty when no server came up.
_p41_udp_echo_flows() {
    local _family="$1" _server="$2" _out=""

    _e2e_rss_start_server "$_family" "$_server" || return 0
    _out=$(_e2e_rss_client "$_family" "$_server")
    printf '%s\n' "$_out" | sed -nE 's/.*flows_with_reply=([0-9]+).*/\1/p' | tail -n 1 || true
}

phase41_distributor_mode() {
    local _issue="" _state="" _owed="" _orig_line="" _sha_after="" _line_after=""
    local _off_line="" _mode="" _in_dist=0 _not_dist=""
    local _before="" _after="" _wk_issue="" _flows="" _used_before="" _used_after="" _bps=0
    local _wd_rows="" _ld_rows="" _data_rows="" _lan_data_rows="" _restore=""
    local _i _ok=0

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 41 — Software Distributor Data Plane (Steps 185-192)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 185 — restart the node with EnableDDP = false
    # ------------------------------------------------------------------
    info "Step 185: switching the node to the software distributor..."
    _issue=""
    _P41_ORIG_MODE=$(e2e_node_running_datapath_mode || true)
    _state=$(e2e_node_ddp_state)
    read -r _P41_SHA_BEFORE _owed _orig_line <<< "$_state" || true
    [[ "$_P41_ORIG_MODE" != "err" ]] || _issue="running node printed no datapath line"
    [[ "$_owed" == "clear" ]] || \
        _issue="${_issue:+${_issue}; }config switch state '${_owed:-unreadable}' before the phase started"
    if ! _off_line=$(e2e_node_ddp_off); then
        _issue="${_issue:+${_issue}; }config switch failed: $(printf '%s' "$_off_line" | tr '\n' ' ')"
    fi
    if ! e2e_node_restart_for_datapath; then
        _issue="${_issue:+${_issue}; }${_E2E_DP_RESTART_ISSUE}"
    fi
    _mode="$_E2E_DP_RESTART_MODE"
    if [[ "$_mode" == "distributor" ]]; then
        _in_dist=1
    else
        _issue="${_issue:+${_issue}; }node reports datapath mode '${_mode}', expected distributor"
    fi
    if [[ -z "$_issue" ]]; then
        pass "Step 185: node restarted on the software distributor" \
            "was ${_P41_ORIG_MODE} with '${_orig_line}'; now '${_off_line}', datapath: software distributor"
    else
        fail "Step 185: node restarted on the software distributor" \
            "${_issue}; original EnableDDP '${_orig_line:-unknown}', mode before '${_P41_ORIG_MODE:-unknown}'"
    fi
    _not_dist="the node is not running the software distributor (Step 185); nothing was measured"

    # ------------------------------------------------------------------
    # Step 186 — every subscriber dials back in
    # ------------------------------------------------------------------
    info "Step 186: waiting for every subscriber to reach Data phase..."
    if _p41_wait_sessions; then
        pass "Step 186: subscribers reconnect on the distributor" "${_P41_SESSIONS}"
    else
        fail "Step 186: subscribers reconnect on the distributor" "after 150s: ${_P41_SESSIONS}"
    fi

    # ------------------------------------------------------------------
    # Step 187 — the lcores carry the distributor roles
    # ------------------------------------------------------------------
    info "Step 187: reading the lcore roles from /metrics..."
    _before=$(e2e_metrics_body)
    # A role with no lcore prints no row count at all, hence the :-0.
    _wd_rows=$(e2e_metric_rows "$_before" fastrg_node_lcore_total_cycles_total "role=wan_dist_worker")
    _ld_rows=$(e2e_metric_rows "$_before" fastrg_node_lcore_total_cycles_total "role=lan_dist_worker")
    _data_rows=$(e2e_metric_rows "$_before" fastrg_node_lcore_total_cycles_total "role=wan_data")
    _lan_data_rows=$(e2e_metric_rows "$_before" fastrg_node_lcore_total_cycles_total "role=lan_data")
    _data_rows=$(( ${_data_rows:-0} + ${_lan_data_rows:-0} ))
    if [[ -n "$_before" ]] && e2e_dist_worker_roles "${_wd_rows:-0}" "${_ld_rows:-0}" "$_data_rows"; then
        pass "Step 187: lcores run as distributor workers" \
            "wan_dist_worker=${_wd_rows} lan_dist_worker=${_ld_rows}, no wan_data/lan_data lcore"
    else
        fail "Step 187: lcores run as distributor workers" \
            "scrape=$([[ -n "$_before" ]] && echo ok || echo empty) wan_dist_worker=${_wd_rows:-0} lan_dist_worker=${_ld_rows:-0} wan_data+lan_data=${_data_rows}"
    fi

    # ------------------------------------------------------------------
    # Step 188 — IPv4 UDP through NAT round-trips via the workers
    # ------------------------------------------------------------------
    info "Step 188: ${_E2E_RSS_FLOWS} IPv4 UDP echo flows LAN→WAN→LAN..."
    if [[ $_in_dist -eq 1 ]]; then
        _issue=""
        _used_before=$(_p36_nat_used)
        _before=$(e2e_metrics_body)
        _flows=$(_p41_udp_echo_flows 4 "${WAN_IP}")
        _after=$(e2e_metrics_body)
        _used_after=$(_p36_nat_used)
        _e2e_rss_stop_server || true
        e2e_all_flows_replied "$_flows" "$_E2E_RSS_FLOWS" || \
            _issue="flows_with_reply='${_flows:-no echo server}' of ${_E2E_RSS_FLOWS}"
        if ! e2e_counter_increased "$_used_before" "$_used_after"; then
            _issue="${_issue:+${_issue}; }nat_entries_used ${_used_before:-unreadable}->${_used_after:-unreadable}"
        fi
        _wk_issue=$(_p41_worker_issue "$_before" "$_after")
        [[ -z "$_wk_issue" ]] || _issue="${_issue:+${_issue}; }${_wk_issue}"
        if [[ -z "$_issue" ]]; then
            pass "Step 188: IPv4 UDP round-trips through the distributor" \
                "${_flows}/${_E2E_RSS_FLOWS} flows echoed; nat_entries_used ${_used_before}->${_used_after}"
        else
            fail "Step 188: IPv4 UDP round-trips through the distributor" "$_issue"
        fi
    else
        fail "Step 188: IPv4 UDP round-trips through the distributor" "$_not_dist"
    fi

    # ------------------------------------------------------------------
    # Step 189 — IPv4 TCP through NAT carries data via the workers
    # ------------------------------------------------------------------
    info "Step 189: iperf3 TCP LAN→WAN..."
    if [[ $_in_dist -eq 1 ]]; then
        _issue=""
        _before=$(e2e_metrics_body)
        if _p36_start_iperf_server; then
            _bps=$(_p36_iperf_bps)
        else
            _bps=0
            _P36_IPERF_ERR="iperf3 server never listened on ${SRV_PORT}"
        fi
        _after=$(e2e_metrics_body)
        ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" >/dev/null 2>&1 || true
        e2e_counter_increased 0 "$_bps" || _issue="no TCP throughput (${_P36_IPERF_ERR:-unknown})"
        _wk_issue=$(_p41_worker_issue "$_before" "$_after")
        [[ -z "$_wk_issue" ]] || _issue="${_issue:+${_issue}; }${_wk_issue}"
        if [[ -z "$_issue" ]]; then
            pass "Step 189: IPv4 TCP flows through the distributor" "$(_p36_mbps "$_bps") Mbps"
        else
            fail "Step 189: IPv4 TCP flows through the distributor" "$_issue"
        fi
    else
        fail "Step 189: IPv4 TCP flows through the distributor" "$_not_dist"
    fi

    # ------------------------------------------------------------------
    # Step 190 — IPv6 UDP through the firewall round-trips via the workers
    # ------------------------------------------------------------------
    info "Step 190: IPv6 fixture, then ${_E2E_RSS_FLOWS} IPv6 UDP echo flows..."
    if [[ $_in_dist -eq 1 ]]; then
        _p37_ipv6_fixture_up || true
        _issue="$_P37_FIXTURE_ISSUE"
        # The echo burst lasts a second; get the path answering before it.
        _ok=0
        for _i in $(seq 1 6); do
            if _p37_ping6_ok; then
                _ok=1
                break
            fi
            sleep 5
        done
        [[ $_ok -eq 1 ]] || _issue="${_issue:+${_issue}; }ping6 ${_P37_WAN6_HOST_ADDR}: ${_P37_PING_DETAIL:-no response}"
        _used_before=$(_p37_metric fastrg_node_per_user_ipv6_firewall_entries_used)
        _before=$(e2e_metrics_body)
        _flows=$(_p41_udp_echo_flows 6 "${_P37_WAN6_HOST_ADDR}")
        _after=$(e2e_metrics_body)
        _used_after=$(_p37_metric fastrg_node_per_user_ipv6_firewall_entries_used)
        _e2e_rss_stop_server || true
        e2e_all_flows_replied "$_flows" "$_E2E_RSS_FLOWS" || \
            _issue="${_issue:+${_issue}; }flows_with_reply='${_flows:-no echo server}' of ${_E2E_RSS_FLOWS}"
        if ! e2e_counter_increased "$_used_before" "$_used_after"; then
            _issue="${_issue:+${_issue}; }ipv6_firewall_entries_used ${_used_before:-unreadable}->${_used_after:-unreadable}"
        fi
        _wk_issue=$(_p41_worker_issue "$_before" "$_after")
        [[ -z "$_wk_issue" ]] || _issue="${_issue:+${_issue}; }${_wk_issue}"
        _cleanup_phase37_ipv6_firewall || true
        if [[ -z "$_issue" ]]; then
            pass "Step 190: IPv6 UDP round-trips through the distributor" \
                "${_flows}/${_E2E_RSS_FLOWS} flows echoed from ${_P37_LAN6}; ipv6_firewall_entries_used ${_used_before}->${_used_after}"
        else
            fail "Step 190: IPv6 UDP round-trips through the distributor" "$_issue"
        fi
    else
        fail "Step 190: IPv6 UDP round-trips through the distributor" "$_not_dist"
    fi

    # ------------------------------------------------------------------
    # Step 191 — the config and the mode are back to what they were
    # ------------------------------------------------------------------
    info "Step 191: restoring the node config and its original mode..."
    _issue=""
    _restore=$(e2e_node_ddp_restore) || _issue="config restore ${_restore}"
    _state=$(e2e_node_ddp_state)
    read -r _sha_after _owed _line_after <<< "$_state" || true
    [[ -n "$_P41_SHA_BEFORE" && "$_sha_after" == "$_P41_SHA_BEFORE" ]] || \
        _issue="${_issue:+${_issue}; }config sha256 ${_sha_after:-unreadable}, was ${_P41_SHA_BEFORE:-unreadable}"
    [[ "$_owed" == "clear" ]] || _issue="${_issue:+${_issue}; }saved copy still on the node"
    _mode=$(e2e_node_running_datapath_mode || true)
    if [[ "$_restore" == "restored" || "$_mode" != "$_P41_ORIG_MODE" ]]; then
        e2e_node_restart_for_datapath || _issue="${_issue:+${_issue}; }${_E2E_DP_RESTART_ISSUE}"
        _mode="$_E2E_DP_RESTART_MODE"
    fi
    [[ "$_mode" == "$_P41_ORIG_MODE" ]] || \
        _issue="${_issue:+${_issue}; }node reports datapath mode '${_mode}', was '${_P41_ORIG_MODE:-unknown}'"
    if [[ -z "$_issue" ]]; then
        pass "Step 191: config and data-plane mode restored" \
            "config ${_restore}, sha256 ${_sha_after} as before; EnableDDP '${_line_after}'; mode ${_mode}"
    else
        fail "Step 191: config and data-plane mode restored" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 192 — every subscriber dials back in on the original mode
    # ------------------------------------------------------------------
    info "Step 192: waiting for every subscriber to reach Data phase..."
    if _p41_wait_sessions; then
        pass "Step 192: subscribers reconnect on the original mode" "${_P41_SESSIONS}"
    else
        fail "Step 192: subscribers reconnect on the original mode" "after 150s: ${_P41_SESSIONS}"
    fi

    _cleanup_phase41_distributor_mode
    return 0
}
