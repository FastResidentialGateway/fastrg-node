#!/usr/bin/env bash
# =============================================================================
# FastRG Node — End-to-End Data Plane Test Script
#
# Usage:
#   ./run_e2e_test.sh <USER_ID> [OPTIONS]
#
# Options:
#   --fastrg-node  IP     FastRG node IP         (default: 192.168.10.201)
#   --lan-host     IP     LAN-side host IP        (default: 192.168.10.210)
#   --wan-host     IP     WAN-side host IP        (default: 192.168.10.106)
#   --wan-ip       IP     WAN subscriber IP       (default: 192.168.201.10)
#   --runner-host  IP     E2E runner host IP      (default: 192.168.10.207)
#   --ssh-key      PATH   SSH identity file       (default: auto-detect id_ed25519 or id_rsa)
#   --help                Show this help
#
# Requirements (local machine):
#   - jq
#   - ssh / scp
#
# Requirements (remote hosts):
#   - FastRG node: etcdctl
#   - WAN host:    iperf3, python3 + scapy
#   - LAN host:    ping, iperf3, curl, tcpdump
# =============================================================================

# ---------------------------------------------------------------------------
# Self-relocation — the runner must have an user called "the".
# If invoked from any other machine it uploads itself + companion files and
# re-executes there.
# Set _FASTRG_E2E_RELOCATED=1 to skip this check (set automatically on relay).
# ---------------------------------------------------------------------------
# Allow --runner-host to override the default before argument parsing runs.
# We do a quick pre-scan of $@ here so the self-relocation block can use it.
_E2E_RUNNER_HOST="192.168.10.207"
for _arg in "$@"; do
    if [[ "$_arg" == --runner-host=* ]]; then
        _E2E_RUNNER_HOST="${_arg#--runner-host=}"
    fi
done
# Also support the two-token form: --runner-host <IP>
_prev=""
for _arg in "$@"; do
    if [[ "$_prev" == "--runner-host" ]]; then
        _E2E_RUNNER_HOST="$_arg"
    fi
    _prev="$_arg"
done
unset _prev _arg
_E2E_RUNNER_USER="the"
_E2E_REMOTE_DIR='~/fastrg_e2e_test'
_E2E_REMOTE_PATH="${_E2E_REMOTE_DIR}/run_e2e_test.sh"

if [[ -z "${_FASTRG_E2E_RELOCATED:-}" ]]; then
    # Collect local IPs — hostname -I on Linux, ifconfig on macOS
    _my_ips=$(hostname -I 2>/dev/null || \
              ifconfig 2>/dev/null | awk '/inet /{gsub(/addr:/,"",$2); print $2}')
    if ! printf '%s\n' $_my_ips | grep -qx "${_E2E_RUNNER_HOST}"; then
        printf '[INFO]  Not running on %s — uploading files and re-executing remotely...\n' \
               "${_E2E_RUNNER_HOST}"
        _SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10"
        _SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
        _REPO_ROOT="$(cd "${_SCRIPT_DIR}/.." && pwd)"

        # Ensure remote directory exists
        ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" "mkdir -p ${_E2E_REMOTE_DIR}"

        # Upload this script to the remote runner host
        printf '[INFO]  Uploading run_e2e_test.sh...\n'
        scp $_SSH_OPTS "$0" "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_PATH}"

        # Upload gRPC Python client
        if [[ -f "${_SCRIPT_DIR}/fastrg_grpc_client.py" ]]; then
            printf '[INFO]  Uploading fastrg_grpc_client.py...\n'
            scp $_SSH_OPTS "${_SCRIPT_DIR}/fastrg_grpc_client.py" \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/fastrg_grpc_client.py"
        else
            printf '[WARN]  fastrg_grpc_client.py not found at %s\n' "${_SCRIPT_DIR}/fastrg_grpc_client.py"
        fi

        # Upload proto file (needed by fastrg_grpc_client.py at runtime via grpcurl)
        _PROTO_SRC="${_REPO_ROOT}/northbound/grpc/fastrg_node.proto"
        if [[ -f "${_PROTO_SRC}" ]]; then
            printf '[INFO]  Uploading fastrg_node.proto...\n'
            scp $_SSH_OPTS "${_PROTO_SRC}" \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/fastrg_node.proto"
        else
            printf '[WARN]  proto not found at %s\n' "${_PROTO_SRC}"
        fi

        # Upload grpcurl binary (standalone static binary — no pip/install needed on runner)
        _GRPCURL_BIN=$(command -v grpcurl 2>/dev/null || true)
        if [[ -n "$_GRPCURL_BIN" ]]; then
            printf '[INFO]  Uploading grpcurl binary...\n'
            scp $_SSH_OPTS "${_GRPCURL_BIN}" \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/grpcurl"
            ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                "chmod +x ${_E2E_REMOTE_DIR}/grpcurl"
        else
            printf '[WARN]  grpcurl not found on local machine — runner must have grpcurl in PATH\n'
        fi

        # Rebuild quoted arg list to forward all original arguments
        _remote_args=""
        for _a in "$@"; do _remote_args="${_remote_args} '${_a}'"; done
        ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
            "chmod +x ${_E2E_REMOTE_PATH} && _FASTRG_E2E_RELOCATED=1 ${_E2E_REMOTE_PATH}${_remote_args}"
        _ssh_rc=$?
        exit $_ssh_rc
    fi
fi

set -euo pipefail

# Ensure common tool locations are in PATH (needed for macOS SSH non-login shells)
export PATH="/usr/local/bin:/usr/local/sbin:${PATH}"

# ---------------------------------------------------------------------------
# Colour helpers (printf-based, portable macOS/Linux)
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

