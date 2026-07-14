#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 26 — Controller Heartbeat Recovery (Steps 104-106)
#
# Block only node -> controller gRPC traffic on TCP/50051, verify the data
# plane remains healthy, then verify heartbeat recovery and controller
# monitoring reconstruction after a controller pod restart.
# ---------------------------------------------------------------------------

_P26_CONTROLLER_HOST=""
_P26_CONTROLLER_PORT=50051
_p26_iptables_blocked=0
_p26_rollout_started=0
_P26_ROLLOUT_OLD_POD=""
_P26_PING_PID_FILE=/tmp/fastrg_e2e_phase26_ping.pid
_P26_PING_OUT_FILE=/tmp/fastrg_e2e_phase26_ping.out
_P26_PING_RC_FILE=/tmp/fastrg_e2e_phase26_ping.rc
_P26_PING_DETAIL=""

_p26_remove_heartbeat_block() {
    if [[ -n "${_P26_CONTROLLER_HOST:-}" ]]; then
        ssh_node "while iptables -C OUTPUT -p tcp -d ${_P26_CONTROLLER_HOST} --dport ${_P26_CONTROLLER_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null; do iptables -D OUTPUT -p tcp -d ${_P26_CONTROLLER_HOST} --dport ${_P26_CONTROLLER_PORT} -j REJECT --reject-with tcp-reset || break; done" \
            >/dev/null 2>&1 || true
    fi
    _p26_iptables_blocked=0
    return 0
}

_p26_install_heartbeat_block() {
    _p26_remove_heartbeat_block
    if ssh_node "iptables -I OUTPUT 1 -p tcp -d ${_P26_CONTROLLER_HOST} --dport ${_P26_CONTROLLER_PORT} -j REJECT --reject-with tcp-reset" \
        >/dev/null 2>&1; then
        _p26_iptables_blocked=1
        return 0
    fi
    _p26_iptables_blocked=0
    return 1
}

_p26_controller_now() {
    ssh_controller "date -u +%Y-%m-%dT%H:%M:%S.%NZ" 2>/dev/null | tr -d '[:space:]'
}

_p26_controller_logs_since() {
    local _since="$1"
    ssh_controller \
        "kubectl -n fastrg-system logs deploy/fastrg-controller --since-time=${_since} --timestamps" 2>&1
}

_p26_hsi_status() {
    local _uid="$1"
    fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[] | select(.user_id == ${_uid}) | .status // empty" \
        2>/dev/null || true
}

_p26_rule_packets() {
    local _rules="" _packets=""
    _rules=$(ssh_node "iptables -L OUTPUT -n -v -x 2>/dev/null" 2>/dev/null || true)
    _packets=$(printf '%s\n' "$_rules" | awk \
        -v host="${_P26_CONTROLLER_HOST}" -v dport="dpt:${_P26_CONTROLLER_PORT}" \
        '$3 == "REJECT" && $9 == host && index($0, dport) {print $1; exit}' || true)
    [[ "$_packets" =~ ^[0-9]+$ ]] || _packets=0
    printf '%s' "$_packets"
}

_p26_ping_wan() {
    ssh_lan "ping -c 4 -W 3 ${WAN_IP} 2>&1" 2>/dev/null || true
}

_p26_start_continuous_ping() {
    _p26_stop_continuous_ping
    ssh_lan "rm -f '${_P26_PING_PID_FILE}' '${_P26_PING_OUT_FILE}' '${_P26_PING_RC_FILE}'; (ping -c 300 -i 0.2 -W 2 '${WAN_IP}' >'${_P26_PING_OUT_FILE}' 2>&1; echo \$? >'${_P26_PING_RC_FILE}') </dev/null >/dev/null 2>&1 & echo \$! >'${_P26_PING_PID_FILE}'" \
        >/dev/null 2>&1
}

