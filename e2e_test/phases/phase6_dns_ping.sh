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
    #
    # DNS resolution is tested by querying the subscriber gateway IP directly
    # with dig (bypasses OS-level DNS cache entirely).  ping is used only for
    # the proxy-ON verification where OS caching is not a concern.
    # -----------------------------------------------------------------------
    bold "---"
    info "Step 17: Toggle DNS proxy off → DNS query should fail; toggle back on → ping should succeed"

    # Determine the subscriber gateway IP (= fastrg DNS proxy IP on the LAN).
    _P6_GW=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null | \
        jq -r '.config.dhcp_gateway // empty' 2>/dev/null || true)
    if [[ -z "$_P6_GW" ]]; then
        warn "  Cannot determine subscriber gateway IP — skipping Step 17"
        return
    fi
    info "  Subscriber gateway (DNS proxy IP): ${_P6_GW}"

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

    # --- 17b: direct DNS query to fastrg gateway should fail ---
    # Use dig to query fastrg directly (bypasses OS DNS cache).
    # When proxy is OFF, fastrg drops DNS queries → dig returns no answer / times out.
    info "  [17b] Querying www.fastrg.org directly from fastrg DNS (${_P6_GW}) with proxy OFF; expecting no answer..."
    _DIG_OFF=$(ssh_lan "dig @${_P6_GW} +time=3 +tries=1 +short www.fastrg.org 2>&1" || true)
    info "  dig output (proxy OFF): '${_DIG_OFF}'"

    if printf '%s' "$_DIG_OFF" | grep -qF "${WAN_IP}"; then
        fail "Step 17: DNS proxy toggle — proxy OFF" \
            "fastrg DNS at ${_P6_GW} returned ${WAN_IP} even though DNS proxy is disabled"
    else
        pass "Step 17: DNS proxy toggle — proxy OFF" \
            "fastrg DNS returned no answer for www.fastrg.org (proxy off)"
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
