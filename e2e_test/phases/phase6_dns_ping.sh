#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 6 — DNS Static Record + Reverse Ping
# ---------------------------------------------------------------------------
phase6_dns_ping() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 6 — DNS Static + Reverse Ping (Step 16)"
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
}
