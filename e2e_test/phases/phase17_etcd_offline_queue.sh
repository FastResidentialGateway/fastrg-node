#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 17 — etcd Offline Config Queue (Steps 66-67)
#
# The node's SDN guard normally rejects direct gRPC ApplyConfig calls
# (FAILED_PRECONDITION) whenever etcd is reachable — see phase9 Step 38.
# This phase blocks the node's outbound path to etcd with a REJECT iptables
# rule (so the TCP connection fails fast instead of timing out), which flips
# etcd_reachable_=false on the node's next watchdog tick. It then:
#
#   Step 66  while etcd is unreachable, node gRPC ApplyConfig (CLI tier 3) is
#            now ACCEPTED, applies the config locally, and queues it to
#            /etc/fastrg/config_queue.json for later etcd flush
#   Step 67  once connectivity is restored, the node's watchdog flushes the
#            queue via cas_put() within one tick — the config lands in etcd
#            with the values written in Step 66
#
# The watchdog polls etcd reachability every WATCHDOG_CHECK_INTERVAL_SEC
# (60s, etcd_client.cpp), so this phase budgets up to ~90s to detect "down"
# and ~90s to detect "up" + flush. The iptables rule is removed unconditionally
# at the end of the phase AND from the top-level EXIT trap, so a mid-phase
# failure never leaves the node stranded offline from etcd.
# ---------------------------------------------------------------------------

_p17_iptables_blocked=0

_p17_block_etcd() {
    # Idempotent: clear any stale rule left by a previous crashed run first.
    ssh_node "iptables -D OUTPUT -p tcp -d ${_P17_ETCD_HOST} --dport ${_P17_ETCD_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    if ssh_node "iptables -I OUTPUT 1 -p tcp -d ${_P17_ETCD_HOST} --dport ${_P17_ETCD_PORT} -j REJECT --reject-with tcp-reset" \
        >/dev/null 2>&1; then
        _p17_iptables_blocked=1
    else
        _p17_iptables_blocked=0
        warn "Step 66: failed to install iptables block on node"
    fi
}

_p17_unblock_etcd() {
    # -D is safe to call even if the rule is already gone (idempotent cleanup).
    ssh_node "iptables -D OUTPUT -p tcp -d ${_P17_ETCD_HOST} --dport ${_P17_ETCD_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    _p17_iptables_blocked=0
}

# Idempotent: called at the end of phase17 AND from the cleanup_fastrg EXIT
# trap, so a crash mid-phase can never leave the node's etcd path blocked or
# a stray test-user key behind.
_cleanup_phase17_etcd_offline_queue() {
    if [[ "${_p17_iptables_blocked:-0}" -eq 1 ]]; then
        info "Cleanup(phase17): removing node->etcd iptables block..."
        _p17_unblock_etcd
    fi
    if [[ -n "${_P17_UID:-}" ]] && [[ -n "${NODE_UUID:-}" ]]; then
        info "Cleanup(phase17): removing user ${_P17_UID} config with verification..."
        remove_hsi_config_verified "${_P17_UID}" || true
    fi
    if [[ -n "${_P17_ORIG_SC:-}" ]]; then
        fastrg_grpc set_subscriber_count "${_P17_ORIG_SC}" >/dev/null 2>&1 || true
    fi
}

# Raw node gRPC ApplyConfig call (bypasses the controller entirely — same
# invocation shape as phase9 Step 38, but with full DHCP fields so a
# non-SDN-guard ApplyConfig can actually succeed).
_p17_apply_config_direct() {
    local uid="$1" vlan="$2"
    ssh_node "grpcurl -plaintext -import-path /root/fastrg-node/northbound/grpc -proto fastrg_node.proto \
        -d '{\"user_id\":${uid},\"vlan_id\":${vlan},\"pppoe_account\":\"p18test\",\"pppoe_password\":\"p18pw\",\"dhcp_pool_start\":\"10.188.0.2\",\"dhcp_pool_end\":\"10.188.0.9\",\"dhcp_subnet_mask\":\"255.255.255.0\",\"dhcp_gateway\":\"10.188.0.1\"}' \
        127.0.0.1:${FASTRG_GRPC_PORT} fastrgnodeservice.FastrgService/ApplyConfig 2>&1"
}

