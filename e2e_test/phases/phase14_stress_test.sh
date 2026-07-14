#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 14 — DNS Reconnect Stress Test (Steps 51-60)
#
#   Step 51-60  10 rounds of disconnect → reconnect → DNS static verify
#               Validates that dns_proxy_init + etcd_client_load_dns_records
#               correctly reload DNS static records after every PPPoE session
#               re-establishment (1s disconnect interval).
# ---------------------------------------------------------------------------
phase14_stress_test() {
    local STRESS_ROUNDS=10
    local STRESS_WAIT=1

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 14 — DNS Reconnect Stress Test (Steps 51-60)"
    bold "═══════════════════════════════════════════════════════"

    local _base_step=51

    _stress_wait_data_phase() {
        local _try
        for _try in $(seq 1 24); do
            sleep 5
            local _raw _ph
            _raw=$(fastrg_grpc get_hsi_info 2>/dev/null || true)
            # Skip iterations where gRPC returned nothing (transient failure)
            [[ -z "$_raw" ]] && continue
            _ph=$(printf '%s' "$_raw" | \
                jq -r ".hsi_infos[] | select(.user_id == ${USER_ID}) | .status" 2>/dev/null || true)
            [[ "$_ph" == "Data phase" ]] && return 0
        done
        return 1
    }

    _stress_check_dns() {
        local _step_name="$1"
        local _round="$2"

        local DNS_VAL
        DNS_VAL=$(etcdctl_get_value "configs/${NODE_UUID}/dns/${USER_ID}" 2>/dev/null || true)
        DNS_DOMAINS=$(printf '%s' "$DNS_VAL" | jq -r '.[].domain // empty' 2>/dev/null || true)

        if [[ -z "$DNS_DOMAINS" ]]; then
            skip "$_step_name" "No DNS static records in etcd — nothing to verify"
            return 0
        fi

        DNS_GRPC=$(fastrg_grpc get_dns_static "${USER_ID}" 2>/dev/null || true)
        local _fail=0
        local _detail=""

        while IFS= read -r _domain; do
            [[ -z "$_domain" ]] && continue
            _match=$(printf '%s' "$DNS_GRPC" | \
                jq -r ".entries[] | select(.domain == \"${_domain}\") | .domain" 2>/dev/null || true)
            if [[ -n "$_match" ]]; then
                _detail="${_detail} ${_domain}:OK"
            else
                _detail="${_detail} ${_domain}:MISSING"
                _fail=1
            fi
        done <<< "$DNS_DOMAINS"

        if [[ $_fail -eq 0 ]]; then
            pass "$_step_name" "DNS static match —${_detail}"
        else
            fail "$_step_name" "DNS static mismatch —${_detail}"
        fi
    }

    info "Running ${STRESS_ROUNDS} rounds of disconnect→reconnect→DNS verify (${STRESS_WAIT}s interval)..."

    local _r
    for _r in $(seq 1 "$STRESS_ROUNDS"); do
        local _step_num=$((_base_step + _r - 1))
        local _step_name="Step ${_step_num}: DNS stress round ${_r}/${STRESS_ROUNDS}"

        info "${_step_name}: disconnect..."
        fastrg_grpc disconnect_hsi "${USER_ID}" >/dev/null 2>&1 || true
        sleep "$STRESS_WAIT"

        info "${_step_name}: reconnect..."
        fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true

        if ! _stress_wait_data_phase; then
            fail "$_step_name" "PPPoE session did not reach Data phase within 120s"
            continue
        fi

        _stress_check_dns "$_step_name" "$_r"
    done

    # Restore session to Data phase for subsequent phases (summary only, no further phases)
    local _cur_phase
    _cur_phase=$(fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[] | select(.user_id == ${USER_ID}) | .status" 2>/dev/null || true)
    if [[ "$_cur_phase" != "Data phase" ]]; then
        info "Restoring session to Data phase after stress test..."
        fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true
        _stress_wait_data_phase || true
    fi
}
