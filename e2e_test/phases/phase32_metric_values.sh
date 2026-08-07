#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 32 — Prometheus metric value assertions (Steps 128-134)
#
# Phase 15 proves the /metrics route is reachable and well formed. This phase
# checks the numbers themselves for every family whose value can be verified
# from a quiescent node: the PPPoE phase tallies, the DHCP lease/pool gauges,
# the DPDK mempool/heap/hugepage gauges, the lcore cycle counters, the NIC
# metadata and the node start time.
#
# Families whose value only means something while traffic is flowing (per-user
# / per-session / NIC-port counters), or only across a link flap or a node
# restart, are asserted where that scenario already exists — phases 25, 27 and
# 19 respectively — instead of being re-created here.
#
# The phase runs last before the summary because that is where the fixture is
# back in its canonical steady state: phase 31 restores the subscriber count to
# 2 and its Step 127 has just proven both subscribers are in Data phase, which
# is exactly the precondition the tallies are checked against.
# ---------------------------------------------------------------------------

# Number of leasable addresses in a "start-end" (or "start~end") pool range:
# the network/broadcast addresses inside the range are never handed out, which
# is what fastrg_node_per_user_dhcp_max_lease_count reports.
_p32_pool_capacity() {
    local _pool="$1" _start="" _end=""

    _start=$(printf '%s' "$_pool" | awk -F'[-~]' '{print $1}' || true)
    _end=$(printf '%s' "$_pool" | awk -F'[-~]' '{print $2}' || true)
    awk -v s="$_start" -v e="$_end" '
        function to_int(ip,   a) {
            if (split(ip, a, ".") != 4)
                return -1
            return ((a[1] * 256 + a[2]) * 256 + a[3]) * 256 + a[4]
        }
        BEGIN {
            si = to_int(s); ei = to_int(e)
            if (si < 0 || ei < 0 || ei < si || ei - si > 65535)
                exit
            for (i = si; i <= ei; i++) {
                last = i % 256
                if (last != 0 && last != 255)
                    n++
            }
            printf "%d\n", n
        }' || true
}

# DHCP server status as the gRPC API reports it, used to cross-check the
# fastrg_node_total_*_dhcp_server tallies against a second source.
_p32_dhcp_running_count() {
    local _count="$1" _info="" _uid _status _running=0

    _info=$(fastrg_grpc get_dhcp_info 2>/dev/null || true)
    for (( _uid = 1; _uid <= _count; _uid++ )); do
        _status=$(printf '%s' "$_info" | \
            jq -r ".dhcp_infos[]? | select(.user_id == ${_uid}) | .status // empty" \
            2>/dev/null || true)
        [[ "$_status" == "DHCP server is on" ]] && _running=$(( _running + 1 ))
    done
    printf '%d' "$_running"
}

