#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 27 — NIC link flap / LSC handling (Steps 110-112)
# ---------------------------------------------------------------------------

set -euo pipefail

_P27_LAN_NIC="enp7s0"
_P27_WAN_RETURN_ROUTE="192.168.200.128/25"
_P27_WAN_RETURN_GATEWAY="192.168.201.1"
_P27_LOG_PATH=""
_P27_METRICS_PORT=""
_P27_BRAS_SSH_PID=""
_P27_PATH_RECOVERY_DETAIL=""
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

_p27_restore_wan() {
    ssh_wan "ip link set '${WAN_NIC}' up; \
        ip route replace '${_P27_WAN_RETURN_ROUTE}' via '${_P27_WAN_RETURN_GATEWAY}' dev '${WAN_NIC}'" \
        >/dev/null 2>&1 || true
}

_p27_restore_lan() {
    ssh_lan "ip link set '${_P27_LAN_NIC}' up" >/dev/null 2>&1 || true
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
    ssh_wan "nohup sh -c 'sleep 8; ip link set \"${WAN_NIC}\" up; \
        ip route replace \"${_P27_WAN_RETURN_ROUTE}\" via \
        \"${_P27_WAN_RETURN_GATEWAY}\" dev \"${WAN_NIC}\"' \
        >/dev/null 2>&1 </dev/null &" >/dev/null 2>&1
}

