#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 35 — HSI IPv6 end to end (Steps 141-150)
#
# Turns IPv6 on for the primary subscriber, redials so IPV6CP and DHCPv6-PD
# run, then checks the whole path: the LAN host builds a SLAAC address inside
# the delegated prefix, ping6 and iperf3 reach the WAN host, and the
# controller DB reports the same /56 the node does.
#
# Turning IPv6 back off is checked as "RAs stop and forwarding stops". The
# node never sends a lifetime-zero RA — hosts age the prefix out on their own
# — so the LAN address does not disappear on disable; it goes away when this
# phase switches IPv6 back off on the guest interface.
#
# Every sysctl, address and route this phase touches on the bench is read
# first and put back by _cleanup_phase35_ipv6, which also runs from the
# top-level EXIT trap so an aborted run leaves nothing behind.
# ---------------------------------------------------------------------------

set -euo pipefail

# IPv6 defaults built into the dpdk-bras running on this bench.
_P35_WAN6_HOST_ADDR="2001:db8:201::11"   # upstream host, past the BRAS
_P35_WAN6_GW="2001:db8:201::1"           # BRAS address on the upstream link
_P35_WAN6_PLEN=64
_P35_PD_ROUTE="2001:db8:6400::/48"       # delegation pool; needs a return route
_P35_LAN_VLAN="vlan3"                    # primary subscriber's LAN interface

# Drill: point the capture at an interface the segment's traffic never crosses.
# tcpdump still starts and still reports no RAs, so the interface-anchored guard
# and the control packet are what tell this apart from RAs really having stopped.
_p35_inject_capture_wrong_interface() {
    _P35_LAN_VLAN=lo
}

_p35_cleanup_capture_wrong_interface() {
    restore_phase_functions phase35_ipv6.sh
}

case_validation_register ra_capture_wrong_interface phase35_ipv6 \
    _p35_inject_capture_wrong_interface _p35_cleanup_capture_wrong_interface \
    'Step 149:'

# Bench state to undo, and the values read from the node.
_P35_IPV6_SET=0
_P35_LAN_SYSCTL_SAVED=""
_P35_WAN_SYSCTL_SAVED=""
_P35_WAN_ADDR_ADDED=0
_P35_WAN_ROUTE_ADDED=0
_P35_PD_PREFIX=""
_P35_V6_ADDR=""
_P35_V6_DNS_JOINED=""
_P35_REDIAL_STAGE=""

_p35_user_phase() {
    local _uid="$1"

    fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[]? | select(.user_id == ${_uid}) | .status // empty" \
        2>/dev/null || true
}

# One HSI field of a subscriber. The IPv6 fields come back empty whenever the
# forwarding gate is closed, which is what makes them a readiness signal.
_p35_hsi_field() {
    local _uid="$1" _field="$2" _out="" _rc=0

    _out=$(fastrg_grpc get_hsi_info 2>/dev/null) || _rc=$?
    # "err" rather than empty when the RPC itself failed: empty is a pass
    # condition below, so a dead RPC must not be able to produce it.
    if [[ "$_rc" -ne 0 || -z "$_out" ]]; then
        printf 'err'
        return 0
    fi
    printf '%s' "$_out" | \
        jq -r ".hsi_infos[]? | select(.user_id == ${_uid}) | .${_field} // empty" \
        2>/dev/null || true
}

# Whether get_hsi_info still lists a subscriber at all. An RPC that answers
# nothing looks exactly like fields that were correctly cleared.
_p35_hsi_has_record() {
    local _uid="$1"

    fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -e ".hsi_infos[]? | select(.user_id == ${_uid})" >/dev/null 2>&1
}

