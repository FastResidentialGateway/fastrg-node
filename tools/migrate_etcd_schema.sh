#!/usr/bin/env bash
# =============================================================================
# migrate_etcd_schema.sh — one-time etcd migration for the control-plane refactor
#
# Brings existing etcd data to the post-refactor schema
# (see docs/contracts/etcd-schema.md):
#
#   1. HSI configs (configs/{node}/hsi/{user}):
#        - derive config.desire_status from the old metadata.enableStatus
#          (enabled/enabling -> "connect"; disabling/disabled/absent -> "disconnect")
#        - remove metadata.enableStatus (and any stray config.enable_status)
#        - preserve an already-present config.desire_status (idempotent)
#   2. Delete deprecated keys:
#        - commands/                 (PPPoE dial/hangup now driven by desire_status)
#        - failed_events/            (now Kafka -> PostgreSQL)
#        - failed_events_history/    (now Kafka -> PostgreSQL)
#
# The script is idempotent: re-running it makes no further changes.
#
# IMPORTANT: run during a maintenance window with no concurrent config writers
# (CLI/controller). It uses plain puts, not CAS — safe only when nothing else is
# writing the same keys at the same time.
#
# Usage:
#   bash migrate_etcd_schema.sh [--dry-run] [--endpoints host:port]
#
# Options:
#   --dry-run            Print what would change without writing/deleting
#   --endpoints <ep>     etcd endpoint (default: EtcdEndpoints from
#                        /etc/fastrg/config.cfg, else 127.0.0.1:2379)
# =============================================================================

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { printf "${CYAN}[INFO]${NC}  %s\n" "$*"; }
warn()  { printf "${YELLOW}[WARN]${NC}  %s\n" "$*"; }
ok()    { printf "${GREEN}[OK]${NC}    %s\n" "$*"; }
error() { printf "${RED}[ERROR]${NC} %s\n" "$*" >&2; }

DRY_RUN=0
ENDPOINTS=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)   DRY_RUN=1; shift ;;
        --endpoints) ENDPOINTS="${2:-}"; shift 2 ;;
        -h|--help)   grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) error "Unknown option: $1"; exit 1 ;;
    esac
done

# --- dependencies ---
command -v etcdctl >/dev/null 2>&1 || { error "etcdctl not found in PATH"; exit 1; }
command -v jq      >/dev/null 2>&1 || { error "jq not found in PATH"; exit 1; }

# --- etcd endpoint ---
if [[ -z "$ENDPOINTS" ]]; then
    if [[ -f /etc/fastrg/config.cfg ]]; then
        ENDPOINTS=$(awk -F'"' '/EtcdEndpoints/{print $2}' /etc/fastrg/config.cfg)
    fi
    ENDPOINTS="${ENDPOINTS:-127.0.0.1:2379}"
fi
info "etcd endpoint : ${ENDPOINTS}"
[[ "$DRY_RUN" -eq 1 ]] && warn "DRY-RUN: no writes/deletes will be performed"

ETCD() { ETCDCTL_API=3 etcdctl --endpoints="${ENDPOINTS}" "$@"; }

# --- enableStatus -> desire_status mapping (docs/contracts/etcd-schema.md) ---
map_desire() {
    case "$(printf '%s' "${1:-}" | tr '[:upper:]' '[:lower:]')" in
        enabled|enabling)   echo "connect" ;;
        disabling|disabled) echo "disconnect" ;;
        *)                  echo "disconnect" ;;
    esac
}

migrated=0; skipped=0

# ---------------------------------------------------------------------------
# 1. Migrate HSI configs
# ---------------------------------------------------------------------------
info "Scanning HSI configs (configs/*/hsi/*)..."
# Keys look like: configs/{node_uuid}/hsi/{user_id}
HSI_KEYS=$(ETCD get --prefix "configs/" --keys-only 2>/dev/null | grep -E '^configs/[^/]+/hsi/[^/]+$' || true)

while IFS= read -r key; do
    [[ -z "$key" ]] && continue
    val=$(ETCD get --print-value-only "$key" 2>/dev/null || true)
    [[ -z "$val" ]] && continue

    if ! printf '%s' "$val" | jq -e . >/dev/null 2>&1; then
        warn "SKIP (not valid JSON): ${key}"
        skipped=$((skipped + 1))
        continue
    fi

    cur_desire=$(printf '%s' "$val" | jq -r '.config.desire_status // empty')
    enable=$(printf '%s' "$val" | jq -r '.metadata.enableStatus // .config.enable_status // empty')

    if [[ -n "$cur_desire" ]]; then
        desire="$cur_desire"          # already has desire_status — preserve it
    else
        desire="$(map_desire "$enable")"
    fi

    new=$(printf '%s' "$val" | jq -c \
        --arg d "$desire" \
        '.config.desire_status = $d
         | del(.metadata.enableStatus)
         | del(.config.enable_status)')

    # idempotency: compare canonicalised old vs new
    old_norm=$(printf '%s' "$val" | jq -cS .)
    new_norm=$(printf '%s' "$new" | jq -cS .)
    if [[ "$old_norm" == "$new_norm" ]]; then
        skipped=$((skipped + 1))
        continue
    fi

    if [[ "$DRY_RUN" -eq 1 ]]; then
        info "WOULD migrate ${key}: enableStatus='${enable:-<none>}' -> desire_status='${desire}'"
    else
        printf '%s' "$new" | ETCD put "$key" >/dev/null
        ok "migrated ${key} (desire_status='${desire}', enableStatus removed)"
    fi
    migrated=$((migrated + 1))
done <<< "$HSI_KEYS"

# ---------------------------------------------------------------------------
# 2. Delete deprecated key prefixes
# ---------------------------------------------------------------------------
delete_prefix() {
    local prefix="$1"
    local cnt
    cnt=$(ETCD get --prefix "$prefix" --keys-only 2>/dev/null | grep -c . || true)
    if [[ "${cnt:-0}" -eq 0 ]]; then
        info "no keys under '${prefix}' (nothing to delete)"
        return
    fi
    if [[ "$DRY_RUN" -eq 1 ]]; then
        info "WOULD delete ${cnt} key(s) under '${prefix}'"
    else
        ETCD del --prefix "$prefix" >/dev/null
        ok "deleted ${cnt} key(s) under '${prefix}'"
    fi
}

info "Removing deprecated key prefixes..."
delete_prefix "commands/"
delete_prefix "failed_events/"
delete_prefix "failed_events_history/"

printf "\n"
info "Done. HSI configs migrated=${migrated}, skipped(up-to-date)=${skipped}."
[[ "$DRY_RUN" -eq 1 ]] && warn "DRY-RUN: no changes were made. Re-run without --dry-run to apply."
