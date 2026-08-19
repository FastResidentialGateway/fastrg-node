#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Local correctness validation: offline checks that the suite's own assertion
# logic still rejects the data shapes it is supposed to reject. Runs before
# every mode and on its own via --local-validate; it never touches the bench.
#
# A predicate registered here is the very function the phases call. Fixtures are
# data only — each one carries the input plus the answer the predicate owes it,
# so a fixture can never contain a second copy of the parsing logic.
#
# Fixtures live in local_validation_fixtures.txt, one block per case introduced
# by "==== fixture: <name>". Within a block:
#   # invoke: body-first | args-only     how the predicate is called
#   # args: ...                          arguments after the body
#   # expect-output: ...                 required stdout (may be empty)
#   # expect-status: 0|1                  required exit status
#   ---
#   <body: everything below the separator>
# ---------------------------------------------------------------------------

_LOCAL_VALIDATION_IDS=()
_LOCAL_VALIDATION_FUNCTIONS=()
_LOCAL_VALIDATION_FIXTURES=()

# One case's block out of the fixture file: everything between its marker line
# and the next one. Prints nothing when the name is not in the file.
get_local_validation_fixture_block() {
    awk -v want="==== fixture: $1" '
        $0 == want { inblock = 1; next }
        inblock && index($0, "==== fixture: ") == 1 { exit }
        inblock { print }
    ' "${_E2E_PHASES_DIR}/local_validation_fixtures.txt" 2>/dev/null
}

# local_validation_register ID PREDICATE GOOD_FIXTURE BAD_FIXTURE...
local_validation_register() {
    local _id="$1" _function="$2"

    shift 2
    _LOCAL_VALIDATION_IDS+=("$_id")
    _LOCAL_VALIDATION_FUNCTIONS+=("$_function")
    _LOCAL_VALIDATION_FIXTURES+=("$*")
}

# One header field's value; empty when the field is absent.
get_fixture_field() {
    printf '%s\n' "$1" | sed -n '1,/^---$/p' | sed -n "s/^# $2: *//p" | head -1
}

# Whether a header field was declared at all, which is what tells an expected
# empty string apart from an undeclared expectation.
_local_validation_field_present() {
    printf '%s\n' "$1" | sed -n '1,/^---$/p' | grep -qE "^# $2:"
}

# Run one predicate against one fixture. Prints the mismatch when it fails.
_local_validation_check_fixture() {
    local _function="$1" _fixture="$2"
    local _block _body _args _invoke _output _status

    _block=$(get_local_validation_fixture_block "$_fixture")
    if [[ -z "$_block" ]]; then
        printf 'fixture not found: %s' "$_fixture"
        return 1
    fi

    _invoke=$(get_fixture_field "$_block" "invoke")
    _args=$(get_fixture_field "$_block" "args")
    _body=$(printf '%s\n' "$_block" | sed -n '/^---$/,$p' | tail -n +2)

    # shellcheck disable=SC2086
    if [[ "$_invoke" == "args-only" ]]; then
        _output=$("$_function" $_args 2>/dev/null)
        _status=$?
    else
        _output=$("$_function" "$_body" $_args 2>/dev/null)
        _status=$?
    fi

    if _local_validation_field_present "$_block" "expect-status"; then
        local _want_status
        _want_status=$(get_fixture_field "$_block" "expect-status")
        if [[ "$_status" != "$_want_status" ]]; then
            printf 'status %s, want %s' "$_status" "$_want_status"
            return 1
        fi
    fi
    if _local_validation_field_present "$_block" "expect-output"; then
        local _want_output
        _want_output=$(get_fixture_field "$_block" "expect-output")
        if [[ "$_output" != "$_want_output" ]]; then
            printf "output '%s', want '%s'" "$_output" "$_want_output"
            return 1
        fi
    fi
    return 0
}

# Run every registered predicate against every fixture. Prints the tally and
# returns non-zero on the first sign the assertion logic has gone blind.
local_validation_run() {
    local _index _id _function _fixture _detail
    local _checked=0 _failed=0

    for _index in "${!_LOCAL_VALIDATION_IDS[@]}"; do
        _id="${_LOCAL_VALIDATION_IDS[$_index]}"
        _function="${_LOCAL_VALIDATION_FUNCTIONS[$_index]}"
        if ! declare -F "$_function" >/dev/null; then
            printf '  LOCAL CORRECTNESS VALIDATION: %s -> predicate %s is not defined\n' "$_id" "$_function"
            _failed=$(( _failed + 1 ))
            continue
        fi
        for _fixture in ${_LOCAL_VALIDATION_FIXTURES[$_index]}; do
            _checked=$(( _checked + 1 ))
            if ! _detail=$(_local_validation_check_fixture "$_function" "$_fixture"); then
                printf '  LOCAL CORRECTNESS VALIDATION: %s / %s -> %s\n' "$_id" "$_fixture" "$_detail"
                _failed=$(( _failed + 1 ))
            fi
        done
    done

    if [[ "$_failed" -gt 0 ]]; then
        printf 'LOCAL CORRECTNESS VALIDATION: %d/%d predicates OK (%d failed)\n' \
            "$(( _checked - _failed ))" "$_checked" "$_failed"
        return 1
    fi
    printf 'LOCAL CORRECTNESS VALIDATION: %d/%d predicates OK\n' "$_checked" "$_checked"
    return 0
}
