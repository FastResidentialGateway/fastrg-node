#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 27 — NIC link flap / LSC handling (Steps 113-115)
# ---------------------------------------------------------------------------

set -euo pipefail

# LAN topology comes from the suite-level LAN_FLAP_HOST / LAN_FLAP_NIC /
# LAN_PEER_NIC variables (see run_e2e_test.sh) so this phase carries no
# bench-specific interface names. On the default bench both links are 10G
# fiber into a PF on the peer host with SR-IOV enabled, and the peer VMs
# consume the VFs. Flapping the PF on its host is the only action that drops
# the physical signal the node observes — in-guest admin down / unbind / PCI
# reset all leave the SerDes lit.
#
# Both flaps rely on the peer's admin down actually removing the signal. An
# optical module cuts its laser (TX_DISABLE) when the interface goes down, so
# the node sees a real link down that lasts until the interface is brought
# back. A passive direct-attach copper cable has no TX_DISABLE path: the wire
# keeps carrying a valid idle stream, the node observes nothing, and the only
# event it ever sees is the renegotiation transient when the peer comes back
# up. _p27_wan_carrier_is_down exists so that medium change is reported as
# such instead of quietly turning this step into a race against that
# transient.
#
# Nominal link speed both ports negotiate on this bench (10G fiber into both
# PFs). fastrg_nic_link_speed_mbps must report exactly this while the link is
# up and 0 while it is down — the down side is already required by
# _p27_wait_link_down.
_P27_LINK_SPEED_MBPS=10000
# Hold time of the second, deliberately short WAN flap in Step 113. Measured
# on this bench: 0.1s of host-side down yields exactly one down/up pair on the
# node (10/10 runs), well inside the 0.5s polling interval that the pair is
# meant to slip past.
_P27_FLASH_HOLD_SEC=0.1
_P27_LAN_VLAN="vlan3"
# NetworkManager owns vlan3 on the LAN host, and after the LAN link returns it
# does not simply carry on: it tears the connection down and rebuilds it, with
# the address and the default route gone together while it does. Measured once
# on this bench: teardown at link+4.3s, rebuild complete at link+8.7s. A ping
# issued inside that gap prints "Network is unreachable" and never puts a
# packet on the wire, so it says nothing about the data plane Step 115 tests.
#
# Where the three limits come from:
#   window 25s — the gap can only be observed once Step 115 starts pinging,
#     which lands somewhere between a few seconds and ~15s after the link
#     returns depending on how long the checks in between take. 8.7s of gap
#     plus that spread plus margin is 25s. An unreachable ping later than this
#     runs on some other clock — an expired DHCP lease, a route someone
#     removed — and is reported instead of waited out.
#   budget 20s — how long a rebuild that started inside the window gets to
#     finish, more than twice the 8.7s measured.
#   interval 2s — long enough that attempts are not just ssh round trips,
#     short enough to catch the path within about a second of its return.
#
# All three come from one measurement plus margin, not from a distribution.
# If the attempt log starts showing the gap opening later than the window or
# lasting longer than the budget, NetworkManager's behaviour on this bench has
# changed: measure it again rather than widening the numbers here.
_P27_NM_GAP_WINDOW_SEC=25
_P27_NM_GAP_BUDGET_SEC=20
_P27_NM_GAP_INTERVAL_SEC=2
_P27_WAN_RETURN_ROUTE="192.168.200.128/25"
_P27_WAN_RETURN_GATEWAY="192.168.201.1"
_P27_LOG_PATH=""
_P27_METRICS_PORT=""
_P27_BRAS_SSH_PID=""
_P27_PATH_RECOVERY_DETAIL=""
_P27_PROBE_NOTE=""
_P27_PROBE_OUT=""
_P27_WAN_CMD_DETAIL=""
_p27_needs_session_recovery=0
_p27_needs_path_recovery=0

