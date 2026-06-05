#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 10 — desire/current config + show config diff (Steps 39-40)
#
# Exercises `fastrg_cli show config diff`, which compares the DESIRED config
# (etcd, tier-2 source) with the CURRENT running state (node gRPC) for a user.
#   Step 39  in-sync config + desired=connect after a dial
#   Step 40  desired flips to disconnect after a hangup (diff reflects intent)
# ---------------------------------------------------------------------------

_p11_diff() {
    # Run `show config diff user $1` via fastrg_cli on the node (etcd = desire source).
    ssh_node "printf 'show config diff user $1\nquit\n' | /usr/local/bin/fastrg_cli -i 127.0.0.1:${FASTRG_GRPC_PORT} -e ${ETCD_ENDPOINT} -n ${NODE_UUID} 2>&1"
}

phase10_desire_diff() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 10 — desire/current diff (Steps 39-40)"
    bold "═══════════════════════════════════════════════════════"

    if ! ssh_node "test -x /usr/local/bin/fastrg_cli" 2>/dev/null; then
        skip "Step 39: diff in-sync + desired=connect" "fastrg_cli not installed on node"
        skip "Step 40: diff reflects desire flip"      "fastrg_cli not installed on node"
        return
    fi

    # ------------------------------------------------------------------
    # Step 39 — ensure connect baseline, then diff shows in-sync + desired=connect
    # ------------------------------------------------------------------
    info "Step 39: dial USER_ID=${USER_ID}, then 'show config diff'..."
    fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true
    sleep 3
    _p11_out=$(_p11_diff "${USER_ID}" 2>/dev/null || true)
    info "  diff output:"
    printf '%s\n' "$_p11_out" | grep -E "desired|current|diff:" | sed 's/^/    /'

    if printf '%s' "$_p11_out" | grep -q "diff: config in-sync" && \
       printf '%s' "$_p11_out" | grep -q "desired=connect"; then
        pass "Step 39: diff in-sync + desired=connect" "config in-sync and pppoe desired=connect"
    else
        fail "Step 39: diff in-sync + desired=connect" \
            "unexpected diff: $(printf '%s' "$_p11_out" | grep 'diff:' | tr '\n' '|')"
    fi

    # ------------------------------------------------------------------
    # Step 40 — hangup, then diff shows desired=disconnect (intent change)
    # ------------------------------------------------------------------
    info "Step 40: hangup USER_ID=${USER_ID}, then 'show config diff'..."
    fastrg_grpc disconnect_hsi "${USER_ID}" >/dev/null 2>&1 || true
    sleep 3
    _p11_out2=$(_p11_diff "${USER_ID}" 2>/dev/null || true)
    info "  diff output:"
    printf '%s\n' "$_p11_out2" | grep -E "diff:" | sed 's/^/    /'

    if printf '%s' "$_p11_out2" | grep -q "desired=disconnect"; then
        pass "Step 40: diff reflects desire flip" "pppoe desired=disconnect after hangup"
    else
        fail "Step 40: diff reflects desire flip" \
            "expected desired=disconnect: $(printf '%s' "$_p11_out2" | grep 'diff:' | tr '\n' '|')"
    fi

    # Restore connect baseline for subsequent phases / next run.
    fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true
}