# IPv6 DNS servers as one comma-separated string — the form the controller
# stores, so the two sides can be compared directly.
_p35_hsi_dns_joined() {
    local _uid="$1"

    fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[]? | select(.user_id == ${_uid}) | [.ipv6_dnss[]?] | join(\",\")" \
        2>/dev/null || true
}

# Hang up and dial again through the controller. IPV6CP only runs while a
# session is being brought up, so enabling IPv6 needs a fresh session.
# _P35_REDIAL_STAGE describes what went wrong when this returns non-zero.
_p35_redial() {
    local _uid="$1" _i _phase=""

    _P35_REDIAL_STAGE=""
    fastrg_grpc disconnect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 15); do
        sleep 2
        _phase=$(_p35_user_phase "${_uid}" || true)
        [[ "$_phase" != "Data phase" ]] && break
    done
    [[ "$_phase" == "Data phase" ]] && \
        _P35_REDIAL_STAGE="session never dropped after hangup"

    fastrg_grpc connect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 30); do
        sleep 2
        _phase=$(_p35_user_phase "${_uid}" || true)
        [[ "$_phase" == "Data phase" ]] && return 0
    done
    _P35_REDIAL_STAGE="${_P35_REDIAL_STAGE:+${_P35_REDIAL_STAGE}; }did not return to Data phase (last='${_phase:-missing}')"
    return 1
}

# One capture window on the LAN interface, printed as "<control> <ra>": how many
# neighbour solicitations the test's own probe put on the segment, and how many
# Router Advertisements arrived. Prints "err" when tcpdump never started on that
# interface — a capture that failed to run must not read as silence.
# Read one capture window's text: "err" when tcpdump never listened on the
# interface asked for, otherwise "<control> <ra>".
e2e_get_capture_window_counts() {
    local _raw="$1" _interface="$2" _control=0 _ra=0

    # Anchored on the interface name: "listening on lo" must not pass for vlan3.
    if ! printf '%s\n' "$_raw" | grep -q "listening on ${_interface}"; then
        printf 'err'
        return 0
    fi
    _control=$(printf '%s\n' "$_raw" | grep -c 'neighbor solicitation' || true)
    _ra=$(printf '%s\n' "$_raw" | grep -c 'router advertisement' || true)
    printf '%s %s' "$_control" "$_ra"
}

local_validation_register capture_window_counts e2e_get_capture_window_counts \
    capture_window_good \
    capture_window_empty_input \
    capture_window_never_listened \
    capture_window_wrong_interface \
    capture_window_no_control \
    capture_window_ra_present

_p35_ra_window() {
    local _secs="$1" _raw=""

    # Soliciting an unused link-local neighbour puts an icmp6 packet on the
    # segment that the node never sees, so it proves the capture was awake
    # without touching what is being measured.
    ssh_lan "( sleep 2; ping -6 -c 1 -W 1 -I ${_P35_LAN_VLAN} fe80::dead:beef >/dev/null 2>&1 || true ) &" \
        >/dev/null 2>&1 || true
    _raw=$(ssh_lan "timeout ${_secs} tcpdump -l -n -i ${_P35_LAN_VLAN} 'icmp6' 2>&1" || true)
    e2e_get_capture_window_counts "$_raw" "${_P35_LAN_VLAN}"
}

# The PPPoE status row the controller recorded for a user (Kafka-fed).
_p35_ctrl_row() {
    fastrg_grpc ctrl_pppoe_status "$1" 2>/dev/null || true
}

# Recent IPv6 lines from the node log — failure diagnosis only, never asserted.
_p35_node_ipv6_log() {
    local _path=""

    _path=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" 2>/dev/null | \
        awk -F'"' '{print $2}' || true)
    [[ -n "$_path" ]] || _path=/var/log/fastrg/fastrg.log
    ssh_node "tail -n 200 '${_path}' 2>/dev/null" 2>/dev/null | \
        grep -E 'IPV6CP|DHCPv6|IPv6' | tail -n 6 | tr '\n' ' ' || true
}