info()  { printf "${CYAN}[INFO]${NC}  %s\n" "$*"; }
warn()  { printf "${YELLOW}[WARN]${NC}  %s\n" "$*"; }
error() { printf "${RED}[ERROR]${NC} %s\n" "$*" >&2; }
bold()  { printf "${BOLD}%s${NC}\n" "$*"; }

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
FASTRG_NODE="192.168.10.201"
LAN_HOST="192.168.10.210"
WAN_HOST="192.168.10.106"
WAN_IP="192.168.201.10"
# Auto-detect SSH key: prefer id_ed25519, fall back to id_rsa
if [[ -f "${HOME}/.ssh/id_ed25519" ]]; then
    SSH_KEY="${HOME}/.ssh/id_ed25519"
else
    SSH_KEY="${HOME}/.ssh/id_rsa"
fi
FASTRG_GRPC_PORT="50052"   # fastrg gRPC TCP port (NodeGrpcPort in config.cfg)
GRPC_CLIENT_DIR="$(cd "$(dirname "$0")" && pwd)"  # directory of fastrg_grpc_client.py
USER_ID=""

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
usage() {
    # Print only the header block: from line 1 up to (and including) the closing ===== line
    awk '/^# =+$/{found++} found==1{sub(/^# ?/,""); print} found==2{exit}' "$0"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h)       usage ;;
        --fastrg-node)   FASTRG_NODE="$2"; shift 2 ;;
        --lan-host)      LAN_HOST="$2";    shift 2 ;;
        --wan-host)      WAN_HOST="$2";    shift 2 ;;
        --wan-ip)        WAN_IP="$2";      shift 2 ;;
        --runner-host)   _E2E_RUNNER_HOST="$2"; shift 2 ;;
        --ssh-key)       SSH_KEY="$2";     shift 2 ;;
        --grpc-port)     FASTRG_GRPC_PORT="$2"; shift 2 ;;
        -*)              error "Unknown option: $1"; exit 1 ;;
        *)
            if [[ -z "$USER_ID" ]]; then
                USER_ID="$1"
            else
                error "Unexpected argument: $1"; exit 1
            fi
            shift ;;
    esac
done

if [[ -z "$USER_ID" ]]; then
    error "USER_ID is required."
    printf "Usage: %s <USER_ID> [--options]\n" "$0"
    printf "Run '%s --help' for full usage.\n" "$0"
    exit 1
fi

# ---------------------------------------------------------------------------
# SSH helper functions
# ---------------------------------------------------------------------------
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes -i ${SSH_KEY}"

ssh_node() { ssh $SSH_OPTS "root@${FASTRG_NODE}" "$@"; }
ssh_lan()  { ssh $SSH_OPTS "the@${LAN_HOST}"    "$@"; }
ssh_wan()  { ssh $SSH_OPTS "root@${WAN_HOST}"   "$@"; }

# ---------------------------------------------------------------------------
# Test result tracking (indexed arrays — bash 3.2 compatible)
# ---------------------------------------------------------------------------
STEP_NAMES=()
STEP_RESULTS=()   # "PASS" | "FAIL" | "SKIP"
STEP_DETAILS=()

record_result() {
    local name="$1" result="$2" detail="${3:-}"
    STEP_NAMES+=("$name")
    STEP_RESULTS+=("$result")
    STEP_DETAILS+=("$detail")
}

pass() {
    local name="$1" detail="${2:-}"
    printf "  ${GREEN}[PASS]${NC} %s\n" "$name"
    [[ -n "$detail" ]] && printf "         %s\n" "$detail"
    record_result "$name" "PASS" "$detail"
}

fail() {
    local name="$1" detail="${2:-}"
    printf "  ${RED}[FAIL]${NC} %s\n" "$name"
    [[ -n "$detail" ]] && printf "         %s\n" "$detail"
    record_result "$name" "FAIL" "$detail"
}

skip() {
    local name="$1" detail="${2:-}"
    printf "  ${YELLOW}[SKIP]${NC} %s\n" "$name"
    [[ -n "$detail" ]] && printf "         %s\n" "$detail"
    record_result "$name" "SKIP" "$detail"
}

