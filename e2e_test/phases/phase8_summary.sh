#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 8 — Summary
# ---------------------------------------------------------------------------
phase8_summary() {
    local total=${#STEP_NAMES[@]}
    local pass_count=0 fail_count=0 skip_count=0

    printf "\n"
    bold "═══════════════════════════════════════════════════════"
    bold " Test Results Summary"
    bold "═══════════════════════════════════════════════════════"
    printf "\n"

    i=0
    while [[ $i -lt $total ]]; do
        name="${STEP_NAMES[$i]}"
        result="${STEP_RESULTS[$i]}"
        detail="${STEP_DETAILS[$i]}"

        case "$result" in
            PASS)
                printf "  ${GREEN}✔ PASS${NC}  %-40s  %s\n" "$name" "$detail"
                pass_count=$((pass_count + 1))
                ;;
            FAIL)
                printf "  ${RED}✘ FAIL${NC}  %-40s  %s\n" "$name" "$detail"
                fail_count=$((fail_count + 1))
                ;;
            SKIP)
                printf "  ${YELLOW}– SKIP${NC}  %-40s  %s\n" "$name" "$detail"
                skip_count=$((skip_count + 1))
                ;;
        esac
        i=$((i + 1))
    done

    printf "\n"
    printf "  Total: %d   " "$total"
    printf "${GREEN}Pass: %d${NC}   " "$pass_count"
    printf "${RED}Fail: %d${NC}   " "$fail_count"
    printf "${YELLOW}Skip: %d${NC}\n" "$skip_count"
    printf "\n"

    if [[ $fail_count -gt 0 ]]; then
        bold "  RESULT: ${RED}FAILED${NC} (${fail_count} step(s) failed)"
        return 1
    else
        bold "  RESULT: ${GREEN}ALL PASSED${NC}"
        return 0
    fi
}
