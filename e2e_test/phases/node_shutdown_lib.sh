#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Crash check for a SIGTERM stop of the node, shared by every step that stops
# fastrg. The process being gone does not prove a clean stop: a crash during
# teardown ends it too. A clean stop ends the node's stdout log with "bye!"
# and leaves no new apport record and no new kernel trap line.
#
# Usage: take a mark before the SIGTERM, check it once the process is gone.
#   _mark=$(e2e_node_stop_mark)
#   ssh_node "pkill -x fastrg"; <wait for the process to exit>
#   _crash=$(e2e_node_shutdown_check "$_mark") || _issue="... ${_crash}"
# ---------------------------------------------------------------------------

# Run on the node before the SIGTERM. Prints "<stdout file> <apport inode>
# <apport size> <epoch>"; the stdout file is "-" when fastrg is not running
# or its stdout is not a regular file.
_E2E_NODE_STOP_MARK_SCRIPT='
pid=$(pgrep -xo fastrg)
out=-
[ -n "$pid" ] && out=$(readlink -f "/proc/$pid/fd/1" 2>/dev/null)
[ -f "$out" ] || out=-
printf "%s %s %s\n" "$out" "$(stat -c "%i %s" /var/log/apport.log 2>/dev/null || echo "- 0")" "$(date +%s)"
'

# Run on the node after the stop, with LOG/INODE/SIZE/SINCE set from the mark.
# Each source reports "<name>: ok" only when it could really be read, so a
# broken collector never reads as "no crash"; apport counts only while it is
# the core handler, and the kernel source must hold at least one line.
_E2E_NODE_SHUTDOWN_EVIDENCE_SCRIPT='
if [ -f "$LOG" ] && [ -r "$LOG" ]; then
    echo "log: ok"
    printf "log_last: %s\n" "$(tail -n 50 "$LOG" | grep -av "^[[:space:]]*$" | tail -n 1)"
else
    echo "log: unreadable"
fi
A=/var/log/apport.log
case "$(cat /proc/sys/kernel/core_pattern 2>/dev/null)" in
*apport*)
    if [ -e "$A" ] && [ ! -r "$A" ]; then
        echo "apport: unreadable"
    else
        echo "apport: ok"
        if [ "$(stat -c %i "$A" 2>/dev/null || echo -)" = "$INODE" ]; then
            tail -c +"$((SIZE + 1))" "$A" 2>/dev/null
        else
            [ "$INODE" = - ] || tail -c +"$((SIZE + 1))" "$A.1" 2>/dev/null
            cat "$A" 2>/dev/null
        fi | grep -aE "executable: [^ ]*/fastrg( |$)" | sed "s/^/apport_new: /"
    fi
    ;;
*)
    echo "apport: unhooked"
    ;;
esac
if journalctl -k -n 1 -q --no-pager 2>/dev/null | grep -q . &&
        KLOG=$(journalctl -k -q --no-pager --since "@$SINCE" 2>/dev/null); then
    echo "kernel: ok"
    printf "%s\n" "$KLOG" | grep -aE "traps: |segfault at |general protection" | sed "s/^/kernel_new: /"
else
    echo "kernel: unreadable"
fi
'

# Where the coming stop's crash evidence starts. Empty when the node could not
# be reached, which the check reports as a collector failure.
e2e_node_stop_mark() {
    ssh_node "$_E2E_NODE_STOP_MARK_SCRIPT" 2>/dev/null | tail -n 1 || true
}

# The evidence a stop left on the node since a mark: the "<source>: ok" lines
# plus log_last / apport_new / kernel_new lines. Wrapped so a drill can alter it.
_e2e_node_shutdown_evidence() {
    local _log="$1" _inode="$2" _size="$3" _since="$4"

    ssh_node "LOG=$(printf '%q' "$_log"); INODE=$(printf '%q' "$_inode");
        SIZE=$(printf '%q' "$_size"); SINCE=$(printf '%q' "$_since");
        ${_E2E_NODE_SHUTDOWN_EVIDENCE_SCRIPT}" 2>/dev/null || true
}

