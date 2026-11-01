#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 13 — Per-Subscriber Packet Capture (exec pdump) (Steps 48-51c)
#
# Verifies the pdump capture path end-to-end:
#   fastrg_grpc pdump_start/stop → pcap file on node
#
#   Step 48  basic capture (ALL, all subscribers, no filter) → file non-empty
#   Step 49  selective stop (stop LAN USER_ID only, session continues)
#   Step 50  BPF filter ("vlan and icmp") → only ICMP captured, file non-empty
#   Step 51  invalid filter expression → error returned, no session opened
#   Step 51b capture on every subscriber, count lowered by one → stop all
#            closes the pcap file and the next start opens a new one
#   Step 51c capture only on USER_ID, count lowered past it → the count change
#            alone closes the file, and raising the count back does not
#            resume writing into it
# ---------------------------------------------------------------------------
_p13_capture_steps() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 13 — Per-Subscriber Packet Capture (Steps 48-51c)"
    bold "═══════════════════════════════════════════════════════"

    # Ensure no stale capture session is running from a previous (failed) run.
    fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true

    # -----------------------------------------------------------------------
    # Step 48 — basic capture: ALL directions, all subscribers, no filter
    # -----------------------------------------------------------------------
    info "Step 48: pdump start (ALL, all subscribers, no filter)..."

    _P45_OUT=$(fastrg_grpc pdump_start 0 0 2>&1 || true)
    _P45_FILE=$(printf '%s' "$_P45_OUT" | python3 -c \
        "import sys,json; d=json.load(sys.stdin); print(d.get('pcap_file',''))" 2>/dev/null || true)
    _P45_STATUS=$(printf '%s' "$_P45_OUT" | python3 -c \
        "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || true)

    if [[ -z "$_P45_FILE" ]] || printf '%s' "$_P45_OUT" | grep -qi '"error"'; then
        fail "Step 48: pdump basic capture" "pdump_start failed: ${_P45_OUT}"
        fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true
        skip "Step 49: pdump selective stop" "Step 48 failed"
        skip "Step 50: pdump BPF filter"     "Step 48 failed"
        skip "Step 51: pdump invalid filter" "Step 48 failed"
        return
    fi
    info "  pdump started → pcap: ${_P45_FILE}"

    # Send traffic so there are packets to capture.
    ssh_lan "ping -c 8 -W 3 ${WAN_IP} >/dev/null 2>&1 || true" 2>/dev/null || true
    sleep 1

    fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true

    # Verify file exists and has content beyond the 24-byte global header.
    _P45_SIZE=$(ssh_node "stat -c %s '${_P45_FILE}' 2>/dev/null || echo 0" 2>/dev/null \
        | tr -d '[:space:]')
    if [[ "${_P45_SIZE:-0}" -gt 24 ]]; then
        pass "Step 48: pdump basic capture" \
            "pcap written to ${_P45_FILE} (${_P45_SIZE} bytes)"
    else
        fail "Step 48: pdump basic capture" \
            "pcap file missing or has no packet records (size=${_P45_SIZE:-0}): ${_P45_FILE}"
    fi

    # -----------------------------------------------------------------------
    # Step 49 — selective stop: stop LAN for USER_ID, session must continue
    # -----------------------------------------------------------------------
    bold "---"
    info "Step 49: pdump selective stop (start ALL all → stop LAN ${USER_ID} → session continues)..."

    _P46_OUT=$(fastrg_grpc pdump_start 0 0 2>&1 || true)
    _P46_FILE=$(printf '%s' "$_P46_OUT" | python3 -c \
        "import sys,json; d=json.load(sys.stdin); print(d.get('pcap_file',''))" 2>/dev/null || true)

    if [[ -z "$_P46_FILE" ]] || printf '%s' "$_P46_OUT" | grep -qi '"error"'; then
        fail "Step 49: pdump selective stop" "pdump_start failed: ${_P46_OUT}"
        skip "Step 50: pdump BPF filter"     "Step 49 setup failed"
        skip "Step 51: pdump invalid filter" "Step 49 setup failed"
        return
    fi

    # Stop only LAN side for the primary subscriber — WAN + other sub LAN continue.
    _P46_STOP=$(fastrg_grpc pdump_stop 2 "${USER_ID}" 2>&1 || true)
    if printf '%s' "$_P46_STOP" | grep -qi '"error"'; then
        fail "Step 49: pdump selective stop" \
            "pdump_stop(LAN, ${USER_ID}) failed: ${_P46_STOP}"
        fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true
        skip "Step 50: pdump BPF filter"     "Step 49 failed"
        skip "Step 51: pdump invalid filter" "Step 49 failed"
        return
    fi
    info "  stopped LAN subscriber ${USER_ID}; session continues on WAN + other subs"

    # More traffic after partial stop — WAN side still captures it.
    ssh_lan "ping -c 5 -W 3 ${WAN_IP} >/dev/null 2>&1 || true" 2>/dev/null || true
    sleep 1

    # Stop all remaining; verify file is non-empty.
    fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true
    _P46_SIZE=$(ssh_node "stat -c %s '${_P46_FILE}' 2>/dev/null || echo 0" 2>/dev/null \
        | tr -d '[:space:]')
    if [[ "${_P46_SIZE:-0}" -gt 24 ]]; then
        pass "Step 49: pdump selective stop" \
            "session continued after partial stop; final pcap ${_P46_SIZE} bytes"
    else
        fail "Step 49: pdump selective stop" \
            "pcap file missing or empty after selective stop (size=${_P46_SIZE:-0})"
    fi

    # -----------------------------------------------------------------------
    # Step 50 — BPF filter: "vlan and icmp" on LAN side for USER_ID
    # -----------------------------------------------------------------------
    bold "---"
    info "Step 50: pdump BPF filter (LAN, subscriber ${USER_ID}, filter='vlan and icmp')..."

    _P47_OUT=$(fastrg_grpc pdump_start 2 "${USER_ID}" "vlan and icmp" 2>&1 || true)
    _P47_FILE=$(printf '%s' "$_P47_OUT" | python3 -c \
        "import sys,json; d=json.load(sys.stdin); print(d.get('pcap_file',''))" 2>/dev/null || true)

    if [[ -z "$_P47_FILE" ]] || printf '%s' "$_P47_OUT" | grep -qi '"error"'; then
        fail "Step 50: pdump BPF filter" "pdump_start with filter failed: ${_P47_OUT}"
        skip "Step 51: pdump invalid filter" "Step 50 setup failed"
        return
    fi
    info "  pdump started with icmp filter → pcap: ${_P47_FILE}"

    # Send ICMP and TCP — only ICMP should land in the pcap.
    ssh_lan "ping -c 8 -W 3 ${WAN_IP} >/dev/null 2>&1 || true" 2>/dev/null || true
    ssh_lan "curl -s --connect-timeout 5 -o /dev/null http://${WAN_IP}/ 2>/dev/null || true" \
        2>/dev/null || true
    sleep 1

    fastrg_grpc pdump_stop 2 "${USER_ID}" >/dev/null 2>&1 || true

    _P47_SIZE=$(ssh_node "stat -c %s '${_P47_FILE}' 2>/dev/null || echo 0" 2>/dev/null \
        | tr -d '[:space:]')
    _P47_ICMP_COUNT=$(ssh_node \
        "tcpdump -r '${_P47_FILE}' -nn 'vlan and icmp' 2>/dev/null | wc -l" 2>/dev/null \
        | tr -d '[:space:]' || true)
    _P47_TCP_COUNT=$(ssh_node \
        "tcpdump -r '${_P47_FILE}' -nn 'vlan and tcp' 2>/dev/null | wc -l" 2>/dev/null \
        | tr -d '[:space:]' || true)
    if [[ "${_P47_ICMP_COUNT:-0}" -gt 0 && "${_P47_TCP_COUNT:-0}" -eq 0 ]]; then
        _P47_DETAIL="pcap contains ${_P47_ICMP_COUNT} ICMP and ${_P47_TCP_COUNT} TCP packets"
        _P47_DETAIL+=" (${_P47_SIZE:-0} bytes): ${_P47_FILE}"
        pass "Step 50: pdump BPF filter (vlan and icmp)" "${_P47_DETAIL}"
    else
        _P47_DETAIL="expected ICMP > 0 and TCP = 0; got ICMP=${_P47_ICMP_COUNT:-0},"
        _P47_DETAIL+=" TCP=${_P47_TCP_COUNT:-0} (size=${_P47_SIZE:-0}): ${_P47_FILE}"
        fail "Step 50: pdump BPF filter (vlan and icmp)" "${_P47_DETAIL}"
    fi

    # -----------------------------------------------------------------------
    # Step 51 — invalid BPF filter expression → gRPC must return an error
    # -----------------------------------------------------------------------
    bold "---"
    info "Step 51: pdump invalid filter expression → expect error..."

    # Control: a legal filter must open a session first. Without it, a dead
    # control path returns no pcap_file for the invalid filter too, and the
    # rejection below would read as a pass.
    _P48_CONTROL_OUT=$(fastrg_grpc pdump_start 2 "${USER_ID}" "vlan and icmp" 2>&1 || true)
    _P48_CONTROL_FILE=$(printf '%s' "$_P48_CONTROL_OUT" | python3 -c \
        "import sys,json; d=json.load(sys.stdin); print(d.get('pcap_file',''))" 2>/dev/null || true)
    fastrg_grpc pdump_stop 2 "${USER_ID}" >/dev/null 2>&1 || true

    if [[ -z "$_P48_CONTROL_FILE" ]]; then
        fail "Step 51: pdump invalid filter rejected" \
            "pdump control path broken: a legal filter returned no pcap_file: $(printf '%s' "$_P48_CONTROL_OUT" | tr '\n' ' ' | cut -c 1-200)"
    else
        _P48_OUT=$(fastrg_grpc pdump_start 0 0 "(unclosed-paren-syntax-error" 2>&1 || true)
        _P48_FILE=$(printf '%s' "$_P48_OUT" | python3 -c \
            "import sys,json; d=json.load(sys.stdin); print(d.get('pcap_file',''))" 2>/dev/null || true)

        if [[ -n "$_P48_FILE" ]]; then
            fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true
            fail "Step 51: pdump invalid filter rejected" \
                "expected error but got pcap_file=${_P48_FILE}"
        else
            pass "Step 51: pdump invalid filter rejected" \
                "a legal filter opened a session in this step; the invalid one returned no pcap_file"
        fi
    fi
}

