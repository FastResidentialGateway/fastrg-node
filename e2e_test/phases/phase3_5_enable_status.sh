#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 3.5 — PPPoE desire_status (Steps 7-9)
#
# PPPoE intent now lives in config.desire_status ("connect"/"disconnect"),
# set exclusively via the controller (dial/hangup). The node reconciles the
# live PPPoE session toward desire_status. enableStatus has been removed.
# ---------------------------------------------------------------------------
phase3_5_enable_status() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 3.5 — PPPoE desire_status (Steps 7-9)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 7 — Read current desire_status; disconnect if currently connect
    # ------------------------------------------------------------------
    info "Step 7: Reading current desire_status for USER_ID=${USER_ID}..."
    _35_hsi_json=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)

    if [[ -z "$_35_hsi_json" ]]; then
        fail "Step 7: Read desire_status" \
            "Cannot read configs/${NODE_UUID}/hsi/${USER_ID} from etcd"
        skip "Step 8: Reconnect HSI" "Cannot read current desire_status"
        skip "Step 9: Verify desire_status" "Cannot read current desire_status"
        return
    fi

    _35_cur_status=$(printf '%s' "$_35_hsi_json" | jq -r '.config.desire_status // empty')
    info "  Current desire_status: ${_35_cur_status:-<empty>}"

    if [[ "$_35_cur_status" == "connect" ]]; then
        info "Step 7: desire_status is currently connect — hangup to reset connection..."
        fastrg_grpc disconnect_hsi "${USER_ID}" >/dev/null 2>&1 || true
        pass "Step 7: HangupPPPoE USER_ID=${USER_ID}" "Hangup issued via controller"
        sleep 3
    else
        pass "Step 7: Read desire_status" \
            "desire_status=${_35_cur_status:-<empty>} — no hangup needed"
    fi

    # ------------------------------------------------------------------
    # Step 8 — Dial via controller; wait for PPPoE session to come up
    # ------------------------------------------------------------------
    info "Step 8: DialPPPoE USER_ID=${USER_ID} via controller — re-establishing session..."
    fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true

    # Wait up to 50s for PPPoE session to reach "Data phase"
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
        pass "Step 8: DialPPPoE USER_ID=${USER_ID}" \
            "PPPoE session re-established (ppp_phase='${_35_phase}')"

        # Dataplane warmup: after PPPoE re-establishes, the NAT table is empty and
        # the WAN-side ARP cache may be stale. Send a few ICMP pings and wait for a
        # reply before continuing — this (a) populates the outbound NAT entry and
        # (b) confirms end-to-end dataplane is ready, preventing the flaky Step 11
        # iperf3 / Phase 4.5 tcpdump "Connection timed out / no NAT port" failures
        # that occur when iperf3 starts before the first NAT entry is seeded.
        info "  Warming up dataplane after PPPoE re-dial (waiting for first ICMP reply)..."
        _35_warm_ok=0
        for _35_w in $(seq 1 8); do
            _35_ping=$(ssh_lan "ping -c 2 -W 2 ${WAN_IP} 2>&1" || true)
            if printf '%s' "$_35_ping" | grep -qE "0% packet loss|0\.0% packet loss|bytes from"; then
                _35_warm_ok=1
                info "  Dataplane warm (ICMP reply received, ${_35_w}x3s after Data phase)"
                break
            fi
            sleep 3
        done
        [[ $_35_warm_ok -eq 0 ]] && warn "  Dataplane warmup did not receive ICMP reply within 24s (Step 10 ping will verify)"
    else
        fail "Step 8: DialPPPoE USER_ID=${USER_ID}" \
            "PPPoE session did not come up within 50s (last ppp_phase='${_35_phase:-<empty>}')"
    fi

    # ------------------------------------------------------------------
    # Step 8-1 — Bring up each secondary subscriber's PPPoE session alongside
    # the primary subscriber ${USER_ID}. They dial into the same dpdk-bras
    # (e.g. subscriber 1 on VLAN 5, subscriber ${USER_ID} on VLAN 3); verify
    # each session also reaches "Data phase".
    # ------------------------------------------------------------------
    if [[ ${#SUB_SECONDARY_IDS[@]} -eq 0 ]]; then
        info "Step 8-1: No secondary subscribers configured — skipping secondary PPPoE check."
    else
        for _sub in "${SUB_SECONDARY_IDS[@]}"; do
            info "Step 8-1: DialPPPoE subscriber ${_sub} via controller — establishing session..."
            fastrg_grpc connect_hsi "${_sub}" >/dev/null 2>&1 || true

            _35_sub_ok=0
            _35_sub_phase=""
            for _35_i in $(seq 1 10); do
                sleep 5
                _35_hsi_now=$(fastrg_grpc get_hsi_info 2>/dev/null || true)
                _35_sub_phase=$(printf '%s' "$_35_hsi_now" | \
                    jq -r ".hsi_infos[] | select(.user_id == ${_sub}) | .status" \
                    2>/dev/null || true)
                if [[ -n "$_35_sub_phase" ]] && [[ "$_35_sub_phase" == "Data phase" ]]; then
                    _35_sub_ok=1
                    break
                fi
                info "  waiting for subscriber ${_sub} PPPoE session... (${_35_i}x5s, ppp_phase='${_35_sub_phase}')"
            done

            if [[ $_35_sub_ok -eq 1 ]]; then
                pass "Step 8-1: DialPPPoE subscriber ${_sub}" \
                    "PPPoE session established (ppp_phase='${_35_sub_phase}')"
            else
                fail "Step 8-1: DialPPPoE subscriber ${_sub}" \
                    "PPPoE session did not come up within 50s (last ppp_phase='${_35_sub_phase:-<empty>}')"
            fi
        done
    fi

    # ------------------------------------------------------------------
    # Step 9 — Verify etcd config.desire_status = "connect"
    # ------------------------------------------------------------------
    info "Step 9: Checking etcd config.desire_status for USER_ID=${USER_ID}..."
    HSI_JSON=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)

    if [[ -z "$HSI_JSON" ]]; then
        fail "Step 9: desire_status" "Cannot re-read configs/${NODE_UUID}/hsi/${USER_ID} from etcd"
        return
    fi

    DESIRE_STATUS=$(printf '%s' "$HSI_JSON" | jq -r '.config.desire_status // empty')

    if [[ "$DESIRE_STATUS" == "connect" ]]; then
        pass "Step 9: PPPoE desire_status" "config.desire_status = \"connect\""
    else
        fail "Step 9: PPPoE desire_status" "Expected \"connect\", got \"${DESIRE_STATUS}\""
    fi
}
