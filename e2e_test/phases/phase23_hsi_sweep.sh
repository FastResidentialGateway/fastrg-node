#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 23 — etcd HSI Sweep Reconcile (Steps 99-101d)
#
# Block the node's path to etcd, remove a synthetic subscriber through the
# controller while the watch is offline, then verify the periodic reconcile
# sweep removes the stale local subscriber after connectivity returns.
#
# Steps 101b-101d then check what a reconcile pass does with user 3's keys: a
# DNS value the node cannot read keeps the records it has, and an HSI key left
# behind a lowered subscriber count is skipped without an apply-failure report.
# ---------------------------------------------------------------------------

_P23_UID=3
_P23_VLAN=210
_P23_CANONICAL_COUNT=2
_p23_iptables_blocked=0

# User 3's static DNS record in Steps 101b-101d, and the value Step 101b writes
# over its etcd key: cut off mid-record, the way a torn write leaves it.
_P23_DNS_DOMAIN="reconcile-keep.fastrg.test"
_P23_DNS_IP="192.0.2.79"
_P23_DNS_TTL=60
_P23_BAD_DNS_VALUE='{"records":[{"domain":"reconcile-keep.fastrg.test"'
# The node's stdout/stderr; the app log at LogPath never sees stderr.
_P23_NODE_STDOUT_LOG=/var/log/fastrg.log

# Cleanup state of Steps 101b-101d: whether they changed the fixture, and the
# readable dns/3 value to put back while an unreadable one may be in etcd.
_P23_RC_SEEDED=0
_P23_RC_DNS_ORIGINAL=""

# ---------------------------------------------------------------------------
# Predicates (registered with local correctness validation below)
# ---------------------------------------------------------------------------

