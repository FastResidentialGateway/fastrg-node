#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 26 — Controller Heartbeat / Re-registration (Steps 104-107)
# ---------------------------------------------------------------------------

_P26_CONTROLLER_HOST=""
_P26_CONTROLLER_PORT=""
_P26_LOG_PATH=""
_P26_STDOUT_LOG=/var/log/fastrg.log
_p26_iptables_blocked=0
_p26_needs_recovery=0

_p26_block_controller() {
    ssh_node "iptables -D OUTPUT -p tcp -d ${_P26_CONTROLLER_HOST} --dport ${_P26_CONTROLLER_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    if ssh_node "iptables -I OUTPUT 1 -p tcp -d ${_P26_CONTROLLER_HOST} --dport ${_P26_CONTROLLER_PORT} -j REJECT --reject-with tcp-reset" \
        >/dev/null 2>&1; then
        _p26_iptables_blocked=1
        _p26_needs_recovery=1
        return 0
    fi
    _p26_iptables_blocked=0
    return 1
}

_p26_unblock_controller() {
    if [[ -n "${_P26_CONTROLLER_HOST:-}" ]] && [[ -n "${_P26_CONTROLLER_PORT:-}" ]]; then
        ssh_node "iptables -D OUTPUT -p tcp -d ${_P26_CONTROLLER_HOST} --dport ${_P26_CONTROLLER_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null || true; ! iptables -C OUTPUT -p tcp -d ${_P26_CONTROLLER_HOST} --dport ${_P26_CONTROLLER_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null" \
            >/dev/null 2>&1 || return 1
    fi
    _p26_iptables_blocked=0
    return 0
}

