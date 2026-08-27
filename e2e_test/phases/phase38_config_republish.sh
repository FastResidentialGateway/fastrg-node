#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 38 — Config status republish (Steps 171-172)
#
# The node no longer keeps config-apply events on disk, so the controller's way
# back to a lost one is to ask for it again. The node answers by queueing each
# configured subscriber for a re-check on its control-plane loop, which is what
# the reply counts; the reports follow shortly after. This phase exercises that
# request against a fixture whose configs already match etcd:
#   Step 171  the node queues one re-check per configured subscriber
#   Step 172  asking again is stable — same count, configs still untouched
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Data judgement. The reply carries the same event_count field the PPPoE
# republish uses, so the count is read with that same predicate rather than a
# second copy of the parsing rule. What is new here is the comparison against
# the number of configured subscribers, which lives in its own predicate below.
# ---------------------------------------------------------------------------

# e2e_config_republish_count_ok COUNT EXPECTED — verdict on the number of
# subscribers a config republish queued:
#   ok      one per configured subscriber
#   short   fewer than expected, so some subscriber was skipped
#   over    more than expected, which means it queued something twice
#   err     either side missing or not a number, so there is nothing to compare
e2e_config_republish_count_ok() {
    local _count="${1:-}" _expected="${2:-}"

    [[ "$_count" =~ ^[0-9]+$ ]] || { printf 'err'; return 1; }
    [[ "$_expected" =~ ^[0-9]+$ ]] || { printf 'err'; return 1; }

    if [[ "$_count" -lt "$_expected" ]]; then
        printf 'short'
        return 1
    fi
    if [[ "$_count" -gt "$_expected" ]]; then
        printf 'over'
        return 1
    fi
    printf 'ok'
}

local_validation_register config_republish_count_ok e2e_config_republish_count_ok \
    config_republish_count_ok_good config_republish_count_ok_short \
    config_republish_count_ok_over config_republish_count_ok_not_a_number \
    config_republish_count_ok_empty_input

# The node's answer to a config republish request. A separate function so a
# drill can hand the steps a wrong answer without touching the gRPC helper
# every other step uses.
_p38_config_republish_reply() {
    fastrg_grpc republish_config_status
}

# Drill: answer the request without ever asking the node, so nothing is queued.
# Both steps have to notice the count that does not match the configured
# subscribers.
_p38_inject_config_republish_not_produced() {
    sabotage_override_function _p38_config_republish_reply \
        'printf "%s" "{\"event_count\": 0}"'
}

_p38_cleanup_config_republish_not_produced() {
    restore_phase_functions phase38_config_republish.sh
}

case_validation_register config_republish_not_produced phase38_config_republish \
    _p38_inject_config_republish_not_produced _p38_cleanup_config_republish_not_produced \
    'Step 171:'

phase38_config_republish() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 38 — Config status republish (Steps 171-172)"
    bold "═══════════════════════════════════════════════════════"

    local _expected _reply _count _verdict

    # One event is expected per subscriber under test; that is what this
    # fixture has configured in etcd.
    _expected=${#SUB_IDS[@]}

    # ---- Step 171: the node reports every configured subscriber ----------
    info "Step 171: asking the node to re-check every subscriber's config state..."
    _reply=$(_p38_config_republish_reply 2>&1 || true)
    _count=$(e2e_republish_event_count "$_reply" || true)
    _verdict=$(e2e_config_republish_count_ok "${_count:-}" "$_expected" || true)

    if [[ "$_verdict" == "ok" ]]; then
        pass "Step 171: config status republish" \
            "queued event_count=${_count} for ${_expected} configured subscriber(s)"
    else
        fail "Step 171: config status republish" \
            "verdict=${_verdict:-err}, event_count='${_count:-<none>}', expected ${_expected}; reply: $(printf '%s' "$_reply" | head -c 300)"
    fi

    # ---- Step 172: asking again is stable --------------------------------
    # A confirmation must not change anything, so a second request has to
    # return the same count. A drifting count would mean the request itself
    # mutates config state.
    local _reply2 _count2 _verdict2
    _reply2=$(_p38_config_republish_reply 2>&1 || true)
    _count2=$(e2e_republish_event_count "$_reply2" || true)
    _verdict2=$(e2e_config_republish_count_ok "${_count2:-}" "$_expected" || true)

    if [[ "$_verdict2" == "ok" ]]; then
        pass "Step 172: a repeated config republish is stable" \
            "event_count=${_count2} again for ${_expected} subscriber(s)"
    else
        fail "Step 172: a repeated config republish is stable" \
            "verdict=${_verdict2:-err}, event_count='${_count2:-<none>}', expected ${_expected}; reply: $(printf '%s' "$_reply2" | head -c 300)"
    fi
}
