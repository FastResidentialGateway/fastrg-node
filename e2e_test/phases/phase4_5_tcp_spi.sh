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
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" || true
    sleep 1
    ssh_wan "iperf3 -s -B ${WAN_IP} -p ${SRV_PORT} -D >/dev/null 2>&1 || true" || true
    for _iw in $(seq 1 10); do
        sleep 1
        ssh_wan "ss -ltn 2>/dev/null | grep -q ':${SRV_PORT}'" 2>/dev/null && break
    done

    # ------------------------------------------------------------------
    # 3. Run iperf3 client from LAN for 30s in the background, leaving a
    #    live ESTABLISHED entry to inject against.
    #    Steps 13-15 take ~23s total, so 30s gives adequate margin.
    # ------------------------------------------------------------------
    info "Initiating iperf3 client from LAN host (30s)..."
    # Do NOT fix the client source port with --cport: a fixed port lingers in
    # TIME_WAIT for 60s after Step 11's iperf3 ends, causing "Address already
    # in use" which means no flow is ever established and tcpdump finds nothing.
    # Let the OS pick an ephemeral source port; we discover it via tcpdump.
    ssh_lan "(iperf3 -c ${WAN_IP} -p ${SRV_PORT} -t 30 -J >/dev/null 2>&1) &" || true
    # Wait until the iperf3 TCP flow is actually ESTABLISHED before starting
    # tcpdump — otherwise a brief NAT-table warmup delay causes a race where
    # tcpdump runs for its 6-second window before any packets arrive.
    _spi_flow_ready=0
    for _sw in $(seq 1 10); do
        sleep 1
        _flow=$(ssh_lan "ss -tn 'dport = :${SRV_PORT}' 2>/dev/null | grep ESTAB || true")
        if [[ -n "$_flow" ]]; then
            _spi_flow_ready=1
            info "  iperf3 flow ESTABLISHED after ${_sw}s"
            break
        fi
    done
    [[ $_spi_flow_ready -eq 0 ]] && info "  iperf3 flow not yet ESTABLISHED; proceeding anyway"
    sleep 2

    # ------------------------------------------------------------------
    # 4. Discover the NAT source port by sniffing any packet of the flow on
    #    the WAN side (no SYN filter — we missed the handshake by now).
    # ------------------------------------------------------------------
    info "Discovering NAT source port via tcpdump on WAN..."
    local TCPDUMP_OUT
    # -S: absolute seq/ack numbers so we can extract the WAN-server ack baseline
    TCPDUMP_OUT=$(ssh_wan "timeout 6 tcpdump -S -nn -i ${WAN_NIC} -c 1 'tcp and src host ${FASTRG_PUB_IP} and dst port ${SRV_PORT}' 2>&1" || true)
    local NAT_PORT=""
    NAT_PORT=$(printf '%s' "$TCPDUMP_OUT" | grep -oE "${FASTRG_PUB_IP}\\.[0-9]+" 2>/dev/null | head -1 | awk -F'.' '{print $NF}' || true)

    if [[ -z "$NAT_PORT" ]]; then
        fail "Phase 4.5: TCP SPI" "Could not capture NAT source port from WAN tcpdump"
        ssh_lan "pkill -f 'iperf3 -c ${WAN_IP}' 2>/dev/null || true" || true
        ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" || true
        return
    fi
    info "NAT source port: ${NAT_PORT}"

    # Derive a seq value guaranteed to be outside seq_valid's ±16 MB window.
    # The captured packet is LAN→WAN; its ack field ≈ max_ack_lan (WAN server's seq).
    # We use 512 MB offset: at 50 Mbps, iperf3 advances max_ack_lan by ~62 MB in
    # the ~10 s between capture and step-14 injection, leaving 450 MB of margin
    # above the ±16 MB acceptance window.  32-bit wrap is handled correctly by
    # TCP seq arithmetic (uint32_t subtraction cast to int32_t).
    local INJECT_SEQ_BASE INJECT_SEQ
    INJECT_SEQ_BASE=$(printf '%s' "$TCPDUMP_OUT" | grep -oE "ack [0-9]+" | head -1 | awk '{print $2}' || true)
    if [[ -n "$INJECT_SEQ_BASE" && "$INJECT_SEQ_BASE" -gt 0 ]]; then
        INJECT_SEQ=$(( (INJECT_SEQ_BASE + 536870912) % 4294967296 ))
    else
        INJECT_SEQ=3735928559  # 0xDEADBEEF fallback
    fi
    info "WAN-server ack base: ${INJECT_SEQ_BASE}, step-14 inject seq: ${INJECT_SEQ}"

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
pkt = Ether(dst='${FASTRG_NODE_WAN_MAC}', src='${WAN_HOST_MAC}') \
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

    info "Injecting ACK with out-of-window seq (inject_seq=${INJECT_SEQ})..."
    ssh_wan "python3 -c \"