# The value config_snapshot.json holds for KEY (e.g. "dns/3"). Prints nothing
# when the entry is absent or marked deleted, and "err" when BODY is not a
# snapshot file.
e2e_snapshot_entry_value() {
    local _body="${1-}" _key="${2-}" _value=""

    if [[ -z "$_key" ]] || ! _value=$(printf '%s' "$_body" | jq -ej --arg k "$_key" '
            if type != "object" or (.entries | type) != "array" then error("not a snapshot")
            else [.entries[] | select(.key == $k and .exists == true)]
                 | if length == 0 then "" else (.[0].value | strings) end
            end' 2>/dev/null); then
        printf 'err'
        return 1
    fi
    printf '%s' "$_value"
}

# One get_dns_static reply as its "domain ip ttl" records, sorted and joined by
# ';' so two readings compare as strings. Prints nothing for an empty set, and
# "err" when BODY is not a reply, which is never an empty set.
e2e_dns_static_records() {
    local _body="${1-}" _records=""

    if ! _records=$(printf '%s' "$_body" | jq -ej '
            if type != "object" or (.entries | type) != "array" then error("not a reply")
            else [.entries[] | "\(.domain) \(.ip) \(.ttl)"] | sort | join(";") end' \
            2>/dev/null); then
        printf 'err'
        return 1
    fi
    printf '%s' "$_records"
}

# How many lines of a node stdout window report USER's DNS key on NODE as an
# unreadable envelope. "err" when the window was never read. The key has to end
# its line, so dns/3 does not count dns/30.
e2e_dns_envelope_error_count() {
    local _body="${1-}" _node="${2-}" _uid="${3-}"

    if [[ "$_body" == "err" || -z "$_node" || -z "$_uid" ]]; then
        printf 'err'
        return 1
    fi
    awk -v needle="Invalid DNS records envelope: configs/${_node}/dns/${_uid}" '
        { i = index($0, needle); if (i > 0 && substr($0, i) == needle) n++ }
        END { printf "%d", n }' <<< "$_body"
}

# How many reconcile passes in a node log window found USER's HSI config
# already applied. "err" when the window was never read.
e2e_reconcile_match_count() {
    local _body="${1-}" _uid="${2-}"

    if [[ "$_body" == "err" || -z "$_uid" ]]; then
        printf 'err'
        return 1
    fi
    grep -acF "Reconcile: HSI user ${_uid} already matches local state," <<< "$_body" || true
}

# Whether a node log window shows HSI user USER, beyond the subscriber count
# COUNT, skipped by a reconcile without being reported. Every user within COUNT
# must have been reconciled in the window: without a pass, a missing report
# proves nothing.
#
# Prints ok | err | reported:invalid_user_id | reported:apply_failed |
# no_reconcile | not_skipped.
e2e_reconcile_outside_count_verdict() {
    local _body="${1-}" _uid="${2-}" _count="${3-}" _user

    if [[ "$_body" == "err" || -z "$_uid" || ! "$_count" =~ ^[0-9]+$ ]]; then
        printf 'err'
        return 1
    fi
    if grep -qaF "Invalid user_id: ${_uid} (valid range:" <<< "$_body"; then
        printf 'reported:invalid_user_id'
        return 1
    fi
    if grep -qaF "apply failed for user ${_uid} (" <<< "$_body"; then
        printf 'reported:apply_failed'
        return 1
    fi
    for (( _user = 1; _user <= _count; _user++ )); do
        if [[ "$(e2e_reconcile_match_count "$_body" "$_user")" -lt 1 ]]; then
            printf 'no_reconcile'
            return 1
        fi
    done
    if ! grep -qaF "Reconcile: HSI user ${_uid} is outside the subscriber count ${_count}, skipping" \
            <<< "$_body"; then
        printf 'not_skipped'
        return 1
    fi
    printf 'ok'
}

local_validation_register snapshot_entry_value e2e_snapshot_entry_value \
    snapshot_entry_value_present \
    snapshot_entry_value_deleted \
    snapshot_entry_value_absent \
    snapshot_entry_value_unreadable

local_validation_register dns_static_records e2e_dns_static_records \
    dns_static_records_sorted \
    dns_static_records_empty \
    dns_static_records_unanswered \
    dns_static_records_error_reply

local_validation_register dns_envelope_error_count e2e_dns_envelope_error_count \
    dns_envelope_error_count_match \
    dns_envelope_error_count_mid_line \
    dns_envelope_error_count_other_key \
    dns_envelope_error_count_unread

local_validation_register reconcile_match_count e2e_reconcile_match_count \
    reconcile_match_count_two_passes \
    reconcile_match_count_other_user \
    reconcile_match_count_unread

local_validation_register reconcile_outside_count_verdict e2e_reconcile_outside_count_verdict \
    reconcile_outside_count_ok \
    reconcile_outside_count_invalid_user \
    reconcile_outside_count_apply_failed \
    reconcile_outside_count_no_reconcile \
    reconcile_outside_count_not_skipped \
    reconcile_outside_count_other_user_invalid \
    reconcile_outside_count_unread

_p23_block_etcd() {
    # Idempotent installation: discard a stale rule from an interrupted run.
    ssh_node "iptables -D OUTPUT -p tcp -d ${_P23_ETCD_HOST} --dport ${_P23_ETCD_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    if ssh_node "iptables -I OUTPUT 1 -p tcp -d ${_P23_ETCD_HOST} --dport ${_P23_ETCD_PORT} -j REJECT --reject-with tcp-reset" \
        >/dev/null 2>&1; then
        _p23_iptables_blocked=1
        return 0
    fi
    _p23_iptables_blocked=0
    return 1
}

_p23_unblock_etcd() {
    if [[ -n "${_P23_ETCD_HOST:-}" ]] && [[ -n "${_P23_ETCD_PORT:-}" ]]; then
        ssh_node "iptables -D OUTPUT -p tcp -d ${_P23_ETCD_HOST} --dport ${_P23_ETCD_PORT} -j REJECT --reject-with tcp-reset 2>/dev/null || true" \
            >/dev/null 2>&1 || true
    fi
    _p23_iptables_blocked=0
    return 0
}

_p23_hsi_field() {
    local _uid="$1" _field="$2"
    fastrg_grpc get_hsi_info 2>/dev/null | \
        jq -r ".hsi_infos[] | select(.user_id == ${_uid}) | .${_field} // empty" \
        2>/dev/null || true
}

_p23_log_count() {
    local _needle="$1" _count
    _count=$(ssh_node "grep -cF '${_needle}' '${_P23_LOG_PATH}' 2>/dev/null || true" \
        2>/dev/null | tail -1 | tr -d '[:space:]' || true)
    [[ "$_count" =~ ^[0-9]+$ ]] || _count=0
    printf '%s' "$_count"
}

# Read-only observation that the fresh REJECT rule has seen node->etcd traffic
# and that no established TCP connection to the endpoint remains. This avoids
# probing etcd_reachable through a node gRPC write during the blocked window.
_p23_etcd_path_is_down() {
    local _rules="" _packets="" _established=""

    _rules=$(ssh_node "iptables -L OUTPUT -n -v -x 2>/dev/null" 2>/dev/null || true)
    _packets=$(printf '%s\n' "$_rules" | awk \
        -v host="${_P23_ETCD_HOST}" -v dport="dpt:${_P23_ETCD_PORT}" \
        '$3 == "REJECT" && $9 == host && index($0, dport) {print $1; exit}' || true)
    [[ "$_packets" =~ ^[0-9]+$ ]] || _packets=0
    [[ "$_packets" -gt 0 ]] || return 1

    _established=$(ssh_node "ss -Hnt state established" 2>/dev/null) || return 1
    ! e2e_etcd_connection_up "$_established" "${_P23_ETCD_HOST}" "${_P23_ETCD_PORT}" >/dev/null
}

# Condition: both etcd and the node report WANT subscribers. A failed read of
# either one means that side could not be asked, which the waiter treats
# differently from "the count has not caught up yet".
_p23_count_is() {
    local _want="$1" _raw _etcd_count _local_count

    _raw=$(etcdctl_get_value "user_counts/${NODE_UUID}/") || return 2
    _etcd_count=$(printf '%s' "$_raw" | jq -r '.subscriber_count // empty' 2>/dev/null || true)
    _raw=$(fastrg_grpc_checked get_system_info) || return 2
    _local_count=$(printf '%s' "$_raw" | jq -r '.num_users // empty' 2>/dev/null || true)

    [[ "$_etcd_count" == "$_want" ]] && [[ "$_local_count" == "$_want" ]]
}

# Condition: both etcd and the node report the canonical subscriber count.
_p23_count_is_canonical() {
    _p23_count_is "${_P23_CANONICAL_COUNT}"
}

_p23_restore_count() {
    fastrg_grpc set_subscriber_count "${_P23_CANONICAL_COUNT}" >/dev/null 2>&1 || true
    _e2e_wait_for "Cleanup(phase23): subscriber count back to ${_P23_CANONICAL_COUNT}" \
        15 2 _p23_count_is_canonical
}

# Set the subscriber count through the controller and wait until etcd and the
# node both report it.
_p23_set_count() {
    local _rc=0

    fastrg_grpc set_subscriber_count "$1" >/dev/null 2>&1 || true
    _e2e_wait_for "Phase 23: subscriber count ${1}" 15 2 _p23_count_is "$1" || _rc=$?
    return "$_rc"
}

# Wait up to 90s for the node's etcd connection to be gone under the block.
_p23_wait_etcd_path_down() {
    local _i

    for _i in $(seq 1 45); do
        sleep 2
        _p23_etcd_path_is_down && return 0
    done
    return 1
}

# Every config key etcd holds for this node, sorted onto one line. Non-zero
# when etcd could not be asked.
_p23_config_keys() {
    local _keys=""

    _keys=$(etcdctl_get "--prefix --keys-only configs/${NODE_UUID}/" 2>/dev/null) || return 1
    printf '%s\n' "$_keys" | awk 'NF' | sort | tr '\n' ' ' | sed 's/ $//'
}

# The value the node's config snapshot holds for KEY; "err" (non-zero) when the
# snapshot file could not be read.
_p23_snapshot_value() {
    local _raw=""

    if ! _raw=$(ssh_node "cat /etc/fastrg/config_snapshot.json" 2>/dev/null); then
        printf 'err'
        return 1
    fi
    e2e_snapshot_entry_value "$_raw" "$1"
}

# Condition: the node's config snapshot holds VALUE for KEY.
_p23_snapshot_is() {
    local _value=""

    _value=$(_p23_snapshot_value "$1") || return 2
    [[ "$_value" == "$2" ]]
}

# USER's static DNS records on the node, as e2e_dns_static_records prints them.
_p23_dns_records() {
    e2e_dns_static_records "$(fastrg_grpc get_dns_static "$1")"
}

# Condition: the node holds exactly RECORDS for USER.
_p23_dns_records_are() {
    local _records=""

    _records=$(_p23_dns_records "$1") || return 2
    [[ "$_records" == "$2" ]]
}

# Condition: the node lists user 3 on its VLAN with an account.
_p23_user_active() {
    [[ "$(_p23_hsi_field "${_P23_UID}" vlan_id)" == "${_P23_VLAN}" ]] && \
        [[ -n "$(_p23_hsi_field "${_P23_UID}" account)" ]]
}

# Line count of a node log, the mark a later window starts after. Prints
# nothing when the log could not be read.
_p23_log_mark() {
    local _lines=""

    _lines=$(ssh_node "wc -l < '$1'" 2>/dev/null | tr -d '[:space:]' || true)
    [[ "$_lines" =~ ^[0-9]+$ ]] && printf '%s' "$_lines"
    return 0
}

# What a node log gained after MARK. Prints "err" when the log could not be read
# or is now shorter than the mark (replaced since), so a window that was never
# read cannot pass for an empty one.
_p23_log_window() {
    local _path="$1" _mark="$2" _window=""

    if [[ ! "$_mark" =~ ^[0-9]+$ ]] || ! _window=$(ssh_node \
            "[ -r '${_path}' ] && [ \"\$(wc -l < '${_path}')\" -ge ${_mark} ] && tail -n +$(( _mark + 1 )) '${_path}'" \
            2>/dev/null); then
        printf 'err'
        return 0
    fi
    printf '%s' "$_window"
}

# Condition: the app log gained more than BASE user 1 reconcile matches after
# MARK.
_p23_reconcile_passes_exceed() {
    local _count=""

    _count=$(e2e_reconcile_match_count "$(_p23_log_window "$_P23_LOG_PATH" "$1")" 1) || return 2
    [[ "$_count" -gt "$2" ]]
}

# Condition: the node stdout log gained an envelope error for user 3's DNS key
# after MARK.
_p23_dns_envelope_error_seen() {
    local _errors=""

    _errors=$(e2e_dns_envelope_error_count "$(_p23_log_window "$_P23_NODE_STDOUT_LOG" "$1")" \
        "${NODE_UUID}" "${_P23_UID}") || return 2
    [[ "$_errors" -gt 0 ]]
}

# What is off with users 1 and 2 against the DNS records read before Step 101b;
# empty when both are in Data phase with those records.
_p23_fixture_issues() {
    local _dns1="$1" _dns2="$2" _issues="" _seen=""

    _seen=$(_p23_hsi_field 1 status)
    [[ "$_seen" == "Data phase" ]] || _issues="${_issues} user1_status='${_seen:-empty}'"
    _seen=$(_p23_hsi_field 2 status)
    [[ "$_seen" == "Data phase" ]] || _issues="${_issues} user2_status='${_seen:-empty}'"
    _seen=$(_p23_dns_records 1 || true)
    [[ "$_seen" == "$_dns1" ]] || _issues="${_issues} user1_dns='${_seen}' (was '${_dns1}')"
    _seen=$(_p23_dns_records 2 || true)
    [[ "$_seen" == "$_dns2" ]] || _issues="${_issues} user2_dns='${_seen}' (was '${_dns2}')"
    printf '%s' "${_issues# }"
}

# Put back the readable dns/3 value Step 101b replaced, if it is still out.
_p23_restore_dns_value() {
    [[ -n "${_P23_RC_DNS_ORIGINAL}" ]] || return 0
    e2e_etcd_http_put "configs/${NODE_UUID}/dns/${_P23_UID}" "${_P23_RC_DNS_ORIGINAL}" || return 1
    _P23_RC_DNS_ORIGINAL=""
}

# Undo Steps 101b-101d: lift the block, put the readable DNS value back, remove
# user 3 through the controller while the count still allows it, and return to
# the canonical count. Idempotent; only lifts the block when nothing was seeded.
_p23_reconcile_cleanup() {
    local _hsi_key="configs/${NODE_UUID:-}/hsi/${_P23_UID}"
    local _dns_key="configs/${NODE_UUID:-}/dns/${_P23_UID}"
    local _hsi="" _dns=""

    e2e_unblock_node_etcd
    [[ "${_P23_RC_SEEDED}" -eq 1 ]] || return 0

    _p23_restore_dns_value || \
        warn "Cleanup(phase23): could not put the readable value back into ${_dns_key}."
    _hsi=$(e2e_etcd_http_get "$_hsi_key" || true)
    _dns=$(e2e_etcd_http_get "$_dns_key" || true)
    if [[ -n "$_hsi" || -n "$_dns" ]]; then
        # The controller refuses a user id above the count, so raise it first.
        _p23_set_count "${_P23_UID}" || \
            warn "Cleanup(phase23): subscriber count ${_P23_UID} did not converge."
        if [[ -n "$_dns" ]]; then
            fastrg_grpc remove_dns_record "${_P23_UID}" "${_P23_DNS_DOMAIN}" >/dev/null 2>&1 || true
        fi
        if [[ -n "$_hsi" ]]; then
            remove_hsi_config_verified "${_P23_UID}" || true
        fi
    fi
    _p23_restore_count || true
    _P23_RC_SEEDED=0
    return 0
}

# Idempotent: called at the end of phase23 and from the top-level EXIT trap.
# Connectivity is always restored before any controller cleanup write.
_cleanup_phase23_hsi_sweep() {
    _p23_unblock_etcd
    _p23_reconcile_cleanup

    if [[ -n "${NODE_UUID:-}" ]]; then
        local _remaining
        _remaining=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${_P23_UID}" 2>/dev/null || true)
        if [[ -n "$_remaining" ]]; then
            info "Cleanup(phase23): removing residual user ${_P23_UID} config..."
            remove_hsi_config_verified "${_P23_UID}" || true
        fi
        _p23_restore_count || true
    fi
    return 0
}

# Steps 101b-101d: what a reconcile pass does with user 3's etcd keys.
#   101b  a DNS value the node cannot read keeps the records it has
#   101c  an HSI key left behind a lowered subscriber count is skipped unreported
#   101d  the canonical fixture is back
_p23_reconcile_checks() {
    local _hsi_key="configs/${NODE_UUID}/hsi/${_P23_UID}"
    local _dns_key="configs/${NODE_UUID}/dns/${_P23_UID}"
    local _snapshot_key="dns/${_P23_UID}"
    local _records="${_P23_DNS_DOMAIN} ${_P23_DNS_IP} ${_P23_DNS_TTL}"
    local _seed_ok=1 _ok_b=1 _ok_c=1
    local _issue_seed="" _issue_b="" _issue_c="" _issue_d=""
    local _keys_before="" _keys_after="" _dns1_before="" _dns2_before=""
    local _baseline="" _account="" _password="" _original="" _seen=""
    local _out_mark="" _app_mark="" _control_mark="" _matches="" _errors="" _verdict=""
    local _rc=0 _i

    # What the cleanup below has to hand back.
    _keys_before=$(_p23_config_keys) || _keys_before="err"
    _dns1_before=$(_p23_dns_records 1 || true)
    _dns2_before=$(_p23_dns_records 2 || true)
    _rc=0
    _p23_count_is "${_P23_CANONICAL_COUNT}" || _rc=$?
    if [[ "$_keys_before" == "err" ]]; then
        _seed_ok=0
        _issue_seed="etcd config keys could not be read"
    elif [[ " ${_keys_before} " == *" ${_hsi_key} "* || " ${_keys_before} " == *" ${_dns_key} "* ]]; then
        _seed_ok=0
        _issue_seed="user ${_P23_UID} keys are already in etcd: ${_keys_before}"
    elif [[ "$_rc" -ne 0 ]]; then
        _seed_ok=0
        _issue_seed="subscriber count is not ${_P23_CANONICAL_COUNT} in both etcd and the node"
    elif [[ "$_dns1_before" == "err" || "$_dns2_before" == "err" ]]; then
        _seed_ok=0
        _issue_seed="DNS records of users 1/2 could not be read"
    fi

    # Seed user 3 with an HSI config and one DNS record through the controller.
    if [[ $_seed_ok -eq 1 ]]; then
        info "Step 101b: expanding to ${_P23_UID} subscribers and seeding user ${_P23_UID} (VLAN ${_P23_VLAN}, DNS ${_P23_DNS_DOMAIN})..."
        _baseline=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/2" 2>/dev/null || true)
        _account=$(printf '%s' "$_baseline" | jq -r '.config.account_name // empty' 2>/dev/null || true)
        _password=$(printf '%s' "$_baseline" | jq -r '.config.password // empty' 2>/dev/null || true)
        if [[ -z "$_account" || -z "$_password" ]]; then
            _seed_ok=0
            _issue_seed="user 2 baseline account/password is unavailable"
        fi
    fi
    if [[ $_seed_ok -eq 1 ]]; then
        _P23_RC_SEEDED=1
        if ! _p23_set_count "${_P23_UID}"; then
            _seed_ok=0
            _issue_seed="subscriber count ${_P23_UID} did not converge"
        fi
    fi
    if [[ $_seed_ok -eq 1 ]]; then
        fastrg_grpc apply_config "${_P23_UID}" "${_P23_VLAN}" "$_account" "$_password" \
            192.168.6.2 192.168.6.10 255.255.255.0 192.168.6.1 >/dev/null
        _rc=0
        _e2e_wait_for "Step 101b: user ${_P23_UID} active" 30 2 _p23_user_active || _rc=$?
        if [[ "$_rc" -ne 0 ]]; then
            _seed_ok=0
            _issue_seed="user ${_P23_UID} did not become active locally (vlan='$(_p23_hsi_field "${_P23_UID}" vlan_id)')"
        fi
    fi
    if [[ $_seed_ok -eq 1 ]]; then
        fastrg_grpc add_dns_record "${_P23_UID}" "${_P23_DNS_DOMAIN}" "${_P23_DNS_IP}" \
            "${_P23_DNS_TTL}" >/dev/null
        _rc=0
        _e2e_wait_for "Step 101b: user ${_P23_UID} DNS record" 15 2 \
            _p23_dns_records_are "${_P23_UID}" "$_records" || _rc=$?
        if [[ "$_rc" -ne 0 ]]; then
            _seed_ok=0
            _issue_seed="user ${_P23_UID} DNS records read '$(_p23_dns_records "${_P23_UID}" || true)', want '${_records}'"
        fi
    fi
    if [[ $_seed_ok -eq 1 ]]; then
        _original=$(e2e_etcd_http_get "$_dns_key" || true)
        if [[ -z "$_original" || "$_original" == "err" ]]; then
            _seed_ok=0
            _issue_seed="${_dns_key} read '${_original:-absent}' through $(e2e_etcd_http_url)"
        fi
    fi
    if [[ $_seed_ok -eq 1 ]]; then
        _rc=0
        _e2e_wait_for "Step 101b: snapshot holds ${_snapshot_key}" 10 1 \
            _p23_snapshot_is "$_snapshot_key" "$_original" || _rc=$?
        if [[ "$_rc" -ne 0 ]]; then
            _seed_ok=0
            _issue_seed="node snapshot does not hold the seeded ${_snapshot_key} value"
        fi
    fi

    # ---- Step 101b: an unreadable DNS value keeps the node's records ----
    if [[ $_seed_ok -eq 0 ]]; then
        _ok_b=0
        _issue_b="seeding failed: ${_issue_seed}"
    fi
    # The write waits until the node's connection is gone, so etcd has no stream
    # to deliver it on. Blocking etcd->node too would drop the reset that closes
    # that connection, and the write would arrive once the block lifts.
    if [[ $_ok_b -eq 1 ]]; then
        info "Step 101b: blocking node->etcd and waiting for the node's connection to drop..."
        if ! e2e_block_node_etcd; then
            _ok_b=0
            _issue_b="failed to install the node->etcd block"
        elif ! _p23_wait_etcd_path_down; then
            _ok_b=0
            _issue_b="node->etcd connection still up 90s after the block"
        fi
    fi
    if [[ $_ok_b -eq 1 ]]; then
        info "Step 101b: writing an unreadable value into ${_dns_key} from the runner..."
        _out_mark=$(_p23_log_mark "$_P23_NODE_STDOUT_LOG")
        # Saved before the write, so every path out puts the readable value back.
        _P23_RC_DNS_ORIGINAL="$_original"
        if [[ -z "$_out_mark" ]]; then
            _ok_b=0
            _issue_b="node log ${_P23_NODE_STDOUT_LOG} could not be read"
        elif ! e2e_etcd_http_put "$_dns_key" "$_P23_BAD_DNS_VALUE"; then
            _ok_b=0
            _issue_b="could not write the unreadable value through $(e2e_etcd_http_url)"
        else
            _seen=$(_p23_snapshot_value "$_snapshot_key" || true)
            if [[ "$_seen" != "$_original" ]]; then
                _ok_b=0
                _issue_b="node snapshot ${_snapshot_key} changed while cut off from etcd: '${_seen:0:120}'"
            fi
        fi
    fi
    if [[ $_ok_b -eq 1 ]]; then
        _app_mark=$(_p23_log_mark "$_P23_LOG_PATH")
        if [[ -z "$_app_mark" ]]; then
            _ok_b=0
            _issue_b="node log ${_P23_LOG_PATH} could not be read"
        fi
    fi
    e2e_unblock_node_etcd

    # Control: only Step 3c of a reconcile can mirror a value the watch never saw.
    if [[ $_ok_b -eq 1 ]]; then
        info "Step 101b: waiting up to 180s for a reconcile to mirror the unreadable value..."
        _rc=0
        _e2e_wait_for "Step 101b: reconcile mirrors ${_snapshot_key}" 90 2 \
            _p23_snapshot_is "$_snapshot_key" "$_P23_BAD_DNS_VALUE" || _rc=$?
        if [[ "$_rc" -ne 0 ]]; then
            _seen=$(_p23_snapshot_value "$_snapshot_key" || true)
            _ok_b=0
            _issue_b="no reconcile mirrored the unreadable ${_snapshot_key} within 180s (snapshot: '${_seen:0:120}')"
        fi
    fi
    # The next pass queues behind this pass's DNS events; once it has run, the
    # records read below are what the reconcile left.
    if [[ $_ok_b -eq 1 ]]; then
        _matches=$(e2e_reconcile_match_count "$(_p23_log_window "$_P23_LOG_PATH" "$_app_mark")" 1 || true)
        _rc=0
        if [[ ! "$_matches" =~ ^[0-9]+$ ]]; then
            _rc=2
        else
            _e2e_wait_for "Step 101b: next reconcile pass" 45 2 \
                _p23_reconcile_passes_exceed "$_app_mark" "$_matches" || _rc=$?
        fi
        if [[ "$_rc" -ne 0 ]]; then
            _ok_b=0
            _issue_b="no further reconcile pass in ${_P23_LOG_PATH} within 90s (user 1 matches since the block lifted: ${_matches:-unread})"
        fi
    fi
    if [[ $_ok_b -eq 1 ]]; then
        _seen=$(_p23_dns_records "${_P23_UID}" || true)
        if [[ "$_seen" != "$_records" ]]; then
            _ok_b=0
            _issue_b="user ${_P23_UID} DNS records read '${_seen}' after the reconcile, want '${_records}'"
        fi
    fi
    # Guard: had the watch seen the write too, the mirror above would not be
    # the reconcile's alone.
    if [[ $_ok_b -eq 1 ]]; then
        _errors=$(e2e_dns_envelope_error_count \
            "$(_p23_log_window "$_P23_NODE_STDOUT_LOG" "$_out_mark")" "${NODE_UUID}" "${_P23_UID}" || true)
        if [[ "$_errors" != "0" ]]; then
            _ok_b=0
            _issue_b="the watch also read the unreadable value: ${_errors} envelope error(s) for ${_dns_key} in ${_P23_NODE_STDOUT_LOG}"
        fi
    fi
    # Control for the guard: the same write with the watch up has to show there.
    if [[ $_ok_b -eq 1 ]]; then
        _control_mark=$(_p23_log_mark "$_P23_NODE_STDOUT_LOG")
        _rc=0
        if [[ -z "$_control_mark" ]] || ! e2e_etcd_http_put "$_dns_key" "$_P23_BAD_DNS_VALUE"; then
            _rc=2
        else
            _e2e_wait_for "Step 101b: watch rejects the unreadable value" 10 1 \
                _p23_dns_envelope_error_seen "$_control_mark" || _rc=$?
        fi
        if [[ "$_rc" -ne 0 ]]; then
            _ok_b=0
            _issue_b="guard control failed: rewriting the unreadable value with the watch up left no envelope error in ${_P23_NODE_STDOUT_LOG}"
        fi
    fi
    if [[ -n "${_P23_RC_DNS_ORIGINAL}" ]]; then
        _rc=0
        if ! _p23_restore_dns_value; then
            _rc=1
        else
            _e2e_wait_for "Step 101b: snapshot takes the readable value back" 10 1 \
                _p23_snapshot_is "$_snapshot_key" "$_original" || _rc=$?
        fi
        if [[ "$_rc" -ne 0 ]]; then
            _ok_b=0
            _issue_b="${_issue_b:+${_issue_b}; }the readable ${_dns_key} value did not get back to etcd and the node snapshot"
        fi
    fi

    if [[ $_ok_b -eq 1 ]]; then
        pass "Step 101b: reconcile keeps DNS records whose etcd value is unreadable" \
            "reconcile mirrored the unreadable ${_snapshot_key}; user ${_P23_UID} kept '${_records}'; the watch never saw it while cut off"
    else
        fail "Step 101b: reconcile keeps DNS records whose etcd value is unreadable" "$_issue_b"
    fi

    # ---- Step 101c: an HSI key beyond the count is skipped unreported ----
    if [[ $_seed_ok -eq 0 ]]; then
        _ok_c=0
        _issue_c="seeding failed: ${_issue_seed}"
    fi
    # Lowering the count leaves user 3's keys in etcd.
    if [[ $_ok_c -eq 1 ]]; then
        info "Step 101c: lowering the subscriber count to ${_P23_CANONICAL_COUNT} with user ${_P23_UID}'s keys left in etcd..."
        if ! _p23_set_count "${_P23_CANONICAL_COUNT}"; then
            _ok_c=0
            _issue_c="subscriber count ${_P23_CANONICAL_COUNT} did not converge"
        else
            _seen=$(e2e_etcd_http_get "$_hsi_key" || true)
            if [[ -z "$_seen" || "$_seen" == "err" ]]; then
                _ok_c=0
                _issue_c="${_hsi_key} read '${_seen:-absent}' after lowering the count"
            fi
        fi
    fi
    if [[ $_ok_c -eq 1 ]]; then
        info "Step 101c: blocking node->etcd, then lifting the block to trigger a reconcile..."
        if ! e2e_block_node_etcd; then
            _ok_c=0
            _issue_c="failed to install the node->etcd block"
        elif ! _p23_wait_etcd_path_down; then
            _ok_c=0
            _issue_c="node->etcd connection still up 90s after the block"
        else
            _app_mark=$(_p23_log_mark "$_P23_LOG_PATH")
            if [[ -z "$_app_mark" ]]; then
                _ok_c=0
                _issue_c="node log ${_P23_LOG_PATH} could not be read"
            fi
        fi
    fi
    e2e_unblock_node_etcd
    if [[ $_ok_c -eq 1 ]]; then
        info "Step 101c: waiting up to 180s for a reconcile pass after the block..."
        for _i in $(seq 1 90); do
            _verdict=$(e2e_reconcile_outside_count_verdict \
                "$(_p23_log_window "$_P23_LOG_PATH" "$_app_mark")" \
                "${_P23_UID}" "${_P23_CANONICAL_COUNT}" || true)
            [[ "$_verdict" == "ok" || "$_verdict" == reported:* ]] && break
            sleep 2
        done
        if [[ "$_verdict" != "ok" ]]; then
            _ok_c=0
            _issue_c="${_P23_LOG_PATH} after the block: ${_verdict:-no verdict} (want ok)"
        fi
    fi
    if [[ $_ok_c -eq 1 ]]; then
        _issue_c=$(_p23_fixture_issues "$_dns1_before" "$_dns2_before")
        [[ -z "$_issue_c" ]] || _ok_c=0
    fi

    if [[ $_ok_c -eq 1 ]]; then
        pass "Step 101c: reconcile skips HSI keys beyond the subscriber count unreported" \
            "users 1/2 reconciled; user ${_P23_UID} skipped at count ${_P23_CANONICAL_COUNT} with no Invalid user_id or apply failure; users 1/2 in Data phase, DNS unchanged"
    else
        fail "Step 101c: reconcile skips HSI keys beyond the subscriber count unreported" "$_issue_c"
    fi

    # ---- Step 101d: canonical fixture restored ----
    info "Step 101d: removing user ${_P23_UID} through the controller and restoring the canonical fixture..."
    _p23_reconcile_cleanup
    _keys_after=$(_p23_config_keys) || _keys_after="err"
    if [[ "$_keys_before" == "err" || "$_keys_after" != "$_keys_before" ]]; then
        _issue_d="${_issue_d} config_keys='${_keys_after}' (was '${_keys_before}')"
    fi
    _rc=0
    _p23_count_is "${_P23_CANONICAL_COUNT}" || _rc=$?
    [[ "$_rc" -eq 0 ]] || _issue_d="${_issue_d} subscriber_count_not_${_P23_CANONICAL_COUNT}"
    _seen=$(_p23_fixture_issues "$_dns1_before" "$_dns2_before")
    [[ -z "$_seen" ]] || _issue_d="${_issue_d} ${_seen}"

    if [[ -z "$_issue_d" ]]; then
        pass "Step 101d: restore canonical fixture after the reconcile checks" \
            "etcd config keys as before; etcd/local count=${_P23_CANONICAL_COUNT}; users 1/2 in Data phase, DNS unchanged"
    else
        fail "Step 101d: restore canonical fixture after the reconcile checks" "${_issue_d# }"
    fi
    return 0
}

phase23_hsi_sweep() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 23 — etcd HSI Sweep Reconcile (Steps 99-101d)"
    bold "═══════════════════════════════════════════════════════"

    local _step95_ok=1 _step96_ok=1 _step97_ok=1
    local _issue95="" _issue96="" _issue97=""
    local _baseline="" _account="" _password=""
    local _count_reply="" _apply_reply="" _remove_reply=""
    local _etcd_count="" _local_count="" _local_vlan="" _local_account=""
    local _sweep_log_before=0 _sweep_log_after=0
    local _status1="" _status2="" _keys="" _key_ids=""
    local _log_path_raw=""
    local _i _rc=0

    _P23_ETCD_HOST="${ETCD_ENDPOINT%%:*}"
    _P23_ETCD_PORT="${ETCD_ENDPOINT##*:}"
    _log_path_raw=$(ssh_node "grep 'LogPath' /etc/fastrg/config.cfg 2>/dev/null" || true)
    _P23_LOG_PATH=$(printf '%s' "$_log_path_raw" | awk -F'"' '{print $2}' || true)
    [[ -n "$_P23_LOG_PATH" ]] || _P23_LOG_PATH=/var/log/fastrg.log

    if ! ssh_node "command -v iptables >/dev/null 2>&1"; then
        _step95_ok=0
        _issue95="iptables is not available on the node"
    elif [[ -z "$_P23_ETCD_HOST" || -z "$_P23_ETCD_PORT" || \
            "$_P23_ETCD_HOST" == "$ETCD_ENDPOINT" || "$_P23_ETCD_PORT" == "$ETCD_ENDPOINT" ]]; then
        _step95_ok=0
        _issue95="could not parse ETCD_ENDPOINT='${ETCD_ENDPOINT:-empty}'"
    fi

    # Seed user 3 through the controller while etcd is reachable.
    if [[ $_step95_ok -eq 1 ]]; then
        _baseline=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/2" 2>/dev/null || true)
        _account=$(printf '%s' "$_baseline" | jq -r '.config.account_name // empty' 2>/dev/null || true)
        _password=$(printf '%s' "$_baseline" | jq -r '.config.password // empty' 2>/dev/null || true)
        if [[ -z "$_account" || -z "$_password" ]]; then
            _step95_ok=0
            _issue95="user 2 baseline account/password is unavailable"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        info "Step 99: expanding to 3 subscribers and applying synthetic user 3 (VLAN 210)..."
        _count_reply=$(fastrg_grpc set_subscriber_count 3 2>/dev/null || true)
        if [[ -z "$(printf '%s' "$_count_reply" | jq -r '.status // empty' 2>/dev/null || true)" ]]; then
            _step95_ok=0
            _issue95="SetSubscriberCount returned no status: ${_count_reply:-empty}"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        for _i in $(seq 1 15); do
            _etcd_count=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null | \
                jq -r '.subscriber_count // empty' 2>/dev/null || true)
            _local_count=$(fastrg_grpc get_system_info 2>/dev/null | \
                jq -r '.num_users // empty' 2>/dev/null || true)
            [[ "$_etcd_count" == "3" && "$_local_count" == "3" ]] && break
            sleep 2
        done
        if [[ "$_etcd_count" != "3" || "$_local_count" != "3" ]]; then
            _step95_ok=0
            _issue95="subscriber count 3 did not converge (etcd='${_etcd_count:-empty}', local='${_local_count:-empty}')"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        _apply_reply=$(fastrg_grpc apply_config \
            3 210 "$_account" "$_password" \
            192.168.6.2 192.168.6.10 255.255.255.0 192.168.6.1 \
            2>/dev/null || true)
        if [[ -z "$(printf '%s' "$_apply_reply" | jq -r '.status // empty' 2>/dev/null || true)" ]]; then
            _step95_ok=0
            _issue95="ApplyConfig returned no status: ${_apply_reply:-empty}"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        for _i in $(seq 1 30); do
            _local_vlan=$(_p23_hsi_field 3 vlan_id)
            _local_account=$(_p23_hsi_field 3 account)
            [[ "$_local_vlan" == "210" && -n "$_local_account" ]] && break
            sleep 2
        done
        if [[ "$_local_vlan" != "210" || -z "$_local_account" ]]; then
            _step95_ok=0
            _issue95="user 3 did not become active locally (vlan='${_local_vlan:-empty}', account='${_local_account:-empty}')"
        fi
    fi

    # The blocked window contains exactly one write: controller remove_config 3.
    if [[ $_step95_ok -eq 1 ]]; then
        info "Step 99: blocking node->etcd (${_P23_ETCD_HOST}:${_P23_ETCD_PORT}) and waiting for watchdog offline detection..."
        if ! _p23_block_etcd; then
            _step95_ok=0
            _issue95="failed to install node->etcd iptables REJECT rule"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        for _i in $(seq 1 45); do
            sleep 2
            _p23_etcd_path_is_down && break
        done
        if ! _p23_etcd_path_is_down; then
            _step95_ok=0
            _issue95="node->etcd REJECT traffic/connection-down state was not observed within 90s"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        info "Step 99: deleting user 3 through the controller while the node watch is offline..."
        _remove_reply=$(fastrg_grpc remove_config 3 2>/dev/null || true)
        if [[ -z "$(printf '%s' "$_remove_reply" | jq -r '.status // empty' 2>/dev/null || true)" ]]; then
            _step95_ok=0
            _issue95="RemoveConfig returned no status: ${_remove_reply:-empty}"
        fi
    fi

    # Asked from the runner: the block also cuts etcdctl on the node off.
    if [[ $_step95_ok -eq 1 ]]; then
        _rc=0
        _e2e_wait_for "Step 99: waiting for etcd hsi/3 to disappear" 15 2 \
            e2e_etcd_http_key_absent "configs/${NODE_UUID}/hsi/3" || _rc=$?
        _local_vlan=$(_p23_hsi_field 3 vlan_id)
        _local_account=$(_p23_hsi_field 3 account)
        if [[ "$_rc" -ge 2 ]]; then
            _step95_ok=0
            _issue95="etcd could not be read from the runner ($(e2e_etcd_http_url)) to confirm hsi/3 is gone"
        elif [[ "$_rc" -ne 0 || "$_local_vlan" != "210" || -z "$_local_account" ]]; then
            _step95_ok=0
            _issue95="stale state not established (etcd_key=$([[ "$_rc" -eq 0 ]] && printf absent || printf present), local_vlan='${_local_vlan:-empty}', local_account='${_local_account:-empty}')"
        fi
    fi

    if [[ $_step95_ok -eq 1 ]]; then
        pass "Step 99: establish stale HSI subscriber" \
            "etcd hsi/3 absent while node still lists user 3 with vlan=210"
    else
        fail "Step 99: establish stale HSI subscriber" "$_issue95"
    fi

    # Restore connectivity at the phase boundary even when Step 99 failed.
    _sweep_log_before=$(_p23_log_count "Reconcile sweep: user 3 active locally but absent from etcd")
    _p23_unblock_etcd

    if [[ $_step95_ok -eq 0 ]]; then
        _step96_ok=0
        _issue96="stale-state prerequisite failed"
    else
        info "Step 100: waiting up to 180s for reconcile sweep to remove stale user 3..."
        for _i in $(seq 1 90); do
            _local_vlan=$(_p23_hsi_field 3 vlan_id)
            if [[ -z "$_local_vlan" || "$_local_vlan" == "0" ]]; then
                break
            fi
            sleep 2
        done
        _sweep_log_after=$(_p23_log_count "Reconcile sweep: user 3 active locally but absent from etcd")
        if [[ -n "$_local_vlan" && "$_local_vlan" != "0" ]]; then
            _step96_ok=0
            _issue96="node still lists user 3 with vlan='${_local_vlan}' after 180s"
        elif [[ "$_sweep_log_after" -le "$_sweep_log_before" ]]; then
            _step96_ok=0
            _issue96="user 3 cleared but current-run reconcile log is missing; log tail: $(ssh_node "tail -20 '${_P23_LOG_PATH}' 2>/dev/null || true" 2>/dev/null | tr '\n' '|' | tail -c 800)"
        fi
    fi

    if [[ $_step96_ok -eq 1 ]]; then
        pass "Step 100: reconcile sweep removes stale subscriber" \
            "user 3 removed locally and reconcile sweep log observed"
    else
        fail "Step 100: reconcile sweep removes stale subscriber" "$_issue96"
    fi

    # Verify the canonical fixture before and after restoring subscriber count.
    _status1=$(_p23_hsi_field 1 status)
    _status2=$(_p23_hsi_field 2 status)
    [[ "$_status1" != "Data phase" ]] && _issue97="${_issue97} user1_status='${_status1:-empty}'"
    [[ "$_status2" != "Data phase" ]] && _issue97="${_issue97} user2_status='${_status2:-empty}'"

    info "Step 101: restoring canonical subscriber count and verifying HSI keys..."
    if ! _p23_restore_count; then
        _issue97="${_issue97} subscriber_count_restore_failed"
    fi

    _etcd_count=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null | \
        jq -r '.subscriber_count // empty' 2>/dev/null || true)
    _local_count=$(fastrg_grpc get_system_info 2>/dev/null | \
        jq -r '.num_users // empty' 2>/dev/null || true)
    _keys=$(ssh_node \
        "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} get --prefix --keys-only configs/${NODE_UUID}/hsi/" \
        2>/dev/null || true)
    _key_ids=$(printf '%s\n' "$_keys" | awk -F/ 'NF {print $NF}' | sort -n | tr '\n' ' ' | sed 's/ $//' || true)

    [[ "$_etcd_count" != "2" ]] && _issue97="${_issue97} etcd_count='${_etcd_count:-empty}'"
    [[ "$_local_count" != "2" ]] && _issue97="${_issue97} local_count='${_local_count:-empty}'"
    [[ "$_key_ids" != "1 2" ]] && _issue97="${_issue97} hsi_keys='${_key_ids:-empty}'"
    [[ -n "$_issue97" ]] && _step97_ok=0

    if [[ $_step97_ok -eq 1 ]]; then
        pass "Step 101: preserve and restore canonical fixture" \
            "users 1/2 remain in Data phase; etcd/local count=2; HSI keys=1,2"
    else
        fail "Step 101: preserve and restore canonical fixture" "${_issue97# }"
    fi

    _cleanup_phase23_hsi_sweep

    _p23_reconcile_checks
    return 0
}