# Idempotent: called after phase27 and from the top-level EXIT trap.
_cleanup_phase27_link_flap() {
    local _i _ping=""

    _p27_restore_wan
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
            if printf '%s\n' "$_ping" | grep -qE '0% packet loss|0\.0% packet loss'; then
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
    bold " Phase 27 — NIC Link Flap / LSC Handling (Steps 110-112)"
    bold "═══════════════════════════════════════════════════════"

    local _step110_ok=1 _step111_ok=1 _step112_ok=1
    local _issue110="" _issue111="" _issue112=""
    local _mport_raw="" _config_log_path="" _wan_log_baseline=0
    local _wan_flap_base="" _wan_flap_after="" _wan_delta=-1
    local _lan_flap_base="" _lan_flap_after="" _lan_delta=-1
    local _wan_after_lan="" _wan_up_base="" _lan_up_base=""
    local _wan_new_log="" _down_log=0 _up_log=0
    local _lan_log_baseline=0 _lan_new_log="" _lan_down_log=0 _lan_up_log=0 _i
    local _session_log="" _ping_out="" _ping_loss=""

    _mport_raw=$(ssh_node "grep 'MetricsListenPort' /etc/fastrg/config.cfg 2>/dev/null" | \
        awk -F'"' '{print $2}' || true)
    _P27_METRICS_PORT="${_mport_raw##*:}"
    _config_log_path=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" || true)
    _P27_LOG_PATH=$(printf '%s' "$_config_log_path" | awk -F'"' '{print $2}' || true)
    [[ -n "$_P27_LOG_PATH" ]] || _P27_LOG_PATH=/var/log/fastrg/fastrg.log

    _cleanup_phase27_link_flap || true

    # Step 110 — WAN LSC down/up, speed cache, flap count, and current-run logs.
    _wan_flap_base=$(_p27_read_metric fastrg_nic_link_flaps_total 1 || true)
    _wan_up_base=$(_p27_read_metric fastrg_nic_link_up 1 || true)
    _wan_log_baseline=$(_p27_log_line_count "$_P27_LOG_PATH")
    if [[ -z "$_P27_METRICS_PORT" || ! "$_wan_flap_base" =~ ^[0-9]+$ || \
          "$_wan_up_base" != "1" ]]; then
        _step110_ok=0
        _issue110="invalid baseline: metrics_port='${_P27_METRICS_PORT}' flap='${_wan_flap_base}' link_up='${_wan_up_base}'"
    elif ! _p27_arm_wan_watchdog; then
        _step110_ok=0
        _issue110="failed to arm the WAN peer eight-second recovery watchdog"
    elif ! ssh_wan "ip link set '${WAN_NIC}' down" >/dev/null 2>&1; then
        _step110_ok=0
        _issue110="failed to set WAN peer ${WAN_NIC} down"
    else
        _p27_needs_session_recovery=1
        _p27_needs_path_recovery=1
        if ! _p27_wait_link_down 1 14; then
            _step110_ok=0
            _issue110="port 1 did not report link_up=0/speed=0 within 7s (up='${_P27_OBS_UP}' speed='${_P27_OBS_SPEED}')"
        fi
        # Restore immediately after observing down; the total down window remains below 10s.
        _p27_restore_wan
        if ! _p27_wait_link_up 1 20; then
            _step110_ok=0
            _issue110="${_issue110:+${_issue110}; }port 1 did not recover link_up=1/speed>0 within 10s (up='${_P27_OBS_UP}' speed='${_P27_OBS_SPEED}')"
        fi
    fi
    _p27_restore_wan
    sleep 2

    _wan_flap_after=$(_p27_read_metric fastrg_nic_link_flaps_total 1 || true)
    if [[ "$_wan_flap_base" =~ ^[0-9]+$ && "$_wan_flap_after" =~ ^[0-9]+$ ]]; then
        _wan_delta=$(( _wan_flap_after - _wan_flap_base ))
    fi
    _wan_new_log=$(_p27_new_log "$_P27_LOG_PATH" "$_wan_log_baseline" || true)
    printf '%s\n' "$_wan_new_log" | grep -qF "Port 1 Link Down" && _down_log=1 || true
    printf '%s\n' "$_wan_new_log" | grep -qF "Port 1 Link Up" && _up_log=1 || true
    if [[ $_wan_delta -lt 2 || $(( _wan_delta % 2 )) -ne 0 || \
          $_down_log -ne 1 || $_up_log -ne 1 ]]; then
        _step110_ok=0
        _issue110="${_issue110:+${_issue110}; }flap=${_wan_flap_base}->${_wan_flap_after} delta=${_wan_delta}; down_log=${_down_log} up_log=${_up_log}; log='$(_p27_log_snippet "$_P27_LOG_PATH" "$_wan_log_baseline")'"
    fi

    if [[ $_step110_ok -eq 1 ]]; then
        pass "Step 110: WAN LSC event and flap counter" \
            "port 1 link 1→0→1, speed 0→${_P27_OBS_SPEED} Mbps, flap ${_wan_flap_base}→${_wan_flap_after} (+${_wan_delta}, even), current-run down/up logs present"
    else
        fail "Step 110: WAN LSC event and flap counter" "$_issue110"
    fi

    # Let the WAN events settle before independently flapping the LAN peer.
    sleep 2

    # Step 111 — LAN flap: per-port counter isolation and paired LSC delivery.
    #
    # This bench's LAN NIC (port 0) does not deliver the LSC down event while
    # the peer link stays down: the driver coalesces and delivers the down+up
    # pair together once the link returns (reproduced on a clean baseline;
    # recorded as root_plan task-30). The intermediate link_up=0 state is
    # therefore only asserted on the WAN side (Step 110); here we assert the
    # paired delivery, the exact +2 flap delta on port 0, and that port 1's
    # counter is untouched.
    _lan_flap_base=$(_p27_read_metric fastrg_nic_link_flaps_total 0 || true)
    _lan_up_base=$(_p27_read_metric fastrg_nic_link_up 0 || true)
    _lan_log_baseline=$(_p27_log_line_count "$_P27_LOG_PATH")
    if [[ ! "$_lan_flap_base" =~ ^[0-9]+$ || "$_lan_up_base" != "1" || \
          ! "$_wan_flap_after" =~ ^[0-9]+$ ]]; then
        _step111_ok=0
        _issue111="invalid baseline: LAN flap='${_lan_flap_base}' link_up='${_lan_up_base}', WAN flap='${_wan_flap_after}'"
    elif ! ssh_lan "ip link set '${_P27_LAN_NIC}' down" >/dev/null 2>&1; then
        _step111_ok=0
        _issue111="failed to set LAN peer ${_P27_LAN_NIC} down"
    else
        sleep 8   # hold the peer down for the same window as the WAN flap
        _p27_restore_lan
        # The coalesced down+up pair lands after the link returns; wait for
        # the +2 counter delta instead of an intermediate down state.
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
            _issue111="port 0 did not recover link_up=1/speed>0 within 15s (up='${_P27_OBS_UP}' speed='${_P27_OBS_SPEED}')"
        fi
    fi
    _p27_restore_lan
    sleep 2

    _wan_after_lan=$(_p27_read_metric fastrg_nic_link_flaps_total 1 || true)
    _lan_new_log=$(_p27_new_log "$_P27_LOG_PATH" "$_lan_log_baseline" || true)
    printf '%s\n' "$_lan_new_log" | grep -qF "Port 0 Link Down" && _lan_down_log=1 || true
    printf '%s\n' "$_lan_new_log" | grep -qF "Port 0 Link Up" && _lan_up_log=1 || true
    if [[ $_lan_delta -ne 2 || "$_wan_after_lan" != "$_wan_flap_after" || \
          $_lan_down_log -ne 1 || $_lan_up_log -ne 1 ]]; then
        _step111_ok=0
        _issue111="${_issue111:+${_issue111}; }LAN flap=${_lan_flap_base}->${_lan_flap_after} delta=${_lan_delta}; WAN flap=${_wan_flap_after}->${_wan_after_lan}; down_log=${_lan_down_log} up_log=${_lan_up_log}; log='$(_p27_log_snippet "$_P27_LOG_PATH" "$_lan_log_baseline")'"
    fi

    if [[ $_step111_ok -eq 1 ]]; then
        pass "Step 111: LAN flap and per-port isolation" \
            "port 0 flap ${_lan_flap_base}→${_lan_flap_after} (+2, paired down/up delivered on restore), final link_up=1/speed>0; port 1 remained ${_wan_after_lan}"
    else
        fail "Step 111: LAN flap and per-port isolation" "$_issue111"
    fi

    # Step 112 — the sub-10s WAN outage must not run link_disconnect or drop data.
    _ping_out=$(ssh_lan "ping -c 4 -W 3 ${WAN_IP}" 2>&1 || true)
    if ! printf '%s\n' "$_ping_out" | grep -qE '0% packet loss|0\.0% packet loss'; then
        _step112_ok=0
        _ping_loss=$(printf '%s\n' "$_ping_out" | \
            grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
        _issue112="${WAN_IP} was not reachable (${_ping_loss:-no response})"
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
        pass "Step 112: preserve session across short WAN flap" \
            "${WAN_IP} reachable with 0% packet loss; no automatic session teardown; ${_P27_PATH_RECOVERY_DETAIL}"
    else
        fail "Step 112: preserve session across short WAN flap" "$_issue112"
    fi

    _cleanup_phase27_link_flap || true
    return 0
}