_p26_stop_continuous_ping() {
    local _pid="" _i
    _pid=$(ssh_lan "cat '${_P26_PING_PID_FILE}' 2>/dev/null || true" 2>/dev/null | tr -d '[:space:]' || true)
    if [[ "$_pid" =~ ^[0-9]+$ ]] && ssh_lan "kill -0 ${_pid} 2>/dev/null" 2>/dev/null; then
        ssh_lan "kill -TERM ${_pid} 2>/dev/null || true" >/dev/null 2>&1 || true
        for _i in $(seq 1 5); do
            ssh_lan "kill -0 ${_pid} 2>/dev/null" 2>/dev/null || break
            sleep 1
        done
    fi
    ssh_lan "rm -f '${_P26_PING_PID_FILE}' '${_P26_PING_OUT_FILE}' '${_P26_PING_RC_FILE}'" \
        >/dev/null 2>&1 || true
    return 0
}

_p26_wait_continuous_ping() {
    local _pid="" _rc="" _out="" _i
    for _i in $(seq 1 75); do
        _pid=$(ssh_lan "cat '${_P26_PING_PID_FILE}' 2>/dev/null || true" \
            2>/dev/null | tr -d '[:space:]' || true)
        if [[ ! "$_pid" =~ ^[0-9]+$ ]] || \
           ! ssh_lan "kill -0 ${_pid} 2>/dev/null" 2>/dev/null; then
            break
        fi
        sleep 1
    done

    _rc=$(ssh_lan "cat '${_P26_PING_RC_FILE}' 2>/dev/null || true" \
        2>/dev/null | tr -d '[:space:]' || true)
    _out=$(ssh_lan "cat '${_P26_PING_OUT_FILE}' 2>/dev/null || true" 2>/dev/null || true)
    _P26_PING_DETAIL=$(printf '%s\n' "$_out" | tail -n 2 | tr '\n' ' ' | cut -c 1-300 || true)
    _p26_stop_continuous_ping

    [[ "$_rc" == "0" ]] && printf '%s\n' "$_out" | grep -q ', 0% packet loss'
}

_p26_restart_controller() {
    _P26_ROLLOUT_OLD_POD=$(ssh_controller \
        "kubectl -n fastrg-system get pods -l app=fastrg-controller --field-selector=status.phase=Running -o name | head -n 1" \
        2>/dev/null | tr -d '[:space:]' || true)
    [[ -n "$_P26_ROLLOUT_OLD_POD" ]] || return 1

    _p26_rollout_started=1
    # The single-node deployment binds hostPort, so a normal RollingUpdate
    # cannot schedule the replacement beside the old pod. Delete the pod that
    # was current before rollout restart to release those ports.
    ssh_controller \
        "kubectl -n fastrg-system rollout restart deploy/fastrg-controller && sleep 3 && kubectl -n fastrg-system delete '${_P26_ROLLOUT_OLD_POD}' --ignore-not-found --wait=false && kubectl -n fastrg-system rollout status deploy/fastrg-controller --timeout=180s"
}

_p26_wait_controller_healthy() {
    local _available=""
    if ! ssh_controller \
        "kubectl -n fastrg-system rollout status deploy/fastrg-controller --timeout=180s" \
        >/dev/null 2>&1; then
        return 1
    fi
    _available=$(ssh_controller \
        "kubectl -n fastrg-system get deploy fastrg-controller -o jsonpath='{.status.availableReplicas}'" \
        2>/dev/null | tr -d '[:space:]' || true)
    [[ "$_available" == "1" ]]
}

# Idempotent: called at the end of phase26 and from the top-level EXIT trap.
# Heartbeat connectivity is restored before waiting for controller health.
_cleanup_phase26_heartbeat() {
    _p26_remove_heartbeat_block
    _p26_stop_continuous_ping

    if [[ "${_p26_rollout_started:-0}" -eq 1 ]] && [[ -n "${_P26_ROLLOUT_OLD_POD:-}" ]]; then
        ssh_controller \
            "kubectl -n fastrg-system delete '${_P26_ROLLOUT_OLD_POD}' --ignore-not-found --wait=false" \
            >/dev/null 2>&1 || true
    fi
    if ! _p26_wait_controller_healthy; then
        warn "Cleanup(phase26): controller deployment is not healthy after 180s"
    fi
    _p26_rollout_started=0
    return 0
}

