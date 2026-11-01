#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Cutting the node off from etcd, shared by every phase that needs the node in
# offline mode. One rule and one flag for all of them, so the EXIT trap lifts
# the block no matter which phase installed it.
#
# Two modes, with different meanings for the node:
#
#   mode     rules                    node's etcd connection while blocked
#   default  OUTPUT REJECT tcp-reset  reset the next time the node sends on it
#   both     + INPUT DROP             stays ESTABLISHED: the reset is dropped.
#                                     etcd gives up its end after ~2.5 min of
#                                     unanswered keepalives; the node learns
#                                     that only once the block is lifted
#
#   mode     etcd events written while blocked  how the node notices
#   default  missed; the reconcile after the    its watch is reset, so it
#            reconnect brings them in           starts reconnecting at once
#   both     retransmitted on the same watch    a failed watchdog GET (60s
#            once lifted while etcd still       tick, 5s timeout); after >180s
#            holds the connection, else the     without watch activity it
#            reconcile after the reconnect      retries, still on the same
#            brings them in                     connection
#
# Default is a clean cut: a caller that needs the node to miss an event uses it
# and waits for e2e_wait_node_etcd_down before writing. "both" is a silent
# outage (a cut fibre): use it to test how the node finds out on its own.
#
# Direct gRPC writes are refused until the node marks etcd unreachable: as it
# enters reconnect in default mode, on the first failed watchdog tick (<= 65s
# after the block) with "both".
# ---------------------------------------------------------------------------

_E2E_NODE_ETCD_BLOCKED=0
_E2E_NODE_ETCD_BLOCKED_IN=0

# The iptables match for the node's etcd endpoint, shared by insert and delete
# so the rule removed is always the rule installed.
_e2e_node_etcd_match() {
    printf -- '-p tcp -d %s --dport %s -j REJECT --reject-with tcp-reset' \
        "${ETCD_ENDPOINT%%:*}" "${ETCD_ENDPOINT##*:}"
}

# The same for what etcd sends back. DROP also eats the reset the OUTPUT rule
# sends the node, so its connection stays up with no error.
_e2e_node_etcd_match_in() {
    printf -- '-p tcp -s %s --sport %s -j DROP' \
        "${ETCD_ENDPOINT%%:*}" "${ETCD_ENDPOINT##*:}"
}

# Block node->etcd. Idempotent: a rule left by a crashed run is removed first,
# so the block is never stacked and one -D always lifts it.
#
# e2e_block_node_etcd [both] — "both" also drops what etcd sends the node.
e2e_block_node_etcd() {
    local _match _match_in _err=""

    _match=$(_e2e_node_etcd_match)
    _match_in=$(_e2e_node_etcd_match_in)

    # One ssh for both rules: INPUT DROP must not stay up alone.
    # A failed OUTPUT insert also removes the INPUT rule.
    if [[ "${1:-}" == "both" ]]; then
        if _err=$(ssh_node "iptables -D INPUT ${_match_in} 2>/dev/null; \
                iptables -I INPUT 1 ${_match_in} || exit 1; \
                iptables -D OUTPUT ${_match} 2>/dev/null; \
                iptables -I OUTPUT 1 ${_match} || \
                { iptables -D INPUT ${_match_in}; exit 1; }" 2>&1); then
            _E2E_NODE_ETCD_BLOCKED_IN=1
            _E2E_NODE_ETCD_BLOCKED=1
            return 0
        fi
        _E2E_NODE_ETCD_BLOCKED_IN=0
        _E2E_NODE_ETCD_BLOCKED=0
        warn "failed to install the two-way etcd iptables block on ${FASTRG_NODE}: ${_err}"
        return 1
    fi

    ssh_node "iptables -D OUTPUT ${_match} 2>/dev/null || true" >/dev/null 2>&1 || true
    if ssh_node "iptables -I OUTPUT 1 ${_match}" >/dev/null 2>&1; then
        _E2E_NODE_ETCD_BLOCKED=1
        return 0
    fi
    _E2E_NODE_ETCD_BLOCKED=0
    warn "failed to install the node->etcd iptables block on ${FASTRG_NODE}"
    return 1
}

# Restore node->etcd. -D is safe to call when a rule is already gone, so this
# lifts both directions whichever one was installed.
e2e_unblock_node_etcd() {
    ssh_node "iptables -D OUTPUT $(_e2e_node_etcd_match) 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    ssh_node "iptables -D INPUT $(_e2e_node_etcd_match_in) 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    _E2E_NODE_ETCD_BLOCKED=0
    _E2E_NODE_ETCD_BLOCKED_IN=0
}

