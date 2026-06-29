#!/usr/bin/env bash
# =============================================================================
# restore_etcd_config.sh — Restore standard E2E test config (subscribers 2 + 1)
#
# Writes through the CONTROLLER REST API (not etcd directly), so the controller
# DB history is seeded alongside etcd. This lets rollback tests find a
# "last successful" version, and keeps the controller as the single writer.
#
# Ordering matters:
#   1. subscriber count first — the node must expand its CCB pool before it
#      applies the HSI config, otherwise apply fails and the controller rolls
#      the (orphaned) config back out of etcd.
#   2. HSI config (create, fall back to update) → node applies, emits
#      CONFIG_APPLY_OK → controller DB history seeded.
#   3. PPPoE dial → desire_status=connect
#   4. DNS static record
#   5. Subscriber 1 HSI (VLAN 5) + dial — the e2e default (SUB_ID_SPEC="2,1")
#      tests this secondary subscriber too (Step 8-1).
#
# Run from the FastRG node or any host that can reach the controller REST API.
#
# Usage:
#   bash restore_etcd_config.sh [--dry-run] [--force]
#
# Environment overrides (optional):
#   CONTROLLER_REST   Controller REST base URL (default: derived from config.cfg, port 8443)
#   CONTROLLER_USER   Admin username           (default: admin)
#   CONTROLLER_PASS   Admin password           (default: admin)
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
# Controller REST settings (override via env, else derive from config.cfg)
# ---------------------------------------------------------------------------
if [[ -z "${CONTROLLER_REST:-}" ]]; then
    _ctrl_host=$(awk -F'"' '/ControllerAddress/{print $2}' /etc/fastrg/config.cfg 2>/dev/null | cut -d: -f1)
    [[ -z "$_ctrl_host" ]] && _ctrl_host="192.168.10.212"
    CONTROLLER_REST="https://${_ctrl_host}:8443"
fi
CONTROLLER_USER="${CONTROLLER_USER:-admin}"
CONTROLLER_PASS="${CONTROLLER_PASS:-admin}"
info "CONTROLLER_REST: ${CONTROLLER_REST}"

printf "\n"
_dry_label=""
[[ "$DRY_RUN" -eq 1 ]] && _dry_label="[DRY-RUN] "
info "Restoring ${_dry_label}config via controller (FORCE=${FORCE})..."
printf "\n"

if [[ "$DRY_RUN" -eq 1 ]]; then
    info "DRY-RUN: PUT  /api/nodes/${NODE_UUID}/subscriber-count {subscriber_count:2}"
    info "DRY-RUN: POST /api/config/${NODE_UUID}/hsi (user 2, vlan 3)"
    info "DRY-RUN: POST /api/pppoe/dial (user 2)"
    info "DRY-RUN: POST /api/config/${NODE_UUID}/dns/2 (www.fastrg.org)"
    info "DRY-RUN: POST /api/config/${NODE_UUID}/hsi (user 1, vlan 5)"
    info "DRY-RUN: POST /api/pppoe/dial (user 1)"
    printf "\n"; info "Done."
    exit 0
fi

# ---------------------------------------------------------------------------
# All controller calls run in one Python process (shared token + ordering).
# ---------------------------------------------------------------------------
CONTROLLER_REST="$CONTROLLER_REST" CONTROLLER_USER="$CONTROLLER_USER" \
CONTROLLER_PASS="$CONTROLLER_PASS" NODE_UUID="$NODE_UUID" FORCE="$FORCE" \
python3 - <<'PYEOF'
import os, ssl, json, time, sys, urllib.request, urllib.error

CTRL = os.environ["CONTROLLER_REST"]
NODE = os.environ["NODE_UUID"]
USER = os.environ["CONTROLLER_USER"]
PASS = os.environ["CONTROLLER_PASS"]

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