_p26_log_line_count() {
    local _path="$1" _count

    _count=$(ssh_node "wc -l < '${_path}' 2>/dev/null || echo 0" 2>/dev/null | \
        tail -1 | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

_p26_new_log() {
    local _path="$1" _baseline="$2" _start

    [[ "$_baseline" =~ ^[0-9]+$ ]] || _baseline=0
    _start=$(( _baseline + 1 ))
    ssh_node "tail -n +${_start} '${_path}' 2>/dev/null || true" 2>/dev/null || true
}

_p26_wait_for_new_log() {
    local _path="$1" _baseline="$2" _needle="$3" _timeout="$4"
    local _elapsed _new=""

    for _elapsed in $(seq 1 "$_timeout"); do
        _new=$(_p26_new_log "$_path" "$_baseline" || true)
        if printf '%s\n' "$_new" | grep -qF "$_needle"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

_p26_log_snippet() {
    _p26_new_log "$1" "$2" | tr '\n' '|' | tail -c 1000 || true
}

_p26_controller_node_state() {
    P26_CONTROLLER_REST="$CONTROLLER_REST" \
        P26_CONTROLLER_USER="$CONTROLLER_USER" \
        P26_CONTROLLER_PASS="$CONTROLLER_PASS" \
        P26_NODE_UUID="$NODE_UUID" \
        python3 - <<'PY' 2>/dev/null || true
import json
import os
import ssl
import urllib.error
import urllib.request

context = ssl.create_default_context()
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE


def login():
    body = json.dumps({
        "username": os.environ["P26_CONTROLLER_USER"],
        "password": os.environ["P26_CONTROLLER_PASS"],
    }).encode()
    request = urllib.request.Request(
        os.environ["P26_CONTROLLER_REST"] + "/api/login",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, context=context, timeout=10) as response:
        token = json.loads(response.read())["token"]
    with open("/tmp/.fastrg_e2e_ctrl_token", "w", encoding="utf-8") as token_file:
        token_file.write(token)
    return token


def list_nodes(token):
    request = urllib.request.Request(
        os.environ["P26_CONTROLLER_REST"] + "/api/nodes",
        headers={"Authorization": token},
        method="GET",
    )
    with urllib.request.urlopen(request, context=context, timeout=10) as response:
        return json.loads(response.read())


try:
    with open("/tmp/.fastrg_e2e_ctrl_token", encoding="utf-8") as token_file:
        token = token_file.read().strip()
    nodes = list_nodes(token)
except (FileNotFoundError, urllib.error.HTTPError):
    nodes = list_nodes(login())
uuid = os.environ["P26_NODE_UUID"]
print("present" if any(uuid in json.dumps(node) for node in nodes) else "absent")
PY
}

_p26_unregister_node() {
    local _grpcurl

    if [[ -x "${GRPC_CLIENT_DIR}/grpcurl" ]]; then
        _grpcurl="${GRPC_CLIENT_DIR}/grpcurl"
    else
        _grpcurl=$(command -v grpcurl 2>/dev/null || true)
    fi
    [[ -n "$_grpcurl" ]] || return 1
    [[ -f "${GRPC_CLIENT_DIR}/controller.proto" ]] || return 1

    "$_grpcurl" -plaintext \
        -import-path "${GRPC_CLIENT_DIR}" \
        -proto controller.proto \
        -connect-timeout 10 \
        -max-time 10 \
        -d "{\"node_uuid\":\"${NODE_UUID}\"}" \
        "$CONTROLLER_GRPC" \
        controller.NodeManagement/UnregisterNode \
        >/dev/null 2>&1
}

# Idempotent: called after phase26 and from the top-level EXIT trap.
_cleanup_phase26_heartbeat_reregister() {
    local _recovery_log_base=0 _recovery_stdout_base=0 _i _state=""

    if [[ ${_p26_needs_recovery:-0} -eq 1 ]]; then
        _recovery_log_base=$(_p26_log_line_count "${_P26_LOG_PATH:-/var/log/fastrg/fastrg.log}")
        _recovery_stdout_base=$(_p26_log_line_count "${_P26_STDOUT_LOG}")
    fi

    _p26_unblock_controller || true

    if [[ ${_p26_needs_recovery:-0} -eq 1 ]] && \
       ssh_node "pgrep -x fastrg >/dev/null 2>&1" 2>/dev/null; then
        info "Cleanup(phase26): waiting for controller heartbeat/re-registration recovery..."
        for _i in $(seq 1 45); do
            _state=$(_p26_controller_node_state || true)
            if [[ "$_state" == "present" ]] && \
               { _p26_wait_for_new_log "${_P26_LOG_PATH:-/var/log/fastrg/fastrg.log}" \
                     "$_recovery_log_base" "Heartbeat sent successfully" 1 || \
                 _p26_wait_for_new_log "${_P26_STDOUT_LOG}" \
                     "$_recovery_stdout_base" "Re-registration successful" 1; }; then
                _p26_needs_recovery=0
                return 0
            fi
            sleep 1
        done
        warn "Cleanup(phase26): controller recovery was not observed within 45s."
        return 1
    fi

    _p26_needs_recovery=0
    return 0
}

phase26_heartbeat_reregister() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 26 — Controller Heartbeat / Re-registration (Steps 104-107)"
    bold "═══════════════════════════════════════════════════════"

    local _step104_ok=1 _step105_ok=1 _step106_ok=1 _step107_ok=1
    local _issue104="" _issue105="" _issue106="" _issue107=""
    local _log_baseline=0 _stdout_baseline=0 _recovery_baseline=0
    local _reregister_log_baseline=0 _final_heartbeat_baseline=0
    local _controller_state="" _ping_out="" _ping_loss="" _config_log_path=""

    _P26_CONTROLLER_HOST="${CONTROLLER_GRPC%%:*}"
    _P26_CONTROLLER_PORT="${CONTROLLER_GRPC##*:}"
    _config_log_path=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" || true)
    _P26_LOG_PATH=$(printf '%s' "$_config_log_path" | awk -F'"' '{print $2}' || true)
    [[ -n "$_P26_LOG_PATH" ]] || _P26_LOG_PATH=/var/log/fastrg/fastrg.log

    _log_baseline=$(_p26_log_line_count "$_P26_LOG_PATH")
    _stdout_baseline=$(_p26_log_line_count "$_P26_STDOUT_LOG")
    info "Step 104 log baselines: FastRG_LOG=${_log_baseline}, stdout=${_stdout_baseline}"
    _cleanup_phase26_heartbeat_reregister || true

    if ! ssh_node "command -v iptables >/dev/null 2>&1" 2>/dev/null; then
        _step104_ok=0
        _issue104="iptables is not available on the node"
    elif [[ -z "$_P26_CONTROLLER_HOST" || -z "$_P26_CONTROLLER_PORT" || \
            "$_P26_CONTROLLER_HOST" == "$CONTROLLER_GRPC" || \
            "$_P26_CONTROLLER_PORT" == "$CONTROLLER_GRPC" ]]; then
        _step104_ok=0
        _issue104="could not parse CONTROLLER_GRPC='${CONTROLLER_GRPC:-empty}'"
    elif ! _p26_block_controller; then
        _step104_ok=0
        _issue104="failed to install node->controller iptables REJECT rule"
    fi

    if [[ $_step104_ok -eq 1 ]]; then
        info "Step 104: blocking node->controller heartbeat for 50s..."
        sleep 50
        if ! _p26_wait_for_new_log "$_P26_LOG_PATH" "$_log_baseline" \
            "Failed to send heartbeat" 1; then
            _step104_ok=0
            _issue104="current-run heartbeat failure log is missing; log='$(_p26_log_snippet "$_P26_LOG_PATH" "$_log_baseline")'"
        elif ! ssh_node "pgrep -x fastrg >/dev/null 2>&1" 2>/dev/null; then
            _step104_ok=0
            _issue104="fastrg exited while controller heartbeat was blocked"
        fi
    fi

    if [[ $_step104_ok -eq 1 ]]; then
        pass "Step 104: tolerate controller heartbeat interruption" \
            "tcp-reset block produced heartbeat failure log; fastrg remained alive"
    else
        fail "Step 104: tolerate controller heartbeat interruption" "$_issue104"
    fi

    info "Step 105: verifying LAN→WAN data plane while controller gRPC remains blocked..."
    if [[ ${_p26_iptables_blocked:-0} -ne 1 ]]; then
        _step105_ok=0
        _issue105="controller heartbeat block prerequisite failed"
    else
        _ping_out=$(ssh_lan "ping -c 4 -W 3 ${WAN_IP}" 2>&1 || true)
        if ! printf '%s\n' "$_ping_out" | grep -qE '0% packet loss|0\.0% packet loss'; then
            _step105_ok=0
            _ping_loss=$(printf '%s\n' "$_ping_out" | \
                grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
            _issue105="${WAN_IP} was not reachable (${_ping_loss:-no response})"
        fi
    fi

    if [[ $_step105_ok -eq 1 ]]; then
        pass "Step 105: preserve data plane during heartbeat outage" \
            "${WAN_IP} reachable with 0% packet loss while controller gRPC was blocked"
    else
        fail "Step 105: preserve data plane during heartbeat outage" "$_issue105"
    fi

    _recovery_baseline=$(_p26_log_line_count "$_P26_LOG_PATH")
    if ! _p26_unblock_controller; then
        _step106_ok=0
        _issue106="failed to remove node->controller iptables REJECT rule"
    else
        info "Step 106: controller path restored; waiting 50s for heartbeat recovery..."
        sleep 50
        if ! _p26_wait_for_new_log "$_P26_LOG_PATH" "$_recovery_baseline" \
            "Heartbeat sent successfully" 1; then
            _step106_ok=0
            _issue106="heartbeat success was not logged after unblock; log='$(_p26_log_snippet "$_P26_LOG_PATH" "$_recovery_baseline")'"
        fi
    fi

    if [[ $_step106_ok -eq 1 ]]; then
        _p26_needs_recovery=0
        pass "Step 106: resume heartbeat after controller recovery" \
            "current-run heartbeat success log observed after removing tcp-reset block"
    else
        fail "Step 106: resume heartbeat after controller recovery" "$_issue106"
    fi

    # Synchronize immediately after a heartbeat so the unregister disappearance
    # can be observed before the next 30-second heartbeat re-registers the node.
    _final_heartbeat_baseline=$(_p26_log_line_count "$_P26_LOG_PATH")
    info "Step 107: synchronizing with a fresh heartbeat before unregister..."
    if ! _p26_wait_for_new_log "$_P26_LOG_PATH" "$_final_heartbeat_baseline" \
        "Heartbeat sent successfully" 40; then
        _step107_ok=0
        _issue107="could not synchronize with a fresh heartbeat within 40s"
    fi

    if [[ $_step107_ok -eq 1 ]]; then
        _reregister_log_baseline=$(_p26_log_line_count "$_P26_STDOUT_LOG")
        if ! _p26_unregister_node; then
            _step107_ok=0
            _issue107="UnregisterNode RPC failed or controller.proto/grpcurl is unavailable"
        else
            _p26_needs_recovery=1
            _controller_state=$(_p26_controller_node_state || true)
            if [[ "$_controller_state" != "absent" ]]; then
                _step107_ok=0
                _issue107="GET /api/nodes did not show the node absent immediately after unregister (state='${_controller_state:-error}')"
            fi
        fi
    fi

    if [[ $_step107_ok -eq 1 ]]; then
        info "Step 107: waiting 50s for heartbeat-triggered re-registration..."
        sleep 50
        if ! _p26_wait_for_new_log "$_P26_STDOUT_LOG" "$_reregister_log_baseline" \
            "attempting to re-register" 1; then
            _step107_ok=0
            _issue107="re-registration attempt log is missing; stdout='$(_p26_log_snippet "$_P26_STDOUT_LOG" "$_reregister_log_baseline")'"
        elif ! _p26_wait_for_new_log "$_P26_STDOUT_LOG" "$_reregister_log_baseline" \
            "Re-registration successful" 1; then
            _step107_ok=0
            _issue107="re-registration success log is missing; stdout='$(_p26_log_snippet "$_P26_STDOUT_LOG" "$_reregister_log_baseline")'"
        else
            _controller_state=$(_p26_controller_node_state || true)
            if [[ "$_controller_state" != "present" ]]; then
                _step107_ok=0
                _issue107="GET /api/nodes did not show the re-registered node (state='${_controller_state:-error}')"
            fi
        fi
    fi

    if [[ $_step107_ok -eq 1 ]]; then
        _final_heartbeat_baseline=$(_p26_log_line_count "$_P26_LOG_PATH")
        info "Step 107: waiting one more heartbeat cycle to verify steady state..."
        sleep 35
        if ! _p26_wait_for_new_log "$_P26_LOG_PATH" "$_final_heartbeat_baseline" \
            "Heartbeat sent successfully" 1; then
            _step107_ok=0
            _issue107="no successful steady-state heartbeat one cycle after re-registration"
        fi
    fi

    if [[ $_step107_ok -eq 1 ]]; then
        _p26_needs_recovery=0
        pass "Step 107: re-register after controller forgets node" \
            "REST record disappeared and returned; re-register logs and following heartbeat observed"
    else
        fail "Step 107: re-register after controller forgets node" "$_issue107"
    fi

    _cleanup_phase26_heartbeat_reregister || true
    return 0
}
