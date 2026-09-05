#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 6 — DNS Static Record + Reverse Ping
# ---------------------------------------------------------------------------

# What one `dig +short` output says about an address: "answered" when a whole
# line of it is that address, "silent" when no such answer came back, "err"
# when the query could not be made at all.
#
# The address is matched whole-line, so 192.168.201.110 cannot read as an
# answer of 192.168.201.11. A dig that timed out prints its own error text and
# reads as silent, which is what a dropped query looks like.
e2e_dns_answer_verdict() {
    local _out="${1:-}" _ip="${2:-}"

    if [[ -z "$_ip" ]]; then
        printf 'err'
        return 1
    fi
    if printf '%s\n' "$_out" | grep -qE 'command not found|No such file or directory'; then
        printf 'err'
        return 1
    fi
    if printf '%s\n' "$_out" | grep -qxF "$_ip"; then
        printf 'answered'
        return 0
    fi
    printf 'silent'
    return 0
}

local_validation_register dns_answer_verdict e2e_dns_answer_verdict \
    dns_answer_answered \
    dns_answer_prefix_of_longer_ip \
    dns_answer_timed_out \
    dns_answer_empty_output \
    dns_answer_dig_missing \
    dns_answer_no_ip_asked

phase6_dns_ping() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 6 — DNS Static + Reverse Ping (Steps 16-18b)"
    bold "═══════════════════════════════════════════════════════"

    info "Step 17: Ping www.fastrg.org from LAN host; expecting reply from ${WAN_IP}..."
    # Wrap in `timeout`: name resolution (getaddrinfo) is NOT bounded by ping's
    # -W flag, so a non-answering DNS proxy would hang the whole suite.
    PING_OUT=$(ssh_lan "timeout 25 ping -c 4 -W 5 www.fastrg.org 2>&1" || true)

    info "  ping output:"
    printf '%s\n' "$PING_OUT" | while IFS= read -r line; do
        printf "    %s\n" "$line"
    done

    if printf '%s' "$PING_OUT" | grep -q "from ${WAN_IP}"; then
        pass "Step 17: DNS static + ping www.fastrg.org" "Received ICMP reply from ${WAN_IP}"
    else
        # Check if it resolved but got a different IP (DNS not overridden)
        if printf '%s' "$PING_OUT" | grep -qE "PING|bytes from"; then
            REPLY_IP=$(printf '%s' "$PING_OUT" | grep -oE "from [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" | head -1 | awk '{print $2}' || true)
            fail "Step 17: DNS static + ping www.fastrg.org" "Got reply from ${REPLY_IP:-unknown}, expected ${WAN_IP} — DNS static record may not be configured"
        else
            fail "Step 17: DNS static + ping www.fastrg.org" "No ICMP reply received"
        fi
    fi

    # -----------------------------------------------------------------------
    # Step 18a/18b — DNS proxy on/off toggle via gRPC SetDnsProxy
    #
    # DNS resolution is tested by querying the subscriber gateway IP directly
    # with dig (bypasses OS-level DNS cache entirely).  ping is used only for
    # the proxy-ON verification where OS caching is not a concern.
    # -----------------------------------------------------------------------
    bold "---"
    info "Step 18a/18b: Toggle DNS proxy off → DNS query should fail; toggle back on → ping should succeed"

    # Determine the subscriber gateway IP (= fastrg DNS proxy IP on the LAN).
    _P6_GW=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null | \
        jq -r '.config.dhcp_gateway // empty' 2>/dev/null || true)
    if [[ -z "$_P6_GW" ]]; then
        warn "  Cannot determine subscriber gateway IP — skipping Step 18a/18b"
        return
    fi
    info "  Subscriber gateway (DNS proxy IP): ${_P6_GW}"

    # --- 18a: disable DNS proxy and verify DNS queries fail ---
    info "  [18a] Disabling DNS proxy for user ${USER_ID} via gRPC..."
    _DISABLE_OUT=$(fastrg_grpc set_dns_proxy "$USER_ID" false 2>&1 || true)
    _DISABLE_STATUS=$(printf '%s' "$_DISABLE_OUT" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || true)
    if [[ -z "$_DISABLE_STATUS" ]] || printf '%s' "$_DISABLE_OUT" | grep -qi "error"; then
        fail "Step 18a: DNS proxy toggle — proxy OFF" "SetDnsProxy(false) failed: ${_DISABLE_OUT}"
        return
    fi
    info "  SetDnsProxy(false) → ok"

    sleep 1

    # --- 18a: direct DNS query to fastrg gateway should fail ---
    # Use dig to query fastrg directly (bypasses OS DNS cache).
    # When proxy is OFF, fastrg drops DNS queries → dig returns no answer / times out.
    info "  [18a] Querying www.fastrg.org directly from fastrg DNS (${_P6_GW}) with proxy OFF; expecting no answer..."
    _DIG_OFF=$(ssh_lan "timeout 10 dig @${_P6_GW} +time=3 +tries=1 +short www.fastrg.org 2>&1" || true)
    info "  dig output (proxy OFF): '${_DIG_OFF}'"
    _DIG_OFF_VERDICT=$(e2e_dns_answer_verdict "$_DIG_OFF" "${WAN_IP}" || true)

    if [[ "$_DIG_OFF_VERDICT" == "silent" ]]; then
        pass "Step 18a: DNS proxy toggle — proxy OFF" \
            "fastrg DNS returned no answer for www.fastrg.org (proxy off)"
    else
        fail "Step 18a: DNS proxy toggle — proxy OFF" \
            "fastrg DNS at ${_P6_GW} verdict '${_DIG_OFF_VERDICT}' for ${WAN_IP}, want silent"
    fi

    # --- 18b: re-enable DNS proxy and verify ping succeeds ---
    info "  [18b] Re-enabling DNS proxy for user ${USER_ID} via gRPC..."
    _ENABLE_OUT=$(fastrg_grpc set_dns_proxy "$USER_ID" true 2>&1 || true)
    _ENABLE_STATUS=$(printf '%s' "$_ENABLE_OUT" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || true)
    if [[ -z "$_ENABLE_STATUS" ]] || printf '%s' "$_ENABLE_OUT" | grep -qi "error"; then
        fail "Step 18b: DNS proxy toggle — proxy ON" "SetDnsProxy(true) failed: ${_ENABLE_OUT}"
        return
    fi
    info "  SetDnsProxy(true) → ok"

    sleep 1

    # --- 18b: ping should succeed again ---
    info "  [18b] Pinging www.fastrg.org with DNS proxy ON; expecting reply from ${WAN_IP}..."
    _PING_ON=$(ssh_lan "timeout 25 ping -c 4 -W 5 www.fastrg.org 2>&1" || true)
    info "  ping output (proxy ON):"
    printf '%s\n' "$_PING_ON" | while IFS= read -r line; do
        printf "    %s\n" "$line"
    done

    if printf '%s' "$_PING_ON" | grep -q "from ${WAN_IP}"; then
        pass "Step 18b: DNS proxy toggle — proxy ON" "Received ICMP reply from ${WAN_IP} after re-enabling DNS proxy"
    else
        if printf '%s' "$_PING_ON" | grep -qE "PING|bytes from"; then
            _REPLY_IP=$(printf '%s' "$_PING_ON" | grep -oE "from [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" | head -1 | awk '{print $2}' || true)
            fail "Step 18b: DNS proxy toggle — proxy ON" "Got reply from ${_REPLY_IP:-unknown}, expected ${WAN_IP}"
        else
            fail "Step 18b: DNS proxy toggle — proxy ON" "No ICMP reply received after re-enabling DNS proxy"
        fi
    fi
}