# Idempotent: called at the end of phase35 and from the top-level EXIT trap.
_cleanup_phase35_ipv6() {
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" >/dev/null 2>&1 || true

    if [[ -n "${_P35_LAN_SYSCTL_SAVED:-}" ]]; then
        info "Cleanup(phase35): restoring ${_P35_LAN_VLAN} IPv6 sysctl on the LAN host..."
        ssh_lan "sysctl -w net.ipv6.conf.${_P35_LAN_VLAN}.disable_ipv6=${_P35_LAN_SYSCTL_SAVED}" \
            >/dev/null 2>&1 || true
        _P35_LAN_SYSCTL_SAVED=""
    fi

    if [[ "${_P35_WAN_ROUTE_ADDED:-0}" -eq 1 ]]; then
        ssh_wan "ip -6 route del ${_P35_PD_ROUTE} via ${_P35_WAN6_GW} dev ${WAN_NIC}" \
            >/dev/null 2>&1 || true
        _P35_WAN_ROUTE_ADDED=0
    fi
    if [[ "${_P35_WAN_ADDR_ADDED:-0}" -eq 1 ]]; then
        ssh_wan "ip -6 addr del ${_P35_WAN6_HOST_ADDR}/${_P35_WAN6_PLEN} dev ${WAN_NIC}" \
            >/dev/null 2>&1 || true
        _P35_WAN_ADDR_ADDED=0
    fi
    if [[ -n "${_P35_WAN_SYSCTL_SAVED:-}" ]]; then
        info "Cleanup(phase35): restoring ${WAN_NIC} IPv6 sysctl on the WAN host..."
        ssh_wan "sysctl -w net.ipv6.conf.${WAN_NIC}.disable_ipv6=${_P35_WAN_SYSCTL_SAVED}" \
            >/dev/null 2>&1 || true
        _P35_WAN_SYSCTL_SAVED=""
    fi

    if [[ "${_P35_IPV6_SET:-0}" -eq 1 ]]; then
        info "Cleanup(phase35): switching ipv6_enable back off for user ${USER_ID}..."
        fastrg_grpc set_ipv6 "${USER_ID}" false >/dev/null 2>&1 || true
        _P35_IPV6_SET=0
    fi
    return 0
}

