#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 39 — Durable WAL holds runtime errors only (Steps 173-175)
#
# Unconfirmed telemetry is buffered in memory, and only runtime errors are also
# written to /etc/fastrg/kafka_queue.json: they are the one kind of event
# nothing can reconstruct once the process is gone. Every other kind can be
# asked for again, so none of them may reach that file.
#
#   Step 173  a PPPoE republish produces state events and the WAL stays empty
#   Step 174  a rejected over-capacity count writes its runtime error there
#   Step 175  the broker's confirmation empties the file again
#
# Steps 174-175 hold the node's path to the broker down with a REJECT rule
# while the error is produced. With the broker reachable the confirmation
# arrives in milliseconds and the file is back to "[]" long before a read over
# ssh could see anything in it. The rule comes off at the end of the phase and
# from the top-level EXIT trap.
# ---------------------------------------------------------------------------

# The node's WAL, as the producer names it.
_P39_WAL_PATH="/etc/fastrg/kafka_queue.json"
# The error code the over-capacity trigger makes the node report.
_P39_ERR_CODE="COUNT_EXCEEDS_MAX"
# How long the confirmation may take to empty the WAL once the broker is back.
_P39_DRAIN_TIMEOUT=90

# ---------------------------------------------------------------------------
# Data judgement. Both predicates read the WAL file's text and nothing else, so
# the offline validation layer can check them against fixtures.
# ---------------------------------------------------------------------------

