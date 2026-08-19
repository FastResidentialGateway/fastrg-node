#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Case correctness validation: break the system on purpose and require the
# phase's own assertions to notice. Runs on the bench under --validate-case and
# in the default mode. Local correctness validation (the offline predicate
# checks) lives in local_validation_lib.sh.
# ---------------------------------------------------------------------------

_CASE_VALIDATION_IDS=()
_CASE_VALIDATION_PHASES=()
_CASE_VALIDATION_INJECTIONS=()
_CASE_VALIDATION_CLEANUPS=()
_CASE_VALIDATION_EXPECTED=()

# case_validation_register ID PHASE_FUNCTION INJECT_FUNCTION CLEANUP_FUNCTION EXPECTED_FAIL_REGEX
case_validation_register() {
    _CASE_VALIDATION_IDS+=("$1")
    _CASE_VALIDATION_PHASES+=("$2")
    _CASE_VALIDATION_INJECTIONS+=("$3")
    _CASE_VALIDATION_CLEANUPS+=("$4")
    _CASE_VALIDATION_EXPECTED+=("$5")
}

# Step names recorded since a mark, restricted to one result value.
get_case_validation_steps_since() {
    local _mark="$1" _want="$2" _index

    for (( _index = _mark; _index < ${#STEP_RESULTS[@]}; _index++ )); do
        [[ "${STEP_RESULTS[$_index]}" == "$_want" ]] && printf '%s\n' "${STEP_NAMES[$_index]}"
    done
    return 0
}

# Drop the draft results a drill phase run produced; the normal tally is
# never built in this mode.
_case_validation_discard_results_since() {
    local _mark="$1"

    STEP_NAMES=("${STEP_NAMES[@]:0:$_mark}")
    STEP_RESULTS=("${STEP_RESULTS[@]:0:$_mark}")
    STEP_DETAILS=("${STEP_DETAILS[@]:0:$_mark}")
}

# Run one entry: inject, run the phase, see whether the expected step went red.
_case_validation_run_one() {
    local _index="$1"
    local _id="${_CASE_VALIDATION_IDS[$_index]}"
    local _phase="${_CASE_VALIDATION_PHASES[$_index]}"
    local _inject="${_CASE_VALIDATION_INJECTIONS[$_index]}"
    local _cleanup="${_CASE_VALIDATION_CLEANUPS[$_index]}"
    local _expected="${_CASE_VALIDATION_EXPECTED[$_index]}"
    local _mark="${#STEP_RESULTS[@]}" _failed_steps="" _caught=1

    printf '\n--- case correctness validation %s: injecting ---\n' "$_id"
    # The cleanup has to run even if the injection or the phase dies partway.
    trap "$_cleanup >/dev/null 2>&1 || true" RETURN
    if ! "$_inject"; then
        printf 'case correctness validation %s: injection failed\n' "$_id"
        _case_validation_discard_results_since "$_mark"
        return 1
    fi
    "$_phase" || true
    _failed_steps=$(get_case_validation_steps_since "$_mark" FAIL)
    if printf '%s\n' "$_failed_steps" | grep -qE "$_expected"; then
        _caught=0
        printf 'case correctness validation %s: CAUGHT\n' "$_id"
    else
        printf 'case correctness validation %s: MISS (expected /%s/ to fail; failed: %s)\n' \
            "$_id" "$_expected" "$(printf '%s' "$_failed_steps" | tr '\n' ',')"
    fi
    _case_validation_discard_results_since "$_mark"
    return $_caught
}

# A phase must be clean again once its entries are done, or the sabotage leaked.
_case_validation_phase_is_clean_again() {
    local _phase="$1" _mark="${#STEP_RESULTS[@]}" _failed_steps=""

    printf '\n--- case correctness validation: clean rerun of %s ---\n' "$_phase"
    "$_phase" || true
    _failed_steps=$(get_case_validation_steps_since "$_mark" FAIL)
    _case_validation_discard_results_since "$_mark"
    [[ -z "$_failed_steps" ]] && return 0
    printf 'case correctness validation: %s still failing after cleanup: %s\n' \
        "$_phase" "$(printf '%s' "$_failed_steps" | tr '\n' ',')"
    return 1
}

# case_validation_run [ID,...] — every entry when no list is given.
case_validation_run() {
    local _wanted="${1:-}" _index _phase
    local _total=0 _caught=0 _polluted=0
    local _selected=() _phases_done=" "

    for _index in "${!_CASE_VALIDATION_IDS[@]}"; do
        if [[ -n "$_wanted" ]]; then
            printf '%s' ",${_wanted}," | grep -q ",${_CASE_VALIDATION_IDS[$_index]}," || continue
        fi
        _selected+=("$_index")
    done
    if [[ ${#_selected[@]} -eq 0 ]]; then
        printf 'VALIDATE RESULT: no entries selected\n'
        return 1
    fi

    for _index in "${_selected[@]}"; do
        _total=$(( _total + 1 ))
        _case_validation_run_one "$_index" && _caught=$(( _caught + 1 ))
    done

    for _index in "${_selected[@]}"; do
        _phase="${_CASE_VALIDATION_PHASES[$_index]}"
        [[ "$_phases_done" == *" $_phase "* ]] && continue
        _phases_done="${_phases_done}${_phase} "
        _case_validation_phase_is_clean_again "$_phase" || _polluted=$(( _polluted + 1 ))
    done

    printf '\nVALIDATE RESULT: %d/%d caught, %d polluted\n' "$_caught" "$_total" "$_polluted"
    [[ "$_caught" -eq "$_total" && "$_polluted" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# Shared sabotage helpers.
#
# An injection runs before the phase does, while the collector a capture phase
# uses is started inside it. So every shape below works by replacing the phase's
# own capture-start helper for the duration of one drill; the cleanup re-sources
# the phase file to put the real one back. Production files carry no injection
# hooks of their own.
# ---------------------------------------------------------------------------

# Keep a copy of a function under a second name, so an override can still reach
# the real one.
sabotage_copy_function() {
    local _from="$1" _to="$2"

    eval "${_to}() $(declare -f "$_from" | tail -n +2)"
}

# Replace a shell function for the duration of one drill. The body goes on its
# own line so a closing brace never lands on the last command of it.
sabotage_override_function() {
    local _name="$1" _body="$2"

    eval "${_name}() {
        ${_body}
    }"
}

# Put a phase's real functions back by re-reading the file that defines them.
restore_phase_functions() {
    # shellcheck source=/dev/null
    source "${_E2E_PHASES_DIR}/$1"
}