from scapy.all import Ether,IP,TCP,sendp
pkt = Ether(dst='${FASTRG_NODE_WAN_MAC}', src='${WAN_HOST_MAC}') \
    / IP(src='${WAN_IP}', dst='${FASTRG_PUB_IP}', ttl=64) \
    / TCP(sport=${SRV_PORT}, dport=${NAT_PORT}, flags='A', seq=${INJECT_SEQ}, ack=0xCAFEBABE)
sendp(pkt, iface='${WAN_NIC}', verbose=0)
\" 2>&1 || true"
    sleep 5

    DROP_AFTER=$(_spi_drop_count)
    DROP_DELTA=$(( DROP_AFTER - DROP_BEFORE ))
    info "  WAN dropped_packets after seq injection: ${DROP_AFTER} (delta=${DROP_DELTA})"

    if [[ "$DROP_DELTA" -ge 1 ]]; then
        pass "Step 14: Out-of-window seq dropped" "WAN drop counter delta=${DROP_DELTA}"
    else
        fail "Step 14: Out-of-window seq dropped" "Expected WAN drop counter to increase by ≥1, delta=${DROP_DELTA}"
    fi

    # ------------------------------------------------------------------
    # 7. Step 15 — tcp_conntrack toggle
    #    Phase A: disable → inject SYN → forwarded to LAN; verified by LAN-side tcpdump.
    #    Phase B: re-enable → inject SYN again → ESTABLISHED disallows SYN → drop.
    # ------------------------------------------------------------------
    info "Step 15a: Disabling TCP conntrack via gRPC..."
    fastrg_grpc set_tcp_conntrack "${USER_ID}" false 2>&1 || true
    sleep 1

    # Primary verification: LAN-side tcpdump (requires sudo on LAN host).
    # Backup verification: TCPSYNChallenge kernel counter — when the forwarded SYN
    # hits an ESTABLISHED socket (iperf3 on an ephemeral port), the kernel sends a
    # challenge ACK (RFC 5961) and increments this counter; no root needed.
    local _LAN_IFACE
    _LAN_IFACE=$(ssh_lan "ip -o route get ${WAN_IP} 2>/dev/null | awk 'NR==1{for(i=1;i<=NF;i++) if(\$i==\"dev\"){print \$(i+1); exit}}'" || echo "")
    _LAN_IFACE=${_LAN_IFACE:-any}
    info "  LAN subscriber interface for tcpdump: ${_LAN_IFACE}"

    # Snapshot TCPSYNChallenge before injection (no root required)
    local _SYNCHALLENGE_BEFORE
    _SYNCHALLENGE_BEFORE=$(ssh_lan "awk '/^TcpExt:/{n=split(\$0,h); getline l; split(l,v); for(i=1;i<=n;i++) if(h[i]==\"TCPSYNChallenge\"){print v[i]; exit}}' /proc/net/netstat 2>/dev/null || echo 0" | tr -d '[:space:]')
    _SYNCHALLENGE_BEFORE=${_SYNCHALLENGE_BEFORE:-0}
    info "  TCPSYNChallenge baseline: ${_SYNCHALLENGE_BEFORE}"

    info "Starting tcpdump on LAN to detect forwarded SYN (conntrack disabled)..."
    # sudo required: 'the' user lacks cap_net_raw on vlan interfaces
    ssh_lan "rm -f /tmp/p45_15a.txt; nohup sudo timeout 8 tcpdump -l -nn -i '${_LAN_IFACE}' 'tcp and src host ${WAN_IP}' > /tmp/p45_15a.txt 2>&1 < /dev/null &" || true
    sleep 2

    info "Injecting SYN with tcp_conntrack disabled (should be forwarded to LAN)..."
    ssh_wan "python3 -c \"
from scapy.all import Ether,IP,TCP,sendp
pkt = Ether(dst='${FASTRG_NODE_WAN_MAC}', src='${WAN_HOST_MAC}') \
    / IP(src='${WAN_IP}', dst='${FASTRG_PUB_IP}', ttl=64) \
    / TCP(sport=${SRV_PORT}, dport=${NAT_PORT}, flags='S', seq=0x12345678)
