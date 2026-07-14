#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 18 — DNS cache behaviour and upstream failover (Steps 70-74)
#
# Runtime upstream DNS is negotiated through PPP IPCP; the obsolete HSI DNS
# schema fields have been removed. This phase temporarily restarts dpdk-bras
# with WAN_IP as its primary DNS.
# ---------------------------------------------------------------------------

_P18_DOMAIN="cachetest.fastrg.internal"
_P18_ANSWER="192.0.2.111"
_P18_FAILOVER_DOMAIN="failovertest.fastrg.internal"
_P18_FAILOVER_ANSWER="192.0.2.222"
_P18_FAILOVER_TIMEOUT_SECS=60
_P18_SECONDARY_DNS="${WAN_IP%.*}.12"
_P18_RESPONDER_REMOTE="/tmp/fastrg_dns_responder.py"
_P18_RESPONDER_LOG="/tmp/fastrg_dns_responder.log"
_P18_RESPONDER_OUT="/tmp/fastrg_dns_responder.out"
_P18_SECONDARY_RESPONDER_LOG="/tmp/fastrg_secondary_dns_responder.log"
_P18_SECONDARY_RESPONDER_OUT="/tmp/fastrg_secondary_dns_responder.out"
_P18_RESPONDER_PID=""
_P18_SECONDARY_RESPONDER_PID=""
_P18_SECONDARY_ALIAS_ADDED=0
_P18_BRAS_OVERRIDDEN=0

_p18_stop_responder() {
    local _i
    if [[ -n "${_P18_RESPONDER_PID:-}" ]]; then
        ssh_wan "kill -TERM ${_P18_RESPONDER_PID} 2>/dev/null || true" >/dev/null 2>&1 || true
        for _i in $(seq 1 10); do
            if ! ssh_wan "kill -0 ${_P18_RESPONDER_PID} 2>/dev/null" >/dev/null 2>&1; then
                info "Cleanup(phase18): verified DNS responder pid ${_P18_RESPONDER_PID} stopped."
                _P18_RESPONDER_PID=""
                return 0
            fi
            sleep 1
        done
        warn "Cleanup(phase18): DNS responder pid ${_P18_RESPONDER_PID} did not exit after SIGTERM."
        return 1
    fi
    return 0
}

_p18_start_responder() {
    local _ttl="$1"
    _p18_stop_responder || true
    ssh_wan ": > ${_P18_RESPONDER_LOG}; : > ${_P18_RESPONDER_OUT}" >/dev/null 2>&1 || true
    _P18_RESPONDER_PID=$(ssh_wan \
        "nohup python3 ${_P18_RESPONDER_REMOTE} --bind ${WAN_IP} --answer ${_P18_ANSWER} --ttl ${_ttl} --log ${_P18_RESPONDER_LOG} >${_P18_RESPONDER_OUT} 2>&1 & echo \$!" \
        2>/dev/null | tr -d '[:space:]' || true)
    [[ "${_P18_RESPONDER_PID:-}" =~ ^[0-9]+$ ]] || return 1
    sleep 1
    ssh_wan "kill -0 ${_P18_RESPONDER_PID}" >/dev/null 2>&1
}

_p18_stop_secondary_responder() {
    local _i
    if [[ -n "${_P18_SECONDARY_RESPONDER_PID:-}" ]]; then
        ssh_wan "kill -TERM ${_P18_SECONDARY_RESPONDER_PID} 2>/dev/null || true" >/dev/null 2>&1 || true
        for _i in $(seq 1 10); do
            if ! ssh_wan "kill -0 ${_P18_SECONDARY_RESPONDER_PID} 2>/dev/null" >/dev/null 2>&1; then
                info "Cleanup(phase18): verified secondary DNS responder pid ${_P18_SECONDARY_RESPONDER_PID} stopped."
                _P18_SECONDARY_RESPONDER_PID=""
                return 0
            fi
            sleep 1
        done
        warn "Cleanup(phase18): secondary DNS responder pid ${_P18_SECONDARY_RESPONDER_PID} did not exit after SIGTERM."
        return 1
    fi
    return 0
}

