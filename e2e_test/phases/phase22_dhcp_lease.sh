#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 22 — DHCP lease lifecycle with an isolated virtual client (Steps 93-96)
# ---------------------------------------------------------------------------

_P22_REMOTE_CLIENT=/tmp/fastrg_dhcp_client_sim.py
_P22_VIRTUAL_MAC=02:e2:e2:00:00:11
_P22_BOGUS_MAC=02:e2:e2:00:00:12
_P22_MACVLAN_IF=mv_p22dhcp
_P22_REAL_LEASE=192.168.4.2
_P22_LEASE_IP=""
_P22_SERVER_ID=""
_P22_SERVER_MAC=""
_P22_LAN_IFACE=""

_p22_snippet() {
    printf '%s' "$1" | tr '\n' ' ' | cut -c 1-500 || true
}

_p22_run_client_for_mac() {
    local _mac="$1"
    shift
    ssh_lan "python3 '${_P22_REMOTE_CLIENT}' --interface '${_P22_LAN_IFACE}' \
        --mac '${_mac}' $*" 2>&1
}

_p22_run_client() {
    _p22_run_client_for_mac "$_P22_VIRTUAL_MAC" "$@"
}

_p22_get_ips() {
    fastrg_grpc get_dhcp_info | jq -c \
        ".dhcp_infos[] | select(.user_id == ${USER_ID}) | (.inuse_ips // [])" \
        2>/dev/null || true
}

