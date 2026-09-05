#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 40 — Node CLI config writes while etcd is unreachable (Steps 176-184)
#
# Config writes normally travel through the controller, so the node handlers'
# standalone path — accepted on the node's own gRPC because etcd is out of
# reach, applied by the control thread, recorded in the snapshot as a dirty
# offline edit — is only covered for ApplyConfig and RemoveConfig. This phase
# blocks node->etcd and drives the remaining seven writes straight at the node
# with grpcurl: SetIpv6, SetTcpConntrack, SetDnsProxy, SetSnatConfig,
# RemoveSnatConfig, AddDnsRecord and RemoveDnsRecord.
#
# The toggles are put back to their fixture values while the block is still up.
# The SNAT rule and the DNS record are re-added at the end of their steps and
# left in place on purpose: an entry whose content matches etcd is cleared
# without ever being reported, so without a difference nothing would prove the
# edits had been queued. Step 183 waits for the report, then takes both back
# out through the controller.
#
# Step 184 then restarts the node with the block up again: with etcd out of
# reach the local snapshot is the only place the subscriber's static DNS
# records can come from, so it is the one path that proves they are applied at
# boot rather than when a session comes up.
#
# Everything here is done for the primary subscriber only.
# ---------------------------------------------------------------------------

# grpcurl runs on the node against the node's own proto copy: the write has to
# reach the node over a path the controller is not part of.
_P40_PROTO_DIR="/root/fastrg/fastrg-node/northbound/grpc"

# Values this phase owns; nothing else in the suite uses them.
_P40_EPORT=39999
_P40_IPORT=22
_P40_DNS_DOMAIN="offline.fastrg.test"
# Only ever asked to be removed, and never present: the guard probe.
_P40_PROBE_DOMAIN="offline-probe.fastrg.test"
_P40_DNS_IP="192.0.2.77"
_P40_DNS_TTL=60

# Bench state to undo, and what the phase read before it started editing.
# Assigned only when unset: a drill puts the real functions back by re-reading
# this file, and cleanup runs right after that with edits still outstanding.
: "${_P40_SNAT_LEFT:=0}"
: "${_P40_DNS_RECORD_LEFT:=0}"
: "${_P40_IPV6_LEFT:=0}"
: "${_P40_CONNTRACK_OFF:=0}"
: "${_P40_DNS_PROXY_OFF:=0}"
: "${_P40_DIP:=}"
: "${_P40_GW:=}"
: "${_P40_BROKERS:=}"
: "${_P40_KAFKA_BASELINE:=-1}"
# 1 while Step 184 has the node down and has not seen it come back.
: "${_P40_NODE_STOPPED:=0}"

# Filled in by _p40_dig_check: the verdict and the raw answer of the last dig.
_P40_DIG_VERDICT=""
_P40_DIG_RAW=""

# How long a reconnected node may take to report its offline edits. It only
# notices etcd is back on a watchdog tick and then retries the connection with
# a backoff; 150s has been measured, so the budget is twice that.
_P40_DIRTY_WAIT_SECS=300

# Filled in by _p40_wait_dirty_clear: the last two readings and the wait.
_P40_DIRTY_HSI=""
_P40_DIRTY_DNS=""
_P40_DIRTY_WAITED=0

# What one IPv6 toggle did to a session. AFTER_STATE is the "<session_id>
# <ipv6_pd_prefix> <ipv6_addr>" reading taken after the toggle, WANT is "on"
# or "off".
#
# Prints pass | same_session | no_prefix | no_addr | ipv6_present | err.
# IPV6CP only runs while a session comes up, so an unchanged session id is the
# failure this has to name. "err" covers a reading that could not be taken:
# cleared fields are a pass condition for want=off, so a dead RPC must never
# be able to produce one.
e2e_session_redial_verdict() {
    local _after="${1:-}" _sid_before="${2:-}" _want="${3:-}"
    local _sid_after="" _prefix="" _addr=""

    read -r _sid_after _prefix _addr <<< "$_after" || true
    # Both ids have to be real numbers: a reading that failed and a session
    # the node no longer reports an id for both arrive as text here, and
    # either one would otherwise pass for "the session changed".
    if ! [[ "$_sid_before" =~ ^[0-9]+$ ]] || ! [[ "$_sid_after" =~ ^[0-9]+$ ]]; then
        printf 'err'
        return 1
    fi
    if [[ "$_want" != "on" && "$_want" != "off" ]]; then
        printf 'err'
        return 1
    fi
    if [[ "$_sid_after" == "$_sid_before" ]]; then
        printf 'same_session'
        return 1
    fi
    if [[ "$_want" == "on" ]]; then
        if [[ "$_prefix" != */56 ]]; then
            printf 'no_prefix'
            return 1
        fi
        if [[ "$_addr" != fe80:* ]]; then
            printf 'no_addr'
            return 1
        fi
    elif [[ -n "$_prefix" || -n "$_addr" ]]; then
        printf 'ipv6_present'
        return 1
    fi
    printf 'pass'
    return 0
}

local_validation_register session_redial_verdict e2e_session_redial_verdict \
    redial_verdict_enabled \
    redial_verdict_disabled \
    redial_verdict_same_session \
    redial_verdict_no_prefix \
    redial_verdict_no_addr \
    redial_verdict_still_has_ipv6 \
    redial_verdict_unreadable \
    redial_verdict_session_absent \
    redial_verdict_no_session_before

# One config write straight at the node. Wrapped so the drills below can take
# a single RPC away without touching the steps.
_p40_rpc() {
    local _rpc="$1" _json="$2"

    ssh_node "grpcurl -plaintext -import-path ${_P40_PROTO_DIR} -proto fastrg_node.proto \
        -d '${_json}' 127.0.0.1:${FASTRG_GRPC_PORT} fastrgnodeservice.FastrgService/${_rpc} 2>&1" \
        2>/dev/null || true
}

# Whether one _p40_rpc answer is a reply rather than a refusal: grpcurl prints
# the reply JSON on success and an "ERROR:" block on any status code.
_p40_rpc_ok() {
    local _out="$1"

    printf '%s' "$_out" | grep -q 'ERROR:' && return 1
    printf '%s' "$_out" | grep -q '"status"'
}

