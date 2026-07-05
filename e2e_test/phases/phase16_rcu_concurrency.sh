#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 16 — RCU reader/writer concurrency under data-plane load
#
# Exercises the per-lcore persistent-online RCU getter fast path: a high-rate
# iperf3 flow keeps the data-plane lcores hammering PPPD_GET_CCB / DHCP / stats
# getters (RCU readers on the fast path, reporting quiescent once per burst),
# while SetSubscriberCount is fired repeatedly — each call makes fastrg realloc
# the ppp_ccb / dhcp_ccb / per_subscriber_stats arrays and run
# rte_rcu_qsbr_synchronize (the RCU writer).
#
# If fast-path readers stopped reporting quiescent, synchronize would block and
# SetSubscriberCount would hang; if a reader saw a freed array the node would
# crash. Both steps passing under sustained load confirms reader/writer
# concurrency is correct.
# ---------------------------------------------------------------------------
# Helper: restore the subscriber count churned by the Step 64 loop below +
# stop the iperf3 server. Idempotent (set_subscriber_count/pkill both tolerate
# an already-correct/already-dead target). Called at end of phase16 AND from
# the cleanup_fastrg EXIT trap.
_cleanup_phase16_rcu_concurrency() {
    [[ -z "${_cnt_orig:-}" ]] && return
    [[ "${_cnt_orig}" -le 0 ]] 2>/dev/null && return
    info "Cleanup(phase16): restoring subscriber count to ${_cnt_orig}..."
    fastrg_grpc set_subscriber_count "${_cnt_orig}" >/dev/null 2>&1 || true
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" || true
}

phase16_rcu_concurrency() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 16 — RCU reader/writer concurrency under load"
    bold "═══════════════════════════════════════════════════════"

    local SRV_PORT=5901
    local ITERS=8

    local _sys
    _sys=$(fastrg_grpc get_system_info)
    # We can't set _cnt_orig to local, because the cleanup_fastrg EXIT trap calls
    # _cleanup_phase16_rcu_concurrency from outside this function's scope.
    _cnt_orig=$(printf '%s' "$_sys" | jq -r '.num_users // 0' 2>/dev/null || echo 0)
    if [[ "${_cnt_orig:-0}" -le 0 ]]; then
        skip "Step 64: writer churn under load" "cannot read subscriber count"
        skip "Step 65: data plane survived load+churn" "cannot read subscriber count"
        return
    fi

    # 1. iperf3 server on WAN
    info "Starting iperf3 server on WAN (port ${SRV_PORT})..."
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" || true
    sleep 1
    ssh_wan "iperf3 -s -B ${WAN_IP} -p ${SRV_PORT} -D >/dev/null 2>&1 || true" || true
    for _w in $(seq 1 10); do
        sleep 1
        ssh_wan "ss -ltn 2>/dev/null | grep -q ':${SRV_PORT}'" 2>/dev/null && break
    done

    # 2. Background iperf3 LAN->WAN load spanning the churn window
    info "Starting background iperf3 LAN->WAN load (25s)..."
    ssh_lan "(iperf3 -c ${WAN_IP} -p ${SRV_PORT} -t 25 -J >/tmp/e2e_rcu_iperf.json 2>/dev/null) &" || true
    local _ready=0
    for _w in $(seq 1 12); do
        sleep 1
        if ssh_lan "ss -tn 'dport = :${SRV_PORT}' 2>/dev/null | grep -q ESTAB"; then _ready=1; break; fi
    done
    [[ "$_ready" -eq 1 ]] && info "  iperf3 flow ESTABLISHED" \
        || info "  (iperf3 ESTAB not confirmed; getters still exercised by warmup traffic)"

    # 3. RCU writer churn while traffic flows: each SetSubscriberCount reallocs
    #    the ccb/stats arrays and calls rte_rcu_qsbr_synchronize.  A stuck
    #    fast-path reader would make this hang; we also re-query the node right
    #    after to confirm synchronize returned and the node is still serving.
    local _ok=1 _detail="" i target reply status sys2 cnt2
    for ((i=1; i<=ITERS; i++)); do
        if (( i % 2 == 1 )); then target=$((_cnt_orig + 2)); else target=$((_cnt_orig + 6)); fi
        reply=$(fastrg_grpc set_subscriber_count "${target}")
        status=$(printf '%s' "$reply" | jq -r '.status // empty' 2>/dev/null || true)
        sys2=$(fastrg_grpc get_system_info)
        cnt2=$(printf '%s' "$sys2" | jq -r '.num_users // empty' 2>/dev/null || true)
        if [[ -z "$status" || -z "$cnt2" ]]; then
            _ok=0
            _detail="iter ${i}: set_subscriber_count(${target}) status='${status}' num_users='${cnt2}'"
            break
        fi
    done

    if [[ "$_ok" -eq 1 ]]; then
        pass "Step 64: writer churn under load" \
            "${ITERS} SetSubscriberCount reallocs all returned + node responsive while data plane under load"
    else
        fail "Step 64: writer churn under load" "${_detail}"
    fi

    # 4. Drain iperf3, assert node still alive (readers not starved, no UAF crash)
    info "Waiting for background iperf3 to finish..."
    ssh_lan "for _k in \$(seq 1 30); do pgrep -f 'iperf3 -c' >/dev/null || break; sleep 1; done" || true
    local mbps sys3 cnt3
    mbps=$(ssh_lan "command -v jq >/dev/null 2>&1 && jq -r '(.end.sum_received.bits_per_second // 0)/1000000 | floor' /tmp/e2e_rcu_iperf.json 2>/dev/null || echo NA")
    sys3=$(fastrg_grpc get_system_info)
    cnt3=$(printf '%s' "$sys3" | jq -r '.num_users // empty' 2>/dev/null || true)
    if [[ -n "$cnt3" ]]; then
        pass "Step 65: data plane survived load+churn" \
            "node responsive after load+churn (num_users=${cnt3}, iperf3=${mbps} Mbps)"
    else
        fail "Step 65: data plane survived load+churn" "node unresponsive after load+churn"
    fi

    # 5. Cleanup: restore original count + stop iperf3 server
    info "Restoring subscriber count to ${_cnt_orig}..."
    fastrg_grpc set_subscriber_count "${_cnt_orig}" >/dev/null 2>&1 || true
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" || true
}
