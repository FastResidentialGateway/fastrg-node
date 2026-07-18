#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 31 — Subscriber slot scale boundary (Steps 123-126)
#
# MaxUserCount is read from the node after the runner temporarily sets it from
# E2E_MAX_USER_COUNT (default 100). A 100-slot run requires about 15.5 GB of
# hugepages because each subscriber consumes about 148.5 MB including its
# ppp_ccb and mac_table. If hugepages are insufficient, node startup fails at
# ppp_ccb mempool or mac_table allocation. The apply timeout has a fixed
# 60-second base plus one second per four slots; keep any future adjustment
# proportional to the configured scale rather than using a fixed timeout.
# ---------------------------------------------------------------------------

set -euo pipefail

_P31_LAST_LOCAL_COUNT=""
_P31_LAST_ETCD_COUNT=""
_P31_MP_AVAIL=""
_P31_MP_IN_USE=""
_P31_MP_SIZE=""

_p31_read_count_state() {
    local _system_json="" _etcd_json=""

    _system_json=$(fastrg_grpc get_system_info 2>/dev/null || true)
    _P31_LAST_LOCAL_COUNT=$(printf '%s' "$_system_json" | \
        jq -r '.num_users // empty' 2>/dev/null | tr -d '[:space:]' || true)
    _etcd_json=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null || true)
    _P31_LAST_ETCD_COUNT=$(printf '%s' "$_etcd_json" | \
        jq -r '.subscriber_count // empty' 2>/dev/null | tr -d '[:space:]' || true)
}

_p31_wait_count_state() {
    local _target="$1" _timeout="$2" _started=$SECONDS

    while (( SECONDS - _started <= _timeout )); do
        _p31_read_count_state
        if [[ "$_P31_LAST_LOCAL_COUNT" == "$_target" && \
              "$_P31_LAST_ETCD_COUNT" == "$_target" ]]; then
            return 0
        fi
        sleep 2
    done
    return 1
}

_p31_read_ppp_mempool() {
    local _stats_json=""

    _stats_json=$(fastrg_grpc get_system_stats 2>/dev/null || true)
    _P31_MP_AVAIL=$(printf '%s' "$_stats_json" | jq -r \
        '[.mempool_stats[]? | select(.name == "ppp_ccb_pool")][0].avail_count // empty' \
        2>/dev/null | tr -d '[:space:]' || true)
    _P31_MP_IN_USE=$(printf '%s' "$_stats_json" | jq -r \
        '[.mempool_stats[]? | select(.name == "ppp_ccb_pool")][0].in_use_count // empty' \
        2>/dev/null | tr -d '[:space:]' || true)
    _P31_MP_SIZE=$(printf '%s' "$_stats_json" | jq -r \
        '[.mempool_stats[]? | select(.name == "ppp_ccb_pool")][0].size // empty' \
        2>/dev/null | tr -d '[:space:]' || true)

    [[ "$_P31_MP_AVAIL" =~ ^[0-9]+$ && "$_P31_MP_IN_USE" =~ ^[0-9]+$ && \
       "$_P31_MP_SIZE" =~ ^[0-9]+$ ]]
}

_p31_node_alive() {
    local _system_json="" _count=""

    _system_json=$(fastrg_grpc get_system_info 2>/dev/null || true)
    _count=$(printf '%s' "$_system_json" | jq -r '.num_users // empty' \
        2>/dev/null | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ && "$_count" -gt 0 ]]
}

# Idempotent and unconditional: called by phase31 and by the top-level EXIT trap.
_cleanup_phase31_subscriber_scale() {
    local _cleanup_timeout=90

    [[ -n "${NODE_UUID:-}" ]] || return 0
    info "Cleanup(phase31): restoring canonical subscriber count to 2..."
    fastrg_grpc set_subscriber_count 2 >/dev/null 2>&1 || true
    _p31_wait_count_state 2 "$_cleanup_timeout" >/dev/null 2>&1 || true
}