# Drills: leave one RPC out and let the real one through for everything else,
# so the step that owns that write is the one that has to notice.
_p40_skip_rpc() {
    sabotage_copy_function _p40_rpc _p40_rpc_real
    sabotage_override_function _p40_rpc \
        "[[ \"\$1\" == $1 ]] && { printf '{}'; return 0; }
        _p40_rpc_real \"\$@\""
}

_p40_inject_ipv6_toggle_skipped()      { _p40_skip_rpc SetIpv6; }
_p40_inject_conntrack_toggle_skipped() { _p40_skip_rpc SetTcpConntrack; }
_p40_inject_dns_proxy_toggle_skipped() { _p40_skip_rpc SetDnsProxy; }
_p40_inject_snat_add_skipped()         { _p40_skip_rpc SetSnatConfig; }
_p40_inject_dns_record_add_skipped()   { _p40_skip_rpc AddDnsRecord; }

# Put the real RPC back before undoing the phase's edits: cleanup writes
# through the same wrapper.
_p40_cleanup_drill() {
    restore_phase_functions phase40_offline_cli_writes.sh
    _cleanup_phase40_offline_cli_writes
}

case_validation_register offline_ipv6_toggle_skipped phase40_offline_cli_writes \
    _p40_inject_ipv6_toggle_skipped _p40_cleanup_drill 'Step 177:'
case_validation_register offline_conntrack_toggle_skipped phase40_offline_cli_writes \
    _p40_inject_conntrack_toggle_skipped _p40_cleanup_drill 'Step 179:'
case_validation_register offline_dns_proxy_toggle_skipped phase40_offline_cli_writes \
    _p40_inject_dns_proxy_toggle_skipped _p40_cleanup_drill 'Step 180:'
case_validation_register offline_snat_add_skipped phase40_offline_cli_writes \
    _p40_inject_snat_add_skipped _p40_cleanup_drill 'Step 181:'
case_validation_register offline_dns_record_add_skipped phase40_offline_cli_writes \
    _p40_inject_dns_record_add_skipped _p40_cleanup_drill 'Step 182:'

# ---------------------------------------------------------------------------
# Node-side readings. All of them go straight to the node, which is the only
# side that can answer while etcd is unreachable.
# ---------------------------------------------------------------------------

# One HSI field of a subscriber; "err" when the RPC itself failed, so a read
# that never happened cannot read as a cleared field.
_p40_hsi_field() {
    local _uid="$1" _field="$2" _out="" _rc=0

    _out=$(fastrg_grpc get_hsi_info) || _rc=$?
    if [[ "$_rc" -ne 0 || -z "$_out" ]]; then
        printf 'err'
        return 0
    fi
    printf '%s' "$_out" | \
        jq -r ".hsi_infos[]? | select(.user_id == ${_uid}) | .${_field} // empty" \
        2>/dev/null || true
}

# The subscriber's IPv6 state as one line: "<session_id> <prefix> <addr>",
# which is what e2e_session_redial_verdict reads. One read for all three, so
# a reconnect landing between them cannot mix an old prefix into a new
# session. "err" when the RPC answered nothing at all.
_p40_ipv6_state() {
    local _uid="$1" _out=""

    _out=$(fastrg_grpc get_hsi_info)
    if [[ -z "$_out" ]]; then
        printf 'err'
        return 0
    fi
    printf '%s' "$_out" | jq -r ".hsi_infos[]? | select(.user_id == ${_uid})
        | \"\(.session_id) \(.ipv6_pd_prefix // \"\") \(.ipv6_addr // \"\")\"" \
        2>/dev/null || true
}

_p40_user_phase() {
    _p40_hsi_field "$1" status
}

# Whether a fastrg process is alive on the node.
_p40_node_running() {
    ssh_node "pgrep -x fastrg >/dev/null 2>&1" 2>/dev/null
}

# Step 184 takes the node down on purpose. If it never came back, nothing else
# in this phase's cleanup — nor any later phase — can talk to the node, so put
# one back before anything else runs.
_p40_restart_node_if_down() {
    local _i _running=0

    [[ "${_P40_NODE_STOPPED:-0}" -eq 1 ]] || return 0
    if _p40_node_running; then
        _P40_NODE_STOPPED=0
        return 0
    fi

    warn "Cleanup(phase40): the cold start left no fastrg running; retrying startup best-effort."
    e2e_start_node >/dev/null 2>&1 || true
    _FASTRG_STARTED_BY_SCRIPT=1
    for _i in $(seq 1 15); do
        if _p40_node_running; then
            _running=1
            break
        fi
        sleep 1
    done
    if [[ $_running -eq 1 ]]; then
        _P40_NODE_STOPPED=0
        info "Cleanup(phase40): fastrg startup retry launched successfully."
    else
        warn "Cleanup(phase40): fastrg startup retry did not launch a process."
    fi
    return 0
}

# One entry of the node's persisted snapshot ("" when it holds no such key).
_p40_snapshot_entry() {
    local _key="$1"

    ssh_node "cat /etc/fastrg/config_snapshot.json 2>/dev/null" 2>/dev/null | \
        jq -r ".entries[]? | select(.key == \"${_key}\")" 2>/dev/null || true
}

# One config field of an HSI snapshot entry. The entry keeps its value as a
# JSON string, so it takes a second decode.
_p40_snapshot_hsi_field() {
    local _uid="$1" _field="$2"

    _p40_snapshot_entry "hsi/${_uid}" | jq -r '.value // ""' 2>/dev/null | \
        jq -r ".config.${_field}" 2>/dev/null || true
}

# Whether a snapshot entry still carries an unreported offline edit. Prints
# true/false, or "err" when the entry is not there to be read.
_p40_snapshot_dirty() {
    local _entry

    _entry=$(_p40_snapshot_entry "$1")
    [[ -n "$_entry" ]] || { printf 'err'; return 0; }
    # No `// empty` here: jq's `//` treats false as absent, and false is the
    # answer being waited for.
    printf '%s' "$_entry" | jq -r '.dirty' 2>/dev/null || true
}

