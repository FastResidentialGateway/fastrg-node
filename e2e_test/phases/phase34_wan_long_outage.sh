#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 34 — WAN outage long enough to tear PPPoE down (Steps 137-139)
#
# The node arms a ten-second timer when the WAN link drops and tears every
# PPPoE session down if the link is still gone when it fires. Phase 27 keeps
# its outage under that threshold and requires the sessions to survive; this
# phase drives the other half of the same timer: stay down past it, require
# the teardown, then require the node to dial back on its own once the link
# returns. Nothing here issues a dial — recovery has to come from the node's
# own reconcile cycle, which is what a real line cut looks like.
# ---------------------------------------------------------------------------

# Nominal speed of the WAN link on this bench; the port must report exactly
# this once the outage is over.
_P34_LINK_SPEED_MBPS=10000
# How long the link stays down. It has to clear the node's ten-second
# teardown timer with room to spare for the moment the node needs to notice
# the link is gone.
_P34_OUTAGE_SEC=14
# Last-resort restore if the runner dies mid-outage. Deliberately longer than
# the outage: this phase wants the teardown to happen, so the watchdog must
# not cut the outage short — it only guarantees the bench never stays down.
_P34_WATCHDOG_SEC=40
# The node redials from its periodic reconcile pass, so recovery waits for
# that cycle rather than for a link event. Measured on this bench at roughly
# half a minute; the budget leaves room for a pass that lands while the link
# is still down and has to wait for the next one.
_P34_RECOVERY_BUDGET_SEC=150
_P34_WAN_RETURN_ROUTE="192.168.200.128/25"
_P34_WAN_RETURN_GATEWAY="192.168.201.1"
_P34_LOG_PATH=""
_p34_needs_recovery=0

# The exact host-side command that puts the WAN link back, defined once so the
# watchdog and every restore path stay in step. Double quotes because the
# watchdog embeds it inside a single-quoted sh -c.
_p34_wan_restore_cmd() {
    printf 'ip link set "%s" up; ip route replace "%s" via "%s" dev "%s"' \
        "$WAN_NIC" "$_P34_WAN_RETURN_ROUTE" "$_P34_WAN_RETURN_GATEWAY" "$WAN_NIC"
}

_p34_wan_link_kill() {
    ssh_wan "ip link set '${WAN_NIC}' down" >/dev/null 2>&1
}

_p34_wan_link_restore() {
    ssh_wan "$(_p34_wan_restore_cmd)" >/dev/null 2>&1 || true
}

_p34_arm_wan_watchdog() {
    ssh_wan "nohup sh -c 'sleep ${_P34_WATCHDOG_SEC}; $(_p34_wan_restore_cmd)' \
        >/dev/null 2>&1 </dev/null &" >/dev/null 2>&1
}

# True once the peer host has actually lost carrier. LOWER_UP is the carrier
# bit: while it is still set the peer is up on the wire whatever its
# administrative state says, and the node has nothing to detect.
_p34_wan_carrier_is_down() {
    local _i _state

    for _i in $(seq 1 5); do
        _state=$(ssh_wan "ip link show '${WAN_NIC}' 2>/dev/null" 2>/dev/null || true)
        printf '%s\n' "$_state" | grep -q 'LOWER_UP' || return 0
        sleep 0.3
    done
    return 1
}

_p34_link_value() {
    local _body="$1" _metric="$2" _port="$3"

    e2e_metric_value "$_body" "$_metric" "port_id=${_port}"
}

_p34_wait_link_down() {
    local _attempts="${1:-14}" _i _body

    for _i in $(seq 1 "$_attempts"); do
        _body=$(e2e_metrics_body || true)
        _P34_OBS_UP=$(_p34_link_value "$_body" fastrg_nic_link_up 1)
        _P34_OBS_SPEED=$(_p34_link_value "$_body" fastrg_nic_link_speed_mbps 1)
        [[ "$_P34_OBS_UP" == "0" && "$_P34_OBS_SPEED" == "0" ]] && return 0
        sleep 0.5
    done
    return 1
}