# ---------------------------------------------------------------------------
# Phase 0 — Prerequisites
# ---------------------------------------------------------------------------
phase0_setup() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 0 — Prerequisite Checks"
    bold "═══════════════════════════════════════════════════════"

    # Check local jq
    if ! command -v jq >/dev/null 2>&1; then
        error "jq is not installed on this machine. Please install jq first."
        exit 1
    fi
    info "Local jq: $(jq --version)"

    # Check SSH key
    if [[ ! -f "$SSH_KEY" ]]; then
        error "SSH key not found: ${SSH_KEY}"
        exit 1
    fi

    # Check SSH connectivity to FastRG node
    info "Checking SSH connectivity to FastRG node (${FASTRG_NODE})..."
    if ! ssh_node "true" 2>/dev/null; then
        error "Cannot SSH to FastRG node at ${FASTRG_NODE}"
        exit 1
    fi
    info "FastRG node reachable."

    # Read NODE_UUID
    info "Reading NODE_UUID from /etc/fastrg/node_uuid..."
    NODE_UUID=$(ssh_node "cat /etc/fastrg/node_uuid" 2>/dev/null | tr -d '[:space:]')
    if [[ -z "$NODE_UUID" ]]; then
        error "Failed to read NODE_UUID from /etc/fastrg/node_uuid on ${FASTRG_NODE}"
        exit 1
    fi
    info "NODE_UUID: ${NODE_UUID}"

    # Read ETCD_ENDPOINT from /etc/fastrg/config.cfg
    info "Reading ETCD_ENDPOINT from /etc/fastrg/config.cfg..."
    ETCD_RAW=$(ssh_node "grep 'EtcdEndpoints' /etc/fastrg/config.cfg" 2>/dev/null || true)
    ETCD_ENDPOINT=$(printf '%s' "$ETCD_RAW" | awk -F'"' '{print $2}')
    if [[ -z "$ETCD_ENDPOINT" ]]; then
        error "Failed to parse EtcdEndpoints from /etc/fastrg/config.cfg"
        exit 1
    fi
    info "ETCD_ENDPOINT: ${ETCD_ENDPOINT}"

    # Read FASTRG_GRPC_PORT from node config (may override default)
    _grpc_port_raw=$(ssh_node "grep 'NodeGrpcPort' /etc/fastrg/config.cfg 2>/dev/null" | awk -F'"' '{print $2}')
    if [[ -n "$_grpc_port_raw" ]]; then
        FASTRG_GRPC_PORT="$_grpc_port_raw"
    fi
    info "FASTRG_GRPC_PORT: ${FASTRG_GRPC_PORT}"

    # Export for use in all subsequent functions
    export NODE_UUID ETCD_ENDPOINT FASTRG_GRPC_PORT

    # ------------------------------------------------------------------
    # Check Python3 + grpcurl + proto (fastrg_grpc_client.py uses grpcurl;
    # no grpcio or pb2 stubs required)
    # ------------------------------------------------------------------
    info "Checking Python3..."
    if ! command -v python3 >/dev/null 2>&1; then
        error "python3 is required but not found. Please install python3."
        exit 1
    fi
    if [[ ! -f "${GRPC_CLIENT_DIR}/fastrg_grpc_client.py" ]]; then
        error "fastrg_grpc_client.py not found in ${GRPC_CLIENT_DIR}"
        exit 1
    fi
    info "Python3 gRPC client: ${GRPC_CLIENT_DIR}/fastrg_grpc_client.py"

    # Check grpcurl binary (bundled alongside script takes priority)
    if [[ -f "${GRPC_CLIENT_DIR}/grpcurl" ]] && [[ -x "${GRPC_CLIENT_DIR}/grpcurl" ]]; then
        info "grpcurl: ${GRPC_CLIENT_DIR}/grpcurl"
    elif command -v grpcurl >/dev/null 2>&1; then
        info "grpcurl: $(command -v grpcurl)"
    else
        error "grpcurl not found. Upload it alongside this script or install it in PATH."
        exit 1
    fi

    # Check proto file (needed by grpcurl at runtime)
    if [[ ! -f "${GRPC_CLIENT_DIR}/fastrg_node.proto" ]]; then
        error "fastrg_node.proto not found in ${GRPC_CLIENT_DIR}"
        error "Re-run the script from the repo root to allow automatic proto upload"
        exit 1
    fi
    info "proto: ${GRPC_CLIENT_DIR}/fastrg_node.proto"

    # ------------------------------------------------------------------
    # Ensure fastrg daemon is running; start it if not
    # ------------------------------------------------------------------
    info "Checking if fastrg daemon is running on ${FASTRG_NODE}..."
    FASTRG_PID=$(ssh_node "pgrep -x fastrg 2>/dev/null || pidof fastrg 2>/dev/null || true" | tr -d '[:space:]')
    if [[ -z "$FASTRG_PID" ]]; then
        warn "fastrg is NOT running — attempting to start..."
        _FASTRG_DAEMON="/root/fastrg-node/fastrg"
        _FASTRG_START_CMD="${_FASTRG_DAEMON} -l 1-9 -n 4 -a 0000:04:00.0 -a 0000:08:00.0"
        info "Starting: ${_FASTRG_START_CMD}"
        ssh_node "nohup ${_FASTRG_START_CMD} >/var/log/fastrg.log 2>&1 &"

        # Wait up to 120 s for fastrg gRPC + HSI data for USER_ID to be ready
        # (including PPPoE session establishment — account must be non-empty)
        info "Waiting for fastrg gRPC + HSI data for USER_ID=${USER_ID} to be ready (up to 120s)..."
        _ready=0
        for _i in $(seq 1 24); do
            sleep 5
            _hsi=$(python3 "${GRPC_CLIENT_DIR}/fastrg_grpc_client.py" \
                      --node "${FASTRG_NODE}:${FASTRG_GRPC_PORT}" \
                      get_hsi_info 2>/dev/null || true)
            # Wait for: user exists in hsi_infos AND account is non-empty (PPPoE session up)
            if printf '%s' "$_hsi" | python3 -c \
                "import sys,json; d=json.load(sys.stdin); \
                 infos=d.get('hsi_infos',[]); \
                 u=[h for h in infos if h.get('user_id')==${USER_ID}]; \
                 sys.exit(0 if u and u[0].get('account','') else 1)" \
                2>/dev/null; then
                _ready=1
                break
            fi
            _cnt=$(printf '%s' "$_hsi" | python3 -c \
                'import sys,json; d=json.load(sys.stdin); print(len(d.get("hsi_infos",[])))' \
                2>/dev/null || echo '?')
            _acct=$(printf '%s' "$_hsi" | python3 -c \
                "import sys,json; d=json.load(sys.stdin); \
                 u=[h for h in d.get('hsi_infos',[]) if h.get('user_id')==${USER_ID}]; \
                 print(u[0].get('account','') if u else '')" \
                2>/dev/null || echo '')
            info "  still waiting... (${_i}×5s, hsi_infos=${_cnt}, account='${_acct}')"
        done
        if [[ $_ready -eq 0 ]]; then
            error "fastrg gRPC did not become ready within 120 seconds."
            info "Last log output:"
            ssh_node "tail -20 /var/log/fastrg.log 2>/dev/null || true"
            exit 1
        fi

        # Additionally wait for DNS static records to be loaded (needed for step 2d)
        _dns_ready=0
        for _i in $(seq 1 12); do
            _dns=$(python3 "${GRPC_CLIENT_DIR}/fastrg_grpc_client.py" \
                      --node "${FASTRG_NODE}:${FASTRG_GRPC_PORT}" \
                      get_dns_static "${USER_ID}" 2>/dev/null || true)
            if printf '%s' "$_dns" | python3 -c \
                "import sys,json; d=json.load(sys.stdin); \
                 sys.exit(0 if d.get('total_entries',0) > 0 or d.get('entries') else 1)" \
                2>/dev/null; then
                _dns_ready=1
                break
            fi
            info "  waiting for DNS static records... (${_i}×5s)"
            sleep 5
        done
        [[ $_dns_ready -eq 0 ]] && info "  (DNS static not loaded yet — will verify in step 2d)"

        info "fastrg gRPC is ready."
        _FASTRG_STARTED_BY_SCRIPT=1
    else
        info "fastrg is running (pid: ${FASTRG_PID})."
        _FASTRG_STARTED_BY_SCRIPT=0
        # Verify gRPC is reachable
        _sys=$(python3 "${GRPC_CLIENT_DIR}/fastrg_grpc_client.py" \
                  --node "${FASTRG_NODE}:${FASTRG_GRPC_PORT}" \
                  get_system_info 2>/dev/null || true)
        if ! printf '%s' "$_sys" | python3 -c "import sys,json; json.load(sys.stdin)" 2>/dev/null; then
            error "fastrg is running but gRPC on ${FASTRG_NODE}:${FASTRG_GRPC_PORT} is not responding."
            exit 1
        fi
        info "fastrg gRPC is reachable."
    fi

    printf "\n"
    info "Configuration summary:"
    printf "  USER_ID         : %s\n" "$USER_ID"
    printf "  FASTRG_NODE     : %s\n" "$FASTRG_NODE"
    printf "  FASTRG_GRPC     : %s:%s\n" "$FASTRG_NODE" "$FASTRG_GRPC_PORT"
    printf "  LAN_HOST        : %s (user: the)\n" "$LAN_HOST"
    printf "  WAN_HOST        : %s\n" "$WAN_HOST"
    printf "  WAN_IP          : %s\n" "$WAN_IP"
    printf "  NODE_UUID       : %s\n" "$NODE_UUID"
    printf "  ETCD_ENDPOINT   : %s\n" "$ETCD_ENDPOINT"
    printf "\n"
}

