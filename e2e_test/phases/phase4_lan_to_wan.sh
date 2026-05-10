#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 4 — LAN→WAN Traffic Tests
# ---------------------------------------------------------------------------
phase4_lan_to_wan() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 4 — LAN→WAN Traffic (Steps 10-12)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 10 — Ping
    # ------------------------------------------------------------------
    info "Step 10: Ping ${WAN_IP} from LAN host ${LAN_HOST}..."
    PING_OUT=$(ssh_lan "ping -c 4 -W 3 ${WAN_IP} 2>&1" || true)
    if printf '%s' "$PING_OUT" | grep -qE "0% packet loss|0\.0% packet loss"; then
        pass "Step 10: Ping LAN→WAN" "${WAN_IP} reachable, 0% packet loss"
    else
        LOSS=$(printf '%s' "$PING_OUT" | grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || echo "no response")
        fail "Step 10: Ping LAN→WAN" "${WAN_IP} - ${LOSS}"
    fi

    # ------------------------------------------------------------------
    # Step 11 — iperf3
    # ------------------------------------------------------------------
    info "Step 11: iperf3 test (LAN→WAN, port ${SRV_PORT}, cport 47792)..."
    # Start iperf3 server on WAN host (daemon mode)
    ssh_wan "iperf3 -s -B ${WAN_IP} -p ${SRV_PORT} -D --forceflush >/dev/null 2>&1 || true" || true
    sleep 2

    IPERF_OUT=$(ssh_lan "iperf3 -c ${WAN_IP} -p ${SRV_PORT} --cport 47792 -t 5 -J 2>&1" || true)
    # Cleanup iperf3 server
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" || true

    if [[ -z "$IPERF_OUT" ]]; then
        fail "Step 11: iperf3 LAN→WAN" "No output from iperf3 client"
    else
        BPS=$(printf '%s' "$IPERF_OUT" | jq -r '.end.sum_received.bits_per_second // 0' 2>/dev/null || echo "0")
        BPS_INT=$(printf '%.0f' "${BPS}" 2>/dev/null || echo "0")
        if [[ $BPS_INT -gt 0 ]]; then
            # Format as Mbps for readability
            MBPS=$(awk "BEGIN {printf \"%.2f\", $BPS_INT / 1000000}")
            pass "Step 11: iperf3 LAN→WAN" "Received ${MBPS} Mbps"
        else
            ERR=$(printf '%s' "$IPERF_OUT" | jq -r '.error // empty' 2>/dev/null || true)
            fail "Step 11: iperf3 LAN→WAN" "bits_per_second=0${ERR:+; error: $ERR}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 12 — curl
    # ------------------------------------------------------------------
    info "Step 12: curl http://www.google.com from LAN host ${LAN_HOST}..."
    HTTP_CODE=$(ssh_lan "curl -s -o /dev/null -w '%{http_code}' --max-time 15 http://www.google.com 2>&1" || echo "000")
    HTTP_CODE=$(printf '%s' "$HTTP_CODE" | tr -d "'" | tr -d '[:space:]')

    case "$HTTP_CODE" in
        200|301|302)
            pass "Step 12: curl www.google.com" "HTTP ${HTTP_CODE}"
            ;;
        000)
            fail "Step 12: curl www.google.com" "Connection failed or timed out (HTTP 000)"
            ;;
        *)
            fail "Step 12: curl www.google.com" "Unexpected HTTP status: ${HTTP_CODE}"
            ;;
    esac
}
