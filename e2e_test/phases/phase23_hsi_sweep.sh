#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 23 — etcd HSI Sweep Reconcile (Steps 98-100)
#
# Block the node's path to etcd, remove a synthetic subscriber through the
# controller while the watch is offline, then verify the periodic reconcile
# sweep removes the stale local subscriber after connectivity returns.
# ---------------------------------------------------------------------------

_P23_UID=3
_P23_VLAN=210
_P23_CANONICAL_COUNT=2
_p23_iptables_blocked=0

_p23_block_etcd() {
    # Idempotent installation: discard a stale rule from an interrupted run.
    ssh_node "iptables -D OUTPUT -p tcp -d ${_P23_ETCD_HOST} --dport ${_P23_ETCD_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    if ssh_node "iptables -I OUTPUT 1 -p tcp -d ${_P23_ETCD_HOST} --dport ${_P23_ETCD_PORT} -j REJECT --reject-with tcp-reset" \
        >/dev/null 2>&1; then
        _p23_iptables_blocked=1
        return 0
    fi
    _p23_iptables_blocked=0
    return 1
}

_p23_unblock_etcd() {
    if [[ -n "${_P23_ETCD_HOST:-}" ]] && [[ -n "${_P23_ETCD_PORT:-}" ]]; then
        ssh_node "iptables -D OUTPUT -p tcp -d ${_P23_ETCD_HOST} --dport ${_P23_ETCD_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null || true" \
            >/dev/null 2>&1 || true
    fi
    _p23_iptables_blocked=0
    return 0
}

_p23_hsi_field() {
    local _uid="$1" _field="$2"
    fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[] | select(.user_id == ${_uid}) | .${_field} // empty" \
        2>/dev/null || true
}

_p23_log_count() {
    local _needle="$1" _count
    _count=$(ssh_node "grep -cF '${_needle}' '${_P23_LOG_PATH}' 2>/dev/null || true" \
        2>/dev/null | tail -1 | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

# Read-only observation that the fresh REJECT rule has seen node->etcd traffic
# and that no established TCP connection to the endpoint remains. This avoids
# probing etcd_reachable through a node gRPC write during the blocked window.
_p23_etcd_path_is_down() {
    local _rules="" _packets="" _established=""

    _rules=$(ssh_node "iptables -L OUTPUT -n -v -x 2>/dev/null" 2>/dev/null || true)
    _packets=$(printf '%s\n' "$_rules" | awk \
        -v host="${_P23_ETCD_HOST}" -v dport="dpt:${_P23_ETCD_PORT}" \
        '$3 == "REJECT" && $9 == host && index($0, dport) {print $1; exit}' || true)
    [[ "$_packets" =~ ^[0-9]+$ ]] || _packets=0
    [[ "$_packets" -gt 0 ]] || return 1

    _established=$(ssh_node "ss -Hnt state established 2>/dev/null" 2>/dev/null | \
        grep -F "${_P23_ETCD_HOST}:${_P23_ETCD_PORT}" || true)
    [[ -z "$_established" ]]
}

_p23_restore_count() {
    local _etcd_count="" _local_count="" _i

    fastrg_grpc set_subscriber_count "${_P23_CANONICAL_COUNT}" >/dev/null 2>&1 || true
    for _i in $(seq 1 15); do
        _etcd_count=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null | \
            jq -r '.subscriber_count // empty' 2>/dev/null || true)
        _local_count=$(fastrg_grpc get_system_info 2>/dev/null | \
            jq -r '.num_users // empty' 2>/dev/null || true)
        if [[ "$_etcd_count" == "${_P23_CANONICAL_COUNT}" ]] && \
           [[ "$_local_count" == "${_P23_CANONICAL_COUNT}" ]]; then
            return 0
        fi
        sleep 2
    done

    warn "Cleanup(phase23): subscriber count restore not observed (etcd='${_etcd_count:-empty}', local='${_local_count:-empty}')."
    return 1
}

# Idempotent: called at the end of phase23 and from the top-level EXIT trap.
# Connectivity is always restored before any controller cleanup write.
_cleanup_phase23_hsi_sweep() {
    _p23_unblock_etcd

    if [[ -n "${NODE_UUID:-}" ]]; then
        local _remaining
        _remaining=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P23_UID}" 2>/dev/null || true)
        if [[ -n "$_remaining" ]]; then
            info "Cleanup(phase23): removing residual user ${_P23_UID} config..."
            remove_hsi_config_verified "${_P23_UID}" || true
        fi
        _p23_restore_count || true
    fi
    return 0
}