# ---------------------------------------------------------------------------
# etcdctl wrapper — runs on FastRG node
# ---------------------------------------------------------------------------
etcdctl_get() {
    ssh_node "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} get $*"
}

etcdctl_get_value() {
    # etcdctl prints key on one line, value on next; we want only the value
    ssh_node "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} get --print-value-only $*"
}

# ---------------------------------------------------------------------------
# fastrg_grpc — call FastRG gRPC server directly via Python3 client
# Returns JSON on stdout; empty string on error.
# ---------------------------------------------------------------------------
fastrg_grpc() {
    python3 "${GRPC_CLIENT_DIR}/fastrg_grpc_client.py" \
        --node "${FASTRG_NODE}:${FASTRG_GRPC_PORT}" \
        "$@" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Phase 1 — etcd Config Sync
# ---------------------------------------------------------------------------
phase1_etcd_config_sync() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 1 — etcd Config Sync (Steps 1–2)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 1 — etcd key exists for this subscriber
    # ------------------------------------------------------------------
    info "Step 1: Checking etcd HSI config key exists for USER_ID=${USER_ID}..."
    HSI_JSON=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
    if [[ -z "$HSI_JSON" ]]; then
        fail "Step 1: etcd HSI key" "Key configs/${NODE_UUID}/hsi/${USER_ID} not found or empty"
        warn "Skipping Steps 2a-2d (no etcd data to compare against)"
        skip "Step 2a: PPPoE config match" "No etcd data"
        skip "Step 2b: DHCP config match"  "No etcd data"
        skip "Step 2c: Port-mapping match" "No etcd data"
        skip "Step 2d: DNS static match"   "No etcd data"
        return
    fi
    pass "Step 1: etcd HSI key" "configs/${NODE_UUID}/hsi/${USER_ID} exists"

    # Parse etcd JSON fields (nested: .config.* and .metadata.*)
    ETCD_ACCOUNT=$(printf '%s' "$HSI_JSON"   | jq -r '.config.account_name // empty')
    ETCD_VLAN=$(printf '%s' "$HSI_JSON"      | jq -r '.config.vlan_id // empty')
    ETCD_POOL=$(printf '%s' "$HSI_JSON"      | jq -r '.config.dhcp_addr_pool // empty')
    ETCD_SUBNET=$(printf '%s' "$HSI_JSON"    | jq -r '.config.dhcp_subnet // empty')
    ETCD_GATEWAY=$(printf '%s' "$HSI_JSON"   | jq -r '.config.dhcp_gateway // empty')
    ETCD_DNS_PRI=$(printf '%s' "$HSI_JSON"   | jq -r '.config.dns_primary // empty')
    ETCD_DNS_SEC=$(printf '%s' "$HSI_JSON"   | jq -r '.config.dns_secondary // empty')

    info "  etcd account_name : ${ETCD_ACCOUNT}"
    info "  etcd vlan_id      : ${ETCD_VLAN}"
    info "  etcd dhcp_pool    : ${ETCD_POOL}"
    info "  etcd dhcp_subnet  : ${ETCD_SUBNET}"
    info "  etcd dhcp_gateway : ${ETCD_GATEWAY}"

    # ------------------------------------------------------------------
    # Step 2a — PPPoE config loaded into fastrg
    # ------------------------------------------------------------------
    info "Step 2a: Comparing PPPoE config (gRPC GetFastrgHsiInfo vs etcd)..."
    HSI_GRPC=$(fastrg_grpc get_hsi_info)
    HSI_USER=$(printf '%s' "$HSI_GRPC" | jq -r ".hsi_infos[] | select(.user_id == ${USER_ID})" 2>/dev/null || true)

    if [[ -z "$HSI_USER" ]]; then
        fail "Step 2a: PPPoE config match" "User ID ${USER_ID} not found in gRPC GetFastrgHsiInfo response"
    else
        CLI_ACCOUNT=$(printf '%s' "$HSI_USER" | jq -r '.account // empty')
        CLI_VLAN=$(printf '%s'    "$HSI_USER" | jq -r '.vlan_id // empty')

        MISMATCH=""
        [[ "$CLI_ACCOUNT" != "$ETCD_ACCOUNT" ]] && MISMATCH="${MISMATCH} account(grpc=${CLI_ACCOUNT} etcd=${ETCD_ACCOUNT})"
        [[ "$CLI_VLAN"    != "$ETCD_VLAN"    ]] && MISMATCH="${MISMATCH} vlan(grpc=${CLI_VLAN} etcd=${ETCD_VLAN})"

        if [[ -z "$MISMATCH" ]]; then
            pass "Step 2a: PPPoE config match" "account=${CLI_ACCOUNT} vlan=${CLI_VLAN}"
        else
            fail "Step 2a: PPPoE config match" "Mismatch:${MISMATCH}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 2b — DHCP config
    # ------------------------------------------------------------------
    info "Step 2b: Comparing DHCP config (gRPC GetFastrgDhcpInfo vs etcd)..."
    DHCP_GRPC=$(fastrg_grpc get_dhcp_info)
    DHCP_USER=$(printf '%s' "$DHCP_GRPC" | jq -r ".dhcp_infos[] | select(.user_id == ${USER_ID})" 2>/dev/null || true)

    if [[ -z "$DHCP_USER" ]]; then
        fail "Step 2b: DHCP config match" "User ID ${USER_ID} not found in gRPC GetFastrgDhcpInfo response"
    else
        CLI_POOL=$(printf '%s'   "$DHCP_USER" | jq -r '.ip_range // empty')
        CLI_SUBNET=$(printf '%s' "$DHCP_USER" | jq -r '.subnet_mask // empty')
        CLI_GW=$(printf '%s'     "$DHCP_USER" | jq -r '.gateway // empty')

        # Normalize ip_range: gRPC returns "start - end", etcd uses "start~end"
        # Collapse to "start~end" for comparison
        CLI_POOL_NORM=$(printf '%s' "$CLI_POOL" | tr -d ' ' | tr '-' '~' | sed 's/~~/~/')
        ETCD_POOL_NORM=$(printf '%s' "$ETCD_POOL" | tr -d ' ')

        MISMATCH=""
        [[ "$CLI_POOL_NORM" != "$ETCD_POOL_NORM" ]] && MISMATCH="${MISMATCH} ip_range(grpc=${CLI_POOL} etcd=${ETCD_POOL})"
        [[ "$CLI_SUBNET" != "$ETCD_SUBNET"  ]] && MISMATCH="${MISMATCH} subnet(grpc=${CLI_SUBNET} etcd=${ETCD_SUBNET})"
        [[ "$CLI_GW"     != "$ETCD_GATEWAY" ]] && MISMATCH="${MISMATCH} gateway(grpc=${CLI_GW} etcd=${ETCD_GATEWAY})"

        if [[ -z "$MISMATCH" ]]; then
            pass "Step 2b: DHCP config match" "pool=${CLI_POOL} subnet=${CLI_SUBNET} gw=${CLI_GW}"
        else
            fail "Step 2b: DHCP config match" "Mismatch:${MISMATCH}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 2c — Port-mapping config
    # ------------------------------------------------------------------
    info "Step 2c: Comparing port-mapping config (gRPC GetPortFwdInfo vs etcd)..."
    PM_COUNT=$(printf '%s' "$HSI_JSON" | jq -r '(.config["port-mapping"] // []) | length')

    if [[ "$PM_COUNT" -eq 0 ]]; then
        pass "Step 2c: Port-mapping match" "No port-mappings in etcd (nothing to verify)"
    else
        PORTFWD_GRPC=$(fastrg_grpc get_port_fwd_info "${USER_ID}")
        PM_FAIL=0
        PM_DETAIL=""

        i=0
        while [[ $i -lt $PM_COUNT ]]; do
            E_EPORT=$(printf '%s' "$HSI_JSON" | jq -r ".config[\"port-mapping\"][$i].eport")
            E_DIP=$(printf '%s'   "$HSI_JSON" | jq -r ".config[\"port-mapping\"][$i].dip")
            E_DPORT=$(printf '%s' "$HSI_JSON" | jq -r ".config[\"port-mapping\"][$i].dport")

            # Match eport in gRPC response entries
            ENTRY=$(printf '%s' "$PORTFWD_GRPC" | \
                jq -r ".entries[] | select(.eport == (\"${E_EPORT}\" | tonumber))" 2>/dev/null || true)
            if [[ -n "$ENTRY" ]]; then
                PM_DETAIL="${PM_DETAIL} eport=${E_EPORT}:OK"
            else
                PM_DETAIL="${PM_DETAIL} eport=${E_EPORT}:MISSING(dip=${E_DIP} dport=${E_DPORT})"
                PM_FAIL=1
            fi
            i=$((i + 1))
        done

        if [[ $PM_FAIL -eq 0 ]]; then
            pass "Step 2c: Port-mapping match" "${PM_DETAIL}"
        else
            fail "Step 2c: Port-mapping match" "${PM_DETAIL}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 2d — DNS static records
    # ------------------------------------------------------------------
    info "Step 2d: Comparing DNS static records (gRPC GetDnsStaticRecords vs etcd keys)..."
    DNS_KEYS=$(etcdctl_get_value "--prefix configs/${NODE_UUID}/${USER_ID}/dns/" 2>/dev/null || true)
    DNS_DOMAINS=$(printf '%s' "$DNS_KEYS" | jq -r '.domain // empty' 2>/dev/null || true)

    if [[ -z "$DNS_DOMAINS" ]]; then
        # Try raw key listing (key is the domain, value is JSON)
        DNS_KEYS_RAW=$(ssh_node "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} get --prefix --keys-only configs/${NODE_UUID}/${USER_ID}/dns/" 2>/dev/null || true)
        DNS_DOMAINS=$(printf '%s' "$DNS_KEYS_RAW" | awk -F'/' '{print $NF}' | grep -v '^$' || true)
    fi

    if [[ -z "$DNS_DOMAINS" ]]; then
        pass "Step 2d: DNS static match" "No DNS static keys in etcd (nothing to verify)"
    else
        DNS_GRPC=$(fastrg_grpc get_dns_static "${USER_ID}")
        DNS_FAIL=0
        DNS_DETAIL=""

        while IFS= read -r domain; do
            [[ -z "$domain" ]] && continue
            MATCH=$(printf '%s' "$DNS_GRPC" | \
                jq -r ".entries[] | select(.domain == \"${domain}\") | .domain" 2>/dev/null || true)
            if [[ -n "$MATCH" ]]; then
                DNS_DETAIL="${DNS_DETAIL} ${domain}:OK"
            else
                DNS_DETAIL="${DNS_DETAIL} ${domain}:MISSING"
                DNS_FAIL=1
            fi
        done <<< "$DNS_DOMAINS"

        if [[ $DNS_FAIL -eq 0 ]]; then
            pass "Step 2d: DNS static match" "${DNS_DETAIL}"
        else
            fail "Step 2d: DNS static match" "${DNS_DETAIL}"
        fi
    fi
}

