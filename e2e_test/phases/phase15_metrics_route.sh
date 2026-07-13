#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 15 — Prometheus /metrics route (Steps 59-63)
#
# Verifies the node's built-in lighthttp /metrics endpoint (served by the
# fastrg metrics thread) is healthy and returns a well-formed Prometheus text
# exposition: the route answers 200, exposes the expected fastrg_node_* / NIC
# metric families, has no duplicate TYPE lines, and 404s on an unknown path.
#
# The metrics listen port is read from the node's config.cfg (MetricsListenPort)
# so the check follows whatever the node is actually configured to serve.
# ---------------------------------------------------------------------------
phase15_metrics_route() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 15 — Prometheus /metrics route (Steps 59-63)"
    bold "═══════════════════════════════════════════════════════"

    # Resolve the metrics port from the node config (e.g. "55178" or "0.0.0.0:55178").
    local _mport_raw _mport
    _mport_raw=$(ssh_node "grep 'MetricsListenPort' /etc/fastrg/config.cfg 2>/dev/null" | awk -F'"' '{print $2}')
    _mport="${_mport_raw##*:}"
    if [[ -z "$_mport" ]]; then
        skip "Step 59: read MetricsListenPort" "MetricsListenPort not set in /etc/fastrg/config.cfg"
        skip "Step 60: /metrics returns 200"     "no metrics port"
        skip "Step 61: expected metric families" "no metrics port"
        skip "Step 62: well-formed exposition"   "no metrics port"
        skip "Step 63: unknown path returns 404"  "no metrics port"
        return
    fi
    info "Metrics endpoint: ${FASTRG_NODE}:${_mport}/metrics"
    pass "Step 59: read MetricsListenPort" "port=${_mport}"

    # ------------------------------------------------------------------
    # Step 2 — GET /metrics returns HTTP 200 with a body
    # ------------------------------------------------------------------
    local _code
    _code=$(ssh_node "curl -s -o /tmp/e2e_metrics.txt -w '%{http_code}' --max-time 5 http://127.0.0.1:${_mport}/metrics" 2>/dev/null)
    local _body
    _body=$(ssh_node "cat /tmp/e2e_metrics.txt 2>/dev/null")

    if [[ "$_code" != "200" ]]; then
        fail "Step 60: /metrics returns 200" "got HTTP '${_code}'"
        skip "Step 61: expected metric families" "/metrics not reachable"
        skip "Step 62: well-formed exposition"   "/metrics not reachable"
        skip "Step 63: unknown path returns 404"  "/metrics not reachable"
        return
    fi
    local _lines
    _lines=$(printf '%s\n' "$_body" | wc -l | xargs)
    pass "Step 60: /metrics returns 200" "HTTP 200, ${_lines} lines"

    # ------------------------------------------------------------------
    # Step 3 — expected metric families are present
    # ------------------------------------------------------------------
    local _missing=""
    local _want=(
        fastrg_node_start_time_seconds
        fastrg_node_restart_total
        fastrg_node_rx_packets_total
        fastrg_node_lcore_busy_cycles_total
        fastrg_nic_link_up
        fastrg_nic_link_flaps_total
        fastrg_nic_info
        fastrg_node_per_user_nat_entries_used
        fastrg_node_per_user_nat_alloc_fail_total
        fastrg_node_per_user_nat_gc_reclaimed_total
    )
    local _m
    for _m in "${_want[@]}"; do
        printf '%s\n' "$_body" | grep -q "^${_m}{" || _missing="${_missing} ${_m}"
    done
    if [[ -z "$_missing" ]]; then
        pass "Step 61: expected metric families" "all ${#_want[@]} present"
    else
        fail "Step 61: expected metric families" "missing:${_missing}"
    fi

    # ------------------------------------------------------------------
    # Step 4 — well-formed exposition: no duplicate "# TYPE" lines
    # (Prometheus rejects a scrape with a metric family declared twice)
    # ------------------------------------------------------------------
    local _dups
    _dups=$(printf '%s\n' "$_body" | grep '^# TYPE ' | sort | uniq -d)
    if [[ -z "$_dups" ]]; then
        pass "Step 62: well-formed exposition" "no duplicate TYPE lines"
    else
        fail "Step 62: well-formed exposition" "duplicate TYPE: $(printf '%s' "$_dups" | tr '\n' ';')"
    fi

    # ------------------------------------------------------------------
    # Step 5 — an unknown path returns 404 (route table works)
    # ------------------------------------------------------------------
    local _code404
    _code404=$(ssh_node "curl -s -o /dev/null -w '%{http_code}' --max-time 5 http://127.0.0.1:${_mport}/does-not-exist" 2>/dev/null)
    if [[ "$_code404" == "404" ]]; then
        pass "Step 63: unknown path returns 404" "HTTP 404"
    else
        fail "Step 63: unknown path returns 404" "got HTTP '${_code404}'"
    fi
}