phase26_heartbeat() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 26 — Controller Heartbeat Recovery (Steps 104-106)"
    bold "═══════════════════════════════════════════════════════"

    local _block_since="" _recover_since="" _restart_since=""
    local _logs="" _uuid_logs="" _heartbeat_logs="" _monitor_logs=""
    local _status1="" _status2="" _ping="" _packets=0
    local _step104_issue="" _step105_issue="" _step106_issue=""
    local _logs_ok=0 _heartbeat_recovered=0 _monitor_recovered=0
    local _rollout_ok=0 _continuous_ping_ok=0 _i

    _P26_CONTROLLER_HOST="${CONTROLLER_GRPC%:*}"
    _p26_remove_heartbeat_block

    info "Step 104: blocking node -> controller TCP/50051 for 70s..."
    if ! ssh_node "command -v iptables >/dev/null 2>&1"; then
        _step104_issue="iptables is not available on the node"
    elif ! _p26_install_heartbeat_block; then
        _step104_issue="failed to install node->controller TCP/50051 REJECT rule"
    else
        _block_since=$(_p26_controller_now || true)
        [[ -n "$_block_since" ]] || _step104_issue="failed to read controller clock"
        for _i in $(seq 1 7); do
            sleep 10
            info "  heartbeat block active... $((_i * 10))s/70s"
        done

        _logs_ok=0
        if [[ -n "$_block_since" ]] && _logs=$(_p26_controller_logs_since "$_block_since"); then
            _logs_ok=1
        fi
        _uuid_logs=$(printf '%s\n' "$_logs" | grep -F "${NODE_UUID}" || true)
        _heartbeat_logs=$(printf '%s\n' "$_uuid_logs" | grep -F 'Heartbeat received from node' || true)
        _packets=$(_p26_rule_packets)
        _status1=$(_p26_hsi_status 1)
        _status2=$(_p26_hsi_status 2)
        _ping=$(_p26_ping_wan)

        [[ $_logs_ok -ne 1 ]] && _step104_issue="${_step104_issue} controller_log_query=failed"
        [[ -n "$_heartbeat_logs" ]] && _step104_issue="${_step104_issue} heartbeat_seen_during_block"
        [[ "$_packets" -le 0 ]] && _step104_issue="${_step104_issue} reject_rule_packets=${_packets}"
        [[ "$_status1" != "Data phase" ]] && _step104_issue="${_step104_issue} user1_status='${_status1:-empty}'"
        [[ "$_status2" != "Data phase" ]] && _step104_issue="${_step104_issue} user2_status='${_status2:-empty}'"
        printf '%s\n' "$_ping" | grep -q ', 0% packet loss' || \
            _step104_issue="${_step104_issue} LAN_ping_failed"
    fi

    if [[ -z "$_step104_issue" ]]; then
        pass "Step 104: heartbeat interruption preserves data plane" \
            "70s window: no heartbeat log for ${NODE_UUID}; TCP/50051 rejects=${_packets}; users 1/2 Data phase; LAN ping 0% loss"
    else
        fail "Step 104: heartbeat interruption preserves data plane" "${_step104_issue# }"
    fi

    info "Step 105: removing TCP/50051 block and waiting for heartbeat recovery..."
    _recover_since=$(_p26_controller_now || true)
    _p26_remove_heartbeat_block
    if [[ -z "$_recover_since" ]]; then
        _step105_issue="failed to read controller clock"
    else
        for _i in $(seq 1 30); do
            _logs_ok=0
            if _logs=$(_p26_controller_logs_since "$_recover_since"); then
                _logs_ok=1
            fi
            _uuid_logs=$(printf '%s\n' "$_logs" | grep -F "${NODE_UUID}" || true)
            _heartbeat_logs=$(printf '%s\n' "$_uuid_logs" | \
                grep -E 'Node registered successfully|Heartbeat received from node' || true)
            _monitor_logs=$(printf '%s\n' "$_uuid_logs" | \
                grep -E 'Started monitoring node|Already monitoring node' || true)
            if [[ $_logs_ok -eq 1 ]] && [[ -n "$_heartbeat_logs" ]]; then
                _heartbeat_recovered=1
                [[ -n "$_monitor_logs" ]] && _monitor_recovered=1
                break
            fi
            sleep 5
        done
        [[ $_logs_ok -ne 1 ]] && _step105_issue="controller_log_query=failed"
        [[ $_heartbeat_recovered -ne 1 ]] && \
            _step105_issue="${_step105_issue} no heartbeat or registration log within 150s"
    fi

    if [[ -z "$_step105_issue" ]]; then
        pass "Step 105: heartbeat recovery restores controller tracking" \
            "controller accepted ${NODE_UUID} heartbeat within 150s; monitoring_restart_log=${_monitor_recovered}"
    else
        fail "Step 105: heartbeat recovery restores controller tracking" "${_step105_issue# }"
    fi

    info "Step 106: restarting controller deployment and verifying autonomous monitoring recovery..."
    _restart_since=$(_p26_controller_now || true)
    _status1=$(_p26_hsi_status 1)
    _status2=$(_p26_hsi_status 2)
    [[ "$_status1" != "Data phase" ]] && _step106_issue="${_step106_issue} pre_user1_status='${_status1:-empty}'"
    [[ "$_status2" != "Data phase" ]] && _step106_issue="${_step106_issue} pre_user2_status='${_status2:-empty}'"
    if ! _p26_start_continuous_ping; then
        _step106_issue="${_step106_issue} continuous_ping_start=failed"
    fi

    if _p26_restart_controller; then
        _rollout_ok=1
        _p26_rollout_started=0
    else
        _step106_issue="${_step106_issue} controller_rollout=failed"
    fi

    _heartbeat_recovered=0
    _monitor_recovered=0
    if [[ $_rollout_ok -eq 1 ]] && [[ -n "$_restart_since" ]]; then
        for _i in $(seq 1 30); do
            _logs_ok=0
            if _logs=$(_p26_controller_logs_since "$_restart_since"); then
                _logs_ok=1
            fi
            _uuid_logs=$(printf '%s\n' "$_logs" | grep -F "${NODE_UUID}" || true)
            _heartbeat_logs=$(printf '%s\n' "$_uuid_logs" | \
                grep -E 'Node registered successfully|Heartbeat received from node' || true)
            _monitor_logs=$(printf '%s\n' "$_uuid_logs" | grep -F 'Started monitoring node' || true)
            [[ -n "$_heartbeat_logs" ]] && _heartbeat_recovered=1
            [[ -n "$_monitor_logs" ]] && _monitor_recovered=1
            if [[ $_logs_ok -eq 1 ]] && [[ $_heartbeat_recovered -eq 1 ]] && \
               [[ $_monitor_recovered -eq 1 ]]; then
                break
            fi
            sleep 5
        done
    fi

    if _p26_wait_continuous_ping; then
        _continuous_ping_ok=1
    else
        _step106_issue="${_step106_issue} continuous_ping_failed='${_P26_PING_DETAIL:-no output}'"
    fi
    _status1=$(_p26_hsi_status 1)
    _status2=$(_p26_hsi_status 2)
    [[ $_logs_ok -ne 1 ]] && _step106_issue="${_step106_issue} controller_log_query=failed"
    [[ $_heartbeat_recovered -ne 1 ]] && _step106_issue="${_step106_issue} heartbeat_registration_log=missing"
    [[ $_monitor_recovered -ne 1 ]] && _step106_issue="${_step106_issue} monitoring_start_log=missing"
    [[ "$_status1" != "Data phase" ]] && _step106_issue="${_step106_issue} post_user1_status='${_status1:-empty}'"
    [[ "$_status2" != "Data phase" ]] && _step106_issue="${_step106_issue} post_user2_status='${_status2:-empty}'"

    if [[ -z "$_step106_issue" ]]; then
        pass "Step 106: controller restart triggers monitoring recovery" \
            "rollout healthy; heartbeat + Started monitoring observed for ${NODE_UUID}; users 1/2 Data phase; continuous ping 0% loss"
    else
        fail "Step 106: controller restart triggers monitoring recovery" "${_step106_issue# }"
    fi

    _cleanup_phase26_heartbeat
    return 0
}