# Verdict on a stop's evidence: pass | collector_failed | no_bye |
# apport_record | kernel_trap.
e2e_node_shutdown_verdict() {
    local _evidence="${1:-}" _source _last

    for _source in log apport kernel; do
        if ! printf '%s\n' "$_evidence" | grep -qx "${_source}: ok"; then
            printf 'collector_failed'
            return 1
        fi
    done
    _last=$(printf '%s\n' "$_evidence" | sed -n 's/^log_last: //p' | tail -n 1)
    if [[ "$_last" != *'bye!' ]]; then
        printf 'no_bye'
        return 1
    fi
    if printf '%s\n' "$_evidence" | grep -q '^apport_new: '; then
        printf 'apport_record'
        return 1
    fi
    if printf '%s\n' "$_evidence" | grep -q '^kernel_new: '; then
        printf 'kernel_trap'
        return 1
    fi
    printf 'pass'
    return 0
}

local_validation_register node_shutdown_verdict e2e_node_shutdown_verdict \
    node_shutdown_clean \
    node_shutdown_abort_without_bye \
    node_shutdown_bye_not_last \
    node_shutdown_apport_record \
    node_shutdown_kernel_trap \
    node_shutdown_log_unreadable \
    node_shutdown_apport_unhooked \
    node_shutdown_kernel_unreadable \
    node_shutdown_no_evidence

# Prints "pass" after a clean stop; otherwise the verdict with the evidence
# behind it, and returns non-zero. Read-only on the node.
e2e_node_shutdown_check() {
    local _mark="${1:-}" _log="" _inode="" _size="" _since="" _evidence="" _verdict=""

    read -r _log _inode _size _since <<< "$_mark" || true
    if [[ -z "$_log" || ! "$_inode" =~ ^([0-9]+|-)$ || ! "$_size" =~ ^[0-9]+$ || \
          ! "$_since" =~ ^[0-9]+$ ]]; then
        printf "collector_failed (no usable stop mark: '%s')" "$_mark"
        return 1
    fi
    _evidence=$(_e2e_node_shutdown_evidence "$_log" "$_inode" "$_size" "$_since")
    if _verdict=$(e2e_node_shutdown_verdict "$_evidence"); then
        printf 'pass'
        return 0
    fi
    case "$_verdict" in
        no_bye)
            printf "no_bye (%s last line: '%s')" "$_log" \
                "$(printf '%s\n' "$_evidence" | sed -n 's/^log_last: //p' | tail -n 1 | cut -c 1-200)" ;;
        apport_record)
            printf "apport_record (%s)" \
                "$(printf '%s\n' "$_evidence" | sed -n 's/^apport_new: //p' | head -n 1 | cut -c 1-200)" ;;
        kernel_trap)
            printf "kernel_trap (%s)" \
                "$(printf '%s\n' "$_evidence" | sed -n 's/^kernel_new: //p' | head -n 1 | cut -c 1-200)" ;;
        *)
            printf "%s (stdout log %s; evidence: '%s')" "${_verdict:-collector_failed}" "$_log" \
                "$(printf '%s' "$_evidence" | grep -E '^(log|apport|kernel): ' | tr '\n' ' ')" ;;
    esac
    return 1
}

# Drill: the stop's stdout log loses its "bye!", as a crash in teardown leaves
# it. The phase's own cleanup restores the real collector via the function below.
_e2e_inject_node_shutdown_no_bye() {
    sabotage_copy_function _e2e_node_shutdown_evidence _e2e_node_shutdown_evidence_real
    sabotage_override_function _e2e_node_shutdown_evidence \
        '_e2e_node_shutdown_evidence_real "$@" | sed "/^log_last: /s/bye!/(no bye)/"'
}

_e2e_restore_node_shutdown_functions() {
    restore_phase_functions node_shutdown_lib.sh
}
