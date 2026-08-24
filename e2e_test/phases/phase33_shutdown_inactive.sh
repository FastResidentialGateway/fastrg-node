#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 33 — Graceful shutdown keeps the node visible (Steps 136-137)
#
# A graceful stop reports shutdown to the controller instead of unregistering,
# so nodes/<uuid> survives with status=inactive and the node stays on the
# controller UI. The next cold start re-registers and flips it back to active.
# ---------------------------------------------------------------------------

_P33_LOG_PATH=""
# Where the start command sends the node's stdout and stderr. A teardown
# crash writes its reason here (glibc abort text, C++ terminate) and never to
# the application log, and the next start truncates it.
_P33_STDERR_LOG="/var/log/fastrg.log"
_P33_RESTART_NEEDED=0

_p33_process_state() {
    ssh_node \
        "if pgrep -x fastrg >/dev/null 2>&1; then printf running; else printf stopped; fi" \
        2>/dev/null || true
}

_p33_log_line_count() {
    local _path="$1" _count

    _count=$(ssh_node "wc -l < '${_path}' 2>/dev/null || echo 0" 2>/dev/null | \
        tail -1 | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

_p33_new_log() {
    local _path="$1" _baseline="$2" _start

    [[ "$_baseline" =~ ^[0-9]+$ ]] || _baseline=0
    _start=$(( _baseline + 1 ))
    ssh_node "tail -n +${_start} '${_path}' 2>/dev/null || true" 2>/dev/null || true
}

_p33_wait_for_new_log() {
    local _path="$1" _baseline="$2" _needle="$3" _timeout="$4"
    local _elapsed _new=""

    for _elapsed in $(seq 1 "$_timeout"); do
        _new=$(_p33_new_log "$_path" "$_baseline" || true)
        if printf '%s\n' "$_new" | grep -qF "$_needle"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

_p33_log_snippet() {
    _p33_new_log "$1" "$2" | tr '\n' '|' | tail -c 1000 || true
}

# One evidence line: the content joined onto a single bounded line, or
# "unavailable" when the source could not be read.
_p33_evidence_line() {
    local _label="$1" _body="$2"

    _body=$(printf '%s' "$_body" | tr '\n' '|' | tail -c 2000 || true)
    [[ -n "${_body//[|[:space:]]/}" ]] || _body=""
    info "  Step 136 evidence: ${_label}: ${_body:-unavailable}"
}

# Printed only after Step 136 has already failed, and only from inside Step 136
# so it still runs before Step 137 cold-starts the node and truncates the logs.
# Answers "did the node die, did it say why, and was the machine short of
# memory when it happened".
_p33_failure_evidence() {
    _p33_evidence_line "node stdout+stderr, last 40 lines" \
        "$(ssh_node "tail -40 '${_P33_STDERR_LOG}' 2>/dev/null" 2>/dev/null || true)"
    _p33_evidence_line "crash reporter, last 5 lines" \
        "$(ssh_node "tail -5 /var/log/apport.log 2>/dev/null" 2>/dev/null || true)"
    _p33_evidence_line "node memory" \
        "$(ssh_node "awk '/^MemAvailable:|^SwapFree:/ { printf \"%s %s kB \", \$1, \$2 }' /proc/meminfo 2>/dev/null" 2>/dev/null || true)"
    _p33_evidence_line "kernel OOM lines" \
        "$(ssh_node "dmesg -T 2>/dev/null | grep -i oom | tail -3" 2>/dev/null || true)"
}

# Prints this node's controller record as "present <status> <reason>", or
# "absent" when the key is gone. Missing reason field prints as "-".
_p33_controller_node_entry() {
    P33_CONTROLLER_REST="$CONTROLLER_REST" \
        P33_CONTROLLER_USER="$CONTROLLER_USER" \
        P33_CONTROLLER_PASS="$CONTROLLER_PASS" \
        P33_NODE_UUID="$NODE_UUID" \
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
        "username": os.environ["P33_CONTROLLER_USER"],
        "password": os.environ["P33_CONTROLLER_PASS"],
    }).encode()
    request = urllib.request.Request(
        os.environ["P33_CONTROLLER_REST"] + "/api/login",
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
        os.environ["P33_CONTROLLER_REST"] + "/api/nodes",
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

# /api/nodes returns [{"key": "nodes/<uuid>", "value": "<etcd JSON string>"}].
wanted = "nodes/" + os.environ["P33_NODE_UUID"]
for node in nodes:
    if node.get("key") != wanted:
        continue
    value = json.loads(node.get("value") or "{}")
    print("present", value.get("status") or "-", value.get("inactive_reason") or "-")
    break
else:
    print("absent")
PY
}

# Idempotent: called after phase33 and from the top-level EXIT trap.
_cleanup_phase33_shutdown_inactive() {
    local _stopped=0 _started=0 _i

    if [[ "${_P33_RESTART_NEEDED:-0}" -ne 1 ]]; then
        return 0
    fi

    warn "Cleanup(phase33): node was left stopped; retrying fastrg startup best-effort."

    # A half-dead process would keep the new instance from taking the ports, so
    # make sure the old one is gone first. SIGTERM only — SIGKILL skips the
    # RCU and hugepage teardown.
    if [[ "$(_p33_process_state)" == "running" ]]; then
        ssh_node "pkill -x fastrg" >/dev/null 2>&1 || true
        for _i in $(seq 1 15); do
            if [[ "$(_p33_process_state)" == "stopped" ]]; then
                _stopped=1
                break
            fi
            sleep 1
        done
        if [[ $_stopped -ne 1 ]]; then
            warn "Cleanup(phase33): existing fastrg did not exit after SIGTERM; startup retry skipped."
            return 0
        fi
    fi

    ssh_node "nohup ${_FASTRG_START_CMD} >/var/log/fastrg.log 2>&1 &" >/dev/null 2>&1 || true
    _FASTRG_STARTED_BY_SCRIPT=1
    for _i in $(seq 1 15); do
        if [[ "$(_p33_process_state)" == "running" ]]; then
            _started=1
            break
        fi
        sleep 1
    done

    if [[ $_started -eq 1 ]]; then
        info "Cleanup(phase33): fastrg startup retry launched successfully."
        _P33_RESTART_NEEDED=0
    else
        warn "Cleanup(phase33): fastrg startup retry did not launch a process."
    fi
    return 0
}

phase33_shutdown_inactive() {
    local _issue135="" _issue136=""
    local _config_log_path="" _log_baseline=0
    local _entry="" _presence="" _status="" _reason=""
    local _stopped=0 _relaunched=0 _recovered=0
    local _controller_ok=0 _shutdown_seconds=0
    local _hsi="" _status1="" _status2=""
    local _i

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 33 — Shutdown Keeps Node Visible (Steps 136-137)"
    bold "═══════════════════════════════════════════════════════"

    _config_log_path=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" || true)
    _P33_LOG_PATH=$(printf '%s' "$_config_log_path" | awk -F'"' '{print $2}' || true)
    [[ -n "$_P33_LOG_PATH" ]] || _P33_LOG_PATH=/var/log/fastrg/fastrg.log

    # ------------------------------------------------------------------
    # Step 136 — SIGTERM the node, then verify the controller still lists it
    # as inactive with the self-reported shutdown reason.
    # ------------------------------------------------------------------
    info "Step 136: stopping fastrg gracefully and checking the controller record..."

    if [[ "$(_p33_process_state)" != "running" ]]; then
        fail "Step 136: graceful stop marks node inactive" "fastrg not running before shutdown"
    else
        _log_baseline=$(_p33_log_line_count "$_P33_LOG_PATH")
        info "  log baseline: ${_log_baseline} lines in ${_P33_LOG_PATH}"

        _P33_RESTART_NEEDED=1
        if ! ssh_node "pkill -x fastrg" >/dev/null 2>&1; then
            _issue135="${_issue135} SIGTERM_delivery=failed"
        fi
        for _i in $(seq 1 30); do
            if [[ "$(_p33_process_state)" == "stopped" ]]; then
                _stopped=1
                _shutdown_seconds=$_i
                break
            fi
            sleep 1
        done
        [[ $_stopped -eq 1 ]] || _issue135="${_issue135} fastrg_still_running_after_30s"

        # fastrg.log is truncated on every start, so this must be read before
        # Step 137 cold-starts the node.
        if ! _p33_wait_for_new_log "$_P33_LOG_PATH" "$_log_baseline" \
            "Reported shutdown to controller" 5; then
            _issue135="${_issue135} report_log_missing; log='$(_p33_log_snippet "$_P33_LOG_PATH" "$_log_baseline")'"
        fi

        # inactive_reason distinguishes the node's own report from the
        # controller's heartbeat-timeout detection (which writes
        # heartbeat_timeout), so it proves the shutdown RPC actually landed.
        for _i in $(seq 1 15); do
            _entry=$(_p33_controller_node_entry || true)
            read -r _presence _status _reason <<< "$_entry" || true
            if [[ "$_presence" == "present" && "$_status" == "inactive" && \
                  "$_reason" == "node_shutdown" ]]; then
                _controller_ok=1
                break
            fi
            sleep 1
        done
        [[ $_controller_ok -eq 1 ]] || \
            _issue135="${_issue135} controller_state='${_entry:-error}'"

        if [[ -z "$_issue135" ]]; then
            pass "Step 136: graceful stop marks node inactive" \
                "node still listed after SIGTERM (exited in ${_shutdown_seconds}s): status=${_status}, inactive_reason=${_reason}; shutdown report logged"
        else
            fail "Step 136: graceful stop marks node inactive" "${_issue135# }"
            _p33_failure_evidence
        fi
    fi

    # ------------------------------------------------------------------
    # Step 137 — Cold start: both subscribers return to Data phase and the
    # controller record flips back to active.
    # ------------------------------------------------------------------
    info "Step 137: cold-starting fastrg and waiting up to 150s for both users..."

    if ssh_node "nohup ${_FASTRG_START_CMD} >/var/log/fastrg.log 2>&1 &" >/dev/null 2>&1; then
        _relaunched=1
    else
        _issue136="fastrg relaunch failed"
    fi
    _FASTRG_STARTED_BY_SCRIPT=1

    if [[ $_relaunched -eq 1 ]]; then
        for _i in $(seq 1 30); do
            sleep 5
            _hsi=$(fastrg_grpc get_hsi_info 2>/dev/null || true)
            _status1=$(printf '%s' "$_hsi" | \
                jq -r '.hsi_infos[] | select(.user_id == 1) | .status // empty' 2>/dev/null || true)
            _status2=$(printf '%s' "$_hsi" | \
                jq -r '.hsi_infos[] | select(.user_id == 2) | .status // empty' 2>/dev/null || true)
            if [[ "$_status1" == "Data phase" && "$_status2" == "Data phase" ]]; then
                _recovered=1
                break
            fi
            info "  still recovering... (${_i}x5s, user1='${_status1:-unreachable}', user2='${_status2:-unreachable}')"
        done
        [[ $_recovered -eq 1 ]] || \
            _issue136="${_issue136} recovery_timeout user1='${_status1:-empty}' user2='${_status2:-empty}'"

        # Registration happens far earlier in startup than Data phase, so the
        # record is already active by now. inactive_reason is only reported for
        # human inspection — re-registration rewrites the whole record.
        _controller_ok=0
        for _i in $(seq 1 15); do
            _entry=$(_p33_controller_node_entry || true)
            read -r _presence _status _reason <<< "$_entry" || true
            if [[ "$_presence" == "present" && "$_status" == "active" ]]; then
                _controller_ok=1
                break
            fi
            sleep 1
        done
        [[ $_controller_ok -eq 1 ]] || \
            _issue136="${_issue136} controller_state='${_entry:-error}'"
    fi

    if [[ -z "$_issue136" ]]; then
        _P33_RESTART_NEEDED=0
        pass "Step 137: cold start restores active" \
            "users 1/2 back in Data phase; controller status=${_status}, inactive_reason=${_reason}"
    else
        fail "Step 137: cold start restores active" "${_issue136# }"
    fi

    return 0
}
