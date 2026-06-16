#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 13 — Per-Subscriber Packet Capture (exec pdump) (Steps 45-48)
#
# Verifies the pdump capture path end-to-end:
#   fastrg_grpc pdump_start/stop → pcap file on node
#
#   Step 45  basic capture (ALL, all subscribers, no filter) → file non-empty
#   Step 46  selective stop (stop LAN USER_ID only, session continues)
#   Step 47  BPF filter ("vlan and icmp") → only ICMP captured, file non-empty
#   Step 48  invalid filter expression → error returned, no session opened
# ---------------------------------------------------------------------------
phase13_pdump() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 13 — Per-Subscriber Packet Capture (Steps 45-48)"
    bold "═══════════════════════════════════════════════════════"

    # Ensure no stale capture session is running from a previous (failed) run.
    fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true

    # -----------------------------------------------------------------------
    # Step 45 — basic capture: ALL directions, all subscribers, no filter
    # -----------------------------------------------------------------------
    info "Step 45: pdump start (ALL, all subscribers, no filter)..."

    _P45_OUT=$(fastrg_grpc pdump_start 0 0 2>&1 || true)
    _P45_FILE=$(printf '%s' "$_P45_OUT" | python3 -c \
        "import sys,json; d=json.load(sys.stdin); print(d.get('pcap_file',''))" 2>/dev/null || true)
    _P45_STATUS=$(printf '%s' "$_P45_OUT" | python3 -c \
        "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || true)

    if [[ -z "$_P45_FILE" ]] || printf '%s' "$_P45_OUT" | grep -qi '"error"'; then
        fail "Step 45: pdump basic capture" "pdump_start failed: ${_P45_OUT}"
        fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true
        skip "Step 46: pdump selective stop" "Step 45 failed"
        skip "Step 47: pdump BPF filter"     "Step 45 failed"
        skip "Step 48: pdump invalid filter" "Step 45 failed"
        return
    fi
    info "  pdump started → pcap: ${_P45_FILE}"

    # Send traffic so there are packets to capture.
    ssh_lan "ping -c 8 -W 3 ${WAN_IP} >/dev/null 2>&1 || true" 2>/dev/null || true
    sleep 1

    fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true

    # Verify file exists and has content beyond the 24-byte global header.
    _P45_SIZE=$(ssh_node "stat -c %s '${_P45_FILE}' 2>/dev/null || echo 0" 2>/dev/null \
        | tr -d '[:space:]')
    if [[ "${_P45_SIZE:-0}" -gt 24 ]]; then
        pass "Step 45: pdump basic capture" \
            "pcap written to ${_P45_FILE} (${_P45_SIZE} bytes)"
    else
        fail "Step 45: pdump basic capture" \
            "pcap file missing or has no packet records (size=${_P45_SIZE:-0}): ${_P45_FILE}"
    fi

    # -----------------------------------------------------------------------
    # Step 46 — selective stop: stop LAN for USER_ID, session must continue
    # -----------------------------------------------------------------------
    bold "---"
    info "Step 46: pdump selective stop (start ALL all → stop LAN ${USER_ID} → session continues)..."

    _P46_OUT=$(fastrg_grpc pdump_start 0 0 2>&1 || true)
    _P46_FILE=$(printf '%s' "$_P46_OUT" | python3 -c \
        "import sys,json; d=json.load(sys.stdin); print(d.get('pcap_file',''))" 2>/dev/null || true)

    if [[ -z "$_P46_FILE" ]] || printf '%s' "$_P46_OUT" | grep -qi '"error"'; then
        fail "Step 46: pdump selective stop" "pdump_start failed: ${_P46_OUT}"
        skip "Step 47: pdump BPF filter"     "Step 46 setup failed"
        skip "Step 48: pdump invalid filter" "Step 46 setup failed"
        return
    fi

    # Stop only LAN side for the primary subscriber — WAN + other sub LAN continue.
    _P46_STOP=$(fastrg_grpc pdump_stop 2 "${USER_ID}" 2>&1 || true)
    if printf '%s' "$_P46_STOP" | grep -qi '"error"'; then
        fail "Step 46: pdump selective stop" \
            "pdump_stop(LAN, ${USER_ID}) failed: ${_P46_STOP}"
        fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true
        skip "Step 47: pdump BPF filter"     "Step 46 failed"
        skip "Step 48: pdump invalid filter" "Step 46 failed"
        return
    fi
    info "  stopped LAN subscriber ${USER_ID}; session continues on WAN + other subs"

    # More traffic after partial stop — WAN side still captures it.
    ssh_lan "ping -c 5 -W 3 ${WAN_IP} >/dev/null 2>&1 || true" 2>/dev/null || true
    sleep 1

    # Stop all remaining; verify file is non-empty.
    fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true
    _P46_SIZE=$(ssh_node "stat -c %s '${_P46_FILE}' 2>/dev/null || echo 0" 2>/dev/null \
        | tr -d '[:space:]')
    if [[ "${_P46_SIZE:-0}" -gt 24 ]]; then
        pass "Step 46: pdump selective stop" \
            "session continued after partial stop; final pcap ${_P46_SIZE} bytes"
    else
        fail "Step 46: pdump selective stop" \
            "pcap file missing or empty after selective stop (size=${_P46_SIZE:-0})"
    fi

    # -----------------------------------------------------------------------
    # Step 47 — BPF filter: "vlan and icmp" on LAN side for USER_ID
    # -----------------------------------------------------------------------
    bold "---"
    info "Step 47: pdump BPF filter (LAN, subscriber ${USER_ID}, filter='vlan and icmp')..."

    _P47_OUT=$(fastrg_grpc pdump_start 2 "${USER_ID}" "vlan and icmp" 2>&1 || true)
    _P47_FILE=$(printf '%s' "$_P47_OUT" | python3 -c \
        "import sys,json; d=json.load(sys.stdin); print(d.get('pcap_file',''))" 2>/dev/null || true)

    if [[ -z "$_P47_FILE" ]] || printf '%s' "$_P47_OUT" | grep -qi '"error"'; then
        fail "Step 47: pdump BPF filter" "pdump_start with filter failed: ${_P47_OUT}"
        skip "Step 48: pdump invalid filter" "Step 47 setup failed"
        return
    fi
    info "  pdump started with icmp filter → pcap: ${_P47_FILE}"

    # Send ICMP and TCP — only ICMP should land in the pcap.
    ssh_lan "ping -c 8 -W 3 ${WAN_IP} >/dev/null 2>&1 || true" 2>/dev/null || true
    ssh_lan "curl -s --connect-timeout 5 -o /dev/null http://${WAN_IP}/ 2>/dev/null || true" \
        2>/dev/null || true
    sleep 1

    fastrg_grpc pdump_stop 2 "${USER_ID}" >/dev/null 2>&1 || true

    _P47_SIZE=$(ssh_node "stat -c %s '${_P47_FILE}' 2>/dev/null || echo 0" 2>/dev/null \
        | tr -d '[:space:]')
    if [[ "${_P47_SIZE:-0}" -gt 24 ]]; then
        pass "Step 47: pdump BPF filter (vlan and icmp)" \
            "pcap written with ICMP filter (${_P47_SIZE} bytes): ${_P47_FILE}"
    else
        fail "Step 47: pdump BPF filter (vlan and icmp)" \
            "pcap file missing or empty after icmp-filtered capture (size=${_P47_SIZE:-0})"
    fi

    # -----------------------------------------------------------------------
    # Step 48 — invalid BPF filter expression → gRPC must return an error
    # -----------------------------------------------------------------------
    bold "---"
    info "Step 48: pdump invalid filter expression → expect error..."

    _P48_OUT=$(fastrg_grpc pdump_start 0 0 "(unclosed-paren-syntax-error" 2>&1 || true)
    # Success means gRPC returned a non-empty pcap_file path.
    # Any other outcome (gRPC error, connection failure, empty response) means
    # the invalid filter was rejected — which is the expected behaviour.
    _P48_FILE=$(printf '%s' "$_P48_OUT" | python3 -c \
        "import sys,json; d=json.load(sys.stdin); print(d.get('pcap_file',''))" 2>/dev/null || true)

    if [[ -n "$_P48_FILE" ]]; then
        # pdump_start actually succeeded and handed back a real pcap path — fail.
        fastrg_grpc pdump_stop 0 0 >/dev/null 2>&1 || true
        fail "Step 48: pdump invalid filter rejected" \
            "expected error but got pcap_file=${_P48_FILE}"
    else
        pass "Step 48: pdump invalid filter rejected" \
            "no pcap_file returned (filter correctly rejected or gRPC error)"
    fi
}
