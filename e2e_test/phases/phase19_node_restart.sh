#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 19 — Node Restart Recovery
# ---------------------------------------------------------------------------

_cleanup_phase19_node_restart() {
    local _p19_cleanup_stopped=0
    # Step 79 safety: never leave the node's etcd path blocked.
    if [[ "${_P19_ETCD_BLOCKED:-0}" -eq 1 ]] && [[ -n "${ETCD_ENDPOINT:-}" ]]; then
        ssh_node "iptables -D OUTPUT -p tcp -d ${ETCD_ENDPOINT%%:*} --dport ${ETCD_ENDPOINT##*:} -j REJECT --reject-with tcp-reset 2>/dev/null || true" \
            >/dev/null 2>&1 || true
        _P19_ETCD_BLOCKED=0
    fi
    local _p19_cleanup_started=0
    local _i

    if [[ "${_P19_RESTART_NEEDED:-0}" -ne 1 ]]; then
        return 0
    fi

    warn "Cleanup(phase19): restart recovery did not complete; retrying fastrg startup best-effort."
    if [[ "$(_p19_process_state)" == "running" ]]; then
        ssh_node "pkill -x fastrg" >/dev/null 2>&1 || true
        for _i in $(seq 1 15); do
            if [[ "$(_p19_process_state)" == "stopped" ]]; then
                _p19_cleanup_stopped=1
                break
            fi
            sleep 1
        done
        if [[ $_p19_cleanup_stopped -ne 1 ]]; then
            warn "Cleanup(phase19): existing fastrg did not exit after SIGTERM; startup retry skipped."
            return 0
        fi
    fi

    ssh_node "nohup ${_FASTRG_START_CMD} >/var/log/fastrg.log 2>&1 &" >/dev/null 2>&1 || true
    _FASTRG_STARTED_BY_SCRIPT=1
    for _i in $(seq 1 15); do
        if [[ "$(_p19_process_state)" == "running" ]]; then
            _p19_cleanup_started=1
            break
        fi
        sleep 1
    done

    if [[ $_p19_cleanup_started -eq 1 ]]; then
        info "Cleanup(phase19): fastrg startup retry launched successfully."
        _P19_RESTART_NEEDED=0
    else
        warn "Cleanup(phase19): fastrg startup retry did not launch a process."
    fi
    return 0
}

_p19_process_state() {
    ssh_node \
        "if pgrep -x fastrg >/dev/null 2>&1; then printf running; else printf stopped; fi" \
        2>/dev/null || true
}

_p19_etcd_snapshot() {
    local _key="$1"
    local _raw=""

    _raw=$(ssh_node \
        "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} get -w json ${_key}" \
        2>/dev/null || true)
    printf '%s' "$_raw" | jq -c \
        'if (.kvs | length) == 1 then
             {mod_revision: (.kvs[0].mod_revision | tostring),
              value: (.kvs[0].value | @base64d | fromjson)}
         else empty end' 2>/dev/null || true
}

