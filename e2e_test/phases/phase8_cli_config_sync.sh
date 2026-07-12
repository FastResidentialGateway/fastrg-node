#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 8 — CLI Add Config → etcd → Local Sync (Steps 26-34)
#
# Fills the coverage gaps left by Phases 2/3.5/7:
#   - Steps 26/27: SetSnatConfig / RemoveSnatConfig at runtime
#                  (Phase 2 only validates pre-existing port-mappings; Phase 5
#                   only exercises data-plane DNAT)
#   - Step 29   : ApplyConfig upsert with mutated DHCP pool/subnet/gateway
#                  (Phase 7 only tests fresh ApplyConfig on a new subscriber)
#   - Step 30   : sanity that unrelated fields (vlan_id) survive Step 29
#   - Step 31   : RemoveConfig promoted to graded step
#                  (Phase 7 runs it silently in cleanup)
#   - Steps 32-35: PPPoE desire_status lifecycle (connect/disconnect + node reconcile)
#                  desire_status disconnect<->connect; node reconciles session
#
# Steps 26-30 use a synthetic subscriber (_P9_USER_ID = orig_count + 1).
# Steps 31-34 use USER_ID (the real subscriber with a live PPPoE server).
# ---------------------------------------------------------------------------

# Helper: remove the Phase 8 synthetic subscriber + restore subscriber count.
# Idempotent. Called at end of phase9 AND from cleanup_fastrg trap.
_cleanup_phase9_user() {
    [[ -z "${NODE_UUID:-}" ]] && return
    [[ -z "${_P9_USER_ID:-}" ]] && return
    info "Cleanup(phase8): removing user ${_P9_USER_ID} config with verification..."
    remove_hsi_config_verified "${_P9_USER_ID}" || true
    if [[ -n "${_P9_ORIG_SUB_COUNT:-}" ]]; then
        info "Cleanup(phase8): restoring subscriber count to ${_P9_ORIG_SUB_COUNT}..."
        fastrg_grpc set_subscriber_count "${_P9_ORIG_SUB_COUNT}" >/dev/null 2>&1 || true
        local _count_ok=0
        local _count_json _count_now _i
        for _i in $(seq 1 5); do
            _count_json=$(fastrg_grpc get_system_info 2>/dev/null || true)
            _count_now=$(printf '%s' "$_count_json" | jq -r '.num_users // empty' 2>/dev/null || true)
            if [[ "$_count_now" == "$_P9_ORIG_SUB_COUNT" ]]; then
                _count_ok=1
                break
            fi
            sleep 1
        done
        [[ $_count_ok -eq 0 ]] && \
            warn "Cleanup(phase8): subscriber count restore not observed (wanted ${_P9_ORIG_SUB_COUNT}, got ${_count_now:-empty})."
    fi
    return 0
}

# Helper: read .config.desire_status for a user from etcd. Empty on error.
_p9_etcd_desire() {
    local _uid="$1"
    local _json
    _json=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_uid}" 2>/dev/null || true)
    [[ -z "$_json" ]] && return
    printf '%s' "$_json" | jq -r '.config.desire_status // empty' 2>/dev/null || true
}

# Helper: read a user's current PPPoE phase from the node (gRPC). Empty on error.
_p9_node_phase() {
    local _uid="$1"
    fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[] | select(.user_id == ${_uid}) | .status" 2>/dev/null || true
}

# Helper: poll the node PPPoE phase until it matches regex $2, or timeout.
# $1=user_id $2=phase regex $3=iterations $4=sleep seconds. Returns 0 if matched.
_p9_poll_node_phase() {
    local _uid="$1" _match="$2" _iters="$3" _sleep="$4"
    local _i _val
    for _i in $(seq 1 "$_iters"); do
        _val=$(_p9_node_phase "$_uid")
        if [[ "$_val" =~ ${_match} ]]; then
            return 0
        fi
        sleep "$_sleep"
    done
    return 1
}