# Wait until neither snapshot entry still carries an unreported offline edit.
# Returns non-zero when the budget ran out; the readings and the elapsed wait
# come back in _P40_DIRTY_HSI / _P40_DIRTY_DNS / _P40_DIRTY_WAITED.
#
# The wait ends on anything that is not "true": an entry the node no longer
# holds has nothing left to report, and the steps assert on the readings
# themselves, so an unreadable entry is judged there rather than waited out.
_p40_wait_dirty_clear() {
    _P40_DIRTY_WAITED=0
    while true; do
        _P40_DIRTY_HSI=$(_p40_snapshot_dirty "hsi/${USER_ID}")
        _P40_DIRTY_DNS=$(_p40_snapshot_dirty "dns/${USER_ID}")
        if [[ "$_P40_DIRTY_HSI" != "true" && "$_P40_DIRTY_DNS" != "true" ]]; then
            return 0
        fi
        [[ "$_P40_DIRTY_WAITED" -lt "$_P40_DIRTY_WAIT_SECS" ]] || return 1
        sleep 5
        _P40_DIRTY_WAITED=$(( _P40_DIRTY_WAITED + 5 ))
    done
}

# Static DNS records the node holds for a subscriber, one domain per line.
_p40_dns_domains() {
    fastrg_grpc get_dns_static "$1" | jq -r '.entries[]?.domain' 2>/dev/null || true
}

# Port forward entries the node holds for a subscriber, "<eport> <dip> <iport>"
# per line — read out of the same table the DNAT path looks the rule up in.
_p40_port_fwd_rows() {
    fastrg_grpc get_port_fwd_info "$1" | \
        jq -r '.entries[]? | "\(.eport) \(.dip) \(.iport)"' 2>/dev/null || true
}

# Read back the two leftovers this phase creates and clear their flags only
# for the ones that are really gone. A removal that was accepted and then
# undone (by a late offline-edit report, say) leaves the entry behind, and the
# next phase would meet it as a dirty fixture with no idea where it came from.
_p40_verify_residue_gone() {
    local _rows="" _domains=""

    if [[ "${_P40_SNAT_LEFT:-0}" -eq 1 ]]; then
        _rows=$(_p40_port_fwd_rows "${USER_ID}")
        if printf '%s\n' "$_rows" | grep -q "^${_P40_EPORT} "; then
            warn "Phase 40: port forward ${_P40_EPORT} is still on the node (rows: ${_rows//$'\n'/, })"
        else
            _P40_SNAT_LEFT=0
        fi
    fi
    if [[ "${_P40_DNS_RECORD_LEFT:-0}" -eq 1 ]]; then
        _domains=$(_p40_dns_domains "${USER_ID}")
        if printf '%s\n' "$_domains" | grep -qxF "${_P40_DNS_DOMAIN}"; then
            warn "Phase 40: ${_P40_DNS_DOMAIN} is still on the node (records: ${_domains//$'\n'/, })"
        else
            _P40_DNS_RECORD_LEFT=0
        fi
    fi
    return 0
}

# One dig at the node's DNS proxy from the LAN host.
_p40_dig() {
    ssh_lan "timeout 10 dig @${_P40_GW} +time=3 +tries=1 +short $1 2>&1" 2>/dev/null || true
}

# Dig for DOMAIN and judge the answer against WANT_IP. Results come back in
# globals rather than on stdout so the raw answer survives for the failure
# message: _P40_DIG_VERDICT is what the predicate said, _P40_DIG_RAW the first
# 160 characters of what the proxy actually answered, on one line.
_p40_dig_check() {
    local _domain="$1" _want_ip="$2" _raw=""

    _raw=$(_p40_dig "$_domain")
    _P40_DIG_RAW=$(printf '%s' "$_raw" | tr '\n' '|' | cut -c 1-160)
    _P40_DIG_VERDICT=$(e2e_dns_answer_verdict "$_raw" "$_want_ip" || true)
    info "  dig @${_P40_GW} ${_domain} -> ${_P40_DIG_VERDICT}; answer: '${_P40_DIG_RAW}'"
}

# Everything on the topic since the baseline offset, as greppable text.
_p40_kafka_since_baseline() {
    ssh_node "timeout 20 kcat -b ${_P40_BROKERS} -t fastrg.node.events -C \
        -o $(( _P40_KAFKA_BASELINE + 1 )) -e -q 2>/dev/null | strings" 2>/dev/null || true
}

