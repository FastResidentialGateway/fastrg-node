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
# Self-relocation — the runner must have an user called "root".
# If invoked from any other machine it uploads itself + companion files and
# re-executes there.
# Set _FASTRG_E2E_RELOCATED=1 to skip this check (set automatically on relay).
# ---------------------------------------------------------------------------
# Allow --runner-host to override the default before argument parsing runs.
# We do a quick pre-scan of $@ here so the self-relocation block can use it.
_E2E_RUNNER_HOST="192.168.10.104"
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
_E2E_RUNNER_USER="root"
_E2E_REMOTE_DIR='~/fastrg_e2e_test'
_E2E_REMOTE_PATH="${_E2E_REMOTE_DIR}/run_e2e_test.sh"

if [[ -z "${_FASTRG_E2E_RELOCATED:-}" ]]; then
    # Collect local IPs — hostname -I on Linux, ifconfig on macOS
    _my_ips=$(hostname -I 2>/dev/null || \
              ifconfig 2>/dev/null | awk '/inet /{gsub(/addr:/,"",$2); print $2}')
    if ! printf '%s\n' $_my_ips | grep -qx "${_E2E_RUNNER_HOST}"; then
        info "Not running on ${_E2E_RUNNER_HOST} — uploading files and re-executing remotely..."
        _SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o Port=2222"
        _SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
        _REPO_ROOT="$(cd "${_SCRIPT_DIR}/.." && pwd)"

        # Ensure remote directory exists
        ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" "mkdir -p ${_E2E_REMOTE_DIR}"

        # Upload this script to the remote runner host
        info "Uploading run_e2e_test.sh..."
        scp $_SSH_OPTS "$0" "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_PATH}"

        # Upload gRPC Python client
        if [[ -f "${_SCRIPT_DIR}/fastrg_grpc_client.py" ]]; then
            info "Uploading fastrg_grpc_client.py..."
            scp $_SSH_OPTS "${_SCRIPT_DIR}/fastrg_grpc_client.py" \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/fastrg_grpc_client.py"
        else
            warn "fastrg_grpc_client.py not found at ${_SCRIPT_DIR}/fastrg_grpc_client.py"
        fi

        # Upload phase scripts
        if [[ -d "${_SCRIPT_DIR}/phases" ]]; then
            info "Uploading phase scripts..."
            ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                "mkdir -p ${_E2E_REMOTE_DIR}/phases"
            scp $_SSH_OPTS "${_SCRIPT_DIR}/phases/"*.sh \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/phases/"
        else
            warn "phases/ directory not found at ${_SCRIPT_DIR}/phases"
        fi

        # Upload proto file (needed by fastrg_grpc_client.py at runtime via grpcurl)
        _PROTO_SRC="${_REPO_ROOT}/northbound/grpc/fastrg_node.proto"
        if [[ -f "${_PROTO_SRC}" ]]; then
            info "Uploading fastrg_node.proto..."
            scp $_SSH_OPTS "${_PROTO_SRC}" \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/fastrg_node.proto"
        else
            warn "proto not found at ${_PROTO_SRC}"
        fi

        # Upload grpcurl only when OS+arch match (platform-specific binary)
        _GRPCURL_BIN=$(command -v grpcurl 2>/dev/null || true)
        _local_os=$(uname -s)
        _local_arch=$(uname -m)
        _runner_os=$(ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
            "uname -s" 2>/dev/null || echo "unknown")
        _runner_arch=$(ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
            "uname -m" 2>/dev/null || echo "unknown")
        if [[ -n "$_GRPCURL_BIN" ]] && \
           [[ "$_local_os" == "$_runner_os" ]] && \
           [[ "$_local_arch" == "$_runner_arch" ]]; then
            info "Uploading grpcurl binary ($_runner_os/$_runner_arch)..."
            scp $_SSH_OPTS "${_GRPCURL_BIN}" \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/grpcurl"
            ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                "chmod +x ${_E2E_REMOTE_DIR}/grpcurl"
        elif [[ -n "$_GRPCURL_BIN" ]] && [[ "$_runner_os" != "unknown" ]] && \
             { [[ "$_local_os" != "$_runner_os" ]] || [[ "$_local_arch" != "$_runner_arch" ]]; }; then
            # OS/arch mismatch — verify runner already has its own grpcurl before proceeding
            warn "grpcurl OS/arch mismatch (local=$_local_os/$_local_arch, runner=$_runner_os/$_runner_arch)"
            info "Checking if runner already has grpcurl..."
            # Include /opt/homebrew/bin so Apple Silicon macOS Homebrew installs are found
            _runner_grpcurl=$(ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                "PATH=\"/usr/local/bin:/opt/homebrew/bin:\$PATH\" command -v grpcurl 2>/dev/null || true" \
                2>/dev/null || true)
            if [[ -n "$_runner_grpcurl" ]]; then
                info "Runner has grpcurl at $_runner_grpcurl — it is fine"
            else
                error "grpcurl is not installed on runner ${_E2E_RUNNER_HOST} ($_runner_os/$_runner_arch)"
                if [[ "$_runner_os" == "Darwin" ]]; then
                    error "Install it on the runner first:  brew install grpcurl"
                else
                    error "Install it on the runner first:  https://github.com/fullstorydev/grpcurl/releases"
                fi
                exit 1
            fi
        else
            warn "grpcurl not found locally — runner must have grpcurl in PATH"
            exit 1
        fi

        # Rebuild quoted arg list to forward all original arguments
        _remote_args=""
        for _a in "$@"; do _remote_args="${_remote_args} '${_a}'"; done
        ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
            "chmod +x ${_E2E_REMOTE_PATH} && _FASTRG_E2E_RELOCATED=1 ${_E2E_REMOTE_PATH}${_remote_args}"
        _ssh_rc=$?

        # Clean up uploaded files from runner (always, regardless of test result)
        info "Cleaning up uploaded files from runner ${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR} ..."
        ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
            "rm -rf ${_E2E_REMOTE_DIR}/run_e2e_test.sh \
                   ${_E2E_REMOTE_DIR}/fastrg_grpc_client.py \
                   ${_E2E_REMOTE_DIR}/fastrg_node.proto \
                   ${_E2E_REMOTE_DIR}/grpcurl \
                   ${_E2E_REMOTE_DIR}/phases 2>/dev/null; \
             rmdir ${_E2E_REMOTE_DIR} 2>/dev/null || true" 2>/dev/null || true

        exit $_ssh_rc
    fi
