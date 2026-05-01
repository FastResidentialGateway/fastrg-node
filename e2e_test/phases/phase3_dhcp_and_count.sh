#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 3 — DHCP Assignment + Subscriber Count
# ---------------------------------------------------------------------------
phase3_dhcp_and_count() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 3 — DHCP Assignment & Subscriber Count (Steps 5, 6)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 5 — DHCP In-use IPs (LAN device got an IP)
    # ------------------------------------------------------------------
    info "Step 5: Checking DHCP address assignment for USER_ID=${USER_ID}..."
    DHCP_GRPC3=$(fastrg_grpc get_dhcp_info)
    DHCP_USER3=$(printf '%s' "$DHCP_GRPC3" | jq -r ".dhcp_infos[] | select(.user_id == ${USER_ID})" 2>/dev/null || true)

    if [[ -z "$DHCP_USER3" ]]; then
        fail "Step 5: DHCP address assigned" "User ID ${USER_ID} not found in gRPC GetFastrgDhcpInfo"
    else
        INUSE_IPS=$(printf '%s' "$DHCP_USER3" | jq -r '.inuse_ips | if length > 0 then join(", ") else empty end' 2>/dev/null || true)
        if [[ -n "$INUSE_IPS" ]]; then
            pass "Step 5: DHCP address assigned" "In-use IPs: ${INUSE_IPS}"
        else
            fail "Step 5: DHCP address assigned" "inuse_ips is empty — no LAN device has obtained an IP"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 6 — subscriber_count in etcd vs fastrg gRPC system info
    # ------------------------------------------------------------------
    info "Step 6: Comparing subscriber count (etcd user_counts vs gRPC GetFastrgSystemInfo)..."
    UC_JSON=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null || true)

    if [[ -z "$UC_JSON" ]]; then
        fail "Step 6: Subscriber count match" "Key user_counts/${NODE_UUID}/ not found in etcd"
    else
        # subscriber_count is stored as a STRING in etcd
        ETCD_COUNT=$(printf '%s' "$UC_JSON" | jq -r '.subscriber_count // empty' | tr -d '[:space:]')
        ETCD_COUNT_INT=$(( ${ETCD_COUNT:-0} + 0 ))

        SYS_JSON=$(fastrg_grpc get_system_info)
        CLI_COUNT_INT=$(printf '%s' "$SYS_JSON" | jq -r '.num_users // 0' | tr -d '[:space:]')
        CLI_COUNT_INT=$(( ${CLI_COUNT_INT:-0} + 0 ))

        if [[ $ETCD_COUNT_INT -eq $CLI_COUNT_INT ]]; then
            pass "Step 6: Subscriber count match" "etcd=${ETCD_COUNT_INT} == fastrg=${CLI_COUNT_INT}"
        else
            fail "Step 6: Subscriber count match" "etcd=${ETCD_COUNT_INT} != fastrg=${CLI_COUNT_INT}"
        fi
    fi

    # ------------------------------------------------------------------
    # 10-second wait for PPPoE session establishment
    # ------------------------------------------------------------------
    printf "\n"
    info "Waiting 10 seconds for PPPoE session establishment..."
    sleep 10
    info "Wait complete. Continuing test..."
    printf "\n"
}
