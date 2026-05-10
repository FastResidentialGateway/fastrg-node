#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 4.5 — Reverse-direction TCP SPI verification
#
# Establishes a real LAN→WAN TCP flow, then injects crafted packets from the
# WAN side that should be silently dropped by SPI:
#   - flag-state mismatch  (SYN against an ESTABLISHED entry)
#   - out-of-window seq/ack (R1 sequence validation)
#
# Verification is by reading the per-user WAN dropped_packets counter via
# GetFastrgSystemStats gRPC before and after each injection, and confirming
# the delta is ≥ 1.
# ---------------------------------------------------------------------------
phase4_5_tcp_spi() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 4.5 — Reverse-direction TCP SPI"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Helper: read WAN dropped_packets for USER_ID via gRPC stats.
    # Returns a plain integer; 0 on any error.
    # ------------------------------------------------------------------
    _spi_drop_count() {
        fastrg_grpc get_user_drop_count "${USER_ID}" 1 2>/dev/null \
            | jq -r '.dropped_packets // 0' 2>/dev/null \
            | tr -d '[:space:]' \
            || echo 0
    }

    # ------------------------------------------------------------------
    # 1. Look up FastRG public IP (PPPoE-assigned) for this user
    # ------------------------------------------------------------------
    local _HSI_GRPC
    _HSI_GRPC=$(fastrg_grpc get_hsi_info)
    local FASTRG_PUB_IP
    FASTRG_PUB_IP=$(printf '%s' "$_HSI_GRPC" | \
        jq -r ".hsi_infos[] | select(.user_id == ${USER_ID}) | .ip_addr" 2>/dev/null || true)
    if [[ -z "$FASTRG_PUB_IP" ]]; then
        fail "Phase 4.5: TCP SPI" "Cannot determine FastRG public IP for USER_ID=${USER_ID}"
        return
    fi
    info "FastRG public IP: ${FASTRG_PUB_IP}"

    # ------------------------------------------------------------------
    # 2. Start iperf3 server on WAN.
    # ------------------------------------------------------------------
    info "Starting iperf3 server on WAN host (port ${SRV_PORT})..."
    ssh_wan "iperf3 -s -B ${WAN_IP} -p ${SRV_PORT} -D >/dev/null 2>&1 || true" || true
    sleep 2

    # ------------------------------------------------------------------
    # 3. Run iperf3 client from LAN for 15s in the background, leaving a
    #    live ESTABLISHED entry to inject against.
    # ------------------------------------------------------------------
    info "Initiating iperf3 client from LAN host (cport ${CLIENT_CPORT}, 15s)..."
    ssh_lan "(iperf3 -c ${WAN_IP} -p ${SRV_PORT} --cport ${CLIENT_CPORT} -t 15 -J >/dev/null 2>&1) &" || true
    sleep 4

    # ------------------------------------------------------------------
    # 4. Discover the NAT source port by sniffing any packet of the flow on
    #    the WAN side (no SYN filter — we missed the handshake by now).
    # ------------------------------------------------------------------
    info "Discovering NAT source port via tcpdump on WAN..."
    local TCPDUMP_OUT
    TCPDUMP_OUT=$(ssh_wan "timeout 6 tcpdump -nn -i ${WAN_NIC} -c 1 'tcp and src host ${FASTRG_PUB_IP} and dst port ${SRV_PORT}' 2>&1" || true)
    local NAT_PORT=""
    NAT_PORT=$(printf '%s' "$TCPDUMP_OUT" | grep -oE "${FASTRG_PUB_IP}\\.[0-9]+" 2>/dev/null | head -1 | awk -F'.' '{print $NF}' || true)

    if [[ -z "$NAT_PORT" ]]; then
        fail "Phase 4.5: TCP SPI" "Could not capture NAT source port from WAN tcpdump"
        ssh_lan "pkill -f 'iperf3 -c ${WAN_IP}' 2>/dev/null || true" || true
        ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" || true
        return
    fi
    info "NAT source port: ${NAT_PORT}"

    # ------------------------------------------------------------------
    # 5. Step 13 — SYN→ESTABLISHED flag mismatch
    #    Snapshot drop counter, inject SYN, verify delta ≥ 1.
    # ------------------------------------------------------------------
    local DROP_BEFORE DROP_AFTER DROP_DELTA

    DROP_BEFORE=$(_spi_drop_count)
    info "  WAN dropped_packets before SYN injection: ${DROP_BEFORE}"

    info "Injecting SYN to ESTABLISHED 4-tuple (flag mismatch)..."
    ssh_wan "python3 -c \"
from scapy.all import Ether,IP,TCP,sendp
pkt = Ether(dst='${FASTRG_NODE_MAC}', src='${WAN_HOST_MAC}') \
    / IP(src='${WAN_IP}', dst='${FASTRG_PUB_IP}', ttl=64) \
    / TCP(sport=${SRV_PORT}, dport=${NAT_PORT}, flags='S', seq=0x12345678)
sendp(pkt, iface='${WAN_NIC}', verbose=0)
\" 2>&1 || true"
    sleep 2

    DROP_AFTER=$(_spi_drop_count)
    DROP_DELTA=$(( DROP_AFTER - DROP_BEFORE ))
    info "  WAN dropped_packets after SYN injection: ${DROP_AFTER} (delta=${DROP_DELTA})"

    if [[ "$DROP_DELTA" -ge 1 ]]; then
        pass "Step 13: SYN→ESTABLISHED dropped" "WAN drop counter delta=${DROP_DELTA}"
    else
        fail "Step 13: SYN→ESTABLISHED dropped" "Expected WAN drop counter to increase by ≥1, delta=${DROP_DELTA}"
    fi

    # ------------------------------------------------------------------
    # 6. Step 14 — out-of-window seq
    #    Snapshot drop counter, inject ACK with bogus seq, verify delta ≥ 1.
    # ------------------------------------------------------------------
    DROP_BEFORE=$(_spi_drop_count)
    info "  WAN dropped_packets before seq injection: ${DROP_BEFORE}"

    info "Injecting ACK with out-of-window seq..."
    ssh_wan "python3 -c \"
from scapy.all import Ether,IP,TCP,sendp
pkt = Ether(dst='${FASTRG_NODE_MAC}', src='${WAN_HOST_MAC}') \
    / IP(src='${WAN_IP}', dst='${FASTRG_PUB_IP}', ttl=64) \
    / TCP(sport=${SRV_PORT}, dport=${NAT_PORT}, flags='A', seq=0xDEADBEEF, ack=0xCAFEBABE)
sendp(pkt, iface='${WAN_NIC}', verbose=0)
\" 2>&1 || true"
    sleep 2

    DROP_AFTER=$(_spi_drop_count)
    DROP_DELTA=$(( DROP_AFTER - DROP_BEFORE ))
    info "  WAN dropped_packets after seq injection: ${DROP_AFTER} (delta=${DROP_DELTA})"

    if [[ "$DROP_DELTA" -ge 1 ]]; then
        pass "Step 14: Out-of-window seq dropped" "WAN drop counter delta=${DROP_DELTA}"
    else
        fail "Step 14: Out-of-window seq dropped" "Expected WAN drop counter to increase by ≥1, delta=${DROP_DELTA}"
    fi

    # ------------------------------------------------------------------
    # 7. Cleanup
    # ------------------------------------------------------------------
    ssh_lan "pkill -f 'iperf3 -c ${WAN_IP}' 2>/dev/null || true" || true
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" || true
}