# ---------------------------------------------------------------------------
# Steps 51b/51c — lowering the subscriber count while a capture is open.
# Both lower the count by one so USER_ID (the last slot) is the one removed,
# then put it back and wait for USER_ID to redial.
# ---------------------------------------------------------------------------

_P13_LOG_PATH=""
_P13_ORIG_COUNT=""
_P13_COUNT_CHANGED=0
_P13_CAPTURE_TOUCHED=0
_P13_RESTORE_ISSUE=""
_P13_LOG_VERDICT=""

# e2e_pdump_reply_verdict REPLY [PREV_FILE] — what a pdump start/stop reply
# (client stdout and stderr together) says:
#   new      a capture file other than PREV_FILE was reported
#   same     the reported capture file is PREV_FILE
#   stopped  a stop succeeded
#   idle     the node said no capture is running
#   error    any other error reply
#   err      empty or not a single JSON object: the call cannot be judged
e2e_pdump_reply_verdict() {
    local _reply="${1:-}" _prev="${2:-}" _verdict=""

    _verdict=$(printf '%s' "$_reply" | jq -rs --arg prev "$_prev" '
        if length != 1 or (.[0] | type) != "object" then "err"
        else .[0]
            | if (.error // "") != "" then
                  (if (.error | contains("no capture is running")) then "idle" else "error" end)
              elif (.pcap_file // "") != "" then
                  (if .pcap_file == $prev then "same" else "new" end)
              elif (.status // "") == "capture stopped" then "stopped"
              else "err" end
        end' 2>/dev/null || true)
    [[ "$_verdict" =~ ^(new|same|stopped|idle|error)$ ]] || { printf 'err'; return 1; }
    printf '%s' "$_verdict"
}

# e2e_pdump_fd_state FD_LISTING FILE — whether an `ls -l /proc/<pid>/fd`
# listing has a descriptor on FILE:
#   open    some fd points at FILE (also when FILE was deleted while open)
#   closed  the listing has descriptors and none points at FILE
#   err     no descriptor in the listing or no FILE: nothing to judge
e2e_pdump_fd_state() {
    local _listing="${1:-}" _file="${2:-}"

    [[ -n "$_file" ]] || { printf 'err'; return 1; }
    printf '%s\n' "$_listing" | grep -qF ' -> ' || { printf 'err'; return 1; }
    if printf '%s\n' "$_listing" | awk -v f="$_file" '
        { i = index($0, " -> "); if (i == 0) next
          t = substr($0, i + 4); sub(/ \(deleted\)$/, "", t)
          if (t == f) { found = 1; exit } }
        END { exit !found }'; then
        printf 'open'
    else
        printf 'closed'
    fi
}

# e2e_pdump_log_written LOG_LINES FILE — whether the node logged closing FILE:
#   written  a line ends in "pdump capture written to FILE"
#   absent   there are log lines but none closes FILE
#   err      no log lines or no FILE: the log cannot be judged
e2e_pdump_log_written() {
    local _log="${1:-}" _file="${2:-}"

    [[ -n "$_log" && -n "$_file" ]] || { printf 'err'; return 1; }
    if printf '%s\n' "$_log" | awk -v want="pdump capture written to $_file" '
        { n = length($0); w = length(want)
          if (n >= w && substr($0, n - w + 1) == want) { found = 1; exit } }
        END { exit !found }'; then
        printf 'written'
    else
        printf 'absent'
    fi
}

local_validation_register pdump_reply_verdict e2e_pdump_reply_verdict \
    pdump_reply_verdict_new \
    pdump_reply_verdict_same_session \
    pdump_reply_verdict_new_session \
    pdump_reply_verdict_stopped \
    pdump_reply_verdict_idle \
    pdump_reply_verdict_other_error \
    pdump_reply_verdict_empty_input \
    pdump_reply_verdict_not_json
local_validation_register pdump_fd_state e2e_pdump_fd_state \
    pdump_fd_state_open \
    pdump_fd_state_closed \
    pdump_fd_state_other_capture_open \
    pdump_fd_state_deleted_while_open \
    pdump_fd_state_empty_input \
    pdump_fd_state_no_descriptors
local_validation_register pdump_log_written e2e_pdump_log_written \
    pdump_log_written_good \
    pdump_log_written_absent \
    pdump_log_written_other_file \
    pdump_log_written_start_line_only \
    pdump_log_written_empty_input

# Same RPC as fastrg_grpc but keeps stderr, where the client puts error replies.
_p13_grpc_reply() {
    python3 "${GRPC_CLIENT_DIR}/fastrg_grpc_client.py" \
        --node "${FASTRG_NODE}:${FASTRG_GRPC_PORT}" "$@" 2>&1 || true
}

_p13_reply_file() {
    printf '%s' "$1" | jq -r '.pcap_file // empty' 2>/dev/null || true
}

# One line of a reply for a failure message.
_p13_short() {
    printf '%s' "$1" | tr '\n' ' ' | cut -c 1-200 || true
}

# Line count of the node log; empty when it cannot be read.
_p13_log_lines() {
    local _n

    _n=$(ssh_node "wc -l < '${_P13_LOG_PATH}'" 2>/dev/null | tr -d '[:space:]' || true)
    [[ "$_n" =~ ^[0-9]+$ ]] || _n=""
    printf '%s' "$_n"
}

# Node log lines after line $1; nothing when $1 is not a line count.
_p13_new_log() {
    [[ "${1:-}" =~ ^[0-9]+$ ]] || return 0
    ssh_node "tail -n +$(( $1 + 1 )) '${_P13_LOG_PATH}'" 2>/dev/null || true
}

_p13_node_pid() {
    ssh_node "pgrep -xo fastrg" 2>/dev/null | tr -dc '0-9' || true
}

_p13_fd_listing() {
    [[ -n "${1:-}" ]] || return 0
    ssh_node "ls -l /proc/$1/fd" 2>/dev/null || true
}

_p13_file_size() {
    ssh_node "stat -c %s '$1'" 2>/dev/null | tr -d '[:space:]' || true
}

# Wait up to 15s for the node to log closing $2 after log line $1. The last
# verdict is left in _P13_LOG_VERDICT.
_p13_wait_written() {
    local _i

    for _i in $(seq 1 15); do
        _P13_LOG_VERDICT=$(e2e_pdump_log_written "$(_p13_new_log "$1")" "$2" || true)
        [[ "$_P13_LOG_VERDICT" == "written" ]] && return 0
        sleep 1
    done
    return 1
}

# LAN traffic on USER_ID's VLAN, the same traffic Steps 48-50 capture.
_p13_user_traffic() {
    ssh_lan "ping -c 5 -W 3 ${WAN_IP} >/dev/null 2>&1 || true" 2>/dev/null || true
    sleep 1
}

# Condition: the node and etcd both hold subscriber count $1.
_p13_count_is() {
    local _raw _etcd _local

    _raw=$(etcdctl_get_value "user_counts/${NODE_UUID}/") || return 2
    _etcd=$(printf '%s' "$_raw" | jq -r '.subscriber_count // empty' 2>/dev/null || true)
    _raw=$(fastrg_grpc_checked get_system_info) || return 2
    _local=$(printf '%s' "$_raw" | jq -r '.num_users // empty' 2>/dev/null || true)
    [[ "$_etcd" == "$1" && "$_local" == "$1" ]]
}

# Condition: subscriber $1 is in Data phase.
_p13_user_in_data_phase() {
    local _raw _status

    _raw=$(fastrg_grpc_checked get_hsi_info) || return 2
    _status=$(printf '%s' "$_raw" | \
        jq -r ".hsi_infos[]? | select(.user_id == $1) | .status" 2>/dev/null || true)
    [[ "$_status" == "Data phase" ]]
}

_p13_lower_count() {
    local _target=$(( _P13_ORIG_COUNT - 1 ))

    _P13_COUNT_CHANGED=1
    fastrg_grpc set_subscriber_count "$_target" >/dev/null 2>&1 || true
    _e2e_wait_for "Phase 13: subscriber count down to ${_target}" 15 2 _p13_count_is "$_target"
}

# Put the count back and wait for USER_ID to redial. The problem, if any, is
# left in _P13_RESTORE_ISSUE.
_p13_restore_count() {
    _P13_RESTORE_ISSUE=""
    fastrg_grpc set_subscriber_count "${_P13_ORIG_COUNT}" >/dev/null 2>&1 || true
    if ! _e2e_wait_for "Phase 13: subscriber count back to ${_P13_ORIG_COUNT}" 15 2 \
            _p13_count_is "${_P13_ORIG_COUNT}"; then
        _P13_RESTORE_ISSUE="subscriber count did not return to ${_P13_ORIG_COUNT}"
        return 1
    fi
    _P13_COUNT_CHANGED=0
    if ! _e2e_wait_for "Phase 13: user ${USER_ID} back in Data phase" 75 2 \
            _p13_user_in_data_phase "${USER_ID}"; then
        _P13_RESTORE_ISSUE="user ${USER_ID} did not return to Data phase within 150s"
        return 1
    fi
    return 0
}

# Idempotent: called at the end of phase13 and from the top-level EXIT trap.
# The count goes back first: a capture flag left on a removed subscriber can
# only be stopped once that subscriber is within the count again.
_cleanup_phase13_pdump() {
    if [[ "${_P13_COUNT_CHANGED:-0}" -eq 1 && -n "${_P13_ORIG_COUNT:-}" ]]; then
        info "Cleanup(phase13): restoring the subscriber count to ${_P13_ORIG_COUNT}..."
        _p13_restore_count || warn "Cleanup(phase13): ${_P13_RESTORE_ISSUE}"
    fi
    if [[ "${_P13_CAPTURE_TOUCHED:-0}" -eq 1 ]]; then
        fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true
        _P13_CAPTURE_TOUCHED=0
    fi
    return 0
}

# Start a capture with "$@" (dir subscriber) and check fastrg holds the file
# open. Sets _p13_file and _p13_pid; on failure records a precondition FAIL
# for $1 and returns 1.
_p13_open_capture() {
    local _name="$1" _reply _verdict _state

    shift
    _P13_CAPTURE_TOUCHED=1
    _reply=$(_p13_grpc_reply pdump_start "$@")
    _verdict=$(e2e_pdump_reply_verdict "$_reply" || true)
    _p13_file=$(_p13_reply_file "$_reply")
    if [[ "$_verdict" != "new" || -z "$_p13_file" ]]; then
        fail "$_name" "precondition: pdump_start $* opened no capture (${_verdict}): $(_p13_short "$_reply")"
        return 1
    fi
    _p13_pid=$(_p13_node_pid)
    _state=$(e2e_pdump_fd_state "$(_p13_fd_listing "$_p13_pid")" "$_p13_file" || true)
    if [[ "$_state" != "open" ]]; then
        fail "$_name" "precondition: fastrg pid ${_p13_pid:-none} does not hold ${_p13_file} open (fd check: ${_state})"
        return 1
    fi
    info "  capture ${_p13_file} open in fastrg pid ${_p13_pid}"
    return 0
}

_p13_step51b() {
    local _name="Step 51b: stop all after a count drop closes the capture"
    local _base="" _stop="" _state="" _reply="" _verdict="" _file2="" _issue=""
    local _p13_file="" _p13_pid=""

    info "Step 51b: capture all, lower the count to $(( _P13_ORIG_COUNT - 1 )), then stop all..."
    fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true
    _base=$(_p13_log_lines)
    _p13_open_capture "$_name" 0 0 || { _cleanup_phase13_pdump; return 0; }

    if ! _p13_lower_count; then
        fail "$_name" "precondition: subscriber count did not drop to $(( _P13_ORIG_COUNT - 1 ))"
        _cleanup_phase13_pdump
        return 0
    fi
    _stop=$(e2e_pdump_reply_verdict "$(_p13_grpc_reply pdump_stop 0 0)" || true)

    _p13_wait_written "$_base" "$_p13_file" || \
        _issue="(a) node log has no 'pdump capture written to ${_p13_file}' (${_P13_LOG_VERDICT}; stop all: ${_stop})"
    _state=$(e2e_pdump_fd_state "$(_p13_fd_listing "$_p13_pid")" "$_p13_file" || true)
    [[ "$_state" == "closed" ]] || \
        _issue="${_issue:+${_issue}; }(b) fastrg pid ${_p13_pid} fd check on ${_p13_file}: ${_state}"
    _reply=$(_p13_grpc_reply pdump_start 0 0)
    _verdict=$(e2e_pdump_reply_verdict "$_reply" "$_p13_file" || true)
    _file2=$(_p13_reply_file "$_reply")
    [[ "$_verdict" == "new" ]] || \
        _issue="${_issue:+${_issue}; }(c) the next start got '${_verdict}' (${_file2:-$(_p13_short "$_reply")}), not a new file"
    fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true

    _p13_restore_count || _issue="${_issue:+${_issue}; }restore: ${_P13_RESTORE_ISSUE}"
    _cleanup_phase13_pdump

    if [[ -z "$_issue" ]]; then
        pass "$_name" "count ${_P13_ORIG_COUNT}->$(( _P13_ORIG_COUNT - 1 )) then stop all closed ${_p13_file} (logged, fd gone); next start opened ${_file2}"
    else
        fail "$_name" "$_issue"
    fi
}

_p13_step51c() {
    local _name="Step 51c: a count drop closes a capture on the removed subscriber"
    local _base="" _size0="" _size1="" _size2="" _size3="" _state="" _stop="" _issue=""
    local _p13_file="" _p13_pid=""

    info "Step 51c: capture user ${USER_ID} only, lower the count past it, no stop..."
    if ! _p13_count_is "${_P13_ORIG_COUNT}" || ! _p13_user_in_data_phase "${USER_ID}"; then
        fail "$_name" "precondition: count is not ${_P13_ORIG_COUNT} or user ${USER_ID} is not in Data phase"
        return 0
    fi
    fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true
    _base=$(_p13_log_lines)
    _p13_open_capture "$_name" 0 "${USER_ID}" || { _cleanup_phase13_pdump; return 0; }

    # Control for (d): the same traffic does land in the file while it is open.
    _size0=$(_p13_file_size "$_p13_file")
    _p13_user_traffic
    _size1=$(_p13_file_size "$_p13_file")
    if ! e2e_counter_increased "$_size0" "$_size1"; then
        fail "$_name" "precondition: user ${USER_ID} traffic did not grow ${_p13_file} while capturing (${_size0:-?} -> ${_size1:-?} bytes)"
        _cleanup_phase13_pdump
        return 0
    fi

    if ! _p13_lower_count; then
        fail "$_name" "precondition: subscriber count did not drop to $(( _P13_ORIG_COUNT - 1 ))"
        _cleanup_phase13_pdump
        return 0
    fi
    _p13_wait_written "$_base" "$_p13_file" || \
        _issue="(a) node log has no 'pdump capture written to ${_p13_file}' (${_P13_LOG_VERDICT})"
    _state=$(e2e_pdump_fd_state "$(_p13_fd_listing "$_p13_pid")" "$_p13_file" || true)
    [[ "$_state" == "closed" ]] || \
        _issue="${_issue:+${_issue}; }(b) fastrg pid ${_p13_pid} fd check on ${_p13_file}: ${_state}"
    _stop=$(e2e_pdump_reply_verdict "$(_p13_grpc_reply pdump_stop 0 0)" || true)
    [[ "$_stop" == "idle" ]] || \
        _issue="${_issue:+${_issue}; }(c) stop all got '${_stop}', not 'no capture is running'"

    # (d) the closed file stays as it is once the subscriber is back.
    _size2=$(_p13_file_size "$_p13_file")
    if _p13_restore_count; then
        _p13_user_traffic
        _size3=$(_p13_file_size "$_p13_file")
        if ! e2e_all_uint "$_size2" "$_size3"; then
            _issue="${_issue:+${_issue}; }(d) cannot read the size of ${_p13_file} (${_size2:-?} -> ${_size3:-?})"
        elif e2e_counter_increased "$_size2" "$_size3"; then
            _issue="${_issue:+${_issue}; }(d) ${_p13_file} grew ${_size2} -> ${_size3} bytes after user ${USER_ID} came back"
        fi
    else
        _issue="${_issue:+${_issue}; }(d) restore: ${_P13_RESTORE_ISSUE}"
    fi
    _cleanup_phase13_pdump

    if [[ -z "$_issue" ]]; then
        pass "$_name" "count ${_P13_ORIG_COUNT}->$(( _P13_ORIG_COUNT - 1 )) closed ${_p13_file} with no stop (logged, fd gone, stop all idle); stayed ${_size3} bytes after user ${USER_ID} redialled (grew ${_size0}->${_size1} while open)"
    else
        fail "$_name" "$_issue"
    fi
}

phase13_pdump() {
    local _raw=""

    _p13_capture_steps

    bold "---"
    _raw=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" || true)
    _P13_LOG_PATH=$(printf '%s' "$_raw" | awk -F'"' '{print $2}' || true)
    [[ -n "$_P13_LOG_PATH" ]] || _P13_LOG_PATH=/var/log/fastrg/fastrg.log
    _P13_ORIG_COUNT=$(fastrg_grpc get_system_info | jq -r '.num_users // empty' 2>/dev/null || true)
    if [[ "$_P13_ORIG_COUNT" != "$USER_ID" || "$USER_ID" -lt 2 ]]; then
        fail "Step 51b: stop all after a count drop closes the capture" \
            "needs user ${USER_ID} as the last of at least 2 subscribers, node has ${_P13_ORIG_COUNT:-no} subscribers"
        fail "Step 51c: a count drop closes a capture on the removed subscriber" \
            "needs user ${USER_ID} as the last of at least 2 subscribers, node has ${_P13_ORIG_COUNT:-no} subscribers"
        return 0
    fi

    _p13_step51b
    bold "---"
    _p13_step51c
}
