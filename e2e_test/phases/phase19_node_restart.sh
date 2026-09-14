#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 19 — Node Restart Recovery
# ---------------------------------------------------------------------------

# Verdict on the node record a registration wrote: pass | unreadable |
# no_timestamps | heartbeat_advanced | no_host_os.
e2e_registration_host_os_verdict() {
    local _record="${1:-}" _registered="" _last_seen="" _host_os=""

    if ! printf '%s' "$_record" | jq -e 'type == "object"' >/dev/null 2>&1; then
        printf 'unreadable'
        return 1
    fi
    _registered=$(printf '%s' "$_record" | jq -r '.registered_at // empty' 2>/dev/null || true)
    _last_seen=$(printf '%s' "$_record" | jq -r '.last_seen_time // empty' 2>/dev/null || true)
    _host_os=$(printf '%s' "$_record" | jq -r '.host_os // empty' 2>/dev/null || true)
    if ! [[ "$_registered" =~ ^[0-9]+$ ]] || ! [[ "$_last_seen" =~ ^[0-9]+$ ]]; then
        printf 'no_timestamps'
        return 1
    fi
    if [[ "$_last_seen" != "$_registered" ]]; then
        printf 'heartbeat_advanced'
        return 1
    fi
    if [[ -z "$_host_os" ]]; then
        printf 'no_host_os'
        return 1
    fi
    printf 'pass'
    return 0
}

local_validation_register registration_host_os_verdict e2e_registration_host_os_verdict \
    registration_host_os_good \
    registration_host_os_field_missing \
    registration_host_os_empty_value \
    registration_host_os_after_heartbeat \
    registration_host_os_no_timestamps \
    registration_host_os_unreadable \
    registration_host_os_no_record

# The record for one node out of a /api/nodes answer; empty when it is absent.
# An element either wraps the record in a "value" JSON string or is the record.
e2e_rest_node_record() {
    printf '%s' "${1:-}" | jq -c --arg u "${2:-}" \
        '[.[]? | if (type == "object" and has("value")) then ((.value | fromjson?) // {}) else . end]
         | map(select(.node_uuid == $u)) | first // empty' 2>/dev/null || true
}

local_validation_register rest_node_record e2e_rest_node_record \
    rest_node_record_wrapped \
    rest_node_record_plain \
    rest_node_record_other_nodes_only \
    rest_node_record_empty_array \
    rest_node_record_unreadable