# e2e_wal_durable_count WAL_TEXT — how many events the durable WAL holds.
# Text that is only whitespace counts as an empty WAL: the node creates the
# file the first time it has something durable to keep, so no file means
# nothing was kept. Prints nothing and returns 1 for text that is not a WAL, so
# a damaged file can never be read as "zero events, all good".
e2e_wal_durable_count() {
    local _text="${1:-}" _count

    [[ -z "${_text//[[:space:]]/}" ]] && { printf '0'; return 0; }

    _count=$(printf '%s' "$_text" | jq -r '
        if type == "array" then
            (if map(type == "object"
                    and (.seq | type) == "number"
                    and (.seq | floor) == .seq
                    and .seq > 0
                    and (.payload | type) == "string"
                    and (.payload | length) > 0
                    and (.payload | test("^([0-9a-fA-F][0-9a-fA-F])*$"))) | all
             then length else empty end)
        else empty end' 2>/dev/null || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || return 1
    printf '%s' "$_count"
}

# e2e_wal_holds_error_code WAL_TEXT CODE — verdict on whether the WAL carries a
# runtime error with that code:
#   ok       at least one event does
#   absent   the WAL holds events, none of them that code
#   empty    the WAL holds nothing
#   err      the text is not a readable WAL, or no code was given
# A payload is the event's bytes in hex, and a protobuf string field keeps its
# characters verbatim, so the code is looked for as its own hex spelling.
e2e_wal_holds_error_code() {
    local _text="${1:-}" _code="${2:-}" _count _needle="" _index _payloads

    [[ -n "$_code" ]] || { printf 'err'; return 1; }
    _count=$(e2e_wal_durable_count "$_text") || { printf 'err'; return 1; }
    [[ "$_count" == "0" ]] && { printf 'empty'; return 1; }

    for (( _index = 0; _index < ${#_code}; _index++ )); do
        _needle="${_needle}$(printf '%02x' "'${_code:$_index:1}")"
    done

    _payloads=$(printf '%s' "$_text" | jq -r '.[].payload' 2>/dev/null | \
        tr 'A-F' 'a-f' || true)
    if printf '%s\n' "$_payloads" | grep -qF "$_needle"; then
        printf 'ok'
        return 0
    fi
    printf 'absent'
    return 1
}

local_validation_register wal_durable_count e2e_wal_durable_count \
    wal_durable_count_good wal_durable_count_empty_array \
    wal_durable_count_absent_file wal_durable_count_not_an_array \
    wal_durable_count_odd_payload wal_durable_count_seq_missing \
    wal_durable_count_garbage
local_validation_register wal_holds_error_code e2e_wal_holds_error_code \
    wal_holds_error_code_good wal_holds_error_code_uppercase_payload \
    wal_holds_error_code_other_code wal_holds_error_code_empty_wal \
    wal_holds_error_code_corrupt wal_holds_error_code_no_code

# ---------------------------------------------------------------------------
# Bench access
# ---------------------------------------------------------------------------

# The WAL file's text. A read that could not happen prints nothing and returns
# non-zero, so an unreachable node is never read as an empty WAL.
_p39_read_wal() {
    local _out

    _out=$(ssh_node "cat ${_P39_WAL_PATH} 2>/dev/null; printf '%s' __P39_READ_OK__") || return 1
    [[ "$_out" == *__P39_READ_OK__ ]] || return 1
    printf '%s' "${_out%__P39_READ_OK__}"
}

# Wait until the WAL holds the number of durable events asked for. Prints what
# it was still looking at, so the caller can say what it settled on:
# a count, "corrupt" or "unreadable".
_p39_wait_wal_count() {
    local _want="$1" _timeout="$2" _started=$SECONDS _wal="" _seen=""

    while :; do
        if _wal=$(_p39_read_wal); then
            if _seen=$(e2e_wal_durable_count "$_wal"); then
                if [[ "$_seen" == "$_want" ]]; then
                    printf '%s' "$_seen"
                    return 0
                fi
            else
                _seen="corrupt"
            fi
        else
            _seen="unreadable"
        fi
        if (( SECONDS - _started >= _timeout )); then
            break
        fi
        sleep 2
    done
    printf '%s' "${_seen:-unreadable}"
    return 1
}

# The node's broker endpoint, read from its own config so the block below lands
# on the broker the producer actually talks to. Prints "host port".
_p39_kafka_endpoint() {
    local _brokers

    _brokers=$(ssh_node "grep 'KafkaBrokers' /etc/fastrg/config.cfg 2>/dev/null" 2>/dev/null | \
        awk -F'"' '{print $2}' || true)
    _brokers="${_brokers%%,*}"
    [[ "$_brokers" == *:* ]] || return 1
    printf '%s %s' "${_brokers%%:*}" "${_brokers##*:}"
}

# Cut the node's path to the broker, so a produced event stays unconfirmed and
# with it in the WAL long enough to be read. Idempotent, and without a window
# in which the rule is momentarily off: an existing rule is left alone.
_p39_block_kafka() {
    ssh_node "iptables -C OUTPUT -p tcp -d ${_P39_KAFKA_HOST} --dport ${_P39_KAFKA_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null \
        || iptables -I OUTPUT 1 -p tcp -d ${_P39_KAFKA_HOST} --dport ${_P39_KAFKA_PORT} -j REJECT --reject-with tcp-reset" \
        >/dev/null 2>&1
}

# Idempotent: -D on a rule that is already gone is a no-op.
_p39_unblock_kafka() {
    [[ -n "${_P39_KAFKA_HOST:-}" && -n "${_P39_KAFKA_PORT:-}" ]] || return 0
    ssh_node "iptables -D OUTPUT -p tcp -d ${_P39_KAFKA_HOST} --dport ${_P39_KAFKA_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null || true" \
        >/dev/null 2>&1 || true
}

# Ask for one subscriber more than the node's computed capacity. The node
# refuses it and reports COUNT_EXCEEDS_MAX — a runtime error, and so the one
# event kind that has to reach the disk.
_p39_force_over_capacity() {
    local _max

    _max=$(e2e_metric_value "$(e2e_metrics_body)" fastrg_node_max_user_count)
    [[ "$_max" =~ ^[0-9]+$ && "$_max" -ge 2 ]] || return 1
    fastrg_grpc set_subscriber_count "$(( _max + 1 ))" >/dev/null 2>&1 || true
}

# Wait until the node and etcd are both back on the given subscriber count.
# Prints what each side held when it gave up.
_p39_wait_count_restored() {
    local _want="$1" _timeout="$2" _started=$SECONDS _local="" _etcd=""

    while :; do
        _local=$(fastrg_grpc get_system_info | jq -r '.num_users // empty' \
            2>/dev/null || true)
        _etcd=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null | \
            jq -r '.subscriber_count // empty' 2>/dev/null || true)
        if [[ "$_local" == "$_want" && "$_etcd" == "$_want" ]]; then
            return 0
        fi
        if (( SECONDS - _started >= _timeout )); then
            break
        fi
        sleep 2
    done
    printf 'local=%s etcd=%s' "${_local:-empty}" "${_etcd:-empty}"
    return 1
}

_p39_restore_count() {
    fastrg_grpc set_subscriber_count "$1" >/dev/null 2>&1 || true
    _p39_wait_count_restored "$1" "$2"
}

# The node's answer to a PPPoE republish request. A separate function so a
# drill can make the republish window look different without touching the gRPC
# helper every other step uses.
_p39_republish_state_events() {
    fastrg_grpc republish_pppoe_status
}

# Idempotent and unconditional: called at the end of the phase AND from the
# cleanup_fastrg EXIT trap, so a crash mid-phase cannot leave the node's broker
# path blocked or its subscriber count off the canonical value.
_cleanup_phase39_wal_durability() {
    _p39_unblock_kafka
    if [[ -n "${_P39_ORIG_SC:-}" && -n "${NODE_UUID:-}" ]]; then
        info "Cleanup(phase39): restoring subscriber count to ${_P39_ORIG_SC}..."
        _p39_restore_count "${_P39_ORIG_SC}" 90 >/dev/null 2>&1 || true
    fi
}

# ---------------------------------------------------------------------------
# Drills
# ---------------------------------------------------------------------------

# Make something durable land in the WAL during the republish window, which is
# what a state event turned durable would look like from outside. Step 173 has
# to notice the file is no longer empty.
_p39_inject_state_event_looks_durable() {
    sabotage_copy_function _p39_republish_state_events _p39_real_republish_state_events
    sabotage_override_function _p39_republish_state_events '
        _p39_block_kafka || true
        _p39_force_over_capacity || true
        sleep 10
        _p39_real_republish_state_events
    '
}

_p39_cleanup_state_event_looks_durable() {
    restore_phase_functions phase39_wal_durability.sh
    _cleanup_phase39_wal_durability
}

# Leave the broker path down when the phase believes it restored it, so nothing
# ever confirms the buffered error. Step 175 has to notice the WAL never empties.
_p39_inject_ack_never_arrives() {
    sabotage_override_function _p39_unblock_kafka 'return 0'
}

_p39_cleanup_ack_never_arrives() {
    restore_phase_functions phase39_wal_durability.sh
    _cleanup_phase39_wal_durability
}

case_validation_register wal_state_event_not_persisted phase39_wal_durability \
    _p39_inject_state_event_looks_durable _p39_cleanup_state_event_looks_durable \
    'Step 173:'
case_validation_register wal_durable_ack_drains phase39_wal_durability \
    _p39_inject_ack_never_arrives _p39_cleanup_ack_never_arrives \
    'Step 175:'

phase39_wal_durability() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 39 — Durable WAL Holds Runtime Errors Only (Steps 173-175)"
    bold "═══════════════════════════════════════════════════════"

    local _issue173="" _issue174="" _issue175=""
    local _endpoint="" _reply="" _events="" _wal="" _count="" _verdict=""
    local _baseline="" _drained="" _restored="" _i

    if ! ssh_node "command -v iptables >/dev/null 2>&1"; then
        skip "Step 173: state events stay out of the durable WAL" \
            "iptables not available on node"
        skip "Step 174: a runtime error is written to the durable WAL" \
            "iptables not available on node"
        skip "Step 175: a confirmed runtime error leaves the durable WAL" \
            "iptables not available on node"
        return 0
    fi

    _endpoint=$(_p39_kafka_endpoint || true)
    if [[ -z "$_endpoint" ]]; then
        _issue173="cannot read KafkaBrokers from the node config, so nothing would confirm an event"
        fail "Step 173: state events stay out of the durable WAL" "$_issue173"
        fail "Step 174: a runtime error is written to the durable WAL" \
            "precondition failed: $_issue173"
        fail "Step 175: a confirmed runtime error leaves the durable WAL" \
            "precondition failed: $_issue173"
        return 0
    fi
    _P39_KAFKA_HOST="${_endpoint%% *}"
    _P39_KAFKA_PORT="${_endpoint##* }"

    # What to put back once the over-capacity trigger has done its work. The
    # canonical fixture runs 2 subscribers, which is the fallback when the node
    # cannot be asked.
    _P39_ORIG_SC=$(fastrg_grpc get_system_info | jq -r '.num_users // empty' \
        2>/dev/null || true)
    [[ "${_P39_ORIG_SC}" =~ ^[0-9]+$ ]] || _P39_ORIG_SC=2

    # ---- Step 173: a burst of state events leaves the WAL untouched -------
    # The probe says nothing unless the file was empty to begin with. Earlier
    # phases produce runtime errors of their own; with the broker reachable
    # those are confirmed and dropped within seconds.
    info "Step 173: waiting for the durable WAL to settle empty..."
    _baseline=$(_p39_wait_wal_count 0 60 || true)
    if [[ "$_baseline" != "0" ]]; then
        _issue173="WAL did not settle empty before the probe (last read: ${_baseline:-unreadable})"
    fi

    info "Step 173: asking the node to republish every subscriber's PPPoE state..."
    _reply=$(_p39_republish_state_events 2>&1 || true)
    _events=$(e2e_republish_event_count "$_reply" || true)
    # Positive control for a check that passes on seeing nothing: state events
    # have to have been produced at all, or an empty WAL means nothing. That the
    # reader can see content when there is some is Step 174's job, with the very
    # same reader.
    if [[ ! "$_events" =~ ^[1-9][0-9]*$ ]]; then
        _issue173="${_issue173:+${_issue173}; }republish produced no state events (event_count='${_events:-<none>}'; reply: $(printf '%s' "$_reply" | head -c 200))"
    fi

    # Once right away and once after the reports have had time to be produced.
    for _i in 1 2; do
        if _wal=$(_p39_read_wal); then
            if _count=$(e2e_wal_durable_count "$_wal"); then
                if [[ "$_count" != "0" ]]; then
                    _issue173="${_issue173:+${_issue173}; }WAL holds ${_count} event(s) after the republish"
                fi
            else
                _issue173="${_issue173:+${_issue173}; }WAL is not readable as a WAL: $(printf '%s' "$_wal" | head -c 120)"
            fi
        else
            _issue173="${_issue173:+${_issue173}; }could not read ${_P39_WAL_PATH} from the node"
        fi
        if [[ "$_i" == "1" ]]; then
            sleep 5
        fi
    done

    if [[ -z "$_issue173" ]]; then
        pass "Step 173: state events stay out of the durable WAL" \
            "republish produced ${_events} state event(s); ${_P39_WAL_PATH} still holds 0 durable event(s)"
    else
        fail "Step 173: state events stay out of the durable WAL" "$_issue173"
    fi

    # ---- Step 174: a runtime error is written to the WAL ------------------
    info "Step 174: blocking node->broker (${_P39_KAFKA_HOST}:${_P39_KAFKA_PORT}) so a produced error stays unconfirmed..."
    if ! _p39_block_kafka; then
        _issue174="failed to install the node->broker iptables block"
    fi
    if ! _p39_force_over_capacity; then
        _issue174="${_issue174:+${_issue174}; }cannot read a valid node capacity from fastrg_node_max_user_count"
    fi

    _verdict=""
    for (( _i = 1; _i <= 30; _i++ )); do
        if _wal=$(_p39_read_wal); then
            _verdict=$(e2e_wal_holds_error_code "$_wal" "$_P39_ERR_CODE" || true)
        else
            _verdict="unreadable"
            _wal=""
        fi
        if [[ "$_verdict" == "ok" ]]; then
            break
        fi
        sleep 2
    done
    if [[ "$_verdict" != "ok" ]]; then
        _issue174="${_issue174:+${_issue174}; }WAL never carried a ${_P39_ERR_CODE} event (verdict=${_verdict:-<none>}; file: $(printf '%s' "$_wal" | head -c 120))"
    fi
    _count=$(e2e_wal_durable_count "$_wal" || true)

    if [[ -z "$_issue174" ]]; then
        pass "Step 174: a runtime error is written to the durable WAL" \
            "${_P39_WAL_PATH} holds ${_count} durable event(s), one of them ${_P39_ERR_CODE}, with the broker path down"
    else
        fail "Step 174: a runtime error is written to the durable WAL" "$_issue174"
    fi

    # ---- Step 175: the confirmation empties the WAL again -----------------
    # The canonical count goes back first: leaving the over-capacity value in
    # etcd has the node reject it again on every reconcile, refilling the WAL
    # under the very wait below.
    info "Step 175: restoring subscriber count to ${_P39_ORIG_SC} before the drain window..."
    _restored=$(_p39_restore_count "${_P39_ORIG_SC}" 90 || true)
    if [[ -n "$_restored" ]]; then
        _issue175="subscriber count did not return to ${_P39_ORIG_SC} (${_restored})"
    fi

    info "Step 175: restoring the node->broker path and waiting for the WAL to empty..."
    _p39_unblock_kafka
    _drained=$(_p39_wait_wal_count 0 "$_P39_DRAIN_TIMEOUT" || true)
    if [[ "$_drained" != "0" ]]; then
        _issue175="${_issue175:+${_issue175}; }WAL still at '${_drained:-unreadable}' after ${_P39_DRAIN_TIMEOUT}s with the broker reachable"
    fi
    # Confirming a durable event takes the WAL's file lock and its buffer lock
    # in turn; a node that stops answering right afterwards is what this step
    # is here to catch.
    if ! fastrg_grpc get_system_info | jq -e '.num_users' >/dev/null 2>&1; then
        _issue175="${_issue175:+${_issue175}; }node stopped answering after the confirmation"
    fi

    if [[ -z "$_issue175" ]]; then
        pass "Step 175: a confirmed runtime error leaves the durable WAL" \
            "${_P39_WAL_PATH} back to 0 durable event(s) within ${_P39_DRAIN_TIMEOUT}s; node still answering; subscriber count ${_P39_ORIG_SC}"
    else
        fail "Step 175: a confirmed runtime error leaves the durable WAL" "$_issue175"
    fi

    _cleanup_phase39_wal_durability
    return 0
}