sendp(pkt, iface='${WAN_NIC}', verbose=0)
\" 2>&1 || true"
    sleep 4

    # timeout 8 should have expired by now (~2+1+4=7s elapsed); also sudo-pkill as fallback
    ssh_lan "sudo pkill -f 'tcpdump' 2>/dev/null || pkill -f 'tcpdump' 2>/dev/null || true" || true
    sleep 1
    local _P45_15A_CAP
    _P45_15A_CAP=$(ssh_lan "cat /tmp/p45_15a.txt 2>/dev/null; rm -f /tmp/p45_15a.txt" || true)
    info "  LAN tcpdump output (first 500 chars): ${_P45_15A_CAP:0:500}"

    local _SYNCHALLENGE_AFTER _SYNCHALLENGE_DELTA
    _SYNCHALLENGE_AFTER=$(ssh_lan "awk '/^TcpExt:/{n=split(\$0,h); getline l; split(l,v); for(i=1;i<=n;i++) if(h[i]==\"TCPSYNChallenge\"){print v[i]; exit}}' /proc/net/netstat 2>/dev/null || echo 0" | tr -d '[:space:]')
    _SYNCHALLENGE_AFTER=${_SYNCHALLENGE_AFTER:-0}
    _SYNCHALLENGE_DELTA=$(( _SYNCHALLENGE_AFTER - _SYNCHALLENGE_BEFORE ))
    info "  TCPSYNChallenge after: ${_SYNCHALLENGE_AFTER} (delta=${_SYNCHALLENGE_DELTA})"

    # Pass if tcpdump captured the forwarded SYN (primary) OR kernel challenge counter rose (backup).
    # tcpdump -nn: "IP WAN_IP.SRV_PORT > LAN_IP.<any_port>: Flags [S], ..."
    # Use src-port filter (WAN_IP.SRV_PORT) rather than dst-port (CLIENT_CPORT)
    # because the client source port is now ephemeral (not fixed).
    if printf '%s' "$_P45_15A_CAP" | grep -qE "\.${SRV_PORT} >.*Flags \[S\]"; then
        pass "Step 15a: SYN forwarded to LAN when conntrack disabled" "tcpdump captured forwarded SYN from port ${SRV_PORT}"
    elif [[ "$_SYNCHALLENGE_DELTA" -ge 1 ]]; then
        pass "Step 15a: SYN forwarded to LAN when conntrack disabled" \
            "LAN kernel sent challenge ACK (TCPSYNChallenge delta=${_SYNCHALLENGE_DELTA}); tcpdump unavailable (iface=${_LAN_IFACE}, sudo required)"
    else
        fail "Step 15a: SYN forwarded to LAN when conntrack disabled" \
            "Neither tcpdump nor TCPSYNChallenge confirmed forwarding (iface=${_LAN_IFACE}, synchallenge_delta=${_SYNCHALLENGE_DELTA})"
    fi

    info "Step 15b: Re-enabling TCP conntrack via gRPC..."
    fastrg_grpc set_tcp_conntrack "${USER_ID}" true 2>&1 || true
    sleep 1

    DROP_BEFORE=$(_spi_drop_count)
    info "  WAN dropped_packets before SYN injection (conntrack ON): ${DROP_BEFORE}"

    info "Injecting 2x SYN with tcp_conntrack re-enabled (ESTABLISHED state → should drop)..."
    ssh_wan "python3 -c \"
from scapy.all import Ether,IP,TCP,sendp
pkt = Ether(dst='${FASTRG_NODE_WAN_MAC}', src='${WAN_HOST_MAC}') \
    / IP(src='${WAN_IP}', dst='${FASTRG_PUB_IP}', ttl=64) \
    / TCP(sport=${SRV_PORT}, dport=${NAT_PORT}, flags='S', seq=0x12345678)
sendp(pkt, iface='${WAN_NIC}', verbose=0, count=2)
\" 2>&1 || true"
    sleep 2

    DROP_AFTER=$(_spi_drop_count)
    DROP_DELTA=$(( DROP_AFTER - DROP_BEFORE ))
    info "  WAN dropped_packets after SYN injection (conntrack ON): ${DROP_AFTER} (delta=${DROP_DELTA})"

    # Require delta >= 2: both injected SYNs are dropped by ESTABLISHED state check.
    # tcp_state stayed ESTABLISHED because the conntrack FSM was not updated while
    # conntrack was disabled.
    if [[ "$DROP_DELTA" -ge 2 ]]; then
        pass "Step 15b: SYN dropped when conntrack re-enabled" "WAN drop counter delta=${DROP_DELTA}"
    else
        fail "Step 15b: SYN dropped when conntrack re-enabled" "Expected delta ≥2, got ${DROP_DELTA}"
    fi

    # ------------------------------------------------------------------
    # 8. Cleanup
    # ------------------------------------------------------------------
    ssh_lan "pkill -f 'iperf3 -c ${WAN_IP}' 2>/dev/null || true" || true
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" || true
}