_cleanup_phase19_node_restart() {
    local _p19_cleanup_stopped=0
    # Step 80 safety: never leave the node's etcd path blocked.
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

    e2e_start_node >/dev/null 2>&1 || true
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

# The controller's record for this node; wrapped so a drill can alter it.
_p19_node_record() {
    etcdctl_get_value "nodes/${NODE_UUID}" 2>/dev/null || true
}

# The same node record as the controller's REST API reports it; wrapped so a
# drill can alter it without touching the step that reads it.
_p19_rest_node_record() {
    e2e_rest_node_record "$(controller_rest_get /api/nodes || true)" "${NODE_UUID}"
}

# A node record's registered_at stamp; empty unless it is a number.
_p19_registered_at() {
    local _stamp

    _stamp=$(printf '%s' "${1:-}" | jq -r '.registered_at // empty' 2>/dev/null || true)
    [[ "$_stamp" =~ ^[0-9]+$ ]] && printf '%s' "$_stamp"
    return 0
}

# Drill: strip host_os from the record Step 77a reads, leaving the stamps.
_p19_inject_registration_host_os_missing() {
    sabotage_copy_function _p19_node_record _p19_node_record_real
    sabotage_override_function _p19_node_record \
        '_p19_node_record_real | jq -c "del(.host_os)" 2>/dev/null || true'
}

_p19_cleanup_registration_drill() {
    restore_phase_functions phase19_node_restart.sh
    _cleanup_phase19_node_restart
}

case_validation_register registration_host_os_missing phase19_node_restart \
    _p19_inject_registration_host_os_missing _p19_cleanup_registration_drill \
    'Step 77a:'

# Drill: same removal on the REST side, so the etcd reading alone cannot carry
# the step.
_p19_inject_registration_host_os_rest_missing() {
    sabotage_copy_function _p19_rest_node_record _p19_rest_node_record_real
    sabotage_override_function _p19_rest_node_record \
        '_p19_rest_node_record_real | jq -c "del(.host_os)" 2>/dev/null || true'
}

case_validation_register registration_host_os_rest_missing phase19_node_restart \
    _p19_inject_registration_host_os_rest_missing _p19_cleanup_registration_drill \
    'Step 77a:'

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
    local _p19_metrics=""
    local _p19_dns_started_at=0
    local _p19_dns_elapsed=0
    local _p19_dns_checked=0
    local _p19_dns_raw=""
    local _p19_dns_records=""
    local _p19_dns_detail=""
    local _p19_primary_status=""
    local _p19_restart_before=""
    local _p19_restart_after=""
    local _p19_start_before=""
    local _p19_start_after=""
    local _step73_issue=""
    local _step74_issue=""
    local _step76_issue=""
    local _step77a_issue=""
    local _p19_reg_before=""
    local _p19_reg_record=""
    local _p19_rest_record=""
    local _p19_rest_verdict=""
    local _p19_reg_stamp=""
    local _p19_reg_seen=0
    local _p19_reg_waited=0
    local _p19_reg_verdict=""
    local _p19_host_os=""
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
    bold " Phase 19 — Node Restart Recovery (Steps 76-80)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 76 — Snapshot the read-only recovery inputs, then stop the node
    # gracefully. No etcd write is permitted from this point through Step 79.
    # ------------------------------------------------------------------
    info "Step 76: Waiting for users 1 and 2 to be ready before the restart snapshot..."
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

    # fastrg_node_restart_total is a persisted process-start counter and
    # fastrg_node_start_time_seconds is the current process's start epoch. This
    # phase already performs a graceful stop + cold start, so both are sampled
    # across it here instead of restarting the node a second time elsewhere.
    _p19_metrics=$(e2e_metrics_body)
    _p19_restart_before=$(e2e_metric_value "$_p19_metrics" fastrg_node_restart_total)
    _p19_start_before=$(e2e_metric_value "$_p19_metrics" fastrg_node_start_time_seconds)

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
        pass "Step 76: Snapshot + graceful shutdown" \
            "users 1/2 Data phase; desire_status=connect; revisions=${_hsi1_rev_before}/${_hsi2_rev_before}/${_count_rev_before}; clean SIGTERM exit"
    else
        fail "Step 76: Snapshot + graceful shutdown" "${_step73_issue# }"
    fi

    # ------------------------------------------------------------------
    # Step 77 — Cold-start the exact phase0 command and wait for both users
    # to recover from the etcd desire_status without any dial/config call.
    # ------------------------------------------------------------------
    info "Step 77: Cold-starting fastrg and waiting up to 150s for autonomous recovery..."
    _p19_reg_before=$(_p19_registered_at "$(_p19_node_record)")
    _p19_dns_started_at=$(date +%s)
    if e2e_start_node >/dev/null 2>&1; then
        _restart_launched=1
    else
        _step74_issue="startup_command=failed"
    fi
    _FASTRG_STARTED_BY_SCRIPT=1

    if [[ $_restart_launched -eq 1 ]]; then
        # Step 77a reads here: the first heartbeat is 30s away and overwrites it.
        for _i in $(seq 1 30); do
            sleep 2
            _p19_reg_record=$(_p19_node_record)
            _p19_reg_stamp=$(_p19_registered_at "$_p19_reg_record")
            if [[ -n "$_p19_reg_stamp" && "$_p19_reg_stamp" != "$_p19_reg_before" ]]; then
                _p19_rest_record=$(_p19_rest_node_record)
                _p19_reg_seen=1
                break
            fi
        done
        _p19_reg_waited=$(( $(date +%s) - _p19_dns_started_at ))

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
            # Static DNS records are applied when a subscriber's config is
            # loaded, not when its session comes up, so they have to be there
            # the moment the primary subscriber reaches Data phase. Read once,
            # as early as possible: the 60s reconcile refills the table later
            # and would hide a boot path that never applied them.
            if [[ "$USER_ID" == "1" ]]; then
                _p19_primary_status="$_status1_after"
            else
                _p19_primary_status="$_status2_after"
            fi
            if [[ $_p19_dns_checked -eq 0 && "$_p19_primary_status" == "Data phase" ]]; then
                _p19_dns_elapsed=$(( $(date +%s) - _p19_dns_started_at ))
                _p19_dns_raw=$(fastrg_grpc get_dns_static "${USER_ID}" 2>/dev/null || true)
                _p19_dns_records=$(printf '%s' "$_p19_dns_raw" | \
                    jq -r '.entries[]?.domain' 2>/dev/null || true)
                _p19_dns_checked=1
            fi
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

        _p19_metrics=$(e2e_metrics_body)
        _p19_restart_after=$(e2e_metric_value "$_p19_metrics" fastrg_node_restart_total)
        _p19_start_after=$(e2e_metric_value "$_p19_metrics" fastrg_node_start_time_seconds)
        if ! e2e_all_uint "$_p19_restart_before" "$_p19_restart_after" \
                "$_p19_start_before" "$_p19_start_after"; then
            _step74_issue="${_step74_issue} restart_total='${_p19_restart_before:-NA}'->'${_p19_restart_after:-NA}' start_time='${_p19_start_before:-NA}'->'${_p19_start_after:-NA}'"
        else
            [[ "$_p19_restart_after" -ne $(( _p19_restart_before + 1 )) ]] && \
                _step74_issue="${_step74_issue} restart_total=${_p19_restart_before}->${_p19_restart_after} (expected +1 for one restart)"
            [[ "$_p19_start_after" -le "$_p19_start_before" ]] && \
                _step74_issue="${_step74_issue} start_time=${_p19_start_before}->${_p19_start_after} (expected to move forward)"
        fi
    fi

    if [[ $_p19_dns_checked -ne 1 ]]; then
        _p19_dns_detail="static DNS records not read (the primary subscriber never reached Data phase)"
    elif [[ "$_p19_dns_elapsed" -ge 55 ]]; then
        # Past the 60s reconcile the table says nothing about the boot path,
        # either way, so this reading is reported and not asserted on.
        _p19_dns_detail="dns check inconclusive (${_p19_dns_elapsed}s after start)"
    elif [[ -z "$_p19_dns_raw" ]]; then
        # An empty answer is the RPC failing, not an empty table: say so
        # rather than reporting a record that was never actually looked for.
        _step74_issue="${_step74_issue} dns_static_unreadable=${_p19_dns_elapsed}s"
        _p19_dns_detail="get_dns_static answered nothing ${_p19_dns_elapsed}s after start"
    elif printf '%s\n' "$_p19_dns_records" | grep -qxF 'www.fastrg.org'; then
        _p19_dns_detail="www.fastrg.org already in the static records ${_p19_dns_elapsed}s after start"
    else
        _step74_issue="${_step74_issue} dns_static_missing=${_p19_dns_elapsed}s records='${_p19_dns_records//$'\n'/,}'"
        _p19_dns_detail="www.fastrg.org missing from the static records ${_p19_dns_elapsed}s after start"
    fi

    if [[ -z "$_step74_issue" ]]; then
        pass "Step 77: Cold restart autonomous recovery" \
            "users 1/2 returned to Data phase with etcd account/vlan, without dial or config writes; restart_total ${_p19_restart_before}->${_p19_restart_after}, start_time ${_p19_start_before}->${_p19_start_after}; ${_p19_dns_detail}"
    else
        fail "Step 77: Cold restart autonomous recovery" \
            "${_step74_issue# }; ${_p19_dns_detail}"
    fi

    # ------------------------------------------------------------------
    # Step 77a — Registration carries host_os (read-only).
    # ------------------------------------------------------------------
    if [[ $_p19_reg_seen -ne 1 ]]; then
        _step77a_issue="no new registration within 60s of the cold start (registered_at stayed '${_p19_reg_before:-none}')"
    else
        _p19_reg_verdict=$(e2e_registration_host_os_verdict "$_p19_reg_record") || true
        _p19_rest_verdict=$(e2e_registration_host_os_verdict "$_p19_rest_record") || true
        _p19_host_os=$(printf '%s' "$_p19_reg_record" | jq -r '.host_os // empty' 2>/dev/null || true)
        [[ "$_p19_reg_verdict" != "pass" ]] && \
            _step77a_issue="etcd:${_p19_reg_verdict:-empty} record=$(printf '%s' "$_p19_reg_record" | tr '\n' ' ' | cut -c 1-200 || true)"
        [[ "$_p19_rest_verdict" != "pass" ]] && \
            _step77a_issue="${_step77a_issue:+${_step77a_issue}; }rest:${_p19_rest_verdict:-empty} record=$(printf '%s' "$_p19_rest_record" | tr '\n' ' ' | cut -c 1-200 || true)"
    fi

    if [[ -z "$_step77a_issue" ]]; then
        pass "Step 77a: host_os present at registration" \
            "etcd and REST both report host_os='${_p19_host_os}' for ${NODE_UUID} ${_p19_reg_waited}s after the cold start, last_seen_time still at registered_at=${_p19_reg_stamp}"
    else
        fail "Step 77a: host_os present at registration" "$_step77a_issue"
    fi

    # ------------------------------------------------------------------
    # Step 78 — Verify lazy DNS-static reload and data-plane forwarding.
    # ------------------------------------------------------------------
    if [[ "$USER_ID" == "1" ]]; then
        _gateway="$_hsi1_gateway"
    elif [[ "$USER_ID" == "2" ]]; then
        _gateway="$_hsi2_gateway"
    fi

    info "Step 78: Checking DNS static and ping after restart (gateway=${_gateway:-unknown})..."
    if [[ -n "$_gateway" ]]; then
        _dig=$(ssh_lan \
            "timeout 10 dig @${_gateway} +time=3 +tries=1 +short www.fastrg.org A" \
            2>/dev/null || true)
    fi
    _ping=$(ssh_lan "timeout 25 ping -c 4 -W 5 www.fastrg.org 2>&1" 2>/dev/null || true)

    if [[ "$_dig" == "${WAN_IP}" ]] && printf '%s' "$_ping" | grep -q "from ${WAN_IP}"; then
        pass "Step 78: Post-restart data plane" \
            "dig @${_gateway}=${WAN_IP}; LAN ping received reply from ${WAN_IP}"
    else
        fail "Step 78: Post-restart data plane" \
            "dig='${_dig:-empty}' gateway='${_gateway:-empty}'; ping_reply=$(printf '%s' "$_ping" | grep -oE 'from [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)"
    fi

    # ------------------------------------------------------------------
    # Step 79 — Re-read only: startup must not mutate HSI or count keys.
    # ------------------------------------------------------------------
    info "Step 79: Re-reading etcd revisions and system subscriber count..."
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
        pass "Step 79: Startup path keeps etcd read-only" \
            "HSI/count revisions unchanged (${_hsi1_rev_after}/${_hsi2_rev_after}/${_count_rev_after}); num_users=2"
    else
        fail "Step 79: Startup path keeps etcd read-only" "${_step76_issue# }"
    fi

    # ------------------------------------------------------------------
    # Step 80 — Restart with etcd unreachable: the persisted snapshot
    # (/etc/fastrg/config_snapshot.json) is the operating base. The node must
    # boot, apply both subscribers' configs from the snapshot and re-establish
    # PPPoE — all while etcd is blocked. etcd connectivity is then restored
    # and the watchers recover normal sync.
    # ------------------------------------------------------------------
    info "Step 80: restart with etcd unreachable — snapshot is the operating base..."
    local _step79_issue=""
    local _p19_etcd_host="${ETCD_ENDPOINT%%:*}"
    local _p19_etcd_port="${ETCD_ENDPOINT##*:}"
    _P19_ETCD_BLOCKED=0

    if ! ssh_node "command -v iptables >/dev/null 2>&1"; then
        fail "Step 80: snapshot is the boot base while etcd is down" "iptables not available on node"
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
        fail "Step 80: snapshot is the boot base while etcd is down" "fastrg did not stop within 20s"
        return
    fi
    ssh_node "iptables -I OUTPUT 1 -p tcp -d ${_p19_etcd_host} --dport ${_p19_etcd_port} -j REJECT --reject-with tcp-reset" \
        >/dev/null 2>&1 && _P19_ETCD_BLOCKED=1
    if [[ $_P19_ETCD_BLOCKED -ne 1 ]]; then
        fail "Step 80: snapshot is the boot base while etcd is down" "failed to install iptables block"
        _cleanup_phase19_node_restart || true
        return
    fi

    if ! e2e_start_node >/dev/null 2>&1; then
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
        pass "Step 80: snapshot is the boot base while etcd is down" \
            "cold start with etcd blocked: both users restored from snapshot to Data phase, num_users=2; etcd sync recovered after unblock"
    else
        fail "Step 80: snapshot is the boot base while etcd is down" "$_step79_issue"
    fi
}