# Whether this run currently holds the block; cleanup paths read it.
e2e_node_etcd_blocked() {
    [[ "${_E2E_NODE_ETCD_BLOCKED:-0}" -eq 1 ]]
}

# Whether `ss -Hnt state established` lists a connection to HOST:PORT. gRPC
# opens dual-stack sockets, so the peer may read [::ffff:HOST]:PORT.
#
# Prints up | down.
e2e_etcd_connection_up() {
    local _body="${1-}" _host="${2-}" _port="${3-}"

    if awk -v v4="${_host}:${_port}" -v mapped="[::ffff:${_host}]:${_port}" \
            '$4 == v4 || $4 == mapped { found = 1 } END { exit !found }' <<< "$_body"; then
        printf 'up'
        return 0
    fi
    printf 'down'
    return 1
}

local_validation_register etcd_connection_up e2e_etcd_connection_up \
    etcd_connection_up_mapped \
    etcd_connection_up_v4 \
    etcd_connection_up_other_port \
    etcd_connection_up_local_side \
    etcd_connection_up_none

# Wait up to MAX_SECS (default 90) until the node holds no established
# connection to etcd. Prints the seconds waited; non-zero on timeout.
e2e_wait_node_etcd_down() {
    local _max="${1:-90}" _waited=0 _established=""

    while true; do
        if _established=$(ssh_node "ss -Hnt state established" 2>/dev/null) && \
                ! e2e_etcd_connection_up "$_established" \
                    "${ETCD_ENDPOINT%%:*}" "${ETCD_ENDPOINT##*:}" >/dev/null; then
            printf '%s' "$_waited"
            return 0
        fi
        [[ "$_waited" -lt "$_max" ]] || break
        sleep 2
        _waited=$(( _waited + 2 ))
    done
    printf '%s' "$_waited"
    return 1
}

# The local port of the first connection to HOST:PORT that
# `ss -Hnt state established` lists; the peer is matched as in
# e2e_etcd_connection_up.
#
# Prints the port, or none.
e2e_etcd_connection_sport() {
    local _body="${1-}" _host="${2-}" _port="${3-}" _sport=""

    _sport=$(awk -v v4="${_host}:${_port}" -v mapped="[::ffff:${_host}]:${_port}" '
        $4 == v4 || $4 == mapped { n = split($3, a, ":"); print a[n]; exit }' <<< "$_body")
    if [[ "$_sport" =~ ^[0-9]+$ ]]; then
        printf '%s' "$_sport"
        return 0
    fi
    printf 'none'
    return 1
}

local_validation_register etcd_connection_sport e2e_etcd_connection_sport \
    etcd_connection_sport_v4 \
    etcd_connection_sport_mapped \
    etcd_connection_sport_two \
    etcd_connection_sport_none \
    etcd_connection_sport_local_side

# e2e_etcd_connection_sport on the node's sockets, narrowed by an ss FILTER
# (e.g. "sport = :45344"). Prints "err" when ss could not be read.
_e2e_node_etcd_sport() {
    local _body=""

    if ! _body=$(ssh_node "ss -Hnt state established${1:+ '( $1 )'}" 2>/dev/null); then
        printf 'err'
        return 1
    fi
    e2e_etcd_connection_sport "$_body" "${ETCD_ENDPOINT%%:*}" "${ETCD_ENDPOINT##*:}"
}

# Wait up to MAX_SECS (default 100) until the node's etcd connection comes
# from a port other than OLD_PORT and OLD_PORT's is gone; an empty OLD_PORT
# takes any connection. Prints the seconds waited; non-zero on timeout.
e2e_wait_node_etcd_conn_changed() {
    local _old="${1-}" _max="${2:-100}" _start=$SECONDS _waited=0 _now=""

    [[ -z "$_old" || "$_old" =~ ^[0-9]+$ ]] || { printf '0'; return 1; }
    while true; do
        _waited=$(( SECONDS - _start ))
        _now=$(_e2e_node_etcd_sport) || true
        if [[ "$_now" =~ ^[0-9]+$ && "$_now" != "$_old" ]] && { [[ -z "$_old" ]] || \
                [[ "$(_e2e_node_etcd_sport "sport = :${_old}")" == "none" ]]; }; then
            printf '%s' "$_waited"
            return 0
        fi
        [[ "$_waited" -lt "$_max" ]] || break
        sleep 2
    done
    printf '%s' "$_waited"
    return 1
}