# ---------------------------------------------------------------------------
# Phase 2 — DHCP Assignment + Subscriber Count
# ---------------------------------------------------------------------------
phase2_dhcp_and_count() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 2 — DHCP Assignment & Subscriber Count (Steps 3, 10)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 3 — DHCP In-use IPs (LAN device got an IP)
    # ------------------------------------------------------------------
    info "Step 3: Checking DHCP address assignment for USER_ID=${USER_ID}..."
    DHCP_GRPC3=$(fastrg_grpc get_dhcp_info)
    DHCP_USER3=$(printf '%s' "$DHCP_GRPC3" | jq -r ".dhcp_infos[] | select(.user_id == ${USER_ID})" 2>/dev/null || true)

    if [[ -z "$DHCP_USER3" ]]; then
        fail "Step 3: DHCP address assigned" "User ID ${USER_ID} not found in gRPC GetFastrgDhcpInfo"
    else
        INUSE_IPS=$(printf '%s' "$DHCP_USER3" | jq -r '.inuse_ips | if length > 0 then join(", ") else empty end' 2>/dev/null || true)
        if [[ -n "$INUSE_IPS" ]]; then
            pass "Step 3: DHCP address assigned" "In-use IPs: ${INUSE_IPS}"
        else
            fail "Step 3: DHCP address assigned" "inuse_ips is empty — no LAN device has obtained an IP"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 10 — subscriber_count in etcd vs fastrg gRPC system info
    # ------------------------------------------------------------------
    info "Step 10: Comparing subscriber count (etcd user_counts vs gRPC GetFastrgSystemInfo)..."
    UC_JSON=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null || true)

    if [[ -z "$UC_JSON" ]]; then
        fail "Step 10: Subscriber count match" "Key user_counts/${NODE_UUID}/ not found in etcd"
    else
        # subscriber_count is stored as a STRING in etcd
        ETCD_COUNT=$(printf '%s' "$UC_JSON" | jq -r '.subscriber_count // empty' | tr -d '[:space:]')
        ETCD_COUNT_INT=$(( ${ETCD_COUNT:-0} + 0 ))

        SYS_JSON=$(fastrg_grpc get_system_info)
        CLI_COUNT_INT=$(printf '%s' "$SYS_JSON" | jq -r '.num_users // 0' | tr -d '[:space:]')
        CLI_COUNT_INT=$(( ${CLI_COUNT_INT:-0} + 0 ))

        if [[ $ETCD_COUNT_INT -eq $CLI_COUNT_INT ]]; then
            pass "Step 10: Subscriber count match" "etcd=${ETCD_COUNT_INT} == fastrg=${CLI_COUNT_INT}"
        else
            fail "Step 10: Subscriber count match" "etcd=${ETCD_COUNT_INT} != fastrg=${CLI_COUNT_INT}"
        fi
    fi

    # ------------------------------------------------------------------
    # 10-second wait for PPPoE session establishment
    # ------------------------------------------------------------------
    printf "\n"
    info "Waiting 10 seconds for PPPoE session establishment..."
    sleep 10
    info "Wait complete. Continuing test..."
    printf "\n"
}