phase17_etcd_offline_queue() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 17 — etcd Offline Config Queue (Steps 66-67)"
    bold "═══════════════════════════════════════════════════════"

    if ! ssh_node "command -v iptables >/dev/null 2>&1"; then
        skip "Step 66: offline write accepted + queued" "iptables not available on node"
        skip "Step 67: queued write flushed to etcd on reconnect" "iptables not available on node"
        return
    fi

    # Derive the etcd host/port to block from ETCD_ENDPOINT (parsed by phase0
    # from the node's own /etc/fastrg/config.cfg) rather than hardcoding, so
    # this can't drift from the real endpoint. Not `local` — the block/unblock
    # helpers and the cleanup trap read these too.
    _P17_ETCD_HOST="${ETCD_ENDPOINT%%:*}"
    _P17_ETCD_PORT="${ETCD_ENDPOINT##*:}"
    if [[ -z "$_P17_ETCD_HOST" ]] || [[ -z "$_P17_ETCD_PORT" ]]; then
        fail "Step 66: offline write accepted + queued" \
            "could not parse ETCD_ENDPOINT='${ETCD_ENDPOINT:-<empty>}'"
        skip "Step 67: queued write flushed to etcd on reconnect" "prerequisite failed"
        return
    fi

    # Allocate a fresh, unconfigured user_id beyond the current subscriber
    # count (same pattern as phase9's CLI test users) so ApplyConfig's
    # user-existence check doesn't reject before reaching the SDN guard.
    local _sc
    _sc=$(fastrg_grpc get_system_info 2>/dev/null | jq -r '.num_users // 0' 2>/dev/null || echo 0)
    _sc=$(( ${_sc:-0} + 0 ))
    _P17_ORIG_SC="$_sc"
    _P17_UID=$(( _sc + 2 ))
    local _tgt_sc=$(( _P17_UID + 1 ))
    fastrg_grpc set_subscriber_count "${_tgt_sc}" >/dev/null 2>&1 || true
    local _sc_ok=0
    for _i in $(seq 1 12); do
        sleep 2
        local _got_sc
        _got_sc=$(fastrg_grpc get_system_info 2>/dev/null | jq -r '.num_users // 0' 2>/dev/null || echo 0)
        [[ "${_got_sc:-0}" -ge "${_tgt_sc}" ]] && { _sc_ok=1; break; }
    done
    if [[ $_sc_ok -eq 0 ]]; then
        fail "Step 66: offline write accepted + queued" \
            "subscriber count did not propagate to ${_tgt_sc} in time — cannot allocate test user ${_P17_UID}"
        skip "Step 67: queued write flushed to etcd on reconnect" "prerequisite failed"
        _cleanup_phase17_etcd_offline_queue
        return
    fi

    # ------------------------------------------------------------------
    # Step 66 — block node->etcd, wait for the SDN guard to flip, apply
    #           directly, confirm local apply + on-disk offline queue entry
    # ------------------------------------------------------------------
    info "Step 66: blocking node->etcd (${_P17_ETCD_HOST}:${_P17_ETCD_PORT}) and waiting for offline mode..."
    _p17_block_etcd

    local _p17_vlan=888
    local _p17_accepted=0
    local _p17_last_out=""
    for _i in $(seq 1 45); do
        sleep 2
        _p17_last_out=$(_p17_apply_config_direct "${_P17_UID}" "${_p17_vlan}" 2>/dev/null || true)
        if ! printf '%s' "$_p17_last_out" | grep -qi "FailedPrecondition"; then
            _p17_accepted=1
            info "  ${_i}x2s: ApplyConfig no longer rejected by SDN guard"
            break
        fi
        info "  ${_i}x2s: still SDN-guard-rejected (etcd still looks reachable to the node)"
    done

    if [[ $_p17_accepted -eq 0 ]]; then
        fail "Step 66: offline write accepted + queued" \
            "SDN guard never released within 90s; last output: $(printf '%s' "$_p17_last_out" | tr '\n' '|' | tail -c 200)"
        skip "Step 67: queued write flushed to etcd on reconnect" "prerequisite failed"
        _cleanup_phase17_etcd_offline_queue
        return
    fi

    if ! printf '%s' "$_p17_last_out" | grep -qi "Configuration successful\|\"status\""; then
        fail "Step 66: offline write accepted + queued" \
            "ApplyConfig accepted but did not report success: $(printf '%s' "$_p17_last_out" | tr '\n' '|' | tail -c 200)"
        skip "Step 67: queued write flushed to etcd on reconnect" "prerequisite failed"
        _cleanup_phase17_etcd_offline_queue
        return
    fi

    # Confirm it actually applied locally (not just accepted).
    local _p17_hsi
    _p17_hsi=$(fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[] | select(.user_id == ${_P17_UID})" 2>/dev/null || true)
    local _p17_local_vlan
    _p17_local_vlan=$(printf '%s' "$_p17_hsi" | jq -r '.vlan_id // empty' 2>/dev/null || true)

    # Confirm it landed in the on-disk offline queue (direct proof of queueing,
    # independent of the flush that Step 67 verifies separately).
    local _p17_queue
    _p17_queue=$(ssh_node "cat /etc/fastrg/config_queue.json 2>/dev/null" || true)
    local _p17_queued
    _p17_queued=$(printf '%s' "$_p17_queue" | \
        jq -r ".[] | select(.key == \"configs/${NODE_UUID}/hsi/${_P17_UID}\")" 2>/dev/null || true)

    if [[ "$_p17_local_vlan" == "$_p17_vlan" ]] && [[ -n "$_p17_queued" ]]; then
        pass "Step 66: offline write accepted + queued" \
            "applied locally (vlan=${_p17_local_vlan}) and present in config_queue.json"
    elif [[ "$_p17_local_vlan" == "$_p17_vlan" ]]; then
        fail "Step 66: offline write accepted + queued" \
            "applied locally (vlan=${_p17_local_vlan}) but not found in config_queue.json"
    else
        fail "Step 66: offline write accepted + queued" \
            "not applied locally (got vlan='${_p17_local_vlan:-none}', want ${_p17_vlan})"
    fi

    # ------------------------------------------------------------------
    # Step 67 — restore connectivity, wait for the watchdog to flush the
    #           queue via cas_put(), confirm the config landed in etcd
    # ------------------------------------------------------------------
    info "Step 67: restoring node->etcd connectivity and waiting for queue flush..."
    _p17_unblock_etcd

    local _p67_ok=0
    local _p67_etcd_vlan=""
    for _i in $(seq 1 45); do
        sleep 2
        local _v
        _v=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P17_UID}" 2>/dev/null || true)
        _p67_etcd_vlan=$(printf '%s' "$_v" | jq -r '.config.vlan_id // empty' 2>/dev/null || true)
        if [[ "$_p67_etcd_vlan" == "$_p17_vlan" ]]; then
            _p67_ok=1
            info "  ${_i}x2s: etcd now holds vlan=${_p67_etcd_vlan}"
            break
        fi
        info "  ${_i}x2s: etcd key still missing/mismatched (vlan='${_p67_etcd_vlan:-none}')"
    done

    if [[ $_p67_ok -eq 1 ]]; then
        # Bonus check: the flushed entry should have been removed from the
        # on-disk queue (flush_config_queue() re-persists only unresolved entries).
        local _p67_queue_after
        _p67_queue_after=$(ssh_node "cat /etc/fastrg/config_queue.json 2>/dev/null" || true)
        local _p67_still_queued
        _p67_still_queued=$(printf '%s' "$_p67_queue_after" | \
            jq -r ".[] | select(.key == \"configs/${NODE_UUID}/hsi/${_P17_UID}\")" 2>/dev/null || true)
        if [[ -z "$_p67_still_queued" ]]; then
            pass "Step 67: queued write flushed to etcd on reconnect" \
                "etcd holds vlan=${_p67_etcd_vlan} via CAS flush; queue entry cleared"
        else
            pass "Step 67: queued write flushed to etcd on reconnect" \
                "etcd holds vlan=${_p67_etcd_vlan} (queue file still lists the key — non-fatal)"
        fi
    else
        fail "Step 67: queued write flushed to etcd on reconnect" \
            "etcd key not updated within 90s of restoring connectivity (vlan='${_p67_etcd_vlan:-none}')"
    fi

    _cleanup_phase17_etcd_offline_queue
}