fi

set -euo pipefail

# Ensure common tool locations are in PATH (needed for macOS SSH non-login shells)
export PATH="/usr/local/bin:/usr/local/sbin:${PATH}"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
FASTRG_NODE="192.168.10.211"
LAN_HOST="192.168.10.220"
WAN_HOST="192.168.10.104"
WAN_IP="192.168.201.11"
SRV_PORT=55688                        # only port the WAN-side firewall lets through in the test bench
CLIENT_CPORT=47792                    # arbitrary unprivileged port for iperf3 client source port
WAN_NIC=enp1s0f1                     # WAN-side NIC on the WAN host
FASTRG_NODE_WAN_MAC='74:4d:28:8d:00:2c'  # FastRG node WAN port MAC
WAN_HOST_MAC='90:e2:ba:8a:60:b1'     # WAN host NIC MAC
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
# Load phase scripts (each file defines one phase function)
# ---------------------------------------------------------------------------
_E2E_PHASES_DIR="${GRPC_CLIENT_DIR}/phases"
# shellcheck source=/dev/null
source "${_E2E_PHASES_DIR}/phase0_setup.sh"
source "${_E2E_PHASES_DIR}/phase1_subscriber_count_tests.sh"
source "${_E2E_PHASES_DIR}/phase2_etcd_config_sync.sh"
source "${_E2E_PHASES_DIR}/phase3_dhcp_and_count.sh"
source "${_E2E_PHASES_DIR}/phase3_5_enable_status.sh"
source "${_E2E_PHASES_DIR}/phase4_lan_to_wan.sh"
source "${_E2E_PHASES_DIR}/phase4_5_tcp_spi.sh"
source "${_E2E_PHASES_DIR}/phase5_dnat_test.sh"
source "${_E2E_PHASES_DIR}/phase6_dns_ping.sh"
source "${_E2E_PHASES_DIR}/phase7_extra_user_config_tests.sh"
source "${_E2E_PHASES_DIR}/phase8_summary.sh"

# ---------------------------------------------------------------------------
# Cleanup — kill fastrg only if the script started it
# ---------------------------------------------------------------------------
cleanup_fastrg() {
    set +eu  # Prevent set -e / set -u from interrupting cleanup, ensure all cleanup steps are executed

    # Best-effort: remove new subscriber config if the test left it in etcd
    _cleanup_new_subscriber_config 2>/dev/null || true

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
    phase1_subscriber_count_tests
    phase2_etcd_config_sync
    phase3_dhcp_and_count
    phase3_5_enable_status
    phase4_lan_to_wan
    phase4_5_tcp_spi
    phase5_dnat_test
    phase6_dns_ping
    phase7_extra_user_config_tests
    phase8_summary || true
}

main "$@"
