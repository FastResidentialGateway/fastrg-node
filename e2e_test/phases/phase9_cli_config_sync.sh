#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 9 — CLI Add Config → etcd → Local Sync (Steps 26-34)
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
#   - Steps 31-34: full PPPoE enableStatus lifecycle observation
#                  enabled → disabling → disabled → enabling → enabled
#                  (Phase 3.5 only verifies the "enabled" terminal state)
#
# Steps 26-30 use a synthetic subscriber (_P9_USER_ID = orig_count + 1).
# Steps 31-34 use USER_ID (the real subscriber with a live PPPoE server).
# ---------------------------------------------------------------------------

# Helper: remove the Phase 9 synthetic subscriber + restore subscriber count.
# Idempotent. Called at end of phase9 AND from cleanup_fastrg trap.
_cleanup_phase9_user() {
    [[ -z "${NODE_UUID:-}" ]] && return
    [[ -z "${_P9_USER_ID:-}" ]] && return
    local _chk
    _chk=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P9_USER_ID}" 2>/dev/null || true)
    if [[ -n "$_chk" ]]; then
        info "Cleanup(phase9): removing user ${_P9_USER_ID} config (RemoveConfig gRPC)..."
        fastrg_grpc remove_config "${_P9_USER_ID}" >/dev/null 2>&1 || true
        sleep 1
    fi
    if [[ -n "${_P9_ORIG_SUB_COUNT:-}" ]]; then
        info "Cleanup(phase9): restoring subscriber count to ${_P9_ORIG_SUB_COUNT}..."
        fastrg_grpc set_subscriber_count "${_P9_ORIG_SUB_COUNT}" >/dev/null 2>&1 || true
    fi
}

# Helper: read .metadata.enableStatus for a user from etcd. Empty on error.
_p9_etcd_enable_status() {
    local _uid="$1"
    local _json
    _json=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_uid}" 2>/dev/null || true)
    [[ -z "$_json" ]] && return
    printf '%s' "$_json" | jq -r '.metadata.enableStatus // empty' 2>/dev/null || true
}

# Helper: poll _p9_etcd_enable_status until value matches $2 (regex), or timeout.
# Records every distinct value observed in _P9_SEEN (caller resets it).
# $1 = user_id, $2 = regex of acceptable values, $3 = iterations, $4 = sleep seconds
# Returns 0 if matched, 1 if timeout.
_p9_poll_enable_status() {
    local _uid="$1" _match="$2" _iters="$3" _sleep="$4"
    local _i _val _last=""
    for _i in $(seq 1 "$_iters"); do
        _val=$(_p9_etcd_enable_status "$_uid")
        if [[ -n "$_val" && "$_val" != "$_last" ]]; then
            _P9_SEEN="${_P9_SEEN}${_P9_SEEN:+,}${_val}"
            _last="$_val"
        fi
        if [[ "$_val" =~ ^${_match}$ ]]; then
            return 0
        fi
        sleep "$_sleep"
    done
    return 1
}

# Helper: start a background `etcdctl watch` on the HSI key for $1 (user_id).
# Captures every revision (including transient states) into a log on the FastRG node.
# Caller must invoke _p9_watch_stop_and_read later to terminate and read the log.
# Globals set: _P9_WATCH_LOG (path on remote node).
_p9_watch_start() {
    local _uid="$1"
    _P9_WATCH_LOG="/tmp/p9_watch_${_uid}.log"
    ssh_node "rm -f ${_P9_WATCH_LOG}" 2>/dev/null || true
    # ssh -fn: fork after auth (return immediately) + redirect stdin from /dev/null
    # stdbuf -oL: line-buffered stdout so reads see each revision as it arrives
    ssh -fn $SSH_OPTS "root@${FASTRG_NODE}" \
        "ETCDCTL_API=3 stdbuf -oL etcdctl --endpoints=${ETCD_ENDPOINT} watch configs/${NODE_UUID}/hsi/${_uid} > ${_P9_WATCH_LOG} 2>&1" \
        2>/dev/null || true
    sleep 0.5  # let watch register with etcd before caller does the action
}

# Helper: stop the background watch and return its log content on stdout.
_p9_watch_stop_and_read() {
    local _uid="$1"
    sleep 0.3  # allow last revisions to flush to file
    ssh_node "pkill -f 'etcdctl.*watch.*${NODE_UUID}.*hsi/${_uid}'" 2>/dev/null || true
    sleep 0.2
    ssh_node "cat ${_P9_WATCH_LOG} 2>/dev/null; rm -f ${_P9_WATCH_LOG}" 2>/dev/null || true
}

# Helper: extract comma-separated sequence of enableStatus values from watch log.
_p9_extract_status_sequence() {
    printf '%s' "$1" | grep -oE '"enableStatus":"[a-z]+"' | \
        awk -F'"' '{print $4}' | tr '\n' ',' | sed 's/,$//'
}