phase32_metric_values() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 32 — Prometheus Metric Values (Steps 128-134)"
    bold "═══════════════════════════════════════════════════════"

    local _body="" _body2="" _issue=""
    local _uc="" _max="" _now=""
    local _data="" _ipcp="" _auth="" _lcp="" _init="" _term="" _notcfg="" _err="" _sum=0
    local _uid="" _pool="" _want_cap="" _cur="" _max_lease=""
    local _run="" _stop="" _dncfg="" _grpc_running=""
    local _pools="" _pool_name="" _size="" _avail="" _in_use=""
    local _sockets="" _sid="" _total="" _used="" _free="" _largest="" _pinned="" _slack=0
    local _lcores="" _lid="" _busy1="" _tot1="" _busy2="" _tot2="" _advanced=0
    local _lrx="" _ltx="" _lsum="" _usum="" _uu="" _dir=""
    local _port="" _info_value="" _model="" _driver="" _pci="" _mac="" _start_time="" _persist_ok=""
    local _family="" _nic="" _v1="" _v2="" _checked=0

    _body=$(e2e_metrics_body)
    _uc=$(fastrg_grpc get_system_info 2>/dev/null | \
        jq -r '.num_users // empty' 2>/dev/null | tr -d '[:space:]' || true)
    _max=$(ssh_node "grep -E '^[[:space:]]*MaxUserCount[[:space:]]*=' /etc/fastrg/config.cfg 2>/dev/null" \
        2>/dev/null | awk -F'[=;]' '{gsub(/[[:space:]]/, "", $2); print $2; exit}' || true)

    if [[ -z "$_body" ]] || ! e2e_is_uint "$_uc" || [[ "$_uc" -lt 1 ]]; then
        _issue="metrics scrape empty or subscriber count unreadable (port='$(e2e_metrics_port)', num_users='${_uc:-empty}')"
        fail "Step 128: PPPoE phase tallies"        "$_issue"
        fail "Step 129: DHCP lease and server gauges" "$_issue"
        fail "Step 130: DPDK mempool accounting"    "$_issue"
        fail "Step 131: DPDK heap and hugepage"     "$_issue"
        fail "Step 132: lcore cycle counters"       "$_issue"
        fail "Step 133: NIC info and node start time" "$_issue"
        fail "Step 134: unknown-user and NIC error counters" "$_issue"
        return 0
    fi

    # ------------------------------------------------------------------
    # Step 128 — the eight PPPoE phase gauges partition the subscriber
    # slots: they must sum to user_count, and with the canonical fixture
    # fully connected every session sits in the Data phase.
    # ------------------------------------------------------------------
    _issue=""
    _data=$(e2e_metric_value "$_body" fastrg_node_total_pppoe_data_sessions)
    _ipcp=$(e2e_metric_value "$_body" fastrg_node_total_pppoe_ipcp_sessions)
    _auth=$(e2e_metric_value "$_body" fastrg_node_total_pppoe_auth_sessions)
    _lcp=$(e2e_metric_value "$_body" fastrg_node_total_pppoe_lcp_sessions)
    _init=$(e2e_metric_value "$_body" fastrg_node_total_pppoe_init_sessions)
    _term=$(e2e_metric_value "$_body" fastrg_node_total_pppoe_terminated_sessions)
    _notcfg=$(e2e_metric_value "$_body" fastrg_node_total_pppoe_not_configured_sessions)
    _err=$(e2e_metric_value "$_body" fastrg_node_total_pppoe_error_sessions)

    if ! e2e_all_uint "$_data" "$_ipcp" "$_auth" "$_lcp" "$_init" "$_term" "$_notcfg" "$_err"; then
        _issue="a phase gauge is missing (data='${_data:-NA}' ipcp='${_ipcp:-NA}' auth='${_auth:-NA}' lcp='${_lcp:-NA}' init='${_init:-NA}' terminated='${_term:-NA}' not_configured='${_notcfg:-NA}' error='${_err:-NA}')"
    else
        _sum=$(( _data + _ipcp + _auth + _lcp + _init + _term + _notcfg + _err ))
        [[ $_sum -ne $_uc ]] && \
            _issue="tally sum ${_sum} != user_count ${_uc}"
        [[ $_data -ne $_uc ]] && \
            _issue="${_issue:+${_issue}; }data_sessions=${_data}, expected ${_uc} (fixture fully connected)"
        if [[ $(( _ipcp + _auth + _lcp + _init + _term + _notcfg + _err )) -ne 0 ]]; then
            _issue="${_issue:+${_issue}; }non-Data phases are not all zero (ipcp=${_ipcp} auth=${_auth} lcp=${_lcp} init=${_init} terminated=${_term} not_configured=${_notcfg} error=${_err})"
        fi
    fi

    if [[ -z "$_issue" ]]; then
        pass "Step 128: PPPoE phase tallies" \
            "8 gauges sum to user_count=${_uc}; data=${_data}, every other phase 0 (error=0)"
    else
        fail "Step 128: PPPoE phase tallies" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 129 — per-subscriber lease gauges match the configured pool
    # capacity, and the three server-status gauges partition the slots and
    # agree with what the gRPC API reports.
    # ------------------------------------------------------------------
    _issue=""
    for _uid in "${SUB_IDS[@]}"; do
        _pool=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_uid}" 2>/dev/null | \
            jq -r '.config.dhcp_addr_pool // empty' 2>/dev/null || true)
        _want_cap=$(_p32_pool_capacity "$_pool")
        _cur=$(e2e_metric_value "$_body" fastrg_node_per_user_dhcp_cur_lease_count "user_id=${_uid}")
        _max_lease=$(e2e_metric_value "$_body" fastrg_node_per_user_dhcp_max_lease_count "user_id=${_uid}")
        if ! e2e_all_uint "$_want_cap" "$_cur" "$_max_lease"; then
            _issue="${_issue:+${_issue}; }user ${_uid}: pool='${_pool:-empty}' capacity='${_want_cap:-NA}' cur='${_cur:-NA}' max='${_max_lease:-NA}'"
            continue
        fi
        [[ "$_max_lease" -ne "$_want_cap" ]] && \
            _issue="${_issue:+${_issue}; }user ${_uid} max_lease=${_max_lease}, pool ${_pool} yields ${_want_cap} leasable addresses"
        [[ "$_cur" -gt "$_max_lease" ]] && \
            _issue="${_issue:+${_issue}; }user ${_uid} cur_lease=${_cur} exceeds max_lease=${_max_lease}"
    done

    _run=$(e2e_metric_value "$_body" fastrg_node_total_running_dhcp_server)
    _stop=$(e2e_metric_value "$_body" fastrg_node_total_stopped_dhcp_server)
    _dncfg=$(e2e_metric_value "$_body" fastrg_node_total_not_configured_dhcp_server)
    _grpc_running=$(_p32_dhcp_running_count "$_uc")
    if ! e2e_all_uint "$_run" "$_stop" "$_dncfg"; then
        _issue="${_issue:+${_issue}; }a DHCP server tally is missing (running='${_run:-NA}' stopped='${_stop:-NA}' not_configured='${_dncfg:-NA}')"
    else
        [[ $(( _run + _stop + _dncfg )) -ne $_uc ]] && \
            _issue="${_issue:+${_issue}; }DHCP tallies sum $(( _run + _stop + _dncfg )) != user_count ${_uc}"
        [[ "$_run" -ne "$_grpc_running" ]] && \
            _issue="${_issue:+${_issue}; }running=${_run} but gRPC reports ${_grpc_running} server(s) on"
        [[ "$_dncfg" -ne 0 ]] && \
            _issue="${_issue:+${_issue}; }not_configured=${_dncfg}, expected 0 (both canonical pools are configured)"
    fi

    if [[ -z "$_issue" ]]; then
        pass "Step 129: DHCP lease and server gauges" \
            "max_lease matches the configured pool capacity for users ${SUB_IDS[*]}; running/stopped/not_configured=${_run}/${_stop}/${_dncfg} sums to ${_uc} and matches gRPC"
    else
        fail "Step 129: DHCP lease and server gauges" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 130 — every mempool row is self-consistent, and the two CCB
    # pools show the fixed-max preallocation: all MaxUserCount objects are
    # taken at init and never returned while the node runs.
    # ------------------------------------------------------------------
    _issue=""
    _pools=$(printf '%s\n' "$_body" | awk '
        index($1, "fastrg_node_mempool_size{") == 1 && match($0, /pool="[^"]*"/) {
            print substr($0, RSTART + 6, RLENGTH - 7)
        }' || true)
    if [[ -z "$_pools" ]]; then
        _issue="no fastrg_node_mempool_size rows in the scrape"
    fi
    _checked=0
    for _pool_name in $_pools; do
        _size=$(e2e_metric_value "$_body" fastrg_node_mempool_size "pool=${_pool_name}")
        _avail=$(e2e_metric_value "$_body" fastrg_node_mempool_avail_count "pool=${_pool_name}")
        _in_use=$(e2e_metric_value "$_body" fastrg_node_mempool_in_use_count "pool=${_pool_name}")
        if ! e2e_all_uint "$_size" "$_avail" "$_in_use"; then
            _issue="${_issue:+${_issue}; }pool ${_pool_name}: size='${_size:-NA}' avail='${_avail:-NA}' in_use='${_in_use:-NA}'"
            continue
        fi
        _checked=$(( _checked + 1 ))
        [[ $(( _avail + _in_use )) -ne "$_size" ]] && \
            _issue="${_issue:+${_issue}; }pool ${_pool_name}: avail+in_use=$(( _avail + _in_use )) != size=${_size}"
        if [[ "$_pool_name" == "ppp_ccb_pool" || "$_pool_name" == "dhcp_ccb_pool" ]]; then
            if ! e2e_is_uint "$_max"; then
                _issue="${_issue:+${_issue}; }cannot read MaxUserCount from the node config to check ${_pool_name}"
            elif [[ "$_in_use" -ne "$_max" ]]; then
                _issue="${_issue:+${_issue}; }pool ${_pool_name}: in_use=${_in_use} != MaxUserCount=${_max} (fixed-max preallocation)"
            fi
        fi
    done
    for _pool_name in ppp_ccb_pool dhcp_ccb_pool; do
        printf '%s\n' "$_pools" | grep -qx "$_pool_name" || \
            _issue="${_issue:+${_issue}; }${_pool_name} is absent from the scrape"
    done

    if [[ -z "$_issue" ]]; then
        pass "Step 130: DPDK mempool accounting" \
            "${_checked} pool(s): avail+in_use == size; ppp_ccb_pool/dhcp_ccb_pool in_use == MaxUserCount=${_max}"
    else
        fail "Step 130: DPDK mempool accounting" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 131 — heap accounting per NUMA socket plus the hugepage gauge.
    # used+free is allowed a 0.1% slack rather than exact equality: the four
    # heap values come from one rte_malloc_get_socket_stats() call, but the
    # gauge contract only promises they describe the same heap.
    # ------------------------------------------------------------------
    _issue=""
    _sockets=$(printf '%s\n' "$_body" | awk '
        index($1, "fastrg_node_heap_total_bytes{") == 1 && match($0, /socket_id="[^"]*"/) {
            print substr($0, RSTART + 11, RLENGTH - 12)
        }' || true)
    [[ -n "$_sockets" ]] || _issue="no fastrg_node_heap_total_bytes rows in the scrape"
    for _sid in $_sockets; do
        _total=$(e2e_metric_value "$_body" fastrg_node_heap_total_bytes "socket_id=${_sid}")
        _used=$(e2e_metric_value "$_body" fastrg_node_heap_used_bytes "socket_id=${_sid}")
        _free=$(e2e_metric_value "$_body" fastrg_node_heap_free_bytes "socket_id=${_sid}")
        _largest=$(e2e_metric_value "$_body" fastrg_node_heap_largest_free_block_bytes "socket_id=${_sid}")
        if ! e2e_all_uint "$_total" "$_used" "$_free" "$_largest"; then
            _issue="${_issue:+${_issue}; }socket ${_sid}: total='${_total:-NA}' used='${_used:-NA}' free='${_free:-NA}' largest='${_largest:-NA}'"
            continue
        fi
        [[ "$_total" -le 0 ]] && \
            _issue="${_issue:+${_issue}; }socket ${_sid}: heap_total_bytes=${_total}"
        _slack=$(( _total / 1000 ))
        if [[ $(( _used + _free )) -gt $(( _total + _slack )) || \
              $(( _used + _free )) -lt $(( _total - _slack )) ]]; then
            _issue="${_issue:+${_issue}; }socket ${_sid}: used+free=$(( _used + _free )) is not within 0.1% of total=${_total}"
        fi
        [[ "$_largest" -gt "$_free" ]] && \
            _issue="${_issue:+${_issue}; }socket ${_sid}: largest_free_block=${_largest} > free=${_free}"
    done

    _pinned=$(e2e_metric_value "$_body" fastrg_node_hugepage_pinned_bytes)
    if ! e2e_is_uint "$_pinned" || [[ "$_pinned" -le 0 ]]; then
        _issue="${_issue:+${_issue}; }hugepage_pinned_bytes='${_pinned:-NA}', expected > 0"
    fi

    if [[ -z "$_issue" ]]; then
        pass "Step 131: DPDK heap and hugepage" \
            "socket(s) ${_sockets//$'\n'/,}: used+free ≈ total, largest_free_block <= free; hugepage_pinned_bytes=${_pinned}"
    else
        fail "Step 131: DPDK heap and hugepage" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 132 — lcore cycle counters over two samples. They are cumulative,
    # so neither may go backwards and busy can never exceed total. A lcore
    # that sees no packets legitimately keeps busy at its previous value, so
    # forward progress is required of the polling (total) counter instead,
    # and only for at least one lcore.
    # ------------------------------------------------------------------
    _issue=""
    sleep 2
    _body2=$(e2e_metrics_body)
    [[ -n "$_body2" ]] || _issue="second metrics scrape came back empty"
    _lcores=$(printf '%s\n' "$_body" | awk '
        index($1, "fastrg_node_lcore_total_cycles_total{") == 1 && match($0, /lcore_id="[^"]*"/) {
            print substr($0, RSTART + 10, RLENGTH - 11)
        }' || true)
    [[ -n "$_lcores" ]] || _issue="${_issue:+${_issue}; }no lcore rows in the scrape"
    _advanced=0
    for _lid in $_lcores; do
        _busy1=$(e2e_metric_value "$_body" fastrg_node_lcore_busy_cycles_total "lcore_id=${_lid}")
        _tot1=$(e2e_metric_value "$_body" fastrg_node_lcore_total_cycles_total "lcore_id=${_lid}")
        _busy2=$(e2e_metric_value "$_body2" fastrg_node_lcore_busy_cycles_total "lcore_id=${_lid}")
        _tot2=$(e2e_metric_value "$_body2" fastrg_node_lcore_total_cycles_total "lcore_id=${_lid}")
        if ! e2e_all_uint "$_busy1" "$_tot1" "$_busy2" "$_tot2"; then
            _issue="${_issue:+${_issue}; }lcore ${_lid}: busy='${_busy1:-NA}'->'${_busy2:-NA}' total='${_tot1:-NA}'->'${_tot2:-NA}'"
            continue
        fi
        [[ "$_busy2" -lt "$_busy1" ]] && \
            _issue="${_issue:+${_issue}; }lcore ${_lid} busy went backwards ${_busy1}->${_busy2}"
        [[ "$_tot2" -lt "$_tot1" ]] && \
            _issue="${_issue:+${_issue}; }lcore ${_lid} total went backwards ${_tot1}->${_tot2}"
        [[ "$_busy1" -gt "$_tot1" || "$_busy2" -gt "$_tot2" ]] && \
            _issue="${_issue:+${_issue}; }lcore ${_lid} busy exceeds total (${_busy1}/${_tot1}, ${_busy2}/${_tot2})"
        [[ "$_tot2" -gt "$_tot1" ]] && _advanced=$(( _advanced + 1 ))
    done
    [[ -n "$_lcores" && $_advanced -eq 0 ]] && \
        _issue="${_issue:+${_issue}; }no lcore advanced its total cycles across the two samples"

    # Per-lcore traffic rows: every lcore that reports cycles must also report
    # a numeric rx/tx packet row for each NIC port.
    for _lid in $_lcores; do
        for _nic in 0 1; do
            _lrx=$(e2e_metric_value "$_body" fastrg_node_lcore_rx_packets_total "lcore_id=${_lid}" "nic_index=${_nic}")
            _ltx=$(e2e_metric_value "$_body" fastrg_node_lcore_tx_packets_total "lcore_id=${_lid}" "nic_index=${_nic}")
            e2e_all_uint "$_lrx" "$_ltx" || \
                _issue="${_issue:+${_issue}; }lcore ${_lid} nic ${_nic}: traffic rows rx='${_lrx:-NA}' tx='${_ltx:-NA}'"
        done
    done
    # The per-lcore rows and the per-user (+unknown-user) rows read the same
    # per-lcore stats counters, just aggregated along different axes, so for
    # each port the two sums must agree. The families are gathered a moment
    # apart inside one scrape, so allow the same 100-packet skew slack the
    # NIC-vs-per-user containment checks use.
    for _nic in 0 1; do
        for _dir in rx tx; do
            _lsum=$(e2e_metric_sum "$_body" "fastrg_node_lcore_${_dir}_packets_total" "nic_index=${_nic}")
            _usum=$(e2e_metric_sum "$_body" "fastrg_node_per_user_${_dir}_packets_total" "nic_index=${_nic}")
            _uu=$(e2e_metric_value "$_body" "fastrg_node_unknown_user_${_dir}_packets_total" "nic_index=${_nic}")
            if ! e2e_all_uint "$_lsum" "$_usum" "$_uu"; then
                _issue="${_issue:+${_issue}; }nic ${_nic} ${_dir}: lcore/user sums unreadable (lcore='${_lsum:-NA}' user='${_usum:-NA}' unknown='${_uu:-NA}')"
                continue
            fi
            _usum=$(( _usum + _uu ))
            if (( _lsum > _usum + 100 || _lsum + 100 < _usum )); then
                _issue="${_issue:+${_issue}; }nic ${_nic} ${_dir}: lcore-axis sum ${_lsum} != user-axis sum ${_usum} (100-packet slack)"
            fi
        done
    done

    if [[ -z "$_issue" ]]; then
        pass "Step 132: lcore cycle counters" \
            "$(printf '%s\n' "$_lcores" | wc -l | tr -d '[:space:]') lcore(s) monotonic with busy <= total; ${_advanced} advanced total cycles between samples; per-lcore traffic rows numeric on both ports and lcore-axis sums match user-axis sums (100-packet slack)"
    else
        fail "Step 132: lcore cycle counters" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 133 — NIC info rows are proper info metrics (value 1, all labels
    # populated), and the process start time is a plausible past epoch.
    # ------------------------------------------------------------------
    _issue=""
    _now=$(ssh_node "date +%s" 2>/dev/null | tr -d '[:space:]' || true)
    for _port in 0 1; do
        _info_value=$(e2e_metric_value "$_body" fastrg_nic_info "port_id=${_port}")
        _model=$(e2e_metric_label "$_body" fastrg_nic_info model "port_id=${_port}")
        _driver=$(e2e_metric_label "$_body" fastrg_nic_info driver "port_id=${_port}")
        _pci=$(e2e_metric_label "$_body" fastrg_nic_info pci "port_id=${_port}")
        _mac=$(e2e_metric_label "$_body" fastrg_nic_info mac "port_id=${_port}")
        [[ "$_info_value" == "1" ]] || \
            _issue="${_issue:+${_issue}; }port ${_port} fastrg_nic_info='${_info_value:-NA}', expected 1"
        [[ -n "$_model" && -n "$_driver" && -n "$_pci" && -n "$_mac" ]] || \
            _issue="${_issue:+${_issue}; }port ${_port} labels incomplete (model='${_model}' driver='${_driver}' pci='${_pci}' mac='${_mac}')"
        [[ "$_mac" == "00:00:00:00:00:00" ]] && \
            _issue="${_issue:+${_issue}; }port ${_port} mac label is all zeroes"
    done

    _start_time=$(e2e_metric_value "$_body" fastrg_node_start_time_seconds)
    if ! e2e_is_uint "$_start_time" || ! e2e_is_uint "$_now"; then
        _issue="${_issue:+${_issue}; }start_time='${_start_time:-NA}' or node clock='${_now:-NA}' unreadable"
    elif [[ "$_start_time" -le 0 || "$_start_time" -gt "$_now" ]]; then
        _issue="${_issue:+${_issue}; }start_time=${_start_time} is not in (0, now=${_now}]"
    fi

    # The config snapshot persist gauge must read 1 on a healthy node: the
    # disk is writable, so the last snapshot persist (or the boot default
    # before any persist) reports success.
    _persist_ok=$(e2e_metric_value "$_body" fastrg_node_snapshot_persist_ok)
    [[ "$_persist_ok" == "1" ]] || \
        _issue="${_issue:+${_issue}; }snapshot_persist_ok='${_persist_ok:-NA}', expected 1"

    if [[ -z "$_issue" ]]; then
        pass "Step 133: NIC info and node start time" \
            "ports 0/1 expose fastrg_nic_info=1 with model/driver/pci/mac set; start_time=${_start_time} is $(( _now - _start_time ))s in the past; snapshot_persist_ok=1"
    else
        fail "Step 133: NIC info and node start time" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 134 — the counters no e2e scenario can reliably provoke. They are
    # cumulative gauges, so the contract that is checkable without a fault
    # injection is: the row exists on both ports and never goes backwards.
    # ------------------------------------------------------------------
    _issue=""
    _checked=0
    for _family in \
        fastrg_node_unknown_user_rx_packets_total \
        fastrg_node_unknown_user_rx_bytes_total \
        fastrg_node_unknown_user_tx_packets_total \
        fastrg_node_unknown_user_tx_bytes_total \
        fastrg_node_unknown_user_dropped_packets_total \
        fastrg_node_unknown_user_dropped_bytes_total \
        fastrg_node_rx_errors_total \
        fastrg_node_tx_errors_total \
        fastrg_node_rx_dropped_total; do
        for _nic in 0 1; do
            _v1=$(e2e_metric_value "$_body" "$_family" "nic_index=${_nic}")
            _v2=$(e2e_metric_value "$_body2" "$_family" "nic_index=${_nic}")
            if ! e2e_all_uint "$_v1" "$_v2"; then
                _issue="${_issue:+${_issue}; }${_family}{nic_index=${_nic}}='${_v1:-NA}'->'${_v2:-NA}'"
                continue
            fi
            _checked=$(( _checked + 1 ))
            [[ "$_v2" -lt "$_v1" ]] && \
                _issue="${_issue:+${_issue}; }${_family}{nic_index=${_nic}} went backwards ${_v1}->${_v2}"
        done
    done

    if [[ -z "$_issue" ]]; then
        pass "Step 134: unknown-user and NIC error counters" \
            "${_checked} row(s) across 9 families present on both ports and non-decreasing between samples"
    else
        fail "Step 134: unknown-user and NIC error counters" "$_issue"
    fi

    return 0
}