phase19_node_restart() {
    local _hsi1_key="configs/${NODE_UUID}/hsi/1"
    local _hsi2_key="configs/${NODE_UUID}/hsi/2"
    local _count_key="user_counts/${NODE_UUID}/"
    local _hsi_before=""
    local _hsi_after=""
    local _hsi1_before=""
    local _hsi2_before=""
    local _count_before=""
    local _hsi1_after=""
    local _hsi2_after=""
    local _count_after=""
    local _hsi1_rev_before=""
    local _hsi2_rev_before=""
    local _count_rev_before=""
    local _hsi1_rev_after=""
    local _hsi2_rev_after=""
    local _count_rev_after=""
    local _hsi1_desire=""
    local _hsi2_desire=""
    local _hsi1_account=""
    local _hsi2_account=""
    local _hsi1_vlan=""
    local _hsi2_vlan=""
    local _hsi1_gateway=""
    local _hsi2_gateway=""
    local _count_value_before=""
    local _count_value_after=""
    local _status1_before=""
    local _status2_before=""
    local _status1_after=""
    local _status2_after=""
    local _account1_after=""
    local _account2_after=""
    local _vlan1_after=""
    local _vlan2_after=""
    local _step73_issue=""
    local _step74_issue=""
    local _step76_issue=""
    local _shutdown_done=0
    local _restart_launched=0
    local _recovery_ready=0
    local _gateway=""
    local _dig=""
    local _ping=""
    local _system_info=""
    local _num_users=""
    local _i

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 19 — Node Restart Recovery (Steps 75-79)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 75 — Snapshot the read-only recovery inputs, then stop the node
    # gracefully. No etcd write is permitted from this point through Step 78.
    # ------------------------------------------------------------------
    info "Step 75: Waiting for users 1 and 2 to be ready before the restart snapshot..."
    for _i in $(seq 1 30); do
        _hsi_before=$(fastrg_grpc get_hsi_info 2>/dev/null || true)
        _status1_before=$(printf '%s' "$_hsi_before" | \
            jq -r '.hsi_infos[] | select(.user_id == 1) | .status // empty' 2>/dev/null || true)
        _status2_before=$(printf '%s' "$_hsi_before" | \
            jq -r '.hsi_infos[] | select(.user_id == 2) | .status // empty' 2>/dev/null || true)
        if [[ "$_status1_before" == "Data phase" && "$_status2_before" == "Data phase" ]]; then
            break
        fi
        info "  waiting for restart precondition... (${_i}x5s, user1='${_status1_before:-unreachable}', user2='${_status2_before:-unreachable}')"
        sleep 5
    done

    info "  Snapshotting etcd revisions and recovery inputs..."

    _hsi1_before=$(_p19_etcd_snapshot "$_hsi1_key")
    _hsi2_before=$(_p19_etcd_snapshot "$_hsi2_key")
    _count_before=$(_p19_etcd_snapshot "$_count_key")
    _hsi1_rev_before=$(printf '%s' "$_hsi1_before" | jq -r '.mod_revision // empty' 2>/dev/null || true)
    _hsi2_rev_before=$(printf '%s' "$_hsi2_before" | jq -r '.mod_revision // empty' 2>/dev/null || true)
    _count_rev_before=$(printf '%s' "$_count_before" | jq -r '.mod_revision // empty' 2>/dev/null || true)
    _hsi1_desire=$(printf '%s' "$_hsi1_before" | jq -r '.value.config.desire_status // empty' 2>/dev/null || true)
    _hsi2_desire=$(printf '%s' "$_hsi2_before" | jq -r '.value.config.desire_status // empty' 2>/dev/null || true)
    _hsi1_account=$(printf '%s' "$_hsi1_before" | jq -r '.value.config.account_name // empty' 2>/dev/null || true)
    _hsi2_account=$(printf '%s' "$_hsi2_before" | jq -r '.value.config.account_name // empty' 2>/dev/null || true)
    _hsi1_vlan=$(printf '%s' "$_hsi1_before" | jq -r '.value.config.vlan_id // empty' 2>/dev/null || true)
    _hsi2_vlan=$(printf '%s' "$_hsi2_before" | jq -r '.value.config.vlan_id // empty' 2>/dev/null || true)
    _hsi1_gateway=$(printf '%s' "$_hsi1_before" | jq -r '.value.config.dhcp_gateway // empty' 2>/dev/null || true)
    _hsi2_gateway=$(printf '%s' "$_hsi2_before" | jq -r '.value.config.dhcp_gateway // empty' 2>/dev/null || true)
    _count_value_before=$(printf '%s' "$_count_before" | \
        jq -r '.value.subscriber_count // empty' 2>/dev/null | tr -d '[:space:]' || true)

    [[ "$_status1_before" != "Data phase" ]] && \
        _step73_issue="${_step73_issue} user1_status='${_status1_before:-empty}'"
    [[ "$_status2_before" != "Data phase" ]] && \
        _step73_issue="${_step73_issue} user2_status='${_status2_before:-empty}'"
    [[ -z "$_hsi1_rev_before" || -z "$_hsi1_account" || -z "$_hsi1_vlan" ]] && \
        _step73_issue="${_step73_issue} user1_snapshot=incomplete"
    [[ -z "$_hsi2_rev_before" || -z "$_hsi2_account" || -z "$_hsi2_vlan" ]] && \
        _step73_issue="${_step73_issue} user2_snapshot=incomplete"
    [[ -z "$_count_rev_before" || "$_count_value_before" != "2" ]] && \
        _step73_issue="${_step73_issue} subscriber_count='${_count_value_before:-empty}'"
    [[ "$_hsi1_desire" != "connect" ]] && \
        _step73_issue="${_step73_issue} user1_desire='${_hsi1_desire:-empty}'"
    [[ "$_hsi2_desire" != "connect" ]] && \
        _step73_issue="${_step73_issue} user2_desire='${_hsi2_desire:-empty}'"

    info "  Sending SIGTERM to fastrg and waiting up to 30s for a clean exit..."
    _P19_RESTART_NEEDED=1
    if ! ssh_node "pkill -x fastrg" >/dev/null 2>&1; then
        _step73_issue="${_step73_issue} SIGTERM_delivery=failed"
    fi
    for _i in $(seq 1 30); do
        if [[ "$(_p19_process_state)" == "stopped" ]]; then
            _shutdown_done=1
            break
        fi
        sleep 1
    done
    if [[ $_shutdown_done -ne 1 ]]; then
        _step73_issue="${_step73_issue} shutdown_timeout=30s"
    fi

    if [[ -z "$_step73_issue" ]]; then
        pass "Step 75: Snapshot + graceful shutdown" \
            "users 1/2 Data phase; desire_status=connect; revisions=${_hsi1_rev_before}/${_hsi2_rev_before}/${_count_rev_before}; clean SIGTERM exit"
    else
        fail "Step 75: Snapshot + graceful shutdown" "${_step73_issue# }"
    fi

    # ------------------------------------------------------------------
    # Step 76 — Cold-start the exact phase0 command and wait for both users
    # to recover from the etcd desire_status without any dial/config call.
    # ------------------------------------------------------------------
    info "Step 76: Cold-starting fastrg and waiting up to 150s for autonomous recovery..."
    if ssh_node "nohup ${_FASTRG_START_CMD} >/var/log/fastrg.log 2>&1 &" >/dev/null 2>&1; then
        _restart_launched=1
    else
        _step74_issue="startup_command=failed"
    fi
    _FASTRG_STARTED_BY_SCRIPT=1

    if [[ $_restart_launched -eq 1 ]]; then
        for _i in $(seq 1 30); do
            sleep 5
            _hsi_after=$(fastrg_grpc get_hsi_info 2>/dev/null || true)
            _status1_after=$(printf '%s' "$_hsi_after" | \
                jq -r '.hsi_infos[] | select(.user_id == 1) | .status // empty' 2>/dev/null || true)
            _status2_after=$(printf '%s' "$_hsi_after" | \
                jq -r '.hsi_infos[] | select(.user_id == 2) | .status // empty' 2>/dev/null || true)
            _account1_after=$(printf '%s' "$_hsi_after" | \
                jq -r '.hsi_infos[] | select(.user_id == 1) | .account // empty' 2>/dev/null || true)
            _account2_after=$(printf '%s' "$_hsi_after" | \
                jq -r '.hsi_infos[] | select(.user_id == 2) | .account // empty' 2>/dev/null || true)
            _vlan1_after=$(printf '%s' "$_hsi_after" | \
                jq -r '.hsi_infos[] | select(.user_id == 1) | .vlan_id // empty' 2>/dev/null || true)
            _vlan2_after=$(printf '%s' "$_hsi_after" | \
                jq -r '.hsi_infos[] | select(.user_id == 2) | .vlan_id // empty' 2>/dev/null || true)
            if [[ "$_status1_after" == "Data phase" && "$_status2_after" == "Data phase" && \
                  -n "$_account1_after" && -n "$_account2_after" ]]; then
                _recovery_ready=1
                _P19_RESTART_NEEDED=0
                break
            fi
            info "  still recovering... (${_i}x5s, user1='${_status1_after:-unreachable}', user2='${_status2_after:-unreachable}')"
        done
    fi

    if [[ $_recovery_ready -ne 1 ]]; then
        _step74_issue="${_step74_issue} recovery_timeout=150s user1='${_status1_after:-empty}' user2='${_status2_after:-empty}'"
    else
        [[ "$_account1_after" != "$_hsi1_account" ]] && \
            _step74_issue="${_step74_issue} user1_account='${_account1_after}' expected='${_hsi1_account}'"
        [[ "$_account2_after" != "$_hsi2_account" ]] && \
            _step74_issue="${_step74_issue} user2_account='${_account2_after}' expected='${_hsi2_account}'"
        [[ "$_vlan1_after" != "$_hsi1_vlan" ]] && \
            _step74_issue="${_step74_issue} user1_vlan='${_vlan1_after}' expected='${_hsi1_vlan}'"
        [[ "$_vlan2_after" != "$_hsi2_vlan" ]] && \
            _step74_issue="${_step74_issue} user2_vlan='${_vlan2_after}' expected='${_hsi2_vlan}'"
    fi

    if [[ -z "$_step74_issue" ]]; then
        pass "Step 76: Cold restart autonomous recovery" \
            "users 1/2 returned to Data phase with etcd account/vlan, without dial or config writes"
    else
        fail "Step 76: Cold restart autonomous recovery" "${_step74_issue# }"
    fi

    # ------------------------------------------------------------------
    # Step 77 — Verify lazy DNS-static reload and data-plane forwarding.
    # ------------------------------------------------------------------
    if [[ "$USER_ID" == "1" ]]; then
        _gateway="$_hsi1_gateway"
    elif [[ "$USER_ID" == "2" ]]; then
        _gateway="$_hsi2_gateway"
    fi

    info "Step 77: Checking DNS static and ping after restart (gateway=${_gateway:-unknown})..."
    if [[ -n "$_gateway" ]]; then
        _dig=$(ssh_lan \
            "timeout 10 dig @${_gateway} +time=3 +tries=1 +short www.fastrg.org A" \
            2>/dev/null || true)
    fi
    _ping=$(ssh_lan "timeout 25 ping -c 4 -W 5 www.fastrg.org 2>&1" 2>/dev/null || true)

    if [[ "$_dig" == "${WAN_IP}" ]] && printf '%s' "$_ping" | grep -q "from ${WAN_IP}"; then
        pass "Step 77: Post-restart data plane" \
            "dig @${_gateway}=${WAN_IP}; LAN ping received reply from ${WAN_IP}"
    else
        fail "Step 77: Post-restart data plane" \
            "dig='${_dig:-empty}' gateway='${_gateway:-empty}'; ping_reply=$(printf '%s' "$_ping" | grep -oE 'from [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)"
    fi

    # ------------------------------------------------------------------
    # Step 78 — Re-read only: startup must not mutate HSI or count keys.
    # ------------------------------------------------------------------
    info "Step 78: Re-reading etcd revisions and system subscriber count..."
    _hsi1_after=$(_p19_etcd_snapshot "$_hsi1_key")
    _hsi2_after=$(_p19_etcd_snapshot "$_hsi2_key")
    _count_after=$(_p19_etcd_snapshot "$_count_key")
    _hsi1_rev_after=$(printf '%s' "$_hsi1_after" | jq -r '.mod_revision // empty' 2>/dev/null || true)
    _hsi2_rev_after=$(printf '%s' "$_hsi2_after" | jq -r '.mod_revision // empty' 2>/dev/null || true)
    _count_rev_after=$(printf '%s' "$_count_after" | jq -r '.mod_revision // empty' 2>/dev/null || true)
    _count_value_after=$(printf '%s' "$_count_after" | \
        jq -r '.value.subscriber_count // empty' 2>/dev/null | tr -d '[:space:]' || true)
    _system_info=$(fastrg_grpc get_system_info 2>/dev/null || true)
    _num_users=$(printf '%s' "$_system_info" | jq -r '.num_users // empty' 2>/dev/null | tr -d '[:space:]' || true)

    [[ -z "$_hsi1_rev_before" || "$_hsi1_rev_after" != "$_hsi1_rev_before" ]] && \
        _step76_issue="${_step76_issue} user1_revision=${_hsi1_rev_before:-empty}->${_hsi1_rev_after:-empty}"
    [[ -z "$_hsi2_rev_before" || "$_hsi2_rev_after" != "$_hsi2_rev_before" ]] && \
        _step76_issue="${_step76_issue} user2_revision=${_hsi2_rev_before:-empty}->${_hsi2_rev_after:-empty}"
    [[ -z "$_count_rev_before" || "$_count_rev_after" != "$_count_rev_before" ]] && \
        _step76_issue="${_step76_issue} count_revision=${_count_rev_before:-empty}->${_count_rev_after:-empty}"
    [[ "$_count_value_before" != "2" || "$_count_value_after" != "2" ]] && \
        _step76_issue="${_step76_issue} etcd_count=${_count_value_before:-empty}->${_count_value_after:-empty}"
    [[ "$_num_users" != "2" ]] && \
        _step76_issue="${_step76_issue} grpc_num_users='${_num_users:-empty}'"

    if [[ -z "$_step76_issue" ]]; then
        pass "Step 78: Startup path keeps etcd read-only" \
            "HSI/count revisions unchanged (${_hsi1_rev_after}/${_hsi2_rev_after}/${_count_rev_after}); num_users=2"
    else
        fail "Step 78: Startup path keeps etcd read-only" "${_step76_issue# }"
    fi

    # ------------------------------------------------------------------
    # Step 79 — Restart with etcd unreachable: the persisted snapshot
    # (/etc/fastrg/config_snapshot.json) is the operating base. The node must
    # boot, apply both subscribers' configs from the snapshot and re-establish
    # PPPoE — all while etcd is blocked. etcd connectivity is then restored
    # and the watchers recover normal sync.
    # ------------------------------------------------------------------
    info "Step 79: restart with etcd unreachable — snapshot is the operating base..."
    local _step79_issue=""
    local _p19_etcd_host="${ETCD_ENDPOINT%%:*}"
    local _p19_etcd_port="${ETCD_ENDPOINT##*:}"
    _P19_ETCD_BLOCKED=0

    if ! ssh_node "command -v iptables >/dev/null 2>&1"; then
        fail "Step 79: snapshot is the boot base while etcd is down" "iptables not available on node"
        return
    fi

    # Stop gracefully, then block etcd BEFORE the cold start.
    _P19_RESTART_NEEDED=1
    ssh_node "pkill -x fastrg" >/dev/null 2>&1 || true
    for _i in $(seq 1 20); do
        [[ "$(_p19_process_state)" == "stopped" ]] && break
        sleep 1
    done
    if [[ "$(_p19_process_state)" != "stopped" ]]; then
        fail "Step 79: snapshot is the boot base while etcd is down" "fastrg did not stop within 20s"
        return
    fi
    ssh_node "iptables -I OUTPUT 1 -p tcp -d ${_p19_etcd_host} --dport ${_p19_etcd_port} -j REJECT --reject-with tcp-reset" \
        >/dev/null 2>&1 && _P19_ETCD_BLOCKED=1
    if [[ $_P19_ETCD_BLOCKED -ne 1 ]]; then
        fail "Step 79: snapshot is the boot base while etcd is down" "failed to install iptables block"
        _cleanup_phase19_node_restart || true
        return
    fi

    if ! ssh_node "nohup ${_FASTRG_START_CMD} >/var/log/fastrg.log 2>&1 &" >/dev/null 2>&1; then
        _step79_issue="cold start command failed"
    fi
    _FASTRG_STARTED_BY_SCRIPT=1

    # With etcd blocked, config can only come from the snapshot: both users
    # must reappear with their configs and reach Data phase (BRAS is not
    # affected by the etcd block).
    local _p79_ok=0 _s1="" _s2="" _nu=""
    if [[ -z "$_step79_issue" ]]; then
        for _i in $(seq 1 36); do
            sleep 5
            _s1=$(fastrg_grpc get_hsi_info 2>/dev/null | \
                jq -r '.hsi_infos[]? | select(.user_id == 1) | .status' 2>/dev/null || true)
            _s2=$(fastrg_grpc get_hsi_info 2>/dev/null | \
                jq -r '.hsi_infos[]? | select(.user_id == 2) | .status' 2>/dev/null || true)
            if [[ "$_s1" == "Data phase" && "$_s2" == "Data phase" ]]; then
                _p79_ok=1
                info "  ${_i}x5s: both users in Data phase from the snapshot base"
                break
            fi
            info "  ${_i}x5s: user1='${_s1:-none}' user2='${_s2:-none}'"
        done
        _nu=$(fastrg_grpc get_system_info 2>/dev/null | jq -r '.num_users // empty' 2>/dev/null || true)
        [[ $_p79_ok -eq 1 ]] || _step79_issue="users did not reach Data phase from the snapshot within 180s (user1='${_s1:-none}' user2='${_s2:-none}')"
        [[ "$_nu" == "2" ]] || _step79_issue="${_step79_issue:+${_step79_issue}; }num_users='${_nu:-empty}' (want 2 from snapshot count)"
    fi

    # Restore etcd connectivity; the watchers must recover live sync.
    # NB: etcd_client.cpp's own log lines are compiled out (NB_TEST), so
    # recovery is detected via etcd_integration.c's watch-event logs instead:
    # once the watchers are back, the controller's periodic user_counts
    # writeback (~60s) produces a fresh "User count change request received"
    # line. Mark the current end of the log and wait for a NEW line.
    local _p79_logmark
    _p79_logmark=$(ssh_node "wc -l < /var/log/fastrg/fastrg.log 2>/dev/null" || echo 0)
    [[ "$_p79_logmark" =~ ^[0-9]+$ ]] || _p79_logmark=0
    ssh_node "iptables -D OUTPUT -p tcp -d ${_p19_etcd_host} --dport ${_p19_etcd_port} -j REJECT --reject-with tcp-reset 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    _P19_ETCD_BLOCKED=0
    local _p79_sync=0
    for _i in $(seq 1 75); do
        sleep 2
        if ssh_node "tail -n +$(( _p79_logmark + 1 )) /var/log/fastrg/fastrg.log 2>/dev/null | grep -qE 'User count change request received|Reconcile: HSI user'"; then
            _p79_sync=1
            break
        fi
    done
    [[ $_p79_sync -eq 1 ]] || _step79_issue="${_step79_issue:+${_step79_issue}; }no etcd watch event observed within 150s of unblocking (watchers did not recover)"

    if [[ -z "$_step79_issue" ]]; then
        _P19_RESTART_NEEDED=0
        pass "Step 79: snapshot is the boot base while etcd is down" \
            "cold start with etcd blocked: both users restored from snapshot to Data phase, num_users=2; etcd sync recovered after unblock"
    else
        fail "Step 79: snapshot is the boot base while etcd is down" "$_step79_issue"
    fi
}
