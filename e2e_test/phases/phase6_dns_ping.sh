#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 6 — DNS Static Record + Reverse Ping
# ---------------------------------------------------------------------------
phase6_dns_ping() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 6 — DNS Static + Reverse Ping (Steps 16-17)"
    bold "═══════════════════════════════════════════════════════"

    info "Step 16: Ping www.fastrg.org from LAN host; expecting reply from ${WAN_IP}..."
    PING_OUT=$(ssh_lan "ping -c 4 -W 5 www.fastrg.org 2>&1" || true)

    info "  ping output:"
    printf '%s\n' "$PING_OUT" | while IFS= read -r line; do
        printf "    %s\n" "$line"
    done

    if printf '%s' "$PING_OUT" | grep -q "from ${WAN_IP}"; then
        pass "Step 16: DNS static + ping www.fastrg.org" "Received ICMP reply from ${WAN_IP}"
    else
        # Check if it resolved but got a different IP (DNS not overridden)
        if printf '%s' "$PING_OUT" | grep -qE "PING|bytes from"; then
            REPLY_IP=$(printf '%s' "$PING_OUT" | grep -oE "from [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" | head -1 | awk '{print $2}')
            fail "Step 16: DNS static + ping www.fastrg.org" "Got reply from ${REPLY_IP:-unknown}, expected ${WAN_IP} — DNS static record may not be configured"
        else
            fail "Step 16: DNS static + ping www.fastrg.org" "No ICMP reply received"
        fi
    fi

    # -----------------------------------------------------------------------
    # Step 17 — DNS proxy on/off toggle via gRPC SetDnsProxy
    # -----------------------------------------------------------------------
    bold "---"
    info "Step 17: Toggle DNS proxy off → ping should fail; toggle back on → ping should succeed"

    # --- 17a: disable DNS proxy ---
    info "  [17a] Disabling DNS proxy for user ${USER_ID} via gRPC..."
    _DISABLE_OUT=$(fastrg_grpc set_dns_proxy "$USER_ID" false 2>&1 || true)
    _DISABLE_STATUS=$(printf '%s' "$_DISABLE_OUT" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || true)
    if [[ "$_DISABLE_STATUS" != "ok" ]]; then
        fail "Step 17: DNS proxy toggle" "SetDnsProxy(false) failed: ${_DISABLE_OUT}"
        return
    fi
    info "  SetDnsProxy(false) → ok"

    sleep 1

    # --- 17b: ping should fail (DNS not proxied → domain unresolvable) ---
    info "  [17b] Pinging www.fastrg.org with DNS proxy OFF; expecting failure..."
    _PING_OFF=$(ssh_lan "ping -c 4 -W 5 www.fastrg.org 2>&1" || true)
    info "  ping output (proxy OFF):"
    printf '%s\n' "$_PING_OFF" | while IFS= read -r line; do
        printf "    %s\n" "$line"
    done

    if printf '%s' "$_PING_OFF" | grep -q "from ${WAN_IP}"; then
        fail "Step 17: DNS proxy toggle — proxy OFF" "Got ICMP reply from ${WAN_IP} even though DNS proxy is disabled"
    else
        pass "Step 17: DNS proxy toggle — proxy OFF" "No ICMP reply from ${WAN_IP} as expected"
    fi

    # --- 17c: re-enable DNS proxy ---
    info "  [17c] Re-enabling DNS proxy for user ${USER_ID} via gRPC..."
    _ENABLE_OUT=$(fastrg_grpc set_dns_proxy "$USER_ID" true 2>&1 || true)
    _ENABLE_STATUS=$(printf '%s' "$_ENABLE_OUT" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || true)
    if [[ "$_ENABLE_STATUS" != "ok" ]]; then
        fail "Step 17: DNS proxy toggle" "SetDnsProxy(true) failed: ${_ENABLE_OUT}"
        return
    fi
    info "  SetDnsProxy(true) → ok"

    sleep 1

    # --- 17d: ping should succeed again ---
    info "  [17d] Pinging www.fastrg.org with DNS proxy ON; expecting reply from ${WAN_IP}..."
    _PING_ON=$(ssh_lan "ping -c 4 -W 5 www.fastrg.org 2>&1" || true)
    info "  ping output (proxy ON):"
    printf '%s\n' "$_PING_ON" | while IFS= read -r line; do
        printf "    %s\n" "$line"
    done

    if printf '%s' "$_PING_ON" | grep -q "from ${WAN_IP}"; then
        pass "Step 17: DNS proxy toggle — proxy ON" "Received ICMP reply from ${WAN_IP} after re-enabling DNS proxy"
    else
        if printf '%s' "$_PING_ON" | grep -qE "PING|bytes from"; then
            _REPLY_IP=$(printf '%s' "$_PING_ON" | grep -oE "from [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" | head -1 | awk '{print $2}')
            fail "Step 17: DNS proxy toggle — proxy ON" "Got reply from ${_REPLY_IP:-unknown}, expected ${WAN_IP}"
        else
            fail "Step 17: DNS proxy toggle — proxy ON" "No ICMP reply received after re-enabling DNS proxy"
        fi
    fi
}