# Idempotent: runs at the end of the phase, from the drill cleanups and from
# the cleanup_fastrg EXIT trap. While the block is still up the node's own
# gRPC is the only way in; once it is lifted the node is back under the
# controller and only the controller may write.
_cleanup_phase40_offline_cli_writes() {
    local _flag=""

    _p40_restart_node_if_down
    if e2e_node_etcd_blocked; then
        [[ "${_P40_IPV6_LEFT:-0}" -eq 1 ]] && \
            _p40_rpc SetIpv6 "{\"user_id\":${USER_ID},\"enable\":false}" >/dev/null 2>&1
        [[ "${_P40_CONNTRACK_OFF:-0}" -eq 1 ]] && \
            _p40_rpc SetTcpConntrack "{\"user_id\":${USER_ID},\"enable\":true}" >/dev/null 2>&1
        [[ "${_P40_DNS_PROXY_OFF:-0}" -eq 1 ]] && \
            _p40_rpc SetDnsProxy "{\"user_id\":${USER_ID},\"enable\":true}" >/dev/null 2>&1
        [[ "${_P40_SNAT_LEFT:-0}" -eq 1 ]] && \
            _p40_rpc RemoveSnatConfig "{\"user_id\":${USER_ID},\"eport\":${_P40_EPORT}}" >/dev/null 2>&1
        [[ "${_P40_DNS_RECORD_LEFT:-0}" -eq 1 ]] && \
            _p40_rpc RemoveDnsRecord "{\"user_id\":${USER_ID},\"domain\":\"${_P40_DNS_DOMAIN}\"}" >/dev/null 2>&1
        info "Cleanup(phase40): restoring node->etcd connectivity..."
        e2e_unblock_node_etcd
    else
        # Wait for the node to report its offline edits before removing
        # anything through the controller: a removal that lands first is
        # undone by the report, which then writes the entry into etcd. Only
        # worth waiting for when there is something to remove.
        if [[ "${_P40_SNAT_LEFT:-0}" -eq 1 || "${_P40_DNS_RECORD_LEFT:-0}" -eq 1 ]]; then
            if ! _p40_wait_dirty_clear; then
                warn "Cleanup(phase40): snapshot still dirty (hsi='${_P40_DIRTY_HSI}' dns='${_P40_DIRTY_DNS}') after ${_P40_DIRTY_WAITED}s; removing anyway, best effort."
            fi
        fi
        [[ "${_P40_IPV6_LEFT:-0}" -eq 1 ]] && \
            fastrg_grpc set_ipv6 "${USER_ID}" false >/dev/null 2>&1
        [[ "${_P40_CONNTRACK_OFF:-0}" -eq 1 ]] && \
            fastrg_grpc set_tcp_conntrack "${USER_ID}" true >/dev/null 2>&1
        [[ "${_P40_DNS_PROXY_OFF:-0}" -eq 1 ]] && \
            fastrg_grpc set_dns_proxy "${USER_ID}" true >/dev/null 2>&1
        [[ "${_P40_SNAT_LEFT:-0}" -eq 1 ]] && \
            fastrg_grpc remove_snat_config "${USER_ID}" "${_P40_EPORT}" >/dev/null 2>&1
        [[ "${_P40_DNS_RECORD_LEFT:-0}" -eq 1 ]] && \
            fastrg_grpc remove_dns_record "${USER_ID}" "${_P40_DNS_DOMAIN}" >/dev/null 2>&1
    fi
    _P40_IPV6_LEFT=0
    _P40_CONNTRACK_OFF=0
    _P40_DNS_PROXY_OFF=0
    # The two leftovers keep their flags until the node says they are gone.
    _p40_verify_residue_gone

    # Say so when the fixture did not come back: the next phase would blame
    # its own steps for it. An absent tcp_conntrack_enable/dns_proxy_enable
    # means true to the node, so the flags are compared the same way it reads
    # them; `//` is no use here because it treats a stored false as absent.
    if [[ -n "${NODE_UUID:-}" ]]; then
        _flag=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null | \
            jq -r '[.config.ipv6_enable == true, .config.tcp_conntrack_enable != false, .config.dns_proxy_enable != false] | join(",")' \
            2>/dev/null || true)
        [[ "$_flag" == "false,true,true" ]] || \
            warn "Cleanup(phase40): etcd HSI flags for user ${USER_ID} are '${_flag:-unreadable}', want 'false,true,true'"
    fi
    return 0
}

