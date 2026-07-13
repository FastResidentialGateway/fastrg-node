#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 12 — Config-Apply Failure → Controller Rollback (Steps 43-44)
#
# Verifies the end-to-end rollback pipeline:
#   node apply failure → Kafka CONFIG_APPLY_FAIL
#   → controller reads last-successful config from DB (hsi_config_current)
#   → CAS writes it back to etcd
#   → node applies the restored config successfully (no infinite loop)
#
#   Step 43  existing user: bad config rolled back to last-successful version
#   Step 44  new user (no DB record): bad key removed from etcd on failure
# ---------------------------------------------------------------------------

# Poll etcd until the key's vlan_id matches $1 or $2 max-seconds elapse.
_p13_wait_etcd_vlan() {
    local user_id="$1" expect_vlan="$2" max_sec="${3:-20}"
    local iters=$(( max_sec / 2 ))
    for _i in $(seq 1 "$iters"); do
        sleep 2
        local v
        v=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${user_id}" 2>/dev/null || true)
        local vlan
        vlan=$(printf '%s' "$v" | jq -r '.config.vlan_id // empty' 2>/dev/null || true)
        local acct
        acct=$(printf '%s' "$v" | jq -r '.config.account_name // empty' 2>/dev/null || true)
        local by
        by=$(printf '%s' "$v" | jq -r '.metadata.updatedBy // empty' 2>/dev/null || true)
        info "  ${_i}x2s: vlan=${vlan} acct=${acct} by=${by}" >&2
        [[ "$vlan" == "$expect_vlan" ]] && { echo "$acct|$by"; return 0; }
    done
    return 1
}

# Helper: this phase deliberately writes an invalid vlan_id=0 config into
# USER_ID's canonical hsi fixture (Step 43) and a stray never-configured-user
# key (Step 44) to trigger controller rollback. Both are normally cleaned up
# inline once each step's assertions finish, but if the phase dies mid-step
# (before the controller rolls back / before the inline stray-key delete),
# the bad state would otherwise survive into the next run. Idempotent —
# checks current etcd state before acting. Called at end of phase12 AND from
# the cleanup_fastrg EXIT trap.
_cleanup_phase12_rollback() {
    [[ -z "${NODE_UUID:-}" ]] && return
    if [[ -n "${_P12_NEW_UID:-}" ]]; then
        local _chk
        _chk=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P12_NEW_UID}" 2>/dev/null || true)
        if [[ -n "$_chk" ]]; then
            info "Cleanup(phase12): removing stray test key hsi/${_P12_NEW_UID}..."
            ssh_node "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} del configs/${NODE_UUID}/hsi/${_P12_NEW_UID}" \
                >/dev/null 2>&1 || true
        fi
    fi
    if [[ -n "${_P12_ORIG_VLAN:-}" ]]; then
        local _cur_vlan
        _cur_vlan=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null | \
            jq -r '.config.vlan_id // empty' 2>/dev/null || true)
        if [[ "$_cur_vlan" != "${_P12_ORIG_VLAN}" ]]; then
            info "Cleanup(phase12): hsi/${USER_ID} vlan=${_cur_vlan} (expected ${_P12_ORIG_VLAN}); restoring canonical fixture..."
            ssh_node "bash /root/fastrg-node/e2e_test/restore_etcd_config.sh --force" >/dev/null 2>&1 || true
        fi
    fi
}