_p34_wait_link_up() {
    local _attempts="${1:-30}" _i _body

    for _i in $(seq 1 "$_attempts"); do
        _body=$(e2e_metrics_body || true)
        _P34_OBS_UP=$(_p34_link_value "$_body" fastrg_nic_link_up 1)
        _P34_OBS_SPEED=$(_p34_link_value "$_body" fastrg_nic_link_speed_mbps 1)
        [[ "$_P34_OBS_UP" == "1" && "$_P34_OBS_SPEED" =~ ^[1-9][0-9]*$ ]] && return 0
        sleep 0.5
    done
    return 1
}

_p34_log_line_count() {
    local _count

    _count=$(ssh_node "wc -l < '${_P34_LOG_PATH}' 2>/dev/null || echo 0" 2>/dev/null | \
        tail -1 | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

_p34_new_log() {
    local _baseline="$1"

    [[ "$_baseline" =~ ^[0-9]+$ ]] || _baseline=0
    ssh_node "tail -n +$(( _baseline + 1 )) '${_P34_LOG_PATH}' 2>/dev/null || true" \
        2>/dev/null || true
}

_p34_user_status() {
    fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[] | select(.user_id == $1) | .status" 2>/dev/null || true
}

# Number of "User <id> pppoe is <what>" lines for one subscriber in a log slice.
_p34_user_log_hits() {
    local _count

    _count=$(printf '%s\n' "$1" | grep -cF "User $2 pppoe is $3" || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

# Every canonical subscriber back in Data phase. Prints the ones that are not.
_p34_users_not_in_data_phase() {
    local _uid _status _out=""

    for _uid in "${SUB_IDS[@]}"; do
        _status=$(_p34_user_status "$_uid")
        [[ "$_status" == "Data phase" ]] || _out="${_out}${_out:+, }user ${_uid}='${_status:-unreachable}'"
    done
    printf '%s' "$_out"
}

# Idempotent: called at the end of the phase and from the top-level EXIT trap.
# Bringing the link back is always safe; the redial fallback only runs when the
# node has not recovered on its own, so a healthy run never dials by hand.
_cleanup_phase34_wan_long_outage() {
    local _i _uid _left

    _p34_wan_link_restore
    [[ ${_p34_needs_recovery:-0} -eq 1 ]] || return 0

    for _i in $(seq 1 "$_P34_RECOVERY_BUDGET_SEC"); do
        _left=$(_p34_users_not_in_data_phase)
        if [[ -z "$_left" ]]; then
            _p34_needs_recovery=0
            return 0
        fi
        sleep 1
    done

    warn "Cleanup(phase34): sessions did not return on their own (${_left}); dialling them back."
    for _uid in "${SUB_IDS[@]}"; do
        fastrg_grpc connect_hsi "${_uid}" >/dev/null 2>&1 || true
    done
    for _i in $(seq 1 60); do
        [[ -z "$(_p34_users_not_in_data_phase)" ]] && { _p34_needs_recovery=0; return 0; }
        sleep 2
    done
    warn "Cleanup(phase34): subscribers still not in Data phase after the fallback dial."
    return 1
}

phase34_wan_long_outage() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 34 — WAN Long Outage / PPPoE Teardown (Steps 137-139)"
    bold "═══════════════════════════════════════════════════════"

    local _step137_ok=1 _step138_ok=1 _step139_ok=1
    local _issue137="" _issue138="" _issue139=""
    local _body="" _log_baseline=0 _outage_log="" _round_log=""
    local _wan_flap_base="" _wan_flap_after="" _wan_delta=-1
    local _lan_flap_base="" _lan_flap_after="" _lan_delta=-1
    local _uid _hits _left _down_at _elapsed=-1 _i
    local _ping_out="" _ping_loss="" _etcd_count="" _local_count="" _keys="" _key_ids=""

    _P34_LOG_PATH=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" 2>/dev/null | \
        awk -F'"' '{print $2}' || true)
    [[ -n "$_P34_LOG_PATH" ]] || _P34_LOG_PATH=/var/log/fastrg/fastrg.log

    # Step 137 — the outage must take the sessions down.
    _body=$(e2e_metrics_body || true)
    _wan_flap_base=$(_p34_link_value "$_body" fastrg_nic_link_flaps_total 1)
    _lan_flap_base=$(_p34_link_value "$_body" fastrg_nic_link_flaps_total 0)
    _left=$(_p34_users_not_in_data_phase)
    _log_baseline=$(_p34_log_line_count)

    if [[ -n "$_left" ]]; then
        _step137_ok=0
        _issue137="precondition: subscribers were not all in Data phase before the outage (${_left})"
    elif [[ "$(_p34_link_value "$_body" fastrg_nic_link_up 1)" != "1" || \
            ! "$_wan_flap_base" =~ ^[0-9]+$ || ! "$_lan_flap_base" =~ ^[0-9]+$ ]]; then
        _step137_ok=0
        _issue137="precondition: port 1 was not up with readable counters (link_up='$(_p34_link_value "$_body" fastrg_nic_link_up 1)' wan_flap='${_wan_flap_base}' lan_flap='${_lan_flap_base}')"
    elif ! _p34_arm_wan_watchdog; then
        _step137_ok=0
        _issue137="failed to arm the WAN peer ${_P34_WATCHDOG_SEC}-second recovery watchdog"
    elif ! _p34_wan_link_kill; then
        _step137_ok=0
        _issue137="failed to set WAN peer ${WAN_NIC} down"
    else
        _p34_needs_recovery=1
        _down_at=$SECONDS
        if ! _p34_wan_carrier_is_down; then
            _step137_ok=0
            _issue137="WAN down ineffective on host side (${WAN_NIC} still reports carrier after admin down)"
        fi
        if ! _p34_wait_link_down 14; then
            _step137_ok=0
            _issue137="${_issue137:+${_issue137}; }port 1 did not report link_up=0/speed=0 within 7s (up='${_P34_OBS_UP}' speed='${_P34_OBS_SPEED}')"
        fi

        info "Step 137: holding the WAN link down for ${_P34_OUTAGE_SEC}s to clear the node's teardown timer..."
        while (( SECONDS - _down_at < _P34_OUTAGE_SEC )); do
            sleep 1
        done

        # Read the evidence while the link is still down, so the teardown
        # cannot be confused with anything the restore does.
        _outage_log=$(_p34_new_log "$_log_baseline")
        for _uid in "${SUB_IDS[@]}"; do
            _hits=$(_p34_user_log_hits "$_outage_log" "$_uid" "force terminating")
            [[ "$_hits" -ge 1 ]] || \
                _issue137="${_issue137:+${_issue137}; }user ${_uid} logged no forced PPPoE termination"
        done
        _left=$(_p34_users_not_in_data_phase)
        if [[ -z "$_left" ]]; then
            _issue137="${_issue137:+${_issue137}; }subscribers were still in Data phase after a ${_P34_OUTAGE_SEC}s outage"
        fi
        [[ -n "$_issue137" ]] && _step137_ok=0
    fi

    if [[ $_step137_ok -eq 1 ]]; then
        pass "Step 137: WAN outage past the teardown timer" \
            "${_P34_OUTAGE_SEC}s outage; every subscriber logged a forced PPPoE termination and left Data phase (${_left})"
    else
        fail "Step 137: WAN outage past the teardown timer" "$_issue137"
    fi

    # Step 138 — the node has to dial back by itself once the link returns.
    if [[ $_step137_ok -eq 0 ]]; then
        _step138_ok=0
        _issue138="teardown prerequisite failed"
    else
        _p34_wan_link_restore
        if ! _p34_wait_link_up 30; then
            _step138_ok=0
            _issue138="port 1 did not recover link_up=1/speed>0 within 15s (up='${_P34_OBS_UP}' speed='${_P34_OBS_SPEED}')"
        elif [[ "$_P34_OBS_SPEED" != "$_P34_LINK_SPEED_MBPS" ]]; then
            _step138_ok=0
            _issue138="port 1 came back at ${_P34_OBS_SPEED} Mbps, expected ${_P34_LINK_SPEED_MBPS}"
        fi

        info "Step 138: waiting up to ${_P34_RECOVERY_BUDGET_SEC}s for the node to redial on its own..."
        _down_at=$SECONDS
        for _i in $(seq 1 "$_P34_RECOVERY_BUDGET_SEC"); do
            _left=$(_p34_users_not_in_data_phase)
            if [[ -z "$_left" ]]; then
                _elapsed=$(( SECONDS - _down_at ))
                break
            fi
            sleep 1
        done
        if [[ -n "$_left" ]]; then
            _step138_ok=0
            _issue138="${_issue138:+${_issue138}; }subscribers did not return to Data phase within ${_P34_RECOVERY_BUDGET_SEC}s (${_left})"
        else
            _p34_needs_recovery=0
            _round_log=$(_p34_new_log "$_log_baseline")
            for _uid in "${SUB_IDS[@]}"; do
                _hits=$(_p34_user_log_hits "$_round_log" "$_uid" spawning)
                [[ "$_hits" -ge 1 ]] || \
                    _issue138="${_issue138:+${_issue138}; }user ${_uid} returned without a PPPoE spawn in this run's log"
            done
            [[ -n "$_issue138" ]] && _step138_ok=0
        fi
    fi

    if [[ $_step138_ok -eq 1 ]]; then
        pass "Step 138: subscribers redial themselves after the outage" \
            "users ${SUB_IDS[*]} back in Data phase ${_elapsed}s after the link returned, with no dial from the test"
    else
        fail "Step 138: subscribers redial themselves after the outage" "$_issue138"
    fi

    # Step 139 — data plane back, and the outage confined to the WAN port.
    if [[ $_step138_ok -eq 0 ]]; then
        _step139_ok=0
        _issue139="redial prerequisite failed"
    else
        # The outage clears the peer's ARP entry, so the first packet back is
        # spent on address resolution rather than lost by the data plane.
        ssh_lan "ping -c 1 -W 2 ${WAN_IP}" >/dev/null 2>&1 || true
        _ping_out=$(ssh_lan "ping -c 4 -W 3 ${WAN_IP}" 2>&1 || true)
        if ! printf '%s\n' "$_ping_out" | grep -qE '0% packet loss|0\.0% packet loss'; then
            _step139_ok=0
            _ping_loss=$(printf '%s\n' "$_ping_out" | \
                grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
            _issue139="${WAN_IP} was not reachable (${_ping_loss:-no response})"
        fi

        _body=$(e2e_metrics_body || true)
        _wan_flap_after=$(_p34_link_value "$_body" fastrg_nic_link_flaps_total 1)
        _lan_flap_after=$(_p34_link_value "$_body" fastrg_nic_link_flaps_total 0)
        if e2e_all_uint "$_wan_flap_base" "$_wan_flap_after" "$_lan_flap_base" "$_lan_flap_after"; then
            _wan_delta=$(( _wan_flap_after - _wan_flap_base ))
            _lan_delta=$(( _lan_flap_after - _lan_flap_base ))
        fi
        if [[ $_wan_delta -ne 2 || $_lan_delta -ne 0 ]]; then
            _step139_ok=0
            _issue139="${_issue139:+${_issue139}; }WAN flap ${_wan_flap_base}->${_wan_flap_after} (delta=${_wan_delta}, expected 2); LAN flap ${_lan_flap_base}->${_lan_flap_after} (delta=${_lan_delta}, expected 0)"
        fi

        _local_count=$(fastrg_grpc get_system_info 2>/dev/null | \
            jq -r '.num_users // empty' 2>/dev/null || true)
        _etcd_count=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null | \
            jq -r '.subscriber_count // empty' 2>/dev/null || true)
        _keys=$(ssh_node \
            "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} get --prefix --keys-only configs/${NODE_UUID}/hsi/" \
            2>/dev/null || true)
        _key_ids=$(printf '%s\n' "$_keys" | awk -F/ 'NF {print $NF}' | sort -n | tr '\n' ' ' | \
            sed 's/ $//' || true)
        if [[ "$_local_count" != "2" || "$_etcd_count" != "2" || "$_key_ids" != "1 2" ]]; then
            _step139_ok=0
            _issue139="${_issue139:+${_issue139}; }fixture drifted: local_count='${_local_count:-empty}' etcd_count='${_etcd_count:-empty}' hsi_keys='${_key_ids:-empty}'"
        fi
    fi

    if [[ $_step139_ok -eq 1 ]]; then
        pass "Step 139: data plane and fixture after the outage" \
            "${WAN_IP} reachable with 0% packet loss; port 1 flap +${_wan_delta}, port 0 unchanged; count=2, HSI keys=1,2"
    else
        fail "Step 139: data plane and fixture after the outage" "$_issue139"
    fi

    _cleanup_phase34_wan_long_outage || true
    return 0
}