# ---------------------------------------------------------------------------
# Phase 2.5 — PPPoE Enable Status
# ---------------------------------------------------------------------------
phase2_5_enable_status() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 2.5 — PPPoE Enable Status (Step 11)"
    bold "═══════════════════════════════════════════════════════"

    info "Step 11: Checking etcd metadata.enableStatus for USER_ID=${USER_ID}..."
    HSI_JSON=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)

    if [[ -z "$HSI_JSON" ]]; then
        fail "Step 11: PPPoE enableStatus" "Cannot re-read configs/${NODE_UUID}/hsi/${USER_ID} from etcd"
        return
    fi

    ENABLE_STATUS=$(printf '%s' "$HSI_JSON" | jq -r '.metadata.enableStatus // empty')

    if [[ "$ENABLE_STATUS" == "enabled" ]]; then
        pass "Step 11: PPPoE enableStatus" "metadata.enableStatus = \"enabled\""
    else
        fail "Step 11: PPPoE enableStatus" "Expected \"enabled\", got \"${ENABLE_STATUS}\""
    fi
}

# ---------------------------------------------------------------------------
# Phase 3 — LAN→WAN Traffic Tests
# ---------------------------------------------------------------------------
phase3_lan_to_wan() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 3 — LAN→WAN Traffic (Steps 4–6)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 4 — Ping
    # ------------------------------------------------------------------
    info "Step 4: Ping ${WAN_IP} from LAN host ${LAN_HOST}..."
    PING_OUT=$(ssh_lan "ping -c 4 -W 3 ${WAN_IP} 2>&1" || true)
    if printf '%s' "$PING_OUT" | grep -qE "0% packet loss|0\.0% packet loss"; then
        pass "Step 4: Ping LAN→WAN" "${WAN_IP} reachable, 0% packet loss"
    else
        LOSS=$(printf '%s' "$PING_OUT" | grep -oE '[0-9]+(\.[0-9]+)?% packet loss' | head -1 || echo "no response")
        fail "Step 4: Ping LAN→WAN" "${WAN_IP} - ${LOSS}"
    fi

    # ------------------------------------------------------------------
    # Step 5 — iperf3
    # ------------------------------------------------------------------
    info "Step 5: iperf3 test (LAN→WAN, port 55688, cport 47792)..."
    # Start iperf3 server on WAN host (daemon mode)
    ssh_wan "iperf3 -s -B ${WAN_IP} -p 55688 -D --forceflush >/dev/null 2>&1 || true" || true
    sleep 2

    IPERF_OUT=$(ssh_lan "iperf3 -c ${WAN_IP} -p 55688 --cport 47792 -t 5 -J 2>&1" || true)
    # Cleanup iperf3 server
    ssh_wan "pkill -f 'iperf3 -s' 2>/dev/null || true" || true

    if [[ -z "$IPERF_OUT" ]]; then
        fail "Step 5: iperf3 LAN→WAN" "No output from iperf3 client"
    else
        BPS=$(printf '%s' "$IPERF_OUT" | jq -r '.end.sum_received.bits_per_second // 0' 2>/dev/null || echo "0")
        BPS_INT=$(printf '%.0f' "${BPS}" 2>/dev/null || echo "0")
        if [[ $BPS_INT -gt 0 ]]; then
            # Format as Mbps for readability
            MBPS=$(awk "BEGIN {printf \"%.2f\", $BPS_INT / 1000000}")
            pass "Step 5: iperf3 LAN→WAN" "Received ${MBPS} Mbps"
        else
            ERR=$(printf '%s' "$IPERF_OUT" | jq -r '.error // empty' 2>/dev/null || true)
            fail "Step 5: iperf3 LAN→WAN" "bits_per_second=0${ERR:+; error: $ERR}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 6 — curl
    # ------------------------------------------------------------------
    info "Step 6: curl http://www.google.com from LAN host ${LAN_HOST}..."
    HTTP_CODE=$(ssh_lan "curl -s -o /dev/null -w '%{http_code}' --max-time 15 http://www.google.com 2>&1" || echo "000")
    HTTP_CODE=$(printf '%s' "$HTTP_CODE" | tr -d "'" | tr -d '[:space:]')

    case "$HTTP_CODE" in
        200|301|302)
            pass "Step 6: curl www.google.com" "HTTP ${HTTP_CODE}"
            ;;
        000)
            fail "Step 6: curl www.google.com" "Connection failed or timed out (HTTP 000)"
            ;;
        *)
            fail "Step 6: curl www.google.com" "Unexpected HTTP status: ${HTTP_CODE}"
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Phase 4 — WAN→LAN DNAT Test (scapy + netcat)
# ---------------------------------------------------------------------------
phase4_dnat_test() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 4 — WAN→LAN DNAT (Step 7)"
    bold "═══════════════════════════════════════════════════════"

    info "Step 7: WAN→LAN DNAT — scapy from WAN, nc listen on LAN..."

    # Get port-mapping eport/dport from etcd (already fetched in phase1 as HSI_JSON)
    # Re-fetch in case phase1 was skipped or HSI_JSON is out of scope
    _HSI_ETCD=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
    PM_COUNT=$(printf '%s' "$_HSI_ETCD" | jq -r '(.config["port-mapping"] // []) | length' 2>/dev/null || echo "0")

    if [[ "$PM_COUNT" -eq 0 ]]; then
        skip "Step 7: WAN→LAN DNAT" "No port-mapping in etcd — cannot perform DNAT test"
        return
    fi

    # Use first port-mapping entry
    DNAT_EPORT=$(printf '%s' "$_HSI_ETCD" | jq -r '.config["port-mapping"][0].eport')
    DNAT_DIP=$(printf '%s'   "$_HSI_ETCD" | jq -r '.config["port-mapping"][0].dip')
    DNAT_DPORT=$(printf '%s' "$_HSI_ETCD" | jq -r '.config["port-mapping"][0].dport')
    info "  DNAT rule: WAN:${DNAT_EPORT} → LAN ${DNAT_DIP}:${DNAT_DPORT}"

    # Fetch PPPoE client WAN IP for this user from gRPC (ip_addr in HsiInfo)
    _HSI_GRPC=$(fastrg_grpc get_hsi_info)
    DNAT_PPP_IP=$(printf '%s' "$_HSI_GRPC" | \
        jq -r ".hsi_infos[] | select(.user_id == ${USER_ID}) | .ip_addr" 2>/dev/null || true)
    if [[ -z "$DNAT_PPP_IP" ]]; then
        fail "Step 7: WAN→LAN DNAT" "Cannot determine PPPoE client WAN IP for USER_ID=${USER_ID} from gRPC"
        return
    fi
    info "  PPPoE WAN IP (gRPC ip_addr): ${DNAT_PPP_IP}"

    # Start nc UDP listener on LAN host at the DNAT destination port (no root needed)
    NC_OUT_FILE=$(mktemp /tmp/fastrg_nc_XXXXXX)
    ssh_lan "timeout 10 nc -u -l -p ${DNAT_DPORT} 2>&1" \
        > "$NC_OUT_FILE" 2>&1 &
    NC_PID=$!

    sleep 2

    # Send UDP packet from WAN host using scapy to WAN IP:eport
    SCAPY_CMD="python3 -c \"from scapy.all import Ether,IP,UDP,Raw,sendp; pkt=Ether(dst='74:4d:28:8d:00:2c',src='9c:69:b4:68:65:db')/IP(src='192.168.201.10',dst='${DNAT_PPP_IP}',ttl=64,id=0x4003)/UDP(sport=54321,dport=${DNAT_EPORT})/Raw(load=b'hello'); sendp(pkt, iface='ens6f3np3')\" 2>&1"
    SCAPY_OUT=$(ssh_wan "$SCAPY_CMD" 2>&1 || true)
    info "  scapy output: ${SCAPY_OUT}"

    # Wait for nc to finish
    wait $NC_PID 2>/dev/null || true
    NC_OUT=$(cat "$NC_OUT_FILE")
    rm -f "$NC_OUT_FILE"

    info "  nc output: ${NC_OUT:-<empty — no data received>}"

    # nc prints nothing on success (just exits with the received data) or "hello" payload
    # Also check exit: nc -l exits after receiving one packet (timeout 10 will exit even without data)
    # We consider PASS if nc received data (payload "hello") or exited cleanly within timeout
    if printf '%s' "$NC_OUT" | grep -q "hello"; then
        pass "Step 7: WAN→LAN DNAT" "UDP payload 'hello' received on LAN ${DNAT_DIP}:${DNAT_DPORT}"
    else
        fail "Step 7: WAN→LAN DNAT" "scapy failed to send or nc did not receive on LAN ${DNAT_DIP}:${DNAT_DPORT}"
    fi
}