_p18_start_secondary_responder() {
    _p18_stop_secondary_responder || true
    ssh_wan ": > ${_P18_SECONDARY_RESPONDER_LOG}; : > ${_P18_SECONDARY_RESPONDER_OUT}" \
        >/dev/null 2>&1 || true
    _P18_SECONDARY_RESPONDER_PID=$(ssh_wan \
        "nohup python3 ${_P18_RESPONDER_REMOTE} --bind ${_P18_SECONDARY_DNS} --answer ${_P18_FAILOVER_ANSWER} --ttl 120 --log ${_P18_SECONDARY_RESPONDER_LOG} >${_P18_SECONDARY_RESPONDER_OUT} 2>&1 & echo \$!" \
        2>/dev/null | tr -d '[:space:]' || true)
    [[ "${_P18_SECONDARY_RESPONDER_PID:-}" =~ ^[0-9]+$ ]] || return 1
    sleep 1
    ssh_wan "kill -0 ${_P18_SECONDARY_RESPONDER_PID}" >/dev/null 2>&1
}

_p18_add_secondary_dns_alias() {
    if ssh_wan "ip -4 address show dev ${WAN_NIC} | grep -Fq 'inet ${_P18_SECONDARY_DNS}/'" \
        >/dev/null 2>&1; then
        return 0
    fi
    if ssh_wan "ip address add ${_P18_SECONDARY_DNS}/24 dev ${WAN_NIC}" >/dev/null 2>&1; then
        _P18_SECONDARY_ALIAS_ADDED=1
        return 0
    fi
    return 1
}

_p18_query_count() {
    ssh_wan "wc -l < ${_P18_RESPONDER_LOG} 2>/dev/null || echo 0" 2>/dev/null | tr -d '[:space:]' || true
}

_p18_start_bras() {
    local _primary_dns="$1"
    local _secondary_dns="${2:-8.8.8.8}"
    local _i
    ssh_bras "cd /root/dpdk-bras && exec ./dpdk-bras -l 0-7 -n 4 -- --pri-dns ${_primary_dns} --sec-dns ${_secondary_dns} --drop-pcap ./test.pcap --vlans 3,5 >/var/log/dpdk-bras.log 2>&1" \
        </dev/null >/dev/null 2>&1 &
    _P18_BRAS_SSH_PID=$!
    for _i in $(seq 1 12); do
        sleep 2
        if ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
            return 0
        fi
    done
    return 1
}

_p18_restart_bras() {
    local _primary_dns="$1"
    local _secondary_dns="${2:-8.8.8.8}"
    local _i
    ssh_bras "pkill -TERM -x dpdk-bras 2>/dev/null || true" >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        if ! ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
            break
        fi
        sleep 1
    done
    if ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1" 2>/dev/null; then
        warn "Cleanup(phase18): dpdk-bras did not exit after SIGTERM."
        return 1
    fi
    _p18_start_bras "${_primary_dns}" "${_secondary_dns}"
}

_p18_redial() {
    local _i _phase
    fastrg_grpc disconnect_hsi "${USER_ID}" >/dev/null 2>&1 || true
    for _i in $(seq 1 12); do
        sleep 2
        _phase=$(fastrg_grpc get_hsi_info | \
            jq -r ".hsi_infos[] | select(.user_id == ${USER_ID}) | .status" 2>/dev/null || true)
        [[ "$_phase" != "Data phase" ]] && break
    done
    fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true
    for _i in $(seq 1 30); do
        sleep 2
        _phase=$(fastrg_grpc get_hsi_info | \
            jq -r ".hsi_infos[] | select(.user_id == ${USER_ID}) | .status" 2>/dev/null || true)
        [[ "$_phase" == "Data phase" ]] && return 0
    done
    return 1
}

_cleanup_phase18_dns_cache() {
    local _remaining=""
    _p18_stop_responder || true
    _p18_stop_secondary_responder || true
    ssh_wan "rm -f ${_P18_RESPONDER_REMOTE} ${_P18_RESPONDER_LOG} ${_P18_RESPONDER_OUT} ${_P18_SECONDARY_RESPONDER_LOG} ${_P18_SECONDARY_RESPONDER_OUT}" \
        >/dev/null 2>&1 || true

    if [[ "${_P18_SECONDARY_ALIAS_ADDED:-0}" -eq 1 ]]; then
        ssh_wan "ip address del ${_P18_SECONDARY_DNS}/24 dev ${WAN_NIC} 2>/dev/null || true" \
            >/dev/null 2>&1 || true
        _P18_SECONDARY_ALIAS_ADDED=0
    fi

    if [[ "${_P18_BRAS_OVERRIDDEN:-0}" -eq 1 ]]; then
        info "Cleanup(phase18): restoring dpdk-bras primary DNS to 192.168.10.1..."
        if _p18_restart_bras "192.168.10.1"; then
            _P18_BRAS_OVERRIDDEN=0
            _p18_redial || warn "Cleanup(phase18): USER_ID=${USER_ID} did not return to Data phase."
        else
            warn "Cleanup(phase18): failed to restore dpdk-bras."
        fi
    fi

    fastrg_grpc flush_dns_cache "${USER_ID}" >/dev/null 2>&1 || true
    _remaining=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
    if [[ -n "$_remaining" ]]; then
        info "Cleanup(phase18): verified canonical HSI config for user ${USER_ID} remains present."
    else
        warn "Cleanup(phase18): canonical HSI config for user ${USER_ID} is missing."
    fi
    return 0
}

