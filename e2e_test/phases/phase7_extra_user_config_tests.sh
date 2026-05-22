#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 7 — Dynamic New Subscriber gRPC→etcd→fastrg Config Tests (Steps 17–24)
#
# Determines the current subscriber count (N), creates subscriber N+1,
# applies the same config as subscriber 1 (with VLAN 100), exercises all
# major gRPC configuration commands, verifies each produces the correct
# etcd state and that fastrg_node applies changes locally.  The new
# subscriber config is cleaned up and the subscriber count is restored.
# ---------------------------------------------------------------------------

# Helper: remove newly-created subscriber config + restore count (idempotent).
# Called at end of phase7 AND from cleanup_fastrg trap.
_cleanup_new_subscriber_config() {
    [[ -z "${NODE_UUID:-}" ]] && return
    [[ -z "${_NEW_USER_ID:-}" ]] && return
    local _chk
    _chk=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_NEW_USER_ID}" 2>/dev/null || true)
    if [[ -n "$_chk" ]]; then
        info "Cleanup: removing user ${_NEW_USER_ID} config (RemoveConfig gRPC)..."
        fastrg_grpc remove_config "${_NEW_USER_ID}" >/dev/null 2>&1 || true
        sleep 1
        _chk=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_NEW_USER_ID}" 2>/dev/null || true)
        if [[ -z "$_chk" ]]; then
            info "Cleanup: user ${_NEW_USER_ID} config removed from etcd."
        else
            warn "Cleanup: user ${_NEW_USER_ID} config still in etcd after RemoveConfig — manual cleanup may be needed."
        fi
    fi
    if [[ -n "${_ORIG_SUB_COUNT:-}" ]]; then
        info "Cleanup: restoring subscriber count to ${_ORIG_SUB_COUNT}..."
        fastrg_grpc set_subscriber_count "${_ORIG_SUB_COUNT}" >/dev/null 2>&1 || true
    fi
}

