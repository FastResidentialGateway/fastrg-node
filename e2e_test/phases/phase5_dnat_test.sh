#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 5 — WAN→LAN DNAT Test (scapy + netcat)
# ---------------------------------------------------------------------------
phase5_dnat_test() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 5 — WAN→LAN DNAT (Step 15)"
    bold "═══════════════════════════════════════════════════════"

    info "Step 15: WAN→LAN DNAT — scapy from WAN, nc listen on LAN..."

    # Get port-mapping eport/dport from etcd (already fetched in phase1 as HSI_JSON)
    # Re-fetch in case phase1 was skipped or HSI_JSON is out of scope
    _HSI_ETCD=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
    PM_COUNT=$(printf '%s' "$_HSI_ETCD" | jq -r '(.config["port-mapping"] // []) | length' 2>/dev/null || echo "0")

    if [[ "$PM_COUNT" -eq 0 ]]; then
        skip "Step 15: WAN→LAN DNAT" "No port-mapping in etcd — cannot perform DNAT test"
        return
    fi

    # Use first port-mapping entry
    DNAT_EPORT=$(printf '%s' "$_HSI_ETCD" | jq -r '.config["port-mapping"][0].eport')
    DNAT_DIP=$(printf '%s'   "$_HSI_ETCD" | jq -r '.config["port-mapping"][0].dip')
    DNAT_DPORT=$(printf '%s' "$_HSI_ETCD" | jq -r '.config["port-mapping"][0].dport')
    info "  DNAT rule: WAN:${DNAT_EPORT} → LAN ${DNAT_DIP}:${DNAT_DPORT}"

    # Fetch PPPoE client WAN IP for this user from gRPC (ip_addr in HsiInfo)
    _HSI_GRPC=$(fastrg_grpc get_hsi_info)
    DNAT_PPP_IP=$(printf '%s' "$_HSI_GRPC" | \
        jq -r ".hsi_infos[] | select(.user_id == ${USER_ID}) | .ip_addr" 2>/dev/null || true)
    if [[ -z "$DNAT_PPP_IP" ]]; then
        fail "Step 15: WAN→LAN DNAT" "Cannot determine PPPoE client WAN IP for USER_ID=${USER_ID} from gRPC"
        return
    fi
    info "  PPPoE WAN IP (gRPC ip_addr): ${DNAT_PPP_IP}"

    # Start nc UDP listener on LAN host at the DNAT destination port (no root needed)
    NC_OUT_FILE=$(mktemp /tmp/fastrg_nc_XXXXXX)
    ssh_lan "timeout 10 nc -u -l -p ${DNAT_DPORT} 2>&1" \
        > "$NC_OUT_FILE" 2>&1 &
    NC_PID=$!

    sleep 2

    # Send UDP packet from WAN host using scapy to WAN IP:eport
    SCAPY_CMD="python3 -c \"from scapy.all import Ether,IP,UDP,Raw,sendp; pkt=Ether(dst='${FASTRG_NODE_MAC}',src='${WAN_HOST_MAC}')/IP(src='${WAN_IP}',dst='${DNAT_PPP_IP}',ttl=64,id=0x4003)/UDP(sport=54321,dport=${DNAT_EPORT})/Raw(load=b'hello'); sendp(pkt, iface='${WAN_NIC}')\" 2>&1"
    SCAPY_OUT=$(ssh_wan "$SCAPY_CMD" 2>&1 || true)
    info "  scapy output: ${SCAPY_OUT}"

    # Wait for nc to finish
    wait $NC_PID 2>/dev/null || true
    NC_OUT=$(cat "$NC_OUT_FILE")
    rm -f "$NC_OUT_FILE"

    info "  nc output: ${NC_OUT:-<empty — no data received>}"

    # nc prints nothing on success (just exits with the received data) or "hello" payload
    # Also check exit: nc -l exits after receiving one packet (timeout 10 will exit even without data)
    # We consider PASS if nc received data (payload "hello") or exited cleanly within timeout
    if printf '%s' "$NC_OUT" | grep -q "hello"; then
        pass "Step 15: WAN→LAN DNAT" "UDP payload 'hello' received on LAN ${DNAT_DIP}:${DNAT_DPORT}"
    else
        fail "Step 15: WAN→LAN DNAT" "scapy failed to send or nc did not receive on LAN ${DNAT_DIP}:${DNAT_DPORT}"
    fi
}