phase18_dns_cache() {
    local _gw _flush _dig _cache _entry _count_before _count_after
    local _ttl_first_count _ttl_second_count _static _failover_start _failover_elapsed

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 18 — DNS Cache Behaviour + Failover (Steps 70-74)"
    bold "═══════════════════════════════════════════════════════"

    _gw=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null | \
        jq -r '.config.dhcp_gateway // empty' 2>/dev/null || true)
    if [[ -z "$_gw" ]]; then
        fail "Step 70: DNS cache fill + GetDnsCache" "subscriber gateway unavailable"
        fail "Step 71: DNS cache hit with upstream stopped" "phase setup failed"
        fail "Step 72: DNS TTL expiry refetches upstream" "phase setup failed"
        fail "Step 73: DNS upstream failover" "phase setup failed"
        fail "Step 74: DNS phase cleanup + static recovery" "phase setup failed"
        return
    fi

    if ! scp $SSH_OPTS "${GRPC_CLIENT_DIR}/dns_responder.py" \
        "root@${WAN_HOST}:${_P18_RESPONDER_REMOTE}" >/dev/null 2>&1 || \
       ! _p18_start_responder 120; then
        fail "Step 70: DNS cache fill + GetDnsCache" "could not start WAN DNS responder"
        fail "Step 71: DNS cache hit with upstream stopped" "phase setup failed"
        fail "Step 72: DNS TTL expiry refetches upstream" "phase setup failed"
        fail "Step 73: DNS upstream failover" "phase setup failed"
        fail "Step 74: DNS phase cleanup + static recovery" "phase setup failed"
        _cleanup_phase18_dns_cache
        return
    fi

    _P18_BRAS_OVERRIDDEN=1
    if ! _p18_restart_bras "${WAN_IP}" "${_P18_SECONDARY_DNS}" || ! _p18_redial; then
        fail "Step 70: DNS cache fill + GetDnsCache" "could not negotiate WAN responder as IPCP primary DNS"
        fail "Step 71: DNS cache hit with upstream stopped" "phase setup failed"
        fail "Step 72: DNS TTL expiry refetches upstream" "phase setup failed"
        fail "Step 73: DNS upstream failover" "phase setup failed"
        fail "Step 74: DNS phase cleanup + static recovery" "phase setup failed"
        _cleanup_phase18_dns_cache
        return
    fi

    _flush=$(fastrg_grpc flush_dns_cache "${USER_ID}" || true)
    _dig=$(ssh_lan "timeout 10 dig @${_gw} +time=3 +tries=1 +short ${_P18_DOMAIN} A" 2>/dev/null || true)
    _cache=$(fastrg_grpc get_dns_cache "${USER_ID}" || true)
    _entry=$(printf '%s' "$_cache" | \
        jq -r ".entries[]? | select(.domain == \"${_P18_DOMAIN}\" and .qtype == 1) | .domain" \
        2>/dev/null || true)
    if [[ "$_dig" == "${_P18_ANSWER}" ]] && [[ "$_entry" == "${_P18_DOMAIN}" ]] && \
       [[ "$(printf '%s' "$_flush" | jq -r '.status // empty' 2>/dev/null || true)" == "ok" ]]; then
        pass "Step 70: DNS cache fill + GetDnsCache" "answer=${_P18_ANSWER}; cache entry present"
    else
        fail "Step 70: DNS cache fill + GetDnsCache" \
            "dig='${_dig:-empty}' cache_entry='${_entry:-missing}' flush='${_flush:-empty}'"
    fi

    _count_before=$(_p18_query_count)
    _p18_stop_responder || true
    _dig=$(ssh_lan "timeout 10 dig @${_gw} +time=3 +tries=1 +short ${_P18_DOMAIN} A" 2>/dev/null || true)
    _count_after=$(_p18_query_count)
    if [[ "$_dig" == "${_P18_ANSWER}" ]] && [[ "$_count_after" == "$_count_before" ]]; then
        pass "Step 71: DNS cache hit with upstream stopped" \
            "cached answer returned; upstream count stayed ${_count_before}"
    else
        fail "Step 71: DNS cache hit with upstream stopped" \
            "dig='${_dig:-empty}' upstream_count=${_count_before:-?}->${_count_after:-?}"
    fi

    if ! _p18_start_responder 5; then
        fail "Step 72: DNS TTL expiry refetches upstream" "could not restart WAN DNS responder"
    else
        fastrg_grpc flush_dns_cache "${USER_ID}" >/dev/null 2>&1 || true
        _dig=$(ssh_lan "timeout 10 dig @${_gw} +time=3 +tries=1 +short ${_P18_DOMAIN} A" 2>/dev/null || true)
        _ttl_first_count=$(_p18_query_count)
        sleep 7
        _dig=$(ssh_lan "timeout 10 dig @${_gw} +time=3 +tries=1 +short ${_P18_DOMAIN} A" 2>/dev/null || true)
        _ttl_second_count=$(_p18_query_count)
        if [[ "$_dig" == "${_P18_ANSWER}" ]] && \
           [[ "${_ttl_second_count:-0}" -eq $(( ${_ttl_first_count:-0} + 1 )) ]]; then
            pass "Step 72: DNS TTL expiry refetches upstream" \
                "upstream count ${_ttl_first_count}->${_ttl_second_count} after 7s"
        else
            fail "Step 72: DNS TTL expiry refetches upstream" \
                "dig='${_dig:-empty}' upstream_count=${_ttl_first_count:-?}->${_ttl_second_count:-?}"
        fi
    fi

    fastrg_grpc flush_dns_cache "${USER_ID}" >/dev/null 2>&1 || true
    if ! _p18_add_secondary_dns_alias || ! _p18_start_secondary_responder; then
        fail "Step 73: DNS upstream failover" \
            "could not configure secondary DNS responder at ${_P18_SECONDARY_DNS}"
    else
        _p18_stop_responder || true
        _dig=""
        _failover_start=$(date +%s)
        while true; do
            _dig=$(ssh_lan \
                "timeout 5 dig @${_gw} +time=3 +tries=1 +short ${_P18_FAILOVER_DOMAIN} A" \
                2>/dev/null || true)
            _failover_elapsed=$(( $(date +%s) - _failover_start ))
            if [[ "$_dig" == "${_P18_FAILOVER_ANSWER}" ]] && \
               [[ "$_failover_elapsed" -ge "${_P18_FAILOVER_TIMEOUT_SECS}" ]]; then
                break
            fi
            [[ "$_failover_elapsed" -ge $((_P18_FAILOVER_TIMEOUT_SECS + 15)) ]] && break
            sleep 1
        done
        _failover_elapsed=$(( $(date +%s) - _failover_start ))
        if [[ "$_dig" == "${_P18_FAILOVER_ANSWER}" ]] && \
           [[ "$_failover_elapsed" -ge "${_P18_FAILOVER_TIMEOUT_SECS}" ]]; then
            pass "Step 73: DNS upstream failover" \
                "secondary=${_P18_SECONDARY_DNS} answer=${_P18_FAILOVER_ANSWER} after ${_failover_elapsed}s"
        else
            fail "Step 73: DNS upstream failover" \
                "dig='${_dig:-empty}' elapsed=${_failover_elapsed}s secondary=${_P18_SECONDARY_DNS}"
        fi
    fi

    _cleanup_phase18_dns_cache
    _static=$(ssh_lan "timeout 10 dig @${_gw} +time=3 +tries=1 +short www.fastrg.org A" 2>/dev/null || true)
    if [[ "$_static" == "${WAN_IP}" ]] && \
       ! ssh_wan "pgrep -f '[f]astrg_dns_responder.py' >/dev/null 2>&1" 2>/dev/null; then
        pass "Step 74: DNS phase cleanup + static recovery" \
            "static answer=${WAN_IP}; responder absent; BRAS/PPPoE restored"
    else
        fail "Step 74: DNS phase cleanup + static recovery" \
            "static_answer='${_static:-empty}'; responder or recovery state unexpected"
    fi
}