phase40_offline_cli_writes() {
    local _i _issue="" _out="" _verdict="" _state="" _sid_before=""
    local _phase="" _hsi="" _rows="" _domains="" _dirty_hsi="" _dirty_dns=""
    local _waited=0 _released=0 _marker="" _missing="" _kafka=""
    local _etcd_hsi="" _etcd_dns="" _conntrack="" _dig_off="" _dig_on=""
    local _stopped=0 _recovered=0 _started_at=0 _elapsed=0 _reported=0

    bold "═══════════════════════════════════════════════════════"
    bold " Phase 40 — Offline node CLI config writes (Steps 176-184)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Preconditions. Every one of them is a reason the phase could not run
    # at all, and a phase that cannot run is a failure, not a skip.
    # ------------------------------------------------------------------
    for _i in grpcurl kcat iptables; do
        if ! ssh_node "command -v ${_i} >/dev/null 2>&1"; then
            fail "Step 176: node accepts direct config writes while etcd is unreachable" \
                "${_i} is not installed on the node"
            return
        fi
    done

    _P40_BROKERS=$(ssh_node "grep 'KafkaBrokers' /etc/fastrg/config.cfg 2>/dev/null" 2>/dev/null | \
        awk -F'"' '{print $2}' || true)
    if [[ -z "$_P40_BROKERS" ]]; then
        fail "Step 176: node accepts direct config writes while etcd is unreachable" \
            "cannot read KafkaBrokers from the node config"
        return
    fi

    # etcd is read before the block goes up: every etcdctl call runs on the
    # node and would time out behind it.
    _etcd_hsi=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
    _P40_GW=$(printf '%s' "$_etcd_hsi" | jq -r '.config.dhcp_gateway // empty' 2>/dev/null || true)
    _P40_DIP="$_P40_GW"
    if [[ -z "$_P40_GW" ]]; then
        fail "Step 176: node accepts direct config writes while etcd is unreachable" \
            "no dhcp_gateway in the etcd HSI fixture for user ${USER_ID}"
        return
    fi

    _phase=$(_p40_user_phase "${USER_ID}")
    if [[ "$_phase" != "Data phase" ]]; then
        fail "Step 176: node accepts direct config writes while etcd is unreachable" \
            "user ${USER_ID} is in '${_phase:-unreachable}', not Data phase"
        return
    fi
    _rows=$(_p40_port_fwd_rows "${USER_ID}")
    if printf '%s\n' "$_rows" | grep -q "^${_P40_EPORT} "; then
        fail "Step 176: node accepts direct config writes while etcd is unreachable" \
            "port forward ${_P40_EPORT} already exists before the phase touched anything: ${_rows//$'\n'/, }"
        return
    fi
    _domains=$(_p40_dns_domains "${USER_ID}")
    if printf '%s\n' "$_domains" | grep -qxF "${_P40_DNS_DOMAIN}"; then
        fail "Step 176: node accepts direct config writes while etcd is unreachable" \
            "DNS record ${_P40_DNS_DOMAIN} already exists before the phase touched anything"
        return
    fi

    # Everything the reconnect report produces lands after this offset.
    _P40_KAFKA_BASELINE=$(ssh_node "timeout 10 kcat -b ${_P40_BROKERS} -t fastrg.node.events -C -e -f '%o\n' -o -1 2>/dev/null | tail -1" 2>/dev/null || true)
    [[ "$_P40_KAFKA_BASELINE" =~ ^[0-9]+$ ]] || _P40_KAFKA_BASELINE=-1

    # ------------------------------------------------------------------
    # Step 176 — block node->etcd and wait for the SDN guard to release
    #
    # The probe removes a DNS record that is not there: it changes nothing
    # whichever way it goes, and it is the one guarded write no drill below
    # takes away, so the wait stays honest in every drill. Past the guard the
    # control thread answers NOT_FOUND, which is what says the whole path ran.
    # ------------------------------------------------------------------
    info "Step 176: blocking node->etcd and waiting for the SDN guard to release..."
    e2e_block_node_etcd || true

    for _i in $(seq 1 45); do
        sleep 2
        _out=$(_p40_rpc RemoveDnsRecord "{\"user_id\":${USER_ID},\"domain\":\"${_P40_PROBE_DOMAIN}\"}")
        if ! printf '%s' "$_out" | grep -q 'FailedPrecondition'; then
            _released=1
            _waited=$(( _i * 2 ))
            break
        fi
    done

    if [[ $_released -eq 0 ]]; then
        fail "Step 176: node accepts direct config writes while etcd is unreachable" \
            "SDN guard never released within 90s; last answer: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 200)"
        _cleanup_phase40_offline_cli_writes
        return
    fi
    if ! printf '%s' "$_out" | grep -q 'NotFound'; then
        fail "Step 176: node accepts direct config writes while etcd is unreachable" \
            "the probe was no longer refused but did not reach the control thread either: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 200)"
        _cleanup_phase40_offline_cli_writes
        return
    fi
    pass "Step 176: node accepts direct config writes while etcd is unreachable" \
        "the node took a direct write ${_waited}s after the block went up"

    # ------------------------------------------------------------------
    # Step 177 — SetIpv6 true: the node redials by itself and comes back
    #            with a delegated prefix
    # ------------------------------------------------------------------
    info "Step 177: SetIpv6(true) straight at the node; expecting a reconnected session with a prefix..."
    _issue=""
    _sid_before=$(_p40_hsi_field "${USER_ID}" session_id)
    _P40_IPV6_LEFT=1
    _out=$(_p40_rpc SetIpv6 "{\"user_id\":${USER_ID},\"enable\":true}")
    _p40_rpc_ok "$_out" || \
        _issue="SetIpv6(true) did not answer with a status: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"

    _verdict=err
    for _i in $(seq 1 45); do
        sleep 2
        [[ "$(_p40_user_phase "${USER_ID}")" == "Data phase" ]] || continue
        _state=$(_p40_ipv6_state "${USER_ID}")
        _verdict=$(e2e_session_redial_verdict "$_state" "$_sid_before" on || true)
        [[ "$_verdict" == "pass" ]] && break
    done
    [[ "$_verdict" == "pass" ]] || \
        _issue="${_issue:+${_issue}; }no IPv6 session within 90s (verdict '${_verdict}', session ${_sid_before:-none} -> '${_state:-none}')"

    if [[ -z "$_issue" ]]; then
        pass "Step 177: offline SetIpv6(true) brings IPv6 up" \
            "session ${_sid_before} -> ${_state}"
    else
        fail "Step 177: offline SetIpv6(true) brings IPv6 up" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 178 — SetIpv6 false: another reconnect, IPv6 fields cleared
    # ------------------------------------------------------------------
    info "Step 178: SetIpv6(false) straight at the node; expecting an IPv4-only session..."
    _issue=""
    _sid_before=$(_p40_hsi_field "${USER_ID}" session_id)
    _out=$(_p40_rpc SetIpv6 "{\"user_id\":${USER_ID},\"enable\":false}")
    _p40_rpc_ok "$_out" || \
        _issue="SetIpv6(false) did not answer with a status: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"

    _verdict=err
    for _i in $(seq 1 45); do
        sleep 2
        [[ "$(_p40_user_phase "${USER_ID}")" == "Data phase" ]] || continue
        _state=$(_p40_ipv6_state "${USER_ID}")
        _verdict=$(e2e_session_redial_verdict "$_state" "$_sid_before" off || true)
        [[ "$_verdict" == "pass" ]] && break
    done
    if [[ "$_verdict" == "pass" ]]; then
        _P40_IPV6_LEFT=0
    else
        _issue="${_issue:+${_issue}; }no IPv4-only session within 90s (verdict '${_verdict}', session ${_sid_before:-none} -> '${_state:-none}')"
    fi

    if [[ -z "$_issue" ]]; then
        pass "Step 178: offline SetIpv6(false) takes IPv6 back down" \
            "session ${_sid_before} -> ${_state}"
    else
        fail "Step 178: offline SetIpv6(false) takes IPv6 back down" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 179 — SetTcpConntrack off and on again
    #
    # No read RPC reports this flag, so the snapshot is what says the write
    # landed: the merged config the node persisted has to carry the new value
    # and the entry has to be dirty. Whether the data plane honours the flag
    # is phase 4.5's subject, not this one's.
    # ------------------------------------------------------------------
    info "Step 179: SetTcpConntrack(false) then (true) straight at the node..."
    _issue=""
    _P40_CONNTRACK_OFF=1
    _out=$(_p40_rpc SetTcpConntrack "{\"user_id\":${USER_ID},\"enable\":false}")
    _p40_rpc_ok "$_out" || \
        _issue="SetTcpConntrack(false) did not answer with a status: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"
    _conntrack=$(_p40_snapshot_hsi_field "${USER_ID}" tcp_conntrack_enable)
    [[ "$_conntrack" == "false" ]] || \
        _issue="${_issue:+${_issue}; }snapshot tcp_conntrack_enable='${_conntrack:-missing}' after the disable, want false"

    _out=$(_p40_rpc SetTcpConntrack "{\"user_id\":${USER_ID},\"enable\":true}")
    if _p40_rpc_ok "$_out"; then
        _P40_CONNTRACK_OFF=0
    else
        _issue="${_issue:+${_issue}; }SetTcpConntrack(true) did not answer with a status: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"
    fi
    _conntrack=$(_p40_snapshot_hsi_field "${USER_ID}" tcp_conntrack_enable)
    [[ "$_conntrack" == "true" ]] || \
        _issue="${_issue:+${_issue}; }snapshot tcp_conntrack_enable='${_conntrack:-missing}' after the enable, want true"
    _dirty_hsi=$(_p40_snapshot_dirty "hsi/${USER_ID}")
    [[ "$_dirty_hsi" == "true" ]] || \
        _issue="${_issue:+${_issue}; }snapshot entry hsi/${USER_ID} is dirty='${_dirty_hsi}', want true while etcd is unreachable"

    if [[ -z "$_issue" ]]; then
        pass "Step 179: offline SetTcpConntrack is applied and snapshotted" \
            "snapshot config followed both writes and the entry is queued as an offline edit"
    else
        fail "Step 179: offline SetTcpConntrack is applied and snapshotted" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 180 — SetDnsProxy off and on again
    #
    # "No answer" is only worth something next to an answer: the two halves
    # run the same dig, so a dig that cannot reach the node at all fails the
    # step on its second half instead of passing on the first.
    # ------------------------------------------------------------------
    info "Step 180: SetDnsProxy(false) then (true) straight at the node..."
    _issue=""
    _P40_DNS_PROXY_OFF=1
    _out=$(_p40_rpc SetDnsProxy "{\"user_id\":${USER_ID},\"enable\":false}")
    _p40_rpc_ok "$_out" || \
        _issue="SetDnsProxy(false) did not answer with a status: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"
    sleep 1
    _p40_dig_check www.fastrg.org "${WAN_IP}"
    _dig_off="$_P40_DIG_VERDICT"
    [[ "$_dig_off" == "silent" ]] || \
        _issue="${_issue:+${_issue}; }DNS verdict '${_dig_off}' with the proxy off, want silent (answer: ${_P40_DIG_RAW})"

    _out=$(_p40_rpc SetDnsProxy "{\"user_id\":${USER_ID},\"enable\":true}")
    if _p40_rpc_ok "$_out"; then
        _P40_DNS_PROXY_OFF=0
    else
        _issue="${_issue:+${_issue}; }SetDnsProxy(true) did not answer with a status: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"
    fi
    sleep 1
    _p40_dig_check www.fastrg.org "${WAN_IP}"
    _dig_on="$_P40_DIG_VERDICT"
    [[ "$_dig_on" == "answered" ]] || \
        _issue="${_issue:+${_issue}; }DNS verdict '${_dig_on}' with the proxy back on, want answered (answer: ${_P40_DIG_RAW})"

    if [[ -z "$_issue" ]]; then
        pass "Step 180: offline SetDnsProxy changes what the node answers" \
            "www.fastrg.org: ${_dig_off} with the proxy off, ${_dig_on} with it on"
    else
        fail "Step 180: offline SetDnsProxy changes what the node answers" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 181 — SetSnatConfig and RemoveSnatConfig
    #
    # GetPortFwdInfo reads the subscriber's port forward table, the one the
    # WAN->LAN path looks a rule up in. The rule is added again at the end so
    # the snapshot entry still differs from etcd when Step 183 lifts the
    # block; it never carries traffic here.
    # ------------------------------------------------------------------
    info "Step 181: SetSnatConfig/RemoveSnatConfig eport=${_P40_EPORT} straight at the node..."
    _issue=""
    _P40_SNAT_LEFT=1
    _out=$(_p40_rpc SetSnatConfig \
        "{\"user_id\":${USER_ID},\"eport\":${_P40_EPORT},\"dip\":\"${_P40_DIP}\",\"iport\":${_P40_IPORT}}")
    _p40_rpc_ok "$_out" || \
        _issue="SetSnatConfig did not answer with a status: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"
    _rows=$(_p40_port_fwd_rows "${USER_ID}")
    printf '%s\n' "$_rows" | grep -qxF "${_P40_EPORT} ${_P40_DIP} ${_P40_IPORT}" || \
        _issue="${_issue:+${_issue}; }port forward table has no '${_P40_EPORT} ${_P40_DIP} ${_P40_IPORT}' row after the add (rows: ${_rows//$'\n'/, })"

    _out=$(_p40_rpc RemoveSnatConfig "{\"user_id\":${USER_ID},\"eport\":${_P40_EPORT}}")
    _p40_rpc_ok "$_out" || \
        _issue="${_issue:+${_issue}; }RemoveSnatConfig did not answer with a status: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"
    _rows=$(_p40_port_fwd_rows "${USER_ID}")
    if printf '%s\n' "$_rows" | grep -q "^${_P40_EPORT} "; then
        _issue="${_issue:+${_issue}; }port forward ${_P40_EPORT} still in the table after the removal"
    fi

    _out=$(_p40_rpc SetSnatConfig \
        "{\"user_id\":${USER_ID},\"eport\":${_P40_EPORT},\"dip\":\"${_P40_DIP}\",\"iport\":${_P40_IPORT}}")
    _p40_rpc_ok "$_out" || \
        _issue="${_issue:+${_issue}; }re-adding the rule for the reconnect report failed: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"

    if [[ -z "$_issue" ]]; then
        pass "Step 181: offline SNAT writes reach the port forward table" \
            "eport ${_P40_EPORT} appeared, went away again, and is queued as an offline edit"
    else
        fail "Step 181: offline SNAT writes reach the port forward table" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 182 — AddDnsRecord and RemoveDnsRecord
    #
    # The record is looked up over the data path as well as in the node's own
    # list, and removing it twice has to say NOT_FOUND the second time. It is
    # added back at the end for the same reason the SNAT rule is.
    # ------------------------------------------------------------------
    info "Step 182: AddDnsRecord/RemoveDnsRecord ${_P40_DNS_DOMAIN} straight at the node..."
    _issue=""
    _P40_DNS_RECORD_LEFT=1
    _out=$(_p40_rpc AddDnsRecord \
        "{\"user_id\":${USER_ID},\"domain\":\"${_P40_DNS_DOMAIN}\",\"ip\":\"${_P40_DNS_IP}\",\"ttl\":${_P40_DNS_TTL}}")
    _p40_rpc_ok "$_out" || \
        _issue="AddDnsRecord did not answer with a status: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"
    _domains=$(_p40_dns_domains "${USER_ID}")
    printf '%s\n' "$_domains" | grep -qxF "${_P40_DNS_DOMAIN}" || \
        _issue="${_issue:+${_issue}; }${_P40_DNS_DOMAIN} is not in the node's static records after the add (records: ${_domains//$'\n'/, })"
    _p40_dig_check "${_P40_DNS_DOMAIN}" "${_P40_DNS_IP}"
    _verdict="$_P40_DIG_VERDICT"
    [[ "$_verdict" == "answered" ]] || \
        _issue="${_issue:+${_issue}; }DNS verdict '${_verdict}' for ${_P40_DNS_DOMAIN}, want answered with ${_P40_DNS_IP} (answer: ${_P40_DIG_RAW})"

    _out=$(_p40_rpc RemoveDnsRecord "{\"user_id\":${USER_ID},\"domain\":\"${_P40_DNS_DOMAIN}\"}")
    _p40_rpc_ok "$_out" || \
        _issue="${_issue:+${_issue}; }RemoveDnsRecord did not answer with a status: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"
    _domains=$(_p40_dns_domains "${USER_ID}")
    if printf '%s\n' "$_domains" | grep -qxF "${_P40_DNS_DOMAIN}"; then
        _issue="${_issue:+${_issue}; }${_P40_DNS_DOMAIN} is still in the node's static records after the removal"
    fi
    # The record is gone, so the proxy must stop answering for it while it
    # still answers for the fixture's own record: that second answer is what
    # tells a removed record apart from a DNS path that stopped working.
    _p40_dig_check "${_P40_DNS_DOMAIN}" "${_P40_DNS_IP}"
    _verdict="$_P40_DIG_VERDICT"
    [[ "$_verdict" == "silent" ]] || \
        _issue="${_issue:+${_issue}; }DNS verdict '${_verdict}' for the removed ${_P40_DNS_DOMAIN}, want silent (answer: ${_P40_DIG_RAW})"
    _p40_dig_check www.fastrg.org "${WAN_IP}"
    _verdict="$_P40_DIG_VERDICT"
    [[ "$_verdict" == "answered" ]] || \
        _issue="${_issue:+${_issue}; }DNS verdict '${_verdict}' for www.fastrg.org, so the silence above says nothing (answer: ${_P40_DIG_RAW})"

    _out=$(_p40_rpc RemoveDnsRecord "{\"user_id\":${USER_ID},\"domain\":\"${_P40_DNS_DOMAIN}\"}")
    printf '%s' "$_out" | grep -q 'NotFound' || \
        _issue="${_issue:+${_issue}; }removing ${_P40_DNS_DOMAIN} a second time answered '$(printf '%s' "$_out" | tr '\n' '|' | tail -c 120)', want NotFound"

    _out=$(_p40_rpc AddDnsRecord \
        "{\"user_id\":${USER_ID},\"domain\":\"${_P40_DNS_DOMAIN}\",\"ip\":\"${_P40_DNS_IP}\",\"ttl\":${_P40_DNS_TTL}}")
    _p40_rpc_ok "$_out" || \
        _issue="${_issue:+${_issue}; }re-adding the record for the reconnect report failed: $(printf '%s' "$_out" | tr '\n' '|' | tail -c 160)"

    if [[ -z "$_issue" ]]; then
        pass "Step 182: offline DNS record writes reach the node's records" \
            "${_P40_DNS_DOMAIN} resolved to ${_P40_DNS_IP}, went away again, and a second removal said NotFound"
    else
        _domains=$(_p40_dns_domains "${USER_ID}")
        _domains="${_domains:-none}"
        fail "Step 182: offline DNS record writes reach the node's records" \
            "${_issue}; static records now: ${_domains//$'\n'/, }"
    fi

    # ------------------------------------------------------------------
    # Step 183 — restore connectivity: every offline edit is reported over
    #            Kafka, and the fixture is put back through the controller
    # ------------------------------------------------------------------
    info "Step 183: restoring node->etcd connectivity and waiting for the offline-edit report..."
    _issue=""
    e2e_unblock_node_etcd

    _p40_wait_dirty_clear || true
    _dirty_hsi="$_P40_DIRTY_HSI"
    _dirty_dns="$_P40_DIRTY_DNS"
    _waited="$_P40_DIRTY_WAITED"
    [[ "$_dirty_hsi" == "false" ]] || \
        _issue="hsi/${USER_ID} still dirty='${_dirty_hsi}' ${_waited}s after the reconnect"
    [[ "$_dirty_dns" == "false" ]] || \
        _issue="${_issue:+${_issue}; }dns/${USER_ID} still dirty='${_dirty_dns}' ${_waited}s after the reconnect"

    # One report per snapshot entry carries the summary of every edit made to
    # it, so all ten markers belong to the two messages produced here.
    _kafka=$(_p40_kafka_since_baseline)
    _missing=""
    for _marker in "ipv6=true" "ipv6=false" "tcp_conntrack=false" "tcp_conntrack=true" \
        "dns_proxy=false" "dns_proxy=true" "snat set eport=${_P40_EPORT}" \
        "snat unset eport=${_P40_EPORT}" "dns add ${_P40_DNS_DOMAIN}" "dns del ${_P40_DNS_DOMAIN}"; do
        printf '%s' "$_kafka" | grep -qF "$_marker" || _missing="${_missing} '${_marker}'"
    done
    [[ -z "$_missing" ]] || \
        _issue="${_issue:+${_issue}; }no ConfigOfflineEdit marker after offset ${_P40_KAFKA_BASELINE} for:${_missing}"

    # Only once the report has landed: a removal that reaches etcd before the
    # node reports is undone by the report itself, which then writes the entry
    # back into etcd and leaves it behind for the next phase.
    if [[ "$_dirty_hsi" == "false" && "$_dirty_dns" == "false" ]]; then
        _reported=1
    else
        _issue="${_issue:+${_issue}; }the SNAT rule and the DNS record are left for cleanup: removing them before the node reports would let the report put them back into etcd"
    fi

    # The node is back under the controller, so the leftovers go out the way
    # every other config write does. Arbitration may or may not have copied
    # them into etcd first; either way this is what settles it.
    if [[ $_reported -eq 1 ]]; then
        fastrg_grpc remove_snat_config "${USER_ID}" "${_P40_EPORT}" >/dev/null 2>&1 || true
        fastrg_grpc remove_dns_record "${USER_ID}" "${_P40_DNS_DOMAIN}" >/dev/null 2>&1 || true

        # The flags are read the way the node reads them: an absent
        # tcp_conntrack_enable or dns_proxy_enable means true.
        _hsi=""
        for _i in $(seq 1 15); do
            sleep 2
            _etcd_hsi=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
            _etcd_dns=$(etcdctl_get_value "configs/${NODE_UUID}/dns/${USER_ID}" 2>/dev/null || true)
            _hsi=$(printf '%s' "$_etcd_hsi" | \
                jq -r '[.config.ipv6_enable == true, .config.tcp_conntrack_enable != false, .config.dns_proxy_enable != false] | join(",")' \
                2>/dev/null || true)
            [[ "$_hsi" != "false,true,true" ]] && continue
            printf '%s' "$_etcd_hsi" | jq -e ".config[\"port-mapping\"][]? | select(.eport == \"${_P40_EPORT}\")" \
                >/dev/null 2>&1 && continue
            printf '%s' "$_etcd_dns" | jq -e ".records[]? | select(.domain == \"${_P40_DNS_DOMAIN}\")" \
                >/dev/null 2>&1 && continue
            break
        done
        [[ "$_hsi" == "false,true,true" ]] || \
            _issue="${_issue:+${_issue}; }etcd HSI flags are '${_hsi:-unreadable}', want 'false,true,true'"
        if printf '%s' "$_etcd_hsi" | jq -e ".config[\"port-mapping\"][]? | select(.eport == \"${_P40_EPORT}\")" >/dev/null 2>&1; then
            _issue="${_issue:+${_issue}; }etcd still carries port-mapping ${_P40_EPORT}"
        fi
        if printf '%s' "$_etcd_dns" | jq -e ".records[]? | select(.domain == \"${_P40_DNS_DOMAIN}\")" >/dev/null 2>&1; then
            _issue="${_issue:+${_issue}; }etcd still carries the DNS record ${_P40_DNS_DOMAIN}"
        fi
        # Read back last: the loop above is the settling window for both sides.
        _p40_verify_residue_gone
    fi

    if [[ -z "$_issue" ]]; then
        pass "Step 183: reconnect reports the offline edits and the fixture comes back" \
            "both snapshot entries reported and cleared ${_waited}s after the reconnect; all ten edit markers on fastrg.node.events; etcd back to the fixture"
    else
        fail "Step 183: reconnect reports the offline edits and the fixture comes back" "$_issue"
    fi

    # ------------------------------------------------------------------
    # Step 184 — a cold start with etcd unreachable still applies the
    #            subscriber's static DNS records
    #
    # Only the local snapshot can supply them on this path: etcd stays blocked
    # across the whole restart, so the reconcile cannot be what filled the
    # table, and a missing record means the boot path never applied it. There
    # is no inconclusive reading here for the same reason.
    # ------------------------------------------------------------------
    info "Step 184: cold-starting the node with etcd unreachable..."
    _issue=""
    _domains=""
    e2e_block_node_etcd

    # SIGTERM only: SIGKILL skips the shutdown path that writes the snapshot
    # this step is about to read back.
    _P40_NODE_STOPPED=1
    if _p40_node_running; then
        ssh_node "pkill -x fastrg" >/dev/null 2>&1 || true
        for _i in $(seq 1 30); do
            if ! _p40_node_running; then
                _stopped=1
                break
            fi
            sleep 1
        done
    else
        _stopped=1
    fi
    [[ $_stopped -eq 1 ]] || _issue="fastrg did not exit within 30s of SIGTERM"

    if [[ -z "$_issue" ]]; then
        _started_at=$(date +%s)
        if ! e2e_start_node >/dev/null 2>&1; then
            _issue="the cold start command failed"
        fi
        _FASTRG_STARTED_BY_SCRIPT=1
    fi

    if [[ -z "$_issue" ]]; then
        for _i in $(seq 1 30); do
            sleep 5
            if _p40_node_running; then
                _P40_NODE_STOPPED=0
            fi
            if [[ "$(_p40_user_phase "${USER_ID}")" == "Data phase" ]]; then
                _recovered=1
                break
            fi
        done
        _elapsed=$(( $(date +%s) - _started_at ))
        [[ $_recovered -eq 1 ]] || \
            _issue="user ${USER_ID} did not reach Data phase within 150s of the cold start"
    fi

    if [[ -z "$_issue" ]]; then
        _domains=$(_p40_dns_domains "${USER_ID}")
        printf '%s\n' "$_domains" | grep -qxF 'www.fastrg.org' || \
            _issue="www.fastrg.org is not in the node's static records ${_elapsed}s after a cold start with etcd unreachable (records: ${_domains:-none})"
    fi

    info "Step 184: restoring node->etcd connectivity..."
    e2e_unblock_node_etcd
    _p40_wait_dirty_clear || true
    _dirty_hsi="$_P40_DIRTY_HSI"
    _dirty_dns="$_P40_DIRTY_DNS"
    _waited="$_P40_DIRTY_WAITED"
    [[ "$_dirty_hsi" == "false" ]] || \
        _issue="${_issue:+${_issue}; }hsi/${USER_ID} still dirty='${_dirty_hsi}' ${_waited}s after the reconnect"
    [[ "$_dirty_dns" == "false" ]] || \
        _issue="${_issue:+${_issue}; }dns/${USER_ID} still dirty='${_dirty_dns}' ${_waited}s after the reconnect"

    if [[ -z "$_issue" ]]; then
        pass "Step 184: cold start with etcd unreachable keeps the static DNS records" \
            "user ${USER_ID} back in Data phase and www.fastrg.org already in the static records ${_elapsed}s after the cold start; both snapshot entries clean again ${_waited}s after the reconnect"
    else
        fail "Step 184: cold start with etcd unreachable keeps the static DNS records" "$_issue"
    fi

    _cleanup_phase40_offline_cli_writes
}
