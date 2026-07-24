#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 17 — Offline Edits via Snapshot + Kafka (Steps 68-69)
#
# Controller-is-all: the node never writes etcd. While etcd is unreachable
# (simulated with a REJECT iptables rule, which flips the SDN guard on the
# node's next watchdog tick), node gRPC config writes are accepted, applied
# locally and recorded in the persisted snapshot
# (/etc/fastrg/config_snapshot.json) with a dirty flag and a bumped
# resourceVersion. On reconnect the node diffs dirty entries against etcd and
# reports them to the controller as ConfigOfflineEdit events over Kafka —
# etcd itself must remain untouched by the node.
#
#   Step 68  while etcd is unreachable, node gRPC ApplyConfig (CLI tier 3) is
#            ACCEPTED, applies locally, and lands in the snapshot as a dirty
#            entry (rv stamped per the resource-version)
#   Step 69  once connectivity is restored: the dirty entry is reported and
#            cleared, a ConfigOfflineEdit for this node appears on the Kafka
#            topic (verified with kcat; protobuf strings are greppable), and
#            the etcd key is NOT created by the node
#
# The watchdog polls etcd reachability every 60s, so this phase budgets ~90s
# to detect "down" and ~120s for the reconnect sync. The iptables rule is
# removed unconditionally at the end of the phase AND from the top-level EXIT
# trap.
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
        warn "Step 68: failed to install iptables block on node"
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

# Read the node's persisted snapshot entry for the phase's test user.
_p17_snapshot_entry() {
    ssh_node "cat /etc/fastrg/config_snapshot.json 2>/dev/null" | \
        jq -r ".entries[]? | select(.key == \"hsi/${_P17_UID}\")" 2>/dev/null || true
}