def login():
    req = urllib.request.Request(f"{CTRL}/api/login",
        data=json.dumps({"username": USER, "password": PASS}).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    return json.loads(urllib.request.urlopen(req, context=ctx, timeout=10).read())["token"]

def call(method, path, body, tok):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(f"{CTRL}{path}", data=data,
        headers={"Content-Type": "application/json", "Authorization": tok}, method=method)
    try:
        r = urllib.request.urlopen(req, context=ctx, timeout=10)
        return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")

try:
    tok = login()
except Exception as e:
    print(f"\033[0;31m[ERROR]\033[0m login to controller failed: {e}", file=sys.stderr)
    sys.exit(1)
print("\033[0;32m[OK]\033[0m    controller login")

# 1. subscriber count first so the node expands its CCB pool before hsi apply
st, body = call("PUT", f"/api/nodes/{NODE}/subscriber-count", {"subscriber_count": 2}, tok)
if st != 200:
    print(f"\033[0;31m[ERROR]\033[0m set subscriber-count: HTTP {st} {body[:160]}", file=sys.stderr)
    sys.exit(1)
print("\033[0;32m[OK]\033[0m    subscriber_count=2")
time.sleep(3)

# 2. HSI config for user 2 — create, fall back to update if it exists
hsi = {
    "user_id": "2", "vlan_id": "3", "account_name": "the", "password": "admin",
    "dhcp_addr_pool": "192.168.4.2-192.168.4.10",
    "dhcp_subnet": "255.255.255.0", "dhcp_gateway": "192.168.4.1",
    # Controller HSIConfig expects the hyphenated "port-mapping" key with
    # index/dip/dport/eport fields; an underscore key is silently dropped, which
    # leaves etcd with no port-mapping and makes the DNAT test (Step 16) skip.
    "port-mapping": [{"index": "0", "dip": "192.168.4.2", "dport": "8081", "eport": "12345"}],
}
st, body = call("POST", f"/api/config/{NODE}/hsi", hsi, tok)
if st == 409 or "exist" in body.lower():
    st, body = call("PUT", f"/api/config/{NODE}/hsi/2", hsi, tok)
if st != 200:
    print(f"\033[0;31m[ERROR]\033[0m apply hsi/2: HTTP {st} {body[:160]}", file=sys.stderr)
    sys.exit(1)
print("\033[0;32m[OK]\033[0m    configs/" + NODE + "/hsi/2")
time.sleep(2)

# 3. desire_status=connect via dial
st, body = call("POST", "/api/pppoe/dial", {"node_id": NODE, "user_id": "2"}, tok)
if st != 200:
    print(f"\033[1;33m[WARN]\033[0m  dial user 2: HTTP {st} {body[:120]}", file=sys.stderr)
else:
    print("\033[0;32m[OK]\033[0m    desire_status=connect (dial)")

# 4. DNS static record
dns = {"domain": "www.fastrg.org", "ip": "192.168.201.11", "ttl": 30}
st, body = call("POST", f"/api/config/{NODE}/dns/2", dns, tok)
if st not in (200, 409):
    print(f"\033[1;33m[WARN]\033[0m  add dns: HTTP {st} {body[:120]}", file=sys.stderr)
else:
    print("\033[0;32m[OK]\033[0m    configs/" + NODE + "/dns/2")

# 5. HSI config for user 1 — secondary subscriber on VLAN 5 (the BRAS serves
#    vlans 3,5). The e2e default SUB_ID_SPEC="2,1" dials this as Step 8-1; without
#    it that step fails with ppp_phase='not configured'.
hsi1 = {
    "user_id": "1", "vlan_id": "5", "account_name": "the", "password": "admin",
    "dhcp_addr_pool": "192.168.5.2-192.168.5.10",
    "dhcp_subnet": "255.255.255.0", "dhcp_gateway": "192.168.5.1",
}
st, body = call("POST", f"/api/config/{NODE}/hsi", hsi1, tok)
if st == 409 or "exist" in body.lower():
    st, body = call("PUT", f"/api/config/{NODE}/hsi/1", hsi1, tok)
if st != 200:
    print(f"\033[0;31m[ERROR]\033[0m apply hsi/1: HTTP {st} {body[:160]}", file=sys.stderr)
    sys.exit(1)
print("\033[0;32m[OK]\033[0m    configs/" + NODE + "/hsi/1")
time.sleep(2)

# 6. desire_status=connect via dial for user 1
st, body = call("POST", "/api/pppoe/dial", {"node_id": NODE, "user_id": "1"}, tok)
if st != 200:
    print(f"\033[1;33m[WARN]\033[0m  dial user 1: HTTP {st} {body[:120]}", file=sys.stderr)
else:
    print("\033[0;32m[OK]\033[0m    desire_status=connect (dial user 1)")
PYEOF

printf "\n"
info "Done."