phase23_hsi_sweep() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 23 — etcd HSI Sweep Reconcile (Steps 98-100)"
    bold "═══════════════════════════════════════════════════════"

    local _step95_ok=1 _step96_ok=1 _step97_ok=1
    local _issue95="" _issue96="" _issue97=""
    local _baseline="" _account="" _password=""
    local _count_reply="" _apply_reply="" _remove_reply=""
    local _etcd_count="" _local_count="" _local_vlan="" _local_account=""
    local _sweep_log_before=0 _sweep_log_after=0
    local _etcd_user3="" _status1="" _status2="" _keys="" _key_ids=""
    local _log_path_raw=""
    local _i

    _P23_ETCD_HOST="${ETCD_ENDPOINT%%:*}"
    _P23_ETCD_PORT="${ETCD_ENDPOINT##*:}"
    _log_path_raw=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" || true)
    _P23_LOG_PATH=$(printf '%s' "$_log_path_raw" | awk -F'"' '{print $2}' || true)
    [[ -n "$_P23_LOG_PATH" ]] || _P23_LOG_PATH=/var/log/fastrg.log

    if ! ssh_node "command -v iptables >/dev/null 2>&1"; then
        _step95_ok=0
        _issue95="iptables is not available on the node"
    elif [[ -z "$_P23_ETCD_HOST" || -z "$_P23_ETCD_PORT" || \
            "$_P23_ETCD_HOST" == "$ETCD_ENDPOINT" || "$_P23_ETCD_PORT" == "$ETCD_ENDPOINT" ]]; then
        _step95_ok=0
        _issue95="could not parse ETCD_ENDPOINT='${ETCD_ENDPOINT:-empty}'"
    fi

    # Seed user 3 through the controller while etcd is reachable.
    if [[ $_step95_ok -eq 1 ]]; then
        _baseline=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/2" 2>/dev/null || true)
        _account=$(printf '%s' "$_baseline" | jq -r '.config.account_name // empty' 2>/dev/null || true)
        _password=$(printf '%s' "$_baseline" | jq -r '.config.password // empty' 2>/dev/null || true)
        if [[ -z "$_account" || -z "$_password" ]]; then
            _step95_ok=0
            _issue95="user 2 baseline account/password is unavailable"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        info "Step 98: expanding to 3 subscribers and applying synthetic user 3 (VLAN 210)..."
        _count_reply=$(fastrg_grpc set_subscriber_count 3 2>/dev/null || true)
        if [[ -z "$(printf '%s' "$_count_reply" | jq -r '.status // empty' 2>/dev/null || true)" ]]; then
            _step95_ok=0
            _issue95="SetSubscriberCount returned no status: ${_count_reply:-empty}"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        for _i in $(seq 1 15); do
            _etcd_count=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null | \
                jq -r '.subscriber_count // empty' 2>/dev/null || true)
            _local_count=$(fastrg_grpc get_system_info 2>/dev/null | \
                jq -r '.num_users // empty' 2>/dev/null || true)
            [[ "$_etcd_count" == "3" && "$_local_count" == "3" ]] && break
            sleep 2
        done
        if [[ "$_etcd_count" != "3" || "$_local_count" != "3" ]]; then
            _step95_ok=0
            _issue95="subscriber count 3 did not converge (etcd='${_etcd_count:-empty}', local='${_local_count:-empty}')"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        _apply_reply=$(fastrg_grpc apply_config \
            3 210 "$_account" "$_password" \
            192.168.6.2 192.168.6.10 255.255.255.0 192.168.6.1 \
            2>/dev/null || true)
        if [[ -z "$(printf '%s' "$_apply_reply" | jq -r '.status // empty' 2>/dev/null || true)" ]]; then
            _step95_ok=0
            _issue95="ApplyConfig returned no status: ${_apply_reply:-empty}"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        for _i in $(seq 1 30); do
            _local_vlan=$(_p23_hsi_field 3 vlan_id)
            _local_account=$(_p23_hsi_field 3 account)
            [[ "$_local_vlan" == "210" && -n "$_local_account" ]] && break
            sleep 2
        done
        if [[ "$_local_vlan" != "210" || -z "$_local_account" ]]; then
            _step95_ok=0
            _issue95="user 3 did not become active locally (vlan='${_local_vlan:-empty}', account='${_local_account:-empty}')"
        fi
    fi

    # The blocked window contains exactly one write: controller remove_config 3.
    if [[ $_step95_ok -eq 1 ]]; then
        info "Step 98: blocking node->etcd (${_P23_ETCD_HOST}:${_P23_ETCD_PORT}) and waiting for watchdog offline detection..."
        if ! _p23_block_etcd; then
            _step95_ok=0
            _issue95="failed to install node->etcd iptables REJECT rule"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        for _i in $(seq 1 45); do
            sleep 2
            _p23_etcd_path_is_down && break
        done
        if ! _p23_etcd_path_is_down; then
            _step95_ok=0
            _issue95="node->etcd REJECT traffic/connection-down state was not observed within 90s"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        info "Step 98: deleting user 3 through the controller while the node watch is offline..."
        _remove_reply=$(fastrg_grpc remove_config 3 2>/dev/null || true)
        if [[ -z "$(printf '%s' "$_remove_reply" | jq -r '.status // empty' 2>/dev/null || true)" ]]; then
            _step95_ok=0
            _issue95="RemoveConfig returned no status: ${_remove_reply:-empty}"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        for _i in $(seq 1 15); do
            _etcd_user3=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/3" 2>/dev/null || true)
            [[ -z "$_etcd_user3" ]] && break
            sleep 2
        done
        _local_vlan=$(_p23_hsi_field 3 vlan_id)
        _local_account=$(_p23_hsi_field 3 account)
        if [[ -n "$_etcd_user3" || "$_local_vlan" != "210" || -z "$_local_account" ]]; then
            _step95_ok=0
            _issue95="stale state not established (etcd_key=$([[ -z "$_etcd_user3" ]] && printf absent || printf present), local_vlan='${_local_vlan:-empty}', local_account='${_local_account:-empty}')"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        pass "Step 98: establish stale HSI subscriber" \
            "etcd hsi/3 absent while node still lists user 3 with vlan=210"
    else
        fail "Step 98: establish stale HSI subscriber" "$_issue95"
    fi

    # Restore connectivity at the phase boundary even when Step 98 failed.
    _sweep_log_before=$(_p23_log_count "Reconcile sweep: user 3 active locally but absent from etcd")
    _p23_unblock_etcd

    if [[ $_step95_ok -eq 0 ]]; then
        _step96_ok=0
        _issue96="stale-state prerequisite failed"
    else
        info "Step 99: waiting up to 180s for reconcile sweep to remove stale user 3..."
        for _i in $(seq 1 90); do
            _local_vlan=$(_p23_hsi_field 3 vlan_id)
            if [[ -z "$_local_vlan" || "$_local_vlan" == "0" ]]; then
                break
            fi
            sleep 2
        done
        _sweep_log_after=$(_p23_log_count "Reconcile sweep: user 3 active locally but absent from etcd")
        if [[ -n "$_local_vlan" && "$_local_vlan" != "0" ]]; then
            _step96_ok=0
            _issue96="node still lists user 3 with vlan='${_local_vlan}' after 180s"
        elif [[ "$_sweep_log_after" -le "$_sweep_log_before" ]]; then
            _step96_ok=0
            _issue96="user 3 cleared but current-run reconcile log is missing; log tail: $(ssh_node "tail -20 '${_P23_LOG_PATH}' 2>/dev/null || true" 2>/dev/null | tr '\n' '|' | tail -c 800)"
        fi
    fi

    if [[ $_step96_ok -eq 1 ]]; then
        pass "Step 99: reconcile sweep removes stale subscriber" \
            "user 3 removed locally and reconcile sweep log observed"
    else
        fail "Step 99: reconcile sweep removes stale subscriber" "$_issue96"
    fi

    # Verify the canonical fixture before and after restoring subscriber count.
    _status1=$(_p23_hsi_field 1 status)
    _status2=$(_p23_hsi_field 2 status)
    [[ "$_status1" != "Data phase" ]] && _issue97="${_issue97} user1_status='${_status1:-empty}'"
    [[ "$_status2" != "Data phase" ]] && _issue97="${_issue97} user2_status='${_status2:-empty}'"

    info "Step 100: restoring canonical subscriber count and verifying HSI keys..."
    if ! _p23_restore_count; then
        _issue97="${_issue97} subscriber_count_restore_failed"
    fi

    _etcd_count=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null | \
        jq -r '.subscriber_count // empty' 2>/dev/null || true)
    _local_count=$(fastrg_grpc get_system_info 2>/dev/null | \
        jq -r '.num_users // empty' 2>/dev/null || true)
    _keys=$(ssh_node \
        "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} get --prefix --keys-only configs/${NODE_UUID}/hsi/" \
        2>/dev/null || true)
    _key_ids=$(printf '%s\n' "$_keys" | awk -F/ 'NF {print $NF}' | sort -n | tr '\n' ' ' | sed 's/ $//' || true)

    [[ "$_etcd_count" != "2" ]] && _issue97="${_issue97} etcd_count='${_etcd_count:-empty}'"
    [[ "$_local_count" != "2" ]] && _issue97="${_issue97} local_count='${_local_count:-empty}'"
    [[ "$_key_ids" != "1 2" ]] && _issue97="${_issue97} hsi_keys='${_key_ids:-empty}'"
    [[ -n "$_issue97" ]] && _step97_ok=0

    if [[ $_step97_ok -eq 1 ]]; then
        pass "Step 100: preserve and restore canonical fixture" \
            "users 1/2 remain in Data phase; etcd/local count=2; HSI keys=1,2"
    else
        fail "Step 100: preserve and restore canonical fixture" "${_issue97# }"
    fi

    _cleanup_phase23_hsi_sweep
    return 0
}
