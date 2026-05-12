#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 3.5 — PPPoE Enable Status
# ---------------------------------------------------------------------------
phase3_5_enable_status() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 3.5 — PPPoE Enable Status (Steps 7-9)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 7 — Read current enableStatus; disconnect HSI if enabled
    # ------------------------------------------------------------------
    info "Step 7: Reading current PPPoE enableStatus for USER_ID=${USER_ID}..."
    _35_hsi_json=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)

    if [[ -z "$_35_hsi_json" ]]; then
        fail "Step 7: Read PPPoE enableStatus" \
            "Cannot read configs/${NODE_UUID}/hsi/${USER_ID} from etcd"
        skip "Step 8: Reconnect HSI" "Cannot read current enableStatus"
        skip "Step 9: Verify PPPoE enableStatus" "Cannot read current enableStatus"
        return
    fi

    _35_cur_status=$(printf '%s' "$_35_hsi_json" | jq -r '.metadata.enableStatus // empty')
    info "  Current enableStatus: ${_35_cur_status:-<empty>}"

    if [[ "$_35_cur_status" == "enabled" ]]; then
        info "Step 7: PPPoE is currently enabled — calling DisconnectHsi to reset connection..."
        fastrg_grpc disconnect_hsi "${USER_ID}" >/dev/null 2>&1 || true
        pass "Step 7: DisconnectHsi USER_ID=${USER_ID}" "Disconnect issued"
        sleep 2
    else
        pass "Step 7: Read PPPoE enableStatus" \
            "enableStatus=${_35_cur_status:-<empty>} — no disconnect needed"
    fi

    # ------------------------------------------------------------------
    # Step 8 — Reconnect HSI; wait for PPPoE session to come up
    # ------------------------------------------------------------------
    info "Step 8: ConnectHsi USER_ID=${USER_ID} — re-establishing PPPoE session..."
    fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true

    # Wait up to 30s for PPPoE session to move out of "End phase"
    _35_ppp_ok=0
    _35_phase=""
    for _35_i in $(seq 1 10); do
        sleep 5
        _35_hsi_now=$(fastrg_grpc get_hsi_info 2>/dev/null || true)
        _35_phase=$(printf '%s' "$_35_hsi_now" | \
            jq -r ".hsi_infos[] | select(.user_id == ${USER_ID}) | .status" \
            2>/dev/null || true)
        if [[ -n "$_35_phase" ]] && [[ "$_35_phase" == "Data phase" ]]; then
            _35_ppp_ok=1
            break
        fi
        info "  waiting for PPPoE session... (${_35_i}x5s, ppp_phase='${_35_phase}')"
    done

    if [[ $_35_ppp_ok -eq 1 ]]; then
        pass "Step 8: ConnectHsi USER_ID=${USER_ID}" \
            "PPPoE session re-established (ppp_phase='${_35_phase}')"
    else
        fail "Step 8: ConnectHsi USER_ID=${USER_ID}" \
            "PPPoE session did not come up within 50s (last ppp_phase='${_35_phase:-<empty>}')"
    fi

    # ------------------------------------------------------------------
    # Step 9 — Verify etcd metadata.enableStatus = "enabled"
    # ------------------------------------------------------------------
    info "Step 9: Checking etcd metadata.enableStatus for USER_ID=${USER_ID}..."
    HSI_JSON=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)

    if [[ -z "$HSI_JSON" ]]; then
        fail "Step 9: PPPoE enableStatus" "Cannot re-read configs/${NODE_UUID}/hsi/${USER_ID} from etcd"
        return
    fi

    ENABLE_STATUS=$(printf '%s' "$HSI_JSON" | jq -r '.metadata.enableStatus // empty')

    if [[ "$ENABLE_STATUS" == "enabled" ]]; then
        pass "Step 9: PPPoE enableStatus" "metadata.enableStatus = \"enabled\""
    else
        fail "Step 9: PPPoE enableStatus" "Expected \"enabled\", got \"${ENABLE_STATUS}\""
    fi
}
