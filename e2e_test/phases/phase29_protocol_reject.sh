#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 29 — Unsupported NCP Protocol-Reject and baseline restore (Steps 116-118)
# ---------------------------------------------------------------------------

set -euo pipefail

_P29_LOG_PATH=""
_P29_BRAS_LOG_PATH="/var/log/dpdk-bras.log"
_P29_BRAS_NEEDS_DEFAULT_RESTORE=0
_P29_BRAS_SSH_PID=""

_p29_start_bras() {
    local _inject_arg="${1:-}" _i

    ssh_bras "cd /root/dpdk-bras && exec ./dpdk-bras -l 0-7 -n 4 -- --pri-dns 192.168.10.1 --drop-pcap ./test.pcap --vlans 3,5 ${_inject_arg} >/var/log/dpdk-bras.log 2>&1" \
        </dev/null >/dev/null 2>&1 &
    _P29_BRAS_SSH_PID=$!

    for _i in $(seq 1 12); do
        sleep 2
        if ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
            sleep 3
            return 0
        fi
    done
    return 1
}

_p29_restart_bras() {
    local _inject_arg="${1:-}" _i

    ssh_bras "pkill -TERM -x dpdk-bras 2>/dev/null || true" >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        if ! ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
            _p29_start_bras "${_inject_arg}"
            return $?
        fi
        sleep 1
    done
    warn "Cleanup(phase29): dpdk-bras did not exit after SIGTERM."
    return 1
}

_p29_user_phase() {
    local _uid="$1" _phase

    _phase=$(fastrg_grpc get_hsi_info | \
        jq -r ".hsi_infos[] | select(.user_id == ${_uid}) | .status" \
        2>/dev/null || true)
    printf '%s' "$_phase"
}

_p29_redial() {
    local _uid="$1" _i _phase=""

    fastrg_grpc disconnect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 15); do
        sleep 2
        _phase=$(_p29_user_phase "${_uid}" || true)
        [[ "$_phase" != "Data phase" ]] && break
    done

    fastrg_grpc connect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 30); do
        sleep 2
        _phase=$(_p29_user_phase "${_uid}" || true)
        [[ "$_phase" == "Data phase" ]] && return 0
    done
    return 1
}

_p29_redial_all() {
    local _uid

    for _uid in "${SUB_IDS[@]}"; do
        if ! _p29_redial "${_uid}"; then
            return 1
        fi
    done
    return 0
}