phase35_ipv6() {
    local _i _issue="" _ok=0
    local _etcd_val="" _flag="" _hsi_dump=""
    local _addr_line="" _route_line=""
    local _lan_addrs="" _lan_addr_count=0 _lan64="" _lan_default=""
    local _ping_out="" _loss="" _gw_ping=""
    local _iperf_out="" _bps="" _bps_int="" _mbps="" _iperf_err=""
    local _row="" _db_prefix="" _db_addr="" _db_dns=""
    local _disable_at=0 _elapsed=0 _ra_count="" _pfx="" _v6addr=""
    local _uid="" _phase=""

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 35 — HSI IPv6 End to End (Steps 141-150)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 141 — enable ipv6_enable through the controller
    # ------------------------------------------------------------------
    info "Step 141: enabling ipv6_enable for user ${USER_ID} via the controller..."
    _P35_IPV6_SET=1
    fastrg_grpc set_ipv6 "${USER_ID}" true >/dev/null 2>&1 || true

    for _i in $(seq 1 10); do
        _etcd_val=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
        # No `// empty` here: it would swallow a stored false and read as absent.
        _flag=$(printf '%s' "$_etcd_val" | jq -r '.config.ipv6_enable' 2>/dev/null || true)
        [[ "$_flag" == "true" ]] && break
        sleep 1
    done

    if [[ "$_flag" == "true" ]]; then
        pass "Step 141: enable IPv6 for the subscriber" \
            "etcd configs/${NODE_UUID}/hsi/${USER_ID} carries ipv6_enable=true"
    else
        fail "Step 141: enable IPv6 for the subscriber" \
            "etcd ipv6_enable='${_flag:-missing}'; value=$(printf '%s' "$_etcd_val" | jq -c '.config' 2>/dev/null | cut -c 1-200 || true)"
    fi

    # ------------------------------------------------------------------
    # Step 142 — redial so IPV6CP and DHCPv6-PD run
    # ------------------------------------------------------------------
    info "Step 142: redialling user ${USER_ID} and waiting for the delegated prefix..."
    _issue=""
    if ! _p35_redial "${USER_ID}"; then
        _issue="redial failed: ${_P35_REDIAL_STAGE}"
    fi

    for _i in $(seq 1 15); do
        _P35_PD_PREFIX=$(_p35_hsi_field "${USER_ID}" 'ipv6_pd_prefix')
        [[ -n "$_P35_PD_PREFIX" ]] && break
        sleep 2
    done
    _P35_V6_ADDR=$(_p35_hsi_field "${USER_ID}" 'ipv6_addr')
    _P35_V6_DNS_JOINED=$(_p35_hsi_dns_joined "${USER_ID}")

    if [[ -z "$_P35_PD_PREFIX" ]]; then
        _issue="${_issue:+${_issue}; }no ipv6_pd_prefix reported within 30s"
    elif [[ "$_P35_PD_PREFIX" != */56 ]]; then
        _issue="${_issue:+${_issue}; }delegated prefix is not a /56 ('${_P35_PD_PREFIX}')"
    fi
    if [[ "$_P35_V6_ADDR" != fe80:* ]]; then
        _issue="${_issue:+${_issue}; }IPV6CP link-local missing ('${_P35_V6_ADDR:-empty}')"
    fi

    if [[ -z "$_issue" ]]; then
        pass "Step 142: IPV6CP and DHCPv6-PD come up" \
            "delegated prefix ${_P35_PD_PREFIX}, link-local ${_P35_V6_ADDR}, DNS ${_P35_V6_DNS_JOINED:-none}"
    else
        _hsi_dump=$(fastrg_grpc get_hsi_info 2>/dev/null | \
            jq -c ".hsi_infos[]? | select(.user_id == ${USER_ID})" 2>/dev/null | cut -c 1-300 || true)
        fail "Step 142: IPV6CP and DHCPv6-PD come up" \
            "${_issue}; hsi=${_hsi_dump:-unavailable}; node log: $(_p35_node_ipv6_log)"
    fi

    # ------------------------------------------------------------------
    # Step 143 — give the WAN host an IPv6 address and a return route
    # ------------------------------------------------------------------
    info "Step 143: provisioning IPv6 on the WAN host ${WAN_HOST} (${WAN_NIC})..."
    _issue=""
    _P35_WAN_SYSCTL_SAVED=$(ssh_wan "sysctl -n net.ipv6.conf.${WAN_NIC}.disable_ipv6" \
        2>/dev/null | tr -d '[:space:]' || true)
    if [[ "$_P35_WAN_SYSCTL_SAVED" =~ ^[0-9]+$ ]]; then
        ssh_wan "sysctl -w net.ipv6.conf.${WAN_NIC}.disable_ipv6=0" >/dev/null 2>&1 || true
    else
        _P35_WAN_SYSCTL_SAVED=""
        _issue="cannot read net.ipv6.conf.${WAN_NIC}.disable_ipv6"
    fi

    # The flags go up before the commands run: a partially applied change still
    # has to be undone by cleanup.
    _P35_WAN_ADDR_ADDED=1
    ssh_wan "ip -6 addr add ${_P35_WAN6_HOST_ADDR}/${_P35_WAN6_PLEN} dev ${WAN_NIC}" \
        >/dev/null 2>&1 || true
    _P35_WAN_ROUTE_ADDED=1
    ssh_wan "ip -6 route add ${_P35_PD_ROUTE} via ${_P35_WAN6_GW} dev ${WAN_NIC}" \
        >/dev/null 2>&1 || true

    _addr_line=$(ssh_wan "ip -6 -o addr show dev ${WAN_NIC}" 2>/dev/null | \
        grep -F "${_P35_WAN6_HOST_ADDR}/" || true)
    _route_line=$(ssh_wan "ip -6 route show ${_P35_PD_ROUTE}" 2>/dev/null | \
        grep -F "${_P35_WAN6_GW}" || true)
    [[ -n "$_addr_line" ]]  || _issue="${_issue:+${_issue}; }${_P35_WAN6_HOST_ADDR} not on ${WAN_NIC}"
    [[ -n "$_route_line" ]] || _issue="${_issue:+${_issue}; }no return route for ${_P35_PD_ROUTE} via ${_P35_WAN6_GW}"

    if [[ -z "$_issue" ]]; then
        pass "Step 143: WAN host IPv6 provisioning" \
            "${WAN_NIC} has ${_P35_WAN6_HOST_ADDR}/${_P35_WAN6_PLEN} and a return route for ${_P35_PD_ROUTE}"
    else
        fail "Step 143: WAN host IPv6 provisioning" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 144 — LAN host builds a SLAAC address inside the delegated prefix
    # ------------------------------------------------------------------
    info "Step 144: enabling IPv6 on ${LAN_HOST} ${_P35_LAN_VLAN} and waiting for SLAAC..."
    _issue=""
    _P35_LAN_SYSCTL_SAVED=$(ssh_lan "sysctl -n net.ipv6.conf.${_P35_LAN_VLAN}.disable_ipv6" \
        2>/dev/null | tr -d '[:space:]' || true)
    if [[ "$_P35_LAN_SYSCTL_SAVED" =~ ^[0-9]+$ ]]; then
        # accept_ra and autoconf are already on; this is the only switch needed.
        ssh_lan "sysctl -w net.ipv6.conf.${_P35_LAN_VLAN}.disable_ipv6=0" >/dev/null 2>&1 || true
    else
        _P35_LAN_SYSCTL_SAVED=""
        _issue="cannot read net.ipv6.conf.${_P35_LAN_VLAN}.disable_ipv6"
    fi

    # Each redial delegates a fresh prefix while the previous SLAAC addresses sit
    # out their 24h lifetime, so clear the global ones and let the poll below
    # watch the current prefix form. Link-local is kept: RS/RA runs on it.
    ssh_lan "ip -6 addr flush dev ${_P35_LAN_VLAN} scope global" >/dev/null 2>&1 || true

    for _i in $(seq 1 25); do
        _lan_addrs=$(ssh_lan "ip -6 -o addr show dev ${_P35_LAN_VLAN} scope global" 2>/dev/null | \
            grep -v tentative | awk '{print $4}' | cut -d/ -f1 | tr '\n' ' ' || true)
        [[ -n "${_lan_addrs// /}" ]] && break
        sleep 3
    done
    _lan_addr_count=$(printf '%s' "$_lan_addrs" | wc -w | tr -d '[:space:]' || true)

    # Every global address must sit in the first /64 of the delegated prefix —
    # that is the one the node advertises. Temporary addresses make several.
    if [[ -n "$_P35_PD_PREFIX" ]]; then
        _lan64=$(python3 -c 'import ipaddress, sys
pd = ipaddress.IPv6Network(sys.argv[1], strict=False)
lan64 = ipaddress.IPv6Network((pd.network_address, 64))
addrs = [ipaddress.IPv6Address(a) for a in sys.argv[2:]]
if not addrs or not all(a in lan64 for a in addrs):
    sys.exit(1)
print(lan64)' "$_P35_PD_PREFIX" ${_lan_addrs} 2>/dev/null || true)
    fi
    [[ -n "$_lan64" ]] || \
        _issue="${_issue:+${_issue}; }global addresses '${_lan_addrs:-none}' are not inside the first /64 of ${_P35_PD_PREFIX:-unknown}"

    _lan_default=$(ssh_lan "ip -6 route show default dev ${_P35_LAN_VLAN}" 2>/dev/null || true)
    [[ -n "$_lan_default" ]] || \
        _issue="${_issue:+${_issue}; }RA installed no default route on ${_P35_LAN_VLAN}"

    if [[ -z "$_issue" ]]; then
        pass "Step 144: LAN host SLAAC from the delegated prefix" \
            "${_lan_addr_count} global address(es) in ${_lan64}, default route via RA present"
    else
        fail "Step 144: LAN host SLAAC from the delegated prefix" \
            "${_issue}; addr='$(ssh_lan "ip -6 -o addr show dev ${_P35_LAN_VLAN}" 2>/dev/null | tr '\n' ' ' | cut -c 1-300 || true)'; route='$(printf '%s' "$_lan_default" | tr '\n' ' ' | cut -c 1-200 || true)'"
    fi

    # ------------------------------------------------------------------
    # Step 145 — ping6 LAN → WAN through the node and the BRAS
    # ------------------------------------------------------------------
    info "Step 145: ping6 ${_P35_WAN6_HOST_ADDR} from the LAN host..."
    _ok=0
    _ping_out=""
    # The BRAS backs its upstream neighbor discovery off to one probe per 10s
    # after repeated failures, so the first attempts can land before it has
    # resolved the address we just configured.
    for _i in $(seq 1 6); do
        _ping_out=$(ssh_lan "ping -6 -c 4 -W 3 ${_P35_WAN6_HOST_ADDR} 2>&1" || true)
        # Anchored: only a literal zero loss field counts — "50%"/"100% packet loss" must not substring-match as success.
        if printf '%s' "$_ping_out" | grep -qE '(^|[ ,])0(\.0+)?% packet loss'; then
            _ok=1
            break
        fi
        sleep 5
    done

    if [[ $_ok -eq 1 ]]; then
        pass "Step 145: ping6 LAN→WAN" "${_P35_WAN6_HOST_ADDR} reachable, 0% packet loss"
    else
        _loss=$(printf '%s' "$_ping_out" | grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
        _gw_ping=$(ssh_lan "ping -6 -c 2 -W 2 ${_P35_WAN6_GW} 2>&1" 2>/dev/null | \
            grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
        fail "Step 145: ping6 LAN→WAN" \
            "${_P35_WAN6_HOST_ADDR} - ${_loss:-no response}; BRAS ${_P35_WAN6_GW} - ${_gw_ping:-no response}; route: $(ssh_lan "ip -6 route get ${_P35_WAN6_HOST_ADDR}" 2>/dev/null | tr '\n' ' ' | cut -c 1-200 || true)"
    fi

    # ------------------------------------------------------------------
    # Step 146 — iperf3 over IPv6
    # ------------------------------------------------------------------
    info "Step 146: iperf3 -6 (LAN→WAN, port ${SRV_PORT})..."
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" >/dev/null 2>&1 || true
    sleep 1
    ssh_wan "iperf3 -s -B ${_P35_WAN6_HOST_ADDR} -p ${SRV_PORT} -D --forceflush >/dev/null 2>&1 || true" \
        >/dev/null 2>&1 || true
    _ok=0
    for _i in $(seq 1 10); do
        sleep 1
        if ssh_wan "ss -ltn 2>/dev/null | grep -q ':${SRV_PORT}'" 2>/dev/null; then
            _ok=1
            break
        fi
    done
    [[ $_ok -eq 0 ]] && warn "iperf3 server did not start listening on ${SRV_PORT} within 10s"

    # No fixed client source port: it would sit in TIME_WAIT and break reruns.
    _iperf_out=$(ssh_lan "iperf3 -6 -c ${_P35_WAN6_HOST_ADDR} -p ${SRV_PORT} -t 5 -J 2>&1" || true)
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" >/dev/null 2>&1 || true

    if [[ -z "$_iperf_out" ]]; then
        fail "Step 146: iperf3 -6 LAN→WAN" "no output from the iperf3 client"
    else
        _bps=$(printf '%s' "$_iperf_out" | jq -r '.end.sum_received.bits_per_second // 0' \
            2>/dev/null || echo "0")
        _bps_int=$(printf '%.0f' "${_bps}" 2>/dev/null || echo "0")
        if [[ "$_bps_int" -gt 0 ]]; then
            _mbps=$(awk "BEGIN {printf \"%.2f\", ${_bps_int} / 1000000}")
            pass "Step 146: iperf3 -6 LAN→WAN" "received ${_mbps} Mbps over IPv6"
        else
            _iperf_err=$(printf '%s' "$_iperf_out" | jq -r '.error // empty' 2>/dev/null || true)
            fail "Step 146: iperf3 -6 LAN→WAN" "bits_per_second=0${_iperf_err:+; error: $_iperf_err}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 147 — the controller DB reports the same IPv6 state as the node
    # ------------------------------------------------------------------
    info "Step 147: comparing controller DB IPv6 columns with the node's gRPC report..."
    _ok=0
    _row_seen=0
    for _i in $(seq 1 12); do
        _row=$(_p35_ctrl_row "${USER_ID}")
        # Same RPC, same call: the row has to be there for "IPv6 fields empty"
        # to be a statement about the data rather than about a dead RPC.
        printf '%s' "$_row" | jq -e '.user_id? // .hsi_id?' >/dev/null 2>&1 && _row_seen=1
        _db_prefix=$(printf '%s' "$_row" | jq -r '.hsi_ipv6_pd_prefix // empty' 2>/dev/null || true)
        _db_addr=$(printf '%s' "$_row" | jq -r '.hsi_ipv6 // empty' 2>/dev/null || true)
        _db_dns=$(printf '%s' "$_row" | jq -r '.hsi_ipv6_dns // empty' 2>/dev/null || true)
        if [[ -n "$_P35_PD_PREFIX" && "$_db_prefix" == "$_P35_PD_PREFIX" && \
              "$_db_addr" == "$_P35_V6_ADDR" && \
              -n "$_db_dns" && "$_db_dns" == "$_P35_V6_DNS_JOINED" ]]; then
            _ok=1
            break
        fi
        info "  waiting for the controller DB to carry the IPv6 state... (${_i}x5s)"
        sleep 5
    done

    if [[ $_ok -eq 1 ]]; then
        pass "Step 147: northbound IPv6 report matches the node" \
            "controller DB and node gRPC both report ${_db_prefix}, ${_db_addr}, DNS ${_db_dns}"
    else
        # The raw row and the node's own IPv6 log tell "the row never arrived"
        # apart from "the row is there but the columns are empty".
        info "  Step 147 diagnostic — last ctrl_pppoe_status row:"
        printf '%s\n' "${_row:-<empty>}"
        info "  Step 147 diagnostic — node IPv6 log:"
        _p35_node_ipv6_log
        fail "Step 147: northbound IPv6 report matches the node" \
            "node: prefix='${_P35_PD_PREFIX}' addr='${_P35_V6_ADDR}' dns='${_P35_V6_DNS_JOINED}'; DB: row_seen=${_row_seen} prefix='${_db_prefix:-missing}' addr='${_db_addr:-missing}' dns='${_db_dns:-missing}'"
    fi

    # ------------------------------------------------------------------
    # Step 148 — disabling IPv6 takes effect without a redial
    # ------------------------------------------------------------------
    info "Step 148: switching ipv6_enable back off for user ${USER_ID}..."
    _issue=""
    fastrg_grpc set_ipv6 "${USER_ID}" false >/dev/null 2>&1 || true
    _disable_at=$SECONDS

    _ok=0
    for _i in $(seq 1 15); do
        sleep 1
        _pfx=$(_p35_hsi_field "${USER_ID}" 'ipv6_pd_prefix')
        if [[ -z "$_pfx" ]]; then
            _ok=1
            break
        fi
    done
    [[ $_ok -eq 1 ]] || _issue="gRPC still reports ipv6_pd_prefix='${_pfx}' 15s after disable"
    # The cleared field only means something if the RPC still lists this user.
    _p35_hsi_has_record "${USER_ID}" || \
        _issue="${_issue:+${_issue}; }get_hsi_info lists no record for user ${USER_ID}; a cleared field proves nothing"

    # The guest keeps its address and default route, so these packets do reach
    # the node — and get dropped there.
    _ping_out=$(ssh_lan "ping -6 -c 3 -W 2 ${_P35_WAN6_HOST_ADDR} 2>&1" || true)
    if ! printf '%s' "$_ping_out" | grep -q '100% packet loss'; then
        _loss=$(printf '%s' "$_ping_out" | grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
        _issue="${_issue:+${_issue}; }IPv6 still forwarded after disable (${_loss:-no loss line}: $(printf '%s' "$_ping_out" | tr '\n' ' ' | cut -c 1-160 || true))"
    fi

    if [[ -z "$_issue" ]]; then
        pass "Step 148: disable takes effect live" \
            "gRPC IPv6 fields cleared and IPv6 forwarding stopped without a redial"
    else
        fail "Step 148: disable takes effect live" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 149 — no Router Advertisements after the disable
    # ------------------------------------------------------------------
    # The RA timer stops at its next tick, one interval away at most, so let a
    # full interval pass before opening the silent window.
    _elapsed=$(( SECONDS - _disable_at ))
    if (( _elapsed < 35 )); then
        sleep $(( 35 - _elapsed ))
    fi
    info "Step 149: watching ${_P35_LAN_VLAN} for Router Advertisements for 40s..."
    _ra_window=$(_p35_ra_window 40)
    _ra_control="${_ra_window%% *}"
    _ra_count="${_ra_window##* }"

    if [[ "$_ra_window" == "err" ]]; then
        fail "Step 149: RAs stop after disable" \
            "tcpdump did not start on ${_P35_LAN_VLAN}; the silent window proves nothing"
    elif ! [[ "$_ra_control" =~ ^[1-9] ]]; then
        fail "Step 149: RAs stop after disable" \
            "no neighbour solicitation in the capture (control='${_ra_control:-missing}'); the capture saw nothing at all"
    elif [[ "$_ra_count" == "0" ]]; then
        pass "Step 149: RAs stop after disable" \
            "capture saw ${_ra_control} control packet(s) and no Router Advertisement on ${_P35_LAN_VLAN} in a 40s window"
    else
        fail "Step 149: RAs stop after disable" \
            "${_ra_count} Router Advertisement(s) still sent on ${_P35_LAN_VLAN}"
    fi

    # ------------------------------------------------------------------
    # Step 150 — put the bench back and confirm the IPv4 fixture is healthy
    # ------------------------------------------------------------------
    info "Step 150: restoring the bench and rechecking the canonical fixture..."
    _issue=""
    # Guest first: switching IPv6 off there drops the SLAAC address and the RA
    # default route with it.
    _cleanup_phase35_ipv6

    if ! _p35_redial "${USER_ID}"; then
        _issue="redial to an IPv4-only session failed: ${_P35_REDIAL_STAGE}"
    fi

    for _uid in "${SUB_IDS[@]}"; do
        _phase=$(_p35_user_phase "${_uid}")
        [[ "$_phase" == "Data phase" ]] || \
            _issue="${_issue:+${_issue}; }user ${_uid} status='${_phase:-missing}'"
    done

    _pfx=$(_p35_hsi_field "${USER_ID}" 'ipv6_pd_prefix')
    _v6addr=$(_p35_hsi_field "${USER_ID}" 'ipv6_addr')
    [[ -z "$_pfx" && -z "$_v6addr" ]] || \
        _issue="${_issue:+${_issue}; }gRPC still reports IPv6 (prefix='${_pfx}', addr='${_v6addr}')"

    _ping_out=$(ssh_lan "ping -c 4 -W 3 ${WAN_IP} 2>&1" || true)
    # Anchored: only a literal zero loss field counts — "50%"/"100% packet loss" must not substring-match as success.
    if ! printf '%s' "$_ping_out" | grep -qE '(^|[ ,])0(\.0+)?% packet loss'; then
        _loss=$(printf '%s' "$_ping_out" | grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
        _issue="${_issue:+${_issue}; }LAN→WAN ping ${_loss:-no response}"
    fi

    _ok=0
    for _i in $(seq 1 12); do
        _row=$(_p35_ctrl_row "${USER_ID}")
        _db_prefix=$(printf '%s' "$_row" | jq -r '.hsi_ipv6_pd_prefix // empty' 2>/dev/null || true)
        _db_addr=$(printf '%s' "$_row" | jq -r '.hsi_ipv6 // empty' 2>/dev/null || true)
        _db_dns=$(printf '%s' "$_row" | jq -r '.hsi_ipv6_dns // empty' 2>/dev/null || true)
        if [[ -z "$_db_prefix" && -z "$_db_addr" && -z "$_db_dns" ]]; then
            _ok=1
            break
        fi
        sleep 5
    done
    [[ $_ok -eq 1 ]] || \
        _issue="${_issue:+${_issue}; }controller DB still carries IPv6 (prefix='${_db_prefix}', addr='${_db_addr}', dns='${_db_dns}')"
    [[ $_row_seen -eq 1 ]] || \
        _issue="${_issue:+${_issue}; }ctrl_pppoe_status returned no row for user ${USER_ID}; cleared IPv6 fields prove nothing"

    if [[ -z "$_issue" ]]; then
        pass "Step 150: fixture restored" \
            "IPv4-only sessions healthy for users ${SUB_IDS[*]}, ${WAN_IP} reachable, IPv6 state cleared end to end"
    else
        fail "Step 150: fixture restored" "$_issue"
    fi

    return 0
}