phase31_subscriber_scale() {
    local _config_line="" _max="" _over="" _timeout=0 _reply="" _status=""
    local _before_avail="" _before_in_use="" _at_max_avail="" _at_max_in_use=""
    local _over_avail="" _over_in_use="" _shrink_avail="" _shrink_in_use=""
    local _issue123="" _issue124="" _issue125="" _issue126=""
    local _stable_local="" _stable_etcd="" _uid="" _phase="" _ping_out="" _loss=""

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 31 — Subscriber Slot Scale Boundary (Steps 123-126)"
    bold "═══════════════════════════════════════════════════════"

    _config_line=$(ssh_node \
        "grep -E '^[[:space:]]*MaxUserCount[[:space:]]*=' /etc/fastrg/config.cfg 2>/dev/null" \
        2>/dev/null || true)
    _max=$(printf '%s' "$_config_line" | awk -F'[=;]' \
        '{gsub(/[[:space:]]/, "", $2); print $2; exit}' || true)

    if [[ ! "$_max" =~ ^[0-9]+$ || "$_max" -lt 2 ]]; then
        _issue123="cannot read a valid MaxUserCount from node config: ${_config_line:-empty}"
        fail "Step 123: expand subscriber slots to configured max" "$_issue123"
        fail "Step 124: observe configured max+1 behavior" "precondition failed: $_issue123"
        fail "Step 125: shrink subscriber slots to canonical count" "precondition failed: $_issue123"
        fail "Step 126: data plane healthy after resize" "precondition failed: $_issue123"
        _cleanup_phase31_subscriber_scale
        return 0
    fi

    _over=$(( _max + 1 ))
    _timeout=$(( 60 + (_max / 4) ))
    info "Node MaxUserCount=${_max}; resize timeout=${_timeout}s."

    _p31_read_count_state
    if ! _p31_read_ppp_mempool; then
        _issue123="ppp_ccb_pool is absent from GetFastrgSystemStats"
    else
        _before_avail=$_P31_MP_AVAIL
        _before_in_use=$_P31_MP_IN_USE
    fi

    _reply=$(fastrg_grpc set_subscriber_count "$_max" 2>/dev/null || true)
    _status=$(printf '%s' "$_reply" | jq -r '.status // empty' 2>/dev/null || true)
    [[ -n "$_status" ]] || _issue123="${_issue123:+${_issue123}; }set_subscriber_count returned no status: ${_reply:-empty}"
    if ! _p31_wait_count_state "$_max" "$_timeout"; then
        _issue123="${_issue123:+${_issue123}; }count did not converge (local=${_P31_LAST_LOCAL_COUNT:-empty}, etcd=${_P31_LAST_ETCD_COUNT:-empty})"
    fi
    if _p31_read_ppp_mempool; then
        _at_max_avail=$_P31_MP_AVAIL
        _at_max_in_use=$_P31_MP_IN_USE
    else
        _issue123="${_issue123:+${_issue123}; }cannot read ppp_ccb_pool after expansion"
    fi
    if ! _p31_node_alive; then
        _issue123="${_issue123:+${_issue123}; }node is not responsive"
    fi

    # A count larger than the number of HSI keys must remain stable: empty slots
    # have vlan_id=0 and the reconcile sweep ignores them rather than rolling back.
    sleep 3
    _p31_read_count_state
    _stable_local=$_P31_LAST_LOCAL_COUNT
    _stable_etcd=$_P31_LAST_ETCD_COUNT
    if [[ "$_stable_local" != "$_max" || "$_stable_etcd" != "$_max" ]]; then
        _issue123="${_issue123:+${_issue123}; }count was not stable after reconcile (local=${_stable_local:-empty}, etcd=${_stable_etcd:-empty})"
    fi

    if [[ -z "$_issue123" ]]; then
        pass "Step 123: expand subscriber slots to configured max" \
            "max=${_max}; local/etcd stable; ppp_ccb_pool avail ${_before_avail}->${_at_max_avail}, in_use ${_before_in_use}->${_at_max_in_use}; node responsive"
    else
        fail "Step 123: expand subscriber slots to configured max" "$_issue123"
    fi

    # Current SDN behavior: the controller/etcd path validates only count > 0,
    # not the node's configured MaxUserCount. Because the mempool is rounded up
    # to the next power of two, max+1 is accepted while a spare object exists.
    _reply=$(fastrg_grpc set_subscriber_count "$_over" 2>/dev/null || true)
    _status=$(printf '%s' "$_reply" | jq -r '.status // empty' 2>/dev/null || true)
    [[ -n "$_status" ]] || _issue124="set_subscriber_count(${_over}) returned no status: ${_reply:-empty}"
    if ! _p31_wait_count_state "$_over" "$_timeout"; then
        _issue124="${_issue124:+${_issue124}; }current max+1 acceptance did not converge (local=${_P31_LAST_LOCAL_COUNT:-empty}, etcd=${_P31_LAST_ETCD_COUNT:-empty})"
    fi
    if _p31_read_ppp_mempool; then
        _over_avail=$_P31_MP_AVAIL
        _over_in_use=$_P31_MP_IN_USE
    else
        _issue124="${_issue124:+${_issue124}; }cannot read ppp_ccb_pool after max+1"
    fi
    if ! _p31_node_alive; then
        _issue124="${_issue124:+${_issue124}; }node is not responsive after max+1"
    fi

    if [[ -z "$_issue124" ]]; then
        pass "Step 124: observe configured max+1 behavior" \
            "existing defect reproduced: configured max=${_max}, accepted=${_over}; local/etcd=${_over}; ppp_ccb_pool avail=${_over_avail}, in_use=${_over_in_use}; node responsive"
    else
        fail "Step 124: observe configured max+1 behavior" "$_issue124"
    fi

    _reply=$(fastrg_grpc set_subscriber_count 2 2>/dev/null || true)
    _status=$(printf '%s' "$_reply" | jq -r '.status // empty' 2>/dev/null || true)
    [[ -n "$_status" ]] || _issue125="set_subscriber_count(2) returned no status: ${_reply:-empty}"
    if ! _p31_wait_count_state 2 "$_timeout"; then
        _issue125="${_issue125:+${_issue125}; }canonical count did not converge (local=${_P31_LAST_LOCAL_COUNT:-empty}, etcd=${_P31_LAST_ETCD_COUNT:-empty})"
    fi
    if _p31_read_ppp_mempool; then
        _shrink_avail=$_P31_MP_AVAIL
        _shrink_in_use=$_P31_MP_IN_USE
        if [[ -n "$_over_avail" && ( "$_shrink_avail" != "$_over_avail" || \
              "$_shrink_in_use" != "$_over_in_use" ) ]]; then
            _issue125="${_issue125:+${_issue125}; }CCB retention changed unexpectedly (before shrink avail/in_use=${_over_avail}/${_over_in_use}, after=${_shrink_avail}/${_shrink_in_use})"
        fi
    else
        _issue125="${_issue125:+${_issue125}; }cannot read ppp_ccb_pool after shrink"
    fi
    if ! _p31_node_alive; then
        _issue125="${_issue125:+${_issue125}; }node is not responsive after shrink"
    fi

    if [[ -z "$_issue125" ]]; then
        pass "Step 125: shrink subscriber slots to canonical count" \
            "local/etcd=2; RCU resize returned; CCB pool intentionally retained at avail/in_use=${_shrink_avail}/${_shrink_in_use}; node responsive"
    else
        fail "Step 125: shrink subscriber slots to canonical count" "$_issue125"
    fi

    for _uid in 1 2; do
        _phase=$(fastrg_grpc get_hsi_info 2>/dev/null | jq -r \
            ".hsi_infos[]? | select(.user_id == ${_uid}) | .status" 2>/dev/null || true)
        if [[ "$_phase" != "Data phase" ]]; then
            _issue126="${_issue126:+${_issue126}; }user ${_uid} status='${_phase:-missing}'"
        fi
    done
    _ping_out=$(ssh_lan "ping -c 4 -W 3 ${WAN_IP} 2>&1" || true)
    if ! printf '%s' "$_ping_out" | grep -qE '0% packet loss|0\.0% packet loss'; then
        _loss=$(printf '%s' "$_ping_out" | \
            grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || true)
        _issue126="${_issue126:+${_issue126}; }LAN→WAN ping ${_loss:-no response}"
    fi
    if ! _p31_node_alive; then
        _issue126="${_issue126:+${_issue126}; }node is not responsive after data-plane check"
    fi

    if [[ -z "$_issue126" ]]; then
        pass "Step 126: data plane healthy after resize" \
            "canonical users 1/2 remain in Data phase; ${WAN_IP} reachable with 0% packet loss"
    else
        fail "Step 126: data plane healthy after resize" "$_issue126"
    fi

    _cleanup_phase31_subscriber_scale
    return 0
}
