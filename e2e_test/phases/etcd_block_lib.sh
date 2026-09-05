#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Cutting the node off from etcd, shared by every phase that needs the node in
# offline mode. One rule and one flag for all of them, so the EXIT trap lifts
# the block no matter which phase installed it.
#
# The node's watchdog polls etcd every 60s, so a caller has to wait for the SDN
# guard to release before its direct gRPC writes are accepted.
# ---------------------------------------------------------------------------

_E2E_NODE_ETCD_BLOCKED=0

# The iptables match for the node's etcd endpoint, shared by insert and delete
# so the rule removed is always the rule installed.
_e2e_node_etcd_match() {
    printf -- '-p tcp -d %s --dport %s -j REJECT --reject-with tcp-reset' \
        "${ETCD_ENDPOINT%%:*}" "${ETCD_ENDPOINT##*:}"
}

# Block node->etcd. Idempotent: a rule left by a crashed run is removed first,
# so the block is never stacked and one -D always lifts it.
e2e_block_node_etcd() {
    local _match
    _match=$(_e2e_node_etcd_match)

    ssh_node "iptables -D OUTPUT ${_match} 2>/dev/null || true" >/dev/null 2>&1 || true
    if ssh_node "iptables -I OUTPUT 1 ${_match}" >/dev/null 2>&1; then
        _E2E_NODE_ETCD_BLOCKED=1
        return 0
    fi
    _E2E_NODE_ETCD_BLOCKED=0
    warn "failed to install the node->etcd iptables block on ${FASTRG_NODE}"
    return 1
}

# Restore node->etcd. -D is safe to call when the rule is already gone.
e2e_unblock_node_etcd() {
    ssh_node "iptables -D OUTPUT $(_e2e_node_etcd_match) 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    _E2E_NODE_ETCD_BLOCKED=0
}

# Whether this run currently holds the block; cleanup paths read it.
e2e_node_etcd_blocked() {
    [[ "${_E2E_NODE_ETCD_BLOCKED:-0}" -eq 1 ]]
}
