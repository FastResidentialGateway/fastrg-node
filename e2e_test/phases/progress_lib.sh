#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Progress helpers: duration formatting and the arithmetic behind the ETA the
# phase loop prints.
#
# This file defines no phase function. It lives in phases/ so the existing
# "scp phases/*.sh" upload in run_e2e_test.sh carries it to the runner, and so
# the offline --local-validate mode (which sources phases/*.sh only) can check
# the two calculations below against their fixtures.
#
# The phase table, the timing of each phase and the history file are driven
# from run_e2e_test.sh; only the pure calculations are here.
# ---------------------------------------------------------------------------

# e2e_fmt_hms SECONDS — "HH:MM:SS".
e2e_fmt_hms() {
    local _s="${1:-0}"

    printf '%02d:%02d:%02d' "$(( _s / 3600 ))" "$(( (_s / 60) % 60 ))" "$(( _s % 60 ))"
}

# e2e_fmt_ms SECONDS — "MM:SS". Minutes are not wrapped at 60, so an hour-long
# phase reads 61:30 rather than looking like a minute.
e2e_fmt_ms() {
    local _s="${1:-0}"

    printf '%02d:%02d' "$(( _s / 60 ))" "$(( _s % 60 ))"
}

# e2e_median_of_lines LINES — median of the integers in LINES, one per line.
# Lines that are not plain integers are ignored; with none left it prints
# nothing and fails. An even count takes the lower of the two middle values.
# Sorting is numeric, so 9/10/11 gives 10 and not the 11 a text sort produces.
e2e_median_of_lines() {
    printf '%s\n' "${1:-}" | awk '
        $0 ~ /^[0-9]+$/ { v[n++] = $0 + 0 }
        END {
            if (n == 0)
                exit 1
            for (i = 1; i < n; i++) {
                x = v[i]
                for (j = i - 1; j >= 0 && v[j] > x; j--)
                    v[j + 1] = v[j]
                v[j + 1] = x
            }
            print v[int((n - 1) / 2)]
        }
    '
}

# e2e_eta_seconds FALLBACK CSV — seconds still to come, from a comma-separated
# list of per-phase estimates, plus how many of them were unknown. An entry
# that is empty or not a number counts as unknown and contributes FALLBACK
# (0 when FALLBACK is itself not a number). Prints "<seconds> <unknown>".
e2e_eta_seconds() {
    awk -v fb="${1:-}" -v csv="${2:-}" '
        BEGIN {
            if (fb !~ /^[0-9]+$/)
                fb = 0
            n = split(csv, item, ",")
            for (i = 1; i <= n; i++) {
                if (item[i] ~ /^[0-9]+$/) {
                    total += item[i]
                } else {
                    total += fb
                    unknown++
                }
            }
            print total + 0 " " unknown + 0
        }
    '
}

# e2e_phase_step_ok EXPECTED ACTUAL — true only when the phase loop is about to
# run the phase right after the previous one.
#
# The loop counter is an ordinary shell variable, and a phase that uses the same
# name as a global overwrites it; the suite then silently re-runs phases it has
# already done. That shows up as a step failing for a reason that has nothing to
# do with the step — a phase whose setup ran long ago no longer holds. So the
# position is checked rather than trusted.
e2e_phase_step_ok() {
    [[ "${1:-}" =~ ^[0-9]+$ && "${2:-}" =~ ^[0-9]+$ ]] || return 1
    [[ "$1" -eq "$2" ]]
}

# Sample-based self-verification of the calculations above; the phase loop
# calls these same functions.
local_validation_register median_of_lines e2e_median_of_lines \
    median_of_lines_good median_of_lines_empty_input \
    median_of_lines_even_count median_of_lines_numeric_order \
    median_of_lines_non_numeric_lines
local_validation_register eta_seconds e2e_eta_seconds \
    eta_seconds_good eta_seconds_empty_list eta_seconds_no_history \
    eta_seconds_non_numeric
local_validation_register phase_step_ok e2e_phase_step_ok \
    phase_step_ok_good phase_step_ok_repeat phase_step_ok_jump_back \
    phase_step_ok_skipped phase_step_ok_non_numeric