_p29_node_log_line_count() {
    local _count

    _count=$(ssh_node "wc -l < '${_P29_LOG_PATH}' 2>/dev/null || echo 0" \
        2>/dev/null | tail -1 | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

_p29_bras_log_line_count() {
    local _count

    _count=$(ssh_bras "wc -l < '${_P29_BRAS_LOG_PATH}' 2>/dev/null || echo 0" \
        2>/dev/null | tail -1 | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

_p29_new_node_log() {
    local _baseline="$1" _start

    [[ "$_baseline" =~ ^[0-9]+$ ]] || _baseline=0
    _start=$(( _baseline + 1 ))
    ssh_node "tail -n +${_start} '${_P29_LOG_PATH}' 2>/dev/null || true" \
        2>/dev/null || true
}

_p29_new_bras_log() {
    local _baseline="$1" _start

    [[ "$_baseline" =~ ^[0-9]+$ ]] || _baseline=0
    _start=$(( _baseline + 1 ))
    ssh_bras "tail -n +${_start} '${_P29_BRAS_LOG_PATH}' 2>/dev/null || true" \
        2>/dev/null || true
}

_p29_wait_for_reject_logs() {
    local _node_baseline="$1" _i _uid _node_log="" _bras_log="" _all_seen

    for _i in $(seq 1 15); do
        _node_log=$(_p29_new_node_log "${_node_baseline}" || true)
        # _p29_start_bras truncates the BRAS log, so the post-restart baseline is 0.
        _bras_log=$(_p29_new_bras_log 0 || true)
        _all_seen=1

        for _uid in "${SUB_IDS[@]}"; do
            if ! printf '%s\n' "$_node_log" | \
                    grep -qF "User ${_uid} sent Protocol-Reject for 0x8057" ||
               ! printf '%s\n' "$_node_log" | \
                    grep -qF "User ${_uid} sent Protocol-Reject for 0x8281"; then
                _all_seen=0
            fi
        done
        if ! printf '%s\n' "$_bras_log" | \
                grep -qF "Protocol-Reject received for 0x8057 — stopping IPV6CP" ||
           ! printf '%s\n' "$_bras_log" | \
                grep -qF "Protocol-Reject received for 0x8281 — stopping MPLSCP"; then
            _all_seen=0
        fi

        [[ $_all_seen -eq 1 ]] && return 0
        sleep 1
    done
    return 1
}

# Idempotent: called after phase29 and from the top-level EXIT trap.
_cleanup_phase29_protocol_reject() {
    if [[ ${_P29_BRAS_NEEDS_DEFAULT_RESTORE:-0} -eq 1 ]]; then
        info "Cleanup(phase29): restoring default BRAS and subscriber sessions..."
        if _p29_restart_bras ""; then
            if _p29_redial_all; then
                _P29_BRAS_NEEDS_DEFAULT_RESTORE=0
                return 0
            fi
            warn "Cleanup(phase29): not all subscribers returned to Data phase."
        else
            warn "Cleanup(phase29): failed to restore default BRAS."
        fi
        return 1
    fi
    return 0
}

phase29_protocol_reject() {
    local _config_log_path="" _node_log_baseline=0 _bras_log_baseline=0
    local _stability_log_baseline=0 _new_node_log="" _new_bras_log=""
    local _stability_log="" _session_log="" _uid _phase=""
    local _step116_ok=1 _step117_ok=1 _step118_ok=1
    local _issue116="" _issue117="" _issue118="" _ping_out="" _ping_loss=""

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 29 — IPV6CP/MPLSCP Protocol-Reject (Steps 116-118)"
    bold "═══════════════════════════════════════════════════════"

    _config_log_path=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" || true)
    _P29_LOG_PATH=$(printf '%s' "$_config_log_path" | awk -F'"' '{print $2}' || true)
    [[ -n "$_P29_LOG_PATH" ]] || _P29_LOG_PATH=/var/log/fastrg/fastrg.log
    _node_log_baseline=$(_p29_node_log_line_count)
    _bras_log_baseline=$(_p29_bras_log_line_count)

    _P29_BRAS_NEEDS_DEFAULT_RESTORE=1
    if ! _p29_restart_bras "--inject-ncp ipv6cp,mplscp"; then
        _step116_ok=0
        _issue116="failed to start dpdk-bras with dual NCP injection"
    elif ! _p29_redial_all; then
        _step116_ok=0
        _issue116="one or more subscribers did not reach Data phase"
    elif ! _p29_wait_for_reject_logs "$_node_log_baseline"; then
        _step116_ok=0
        _issue116="timed out waiting for dual-protocol reject convergence"
    fi

    _new_node_log=$(_p29_new_node_log "$_node_log_baseline" || true)
    _new_bras_log=$(_p29_new_bras_log 0 || true)
    for _uid in "${SUB_IDS[@]}"; do
        if ! printf '%s\n' "$_new_node_log" | \
                grep -qF "User ${_uid} sent Protocol-Reject for 0x8057"; then
            _step116_ok=0
            _issue116="${_issue116:+${_issue116}; }missing IPV6CP reject log for user ${_uid}"
        fi
        if ! printf '%s\n' "$_new_node_log" | \
                grep -qF "User ${_uid} sent Protocol-Reject for 0x8281"; then
            _step116_ok=0
            _issue116="${_issue116:+${_issue116}; }missing MPLSCP reject log for user ${_uid}"
        fi
    done
    if ! printf '%s\n' "$_new_bras_log" | \
            grep -qF "Protocol-Reject received for 0x8057 — stopping IPV6CP"; then
        _step116_ok=0
        _issue116="${_issue116:+${_issue116}; }BRAS did not stop IPV6CP injection"
    fi
    if ! printf '%s\n' "$_new_bras_log" | \
            grep -qF "Protocol-Reject received for 0x8281 — stopping MPLSCP"; then
        _step116_ok=0
        _issue116="${_issue116:+${_issue116}; }BRAS did not stop MPLSCP injection"
    fi

    if [[ $_step116_ok -eq 1 ]]; then
        pass "Step 116: reject injected IPV6CP and MPLSCP" \
            "users ${SUB_IDS[*]} sent both rejects; BRAS stopped both injectors (pre-restart BRAS log baseline=${_bras_log_baseline})"
    else
        fail "Step 116: reject injected IPV6CP and MPLSCP" "$_issue116"
    fi

    _stability_log_baseline=$(_p29_node_log_line_count)
    sleep 4
    for _uid in "${SUB_IDS[@]}"; do
        _phase=$(_p29_user_phase "${_uid}" || true)
        if [[ "$_phase" != "Data phase" ]]; then
            _step117_ok=0
            _issue117="${_issue117:+${_issue117}; }user ${_uid} status='${_phase:-missing}'"
        fi
    done

    _ping_out=$(ssh_lan "ping -c 4 -W 3 ${WAN_IP}" 2>&1 || true)
    if ! printf '%s\n' "$_ping_out" | grep -qE '0% packet loss|0\.0% packet loss'; then
        _step117_ok=0
        _ping_loss=$(printf '%s\n' "$_ping_out" | \
            grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
        _issue117="${_issue117:+${_issue117}; }${WAN_IP} was not reachable (${_ping_loss:-no response})"
    fi

    _stability_log=$(_p29_new_node_log "$_stability_log_baseline" || true)
    _session_log=$(printf '%s\n' "$_stability_log" | \
        grep -E 'pppoe is force terminating|pppoe is spawning|HSI module is (terminated|spawned)' || true)
    if [[ -n "$_session_log" ]]; then
        _step117_ok=0
        _issue117="${_issue117:+${_issue117}; }session teardown/redial log observed: '$(printf '%s' "$_session_log" | tr '\n' '|' | tail -c 700 || true)'"
    fi

    if [[ $_step117_ok -eq 1 ]]; then
        pass "Step 117: preserve sessions after Protocol-Reject" \
            "users ${SUB_IDS[*]} remained in Data phase; ${WAN_IP} reachable with 0% loss; no teardown/redial log"
    else
        fail "Step 117: preserve sessions after Protocol-Reject" "$_issue117"
    fi

    if ! _p29_restart_bras ""; then
        _step118_ok=0
        _issue118="failed to restart dpdk-bras with default parameters"
    elif ! _p29_redial_all; then
        _step118_ok=0
        _issue118="one or more subscribers did not return to Data phase"
    else
        _P29_BRAS_NEEDS_DEFAULT_RESTORE=0
    fi

    if [[ $_step118_ok -eq 1 ]]; then
        pass "Step 118: restore default BRAS baseline" \
            "NCP injection disabled; users ${SUB_IDS[*]} returned to Data phase"
    else
        fail "Step 118: restore default BRAS baseline" "$_issue118"
    fi

    _cleanup_phase29_protocol_reject || true
    return 0
}