# ---------------------------------------------------------------------------
# Phase 5 — DNS Static Record + Reverse Ping
# ---------------------------------------------------------------------------
phase5_dns_ping() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 5 — DNS Static + Reverse Ping (Step 8)"
    bold "═══════════════════════════════════════════════════════"

    info "Step 8: Ping www.google.org from LAN host; expecting reply from ${WAN_IP}..."
    PING_OUT=$(ssh_lan "ping -c 4 -W 5 www.google.org 2>&1" || true)

    info "  ping output:"
    printf '%s\n' "$PING_OUT" | while IFS= read -r line; do
        printf "    %s\n" "$line"
    done

    if printf '%s' "$PING_OUT" | grep -q "from ${WAN_IP}"; then
        pass "Step 8: DNS static + ping www.google.org" "Received ICMP reply from ${WAN_IP}"
    else
        # Check if it resolved but got a different IP (DNS not overridden)
        if printf '%s' "$PING_OUT" | grep -qE "PING|bytes from"; then
            REPLY_IP=$(printf '%s' "$PING_OUT" | grep -oE "from [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" | head -1 | awk '{print $2}')
            fail "Step 8: DNS static + ping www.google.org" "Got reply from ${REPLY_IP:-unknown}, expected ${WAN_IP} — DNS static record may not be configured"
        else
            fail "Step 8: DNS static + ping www.google.org" "No ICMP reply received"
        fi
    fi
}