phase9_cli_config_sync() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 9 — CLI Config → etcd → Local Sync (Steps 26-34)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Determine subscriber count and derive synthetic test user
    # ------------------------------------------------------------------
    info "Determining current subscriber count for Phase 9..."
    _sc_sys=$(fastrg_grpc get_system_info)
    _P9_ORIG_SUB_COUNT=$(printf '%s' "$_sc_sys" | jq -r '.num_users // 0' 2>/dev/null || echo 0)
    _P9_ORIG_SUB_COUNT=$(( ${_P9_ORIG_SUB_COUNT:-0} + 0 ))
    if [[ $_P9_ORIG_SUB_COUNT -eq 0 ]]; then
        warn "Cannot determine subscriber count — skipping Phase 9"
        skip "Step 27: Add port forwarding"        "subscriber count unknown"
        skip "Step 28: Remove port forwarding"     "subscriber count unknown"
        skip "Step 29: Change HSI DHCP config"     "subscriber count unknown"
        skip "Step 30: vlan_id unchanged"          "subscriber count unknown"
        skip "Step 31: Remove HSI config"          "subscriber count unknown"
        skip "Step 32: DisconnectHsi → disabling"  "subscriber count unknown"
        skip "Step 33: DisconnectHsi → disabled"   "subscriber count unknown"
        skip "Step 34: ConnectHsi → enabling"      "subscriber count unknown"
        skip "Step 35: ConnectHsi → enabled"       "subscriber count unknown"
        return
    fi
    _P9_USER_ID=$(( _P9_ORIG_SUB_COUNT + 1 ))
    info "Subscriber count: ${_P9_ORIG_SUB_COUNT} → Phase 9 test user: ${_P9_USER_ID}"

    # ------------------------------------------------------------------
    # Derive baseline config from subscriber 1
    # ------------------------------------------------------------------
    info "Reading subscriber 1 config from etcd as baseline..."
    _p9_s1=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/1" 2>/dev/null || true)
    if [[ -z "$_p9_s1" ]]; then
        warn "Subscriber 1 config not found in etcd — skipping Phase 9"
        skip "Step 27: Add port forwarding"        "no baseline config"
        skip "Step 28: Remove port forwarding"     "no baseline config"
        skip "Step 29: Change HSI DHCP config"     "no baseline config"
        skip "Step 30: vlan_id unchanged"          "no baseline config"
        skip "Step 31: Remove HSI config"          "no baseline config"
        skip "Step 32: DisconnectHsi → disabling"  "no baseline config"
        skip "Step 33: DisconnectHsi → disabled"   "no baseline config"
        skip "Step 34: ConnectHsi → enabling"      "no baseline config"
        skip "Step 35: ConnectHsi → enabled"       "no baseline config"
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
    fastrg_grpc set_subscriber_count "${_P9_USER_ID}" >/dev/null 2>&1 || true
    sleep 2
    fastrg_grpc apply_config \
        "${_P9_USER_ID}" "${P9_VLAN}" "${P9_ACCOUNT}" "${P9_PASSWORD}" \
        "${P9_POOL_START}" "${P9_POOL_END}" "${P9_SUBNET}" "${P9_GATEWAY}" \
        >/dev/null 2>&1 || true
    sleep 2

    _p9_baseline=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P9_USER_ID}" 2>/dev/null || true)
    if [[ -z "$_p9_baseline" ]]; then
        warn "Baseline ApplyConfig for user ${_P9_USER_ID} did not land in etcd — skipping Phase 9"
        skip "Step 27: Add port forwarding"        "baseline ApplyConfig failed"
        skip "Step 28: Remove port forwarding"     "baseline ApplyConfig failed"
        skip "Step 29: Change HSI DHCP config"     "baseline ApplyConfig failed"
        skip "Step 30: vlan_id unchanged"          "baseline ApplyConfig failed"
        skip "Step 31: Remove HSI config"          "baseline ApplyConfig failed"
        skip "Step 32: DisconnectHsi → disabling"  "baseline ApplyConfig failed"
        skip "Step 33: DisconnectHsi → disabled"   "baseline ApplyConfig failed"
        skip "Step 34: ConnectHsi → enabling"      "baseline ApplyConfig failed"
        skip "Step 35: ConnectHsi → enabled"       "baseline ApplyConfig failed"
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
    # Steps 31-34 — PPPoE enableStatus lifecycle on real subscriber USER_ID
    # ------------------------------------------------------------------
    info "Step 32-35 prerequisite: reading current enableStatus for USER_ID=${USER_ID}..."
    _p9_start=$(_p9_etcd_enable_status "${USER_ID}")
    info "  current metadata.enableStatus = '${_p9_start:-<empty>}'"

    if [[ "$_p9_start" != "enabled" ]]; then
        warn "USER_ID=${USER_ID} is not in 'enabled' state — attempting ConnectHsi to set baseline..."
        fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true
        _P9_SEEN=""
        if ! _p9_poll_enable_status "${USER_ID}" "enabled" 30 1; then
            warn "Could not bring USER_ID=${USER_ID} to 'enabled' — skipping Steps 31-34"
            skip "Step 32: DisconnectHsi → disabling" "could not establish 'enabled' baseline (seen=${_P9_SEEN:-none})"
            skip "Step 33: DisconnectHsi → disabled"  "could not establish 'enabled' baseline"
            skip "Step 34: ConnectHsi → enabling"     "could not establish 'enabled' baseline"
            skip "Step 35: ConnectHsi → enabled"      "could not establish 'enabled' baseline"
            _cleanup_phase9_user
            return
        fi
    fi

    # ------------------------------------------------------------------
    # Steps 31+32 — DisconnectHsi: capture "disabling" (transient) + "disabled" (terminal)
    # Strategy: start etcdctl watch in background BEFORE the action so every
    # revision is captured; then poll for the terminal state via etcd snapshot.
    # ------------------------------------------------------------------
    info "Step 32+32: starting etcd watch on USER_ID=${USER_ID}, then DisconnectHsi..."
    _p9_watch_start "${USER_ID}"
    fastrg_grpc disconnect_hsi "${USER_ID}" >/dev/null 2>&1 || true

    _P9_SEEN=""
    _p9_dis_terminal_ok=0
    if _p9_poll_enable_status "${USER_ID}" "disabled" 45 1; then
        _p9_dis_terminal_ok=1
    fi
    _p9_dis_log=$(_p9_watch_stop_and_read "${USER_ID}")
    _p9_dis_seq=$(_p9_extract_status_sequence "$_p9_dis_log")

    # Step 32 — verify watch captured "disabling"
    if printf '%s' "$_p9_dis_log" | grep -q '"enableStatus":"disabling"'; then
        pass "Step 32: DisconnectHsi → disabling" "watch captured sequence: ${_p9_dis_seq:-none}"
    else
        fail "Step 32: DisconnectHsi → disabling" \
            "watch did not capture 'disabling' (sequence: ${_p9_dis_seq:-none})"
    fi

    # Step 33 — terminal state reached via polling
    if [[ $_p9_dis_terminal_ok -eq 1 ]]; then
        pass "Step 33: DisconnectHsi → disabled" "terminal state reached (sequence: ${_p9_dis_seq:-none})"
    else
        _p9_now=$(_p9_etcd_enable_status "${USER_ID}")
        fail "Step 33: DisconnectHsi → disabled" \
            "still '${_p9_now:-unknown}' after 45 s (sequence: ${_p9_dis_seq:-none})"
    fi

    # ------------------------------------------------------------------
    # Steps 33+34 — ConnectHsi: capture "enabling" (transient) + "enabled" (terminal)
    # ------------------------------------------------------------------
    info "Step 34+34: starting etcd watch on USER_ID=${USER_ID}, then ConnectHsi..."
    _p9_watch_start "${USER_ID}"
    fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true

    _P9_SEEN=""
    _p9_en_terminal_ok=0
    if _p9_poll_enable_status "${USER_ID}" "enabled" 60 0.5; then
        _p9_en_terminal_ok=1
    fi
    _p9_en_log=$(_p9_watch_stop_and_read "${USER_ID}")
    _p9_en_seq=$(_p9_extract_status_sequence "$_p9_en_log")

    # Step 34 — verify watch captured "enabling"
    if printf '%s' "$_p9_en_log" | grep -q '"enableStatus":"enabling"'; then
        pass "Step 34: ConnectHsi → enabling" "watch captured sequence: ${_p9_en_seq:-none}"
    else
        fail "Step 34: ConnectHsi → enabling" \
            "watch did not capture 'enabling' (sequence: ${_p9_en_seq:-none})"
    fi

    # Step 35 — terminal state reached
    if [[ $_p9_en_terminal_ok -eq 1 ]]; then
        pass "Step 35: ConnectHsi → enabled" "terminal state reached (sequence: ${_p9_en_seq:-none})"
    else
        _p9_now=$(_p9_etcd_enable_status "${USER_ID}")
        fail "Step 35: ConnectHsi → enabled" \
            "still '${_p9_now:-unknown}' after 30 s (sequence: ${_p9_en_seq:-none})"
    fi

    # ------------------------------------------------------------------
    # Cleanup — remove synthetic user + restore subscriber count
    # ------------------------------------------------------------------
    _cleanup_phase9_user
}
