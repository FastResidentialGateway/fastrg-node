#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 30 — PPPoE runtime disconnect and keepalive recovery (Steps 120-123)
# ---------------------------------------------------------------------------

set -euo pipefail

_P30_LOG_PATH=""
_P30_BRAS_LOG_PATH="/var/log/dpdk-bras.log"
_P30_BRAS_NEEDS_DEFAULT_RESTORE=0
_P30_BRAS_SSH_PID=""
_P30_LAST_WAIT_SECONDS=0

_p30_start_bras() {
    local _mode_arg="${1:-}" _i

    ssh_bras "cd /root/dpdk-bras && exec ./dpdk-bras -l 0-7 -n 4 -- --pri-dns 192.168.10.1 --drop-pcap ./test.pcap --vlans 3,5 ${_mode_arg} >/var/log/dpdk-bras.log 2>&1" \
        </dev/null >/dev/null 2>&1 &
    _P30_BRAS_SSH_PID=$!

    for _i in $(seq 1 12); do
        sleep 2
        if ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
            sleep 3
            return 0
        fi
    done
    return 1
}

_p30_restart_bras() {
    local _mode_arg="${1:-}" _i

    ssh_bras "pkill -TERM -x dpdk-bras 2>/dev/null || true" >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        if ! ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
            _p30_start_bras "${_mode_arg}"
            return $?
        fi
        sleep 1
    done
    warn "Cleanup(phase30): dpdk-bras did not exit after SIGTERM."
    return 1
}

_p30_user_phase() {
    local _uid="$1" _phase

    _phase=$(fastrg_grpc get_hsi_info | \
        jq -r ".hsi_infos[] | select(.user_id == ${_uid}) | .status" \
        2>/dev/null || true)
    printf '%s' "$_phase"
}

_p30_redial() {
    local _uid="$1" _i _phase=""

    fastrg_grpc disconnect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 15); do
        sleep 2
        _phase=$(_p30_user_phase "${_uid}" || true)
        [[ "$_phase" != "Data phase" ]] && break
    done

    fastrg_grpc connect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 30); do
        sleep 2
        _phase=$(_p30_user_phase "${_uid}" || true)
        [[ "$_phase" == "Data phase" ]] && return 0
    done
    return 1
}

_p30_redial_all() {
    local _uid

    for _uid in "${SUB_IDS[@]}"; do
        if ! _p30_redial "${_uid}"; then
            return 1
        fi
    done
    return 0
}