phase8_cli_config_sync() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 8 — CLI Config → etcd → Local Sync (Steps 26-34)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Determine subscriber count and derive synthetic test user
    # ------------------------------------------------------------------
    info "Determining current subscriber count for Phase 8..."
    _sc_sys=$(fastrg_grpc get_system_info)
    _P9_ORIG_SUB_COUNT=$(printf '%s' "$_sc_sys" | jq -r '.num_users // 0' 2>/dev/null || echo 0)
    _P9_ORIG_SUB_COUNT=$(( ${_P9_ORIG_SUB_COUNT:-0} + 0 ))
    if [[ $_P9_ORIG_SUB_COUNT -eq 0 ]]; then
        warn "Cannot determine subscriber count — skipping Phase 8"
        skip "Step 27: Add port forwarding"        "subscriber count unknown"
        skip "Step 28: Remove port forwarding"     "subscriber count unknown"
        skip "Step 29: Change HSI DHCP config"     "subscriber count unknown"
        skip "Step 30: vlan_id unchanged"          "subscriber count unknown"
        skip "Step 31: Remove HSI config"          "subscriber count unknown"
        skip "Step 32: HangupPPPoE → desire_status=disconnect"  "subscriber count unknown"
        skip "Step 33: node PPPoE torn down"   "subscriber count unknown"
        skip "Step 34: DialPPPoE → desire_status=connect"      "subscriber count unknown"
        skip "Step 35: node PPPoE re-established"       "subscriber count unknown"
        return
    fi
    _P9_USER_ID=$(( _P9_ORIG_SUB_COUNT + 1 ))
    info "Subscriber count: ${_P9_ORIG_SUB_COUNT} → Phase 8 test user: ${_P9_USER_ID}"

    # ------------------------------------------------------------------
    # Derive baseline config from subscriber 1
    # ------------------------------------------------------------------
    info "Reading subscriber ${USER_ID} config from etcd as baseline..."
    _p9_s1=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
    if [[ -z "$_p9_s1" ]]; then
        warn "Subscriber ${USER_ID} config not found in etcd — skipping Phase 8"
        skip "Step 27: Add port forwarding"        "no baseline config"
        skip "Step 28: Remove port forwarding"     "no baseline config"
        skip "Step 29: Change HSI DHCP config"     "no baseline config"
        skip "Step 30: vlan_id unchanged"          "no baseline config"
        skip "Step 31: Remove HSI config"          "no baseline config"
        skip "Step 32: HangupPPPoE → desire_status=disconnect"  "no baseline config"
        skip "Step 33: node PPPoE torn down"   "no baseline config"
        skip "Step 34: DialPPPoE → desire_status=connect"      "no baseline config"
        skip "Step 35: node PPPoE re-established"       "no baseline config"
        return
    fi

    local P9_VLAN=200
    local P9_ACCOUNT P9_PASSWORD P9_POOL P9_POOL_START P9_POOL_END P9_SUBNET P9_GATEWAY
    P9_ACCOUNT=$(printf '%s'    "$_p9_s1" | jq -r '.config.account_name // empty')
    P9_PASSWORD=$(printf '%s'   "$_p9_s1" | jq -r '.config.password // empty')
    P9_POOL=$(printf '%s'       "$_p9_s1" | jq -r '.config.dhcp_addr_pool // empty')
    P9_POOL_START=$(printf '%s' "$P9_POOL" | awk -F'[-~]' '{print $1}')
    P9_POOL_END=$(printf '%s'   "$P9_POOL" | awk -F'[-~]' '{print $2}')
    P9_SUBNET=$(printf '%s'     "$_p9_s1" | jq -r '.config.dhcp_subnet // empty')
    P9_GATEWAY=$(printf '%s'    "$_p9_s1" | jq -r '.config.dhcp_gateway // empty')

    # Expand subscriber count and apply baseline config for the new user
    info "Expanding subscriber count to ${_P9_USER_ID} and seeding baseline config..."
    local _sc_result
    _sc_result=$(fastrg_grpc set_subscriber_count "${_P9_USER_ID}" 2>&1 || true)
    info "result: ${_sc_result:-<empty>}"
    sleep 2
    fastrg_grpc apply_config \
        "${_P9_USER_ID}" "${P9_VLAN}" "${P9_ACCOUNT}" "${P9_PASSWORD}" \
        "${P9_POOL_START}" "${P9_POOL_END}" "${P9_SUBNET}" "${P9_GATEWAY}" \
        >/dev/null 2>&1 || true
    sleep 2

    _p9_baseline=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P9_USER_ID}" 2>/dev/null || true)
    if [[ -z "$_p9_baseline" ]]; then
        warn "Baseline ApplyConfig for user ${_P9_USER_ID} did not land in etcd — skipping Phase 8"
        skip "Step 27: Add port forwarding"        "baseline ApplyConfig failed"
        skip "Step 28: Remove port forwarding"     "baseline ApplyConfig failed"
        skip "Step 29: Change HSI DHCP config"     "baseline ApplyConfig failed"
        skip "Step 30: vlan_id unchanged"          "baseline ApplyConfig failed"
        skip "Step 31: Remove HSI config"          "baseline ApplyConfig failed"
        skip "Step 32: HangupPPPoE → desire_status=disconnect"  "baseline ApplyConfig failed"
        skip "Step 33: node PPPoE torn down"   "baseline ApplyConfig failed"
        skip "Step 34: DialPPPoE → desire_status=connect"      "baseline ApplyConfig failed"
        skip "Step 35: node PPPoE re-established"       "baseline ApplyConfig failed"
        _cleanup_phase9_user
        return
    fi

    # ------------------------------------------------------------------
    # Step 27 — Add port forwarding (SetSnatConfig)
    # ------------------------------------------------------------------
    local P9_EPORT=18888
    local P9_DIP="192.168.4.20"
    local P9_IPORT=8080

    info "Step 27: SetSnatConfig user ${_P9_USER_ID} eport=${P9_EPORT} dip=${P9_DIP} iport=${P9_IPORT}..."
    _p9_snat_add=$(fastrg_grpc set_snat_config "${_P9_USER_ID}" "${P9_EPORT}" "${P9_DIP}" "${P9_IPORT}")
    _p9_snat_add_status=$(printf '%s' "$_p9_snat_add" | jq -r '.status // empty' 2>/dev/null || true)

    if [[ -z "$_p9_snat_add_status" ]]; then
        fail "Step 27: Add port forwarding" \
            "gRPC SetSnatConfig returned no status — response: $(printf '%s' "$_p9_snat_add")"
        skip "Step 28: Remove port forwarding" "SetSnatConfig failed"
    else
        sleep 2
        _p9_etcd_pm=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P9_USER_ID}" 2>/dev/null || true)
        # etcd field is "dport" (matches input iport); proto field is "iport"
        _p9_etcd_entry=$(printf '%s' "$_p9_etcd_pm" | \
            jq -r ".config[\"port-mapping\"][]? | select(.eport == \"${P9_EPORT}\")" 2>/dev/null || true)
        _p9_grpc_pf=$(fastrg_grpc get_port_fwd_info "${_P9_USER_ID}")
        _p9_grpc_entry=$(printf '%s' "$_p9_grpc_pf" | \
            jq -r ".entries[]? | select(.eport == ${P9_EPORT})" 2>/dev/null || true)

        MISMATCH=""
        if [[ -z "$_p9_etcd_entry" ]]; then
            MISMATCH="${MISMATCH} etcd-entry-missing"
        else
            _p9_e_dip=$(printf '%s'   "$_p9_etcd_entry" | jq -r '.dip // empty')
            _p9_e_dport=$(printf '%s' "$_p9_etcd_entry" | jq -r '.dport // empty')
            [[ "$_p9_e_dip"   != "$P9_DIP"          ]] && MISMATCH="${MISMATCH} etcd-dip(got=${_p9_e_dip} expected=${P9_DIP})"
            [[ "$_p9_e_dport" != "${P9_IPORT}"      ]] && MISMATCH="${MISMATCH} etcd-dport(got=${_p9_e_dport} expected=${P9_IPORT})"
        fi
        if [[ -z "$_p9_grpc_entry" ]]; then
            MISMATCH="${MISMATCH} grpc-entry-missing"
        fi

        if [[ -z "$MISMATCH" ]]; then
            pass "Step 27: Add port forwarding" \
                "etcd dip=${_p9_e_dip} dport=${_p9_e_dport}; grpc entry present"
        else
            fail "Step 27: Add port forwarding" "Mismatch:${MISMATCH}"
        fi

        # ------------------------------------------------------------------
        # Step 28 — Remove port forwarding (RemoveSnatConfig)
        # ------------------------------------------------------------------
        info "Step 28: RemoveSnatConfig user ${_P9_USER_ID} eport=${P9_EPORT}..."
        _p9_snat_del=$(fastrg_grpc remove_snat_config "${_P9_USER_ID}" "${P9_EPORT}")
        _p9_snat_del_status=$(printf '%s' "$_p9_snat_del" | jq -r '.status // empty' 2>/dev/null || true)

        if [[ -z "$_p9_snat_del_status" ]]; then
            fail "Step 28: Remove port forwarding" \
                "gRPC RemoveSnatConfig returned no status — response: $(printf '%s' "$_p9_snat_del")"
        else
            sleep 2
            _p9_etcd_pm2=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P9_USER_ID}" 2>/dev/null || true)
            _p9_etcd_entry2=$(printf '%s' "$_p9_etcd_pm2" | \
                jq -r ".config[\"port-mapping\"][]? | select(.eport == \"${P9_EPORT}\")" 2>/dev/null || true)
            _p9_grpc_pf2=$(fastrg_grpc get_port_fwd_info "${_P9_USER_ID}")
            _p9_grpc_entry2=$(printf '%s' "$_p9_grpc_pf2" | \
                jq -r ".entries[]? | select(.eport == ${P9_EPORT})" 2>/dev/null || true)

            MISMATCH=""
            [[ -n "$_p9_etcd_entry2" ]] && MISMATCH="${MISMATCH} etcd-entry-still-present"
            [[ -n "$_p9_grpc_entry2" ]] && MISMATCH="${MISMATCH} grpc-entry-still-present"

            if [[ -z "$MISMATCH" ]]; then
                pass "Step 28: Remove port forwarding" "etcd + grpc no longer contain eport=${P9_EPORT}"
            else
                fail "Step 28: Remove port forwarding" "Mismatch:${MISMATCH}"
            fi
        fi
    fi

    # ------------------------------------------------------------------
    # Step 29 — Change HSI DHCP config (re-ApplyConfig with new pool)
    # ------------------------------------------------------------------
    local P9_NEW_POOL_START="10.99.1.10"
    local P9_NEW_POOL_END="10.99.1.50"
    local P9_NEW_SUBNET="255.255.255.0"
    local P9_NEW_GATEWAY="10.99.1.1"

    info "Step 29: Re-ApplyConfig user ${_P9_USER_ID} with new DHCP pool=${P9_NEW_POOL_START}-${P9_NEW_POOL_END} gw=${P9_NEW_GATEWAY}..."
    _p9_chg=$(fastrg_grpc apply_config \
        "${_P9_USER_ID}" "${P9_VLAN}" "${P9_ACCOUNT}" "${P9_PASSWORD}" \
        "${P9_NEW_POOL_START}" "${P9_NEW_POOL_END}" "${P9_NEW_SUBNET}" "${P9_NEW_GATEWAY}")
    _p9_chg_status=$(printf '%s' "$_p9_chg" | jq -r '.status // empty' 2>/dev/null || true)
    sleep 1

    if [[ -z "$_p9_chg_status" ]]; then
        fail "Step 29: Change HSI DHCP config" \
            "gRPC ApplyConfig returned no status — response: $(printf '%s' "$_p9_chg")"
        skip "Step 30: vlan_id unchanged" "Change HSI failed"
    else
        sleep 1
        _p9_chg_etcd=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P9_USER_ID}" 2>/dev/null || true)
        _p9_chg_e_pool=$(printf '%s'   "$_p9_chg_etcd" | jq -r '.config.dhcp_addr_pool // empty')
        _p9_chg_e_subnet=$(printf '%s' "$_p9_chg_etcd" | jq -r '.config.dhcp_subnet // empty')
        _p9_chg_e_gw=$(printf '%s'     "$_p9_chg_etcd" | jq -r '.config.dhcp_gateway // empty')
        _p9_expect_pool="${P9_NEW_POOL_START}-${P9_NEW_POOL_END}"

        _p9_chg_grpc=$(fastrg_grpc get_dhcp_info)
        _p9_chg_g_user=$(printf '%s' "$_p9_chg_grpc" | \
            jq -r ".dhcp_infos[] | select(.user_id == ${_P9_USER_ID})" 2>/dev/null || true)
        _p9_chg_g_range=$(printf '%s'  "$_p9_chg_g_user" | jq -r '.ip_range // empty')
        _p9_chg_g_subnet=$(printf '%s' "$_p9_chg_g_user" | jq -r '.subnet_mask // empty')
        _p9_chg_g_gw=$(printf '%s'     "$_p9_chg_g_user" | jq -r '.gateway // empty')

        # Normalize gRPC pool ("start - end") and etcd pool ("start-end") to "start~end"
        _p9_chg_g_norm=$(printf '%s' "$_p9_chg_g_range" | tr -d ' ' | tr '-' '~' | sed 's/~~/~/')
        _p9_chg_expect_norm="${P9_NEW_POOL_START}~${P9_NEW_POOL_END}"

        MISMATCH=""
        [[ "$_p9_chg_e_pool"   != "$_p9_expect_pool"     ]] && MISMATCH="${MISMATCH} etcd-pool(got=${_p9_chg_e_pool} expected=${_p9_expect_pool})"
        [[ "$_p9_chg_e_subnet" != "$P9_NEW_SUBNET"        ]] && MISMATCH="${MISMATCH} etcd-subnet(got=${_p9_chg_e_subnet} expected=${P9_NEW_SUBNET})"
        [[ "$_p9_chg_e_gw"     != "$P9_NEW_GATEWAY"       ]] && MISMATCH="${MISMATCH} etcd-gw(got=${_p9_chg_e_gw} expected=${P9_NEW_GATEWAY})"
        [[ "$_p9_chg_g_norm"   != "$_p9_chg_expect_norm" ]] && MISMATCH="${MISMATCH} grpc-pool(got=${_p9_chg_g_range} expected=${_p9_chg_expect_norm})"
        [[ "$_p9_chg_g_subnet" != "$P9_NEW_SUBNET"        ]] && MISMATCH="${MISMATCH} grpc-subnet(got=${_p9_chg_g_subnet} expected=${P9_NEW_SUBNET})"
        [[ "$_p9_chg_g_gw"     != "$P9_NEW_GATEWAY"       ]] && MISMATCH="${MISMATCH} grpc-gw(got=${_p9_chg_g_gw} expected=${P9_NEW_GATEWAY})"

        if [[ -z "$MISMATCH" ]]; then
            pass "Step 29: Change HSI DHCP config" \
                "pool=${_p9_chg_e_pool} subnet=${_p9_chg_e_subnet} gw=${_p9_chg_e_gw}"
        else
            fail "Step 29: Change HSI DHCP config" "Mismatch:${MISMATCH}"
        fi

        # ------------------------------------------------------------------
        # Step 30 — vlan_id unchanged after Step 29 (regression guard)
        # ------------------------------------------------------------------
        info "Step 30: Verifying vlan_id=${P9_VLAN} survived the DHCP change..."
        _p9_v_hsi=$(fastrg_grpc get_hsi_info | \
            jq -r ".hsi_infos[] | select(.user_id == ${_P9_USER_ID}) | .vlan_id" 2>/dev/null || true)
        _p9_v_etcd=$(printf '%s' "$_p9_chg_etcd" | jq -r '.config.vlan_id // empty')

        MISMATCH=""
        [[ "$_p9_v_hsi"  != "$P9_VLAN" ]] && MISMATCH="${MISMATCH} grpc-vlan(got=${_p9_v_hsi} expected=${P9_VLAN})"
        [[ "$_p9_v_etcd" != "$P9_VLAN" ]] && MISMATCH="${MISMATCH} etcd-vlan(got=${_p9_v_etcd} expected=${P9_VLAN})"

        if [[ -z "$MISMATCH" ]]; then
            pass "Step 30: vlan_id unchanged" "vlan_id=${P9_VLAN} in both etcd and grpc"
        else
            fail "Step 30: vlan_id unchanged" "Mismatch:${MISMATCH}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 31 — Remove HSI config (RemoveConfig as graded step)
    # ------------------------------------------------------------------
    info "Step 31: RemoveConfig user ${_P9_USER_ID}..."
    _p9_rm=$(fastrg_grpc remove_config "${_P9_USER_ID}")
    _p9_rm_status=$(printf '%s' "$_p9_rm" | jq -r '.status // empty' 2>/dev/null || true)

    if [[ -z "$_p9_rm_status" ]]; then
        fail "Step 31: Remove HSI config" \
            "gRPC RemoveConfig returned no status — response: $(printf '%s' "$_p9_rm")"
    else
        sleep 1
        _p9_rm_etcd=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P9_USER_ID}" 2>/dev/null || true)
        _p9_rm_grpc=$(fastrg_grpc get_hsi_info | \
            jq -r ".hsi_infos[] | select(.user_id == ${_P9_USER_ID}) | .status == notconfigured" 2>/dev/null || true)

        MISMATCH=""
        [[ -n "$_p9_rm_etcd" ]] && MISMATCH="${MISMATCH} etcd-key-still-present"
        [[ -n "$_p9_rm_grpc" ]] && MISMATCH="${MISMATCH} grpc-entry-still-present"

        if [[ -z "$MISMATCH" ]]; then
            pass "Step 31: Remove HSI config" "etcd key deleted; grpc no longer lists user ${_P9_USER_ID}"
        else
            fail "Step 31: Remove HSI config" "Mismatch:${MISMATCH}"
        fi
    fi

    # ------------------------------------------------------------------
    # Steps 32-35 — PPPoE desire_status lifecycle on real subscriber USER_ID
    #
    # desire_status (etcd) flips connect<->disconnect via the controller and is
    # deterministic (Steps 32, 34). The node then reconciles the live PPPoE
    # session toward it; the node phase transition (Steps 33, 35) depends on the
    # remote PPPoE server and may be slow/flaky in the test bench.
    # ------------------------------------------------------------------
    info "Step 32-35 prerequisite: reading current desire_status for USER_ID=${USER_ID}..."
    _p9_start=$(_p9_etcd_desire "${USER_ID}")
    info "  current config.desire_status = '${_p9_start:-<empty>}'"

    if [[ "$_p9_start" != "connect" ]]; then
        warn "USER_ID=${USER_ID} is not 'connect' — dialing to set baseline..."
        fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true
        _p9_poll_node_phase "${USER_ID}" "Data phase" 20 3 || true
    fi

    # ---- Step 32: HangupPPPoE → desire_status="disconnect" (deterministic) ----
    info "Step 32: HangupPPPoE USER_ID=${USER_ID} (controller)..."
    fastrg_grpc disconnect_hsi "${USER_ID}" >/dev/null 2>&1 || true
    sleep 2
    _p9_d=$(_p9_etcd_desire "${USER_ID}")
    if [[ "$_p9_d" == "disconnect" ]]; then
        pass "Step 32: HangupPPPoE → desire_status=disconnect" "config.desire_status=disconnect"
    else
        fail "Step 32: HangupPPPoE → desire_status=disconnect" "got '${_p9_d:-<empty>}'"
    fi

    # ---- Step 33: node tears the PPPoE session down (best-effort vs remote server) ----
    info "Step 33: waiting for node to leave 'Data phase' (remote PPPoE teardown)..."
    if _p9_poll_node_phase "${USER_ID}" "^(End phase|not configured|PPPoE phase|LCP phase)$" 12 5; then
        pass "Step 33: node PPPoE torn down" "node phase = '$(_p9_node_phase "${USER_ID}")'"
    else
        fail "Step 33: node PPPoE torn down" \
            "node still '$(_p9_node_phase "${USER_ID}")' after 60s (remote PPPoE terminate may be unreliable)"
    fi

    # ---- Step 34: DialPPPoE → desire_status="connect" (deterministic) ----
    info "Step 34: DialPPPoE USER_ID=${USER_ID} (controller)..."
    fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true
    sleep 2
    _p9_c=$(_p9_etcd_desire "${USER_ID}")
    if [[ "$_p9_c" == "connect" ]]; then
        pass "Step 34: DialPPPoE → desire_status=connect" "config.desire_status=connect"
    else
        fail "Step 34: DialPPPoE → desire_status=connect" "got '${_p9_c:-<empty>}'"
    fi

    # ---- Step 35: node re-establishes the PPPoE session (best-effort) ----
    info "Step 35: waiting for node to reach 'Data phase'..."
    if _p9_poll_node_phase "${USER_ID}" "Data phase" 14 5; then
        pass "Step 35: node PPPoE re-established" "node phase = 'Data phase'"
    else
        fail "Step 35: node PPPoE re-established" \
            "node still '$(_p9_node_phase "${USER_ID}")' after 70s (remote PPPoE server may be slow)"
    fi

    # ------------------------------------------------------------------
    # Cleanup — remove synthetic user + restore subscriber count
    # ------------------------------------------------------------------
    _cleanup_phase9_user
}