phase7_extra_user_config_tests() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 7 — Extra User gRPC→etcd Config Tests (Steps 17-24)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Determine current subscriber count and derive new subscriber ID
    # ------------------------------------------------------------------
    info "Determining current subscriber count..."
    _sc_sys=$(fastrg_grpc get_system_info)
    _ORIG_SUB_COUNT=$(printf '%s' "$_sc_sys" | jq -r '.num_users // 0' 2>/dev/null || echo 0)
    _ORIG_SUB_COUNT=$(( ${_ORIG_SUB_COUNT:-0} + 0 ))
    if [[ $_ORIG_SUB_COUNT -eq 0 ]]; then
        warn "Cannot determine current subscriber count — skipping Phase 7"
        return
    fi
    _NEW_USER_ID=$(( _ORIG_SUB_COUNT + 1 ))
    info "Current subscriber count: ${_ORIG_SUB_COUNT} → new subscriber ID: ${_NEW_USER_ID}"

    # ------------------------------------------------------------------
    # Read subscriber 1's config from etcd as the base config
    # ------------------------------------------------------------------
    info "Reading subscriber 1 config from etcd (configs/${NODE_UUID}/hsi/1)..."
    _s1_etcd=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/1" 2>/dev/null || true)
    if [[ -z "$_s1_etcd" ]]; then
        warn "Subscriber 1 config not found in etcd — skipping Phase 7"
        return
    fi

    # ------------------------------------------------------------------
    # New subscriber parameters — same as subscriber 1, with VLAN 100
    # ------------------------------------------------------------------
    local U1=${_NEW_USER_ID}
    local U1_VLAN=100
    local U1_ACCOUNT
    local U1_PASSWORD
    local U1_POOL
    local U1_POOL_START
    local U1_POOL_END
    local U1_SUBNET
    local U1_GATEWAY
    U1_ACCOUNT=$(printf '%s'    "$_s1_etcd" | jq -r '.config.account_name // empty')
    U1_PASSWORD=$(printf '%s'   "$_s1_etcd" | jq -r '.config.password // empty')
    U1_POOL=$(printf '%s'       "$_s1_etcd" | jq -r '.config.dhcp_addr_pool // empty')
    U1_POOL_START=$(printf '%s' "$U1_POOL"  | awk -F'[-~]' '{print $1}')
    U1_POOL_END=$(printf '%s'   "$U1_POOL"  | awk -F'[-~]' '{print $2}')
    U1_SUBNET=$(printf '%s'     "$_s1_etcd" | jq -r '.config.dhcp_subnet // empty')
    U1_GATEWAY=$(printf '%s'    "$_s1_etcd" | jq -r '.config.dhcp_gateway // empty')
    local U1_DNS_DOMAIN="user${_NEW_USER_ID}test.fastrg.local"
    local U1_DNS_IP="10.1.0.${_NEW_USER_ID}"
    local U1_DNS_TTL=60

    info "New subscriber ${U1}: VLAN=${U1_VLAN} account=${U1_ACCOUNT} pool=${U1_POOL} subnet=${U1_SUBNET} gw=${U1_GATEWAY}"

    # ------------------------------------------------------------------
    # Expand subscriber count to accommodate the new subscriber
    # ------------------------------------------------------------------
    info "Expanding subscriber count to ${_NEW_USER_ID}..."
    fastrg_grpc set_subscriber_count "${_NEW_USER_ID}" >/dev/null 2>&1 || true
    sleep 1

    # ------------------------------------------------------------------
    # Step 17 — ApplyConfig new subscriber → etcd key written correctly
    # ------------------------------------------------------------------
    info "Step 17: ApplyConfig user ${U1} (VLAN=${U1_VLAN} account=${U1_ACCOUNT} pool=${U1_POOL_START}-${U1_POOL_END})..."
    _apply_reply=$(fastrg_grpc apply_config \
        "${U1}" "${U1_VLAN}" "${U1_ACCOUNT}" "${U1_PASSWORD}" \
        "${U1_POOL_START}" "${U1_POOL_END}" "${U1_SUBNET}" "${U1_GATEWAY}")
    _apply_status=$(printf '%s' "$_apply_reply" | jq -r '.status // empty' 2>/dev/null || true)

    if [[ -z "$_apply_status" ]]; then
        fail "Step 17: ApplyConfig user ${U1}" \
            "gRPC ApplyConfig returned no status — response: $(printf '%s' "$_apply_reply")"
        warn "Skipping Steps 18-24 (ApplyConfig failed)"
        skip "Step 18: fastrg applies user ${U1} config"  "ApplyConfig failed"
        skip "Step 19: ConnectHsi user ${U1}"             "ApplyConfig failed"
        skip "Step 20: DisconnectHsi user ${U1}"          "ApplyConfig failed"
        skip "Step 21: DhcpServerStart user ${U1}"        "ApplyConfig failed"
        skip "Step 22: DhcpServerStop user ${U1}"         "ApplyConfig failed"
        skip "Step 23: AddDnsRecord user ${U1}"           "ApplyConfig failed"
        skip "Step 24: RemoveDnsRecord user ${U1}"        "ApplyConfig failed"
        return
    fi

    sleep 1  # allow etcd write to propagate
    _u1_etcd=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${U1}" 2>/dev/null || true)
    if [[ -z "$_u1_etcd" ]]; then
        fail "Step 17: ApplyConfig user ${U1}" \
            "etcd key configs/${NODE_UUID}/hsi/${U1} not found after ApplyConfig"
    else
        _e_account=$(printf '%s' "$_u1_etcd" | jq -r '.config.account_name // empty')
        _e_vlan=$(printf '%s'    "$_u1_etcd" | jq -r '.config.vlan_id // empty')
        _e_pool=$(printf '%s'    "$_u1_etcd" | jq -r '.config.dhcp_addr_pool // empty')
        _e_enable=$(printf '%s'  "$_u1_etcd" | jq -r '.metadata.enableStatus // empty')
        _e_subnet=$(printf '%s'  "$_u1_etcd" | jq -r '.config.dhcp_subnet // empty')
        _e_gw=$(printf '%s'      "$_u1_etcd" | jq -r '.config.dhcp_gateway // empty')
        _expect_pool="${U1_POOL_START}-${U1_POOL_END}"

        MISMATCH=""
        [[ "$_e_account" != "$U1_ACCOUNT"    ]] && MISMATCH="${MISMATCH} account(etcd=${_e_account} expected=${U1_ACCOUNT})"
        [[ "$_e_vlan"    != "$U1_VLAN"       ]] && MISMATCH="${MISMATCH} vlan(etcd=${_e_vlan} expected=${U1_VLAN})"
        [[ "$_e_pool"    != "$_expect_pool"  ]] && MISMATCH="${MISMATCH} dhcp_pool(etcd=${_e_pool} expected=${_expect_pool})"
        [[ "$_e_enable"  != "disabled"       ]] && MISMATCH="${MISMATCH} enableStatus(etcd=${_e_enable} expected=disabled)"
        [[ "$_e_subnet"  != "$U1_SUBNET"     ]] && MISMATCH="${MISMATCH} subnet(etcd=${_e_subnet} expected=${U1_SUBNET})"
        [[ "$_e_gw"      != "$U1_GATEWAY"    ]] && MISMATCH="${MISMATCH} gateway(etcd=${_e_gw} expected=${U1_GATEWAY})"

        if [[ -z "$MISMATCH" ]]; then
            pass "Step 17: ApplyConfig user ${U1}" \
                "account=${_e_account} vlan=${_e_vlan} pool=${_e_pool} enableStatus=${_e_enable}"
        else
            fail "Step 17: ApplyConfig user ${U1}" "etcd mismatch:${MISMATCH}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 18 — fastrg watches etcd → applies user 1 config locally
    # ------------------------------------------------------------------
    info "Step 18: Verifying fastrg applied user ${U1} config locally (GetFastrgHsiInfo + GetFastrgDhcpInfo)..."
    sleep 1  # ensure etcd watcher has fired
    _u1_hsi=$(fastrg_grpc get_hsi_info)
    _u1_hsi_user=$(printf '%s' "$_u1_hsi" | \
        jq -r ".hsi_infos[] | select(.user_id == ${U1})" 2>/dev/null || true)
    _u1_dhcp=$(fastrg_grpc get_dhcp_info)
    _u1_dhcp_user=$(printf '%s' "$_u1_dhcp" | \
        jq -r ".dhcp_infos[] | select(.user_id == ${U1})" 2>/dev/null || true)

    if [[ -z "$_u1_hsi_user" ]] || [[ -z "$_u1_dhcp_user" ]]; then
        fail "Step 18: fastrg applies user ${U1} config" \
            "User ${U1} not found in gRPC response — hsi:$( [[ -n "$_u1_hsi_user" ]] && echo ok || echo missing) dhcp:$( [[ -n "$_u1_dhcp_user" ]] && echo ok || echo missing)"
    else
        _g_vlan=$(printf '%s'  "$_u1_hsi_user"  | jq -r '.vlan_id // empty')
        _g_range=$(printf '%s' "$_u1_dhcp_user" | jq -r '.ip_range // empty')
        _g_gw=$(printf '%s'    "$_u1_dhcp_user" | jq -r '.gateway // empty')
        _g_subnet=$(printf '%s' "$_u1_dhcp_user" | jq -r '.subnet_mask // empty')

        # Normalize pool range for comparison (gRPC returns "start - end", etcd uses "start-end")
        _g_range_norm=$(printf '%s' "$_g_range" | tr -d ' ' | tr '-' '~' | sed 's/~~/~/')
        _e_pool_norm="${U1_POOL_START}~${U1_POOL_END}"

        MISMATCH=""
        [[ "$_g_vlan"       != "$U1_VLAN"     ]] && MISMATCH="${MISMATCH} vlan(grpc=${_g_vlan} expected=${U1_VLAN})"
        [[ "$_g_range_norm" != "$_e_pool_norm" ]] && MISMATCH="${MISMATCH} pool(grpc=${_g_range} expected=${U1_POOL_START}-${U1_POOL_END})"
        [[ "$_g_gw"         != "$U1_GATEWAY"  ]] && MISMATCH="${MISMATCH} gateway(grpc=${_g_gw} expected=${U1_GATEWAY})"
        [[ "$_g_subnet"     != "$U1_SUBNET"   ]] && MISMATCH="${MISMATCH} subnet(grpc=${_g_subnet} expected=${U1_SUBNET})"

        if [[ -z "$MISMATCH" ]]; then
            pass "Step 18: fastrg applies user ${U1} config" \
                "vlan=${_g_vlan} pool=${_g_range} gw=${_g_gw}"
        else
            fail "Step 18: fastrg applies user ${U1} config" "Mismatch:${MISMATCH}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 19 — ConnectHsi user 1 → PPPoE phase starts
    # PPPoE will fail (no server for user 1) — that is expected.
    # We verify the command was processed by observing the phase change.
    # ------------------------------------------------------------------
    info "Step 19: ConnectHsi user ${U1} → verify PPPoE phase changes from 'End phase'..."
    _before_phase=$(fastrg_grpc get_hsi_info | \
        python3 -c "import sys,json; d=json.load(sys.stdin); \
        u=[h for h in d.get('hsi_infos',[]) if h.get('user_id')==${U1}]; \
        print(u[0].get('status','') if u else '')" 2>/dev/null || true)

    fastrg_grpc connect_hsi "${U1}" >/dev/null 2>&1 || true

    # Poll up to 5 s for PPPoE phase to change (PPPoE_CMD_ENABLE event processed)
    _phase_changed=0
    _new_phase=""
    for _ci in $(seq 1 5); do
        sleep 1
        _new_phase=$(fastrg_grpc get_hsi_info | \
            python3 -c "import sys,json; d=json.load(sys.stdin); \
            u=[h for h in d.get('hsi_infos',[]) if h.get('user_id')==${U1}]; \
            print(u[0].get('status','') if u else '')" 2>/dev/null || true)
        if [[ "$_new_phase" != "End phase" && "$_new_phase" != "not configured" ]]; then
            _phase_changed=1
            break
        fi
    done

    if [[ $_phase_changed -eq 1 ]]; then
        pass "Step 19: ConnectHsi user ${U1}" \
            "PPPoE phase: '${_before_phase}' → '${_new_phase}' (failure expected — no server)"
    else
        fail "Step 19: ConnectHsi user ${U1}" \
            "PPPoE phase did not change from '${_before_phase}' — ConnectHsi may not have been processed"
    fi

    # ------------------------------------------------------------------
    # Step 20 — DisconnectHsi user 1 → PPPoE returns to End phase
    # Called while PPPoE is still retrying PADI (within ~5 s of ConnectHsi).
    # ------------------------------------------------------------------
    info "Step 20: DisconnectHsi user ${U1} → verify PPPoE returns to 'End phase'..."
    fastrg_grpc disconnect_hsi "${U1}" >/dev/null 2>&1 || true

    _disc_ok=0
    _disc_phase=""
    # Wait up to 15 s: DisconnectHsi is immediate; natural PPPoE timeout is ~10 s
    for _di in $(seq 1 15); do
        sleep 1
        _disc_phase=$(fastrg_grpc get_hsi_info | \
            python3 -c "import sys,json; d=json.load(sys.stdin); \
            u=[h for h in d.get('hsi_infos',[]) if h.get('user_id')==${U1}]; \
            print(u[0].get('status','') if u else '')" 2>/dev/null || true)
        if [[ "$_disc_phase" == "End phase" ]]; then
            _disc_ok=1
            break
        fi
    done

    if [[ $_disc_ok -eq 1 ]]; then
        pass "Step 20: DisconnectHsi user ${U1}" "PPPoE returned to 'End phase'"
    else
        fail "Step 20: DisconnectHsi user ${U1}" \
            "PPPoE still in '${_disc_phase:-unknown}' after DisconnectHsi + 15 s"
    fi

    # ------------------------------------------------------------------
    # Step 21 — DhcpServerStart user 1 → DHCP server on
    # ------------------------------------------------------------------
    info "Step 21: DhcpServerStart user ${U1} → verify DHCP server starts..."
    fastrg_grpc start_dhcp_server "${U1}" >/dev/null 2>&1 || true
    sleep 1
    _dhcp15=$(fastrg_grpc get_dhcp_info | \
        python3 -c "import sys,json; d=json.load(sys.stdin); \
        u=[h for h in d.get('dhcp_infos',[]) if h.get('user_id')==${U1}]; \
        print(u[0].get('status','') if u else '')" 2>/dev/null || true)

    if [[ "$_dhcp15" == "DHCP server is on" ]]; then
        pass "Step 21: DhcpServerStart user ${U1}" "status='${_dhcp15}'"
    else
        fail "Step 21: DhcpServerStart user ${U1}" \
            "expected 'DHCP server is on', got '${_dhcp15:-empty}'"
    fi

    # ------------------------------------------------------------------
    # Step 22 — DhcpServerStop user 1 → DHCP server off
    # ------------------------------------------------------------------
    info "Step 22: DhcpServerStop user ${U1} → verify DHCP server stops..."
    fastrg_grpc stop_dhcp_server "${U1}" >/dev/null 2>&1 || true
    sleep 1
    _dhcp16=$(fastrg_grpc get_dhcp_info | \
        python3 -c "import sys,json; d=json.load(sys.stdin); \
        u=[h for h in d.get('dhcp_infos',[]) if h.get('user_id')==${U1}]; \
        print(u[0].get('status','') if u else '')" 2>/dev/null || true)

    if [[ "$_dhcp16" != "DHCP server is on" ]]; then
        pass "Step 22: DhcpServerStop user ${U1}" "status='${_dhcp16:-off}'"
    else
        fail "Step 22: DhcpServerStop user ${U1}" \
            "expected DHCP off, got '${_dhcp16}'"
    fi

    # ------------------------------------------------------------------
    # Step 23 — AddDnsRecord user 1 → etcd write + fastrg loads locally
    # ------------------------------------------------------------------
    info "Step 23: AddDnsRecord user ${U1} domain=${U1_DNS_DOMAIN} ip=${U1_DNS_IP} ttl=${U1_DNS_TTL}..."
    _dns_add_reply=$(fastrg_grpc add_dns_record "${U1}" "${U1_DNS_DOMAIN}" "${U1_DNS_IP}" "${U1_DNS_TTL}")
    _dns_add_status=$(printf '%s' "$_dns_add_reply" | jq -r '.status // empty' 2>/dev/null || true)

    if [[ -z "$_dns_add_status" ]]; then
        fail "Step 23: AddDnsRecord user ${U1}" \
            "gRPC AddDnsRecord returned no status — response: $(printf '%s' "$_dns_add_reply")"
        skip "Step 24: RemoveDnsRecord user 1" "AddDnsRecord failed"
    else
        sleep 1
        _dns17_etcd=$(etcdctl_get_value \
            "configs/${NODE_UUID}/${U1}/dns/${U1_DNS_DOMAIN}" 2>/dev/null || true)
        _dns17_grpc=$(fastrg_grpc get_dns_static "${U1}")
        _dns17_match=$(printf '%s' "$_dns17_grpc" | \
            jq -r ".entries[] | select(.domain == \"${U1_DNS_DOMAIN}\") | .domain" 2>/dev/null || true)

        MISMATCH=""
        if [[ -z "$_dns17_etcd" ]]; then
            MISMATCH="${MISMATCH} etcd-key-missing"
        else
            _e17_ip=$(printf '%s' "$_dns17_etcd" | jq -r '.ip // empty')
            _e17_ttl=$(printf '%s' "$_dns17_etcd" | jq -r '.ttl // empty')
            [[ "$_e17_ip"  != "$U1_DNS_IP"  ]] && MISMATCH="${MISMATCH} ip(etcd=${_e17_ip} expected=${U1_DNS_IP})"
            [[ "$_e17_ttl" != "$U1_DNS_TTL" ]] && MISMATCH="${MISMATCH} ttl(etcd=${_e17_ttl} expected=${U1_DNS_TTL})"
        fi
        [[ -z "$_dns17_match" ]] && MISMATCH="${MISMATCH} grpc-record-missing"

        if [[ -z "$MISMATCH" ]]; then
            pass "Step 23: AddDnsRecord user ${U1}" \
                "etcd ip=${_e17_ip} ttl=${_e17_ttl} grpc=ok"
        else
            fail "Step 23: AddDnsRecord user ${U1}" "Mismatch:${MISMATCH}"
        fi

        # ------------------------------------------------------------------
        # Step 24 — RemoveDnsRecord user 1 → etcd deleted + fastrg removes
        # ------------------------------------------------------------------
        info "Step 24: RemoveDnsRecord user ${U1} domain=${U1_DNS_DOMAIN}..."
        _dns_del_reply=$(fastrg_grpc remove_dns_record "${U1}" "${U1_DNS_DOMAIN}")
        _dns_del_status=$(printf '%s' "$_dns_del_reply" | jq -r '.status // empty' 2>/dev/null || true)

        if [[ -z "$_dns_del_status" ]]; then
            fail "Step 24: RemoveDnsRecord user ${U1}" \
                "gRPC RemoveDnsRecord returned no status — response: $(printf '%s' "$_dns_del_reply")"
        else
            sleep 1
            _dns18_gone=$(etcdctl_get_value \
                "configs/${NODE_UUID}/${U1}/dns/${U1_DNS_DOMAIN}" 2>/dev/null || true)
            _dns18_grpc=$(fastrg_grpc get_dns_static "${U1}")
            _dns18_still=$(printf '%s' "$_dns18_grpc" | \
                jq -r ".entries[] | select(.domain == \"${U1_DNS_DOMAIN}\") | .domain" 2>/dev/null || true)

            MISMATCH=""
            [[ -n "$_dns18_gone"  ]] && MISMATCH="${MISMATCH} etcd-key-still-present"
            [[ -n "$_dns18_still" ]] && MISMATCH="${MISMATCH} grpc-record-still-present"

            if [[ -z "$MISMATCH" ]]; then
                pass "Step 24: RemoveDnsRecord user ${U1}" \
                    "etcd key deleted, fastrg record removed"
            else
                fail "Step 24: RemoveDnsRecord user ${U1}" "Mismatch:${MISMATCH}"
            fi
        fi
    fi

    # ------------------------------------------------------------------
    # Cleanup — remove new subscriber config and restore subscriber count
    # ------------------------------------------------------------------
    _cleanup_new_subscriber_config
}