_p30_node_log_line_count() {
    local _count

    _count=$(ssh_node "wc -l < '${_P30_LOG_PATH}' 2>/dev/null || echo 0" \
        2>/dev/null | tail -1 | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

_p30_bras_log_line_count() {
    local _count

    _count=$(ssh_bras "wc -l < '${_P30_BRAS_LOG_PATH}' 2>/dev/null || echo 0" \
        2>/dev/null | tail -1 | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

_p30_new_node_log() {
    local _baseline="$1" _start

    [[ "$_baseline" =~ ^[0-9]+$ ]] || _baseline=0
    _start=$(( _baseline + 1 ))
    ssh_node "tail -n +${_start} '${_P30_LOG_PATH}' 2>/dev/null || true" \
        2>/dev/null || true
}

_p30_new_bras_log() {
    local _baseline="$1" _start

    [[ "$_baseline" =~ ^[0-9]+$ ]] || _baseline=0
    _start=$(( _baseline + 1 ))
    ssh_bras "tail -n +${_start} '${_P30_BRAS_LOG_PATH}' 2>/dev/null || true" \
        2>/dev/null || true
}

_p30_wait_all_data() {
    local _timeout="$1" _started=$SECONDS _uid _phase _all_data

    while (( SECONDS - _started <= _timeout )); do
        _all_data=1
        for _uid in "${SUB_IDS[@]}"; do
            _phase=$(_p30_user_phase "${_uid}" || true)
            [[ "$_phase" == "Data phase" ]] || _all_data=0
        done
        if [[ $_all_data -eq 1 ]]; then
            _P30_LAST_WAIT_SECONDS=$(( SECONDS - _started ))
            return 0
        fi
        sleep 2
    done
    _P30_LAST_WAIT_SECONDS=$(( SECONDS - _started ))
    return 1
}

_p30_wait_terminate_event() {
    local _bras_baseline="$1" _timeout="$2" _started=$SECONDS
    local _idx _uid _phase _bras_log="" _ack_count=0 _all_left
    local -a _seen_left=()

    for _idx in "${!SUB_IDS[@]}"; do
        _seen_left[$_idx]=0
    done

    while (( SECONDS - _started <= _timeout )); do
        _bras_log=$(_p30_new_bras_log "${_bras_baseline}" || true)
        _ack_count=$(printf '%s\n' "$_bras_log" | \
            grep -F "Terminate-Ack received for session " | \
            sed -n 's/.*Terminate-Ack received for session \([0-9][0-9]*\).*/\1/p' | \
            sort -u | wc -l | tr -d '[:space:]' || true)
        [[ "$_ack_count" =~ ^[0-9]+$ ]] || _ack_count=0

        _all_left=1
        for _idx in "${!SUB_IDS[@]}"; do
            _uid="${SUB_IDS[$_idx]}"
            _phase=$(_p30_user_phase "${_uid}" || true)
            [[ "$_phase" != "Data phase" ]] && _seen_left[$_idx]=1
            [[ ${_seen_left[$_idx]} -eq 1 ]] || _all_left=0
        done

        if [[ $_all_left -eq 1 && $_ack_count -ge ${#SUB_IDS[@]} ]]; then
            _P30_LAST_WAIT_SECONDS=$(( SECONDS - _started ))
            return 0
        fi
        sleep 1
    done
    _P30_LAST_WAIT_SECONDS=$(( SECONDS - _started ))
    return 1
}

_p30_wait_padt_event() {
    local _node_baseline="$1" _timeout="$2" _started=$SECONDS
    local _idx _uid _phase _node_log="" _padt_count=0 _all_left
    local -a _seen_left=()

    for _idx in "${!SUB_IDS[@]}"; do
        _seen_left[$_idx]=0
    done

    while (( SECONDS - _started <= _timeout )); do
        _node_log=$(_p30_new_node_log "${_node_baseline}" || true)
        _padt_count=$(printf '%s\n' "$_node_log" | \
            grep -cF "connection disconnected." || true)
        [[ "$_padt_count" =~ ^[0-9]+$ ]] || _padt_count=0

        _all_left=1
        for _idx in "${!SUB_IDS[@]}"; do
            _uid="${SUB_IDS[$_idx]}"
            _phase=$(_p30_user_phase "${_uid}" || true)
            [[ "$_phase" != "Data phase" ]] && _seen_left[$_idx]=1
            [[ ${_seen_left[$_idx]} -eq 1 ]] || _all_left=0
        done

        if [[ $_all_left -eq 1 && $_padt_count -ge ${#SUB_IDS[@]} ]]; then
            _P30_LAST_WAIT_SECONDS=$(( SECONDS - _started ))
            return 0
        fi
        sleep 1
    done
    _P30_LAST_WAIT_SECONDS=$(( SECONDS - _started ))
    return 1
}

_p30_wait_keepalive_failure() {
    local _node_baseline="$1" _timeout="$2" _started=$SECONDS
    local _idx _uid _phase _node_log="" _all_seen
    local -a _seen_log=() _seen_left=()

    for _idx in "${!SUB_IDS[@]}"; do
        _seen_log[$_idx]=0
        _seen_left[$_idx]=0
    done

    while (( SECONDS - _started <= _timeout )); do
        _node_log=$(_p30_new_node_log "${_node_baseline}" || true)
        _all_seen=1
        for _idx in "${!SUB_IDS[@]}"; do
            _uid="${SUB_IDS[$_idx]}"
            if printf '%s\n' "$_node_log" | \
                    grep -qF "User ${_uid} LCP keepalive: peer unresponsive for 3 echo-requests, tearing down session."; then
                _seen_log[$_idx]=1
            fi
            _phase=$(_p30_user_phase "${_uid}" || true)
            [[ "$_phase" != "Data phase" ]] && _seen_left[$_idx]=1
            if [[ ${_seen_log[$_idx]} -ne 1 || ${_seen_left[$_idx]} -ne 1 ]]; then
                _all_seen=0
            fi
        done
        if [[ $_all_seen -eq 1 ]]; then
            _P30_LAST_WAIT_SECONDS=$(( SECONDS - _started ))
            return 0
        fi
        sleep 2
    done
    _P30_LAST_WAIT_SECONDS=$(( SECONDS - _started ))
    return 1
}

# Idempotent: called after phase30 and from the top-level EXIT trap.
_cleanup_phase30_keepalive_failure() {
    if [[ ${_P30_BRAS_NEEDS_DEFAULT_RESTORE:-0} -eq 1 ]]; then
        info "Cleanup(phase30): restoring default BRAS and subscriber sessions..."
        if _p30_restart_bras ""; then
            if _p30_wait_all_data 180; then
                _P30_BRAS_NEEDS_DEFAULT_RESTORE=0
                return 0
            fi
            warn "Cleanup(phase30): automatic recovery timed out; using explicit redial fallback."
            if _p30_redial_all; then
                _P30_BRAS_NEEDS_DEFAULT_RESTORE=0
                return 0
            fi
            warn "Cleanup(phase30): not all subscribers returned to Data phase."
        else
            warn "Cleanup(phase30): failed to restore default BRAS."
        fi
        return 1
    fi
    return 0
}

phase30_keepalive_failure() {
    local _config_log_path="" _uid _desire="" _phase=""
    local _bras_log_baseline=0 _node_log_baseline=0 _event_seconds=0 _recovery_seconds=0
    local _step119_ok=1 _step120_ok=1 _step121_ok=1 _step122_ok=1
    local _issue119="" _issue120="" _issue121="" _issue122=""

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 30 — PPPoE Runtime Recovery (Steps 120-123)"
    bold "═══════════════════════════════════════════════════════"

    _config_log_path=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" || true)
    _P30_LOG_PATH=$(printf '%s' "$_config_log_path" | awk -F'"' '{print $2}' || true)
    [[ -n "$_P30_LOG_PATH" ]] || _P30_LOG_PATH=/var/log/fastrg/fastrg.log

    for _uid in "${SUB_IDS[@]}"; do
        _desire=$(_ctrl_desire_status "${_uid}" || true)
        if [[ "$_desire" != "connect" ]]; then
            _issue119="${_issue119:+${_issue119}; }user ${_uid} desire_status='${_desire:-missing}'"
        fi
        _phase=$(_p30_user_phase "${_uid}" || true)
        if [[ "$_phase" != "Data phase" ]]; then
            _issue119="${_issue119:+${_issue119}; }user ${_uid} status='${_phase:-missing}'"
        fi
    done

    if [[ -n "$_issue119" ]]; then
        fail "Step 120: recover after LCP Terminate-Request" "dirty precondition: ${_issue119}"
        fail "Step 121: recover after peer PADT" "dirty precondition: ${_issue119}"
        fail "Step 122: detect LCP keepalive failure" "dirty precondition: ${_issue119}"
        fail "Step 123: restore default BRAS and auto-recover" "dirty precondition: ${_issue119}"
        return 0
    fi

    _bras_log_baseline=$(_p30_bras_log_line_count)
    if ! ssh_bras "pkill -USR1 -x dpdk-bras" >/dev/null 2>&1; then
        _step119_ok=0
        _issue119="failed to deliver SIGUSR1 to dpdk-bras"
    elif ! _p30_wait_terminate_event "$_bras_log_baseline" 30; then
        _step119_ok=0
        _issue119="Terminate-Ack or subscriber teardown not observed within 30s"
    else
        _event_seconds=$_P30_LAST_WAIT_SECONDS
    fi
    if [[ $_step119_ok -eq 1 ]]; then
        if _p30_wait_all_data 120; then
            _recovery_seconds=$_P30_LAST_WAIT_SECONDS
        else
            _step119_ok=0
            _issue119="automatic reconciliation did not restore all subscribers within 120s"
        fi
    fi

    if [[ $_step119_ok -eq 1 ]]; then
        pass "Step 120: recover after LCP Terminate-Request" \
            "${#SUB_IDS[@]} Terminate-Acks and all teardowns observed in ${_event_seconds}s; users ${SUB_IDS[*]} auto-recovered in ${_recovery_seconds}s"
    else
        fail "Step 120: recover after LCP Terminate-Request" "$_issue119"
    fi

    for _uid in "${SUB_IDS[@]}"; do
        _phase=$(_p30_user_phase "${_uid}" || true)
        if [[ "$_phase" != "Data phase" ]]; then
            _step120_ok=0
            _issue120="${_issue120:+${_issue120}; }user ${_uid} status='${_phase:-missing}' before PADT"
        fi
    done
    _node_log_baseline=$(_p30_node_log_line_count)
    if [[ $_step120_ok -eq 1 ]] && \
       ! ssh_bras "pkill -USR2 -x dpdk-bras" >/dev/null 2>&1; then
        _step120_ok=0
        _issue120="failed to deliver SIGUSR2 to dpdk-bras"
    elif [[ $_step120_ok -eq 1 ]] && \
         ! _p30_wait_padt_event "$_node_log_baseline" 30; then
        _step120_ok=0
        _issue120="PADT disconnect logs or subscriber teardown not observed within 30s"
    elif [[ $_step120_ok -eq 1 ]]; then
        _event_seconds=$_P30_LAST_WAIT_SECONDS
    fi
    if [[ $_step120_ok -eq 1 ]]; then
        if _p30_wait_all_data 120; then
            _recovery_seconds=$_P30_LAST_WAIT_SECONDS
        else
            _step120_ok=0
            _issue120="automatic reconciliation did not restore all subscribers within 120s"
        fi
    fi

    if [[ $_step120_ok -eq 1 ]]; then
        pass "Step 121: recover after peer PADT" \
            "${#SUB_IDS[@]} PADT disconnects and all teardowns observed in ${_event_seconds}s; users ${SUB_IDS[*]} auto-recovered in ${_recovery_seconds}s"
    else
        fail "Step 121: recover after peer PADT" "$_issue120"
    fi

    _P30_BRAS_NEEDS_DEFAULT_RESTORE=1
    if ! _p30_restart_bras "--no-lcp-echo"; then
        _step121_ok=0
        _issue121="failed to start dpdk-bras with --no-lcp-echo"
    elif ! _p30_redial_all; then
        _step121_ok=0
        _issue121="one or more subscribers did not reach Data phase in no-echo mode"
    else
        _node_log_baseline=$(_p30_node_log_line_count)
        if _p30_wait_keepalive_failure "$_node_log_baseline" 180; then
            _event_seconds=$_P30_LAST_WAIT_SECONDS
        else
            _step121_ok=0
            _issue121="keepalive teardown log or subscriber departure not observed within 180s"
        fi
    fi

    if [[ $_step121_ok -eq 1 ]]; then
        pass "Step 122: detect LCP keepalive failure" \
            "users ${SUB_IDS[*]} logged 3 missed echo-requests and left Data phase in ${_event_seconds}s"
    else
        fail "Step 122: detect LCP keepalive failure" "$_issue121"
    fi

    if ! _p30_restart_bras ""; then
        _step122_ok=0
        _issue122="failed to restart dpdk-bras with default parameters"
    elif ! _p30_wait_all_data 180; then
        _step122_ok=0
        _issue122="automatic reconciliation did not restore all subscribers within 180s"
    else
        _recovery_seconds=$_P30_LAST_WAIT_SECONDS
        _P30_BRAS_NEEDS_DEFAULT_RESTORE=0
    fi

    if [[ $_step122_ok -eq 1 ]]; then
        pass "Step 123: restore default BRAS and auto-recover" \
            "default echo behavior restored; users ${SUB_IDS[*]} auto-recovered in ${_recovery_seconds}s without connect_hsi"
    else
        fail "Step 123: restore default BRAS and auto-recover" "$_issue122"
    fi

    _cleanup_phase30_keepalive_failure || true
    return 0
}
