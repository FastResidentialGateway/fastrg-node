#!/usr/bin/env bash
# =============================================================================
# restore_etcd_config.sh — Restore standard E2E test etcd config for USER_ID=2
#
# Run this script DIRECTLY on the FastRG node (192.168.10.201) when etcd
# config keys are missing and you need to restore them before running the
# E2E test suite.
#
# Usage:
#   bash restore_etcd_config.sh [--dry-run] [--force]
#
# Options:
#   --dry-run   Print the keys/values that would be written without writing
#   --force     Overwrite keys even if they already exist (default: skip)
# =============================================================================

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { printf "${CYAN}[INFO]${NC}  %s\n" "$*"; }
warn()  { printf "${YELLOW}[WARN]${NC}  %s\n" "$*"; }
ok()    { printf "${GREEN}[OK]${NC}    %s\n" "$*"; }
error() { printf "${RED}[ERROR]${NC} %s\n" "$*" >&2; }

DRY_RUN=0
FORCE=0
for _a in "$@"; do
    case "$_a" in
        --dry-run) DRY_RUN=1 ;;
        --force)   FORCE=1   ;;
        *) error "Unknown option: $_a"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Read NODE_UUID
# ---------------------------------------------------------------------------
if [[ ! -f /etc/fastrg/node_uuid ]]; then
    error "/etc/fastrg/node_uuid not found — run this script on the FastRG node"
    exit 1
fi
NODE_UUID=$(tr -d '[:space:]' < /etc/fastrg/node_uuid)
if [[ -z "$NODE_UUID" ]]; then
    error "NODE_UUID is empty"
    exit 1
fi
info "NODE_UUID: ${NODE_UUID}"

# ---------------------------------------------------------------------------
# Read ETCD_ENDPOINT from /etc/fastrg/config.cfg
# ---------------------------------------------------------------------------
if [[ ! -f /etc/fastrg/config.cfg ]]; then
    error "/etc/fastrg/config.cfg not found"
    exit 1
fi
ETCD_ENDPOINT=$(awk -F'"' '/EtcdEndpoints/{print $2}' /etc/fastrg/config.cfg)
if [[ -z "$ETCD_ENDPOINT" ]]; then
    error "Cannot parse EtcdEndpoints from /etc/fastrg/config.cfg"
    exit 1
fi
info "ETCD_ENDPOINT: ${ETCD_ENDPOINT}"

# ---------------------------------------------------------------------------
# Verify etcdctl is available
# ---------------------------------------------------------------------------
if ! command -v etcdctl >/dev/null 2>&1; then
    error "etcdctl not found in PATH"
    exit 1
fi

etcdput() {
    local key="$1" val="$2"
    local existing
    existing=$(ETCDCTL_API=3 etcdctl --endpoints="${ETCD_ENDPOINT}" \
        get --print-value-only "${key}" 2>/dev/null || true)

    if [[ -n "$existing" ]] && [[ "$FORCE" -eq 0 ]]; then
        warn "SKIP (already exists): ${key}"
        return
    fi

    if [[ "$DRY_RUN" -eq 1 ]]; then
        info "DRY-RUN put: ${key}"
        printf "  value: %s\n" "$val"
        return
    fi

    ETCDCTL_API=3 etcdctl --endpoints="${ETCD_ENDPOINT}" put "${key}" "${val}" >/dev/null
    ok "Written: ${key}"
}

# ---------------------------------------------------------------------------
# Current timestamp (RFC 3339 / UTC)
# ---------------------------------------------------------------------------
NOW=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

# ---------------------------------------------------------------------------
# KV 1 — DNS static record for USER_ID=2
# ---------------------------------------------------------------------------
KEY_DNS="configs/${NODE_UUID}/2/dns/www.google.org"
VAL_DNS='{"domain":"www.google.org","ip":"192.168.201.10","ttl":30}'

# ---------------------------------------------------------------------------
# KV 2 — HSI config for USER_ID=2
# ---------------------------------------------------------------------------
KEY_HSI="configs/${NODE_UUID}/hsi/2"
VAL_HSI=$(printf \
    '{"config":{"account_name":"admin","dhcp_addr_pool":"192.168.3.2-192.168.3.10","dhcp_gateway":"192.168.3.1","dhcp_subnet":"255.255.255.0","password":"admin","port-mapping":[{"dip":"192.168.3.2","dport":"8081","eport":"12345","index":"0"}],"user_id":"2","vlan_id":"2"},"metadata":{"enableStatus":"enabled","node":"%s","resourceVersion":"1","updatedAt":"%s","updatedBy":"restore_etcd_config"}}' \
    "${NODE_UUID}" "${NOW}")

# ---------------------------------------------------------------------------
# KV 3 — Subscriber count
# ---------------------------------------------------------------------------
KEY_COUNTS="user_counts/${NODE_UUID}/"
VAL_COUNTS=$(printf \
    '{"metadata":{"node":"%s","resourceVersion":"","updatedAt":"%s","updatedBy":"restore_etcd_config"},"subscriber_count":"2"}' \
    "${NODE_UUID}" "${NOW}")

# ---------------------------------------------------------------------------
# Write
# ---------------------------------------------------------------------------
printf "\n"
info "Restoring ${DRY_RUN:+[DRY-RUN] }etcd config (FORCE=${FORCE})..."
printf "\n"

etcdput "${KEY_DNS}"    "${VAL_DNS}"
etcdput "${KEY_HSI}"    "${VAL_HSI}"
etcdput "${KEY_COUNTS}" "${VAL_COUNTS}"

printf "\n"
info "Done."