phase17_etcd_offline_queue() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 17 — Offline Edits via Snapshot + Kafka (Steps 68-69)"
    bold "═══════════════════════════════════════════════════════"

    if ! ssh_node "command -v iptables >/dev/null 2>&1"; then
        skip "Step 68: offline write accepted + snapshotted" "iptables not available on node"
        skip "Step 69: offline edit reported over Kafka, etcd untouched" "iptables not available on node"
        return
    fi
    if ! ssh_node "command -v kcat >/dev/null 2>&1"; then
        fail "Step 68: offline write accepted + snapshotted" "kcat not installed on node (needed to verify the Kafka report)"
        skip "Step 69: offline edit reported over Kafka, etcd untouched" "prerequisite failed"
        return
    fi

    _P17_ETCD_HOST="${ETCD_ENDPOINT%%:*}"
    _P17_ETCD_PORT="${ETCD_ENDPOINT##*:}"
    if [[ -z "$_P17_ETCD_HOST" ]] || [[ -z "$_P17_ETCD_PORT" ]]; then
        fail "Step 68: offline write accepted + snapshotted" \
            "could not parse ETCD_ENDPOINT='${ETCD_ENDPOINT:-<empty>}'"
        skip "Step 69: offline edit reported over Kafka, etcd untouched" "prerequisite failed"
        return
    fi

    # The node's Kafka broker list, read from its own config so the kcat
    # verification consumes from the same cluster the producer uses.
    local _p17_brokers
    _p17_brokers=$(ssh_node "grep 'KafkaBrokers' /etc/fastrg/config.cfg 2>/dev/null" | \
        awk -F'"' '{print $2}' || true)
    if [[ -z "$_p17_brokers" ]]; then
        fail "Step 68: offline write accepted + snapshotted" "cannot read KafkaBrokers from node config"
        skip "Step 69: offline edit reported over Kafka, etcd untouched" "prerequisite failed"
        return
    fi

    # Allocate a fresh, unconfigured user_id beyond the current subscriber
    # count (same pattern as phase9's CLI test users).
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
        fail "Step 68: offline write accepted + snapshotted" \
            "subscriber count did not propagate to ${_tgt_sc} in time — cannot allocate test user ${_P17_UID}"
        skip "Step 69: offline edit reported over Kafka, etcd untouched" "prerequisite failed"
        _cleanup_phase17_etcd_offline_queue
        return
    fi

    # Record the current end of the Kafka topic so Step 69 only inspects
    # messages produced after this point.
    local _p17_kafka_baseline
    _p17_kafka_baseline=$(ssh_node "timeout 10 kcat -b ${_p17_brokers} -t fastrg.node.events -C -e -f '%o\n' -o -1 2>/dev/null | tail -1" || true)
    [[ "$_p17_kafka_baseline" =~ ^[0-9]+$ ]] || _p17_kafka_baseline=-1

    # ------------------------------------------------------------------
    # Step 68 — block node->etcd, wait for the SDN guard to flip, apply
    #           directly, confirm local apply + dirty snapshot entry
    # ------------------------------------------------------------------
    info "Step 68: blocking node->etcd (${_P17_ETCD_HOST}:${_P17_ETCD_PORT}) and waiting for offline mode..."
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
        fail "Step 68: offline write accepted + snapshotted" \
            "SDN guard never released within 90s; last output: $(printf '%s' "$_p17_last_out" | tr '\n' '|' | tail -c 200)"
        skip "Step 69: offline edit reported over Kafka, etcd untouched" "prerequisite failed"
        _cleanup_phase17_etcd_offline_queue
        return
    fi

    if ! printf '%s' "$_p17_last_out" | grep -qi "Configuration successful\|\"status\""; then
        fail "Step 68: offline write accepted + snapshotted" \
            "ApplyConfig accepted but did not report success: $(printf '%s' "$_p17_last_out" | tr '\n' '|' | tail -c 200)"
        skip "Step 69: offline edit reported over Kafka, etcd untouched" "prerequisite failed"
        _cleanup_phase17_etcd_offline_queue
        return
    fi

    # Confirm it actually applied locally (not just accepted).
    local _p17_hsi _p17_local_vlan
    _p17_hsi=$(fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[] | select(.user_id == ${_P17_UID})" 2>/dev/null || true)
    _p17_local_vlan=$(printf '%s' "$_p17_hsi" | jq -r '.vlan_id // empty' 2>/dev/null || true)

    # Confirm the snapshot holds a dirty entry with the edit and a stamped rv.
    local _p17_entry _p17_dirty _p17_rv
    _p17_entry=$(_p17_snapshot_entry)
    _p17_dirty=$(printf '%s' "$_p17_entry" | jq -r '.dirty // false' 2>/dev/null || true)
    _p17_rv=$(printf '%s' "$_p17_entry" | jq -r '.value // ""' 2>/dev/null | \
        jq -r '.metadata.resourceVersion // empty' 2>/dev/null || true)

    if [[ "$_p17_local_vlan" == "$_p17_vlan" ]] && [[ "$_p17_dirty" == "true" ]] && \
       [[ "$_p17_rv" =~ ^[0-9]+$ ]]; then
        pass "Step 68: offline write accepted + snapshotted" \
            "applied locally (vlan=${_p17_local_vlan}); snapshot dirty entry present (rv=${_p17_rv})"
    else
        fail "Step 68: offline write accepted + snapshotted" \
            "local vlan='${_p17_local_vlan:-none}' (want ${_p17_vlan}); snapshot dirty='${_p17_dirty:-none}' rv='${_p17_rv:-none}'"
    fi

    # ------------------------------------------------------------------
    # Step 69 — restore connectivity; the node must report the offline edit
    #           over Kafka and must NOT write etcd
    # ------------------------------------------------------------------
    info "Step 69: restoring node->etcd connectivity and waiting for the offline-edit report..."
    _p17_unblock_etcd

    # Wait for the reconnect sync: the dirty flag clears once the edit has
    # been reported (or matched etcd content, which cannot happen here).
    local _p17_synced=0
    for _i in $(seq 1 60); do
        sleep 2
        _p17_entry=$(_p17_snapshot_entry)
        # NB: no `// empty` here — jq's `//` treats false as absent, which
        # would turn the cleared flag (false) into "" and never match below.
        _p17_dirty=$(printf '%s' "$_p17_entry" | jq -r '.dirty' 2>/dev/null || true)
        if [[ "$_p17_dirty" == "false" ]]; then
            _p17_synced=1
            info "  ${_i}x2s: snapshot entry no longer dirty (reconnect sync ran)"
            break
        fi
        info "  ${_i}x2s: snapshot entry still dirty"
    done

    local _p17_issue=""
    if [[ $_p17_synced -eq 0 ]]; then
        _p17_issue="dirty flag never cleared within 120s of restoring connectivity"
    fi

    # The node must NOT have written etcd: the key stays absent until the
    # controller (once implemented) arbitrates and writes it itself.
    local _p17_etcd_val
    _p17_etcd_val=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P17_UID}" 2>/dev/null || true)
    if [[ -n "$_p17_etcd_val" ]]; then
        _p17_issue="${_p17_issue:+${_p17_issue}; }etcd key was written (node must be read-only): $(printf '%s' "$_p17_etcd_val" | tail -c 120)"
    fi

    # A ConfigOfflineEdit for this node must have landed on the topic after
    # our baseline offset. Protobuf keeps strings verbatim, so grep for the
    # node uuid + the edit summary + the test account written offline.
    local _p17_kafka_out
    _p17_kafka_out=$(ssh_node "timeout 20 kcat -b ${_p17_brokers} -t fastrg.node.events -C -o $(( _p17_kafka_baseline + 1 )) -e -q 2>/dev/null | strings" || true)
    if ! printf '%s' "$_p17_kafka_out" | grep -q "apply config" || \
       ! printf '%s' "$_p17_kafka_out" | grep -q "p18test"; then
        _p17_issue="${_p17_issue:+${_p17_issue}; }no ConfigOfflineEdit observed on Kafka after offset ${_p17_kafka_baseline} (markers 'apply config'/'p18test' missing)"
    fi

    if [[ -z "$_p17_issue" ]]; then
        pass "Step 69: offline edit reported over Kafka, etcd untouched" \
            "dirty cleared after reconnect; ConfigOfflineEdit observed on fastrg.node.events; etcd key absent (node read-only)"
    else
        fail "Step 69: offline edit reported over Kafka, etcd untouched" "$_p17_issue"
    fi

    # ------------------------------------------------------------------
    # Step 69b — offline DELETE proposal (tombstone): while etcd
    # is unreachable, RemoveConfig is accepted, removes locally and records a
    # tombstone; on reconnect a deleted=true ConfigOfflineEdit is reported and
    # the etcd key is NOT deleted by the node.
    # (Suffixed step id — does not shift the global numbering, same precedent
    # as Step 8-1 / 4a-4e.)
    # ------------------------------------------------------------------
    info "Step 69b: seeding etcd HSI key for user ${_P17_UID} to test the offline-delete proposal..."

    # Simulate a controller-written config for the test user and let the watch
    # mirror it into the snapshot (entry exists, clean).
    local _p17b_key="configs/${NODE_UUID}/hsi/${_P17_UID}"
    ssh_node "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} put ${_p17b_key} '{\"config\":{\"account_name\":\"p18test\",\"desire_status\":\"disconnect\",\"dhcp_addr_pool\":\"10.188.0.2-10.188.0.9\",\"dhcp_gateway\":\"10.188.0.1\",\"dhcp_subnet\":\"255.255.255.0\",\"dns_proxy_enable\":true,\"password\":\"p18pw\",\"tcp_conntrack_enable\":true,\"user_id\":\"${_P17_UID}\",\"vlan_id\":\"889\"},\"metadata\":{\"node\":\"${NODE_UUID}\",\"resourceVersion\":\"7\",\"updatedAt\":\"2026-01-01T00:00:00Z\",\"updatedBy\":\"e2e-step69b\"}}'" \
        >/dev/null 2>&1
    local _p17b_mirrored=0
    for _i in $(seq 1 15); do
        sleep 2
        _p17_entry=$(_p17_snapshot_entry)
        if [[ "$(printf '%s' "$_p17_entry" | jq -r '.exists' 2>/dev/null)" == "true" ]] && \
           [[ "$(printf '%s' "$_p17_entry" | jq -r '.dirty' 2>/dev/null)" == "false" ]]; then
            _p17b_mirrored=1
            break
        fi
    done
    if [[ $_p17b_mirrored -eq 0 ]]; then
        fail "Step 69b: offline delete reported over Kafka, etcd key kept" \
            "etcd-seeded config for user ${_P17_UID} was not mirrored into the snapshot within 30s"
        _cleanup_phase17_etcd_offline_queue
        return
    fi

    local _p17b_kafka_baseline
    _p17b_kafka_baseline=$(ssh_node "timeout 10 kcat -b ${_p17_brokers} -t fastrg.node.events -C -e -f '%o\n' -o -1 2>/dev/null | tail -1" || true)
    [[ "$_p17b_kafka_baseline" =~ ^[0-9]+$ ]] || _p17b_kafka_baseline=-1

    info "Step 69b: blocking node->etcd and issuing offline RemoveConfig..."
    _p17_block_etcd
    local _p17b_removed=0 _p17b_out=""
    for _i in $(seq 1 45); do
        sleep 2
        _p17b_out=$(ssh_node "grpcurl -plaintext -import-path /root/fastrg-node/northbound/grpc -proto fastrg_node.proto \
            -d '{\"user_id\":${_P17_UID}}' 127.0.0.1:${FASTRG_GRPC_PORT} fastrgnodeservice.FastrgService/RemoveConfig 2>&1" || true)
        if ! printf '%s' "$_p17b_out" | grep -qi "FailedPrecondition"; then
            _p17b_removed=1
            info "  ${_i}x2s: RemoveConfig no longer rejected by SDN guard"
            break
        fi
        info "  ${_i}x2s: still SDN-guard-rejected"
    done

    local _p17b_issue=""
    if [[ $_p17b_removed -eq 0 ]]; then
        _p17b_issue="SDN guard never released within 90s"
    elif ! printf '%s' "$_p17b_out" | grep -qi "removal successful"; then
        _p17b_issue="RemoveConfig accepted but did not report success: $(printf '%s' "$_p17b_out" | tr '\n' '|' | tail -c 160)"
    else
        # Snapshot must now hold a dirty tombstone: exists=false, dirty=true.
        _p17_entry=$(_p17_snapshot_entry)
        local _p17b_exists _p17b_dirty
        _p17b_exists=$(printf '%s' "$_p17_entry" | jq -r '.exists' 2>/dev/null || true)
        _p17b_dirty=$(printf '%s' "$_p17_entry" | jq -r '.dirty' 2>/dev/null || true)
        if [[ "$_p17b_exists" != "false" ]] || [[ "$_p17b_dirty" != "true" ]]; then
            _p17b_issue="snapshot tombstone missing (exists='${_p17b_exists:-none}' dirty='${_p17b_dirty:-none}')"
        fi
    fi

    info "Step 69b: restoring connectivity and waiting for the delete proposal..."
    _p17_unblock_etcd
    if [[ -z "$_p17b_issue" ]]; then
        local _p17b_synced=0
        for _i in $(seq 1 60); do
            sleep 2
            _p17_entry=$(_p17_snapshot_entry)
            if [[ "$(printf '%s' "$_p17_entry" | jq -r '.dirty' 2>/dev/null)" == "false" ]]; then
                _p17b_synced=1
                info "  ${_i}x2s: tombstone no longer dirty (delete proposal reported)"
                break
            fi
            info "  ${_i}x2s: tombstone still dirty"
        done
        [[ $_p17b_synced -eq 1 ]] || _p17b_issue="tombstone dirty flag never cleared within 120s of restoring connectivity"

        # Arbitration outcome: the controller's arbitration
        # consumer is deployed. In this scenario
        # (rv_node == rv_etcd, edited_at > updatedAt) the proposal arbitrates
        # to DELETE, executed by the CONTROLLER (the node stays read-only).
        local _p17b_deleted=0
        for _i in $(seq 1 30); do
            if [[ -z "$(etcdctl_get_value "$_p17b_key" 2>/dev/null || true)" ]]; then
                _p17b_deleted=1
                info "  ${_i}x2s: etcd key deleted by controller arbitration"
                break
            fi
            sleep 2
        done
        if [[ $_p17b_deleted -eq 0 ]]; then
            _p17b_issue="${_p17b_issue:+${_p17b_issue}; }etcd key not deleted within 60s (arbitration should accept: rv_node==rv_etcd, edited_at newer)"
        else
            # The controller's delete comes back on the watch: the snapshot
            # entry must converge to a clean deletion (exists=false; dirty was
            # already cleared by the report).
            local _p17b_conv=0
            for _i in $(seq 1 10); do
                _p17_entry=$(_p17_snapshot_entry)
                if [[ "$(printf '%s' "$_p17_entry" | jq -r '.exists' 2>/dev/null)" == "false" ]] && \
                   [[ "$(printf '%s' "$_p17_entry" | jq -r '.dirty' 2>/dev/null)" == "false" ]]; then
                    _p17b_conv=1
                    break
                fi
                sleep 2
            done
            [[ $_p17b_conv -eq 1 ]] || \
                _p17b_issue="${_p17b_issue:+${_p17b_issue}; }snapshot did not converge to a clean deletion after the watch DELETE"
        fi

        # A delete proposal must be on the topic after the baseline: the
        # tombstone carries the accumulated summary ending in "remove config".
        local _p17b_kafka_out
        _p17b_kafka_out=$(ssh_node "timeout 20 kcat -b ${_p17_brokers} -t fastrg.node.events -C -o $(( _p17b_kafka_baseline + 1 )) -e -q 2>/dev/null | strings" || true)
        if ! printf '%s' "$_p17b_kafka_out" | grep -q "remove config"; then
            _p17b_issue="${_p17b_issue:+${_p17b_issue}; }no delete proposal observed on Kafka after offset ${_p17b_kafka_baseline} (marker 'remove config' missing)"
        fi
    fi

    if [[ -z "$_p17b_issue" ]]; then
        pass "Step 69b: offline delete arbitrated end-to-end" \
            "tombstone reported (deleted=true) and cleared; controller arbitration deleted the etcd key; snapshot converged via watch"
    else
        fail "Step 69b: offline delete arbitrated end-to-end" "$_p17b_issue"
    fi

    _cleanup_phase17_etcd_offline_queue
}
