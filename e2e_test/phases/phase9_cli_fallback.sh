#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 9 — CLI three-tier write fallback (Steps 36-38)
#
# Exercises fastrg_cli on the node:
#   Step 36  tier 1 — CLI -> controller ConfigService (login + apply)  -> etcd
#   Step 37  tier 2 — CLI -> direct etcd CAS (no controller configured) -> etcd
#   Step 38  node gRPC rejects config writes while etcd is reachable
#            (SDN guard: the node only accepts CLI writes when etcd is down)
# ---------------------------------------------------------------------------

# Run fastrg_cli on the node, feeding it newline-separated commands on stdin.
# $1 = extra CLI flags (string); remaining args = commands piped to the prompt.
_p10_cli() {
    local _flags="$1"; shift
    local _cmds=""
    local _c
    for _c in "$@"; do _cmds="${_cmds}${_c}\n"; done
    ssh_node "printf '${_cmds}quit\n' | /usr/local/bin/fastrg_cli -i 127.0.0.1:${FASTRG_GRPC_PORT} ${_flags} 2>&1"
}

phase9_cli_fallback() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 9 — CLI three-tier write fallback (Steps 36-38)"
    bold "═══════════════════════════════════════════════════════"

    if ! ssh_node "test -x /usr/local/bin/fastrg_cli" 2>/dev/null; then
        skip "Step 36: CLI tier-1 (controller)" "fastrg_cli not installed on node"
        skip "Step 37: CLI tier-2 (etcd direct)" "fastrg_cli not installed on node"
        skip "Step 38: node rejects write in SDN mode" "fastrg_cli not installed on node"
        return
    fi

    # Re-read the current subscriber count directly — do not rely solely on
    # _P9_ORIG_SUB_COUNT which is set by phase8 and may be 0 if phase8 was
    # skipped (e.g. node gRPC was briefly unavailable).  Also enforce a minimum
    # offset of 4 from USER_ID so the test users never collide with USER_ID.
    local _p9_cur_sc
    _p9_cur_sc=$(fastrg_grpc get_system_info 2>/dev/null | jq -r '.num_users // 0' 2>/dev/null || echo 0)
    _p9_cur_sc=$(( ${_p9_cur_sc:-0} + 0 ))
    if [[ $_p9_cur_sc -eq 0 ]]; then
        _p9_cur_sc="${_P9_ORIG_SUB_COUNT:-2}"
        _p9_cur_sc=$(( ${_p9_cur_sc:-2} + 0 ))
        [[ $_p9_cur_sc -lt 2 ]] && _p9_cur_sc=2
    fi
    # Ensure _U1/_U2 are always > USER_ID + 3 to avoid colliding with it.
    local _U1=$(( _p9_cur_sc + 2 ))
    local _U2=$(( _p9_cur_sc + 3 ))
    [[ $_U1 -le $(( USER_ID + 3 )) ]] && { _U1=$(( USER_ID + 4 )); _U2=$(( USER_ID + 5 )); }
    # Make room for the test users.
    fastrg_grpc set_subscriber_count "$(( _U2 + 1 ))" >/dev/null 2>&1 || true
    sleep 2

    # ------------------------------------------------------------------
    # Step 36 — tier 1: CLI -> controller (login + apply config)
    # ------------------------------------------------------------------
    info "Step 36: fastrg_cli tier-1 (controller) apply config for user ${_U1}..."
    _p10_out=$(_p10_cli \
        "-c ${CONTROLLER_GRPC} -r ${CONTROLLER_REST} -n ${NODE_UUID}" \
        "controller login" "${CONTROLLER_USER}" "${CONTROLLER_PASS}" \
        "config add user ${_U1} pppoe-dhcp vlan 360 account cli1@isp password pw1 pool 192.168.36.2~192.168.36.200 subnet 255.255.255.0 gateway 192.168.36.1" \
        2>/dev/null || true)
    sleep 3
    _p10_e1=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_U1}" 2>/dev/null || true)
    _p10_v1=$(printf '%s' "$_p10_e1" | jq -r '.config.vlan_id // empty' 2>/dev/null || true)
    if printf '%s' "$_p10_out" | grep -q "\[controller\] apply config: OK" && [[ "$_p10_v1" == "360" ]]; then
        pass "Step 36: CLI tier-1 (controller)" "config landed in etcd (vlan=360) via controller"
    else
        fail "Step 36: CLI tier-1 (controller)" \
            "controller path failed (etcd vlan='${_p10_v1:-none}'); output: $(printf '%s' "$_p10_out" | tr '\n' '|' | tail -c 200)"
    fi

    # ------------------------------------------------------------------
    # Step 37 — tier 2: CLI -> direct etcd (no controller configured)
    # ------------------------------------------------------------------
    info "Step 37: fastrg_cli tier-2 (direct etcd) apply config for user ${_U2}..."
    _p10_out2=$(_p10_cli \
        "-e ${ETCD_ENDPOINT} -n ${NODE_UUID}" \
        "config add user ${_U2} pppoe-dhcp vlan 370 account cli2@isp password pw2 pool 192.168.37.2~192.168.37.200 subnet 255.255.255.0 gateway 192.168.37.1" \
        2>/dev/null || true)
    sleep 3
    _p10_e2=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_U2}" 2>/dev/null || true)
    _p10_v2=$(printf '%s' "$_p10_e2" | jq -r '.config.vlan_id // empty' 2>/dev/null || true)
    if printf '%s' "$_p10_out2" | grep -q "\[etcd\] apply config: OK" && [[ "$_p10_v2" == "370" ]]; then
        pass "Step 37: CLI tier-2 (etcd direct)" "config landed in etcd (vlan=370) via direct etcd CAS"
    else
        fail "Step 37: CLI tier-2 (etcd direct)" \
            "etcd path failed (etcd vlan='${_p10_v2:-none}'); output: $(printf '%s' "$_p10_out2" | tr '\n' '|' | tail -c 200)"
    fi

    # ------------------------------------------------------------------
    # Step 38 — node gRPC ApplyConfig is rejected while etcd is reachable
    # (SDN guard) — proves the node only accepts CLI writes when etcd is down.
    # ------------------------------------------------------------------
    info "Step 38: node gRPC ApplyConfig should be rejected in SDN mode..."
    _p10_rej=$(ssh_node "ETCDCTL_API=3 true; grpcurl -plaintext -import-path /root/fastrg-node/northbound/grpc -proto fastrg_node.proto -d '{\"user_id\":${_U1},\"vlan_id\":360,\"pppoe_account\":\"x\",\"pppoe_password\":\"x\",\"dhcp_pool_start\":\"1.1.1.2\",\"dhcp_pool_end\":\"1.1.1.9\",\"dhcp_subnet_mask\":\"255.255.255.0\",\"dhcp_gateway\":\"1.1.1.1\"}' 127.0.0.1:${FASTRG_GRPC_PORT} fastrgnodeservice.FastrgService/ApplyConfig 2>&1" 2>/dev/null || true)
    if printf '%s' "$_p10_rej" | grep -qiE "FailedPrecondition|etcd reachable"; then
        pass "Step 38: node rejects write in SDN mode" "node returned FAILED_PRECONDITION as expected"
    else
        # grpcurl/proto may be unavailable on the node — degrade to skip rather than false fail
        if printf '%s' "$_p10_rej" | grep -qiE "No such file|proto|not found|command not found"; then
            skip "Step 38: node rejects write in SDN mode" "grpcurl/proto unavailable on node"
        else
            fail "Step 38: node rejects write in SDN mode" \
                "expected FAILED_PRECONDITION; got: $(printf '%s' "$_p10_rej" | tr '\n' '|' | tail -c 200)"
        fi
    fi

    # Cleanup test users (via controller).
    fastrg_grpc remove_config "${_U1}" >/dev/null 2>&1 || true
    fastrg_grpc remove_config "${_U2}" >/dev/null 2>&1 || true
    [[ -n "${_P9_ORIG_SUB_COUNT:-}" ]] && fastrg_grpc set_subscriber_count "${_P9_ORIG_SUB_COUNT}" >/dev/null 2>&1 || true
}
