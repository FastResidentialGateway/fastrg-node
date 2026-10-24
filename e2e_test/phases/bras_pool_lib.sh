#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Fails by name when dpdk-bras logs "IP pool exhausted"; no phase lives here.
# ---------------------------------------------------------------------------

# Exhaustion lines the previous check counted in the current BRAS log.
: "${_E2E_BRAS_POOL_SEEN:=0}"

# e2e_bras_pool_verdict EVIDENCE SEEN: ok N | exhausted N FIRST_NEW | unreadable
# A count below SEEN means a restarted BRAS, so all its lines are new.
e2e_bras_pool_verdict() {
    local _evidence="${1:-}" _seen="${2:-}" _lines _n _first

    if ! printf '%s\n' "$_evidence" | grep -qx 'read: ok' || ! e2e_is_uint "$_seen"; then
        printf 'unreadable'
        return 1
    fi
    _lines=$(printf '%s\n' "$_evidence" | grep -F 'IP pool exhausted')
    _n=$(printf '%s' "$_lines" | grep -c '' || true)
    if [[ "$_n" -eq 0 || "$_n" -eq "$_seen" ]]; then
        printf 'ok %s' "$_n"
        return 0
    fi
    if [[ "$_n" -gt "$_seen" ]]; then
        _first=$(printf '%s\n' "$_lines" | sed -n "$(( _seen + 1 ))p")
    else
        _first=$(printf '%s\n' "$_lines" | head -n 1)
    fi
    printf 'exhausted %s %s' "$_n" "$_first"
    return 1
}

local_validation_register bras_pool_verdict e2e_bras_pool_verdict \
    bras_pool_clean \
    bras_pool_first_exhaustion \
    bras_pool_already_reported \
    bras_pool_more_since_last \
    bras_pool_log_restarted_clean \
    bras_pool_log_restarted_exhausted \
    bras_pool_other_lines_ignored \
    bras_pool_unreadable \
    bras_pool_bad_seen \
    bras_pool_empty_input

# Exhaustion lines of the BRAS log, then "read: ok" (grep's 1 = no match).
_e2e_bras_pool_log() {
    ssh_bras "grep -a -F 'IP pool exhausted' /var/log/dpdk-bras.log; [ \$? -le 1 ] && echo 'read: ok'" 2>/dev/null
}

# e2e_bras_pool_guard [WHERE] — FAIL step on new exhaustion lines or no log.
e2e_bras_pool_guard() {
    local _where="${1:-standalone check}" _verdict _rest

    _verdict=$(e2e_bras_pool_verdict "$(_e2e_bras_pool_log)" "$_E2E_BRAS_POOL_SEEN") && {
        _E2E_BRAS_POOL_SEEN="${_verdict#ok }"
        return 0
    }
    if [[ "$_verdict" == exhausted\ * ]]; then
        _rest="${_verdict#exhausted }"
        _E2E_BRAS_POOL_SEEN="${_rest%% *}"
        fail "BRAS address pool (${_where})" \
            "dpdk-bras ran out of client addresses: ${_rest#* }; every PPPoE dial it turns away fails, so failures from here on say nothing about the node"
    else
        fail "BRAS address pool (${_where})" \
            "could not read /var/log/dpdk-bras.log on ${BRAS_HOST}"
    fi
    return 1
}

# Count the current lines without reporting them.
_e2e_bras_pool_rebaseline() {
    local _verdict _rest

    _verdict=$(e2e_bras_pool_verdict "$(_e2e_bras_pool_log)" 0)
    case "$_verdict" in
        ok\ *) _E2E_BRAS_POOL_SEEN="${_verdict#ok }" ;;
        exhausted\ *) _rest="${_verdict#exhausted }"; _E2E_BRAS_POOL_SEEN="${_rest%% *}" ;;
    esac
    return 0
}

# Drill: one injected exhaustion line must turn the check red.
_e2e_bras_pool_inject_exhausted() {
    sabotage_copy_function _e2e_bras_pool_log _e2e_bras_pool_log_real
    sabotage_override_function _e2e_bras_pool_log \
        "_e2e_bras_pool_log_real | sed '\$i [drill] PPP: [0] IP pool exhausted — terminating session'"
}

_e2e_bras_pool_cleanup_exhausted() {
    sabotage_copy_function _e2e_bras_pool_log_real _e2e_bras_pool_log
    _e2e_bras_pool_rebaseline
}

case_validation_register bras_pool_exhaustion_unreported e2e_bras_pool_guard \
    _e2e_bras_pool_inject_exhausted _e2e_bras_pool_cleanup_exhausted \
    '^BRAS address pool'