_p22_fetch_metric() {
    local _body
    _body=$(ssh_node \
        "curl -fsS --max-time 5 http://127.0.0.1:${_P22_METRICS_PORT}/metrics" \
        2>/dev/null || true)
    printf '%s\n' "$_body" | awk -v uid="$USER_ID" '
        $1 ~ /^fastrg_node_per_user_dhcp_cur_lease_count\{/ &&
        $1 ~ ("user_id=\"" uid "\"") { print $2; exit }
    ' || true
}

_p22_ip_in_list() {
    local _ips="$1" _ip="$2"
    printf '%s' "$_ips" | jq -e --arg ip "$_ip" 'index($ip) != null' >/dev/null 2>&1
}

_p22_poll_state() {
    local _expected_count="$1" _ip="$2" _expect_present="$3"
    local _i _present=0
    _P22_OBS_METRIC=""
    _P22_OBS_IPS=""
    for _i in $(seq 1 15); do
        _P22_OBS_METRIC=$(_p22_fetch_metric)
        _P22_OBS_IPS=$(_p22_get_ips)
        _present=0
        _p22_ip_in_list "$_P22_OBS_IPS" "$_ip" && _present=1
        if [[ "$_P22_OBS_METRIC" == "$_expected_count" ]] && \
           [[ $_present -eq $_expect_present ]]; then
            return 0
        fi
        sleep 2
    done
    return 1
}

_p22_release_best_effort() {
    [[ -z "${_P22_LEASE_IP:-}" || -z "${_P22_SERVER_ID:-}" || \
       -z "${_P22_SERVER_MAC:-}" || -z "${_P22_LAN_IFACE:-}" ]] && return 0
    _p22_run_client --ip "$_P22_LEASE_IP" --server-id "$_P22_SERVER_ID" \
        --server-mac "$_P22_SERVER_MAC" release >/dev/null 2>&1 || true
    return 0
}

_cleanup_phase22_dhcp_lease() {
    set +eu
    _p22_release_best_effort || true
    ssh_lan "pkill -TERM -f '^python3 ${_P22_REMOTE_CLIENT}( |$)' 2>/dev/null || true; \
        rm -f '${_P22_REMOTE_CLIENT}'; \
        ip link del '${_P22_MACVLAN_IF}' 2>/dev/null || true" >/dev/null 2>&1 || true
    return 0
}

phase22_dhcp_lease() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 22 — DHCP Lease Lifecycle (Steps 93-96)"
    bold "═══════════════════════════════════════════════════════"

    local _hsi="" _vlan="" _pool="" _pool_start="" _pool_end=""
    local _baseline_metric="" _baseline_ips="" _baseline_len="" _expected_metric=""
    local _offer="" _offer_type="" _offer_ip="" _offer_xid=""
    local _ack="" _ack_type="" _ack_ip="" _lease_time="" _renewal_time="" _rebinding_time=""
    local _renew="" _renew_type="" _renew_ip=""
    local _rebind="" _rebind_type="" _rebind_ip=""
    local _bogus_ip="" _bogus="" _bogus_type=""
    local _release="" _step91_ok=1 _step92_ok=1 _step93_ok=1 _step94_ok=1
    local _detail="" _i _p22_parent_if=""

    _hsi=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
    _vlan=$(printf '%s' "$_hsi" | jq -r '.config.vlan_id // empty' 2>/dev/null || true)
    _pool=$(printf '%s' "$_hsi" | jq -r '.config.dhcp_addr_pool // empty' 2>/dev/null || true)
    _pool_start="${_pool%-*}"
    _pool_end="${_pool#*-}"
    _P22_LAN_IFACE="vlan${_vlan}"

    _P22_METRICS_PORT=$(ssh_node \
        "grep 'MetricsListenPort' /etc/fastrg/config.cfg 2>/dev/null" | \
        awk -F'"' '{print $2}' | awk -F: '{print $NF}' || true)

    # Phase 19 cold-starts the server while NetworkManager keeps the LAN host's
    # still-valid address. Re-acquire the fixture lease so the restarted server
    # has an authoritative baseline before the isolated virtual client is added.
    info "Refreshing LAN fixture lease before virtual-client baseline (${_P22_LAN_IFACE})..."
    ssh_lan "nmcli con down netplan-vlan3 >/dev/null 2>&1; \
        nmcli con up netplan-vlan3 >/dev/null 2>&1 || netplan apply >/dev/null 2>&1; true" \
        2>/dev/null || true
    for _i in $(seq 1 12); do
        _baseline_metric=$(_p22_fetch_metric)
        _baseline_ips=$(_p22_get_ips)
        if [[ "$_baseline_metric" == "1" ]] && \
           _p22_ip_in_list "$_baseline_ips" "$_P22_REAL_LEASE"; then
            break
        fi
        sleep 2
    done
    _baseline_metric=$(_p22_fetch_metric)
    _baseline_ips=$(_p22_get_ips)
    _baseline_len=$(printf '%s' "$_baseline_ips" | jq -r 'length' 2>/dev/null || true)

    if [[ -z "$_vlan" || -z "$_pool_start" || -z "$_pool_end" || \
          -z "$_P22_METRICS_PORT" || ! "$_baseline_metric" =~ ^[0-9]+$ || \
          ! "$_baseline_len" =~ ^[0-9]+$ || "$_baseline_metric" != "$_baseline_len" ]] || \
       ! _p22_ip_in_list "$_baseline_ips" "$_P22_REAL_LEASE" || \
       ! ssh_lan "ip link show '${_P22_LAN_IFACE}' >/dev/null 2>&1 && \
            ip -4 addr show '${_P22_LAN_IFACE}' | grep -Fq '${_P22_REAL_LEASE}/'"; then
        _step91_ok=0
    fi

    if [[ $_step91_ok -eq 1 ]]; then
        scp $SSH_OPTS "${GRPC_CLIENT_DIR}/dhcp_client_sim.py" \
            "root@${LAN_HOST}:${_P22_REMOTE_CLIENT}" >/dev/null 2>&1 || _step91_ok=0
        ssh_lan "chmod 700 '${_P22_REMOTE_CLIENT}'" >/dev/null 2>&1 || _step91_ok=0
    fi

    # The virtual client must also be able to RECEIVE unicast: the renew ACK
    # is unicast to ciaddr per RFC 2131, and on the VF-based LAN topology the
    # PF only delivers unicast frames whose dst MAC is registered in a VF's
    # filter table. Register the virtual MAC via a throwaway macvlan on the
    # VLAN's parent device (the VF); broadcast replies need no registration.
    #
    # Why a macvlan can add a MAC to the VF's hardware filter (Linux kernel
    # source, the peer runs kernel ixgbevf/ixgbe):
    #   1. drivers/net/macvlan.c: macvlan_open() -> dev_uc_add(lowerdev, ...)
    #      — bringing the macvlan up adds its MAC to the lower device's
    #      unicast address list.
    #   2. drivers/net/ethernet/intel/ixgbevf/ixgbevf_main.c:
    #      ixgbevf_set_rx_mode() -> ixgbevf_write_uc_addr_list() ->
    #      hw->mac.ops.set_uc_addr (ixgbevf/vf.c) — the VF driver forwards
    #      that list to the PF as IXGBE_VF_SET_MACVLAN mailbox messages; the
    #      guest never touches the hardware filter itself.
    #   3. drivers/net/ethernet/intel/ixgbe/ixgbe_sriov.c:
    #      ixgbe_set_vf_macvlan_msg() -> ixgbe_set_vf_macvlan() — the PF
    #      validates the request and programs the MAC into a RAR filter
    #      entry mapped to this VF's pool. The PF may deny it ("...but is
    #      administratively denied") when the admin pinned the VF MAC via
    #      "ip link set <pf> vf N mac" (pf_set_mac), so this technique
    #      requires the VF MAC to be auto-assigned (as on this bench).
    if [[ $_step91_ok -eq 1 ]]; then
        _p22_parent_if=$(ssh_lan "ip -o link show '${_P22_LAN_IFACE}'" 2>/dev/null | \
            grep -oE "${_P22_LAN_IFACE}@[^:]+" | cut -d@ -f2 || true)
        if [[ -z "$_p22_parent_if" ]] || \
           ! ssh_lan "ip link del '${_P22_MACVLAN_IF}' 2>/dev/null; \
                ip link add '${_P22_MACVLAN_IF}' link '${_p22_parent_if}' \
                    address '${_P22_VIRTUAL_MAC}' type macvlan mode private && \
                ip link set '${_P22_MACVLAN_IF}' up" >/dev/null 2>&1; then
            _step91_ok=0
        fi
    fi

    if [[ $_step91_ok -eq 1 ]]; then
        _offer=$(_p22_run_client --timeout 8 discover) || _step91_ok=0
        _offer_type=$(printf '%s' "$_offer" | jq -r '.received // empty' 2>/dev/null || true)
        _offer_ip=$(printf '%s' "$_offer" | jq -r '.yiaddr // empty' 2>/dev/null || true)
        _offer_xid=$(printf '%s' "$_offer" | jq -r '.xid // empty' 2>/dev/null || true)
        _P22_SERVER_ID=$(printf '%s' "$_offer" | jq -r '.server_id // empty' 2>/dev/null || true)
        _P22_SERVER_MAC=$(printf '%s' "$_offer" | jq -r '.server_mac // empty' 2>/dev/null || true)
        if [[ "$_offer_type" != "OFFER" || -z "$_offer_ip" || -z "$_offer_xid" || \
              -z "$_P22_SERVER_ID" || -z "$_P22_SERVER_MAC" ]] || \
           ! python3 - "$_offer_ip" "$_pool_start" "$_pool_end" <<'PY'
import ipaddress
import sys

ip, start, end = map(ipaddress.IPv4Address, sys.argv[1:])
raise SystemExit(0 if start <= ip <= end else 1)
PY
        then
            _step91_ok=0
        fi
    fi

    if [[ $_step91_ok -eq 1 ]]; then
        _ack=$(_p22_run_client --timeout 8 --xid "$_offer_xid" --ip "$_offer_ip" \
            --server-id "$_P22_SERVER_ID" request) || _step91_ok=0
        _ack_type=$(printf '%s' "$_ack" | jq -r '.received // empty' 2>/dev/null || true)
        _ack_ip=$(printf '%s' "$_ack" | jq -r '.yiaddr // empty' 2>/dev/null || true)
        _lease_time=$(printf '%s' "$_ack" | jq -r '.lease_time // empty' 2>/dev/null || true)
        _renewal_time=$(printf '%s' "$_ack" | jq -r '.renewal_time // "absent"' 2>/dev/null || true)
        _rebinding_time=$(printf '%s' "$_ack" | jq -r '.rebinding_time // "absent"' 2>/dev/null || true)
        _P22_LEASE_IP="$_offer_ip"
        _expected_metric=$(( _baseline_metric + 1 ))
        if [[ "$_ack_type" != "ACK" || "$_ack_ip" != "$_offer_ip" || \
              "$_lease_time" != "3600" ]] || \
           ! _p22_poll_state "$_expected_metric" "$_offer_ip" 1; then
            _step91_ok=0
        fi
    fi

    if [[ $_step91_ok -eq 1 ]]; then
        _detail="baseline metric=${_baseline_metric} ips=${_baseline_ips}; offer='$(_p22_snippet "$_offer")'; ack='$(_p22_snippet "$_ack")'; T1=${_renewal_time}, T2=${_rebinding_time}; observed metric=${_P22_OBS_METRIC} ips=${_P22_OBS_IPS}"
        pass "Step 93: virtual DHCP DORA" "$_detail"
    else
        fail "Step 93: virtual DHCP DORA" \
            "baseline metric='${_baseline_metric}' ips='${_baseline_ips}'; iface='${_P22_LAN_IFACE}' pool='${_pool}'; offer='$(_p22_snippet "$_offer")'; ack='$(_p22_snippet "$_ack")'; observed metric='${_P22_OBS_METRIC:-}' ips='${_P22_OBS_IPS:-}'"
    fi

    if [[ $_step91_ok -eq 1 ]]; then
        _renew=$(_p22_run_client --timeout 8 --ip "$_P22_LEASE_IP" \
            --server-id "$_P22_SERVER_ID" --server-mac "$_P22_SERVER_MAC" \
            request-renew) || _step92_ok=0
        _renew_type=$(printf '%s' "$_renew" | jq -r '.received // empty' 2>/dev/null || true)
        _renew_ip=$(printf '%s' "$_renew" | jq -r '.yiaddr // empty' 2>/dev/null || true)
        [[ "$_renew_type" == "ACK" && "$_renew_ip" == "$_P22_LEASE_IP" ]] || _step92_ok=0

        _rebind=$(_p22_run_client --timeout 8 --ip "$_P22_LEASE_IP" request-rebind) || _step92_ok=0
        _rebind_type=$(printf '%s' "$_rebind" | jq -r '.received // empty' 2>/dev/null || true)
        _rebind_ip=$(printf '%s' "$_rebind" | jq -r '.yiaddr // empty' 2>/dev/null || true)
        [[ "$_rebind_type" == "ACK" && "$_rebind_ip" == "$_P22_LEASE_IP" ]] || _step92_ok=0
        _p22_poll_state "$_expected_metric" "$_P22_LEASE_IP" 1 || _step92_ok=0
    else
        _step92_ok=0
    fi

    if [[ $_step92_ok -eq 1 ]]; then
        pass "Step 94: DHCP renew and rebind" \
            "renew='$(_p22_snippet "$_renew")'; rebind='$(_p22_snippet "$_rebind")'; metric=${_P22_OBS_METRIC} ips=${_P22_OBS_IPS}"
    else
        fail "Step 94: DHCP renew and rebind" \
            "Step 93 ready=${_step91_ok}; renew='$(_p22_snippet "$_renew")'; rebind='$(_p22_snippet "$_rebind")'; metric='${_P22_OBS_METRIC:-}' ips='${_P22_OBS_IPS:-}'"
    fi

    if [[ $_step91_ok -eq 1 ]]; then
        _bogus_ip=$(python3 - "$_pool_start" "$_pool_end" "$_P22_SERVER_ID" <<'PY'
import ipaddress
import sys

start, end, server = map(ipaddress.IPv4Address, sys.argv[1:])
network = ipaddress.IPv4Network(f"{server}/24", strict=False)
for host in (200, 250, 100):
    candidate = network.network_address + host
    if candidate not in (network.network_address, network.broadcast_address) and not start <= candidate <= end:
        print(candidate)
        break
else:
    raise SystemExit(1)
PY
        ) || _step93_ok=0
        _bogus=$(_p22_run_client_for_mac "$_P22_BOGUS_MAC" --timeout 8 --bogus-ip "$_bogus_ip" \
            --server-id "$_P22_SERVER_ID" request-bogus) || _step93_ok=0
        _bogus_type=$(printf '%s' "$_bogus" | jq -r '.received // empty' 2>/dev/null || true)
        # Regression check: decode_request() must reject a same-subnet address that is
        # outside the configured pool range.
        [[ "$_bogus_type" == "NAK" ]] || _step93_ok=0
        _p22_poll_state "$_expected_metric" "$_P22_LEASE_IP" 1 || _step93_ok=0
    else
        _step93_ok=0
    fi

    if [[ $_step93_ok -eq 1 ]]; then
        pass "Step 95: out-of-pool REQUEST rejected (NAK)" \
            "server NAKed out-of-pool ${_bogus_ip}; response='$(_p22_snippet "$_bogus")'; lease metric=${_P22_OBS_METRIC} ips=${_P22_OBS_IPS} unchanged"
    else
        fail "Step 95: out-of-pool REQUEST rejected (NAK)" \
            "expected NAK for bogus='${_bogus_ip}'; response='$(_p22_snippet "$_bogus")'; metric='${_P22_OBS_METRIC:-}' ips='${_P22_OBS_IPS:-}'"
    fi

    if [[ $_step91_ok -eq 1 ]]; then
        _release=$(_p22_run_client --ip "$_P22_LEASE_IP" --server-id "$_P22_SERVER_ID" \
            --server-mac "$_P22_SERVER_MAC" release) || _step94_ok=0
        _p22_poll_state "$_baseline_metric" "$_P22_LEASE_IP" 0 || _step94_ok=0
        _p22_ip_in_list "$_P22_OBS_IPS" "$_P22_REAL_LEASE" || _step94_ok=0
        ssh_lan "ping -I '${_P22_LAN_IFACE}' -c 3 -W 2 '${_P22_SERVER_ID}' >/dev/null 2>&1" || _step94_ok=0
    else
        _step94_ok=0
        _p22_release_best_effort || true
    fi

    if [[ $_step94_ok -eq 1 ]]; then
        pass "Step 96: DHCP RELEASE and isolation" \
            "release='$(_p22_snippet "$_release")'; metric=${_P22_OBS_METRIC} ips=${_P22_OBS_IPS}; real lease ${_P22_REAL_LEASE} and gateway ping intact"
        _P22_LEASE_IP=""
    else
        fail "Step 96: DHCP RELEASE and isolation" \
            "release='$(_p22_snippet "$_release")'; metric='${_P22_OBS_METRIC:-}' ips='${_P22_OBS_IPS:-}'; real lease=${_P22_REAL_LEASE} iface='${_P22_LAN_IFACE}'"
    fi
}
