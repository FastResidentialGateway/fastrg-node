#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 28 — CHAP authentication and PAP baseline restore (Steps 113-115)
# ---------------------------------------------------------------------------

set -euo pipefail

_P28_LOG_PATH=""
_P28_BRAS_NEEDS_PAP_RESTORE=0
_P28_BRAS_SSH_PID=""

_p28_start_bras() {
    local _auth_arg="${1:-}" _i

    ssh_bras "cd /root/dpdk-bras && exec ./dpdk-bras -l 0-7 -n 4 -- --pri-dns 192.168.10.1 --drop-pcap ./test.pcap --vlans 3,5 ${_auth_arg} >/var/log/dpdk-bras.log 2>&1" \
        </dev/null >/dev/null 2>&1 &
    _P28_BRAS_SSH_PID=$!

    for _i in $(seq 1 12); do
        sleep 2
        if ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
            sleep 3
            return 0
        fi
    done
    return 1
}

_p28_restart_bras() {
    local _auth_arg="${1:-}" _i

    ssh_bras "pkill -TERM -x dpdk-bras 2>/dev/null || true" >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        if ! ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
            _p28_start_bras "${_auth_arg}"
            return $?
        fi
        sleep 1
    done
    warn "Cleanup(phase28): dpdk-bras did not exit after SIGTERM."
    return 1
}

_p28_user_phase() {
    local _uid="$1" _phase

    _phase=$(fastrg_grpc get_hsi_info | \
        jq -r ".hsi_infos[] | select(.user_id == ${_uid}) | .status" \
        2>/dev/null || true)
    printf '%s' "$_phase"
}

_p28_redial() {
    local _uid="$1" _i _phase=""

    fastrg_grpc disconnect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 15); do
        sleep 2
        _phase=$(_p28_user_phase "${_uid}" || true)
        [[ "$_phase" != "Data phase" ]] && break
    done

    fastrg_grpc connect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 30); do
        sleep 2
        _phase=$(_p28_user_phase "${_uid}" || true)
        [[ "$_phase" == "Data phase" ]] && return 0
    done
    return 1
}

_p28_redial_all() {
    local _uid

    for _uid in "${SUB_IDS[@]}"; do
        if ! _p28_redial "${_uid}"; then
            return 1
        fi
    done
    return 0
}

_p28_log_line_count() {
    local _count

    _count=$(ssh_node "wc -l < '${_P28_LOG_PATH}' 2>/dev/null || echo 0" \
        2>/dev/null | tail -1 | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

_p28_new_log() {
    local _baseline="$1" _start

    [[ "$_baseline" =~ ^[0-9]+$ ]] || _baseline=0
    _start=$(( _baseline + 1 ))
    ssh_node "tail -n +${_start} '${_P28_LOG_PATH}' 2>/dev/null || true" \
        2>/dev/null || true
}

# Idempotent: called after phase28 and from the top-level EXIT trap.
_cleanup_phase28_chap_auth() {
    if [[ ${_P28_BRAS_NEEDS_PAP_RESTORE:-0} -eq 1 ]]; then
        info "Cleanup(phase28): restoring default PAP BRAS and subscriber sessions..."
        if _p28_restart_bras ""; then
            if _p28_redial_all; then
                _P28_BRAS_NEEDS_PAP_RESTORE=0
                return 0
            fi
            warn "Cleanup(phase28): not all subscribers returned to Data phase."
        else
            warn "Cleanup(phase28): failed to restore default PAP BRAS."
        fi
        return 1
    fi
    return 0
}

phase28_chap_auth() {
    local _config_log_path="" _log_baseline=0 _new_log="" _uid
    local _step113_ok=1 _step114_ok=1 _step115_ok=1
    local _issue113="" _issue114="" _issue115="" _ping_out="" _ping_loss=""

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 28 — CHAP Authentication (Steps 113-115)"
    bold "═══════════════════════════════════════════════════════"

    _config_log_path=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" || true)
    _P28_LOG_PATH=$(printf '%s' "$_config_log_path" | awk -F'"' '{print $2}' || true)
    [[ -n "$_P28_LOG_PATH" ]] || _P28_LOG_PATH=/var/log/fastrg/fastrg.log
    _log_baseline=$(_p28_log_line_count)

    _P28_BRAS_NEEDS_PAP_RESTORE=1
    if ! _p28_restart_bras "--auth chap"; then
        _step113_ok=0
        _issue113="failed to start dpdk-bras with --auth chap"
    elif ! _p28_redial_all; then
        _step113_ok=0
        _issue113="one or more subscribers did not reach Data phase"
    fi

    _new_log=$(_p28_new_log "$_log_baseline" || true)
    if [[ $_step113_ok -eq 1 ]]; then
        for _uid in "${SUB_IDS[@]}"; do
            if ! printf '%s\n' "$_new_log" | grep -qF "User ${_uid} recv chap challenge." ||
               ! printf '%s\n' "$_new_log" | grep -qF "User ${_uid} auth success."; then
                _step113_ok=0
                _issue113="${_issue113:+${_issue113}; }missing CHAP challenge/auth-success log for user ${_uid}"
            fi
        done
    fi

    if [[ $_step113_ok -eq 1 ]]; then
        pass "Step 113: CHAP dial reaches Data phase" \
            "users ${SUB_IDS[*]} reached Data phase with per-user challenge and auth-success logs"
    else
        fail "Step 113: CHAP dial reaches Data phase" "$_issue113"
    fi

    _ping_out=$(ssh_lan "ping -c 4 -W 3 ${WAN_IP}" 2>&1 || true)
    if ! printf '%s\n' "$_ping_out" | grep -qE '0% packet loss|0\.0% packet loss'; then
        _step114_ok=0
        _ping_loss=$(printf '%s\n' "$_ping_out" | \
            grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
        _issue114="${WAN_IP} was not reachable (${_ping_loss:-no response})"
    fi

    if [[ $_step114_ok -eq 1 ]]; then
        pass "Step 114: CHAP session data plane" \
            "${WAN_IP} reachable with 4 packets and 0% packet loss"
    else
        fail "Step 114: CHAP session data plane" "$_issue114"
    fi

    if ! _p28_restart_bras ""; then
        _step115_ok=0
        _issue115="failed to restart dpdk-bras with default PAP parameters"
    elif ! _p28_redial_all; then
        _step115_ok=0
        _issue115="one or more subscribers did not return to Data phase under PAP"
    else
        _P28_BRAS_NEEDS_PAP_RESTORE=0
    fi

    if [[ $_step115_ok -eq 1 ]]; then
        pass "Step 115: restore default PAP baseline" \
            "default BRAS parameters restored; users ${SUB_IDS[*]} returned to Data phase"
    else
        fail "Step 115: restore default PAP baseline" "$_issue115"
    fi

    _cleanup_phase28_chap_auth || true
    return 0
}
