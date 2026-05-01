#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 1 — SetSubscriberCount gRPC→etcd→fastrg Tests (Steps 19–20)
#
# Calls SetSubscriberCount gRPC, verifies the etcd user_counts key is
# updated, and verifies fastrg_node local subscriber count (num_users in
# GetFastrgSystemInfo) changes accordingly.  Restores the original count at
# the end so other phases are not affected.
# ---------------------------------------------------------------------------
phase1_subscriber_count_tests() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 1 — SetSubscriberCount gRPC→etcd Tests (Steps 1-2)"
    bold "═══════════════════════════════════════════════════════"

    # Read current subscriber count from fastrg local state
    info "Reading current subscriber count from fastrg (GetFastrgSystemInfo)..."
    _sc_sys_orig=$(fastrg_grpc get_system_info)
    _sc_orig=$(printf '%s' "$_sc_sys_orig" | jq -r '.num_users // 0' 2>/dev/null || echo 0)

    if [[ "$_sc_orig" -eq 0 ]]; then
        skip "Step 1: SetSubscriberCount new value" \
            "Cannot read current subscriber count from fastrg (got 0 or error)"
        skip "Step 2: Restore SubscriberCount" \
            "Cannot read current subscriber count from fastrg"
        return
    fi
    info "Current subscriber count: ${_sc_orig}"

    # Use current + 1 as the test value (safe: well within MAX_USER_COUNT 4000)
    _sc_new=$((_sc_orig + 1))

    # ------------------------------------------------------------------
    # Step 1 — SetSubscriberCount → etcd updated + fastrg local updated
    # ------------------------------------------------------------------
    info "Step 1: SetSubscriberCount ${_sc_new} (current=${_sc_orig})..."
    _sc20_reply=$(fastrg_grpc set_subscriber_count "${_sc_new}")
    _sc20_status=$(printf '%s' "$_sc20_reply" | jq -r '.status // empty' 2>/dev/null || true)

    if [[ -z "$_sc20_status" ]]; then
        fail "Step 1: SetSubscriberCount ${_sc_new}" \
            "gRPC returned no status — response: $(printf '%s' "$_sc20_reply")"
        skip "Step 2: Restore SubscriberCount ${_sc_orig}" "SetSubscriberCount failed"
        return
    fi

    sleep 2  # allow etcd write + watcher callback to apply locally

    # Verify etcd: key user_counts/{NODE_UUID}/ has subscriber_count = _sc_new
    _sc19_etcd=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null || true)
    _sc19_etcd_val=$(printf '%s' "$_sc19_etcd" | jq -r '.subscriber_count // empty' 2>/dev/null || true)

    # Verify fastrg local: get_system_info num_users = _sc_new
    _sc19_sys=$(fastrg_grpc get_system_info)
    _sc19_local=$(printf '%s' "$_sc19_sys" | jq -r '.num_users // 0' 2>/dev/null || echo 0)

    MISMATCH=""
    [[ "$_sc19_etcd_val" != "$_sc_new" ]] && \
        MISMATCH="${MISMATCH} etcd(got=${_sc19_etcd_val:-empty} expected=${_sc_new})"
    [[ "$_sc19_local"    != "$_sc_new" ]] && \
        MISMATCH="${MISMATCH} local(got=${_sc19_local} expected=${_sc_new})"

    if [[ -z "$MISMATCH" ]]; then
        pass "Step 1: SetSubscriberCount ${_sc_new}" \
            "etcd=${_sc19_etcd_val} fastrg_local=${_sc19_local}"
    else
        fail "Step 1: SetSubscriberCount ${_sc_new}" "Mismatch:${MISMATCH}"
    fi

    # ------------------------------------------------------------------
    # Step 2 — Restore original subscriber count → verify etcd + local
    # ------------------------------------------------------------------
    info "Step 2: Restoring subscriber count to ${_sc_orig}..."
    _sc20_reply=$(fastrg_grpc set_subscriber_count "${_sc_orig}")
    _sc20_status=$(printf '%s' "$_sc20_reply" | jq -r '.status // empty' 2>/dev/null || true)

    if [[ -z "$_sc20_status" ]]; then
        fail "Step 2: Restore SubscriberCount ${_sc_orig}" \
            "gRPC returned no status — response: $(printf '%s' "$_sc20_reply")"
        return
    fi

    sleep 2  # allow etcd write + watcher callback

    # Verify etcd restored
    _sc20_etcd=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null || true)
    _sc20_etcd_val=$(printf '%s' "$_sc20_etcd" | jq -r '.subscriber_count // empty' 2>/dev/null || true)

    # Verify fastrg local restored
    _sc20_sys=$(fastrg_grpc get_system_info)
    _sc20_local=$(printf '%s' "$_sc20_sys" | jq -r '.num_users // 0' 2>/dev/null || echo 0)

    MISMATCH=""
    [[ "$_sc20_etcd_val" != "$_sc_orig" ]] && \
        MISMATCH="${MISMATCH} etcd(got=${_sc20_etcd_val:-empty} expected=${_sc_orig})"
    [[ "$_sc20_local"    != "$_sc_orig" ]] && \
        MISMATCH="${MISMATCH} local(got=${_sc20_local} expected=${_sc_orig})"

    if [[ -z "$MISMATCH" ]]; then
        pass "Step 2: Restore SubscriberCount ${_sc_orig}" \
            "etcd=${_sc20_etcd_val} fastrg_local=${_sc20_local}"
    else
        fail "Step 2: Restore SubscriberCount ${_sc_orig}" "Mismatch:${MISMATCH}"
    fi
}