# ---------------------------------------------------------------------------
# Phase 6 — Summary
# ---------------------------------------------------------------------------
phase6_summary() {
    local total=${#STEP_NAMES[@]}
    local pass_count=0 fail_count=0 skip_count=0

    printf "\n"
    bold "═══════════════════════════════════════════════════════"
    bold " Test Results Summary"
    bold "═══════════════════════════════════════════════════════"
    printf "\n"

    i=0
    while [[ $i -lt $total ]]; do
        name="${STEP_NAMES[$i]}"
        result="${STEP_RESULTS[$i]}"
        detail="${STEP_DETAILS[$i]}"

        case "$result" in
            PASS)
                printf "  ${GREEN}✔ PASS${NC}  %-40s  %s\n" "$name" "$detail"
                pass_count=$((pass_count + 1))
                ;;
            FAIL)
                printf "  ${RED}✘ FAIL${NC}  %-40s  %s\n" "$name" "$detail"
                fail_count=$((fail_count + 1))
                ;;
            SKIP)
                printf "  ${YELLOW}– SKIP${NC}  %-40s  %s\n" "$name" "$detail"
                skip_count=$((skip_count + 1))
                ;;
        esac
        i=$((i + 1))
    done

    printf "\n"
    printf "  Total: %d   " "$total"
    printf "${GREEN}Pass: %d${NC}   " "$pass_count"
    printf "${RED}Fail: %d${NC}   " "$fail_count"
    printf "${YELLOW}Skip: %d${NC}\n" "$skip_count"
    printf "\n"

    if [[ $fail_count -gt 0 ]]; then
        bold "  RESULT: ${RED}FAILED${NC} (${fail_count} step(s) failed)"
        return 1
    else
        bold "  RESULT: ${GREEN}ALL PASSED${NC}"
        return 0
    fi
}

# ---------------------------------------------------------------------------
# Cleanup — kill fastrg only if the script started it
# ---------------------------------------------------------------------------
cleanup_fastrg() {
    set +eu  # Prevent set -e / set -u from interrupting cleanup, ensure all cleanup steps are executed

    if [[ "${_FASTRG_STARTED_BY_SCRIPT:-0}" -eq 1 ]]; then
        info "Stopping fastrg (started by this script)..."
        ssh_node "pkill -x fastrg 2>/dev/null || true" || true
        info "fastrg stopped."
    fi

    info "Cleanup complete."
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    _FASTRG_STARTED_BY_SCRIPT=0

    # Clean up anyway on exit
    trap 'cleanup_fastrg' EXIT

    printf "\n"
    bold "╔═════════════════════════════════════════════════════╗"
    bold "║   FastRG Node — E2E Data Plane Test                 ║"
    bold "╚═════════════════════════════════════════════════════╝"
    printf "\n"
    info "USER_ID: ${USER_ID}"
    printf "\n"

    phase0_setup
    phase1_etcd_config_sync
    phase2_dhcp_and_count
    phase2_5_enable_status
    phase3_lan_to_wan
    phase4_dnat_test
    phase5_dns_ping
    phase6_summary || true
}

main "$@"
