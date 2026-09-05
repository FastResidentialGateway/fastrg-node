#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Shared Prometheus /metrics sampling helpers.
#
# This file defines no phase function. It lives in phases/ so the existing
# "scp phases/*.sh" upload in run_e2e_test.sh carries it to the runner without
# a second transfer path that would have to be kept in sync; run_e2e_test.sh
# sources it before the phase scripts so every phase can use the same sampler.
#
# Phases 20, 25 and 27 predate this file and keep their own private samplers.
# They are deliberately left alone — rewriting working assertions would be
# churn with no behavioural gain. New assertions use the helpers here.
#
# Parsing notes:
#   - A metric line is "family{labels} value". The value is taken from the
#     LAST field, not $2: fastrg_nic_info carries a model label whose value
#     contains spaces ("Intel Corporation Ethernet Controller X710 ...").
#   - Family matching anchors on 'family{' so a family never matches another
#     one that has it as a prefix (fastrg_node_rx_packets_total vs
#     fastrg_node_per_user_rx_packets_total).
#   - Values are read with awk rather than jq: jq's "// empty" idiom silently
#     turns a legitimate 0 or false into an empty string, and most of these
#     metrics legitimately read 0.
#   - node_uuid is never matched on; it is read from the exposition as-is.
# ---------------------------------------------------------------------------

_E2E_METRICS_PORT=""

# Resolve (and cache) the node's MetricsListenPort. Prints the port; prints
# nothing when the node config has no MetricsListenPort.
e2e_metrics_port() {
    local _raw=""

    if [[ -z "$_E2E_METRICS_PORT" ]]; then
        _raw=$(ssh_node "grep 'MetricsListenPort' /etc/fastrg/config.cfg 2>/dev/null" \
            2>/dev/null | awk -F'"' '{print $2}' || true)
        _E2E_METRICS_PORT="${_raw##*:}"
    fi
    printf '%s' "$_E2E_METRICS_PORT"
}

# Print one complete /metrics scrape. Empty when the endpoint is unreachable.
e2e_metrics_body() {
    local _port

    _port=$(e2e_metrics_port)
    [[ -n "$_port" ]] || return 0
    ssh_node "curl -fsS --max-time 5 http://127.0.0.1:${_port}/metrics" 2>/dev/null || true
}

# Internal: scan one scrape body for a family, optionally filtered by
# "label=value" arguments, and print either the first value, the sum of all
# matching values, or the number of matching rows.
_e2e_metric_scan() {
    local _body="$1" _family="$2" _mode="$3"

    shift 3
    printf '%s\n' "$_body" | awk -v family="$_family" -v mode="$_mode" -v filters="$*" '
        BEGIN { nfilters = split(filters, want, " ") }
        index($1, family "{") != 1 { next }
        {
            for (i = 1; i <= nfilters; i++) {
                split(want[i], kv, "=")
                if (index($0, kv[1] "=\"" kv[2] "\"") == 0)
                    next
            }
            rows++
            total += $NF
            if (mode == "first") {
                print $NF
                exit
            }
        }
        END {
            # No matching row prints nothing, so an absent family reads as
            # missing rather than as a legitimate zero.
            if (rows == 0) exit
            if (mode == "sum")   printf "%d\n", total
            if (mode == "count") printf "%d\n", rows
        }
    ' || true
}

# e2e_metric_value BODY FAMILY [label=value ...] — first matching sample value.
e2e_metric_value() {
    _e2e_metric_scan "$1" "$2" first "${@:3}"
}

# e2e_metric_sum BODY FAMILY [label=value ...] — sum over all matching rows.
e2e_metric_sum() {
    _e2e_metric_scan "$1" "$2" sum "${@:3}"
}

# e2e_metric_rows BODY FAMILY [label=value ...] — number of matching rows.
e2e_metric_rows() {
    _e2e_metric_scan "$1" "$2" count "${@:3}"
}

# e2e_metric_label BODY FAMILY LABEL [label=value ...] — the LABEL value of the
# first matching row (empty when the row or the label is absent).
e2e_metric_label() {
    local _body="$1" _family="$2" _label="$3"

    shift 3
    printf '%s\n' "$_body" | awk -v family="$_family" -v label="$_label" -v filters="$*" '
        BEGIN { nfilters = split(filters, want, " ") }
        index($1, family "{") != 1 { next }
        {
            for (i = 1; i <= nfilters; i++) {
                split(want[i], kv, "=")
                if (index($0, kv[1] "=\"" kv[2] "\"") == 0)
                    next
            }
            if (match($0, label "=\"[^\"]*\""))
                print substr($0, RSTART + length(label) + 2, RLENGTH - length(label) - 3)
            exit
        }
    ' || true
}

# e2e_metric_label_values BODY FAMILY LABEL [label=value ...] — the LABEL value
# of EVERY matching row, one per line (empty when no row matches).
e2e_metric_label_values() {
    local _body="$1" _family="$2" _label="$3"

    shift 3
    printf '%s\n' "$_body" | awk -v family="$_family" -v label="$_label" -v filters="$*" '
        BEGIN { nfilters = split(filters, want, " ") }
        index($1, family "{") != 1 { next }
        {
            for (i = 1; i <= nfilters; i++) {
                split(want[i], kv, "=")
                if (index($0, kv[1] "=\"" kv[2] "\"") == 0)
                    next
            }
            if (match($0, label "=\"[^\"]*\""))
                print substr($0, RSTART + length(label) + 2, RLENGTH - length(label) - 3)
        }
    ' || true
}

# All values are unsigned integers in the exposition; a non-numeric read means
# the family or the labelled row was missing.
e2e_is_uint() {
    [[ "${1:-}" =~ ^[0-9]+$ ]]
}

# e2e_all_uint V... — true when every argument parsed as an unsigned integer.
e2e_all_uint() {
    local _v

    for _v in "$@"; do
        e2e_is_uint "$_v" || return 1
    done
    return 0
}

# Sample-based self-verification of the extractors above; the phases call these
# same functions.
local_validation_register metric_scan _e2e_metric_scan \
    metric_scan_good metric_scan_empty_input metric_scan_no_matching_rows
local_validation_register metric_value e2e_metric_value \
    metric_value_good metric_value_family_prefix_collision \
    metric_value_value_in_last_field metric_value_empty_input \
    metric_value_legitimate_zero
local_validation_register metric_sum e2e_metric_sum \
    metric_sum_good metric_sum_family_prefix_collision \
    metric_sum_empty_input metric_sum_no_matching_rows
local_validation_register metric_rows e2e_metric_rows \
    metric_rows_good metric_rows_empty_input metric_rows_no_matching_rows
local_validation_register metric_label e2e_metric_label \
    metric_label_good metric_label_empty_input metric_label_absent_label
local_validation_register metric_label_values e2e_metric_label_values \
    metric_label_values_good metric_label_values_empty_input \
    metric_label_values_no_matching_rows
local_validation_register is_uint e2e_is_uint \
    is_uint_good is_uint_empty_input is_uint_non_numeric is_uint_negative
local_validation_register all_uint e2e_all_uint \
    all_uint_good all_uint_empty_input all_uint_one_non_numeric