phase12_rollback() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 12 — Config-Apply Failure + Rollback (Steps 43-44)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 43 — existing user: bad config → apply fail → controller
    #           reads last-successful from DB → CAS restores etcd
    # ------------------------------------------------------------------
    info "Step 43: trigger apply failure for USER_ID=${USER_ID} and expect rollback..."

    # First ensure the controller DB has a recent apply-OK record for USER_ID.
    # A dial+hangup cycle sends a CONFIG_APPLY_OK which seeds hsi_config_current.
    local _p13_cur_acct
    _p13_cur_acct=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null | \
        jq -r '.config.account_name // empty' 2>/dev/null || true)
    local _p13_cur_vlan
    _p13_cur_vlan=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null | \
        jq -r '.config.vlan_id // empty' 2>/dev/null || true)
    info "  current etcd: acct=${_p13_cur_acct} vlan=${_p13_cur_vlan}"
    # Not `local`: arms _cleanup_phase12_rollback (callable from the
    # cleanup_fastrg EXIT trap) before the bad config is written below.
    _P12_ORIG_VLAN="$_p13_cur_vlan"

    # Write an invalid config (vlan_id=0 is always rejected by the node).
    local _p13_now
    _p13_now=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    local _p13_bad
    _p13_bad=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null | \
        jq -c --arg now "$_p13_now" \
            '.config.vlan_id="0" | .config.account_name="e2e_bad_vlan" |
             .metadata.resourceVersion=(((.metadata.resourceVersion | tonumber) + 1) | tostring) |
             .metadata.updatedAt=$now | .metadata.updatedBy="e2e_rollback_test"' \
        2>/dev/null || true)
    if [[ -z "$_p13_bad" ]]; then
        fail "Step 43: config-apply rollback" "could not construct bad config from etcd"
        skip "Step 44: new-user rollback (delete)" "prerequisite failed"
        return
    fi
    # Pipe the JSON value via stdin to avoid shell quoting issues with the JSON.
    printf '%s' "$_p13_bad" | ssh_node \
        "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} put configs/${NODE_UUID}/hsi/${USER_ID}" \
        >/dev/null 2>&1 || true
    info "  bad config (vlan=0) written — waiting for rollback..."

    local _p43_acct_by
    if _p43_acct_by=$(_p13_wait_etcd_vlan "${USER_ID}" "${_p13_cur_vlan}" 30 2>/dev/null); then
        local _rb_acct="${_p43_acct_by%%|*}"
        local _rb_by="${_p43_acct_by##*|}"
        if [[ "$_rb_acct" == "$_p13_cur_acct" ]]; then
            pass "Step 43: config-apply rollback" \
                "etcd restored to vlan=${_p13_cur_vlan} acct=${_rb_acct} by=${_rb_by}"
        else
            fail "Step 43: config-apply rollback" \
                "vlan restored but acct mismatch (got=${_rb_acct} want=${_p13_cur_acct})"
        fi
    else
        fail "Step 43: config-apply rollback" \
            "etcd was not restored within 30s (vlan still 0 or controller not rolling back)"
    fi

    # ------------------------------------------------------------------
    # Step 44 — new user with no DB record: bad key should be removed
    #           (controller cannot restore a prior version → deletes it)
    # ------------------------------------------------------------------
    info "Step 44: write bad config for a brand-new user (no DB record) — expect key deleted..."

    # Pick a user_id one beyond the current subscriber count (guaranteed unconfigured).
    local _sc
    _sc=$(fastrg_grpc get_system_info 2>/dev/null | jq -r '.num_users // 0' 2>/dev/null || echo 0)
    local _new_uid=$(( _sc + 50 ))   # well beyond count so apply definitely fails
    # Not `local`: arms _cleanup_phase12_rollback's stray-key delete before
    # the bad config for this user is written below.
    _P12_NEW_UID="$_new_uid"
    local _p13_bad2
    _p13_bad2=$(printf \
        '{"config":{"account_name":"e2e_new_bad","dhcp_addr_pool":"10.99.0.2-10.99.0.10","dhcp_gateway":"10.99.0.1","dhcp_subnet":"255.255.255.0","password":"x","user_id":"%s","vlan_id":"199","desire_status":"connect"},"metadata":{"node":"%s","resourceVersion":"1","updatedAt":"%s","updatedBy":"e2e_rollback_test"}}' \
        "${_new_uid}" "${NODE_UUID}" "$(date -u +"%Y-%m-%dT%H:%M:%SZ")")

    printf '%s' "$_p13_bad2" | ssh_node \
        "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} put configs/${NODE_UUID}/hsi/${_new_uid}" \
        >/dev/null 2>&1 || true
    info "  bad config for new user ${_new_uid} written — waiting for deletion..."

    local _p44_ok=0
    for _i in $(seq 1 15); do
        sleep 2
        local _v
        _v=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_new_uid}" 2>/dev/null || true)
        if [[ -z "$_v" ]]; then
            _p44_ok=1
            info "  key deleted after ${_i}x2s"
            break
        fi
        info "  ${_i}x2s: key still exists"
    done

    if [[ $_p44_ok -eq 1 ]]; then
        pass "Step 44: new-user rollback (delete)" \
            "etcd key for new user ${_new_uid} was deleted after apply failure"
    else
        # Some controllers may only rollback existing users; treat as a soft failure.
        local _remaining
        _remaining=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_new_uid}" 2>/dev/null | \
            jq -r '.config.account_name // empty' 2>/dev/null || true)
        fail "Step 44: new-user rollback (delete)" \
            "key not removed within 30s (acct=${_remaining}; controller may not delete new-user keys)"
        # Clean up the stray key so it doesn't affect later phases.
        ssh_node "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} del configs/${NODE_UUID}/hsi/${_new_uid}" \
            >/dev/null 2>&1 || true
    fi

    # Unconditional cleanup: Step 43 wrote vlan=0 into configs/.../hsi/USER_ID,
    # and Step 44 may have left a stray key if its own fail-branch delete above
    # didn't run. Either way, force-restore/delete so subsequent phases (diff,
    # kafka, summary) and the NEXT run all see the correct state.
    _cleanup_phase12_rollback
}