_p27_log_line_count() {
    local _path="$1" _count

    _count=$(ssh_node "wc -l < '${_path}' 2>/dev/null || echo 0" 2>/dev/null | \
        tail -1 | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

_p27_new_log() {
    local _path="$1" _baseline="$2" _start

    [[ "$_baseline" =~ ^[0-9]+$ ]] || _baseline=0
    _start=$(( _baseline + 1 ))
    ssh_node "tail -n +${_start} '${_path}' 2>/dev/null || true" 2>/dev/null || true
}

_p27_log_snippet() {
    _p27_new_log "$1" "$2" | tr '\n' '|' | tail -c 1000 || true
}

_p27_fetch_metrics() {
    ssh_node "curl -fsS --max-time 3 http://127.0.0.1:${_P27_METRICS_PORT}/metrics" \
        2>/dev/null || true
}

_p27_metric_from_body() {
    local _body="$1" _metric="$2" _port="$3"

    printf '%s\n' "$_body" | awk -v metric="$_metric" -v port="$_port" '
        $1 ~ ("^" metric "\\{") && index($1, "port_id=\"" port "\"") {
            print $2
            exit
        }
    ' || true
}

_p27_read_metric() {
    local _metric="$1" _port="$2" _body _value

    _body=$(_p27_fetch_metrics || true)
    _value=$(_p27_metric_from_body "$_body" "$_metric" "$_port" || true)
    printf '%s' "$_value"
}

_p27_wait_link_down() {
    local _port="$1" _attempts="${2:-14}" _i _body _up _speed

    _P27_OBS_UP=""
    _P27_OBS_SPEED=""
    for _i in $(seq 1 "$_attempts"); do
        _body=$(_p27_fetch_metrics || true)
        _up=$(_p27_metric_from_body "$_body" fastrg_nic_link_up "$_port" || true)
        _speed=$(_p27_metric_from_body "$_body" fastrg_nic_link_speed_mbps "$_port" || true)
        _P27_OBS_UP="$_up"
        _P27_OBS_SPEED="$_speed"
        if [[ "$_up" == "0" && "$_speed" == "0" ]]; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

_p27_wait_link_up() {
    local _port="$1" _attempts="${2:-20}" _i _body _up _speed

    _P27_OBS_UP=""
    _P27_OBS_SPEED=""
    for _i in $(seq 1 "$_attempts"); do
        _body=$(_p27_fetch_metrics || true)
        _up=$(_p27_metric_from_body "$_body" fastrg_nic_link_up "$_port" || true)
        _speed=$(_p27_metric_from_body "$_body" fastrg_nic_link_speed_mbps "$_port" || true)
        _P27_OBS_UP="$_up"
        _P27_OBS_SPEED="$_speed"
        if [[ "$_up" == "1" && "$_speed" =~ ^[1-9][0-9]*$ ]]; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

# The exact host-side command that puts the WAN link back. The restore
# primitive, the detached watchdog and the short flap all build on this one
# definition so a change can never reach some restore paths and not others.
# It quotes with double quotes because the watchdog embeds it inside a
# single-quoted sh -c.
#
# Bringing the link up returns before the kernel has reinstalled the
# interface's connected route, and until it is back the gateway resolves to
# nothing: a route command issued in that window fails with "Nexthop has
# invalid gateway" while the link change itself has already happened. Measured
# on this bench, the window is 0.1-40ms wide (half the samples under 2ms), so
# a loaded host hits it and an idle one usually does not.
#
# `onlink` states what is true here — the gateway sits on this interface's own
# segment — so the kernel stops making the route conditional on that reinstall
# finishing. Forwarding is unchanged; only the timing dependency is gone. Any
# other failure (missing interface, bad syntax, no permission) still returns
# non-zero as before.
_p27_wan_restore_cmd() {
    printf 'ip link set "%s" up; ip route replace "%s" via "%s" dev "%s" onlink' \
        "$WAN_NIC" "$_P27_WAN_RETURN_ROUTE" "$_P27_WAN_RETURN_GATEWAY" "$WAN_NIC"
}

# Why the link commands report more than a status: when one of them fails, the
# step used to say only "failed", leaving no way to tell an ssh-level problem
# from the remote command refusing. The pass path and the conditions are the
# same as before — only the failure messages gained the reason.
_p27_wan_cmd_detail() {
    printf "rc=%s, err='%s'" "$1" "$(printf '%s' "$2" | tr '\n' '|' | tail -c 200)"
}

_p27_wan_link_kill() {
    local _out _rc=0

    _out=$(ssh_wan "ip link set '${WAN_NIC}' down" 2>&1) || _rc=$?
    _P27_WAN_CMD_DETAIL=$(_p27_wan_cmd_detail "$_rc" "$_out")
    return "$_rc"
}

_p27_wan_link_restore() {
    ssh_wan "$(_p27_wan_restore_cmd)" >/dev/null 2>&1 || true
}

# One toggle shorter than the polling interval. Down and up travel in a single
# remote command so the hold is the sleep itself rather than two ssh round
# trips.
_p27_wan_link_flap() {
    local _out _rc=0

    _out=$(ssh_wan "ip link set '${WAN_NIC}' down; sleep ${_P27_FLASH_HOLD_SEC}; $(_p27_wan_restore_cmd)" \
        2>&1) || _rc=$?
    _P27_WAN_CMD_DETAIL=$(_p27_wan_cmd_detail "$_rc" "$_out")
    return "$_rc"
}

# True once the peer host reports the WAN link has lost carrier. LOWER_UP is
# the carrier bit: while it is still set the peer is up on the wire whatever
# its administrative state says, and the node has nothing to detect.
_p27_wan_carrier_is_down() {
    local _i _state

    for _i in $(seq 1 5); do
        _state=$(ssh_wan "ip link show '${WAN_NIC}' 2>/dev/null" 2>/dev/null || true)
        printf '%s\n' "$_state" | grep -q 'LOWER_UP' || return 0
        sleep 0.3
    done
    return 1
}

_p27_restore_lan() {
    # Bringing the LAN PF up is idempotent and safe from the EXIT trap. The
    # peer's link (and vlan3 on top of it) follows the PF.
    ssh_lan_flap "ip link set '${LAN_FLAP_NIC}' up" >/dev/null 2>&1 || true
}

_p27_start_bras() {
    local _i

    ssh_bras "cd /root/dpdk-bras && exec ./dpdk-bras -l 0-7 -n 4 -- --pri-dns 192.168.10.1 --drop-pcap ./test.pcap --vlans 3,5 >/var/log/dpdk-bras.log 2>&1" \
        </dev/null >/dev/null 2>&1 &
    _P27_BRAS_SSH_PID=$!

    for _i in $(seq 1 12); do
        sleep 2
        if ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
            sleep 3
            return 0
        fi
    done
    return 1
}

_p27_user_phase() {
    local _uid="$1" _phase

    _phase=$(fastrg_grpc get_hsi_info | \
        jq -r ".hsi_infos[] | select(.user_id == ${_uid}) | .status" \
        2>/dev/null || true)
    printf '%s' "$_phase"
}

_p27_redial() {
    local _uid="$1" _i _phase=""

    fastrg_grpc disconnect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 15); do
        sleep 2
        _phase=$(_p27_user_phase "${_uid}" || true)
        [[ "$_phase" != "Data phase" ]] && break
    done

    fastrg_grpc connect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 30); do
        sleep 2
        _phase=$(_p27_user_phase "${_uid}" || true)
        if [[ "$_phase" == "Data phase" ]]; then
            return 0
        fi
    done
    _P27_PATH_RECOVERY_DETAIL="user ${_uid} last phase='${_phase:-<empty>}'"
    return 1
}

_p27_redial_all() {
    local _uid _failed="" _detail=""

    for _uid in "${SUB_IDS[@]}"; do
        if ! _p27_redial "${_uid}"; then
            _detail="$_P27_PATH_RECOVERY_DETAIL"
            _failed="${_failed}${_failed:+; }${_detail}"
        fi
    done
    if [[ -n "$_failed" ]]; then
        _P27_PATH_RECOVERY_DETAIL="$_failed"
        return 1
    fi
    return 0
}

# Short-timeout PPPoE rediscovery probe: one canonical subscriber must reach
# Data phase within ~20s of connect. A plain ping cannot see a broken BRAS
# path — it reaches WAN_HOST without crossing the node↔BRAS link.
_p27_probe_redial() {
    local _uid="$1" _i _phase=""

    fastrg_grpc disconnect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        sleep 2
        _phase=$(_p27_user_phase "${_uid}" || true)
        [[ "$_phase" != "Data phase" ]] && break
    done
    fastrg_grpc connect_hsi "${_uid}" >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        sleep 2
        _phase=$(_p27_user_phase "${_uid}" || true)
        [[ "$_phase" == "Data phase" ]] && return 0
    done
    return 1
}

# VF ids the WAN peer NIC actually reports; empty when the NIC has none.
_p27_discover_wan_vf_ids() {
    ssh_wan "ip link show '${WAN_NIC}' 2>/dev/null" 2>/dev/null | \
        grep -oE '^[[:space:]]+vf [0-9]+' | awk '{print $2}' || true
}

# PCI devices bound to a DPDK userspace driver on the BRAS host, whatever the
# driver and addresses are (empty when none are bound).
_p27_discover_bras_dpdk_pcis() {
    ssh_bras "for d in vfio-pci igb_uio uio_pci_generic; do \
        ls /sys/bus/pci/drivers/\$d/ 2>/dev/null; done" 2>/dev/null | \
        grep -E '^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-9a-fA-F]$' | \
        sort -u || true
}

# Recover the node↔BRAS PPPoE path after the flaps — but only when a probe
# proves it is actually broken.
#
# This procedure makes NO assumption about the environment: it probes first,
# and every recovery step operates on what it discovers at runtime — the VF
# list of the flapped peer NIC (may be empty) and whatever devices are bound
# to a DPDK userspace driver on the BRAS host (may be none). Environments
# without any of these simply get a plain peer-link bounce + BRAS restart +
# redial verification; a healthy environment gets nothing at all.
#
# Historical note, not a requirement: the fault that motivated this was first
# reproduced on a bench where the flapped peer NIC happened to be an SR-IOV
# PF with the BRAS guest's ports on its VFs, and a BRAS restart alone did not
# recover them. Other benches may fail (or not fail) differently; the probe
# decides.
_p27_recover_pppoe_path() {
    local _i _id _pci _vf_ids="" _pcis="" _vf_disable="" _vf_auto=""

    _P27_PATH_RECOVERY_DETAIL=""

    if _p27_probe_redial "${SUB_IDS[0]}"; then
        if _p27_redial_all; then
            _p27_needs_path_recovery=0
            _p27_needs_session_recovery=0
            _P27_PATH_RECOVERY_DETAIL="PPPoE path healthy after flap (probe passed, no recovery needed)"
            return 0
        fi
        # Another subscriber cannot dial: fall through into recovery.
    fi

    info "Phase27: PPPoE rediscovery probe failed; recovering the peer path..."

    # Guest DPDK ports must be closed while their devices are reset.
    ssh_bras "pkill -TERM -x dpdk-bras 2>/dev/null || true" >/dev/null 2>&1 || true
    for _i in $(seq 1 30); do
        if ! ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
            break
        fi
        sleep 1
    done
    if ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
        _P27_PATH_RECOVERY_DETAIL="dpdk-bras did not exit after SIGTERM"
        return 1
    fi

    # Bounce the peer link. If (and only if) the NIC reports VFs, hold them
    # disabled across the bounce and restore link-state auto afterwards; with
    # no VFs this is a plain link bounce.
    _vf_ids=$(_p27_discover_wan_vf_ids || true)
    for _id in $_vf_ids; do
        _vf_disable+="ip link set '${WAN_NIC}' vf ${_id} state disable; "
        _vf_auto+="ip link set '${WAN_NIC}' vf ${_id} state auto; "
    done
    if ! ssh_wan "set -e; \
        ${_vf_disable} \
        ip link set '${WAN_NIC}' down; \
        sleep 2; \
        ip link set '${WAN_NIC}' up; \
        ip route replace '${_P27_WAN_RETURN_ROUTE}' via '${_P27_WAN_RETURN_GATEWAY}' dev '${WAN_NIC}'; \
        carrier=0; \
        for i in \$(seq 1 30); do \
            if ip link show '${WAN_NIC}' | grep -q LOWER_UP; then carrier=1; break; fi; \
            sleep 1; \
        done; \
        test \$carrier -eq 1; \
        ${_vf_auto} true" \
        >/dev/null 2>&1; then
        ssh_wan "ip link set '${WAN_NIC}' up 2>/dev/null || true; \
            ip route replace '${_P27_WAN_RETURN_ROUTE}' via '${_P27_WAN_RETURN_GATEWAY}' dev '${WAN_NIC}' 2>/dev/null || true; \
            ${_vf_auto} true" \
            >/dev/null 2>&1 || true
        _P27_PATH_RECOVERY_DETAIL="peer link bounce failed (vf ids: ${_vf_ids:-none})"
        return 1
    fi

    # Reset whatever DPDK-bound devices the BRAS host has before DPDK reopens
    # them. Devices without FLR support (no writable reset file) are skipped
    # with a warning instead of failing the recovery.
    _pcis=$(_p27_discover_bras_dpdk_pcis || true)
    if [[ -n "$_pcis" ]]; then
        for _pci in $_pcis; do
            if ! ssh_bras "test -w /sys/bus/pci/devices/${_pci}/reset && \
                echo 1 > /sys/bus/pci/devices/${_pci}/reset" >/dev/null 2>&1; then
                warn "Phase27: PCI reset unsupported or failed for ${_pci}; continuing."
            fi
        done
    else
        info "Phase27: no DPDK-bound devices found on the BRAS host; skipping device reset."
    fi

    if ! _p27_start_bras; then
        _P27_PATH_RECOVERY_DETAIL="dpdk-bras did not restart after path recovery"
        return 1
    fi

    # Configuration readback is not enough: explicitly redial every canonical
    # subscriber and require Data phase, proving discovery crosses the
    # recovered path before phase28 restarts BRAS in CHAP mode.
    if ! _p27_redial_all; then
        return 1
    fi

    _p27_needs_path_recovery=0
    _p27_needs_session_recovery=0
    _P27_PATH_RECOVERY_DETAIL="users ${SUB_IDS[*]} returned to Data phase after path recovery (vf ids: ${_vf_ids:-none}; devices: ${_pcis:-none})"
    return 0
}

_p27_arm_wan_watchdog() {
    # The watchdog is deliberately armed before link-down. Even if the runner
    # is interrupted, the WAN peer returns to up within eight seconds, before
    # the node's ten-second link_disconnect timer can fire.
    ssh_wan "nohup sh -c 'sleep 8; $(_p27_wan_restore_cmd)' \
        >/dev/null 2>&1 </dev/null &" >/dev/null 2>&1
}

_p27_arm_lan_watchdog() {
    # Same guarantee for the LAN PF: even if the runner is interrupted the
    # physical link returns within eight seconds. No return route is involved
    # on the LAN side.
    ssh_lan_flap "nohup sh -c 'sleep 8; ip link set \"${LAN_FLAP_NIC}\" up' \
        >/dev/null 2>&1 </dev/null &" >/dev/null 2>&1
}

# One word for what a ping run means, used for the attempt log and for the
# decision to measure again:
#   no-route — "Network is unreachable": nothing reached the wire, whether the
#              route was already missing (connect:) or vanished mid-run
#              (sendmsg:), so the run measured nothing
#   ok       — every packet came back
#   loss     — packets did reach the wire and did not all come back, which is
#              a verdict about the data plane
#   silent   — no ping output to judge at all (ssh trouble, say)
#
# The loss figure is matched with the percentage anchored, so that the "0%" of
# "100% packet loss" cannot read as a clean run.
_p27_classify_ping() {
    local _out="$1"

    if printf '%s\n' "$_out" | grep -q 'Network is unreachable'; then
        printf 'no-route'
    elif printf '%s\n' "$_out" | grep -qE '(^|[ ,])0(\.0+)?% packet loss'; then
        printf 'ok'
    elif printf '%s\n' "$_out" | grep -qE '[1-9][0-9]* packets transmitted'; then
        printf 'loss'
    else
        printf 'silent'
    fi
}

# Whether the LAN host holds an IPv4 address on its vlan and can resolve a
# route to the ping target right now. NetworkManager takes both away together
# while it rebuilds the connection, so one line per probe shows how far the
# rebuild has got. Evidence for reading a failure afterwards, never a decision
# input: an expired lease looks the same.
_p27_lan_path_snapshot() {
    local _state

    _state=$(ssh_lan "ip -4 -o addr show dev '${_P27_LAN_VLAN}' scope global | grep -q ' inet ' \
            && printf 'addr=yes ' || printf 'addr=no '; \
        ip route get '${WAN_IP}' 2>/dev/null | grep -q ' dev ${_P27_LAN_VLAN}' \
            && printf 'route=yes' || printf 'route=no'" 2>/dev/null || true)
    printf '%s' "${_state:-addr=? route=?}"
}

# The session-teardown lines Step 115 asserts on, over the log this run has
# produced so far. The probe loop repeats this check because a teardown
# settles the outcome: once the node has dropped the session, nothing the LAN
# host does to its own vlan will bring the path back.
_p27_session_teardown_seen() {
    local _baseline="$1" _hit

    _hit=$(_p27_new_log "$_P27_LOG_PATH" "$_baseline" | \
        grep -E 'pppoe is force terminating|pppoe is spawning|HSI module is (terminated|spawned)' || true)
    [[ -n "$_hit" ]]
}

# "1 retry" / "4 retries", because a step's detail line is often all a reader
# gets.
_p27_retries_phrase() {
    if [[ "$1" -eq 1 ]]; then
        printf '1 retry'
    else
        printf '%s retries' "$1"
    fi
}

# The Step 115 probe: one unscored ping to absorb the ARP the flaps cleared,
# then the scored one. The pair is repeated only while the single thing in the
# way is the LAN host rebuilding its vlan — see the _P27_NM_GAP_* notes above
# for what that is and where the limits come from.
#
# _P27_PROBE_OUT comes back holding the scored output of the last attempt, so
# what is asserted on is the same whether it took one attempt or seven. Both
# results travel in variables rather than on stdout: a command substitution
# would run this in a subshell, where the note below would be lost.
#
# Retrying cannot turn a broken node green. It is allowed only for a ping that
# never left the LAN host, and the only way out of that state is the LAN host
# completing a DHCP exchange with the node's own DHCP server — so a node that
# is not serving DHCP fails here by running the budget out, with every attempt
# on the record.
#
# _P27_PROBE_NOTE comes back as a phrase to append to the step detail: empty
# when the first attempt settled it, otherwise how many attempts there were,
# what each one saw, and why the loop stopped.
#
# $1: value of $SECONDS when the LAN link came back, or -1 if the LAN link
#     never went down in this run
# $2: node log line count from before the flaps
_p27_probe_wan_path() {
    local _anchor="$1" _log_baseline="$2"
    local _out="" _kind="" _retries=0 _t=0 _onset=-1 _deadline=0 _attempts=""

    _P27_PROBE_NOTE=""
    while :; do
        ssh_lan "ping -c 1 -W 2 ${WAN_IP}" >/dev/null 2>&1 || true
        _out=$(ssh_lan "ping -c 4 -W 3 ${WAN_IP}" 2>&1 || true)
        _kind=$(_p27_classify_ping "$_out")
        _t=$(( SECONDS - _anchor ))

        # Packets that reached the wire have already answered the question this
        # step asks, whichever way they answered it. Only a ping that never got
        # out is worth issuing again.
        [[ "$_kind" == "no-route" ]] || break

        if [[ $_anchor -lt 0 ]]; then
            _P27_PROBE_NOTE="; not retried: no LAN link restore in this run to attribute the missing route to"
            break
        fi

        _attempts="${_attempts}${_attempts:+; }t+${_t}s ${_kind} $(_p27_lan_path_snapshot)"

        if [[ $_onset -lt 0 ]]; then
            _onset=$_t
            if [[ $_onset -gt $_P27_NM_GAP_WINDOW_SEC ]]; then
                _P27_PROBE_NOTE="; not retried: first unreachable ping came t+${_onset}s after the LAN link returned, outside the ${_P27_NM_GAP_WINDOW_SEC}s reconvergence window (${_attempts})"
                break
            fi
            _deadline=$(( SECONDS + _P27_NM_GAP_BUDGET_SEC ))
        fi

        if _p27_session_teardown_seen "$_log_baseline"; then
            _P27_PROBE_NOTE="; stopped after $(_p27_retries_phrase "$_retries"): the node logged a session teardown (${_attempts})"
            break
        fi

        if [[ $SECONDS -ge $_deadline ]]; then
            _P27_PROBE_NOTE="; still unreachable after $(_p27_retries_phrase "$_retries") over $(( _t - _onset ))s (${_attempts})"
            break
        fi

        sleep "$_P27_NM_GAP_INTERVAL_SEC"
        _retries=$(( _retries + 1 ))
    done

    if [[ "$_kind" == "ok" && $_retries -gt 0 ]]; then
        _P27_PROBE_NOTE="; recovered after $(_p27_retries_phrase "$_retries"), t+${_t}s after the LAN link returned"
    fi
    _P27_PROBE_OUT="$_out"
}

# Idempotent: called after phase27 and from the top-level EXIT trap.
_cleanup_phase27_link_flap() {
    local _i _ping=""

    _p27_wan_link_restore
    _p27_restore_lan

    if [[ ${_p27_needs_path_recovery:-0} -eq 1 ]]; then
        info "Cleanup(phase27): recovering the PPPoE peer path..."
        if _p27_recover_pppoe_path; then
            return 0
        fi
        warn "Cleanup(phase27): path recovery failed (${_P27_PATH_RECOVERY_DETAIL:-unknown})."
        return 1
    fi

    if [[ ${_p27_needs_session_recovery:-0} -eq 1 ]]; then
        info "Cleanup(phase27): waiting for PPPoE data-plane recovery..."
        for _i in $(seq 1 6); do
            _ping=$(ssh_lan "ping -c 2 -W 2 ${WAN_IP}" 2>&1 || true)
            # Anchored: only a literal zero loss field counts — "50%"/"100% packet loss" must not substring-match as success.
            if printf '%s\n' "$_ping" | grep -qE '(^|[ ,])0(\.0+)?% packet loss'; then
                _p27_needs_session_recovery=0
                return 0
            fi
            sleep 5
        done
        warn "Cleanup(phase27): PPPoE data plane did not recover within 30s."
        return 1
    fi

    return 0
}

phase27_link_flap() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 27 — NIC Link Flap / LSC Handling (Steps 113-115)"
    bold "═══════════════════════════════════════════════════════"

    local _step110_ok=1 _step111_ok=1 _step112_ok=1
    local _issue110="" _issue111="" _issue112=""
    local _mport_raw="" _config_log_path="" _wan_log_baseline=0
    local _wan_flap_base="" _wan_flap_after="" _wan_delta=-1
    local _lan_flap_base="" _lan_flap_after="" _lan_delta=-1
    local _wan_after_lan="" _wan_up_base="" _lan_up_base=""
    local _wan_new_log="" _down_log=0 _up_log=0
    local _wan_flap_sustained=""
    local _flash_base="" _flash_after="" _flash_delta=-1
    local _flash_log_baseline=0 _flash_new_log="" _flash_down_log=0 _flash_up_log=0
    local _flash_final=""
    local _lan_log_baseline=0 _lan_new_log="" _lan_down_log=0 _lan_up_log=0 _i
    local _lan_down_started=0 _lan_down_elapsed=-1
    local _lan_up_started=0 _lan_up_elapsed=-1 _lan_vlan_ready=0
    local _lan_restored_at=-1
    local _session_log="" _ping_out="" _ping_loss=""

    _mport_raw=$(ssh_node "grep 'MetricsListenPort' /etc/fastrg/config.cfg 2>/dev/null" | \
        awk -F'"' '{print $2}' || true)
    _P27_METRICS_PORT="${_mport_raw##*:}"
    _config_log_path=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" || true)
    _P27_LOG_PATH=$(printf '%s' "$_config_log_path" | awk -F'"' '{print $2}' || true)
    [[ -n "$_P27_LOG_PATH" ]] || _P27_LOG_PATH=/var/log/fastrg/fastrg.log

    _cleanup_phase27_link_flap || true

    # Step 113 — WAN LSC down/up, speed cache, flap count, and current-run logs.
    _wan_flap_base=$(_p27_read_metric fastrg_nic_link_flaps_total 1 || true)
    _wan_up_base=$(_p27_read_metric fastrg_nic_link_up 1 || true)
    _wan_log_baseline=$(_p27_log_line_count "$_P27_LOG_PATH")
    if [[ -z "$_P27_METRICS_PORT" || ! "$_wan_flap_base" =~ ^[0-9]+$ || \
          "$_wan_up_base" != "1" ]]; then
        _step110_ok=0
        _issue110="invalid baseline: metrics_port='${_P27_METRICS_PORT}' flap='${_wan_flap_base}' link_up='${_wan_up_base}'"
    elif ! _p27_arm_wan_watchdog; then
        _step110_ok=0
        _issue110="sustained: failed to arm the WAN peer eight-second recovery watchdog"
    elif ! _p27_wan_link_kill; then
        _step110_ok=0
        _issue110="sustained: failed to set WAN peer ${WAN_NIC} down (${_P27_WAN_CMD_DETAIL})"
    else
        _p27_needs_session_recovery=1
        _p27_needs_path_recovery=1
        if ! _p27_wan_carrier_is_down; then
            _step110_ok=0
            _issue110="sustained: WAN down ineffective on host side (${WAN_NIC} still reports carrier after admin down)"
        fi
        if ! _p27_wait_link_down 1 14; then
            _step110_ok=0
            _issue110="${_issue110:+${_issue110}; }sustained: port 1 did not report link_up=0/speed=0 within 7s (up='${_P27_OBS_UP}' speed='${_P27_OBS_SPEED}')"
        fi
        # Restore immediately after observing down; the total down window remains below 10s.
        _p27_wan_link_restore
        if ! _p27_wait_link_up 1 20; then
            _step110_ok=0
            _issue110="${_issue110:+${_issue110}; }sustained: port 1 did not recover link_up=1/speed>0 within 10s (up='${_P27_OBS_UP}' speed='${_P27_OBS_SPEED}')"
        elif [[ "$_P27_OBS_SPEED" != "$_P27_LINK_SPEED_MBPS" ]]; then
            _step110_ok=0
            _issue110="${_issue110:+${_issue110}; }sustained: port 1 came back at ${_P27_OBS_SPEED} Mbps, expected ${_P27_LINK_SPEED_MBPS}"
        fi
    fi
    _p27_wan_link_restore
    sleep 2

    _wan_flap_after=$(_p27_read_metric fastrg_nic_link_flaps_total 1 || true)
    if [[ "$_wan_flap_base" =~ ^[0-9]+$ && "$_wan_flap_after" =~ ^[0-9]+$ ]]; then
        _wan_delta=$(( _wan_flap_after - _wan_flap_base ))
    fi
    _wan_flap_sustained="$_wan_flap_after"
    _wan_new_log=$(_p27_new_log "$_P27_LOG_PATH" "$_wan_log_baseline" || true)
    grep -qF "Port 1 Link Down" <<< "$_wan_new_log" && _down_log=1 || true
    grep -qF "Port 1 Link Up" <<< "$_wan_new_log" && _up_log=1 || true
    if [[ $_wan_delta -lt 2 || $(( _wan_delta % 2 )) -ne 0 || \
          $_down_log -ne 1 || $_up_log -ne 1 ]]; then
        _step110_ok=0
        _issue110="${_issue110:+${_issue110}; }sustained: flap=${_wan_flap_base}->${_wan_flap_after} delta=${_wan_delta}; down_log=${_down_log} up_log=${_up_log}; log='$(_p27_log_snippet "$_P27_LOG_PATH" "$_wan_log_baseline")'"
    fi

    # Second part of Step 113 — a flap shorter than the polling interval.
    #
    # The node counts link transitions in the LSC callback, so a toggle the
    # 0.5s polling can never sample must still show up afterwards: the counter
    # advances by exactly one down/up pair and both lines reach the log. That
    # is what is asserted here. The gauge is deliberately not part of it — it
    # only reports the state at scrape time, so requiring it to have read 0
    # would make this a sampling race. The sustained part above keeps its own
    # gauge assertions unchanged.
    if [[ $_step110_ok -eq 1 ]]; then
        sleep 2
        _flash_base=$(_p27_read_metric fastrg_nic_link_flaps_total 1 || true)
        _flash_log_baseline=$(_p27_log_line_count "$_P27_LOG_PATH")
        if [[ ! "$_flash_base" =~ ^[0-9]+$ ]]; then
            _step110_ok=0
            _issue110="fast-flap: invalid baseline flap='${_flash_base}'"
        elif ! _p27_arm_wan_watchdog; then
            _step110_ok=0
            _issue110="fast-flap: failed to arm the WAN peer eight-second recovery watchdog"
        elif ! _p27_wan_link_flap; then
            _step110_ok=0
            _issue110="fast-flap: failed to flap WAN peer ${WAN_NIC} (${_P27_WAN_CMD_DETAIL})"
        else
            if ! _p27_wait_link_up 1 20; then
                _step110_ok=0
                _issue110="fast-flap: port 1 did not recover link_up=1/speed>0 within 10s (up='${_P27_OBS_UP}' speed='${_P27_OBS_SPEED}')"
            elif [[ "$_P27_OBS_SPEED" != "$_P27_LINK_SPEED_MBPS" ]]; then
                _step110_ok=0
                _issue110="fast-flap: port 1 came back at ${_P27_OBS_SPEED} Mbps, expected ${_P27_LINK_SPEED_MBPS}"
            fi
            _flash_after=$(_p27_read_metric fastrg_nic_link_flaps_total 1 || true)
            if [[ "$_flash_base" =~ ^[0-9]+$ && "$_flash_after" =~ ^[0-9]+$ ]]; then
                _flash_delta=$(( _flash_after - _flash_base ))
            fi
            _flash_new_log=$(_p27_new_log "$_P27_LOG_PATH" "$_flash_log_baseline" || true)
            grep -qF "Port 1 Link Down" <<< "$_flash_new_log" && _flash_down_log=1 || true
            grep -qF "Port 1 Link Up" <<< "$_flash_new_log" && _flash_up_log=1 || true
            if [[ $_flash_delta -ne 2 || $_flash_down_log -ne 1 || $_flash_up_log -ne 1 ]]; then
                _step110_ok=0
                _issue110="${_issue110:+${_issue110}; }fast-flap: ${_P27_FLASH_HOLD_SEC}s toggle gave flap=${_flash_base}->${_flash_after} delta=${_flash_delta} (expected exactly 2); down_log=${_flash_down_log} up_log=${_flash_up_log}; log='$(_p27_log_snippet "$_P27_LOG_PATH" "$_flash_log_baseline")'"
            fi
        fi
        # Step 114 checks that port 1 stays put while port 0 flaps, and uses
        # this counter as its baseline. It is read here unconditionally
        # because a flash command that reports failure can still have flapped
        # the link: carrying the pre-flash number forward would then fail
        # Step 114 for a transition Step 113 has already reported, turning one
        # fault into two. Only a valid number overwrites, so a failed read
        # leaves the previous baseline in place.
        #
        # The wait matters as much as the read. The paths that report a
        # failure above skip the recovery wait, so without this the counter
        # can be sampled between the down and the up — one short of where it
        # settles, which lands Step 114 in the same false failure by a
        # different route.
        _p27_wait_link_up 1 20 || true
        _flash_final=$(_p27_read_metric fastrg_nic_link_flaps_total 1 || true)
        [[ "$_flash_final" =~ ^[0-9]+$ ]] && _wan_flap_after="$_flash_final" || true
        _p27_wan_link_restore
        sleep 2
    fi

    if [[ $_step110_ok -eq 1 ]]; then
        pass "Step 113: WAN LSC event and flap counter" \
            "sustained: port 1 link 1→0→1, speed 0→${_P27_LINK_SPEED_MBPS} Mbps, flap ${_wan_flap_base}→${_wan_flap_sustained} (+${_wan_delta}, even), current-run down/up logs present; fast-flap: ${_P27_FLASH_HOLD_SEC}s toggle counted ${_flash_base}→${_flash_after} (+${_flash_delta}) with down/up logs, link back at ${_P27_OBS_SPEED} Mbps"
    else
        fail "Step 113: WAN LSC event and flap counter" "$_issue110"
    fi

    # Let the WAN events settle before independently flapping the LAN peer.
    sleep 2

    # Step 114 — LAN flap: full down/up observation and per-port isolation.
    #
    # The flap is driven on the host-side PF: the peer only holds a VF, and
    # nothing inside the guest can drop the physical signal the node
    # observes — the PF's admin state controls the link. This mirrors the
    # WAN mechanism of Step 113.
    _lan_flap_base=$(_p27_read_metric fastrg_nic_link_flaps_total 0 || true)
    _lan_up_base=$(_p27_read_metric fastrg_nic_link_up 0 || true)
    _lan_log_baseline=$(_p27_log_line_count "$_P27_LOG_PATH")
    if [[ ! "$_lan_flap_base" =~ ^[0-9]+$ || "$_lan_up_base" != "1" || \
          ! "$_wan_flap_after" =~ ^[0-9]+$ ]]; then
        _step111_ok=0
        _issue111="invalid baseline: LAN flap='${_lan_flap_base}' link_up='${_lan_up_base}', WAN flap='${_wan_flap_after}'"
    elif ! _p27_arm_lan_watchdog; then
        _step111_ok=0
        _issue111="failed to arm the LAN PF eight-second recovery watchdog"
    elif ! ssh_lan_flap "ip link set '${LAN_FLAP_NIC}' down" >/dev/null 2>&1; then
        _step111_ok=0
        _issue111="failed to set LAN PF ${LAN_FLAP_NIC} down"
    else
        _lan_down_started=$SECONDS
        if ! _p27_wait_link_down 0 14; then
            _step111_ok=0
            _issue111="port 0 did not report link_up=0/speed=0 within 7s (up='${_P27_OBS_UP}' speed='${_P27_OBS_SPEED}')"
        else
            _lan_down_elapsed=$(( SECONDS - _lan_down_started ))
        fi

        # Restore promptly after observing the down state; together with the
        # watchdog this keeps the total down window under ten seconds so the
        # node-side timers other than LSC handling are never exercised.
        _lan_up_started=$SECONDS
        if ! ssh_lan_flap "ip link set '${LAN_FLAP_NIC}' up" >/dev/null 2>&1; then
            _step111_ok=0
            _issue111="${_issue111:+${_issue111}; }failed to set LAN PF ${LAN_FLAP_NIC} up"
        fi
        # Start of the clock the LAN host's vlan rebuild hangs off. Step 115
        # measures against it to tell that rebuild apart from a path that is
        # down for its own reasons.
        _lan_restored_at=$SECONDS

        for _i in $(seq 1 30); do
            _lan_flap_after=$(_p27_read_metric fastrg_nic_link_flaps_total 0 || true)
            if [[ "$_lan_flap_after" =~ ^[0-9]+$ ]]; then
                _lan_delta=$(( _lan_flap_after - _lan_flap_base ))
                [[ $_lan_delta -ge 2 ]] && break
            fi
            sleep 0.5
        done
        if ! _p27_wait_link_up 0 30; then
            _step111_ok=0
            _issue111="${_issue111:+${_issue111}; }port 0 did not recover link_up=1/speed>0 within 15s (up='${_P27_OBS_UP}' speed='${_P27_OBS_SPEED}')"
        else
            _lan_up_elapsed=$(( SECONDS - _lan_up_started ))
            if [[ "$_P27_OBS_SPEED" != "$_P27_LINK_SPEED_MBPS" ]]; then
                _step111_ok=0
                _issue111="${_issue111:+${_issue111}; }port 0 came back at ${_P27_OBS_SPEED} Mbps, expected ${_P27_LINK_SPEED_MBPS}"
            fi
        fi

        # The peer holds a VF, which relinks a few seconds after the PF. Its
        # vlan device keeps the address the whole time, so wait for carrier
        # too — otherwise the next step measures during the relink.
        for _i in $(seq 1 30); do
            if ssh_lan "ip -o link show '${LAN_PEER_NIC}' 2>/dev/null | \
                    grep -q 'LOWER_UP' && \
                    ip -o link show '${_P27_LAN_VLAN}' 2>/dev/null | \
                    grep -q '${_P27_LAN_VLAN}@${LAN_PEER_NIC}' && \
                    ip -4 -o addr show dev '${_P27_LAN_VLAN}' scope global | grep -q ' inet '" \
                    >/dev/null 2>&1; then
                _lan_vlan_ready=1
                break
            fi
            sleep 1
        done
        if [[ $_lan_vlan_ready -ne 1 ]]; then
            _step111_ok=0
            _issue111="${_issue111:+${_issue111}; }${LAN_PEER_NIC} carrier or ${_P27_LAN_VLAN} IPv4 did not recover within 30s"
        fi
    fi
    _p27_restore_lan
    sleep 2

    _wan_after_lan=$(_p27_read_metric fastrg_nic_link_flaps_total 1 || true)
    _lan_new_log=$(_p27_new_log "$_P27_LOG_PATH" "$_lan_log_baseline" || true)
    grep -qF "Port 0 Link Down" <<< "$_lan_new_log" && _lan_down_log=1 || true
    grep -qF "Port 0 Link Up" <<< "$_lan_new_log" && _lan_up_log=1 || true
    if [[ $_lan_delta -ne 2 || "$_wan_after_lan" != "$_wan_flap_after" || \
          $_lan_down_log -ne 1 || $_lan_up_log -ne 1 ]]; then
        _step111_ok=0
        _issue111="${_issue111:+${_issue111}; }LAN flap=${_lan_flap_base}->${_lan_flap_after} delta=${_lan_delta}; WAN flap=${_wan_flap_after}->${_wan_after_lan}; down_log=${_lan_down_log} up_log=${_lan_up_log}; log='$(_p27_log_snippet "$_P27_LOG_PATH" "$_lan_log_baseline")'"
    fi

    if [[ $_step111_ok -eq 1 ]]; then
        pass "Step 114: LAN flap and per-port isolation" \
            "port 0 link 1→0 in ${_lan_down_elapsed}s →1 in ${_lan_up_elapsed}s, speed 0→${_P27_OBS_SPEED} Mbps, flap ${_lan_flap_base}→${_lan_flap_after} (+2); ${LAN_PEER_NIC} carries and ${_P27_LAN_VLAN} has IPv4; port 1 remained ${_wan_after_lan}"
    else
        fail "Step 114: LAN flap and per-port isolation" "$_issue111"
    fi

    # Step 115 — the sub-10s WAN outage must not run link_disconnect or drop data.
    #
    # The flaps above clear the peer's ARP entry for the LAN side, so the first
    # packet after them is consumed by address resolution rather than lost by
    # the data plane. One unscored probe absorbs that, leaving the scored ping
    # to measure the session itself. _p27_probe_wan_path issues that pair, and
    # repeats it while the LAN host still has no route of its own to send on —
    # what moves is when the measurement starts, not what counts as a pass.
    _p27_probe_wan_path "$_lan_restored_at" "$_wan_log_baseline"
    _ping_out="$_P27_PROBE_OUT"
    # Anchored: only a literal zero loss field counts — "50%"/"100% packet loss" must not substring-match as success.
    if ! printf '%s\n' "$_ping_out" | grep -qE '(^|[ ,])0(\.0+)?% packet loss'; then
        _step112_ok=0
        _ping_loss=$(printf '%s\n' "$_ping_out" | \
            grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
        _issue112="${WAN_IP} was not reachable (${_ping_loss:-no response})${_P27_PROBE_NOTE}"
    fi

    _wan_new_log=$(_p27_new_log "$_P27_LOG_PATH" "$_wan_log_baseline" || true)
    _session_log=$(printf '%s\n' "$_wan_new_log" | \
        grep -E 'pppoe is force terminating|pppoe is spawning|HSI module is (terminated|spawned)' || true)
    if [[ -n "$_session_log" ]]; then
        _step112_ok=0
        _issue112="${_issue112:+${_issue112}; }disconnect/redial log observed: '$(printf '%s' "$_session_log" | tr '\n' '|' | tail -c 700 || true)'"
    fi

    if [[ $_step112_ok -eq 1 && ${_p27_needs_path_recovery:-0} -eq 1 ]]; then
        if ! _p27_recover_pppoe_path; then
            _step112_ok=0
            _issue112="${_issue112:+${_issue112}; }PPPoE path recovery failed: ${_P27_PATH_RECOVERY_DETAIL:-unknown}"
        fi
    fi

    if [[ $_step112_ok -eq 1 ]]; then
        _p27_needs_session_recovery=0
        pass "Step 115: preserve session across short WAN flap" \
            "${WAN_IP} reachable with 0% packet loss; no automatic session teardown; ${_P27_PATH_RECOVERY_DETAIL}${_P27_PROBE_NOTE}"
    else
        fail "Step 115: preserve session across short WAN flap" "$_issue112"
    fi

    _cleanup_phase27_link_flap || true
    return 0
}
