#!/usr/bin/env bash
# =============================================================================
# FastRG Node — End-to-End Data Plane Test Script
#
# Usage:
#   ./run_e2e_test.sh [OPTIONS]
#
# Options:
#   --sub-id       SPEC   Subscriber IDs to test  (default: 2,1)
#                         Accepts a comma list ("2,1,3") or a range ("2-10").
#                         The FIRST id is the primary (full data-plane suite);
#                         the rest are secondaries (loaded + PPPoE checks only).
#   --fastrg-node  IP     FastRG node IP         (default: 192.168.10.201)
#   --lan-host     IP     LAN-side host IP        (default: 192.168.10.210)
#   --wan-host     IP     WAN-side host IP        (default: 192.168.10.106)
#   --wan-ip       IP     WAN subscriber IP       (default: 192.168.201.10)
#   --runner-host  IP     E2E runner host IP      (default: 192.168.10.207)
#   --bras-host    IP     BRAS (dpdk-bras) host IP (default: 192.168.10.215)
#   --ssh-key      PATH   SSH identity file       (default: auto-detect id_ed25519 or id_rsa)
#   (no mode flag)        Run everything, in order: --local-validate,
#                         --run-case, then --validate-case all.
#   --run-case            E2e case correctness validation + the full suite (commit gate).
#   --validate-case all   E2e case correctness validation + phase0 + every drill.
#   --validate-case IDS   Same, limited to the comma-separated drill ids.
#   --local-validate      Run the offline e2e case correctness validation and stop
#                         (it already runs first in every other mode).
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
#
# LAN-side topology (environment overrides; the defaults describe this bench):
#   LAN_FLAP_HOST  Host owning the LAN-side PF that carries the LAN link.
#                  (default: the --wan-host value — on this bench the LAN PF
#                  and the WAN NIC sit in the same machine.)
#   LAN_FLAP_NIC   LAN-side PF on LAN_FLAP_HOST; link flaps are driven here
#                  because nothing inside the peer can drop the physical
#                  signal the node observes.       (default: enp1s0f0)
#   LAN_PEER_NIC   NIC the LAN peer uses for LAN traffic.  (default: enp8s0)
#   LAN_VF_ID      Index of the VF that LAN_PEER_NIC consumes. (default: 0)
#                  Leave it EMPTY (LAN_VF_ID=) on a bench without SR-IOV on
#                  the LAN side: every VF-specific assertion and every
#                  host-side VF fallback is then skipped instead of failing.
#
# Subscriber-scale resource requirement:
#   The node computes its subscriber capacity from the hugepage heap at startup.
#   The bench must start it with --socket-mem 17408 so rte_malloc owns all 17
#   1-GiB pages up front; PA mode cannot dynamically add free system hugepages.
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

# Cleanup runs after the caller's ssh channel may already be gone, and writing
# to that dead channel kills the script part-way through, leaving the rest of
# the cleanup undone. So while cleanup is in progress on the runner, these four
# helpers write to a runner-local file instead of the channel. Anything a
# cleanup path prints through them is covered automatically; a cleanup added
# later needs no special handling as long as it prints through these helpers.
# Outside cleanup, and whenever the suite runs without relocating, the output
# goes exactly where it always did.
_E2E_CLEANUP_LOG=/tmp/fastrg_e2e_cleanup.log
_E2E_IN_CLEANUP=""

# The relocated instance records its own pid here so the caller can signal it by
# identity. Matching a process by its command line is not good enough: the
# suite's forked subshells carry the same argv as the script itself.
_E2E_REMOTE_PID_FILE=/tmp/fastrg_e2e_remote.pid

# Where the interrupt path leaves its notes. Defined up here because the caller
# side needs to be able to point at it before the run relocates.
_E2E_TRACE_LOG=/tmp/fastrg_e2e_interrupt.log

# The caller passes the same format string the helper always used, so colour
# escapes stay in the format and the message stays an argument to %s.
_e2e_emit() {
    local _stream="$1" _fmt="$2" _line
    shift 2
    printf -v _line "$_fmt" "$*"
    if [[ -n "${_E2E_IN_CLEANUP:-}" && -n "${_FASTRG_E2E_RELOCATED:-}" ]]; then
        printf '%s %s' "$(date '+%F %T.%3N')" "$_line" >> "$_E2E_CLEANUP_LOG"
        return 0
    fi
    if [[ "$_stream" == "err" ]]; then
        printf '%s' "$_line" >&2
    else
        printf '%s' "$_line"
    fi
}

info()  { _e2e_emit out "${CYAN}[INFO]${NC}  %s\n" "$*"; }
warn()  { _e2e_emit out "${YELLOW}[WARN]${NC}  %s\n" "$*"; }
error() { _e2e_emit err "${RED}[ERROR]${NC} %s\n" "$*"; }
bold()  { _e2e_emit out "${BOLD}%s${NC}\n" "$*"; }

# ---------------------------------------------------------------------------
# Self-relocation — the runner must have an user called "root".
# If invoked from any other machine it uploads itself + companion files and
# re-executes there.
# Set _FASTRG_E2E_RELOCATED=1 to skip this check (set automatically on relay).
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Every flag the suite accepts, listed once. The pre-check below and the parser
# further down both read these; a flag added to the parser but not here is
# rejected the first time anyone uses it.
# ---------------------------------------------------------------------------
_E2E_VALUE_FLAGS=(--fastrg-node --lan-host --wan-host --wan-ip --runner-host
    --bras-host --sub-id --ssh-key --grpc-port --controller-rest
    --controller-grpc --controller-user --controller-pass --validate-case)
_E2E_BOOLEAN_FLAGS=(--help -h --run-case --local-validate)

_e2e_flag_listed() {
    local _needle="$1" _flag

    shift
    for _flag in "$@"; do
        [[ "$_flag" == "$_needle" ]] && return 0
    done
    return 1
}

# Read-only pre-check, before anything is uploaded: unknown flag or a missing
# value stops here instead of on the runner. It deliberately assigns nothing and
# judges no combinations — that stays with the parser, so this cannot drift into
# being a second one.
_E2E_RUNNER_HOST="192.168.10.104"
_E2E_LOCAL_VALIDATE=0
_e2e_index=1
while [[ $_e2e_index -le $# ]]; do
    _e2e_arg="${!_e2e_index}"
    if [[ "$_e2e_arg" == --runner-host=* ]]; then
        _E2E_RUNNER_HOST="${_e2e_arg#--runner-host=}"
        _e2e_index=$(( _e2e_index + 1 ))
        continue
    fi
    [[ "$_e2e_arg" == "--local-validate" ]] && _E2E_LOCAL_VALIDATE=1
    if _e2e_flag_listed "$_e2e_arg" "${_E2E_VALUE_FLAGS[@]}"; then
        _e2e_next_index=$(( _e2e_index + 1 ))
        _e2e_next="${!_e2e_next_index:-}"
        if [[ -z "$_e2e_next" ]]; then
            error "${_e2e_arg} needs a value — run with --help for usage"
            exit 1
        fi
        [[ "$_e2e_arg" == "--runner-host" ]] && _E2E_RUNNER_HOST="$_e2e_next"
        _e2e_index=$(( _e2e_index + 2 ))
        continue
    fi
    if ! _e2e_flag_listed "$_e2e_arg" "${_E2E_BOOLEAN_FLAGS[@]}"; then
        error "Unknown option: ${_e2e_arg} — run with --help for usage"
        exit 1
    fi
    _e2e_index=$(( _e2e_index + 1 ))
done
unset _e2e_index _e2e_arg _e2e_next_index _e2e_next

_E2E_RUNNER_USER="root"
_E2E_REMOTE_DIR='~/fastrg_e2e_test'
_E2E_REMOTE_PATH="${_E2E_REMOTE_DIR}/run_e2e_test.sh"

# Offline mode, so it is answered before the relocation block below.
if [[ "$_E2E_LOCAL_VALIDATE" -eq 1 ]]; then
    _E2E_PHASES_DIR="$(cd "$(dirname "$0")" && pwd)/phases"
    # shellcheck source=/dev/null
    source "${_E2E_PHASES_DIR}/local_validation_lib.sh"
    source "${_E2E_PHASES_DIR}/case_validation_lib.sh"
    for _registrar in "${_E2E_PHASES_DIR}"/*.sh; do
        case "$_registrar" in
            */local_validation_lib.sh|*/case_validation_lib.sh) continue ;;
        esac
        # shellcheck source=/dev/null
        source "$_registrar"
    done
    local_validation_run || exit 1
    exit 0
fi

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

        # Upload deterministic DNS responder used by the cache behaviour phase.
        if [[ -f "${_SCRIPT_DIR}/dns_responder.py" ]]; then
            info "Uploading dns_responder.py..."
            scp $_SSH_OPTS "${_SCRIPT_DIR}/dns_responder.py" \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/dns_responder.py"
        else
            warn "dns_responder.py not found at ${_SCRIPT_DIR}/dns_responder.py"
        fi

        # Upload the raw-packet virtual DHCP client used by the lease lifecycle phase.
        if [[ -f "${_SCRIPT_DIR}/dhcp_client_sim.py" ]]; then
            info "Uploading dhcp_client_sim.py..."
            scp $_SSH_OPTS "${_SCRIPT_DIR}/dhcp_client_sim.py" \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/dhcp_client_sim.py"
        else
            warn "dhcp_client_sim.py not found at ${_SCRIPT_DIR}/dhcp_client_sim.py"
        fi

        # Upload the raw-frame virtual LAN device helper used by the multi-device phase.
        if [[ -f "${_SCRIPT_DIR}/lan_device_sim.py" ]]; then
            info "Uploading lan_device_sim.py..."
            scp $_SSH_OPTS "${_SCRIPT_DIR}/lan_device_sim.py" \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/lan_device_sim.py"
        else
            warn "lan_device_sim.py not found at ${_SCRIPT_DIR}/lan_device_sim.py"
        fi

        # Upload phase scripts
        if [[ -d "${_SCRIPT_DIR}/phases" ]]; then
            info "Uploading phase scripts..."
            ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                "mkdir -p ${_E2E_REMOTE_DIR}/phases"
            scp $_SSH_OPTS "${_SCRIPT_DIR}/phases/"*.sh \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/phases/"
            scp $_SSH_OPTS "${_SCRIPT_DIR}/phases/local_validation_fixtures.txt" \
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

        # Upload controller proto for the heartbeat/re-registration phase.
        _CONTROLLER_PROTO_SRC="${_REPO_ROOT}/northbound/controller/proto/controller.proto"
        if [[ -f "${_CONTROLLER_PROTO_SRC}" ]]; then
            info "Uploading controller.proto..."
            scp $_SSH_OPTS "${_CONTROLLER_PROTO_SRC}" \
                "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR}/controller.proto"
        else
            warn "proto not found at ${_CONTROLLER_PROTO_SRC}"
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

        # An interrupt typed here reaches this shell and the ssh client together,
        # so the channel is already gone by the time this runs. Signalling the
        # runner-side script over a fresh connection starts its cleanup at once
        # instead of relying on the runner-side watchdog noticing the loss, and
        # it also covers the case where sshd outlives the client so that the
        # watchdog would never fire at all. Its output is fetched afterwards
        # because it can no longer come back over the original channel.
        # The longest cleanup measured on this bench, with etcd blocked
        # throughout, took 216s end to end. Allow a little over that: walking
        # away early is what leaves the bench dirty for the next run, while
        # waiting far longer than any real cleanup only delays telling the
        # operator that something is wrong.
        _E2E_REMOTE_WAIT_LIMIT=240

        # Wait until the runner-side script is really gone. Used from both places
        # that must not get ahead of it: the interrupt relay, and the removal of
        # the uploaded files further down.
        _e2e_wait_for_remote_exit() {
            local _rpid="$1" _waited=0 _alive=""

            while [[ "$_waited" -lt "$_E2E_REMOTE_WAIT_LIMIT" ]]; do
                # Only a real answer counts. When the ssh itself fails it says
                # nothing about whether the script is still running, so that is
                # never read as "finished".
                _alive=$(ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                    "test -d /proc/${_rpid} && echo yes || echo no" 2>/dev/null)
                [[ "$_alive" == "no" ]] && return 0
                sleep 2
                _waited=$((_waited + 2))
                # Say something now and then: a silent wait this long looks no
                # different from a hang.
                if [[ $((_waited % 30)) -eq 0 ]]; then
                    printf '[INFO]  still cleaning up on the runner (%ss): %s\n' "$_waited" \
                        "$(ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                            "tail -1 ${_E2E_CLEANUP_LOG} 2>/dev/null" 2>/dev/null)"
                fi
            done
            return 1
        }

        _E2E_RELAY_ACTIVE=0
        _e2e_relay_interrupt() {
            local _rpid="" _i

            # Pressing the interrupt again usually means "is this thing stuck?".
            # Answer it, but do not leave: the runner is still cleaning up, and
            # walking away now loses both the progress display and the log this
            # would otherwise fetch. The trap therefore stays armed and this
            # guard turns the repeats into a status report.
            if [[ "$_E2E_RELAY_ACTIVE" -eq 1 ]]; then
                printf '\n[INFO]  Still waiting for the runner to finish cleaning up; please hold.\n'
                printf '[INFO]  Latest: %s\n' \
                    "$(ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                        "tail -1 ${_E2E_CLEANUP_LOG} 2>/dev/null" 2>/dev/null)"
                return 0
            fi
            _E2E_RELAY_ACTIVE=1

            printf '\n[INFO]  Interrupt received; asking the runner to stop and clean up...\n'
            for _i in 1 2 3; do
                _rpid=$(ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                    "cat ${_E2E_REMOTE_PID_FILE} 2>/dev/null" 2>/dev/null | tr -dc '0-9')
                [[ -n "$_rpid" ]] && break
                sleep 1
            done
            if [[ -z "$_rpid" ]]; then
                printf '[WARN]  Could not read the runner-side pid; its own watchdog takes over.\n'
                exit 130
            fi
            ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                "kill -TERM ${_rpid} 2>/dev/null" >/dev/null 2>&1 || true
            if ! _e2e_wait_for_remote_exit "$_rpid"; then
                printf '[WARN]  Runner-side cleanup has not finished after %ss; it may well still be running.\n' \
                    "$_E2E_REMOTE_WAIT_LIMIT"
                printf '[WARN]  Follow it on %s: %s (interrupt trace: %s)\n' \
                    "${_E2E_RUNNER_HOST}" "$_E2E_CLEANUP_LOG" "$_E2E_TRACE_LOG"
            fi
            printf '[INFO]  Runner-side cleanup output:\n'
            ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                "cat ${_E2E_CLEANUP_LOG} 2>/dev/null" 2>/dev/null || true
            exit 130
        }
        # A shell that starts a job in the background without job control sets
        # SIGINT to SIG_IGN in the child, and a signal that is already ignored
        # cannot be trapped -- bash accepts the trap and silently never runs it.
        # So say plainly that the relay is off rather than let it look armed.
        if [[ "$(( 16#$(sed -n 's/^SigIgn:\s*//p' /proc/$$/status) & 2 ))" -ne 0 ]]; then
            warn "SIGINT is ignored in this shell, so Ctrl-C cannot be relayed to the runner."
            warn "Cleanup would fall back to the runner-side watchdog. To get the relay, run 'set -m' before starting this in the background."
        fi

        # Clear both files first: a leftover pid from an earlier run would send
        # the interrupt to whatever process now holds that number.
        ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
            "rm -f ${_E2E_REMOTE_PID_FILE} ${_E2E_CLEANUP_LOG}" >/dev/null 2>&1 || true
        trap '_e2e_relay_interrupt' INT TERM

        # Rebuild quoted arg list to forward all original arguments
        _remote_args=""
        for _a in "$@"; do _remote_args="${_remote_args} '${_a}'"; done
        ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
            "chmod +x ${_E2E_REMOTE_PATH} && LAN_FLAP_HOST='${LAN_FLAP_HOST:-}' LAN_FLAP_NIC='${LAN_FLAP_NIC:-}' \
             LAN_PEER_NIC='${LAN_PEER_NIC:-}' LAN_VF_ID='${LAN_VF_ID-0}' \
             _FASTRG_E2E_RELOCATED=1 ${_E2E_REMOTE_PATH}${_remote_args}"
        _ssh_rc=$?
        trap - INT TERM

        # The ssh can return while the runner-side script is still going: killing
        # the client ends this side of the connection, and the runner keeps
        # working under its own watchdog. This is a different entry point from
        # the interrupt relay above and reaches here without anyone having
        # waited, so check for that case explicitly. Removing the uploads now
        # would take the phase scripts, the grpc client and the working
        # directory away from a cleanup that is still using them.
        _remote_pid=$(ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
            "cat ${_E2E_REMOTE_PID_FILE} 2>/dev/null" 2>/dev/null | tr -dc '0-9')
        _remote_busy=0
        if [[ -n "$_remote_pid" ]] && ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
                "test -d /proc/${_remote_pid}" >/dev/null 2>&1; then
            info "Runner-side script is still running; waiting for it before removing the uploads..."
            _e2e_wait_for_remote_exit "$_remote_pid" || _remote_busy=1
        fi
        if [[ "$_remote_busy" -eq 1 ]]; then
            warn "Runner-side cleanup has not finished after ${_E2E_REMOTE_WAIT_LIMIT}s; leaving the uploaded files in place so it can finish."
            warn "Follow it on ${_E2E_RUNNER_HOST}: ${_E2E_CLEANUP_LOG} (interrupt trace: ${_E2E_TRACE_LOG})"
            exit $_ssh_rc
        fi

        # Clean up uploaded files from runner (always, regardless of test result)
        info "Cleaning up uploaded files from runner ${_E2E_RUNNER_HOST}:${_E2E_REMOTE_DIR} ..."
        ssh $_SSH_OPTS "${_E2E_RUNNER_USER}@${_E2E_RUNNER_HOST}" \
            "rm -rf ${_E2E_REMOTE_DIR}/run_e2e_test.sh \
                   ${_E2E_REMOTE_DIR}/fastrg_grpc_client.py \
                   ${_E2E_REMOTE_DIR}/dns_responder.py \
                   ${_E2E_REMOTE_DIR}/dhcp_client_sim.py \
                   ${_E2E_REMOTE_DIR}/lan_device_sim.py \
                   ${_E2E_REMOTE_DIR}/fastrg_node.proto \
                   ${_E2E_REMOTE_DIR}/controller.proto \
                   ${_E2E_REMOTE_DIR}/grpcurl \
                   ${_E2E_REMOTE_DIR}/phases 2>/dev/null; \
             rmdir ${_E2E_REMOTE_DIR} 2>/dev/null || true" 2>/dev/null || true

        exit $_ssh_rc
    fi
fi

set -euo pipefail

# Published before any phase runs so the caller can signal this process the
# moment an interrupt arrives.
if [[ -n "${_FASTRG_E2E_RELOCATED:-}" ]]; then
    printf '%s' "$$" > "$_E2E_REMOTE_PID_FILE" 2>/dev/null || true
fi

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

# Subscriber selection. --sub-id accepts a comma list ("2,1,3") or a range
# ("2-10"); it expands into SUB_IDS. When omitted, defaults to "2,1".
SUB_ID_SPEC=""
SUB_ID_SPEC_DEFAULT="2,1"

# BRAS (dpdk-bras) — the PPPoE/BRAS server simulator the node dials into. The
# runner SSHes here to launch dpdk-bras before the node starts and to kill it on
# exit. It serves PPPoE on VLAN 3 (subscriber 2) and VLAN 5 (subscriber 1).
BRAS_HOST="192.168.10.215"

# Controller (SSOT): config writes + PPPoE dial/hangup go here; the node applies
# from etcd via its watch. REST is used for login + writes; gRPC ConfigService
# address is used by fastrg_cli in the CLI-fallback phase.
CONTROLLER_REST="https://192.168.10.212:8443"    # REST base URL (login + config)
CONTROLLER_GRPC="192.168.10.212:50051"           # ConfigService gRPC (for fastrg_cli)
CONTROLLER_USER="admin"
CONTROLLER_PASS="admin"
ETCD_ENDPOINT_DEFAULT="192.168.10.212:2378"      # CLI tier-2 fallback target

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
usage() {
    # Print only the header block: from line 1 up to (and including) the closing ===== line
    awk '/^# =+$/{found++} found==1{sub(/^# ?/,""); print} found==2{exit}' "$0"
    exit 0
}

RUN_CASE_MODE=0
VALIDATE_CASE_MODE=0
VALIDATE_CASE_IDS=""

# Second layer behind the pre-check: that one stops a missing value before
# anything is uploaded, this keeps the parser correct on its own.
_need_value() {
    [[ -n "$2" ]] || { error "$1 needs a value — run with --help for usage"; exit 1; }
}

while [[ $# -gt 0 ]]; do
    # Flags here must also appear in the _E2E_*_FLAGS lists near the top; the
    # pre-check rejects anything missing from those.
    case "$1" in
        --help|-h)       usage ;;
        --fastrg-node)   _need_value "$1" "${2:-}"; FASTRG_NODE="$2";      shift 2 ;;
        --lan-host)      _need_value "$1" "${2:-}"; LAN_HOST="$2";         shift 2 ;;
        --wan-host)      _need_value "$1" "${2:-}"; WAN_HOST="$2";         shift 2 ;;
        --wan-ip)        _need_value "$1" "${2:-}"; WAN_IP="$2";           shift 2 ;;
        --runner-host)   _need_value "$1" "${2:-}"; _E2E_RUNNER_HOST="$2"; shift 2 ;;
        --bras-host)     _need_value "$1" "${2:-}"; BRAS_HOST="$2";        shift 2 ;;
        --sub-id)        _need_value "$1" "${2:-}"; SUB_ID_SPEC="$2";      shift 2 ;;
        --ssh-key)       _need_value "$1" "${2:-}"; SSH_KEY="$2";          shift 2 ;;
        --grpc-port)     _need_value "$1" "${2:-}"; FASTRG_GRPC_PORT="$2"; shift 2 ;;
        --controller-rest) _need_value "$1" "${2:-}"; CONTROLLER_REST="$2"; shift 2 ;;
        --controller-grpc) _need_value "$1" "${2:-}"; CONTROLLER_GRPC="$2"; shift 2 ;;
        --controller-user) _need_value "$1" "${2:-}"; CONTROLLER_USER="$2"; shift 2 ;;
        --controller-pass) _need_value "$1" "${2:-}"; CONTROLLER_PASS="$2"; shift 2 ;;
        --run-case)      RUN_CASE_MODE=1; shift ;;
        --local-validate) shift ;;
        --validate-case)
            _need_value "$1" "${2:-}"
            VALIDATE_CASE_MODE=1; VALIDATE_CASE_IDS="$2"; shift 2 ;;
        -*)              error "Unknown option: $1"; exit 1 ;;
        *)
            error "Unexpected argument: $1 (subscriber IDs are now passed via --sub-id)"
            exit 1 ;;
    esac
done

if [[ "$RUN_CASE_MODE" -eq 1 && "$VALIDATE_CASE_MODE" -eq 1 ]]; then
    error "--run-case and --validate-case are separate modes; run with no flag to get both"
    exit 1
fi

# ---------------------------------------------------------------------------
# LAN-side topology. These are facts about the bench the suite runs on — not
# fixtures the test invents — so they belong here beside WAN_NIC rather than
# hard-coded inside individual phases. Every value is environment-overridable;
# the header block documents what each one means. Resolved after argument
# parsing because LAN_FLAP_HOST follows whatever --wan-host ends up being.
#
# An empty LAN_VF_ID means "the LAN peer does not sit on an SR-IOV VF": phases
# then skip the VF-specific assertions and the host-side VF fallbacks instead
# of failing on a bench that has no VF to operate on.
# ---------------------------------------------------------------------------
LAN_FLAP_HOST="${LAN_FLAP_HOST:-${WAN_HOST}}"
LAN_FLAP_NIC="${LAN_FLAP_NIC:-enp1s0f0}"
LAN_PEER_NIC="${LAN_PEER_NIC:-enp8s0}"
LAN_VF_ID="${LAN_VF_ID-0}"

if [[ -n "$LAN_VF_ID" && ! "$LAN_VF_ID" =~ ^[0-9]+$ ]]; then
    error "LAN_VF_ID must be a VF index, or empty on a bench without SR-IOV (got '${LAN_VF_ID}')."
    exit 1
fi

# ---------------------------------------------------------------------------
# Expand --sub-id into the SUB_IDS array.
#   "2,1,3" -> (2 1 3)        "2-10" -> (2 3 4 5 6 7 8 9 10)
#   "2-4,7" -> (2 3 4 7)      duplicates are dropped, first-seen order kept.
# The first id is the primary (drives the full data-plane suite); the rest are
# secondaries (loaded + PPPoE checks only). Defaults to "2,1" when --sub-id is
# omitted.
# ---------------------------------------------------------------------------
[[ -z "$SUB_ID_SPEC" ]] && SUB_ID_SPEC="$SUB_ID_SPEC_DEFAULT"

expand_sub_ids() {
    local spec="$1" tok start end i x y seen
    local -a toks=() out=() uniq=()
    IFS=',' read -ra toks <<< "$spec" || true
    for tok in ${toks[@]+"${toks[@]}"}; do
        tok="${tok//[[:space:]]/}"
        [[ -z "$tok" ]] && continue
        if [[ "$tok" =~ ^[0-9]+-[0-9]+$ ]]; then
            start="${tok%-*}"; end="${tok#*-}"
            if (( start <= end )); then
                for (( i=start; i<=end; i++ )); do out+=("$i"); done
            else
                for (( i=start; i>=end; i-- )); do out+=("$i"); done
            fi
        elif [[ "$tok" =~ ^[0-9]+$ ]]; then
            out+=("$tok")
        else
            error "Invalid --sub-id token: '${tok}' (expected N, N-M, or a comma list)"
            exit 1
        fi
    done
    for x in ${out[@]+"${out[@]}"}; do
        seen=0
        for y in ${uniq[@]+"${uniq[@]}"}; do [[ "$y" == "$x" ]] && { seen=1; break; }; done
        [[ $seen -eq 0 ]] && uniq+=("$x")
    done
    SUB_IDS=(${uniq[@]+"${uniq[@]}"})
}

SUB_IDS=()
expand_sub_ids "$SUB_ID_SPEC"
if [[ ${#SUB_IDS[@]} -eq 0 ]]; then
    error "--sub-id expanded to an empty subscriber list (spec='${SUB_ID_SPEC}')."
    printf "Run '%s --help' for full usage.\n" "$0"
    exit 1
fi

# Primary subscriber (full data-plane suite) + secondaries (lighter checks).
USER_ID="${SUB_IDS[0]}"
SUB_SECONDARY_IDS=("${SUB_IDS[@]:1}")

# Export controller config so fastrg_grpc_client.py routes writes to the controller.
export CONTROLLER_REST CONTROLLER_GRPC CONTROLLER_USER CONTROLLER_PASS

# BRAS host + the SUB_IDS / SUB_SECONDARY_IDS arrays are visible to the phase
# scripts directly (they are sourced, not sub-processed).

# ---------------------------------------------------------------------------
# SSH helper functions
# ---------------------------------------------------------------------------
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes -i ${SSH_KEY}"

ssh_node() { ssh $SSH_OPTS "root@${FASTRG_NODE}" "$@"; }
ssh_lan()  { ssh $SSH_OPTS "root@${LAN_HOST}"    "$@"; }
ssh_wan()  { ssh $SSH_OPTS "root@${WAN_HOST}"   "$@"; }
ssh_bras() { ssh $SSH_OPTS "root@${BRAS_HOST}"   "$@"; }
# Host that owns the LAN-side PF (VF policy + LAN link flaps). It is the WAN
# host on this bench, but it is addressed through its own variable so a bench
# with a separate LAN switch host only has to set LAN_FLAP_HOST.
ssh_lan_flap() { ssh $SSH_OPTS "root@${LAN_FLAP_HOST}" "$@"; }

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
# A read that could not reach etcd prints nothing, exactly like a key that is
# absent. Callers treat empty as "absent", so an unreachable endpoint would be
# read as data. Both helpers therefore say so on stderr and return non-zero;
# callers for which absence is a pass condition must check the status.
_etcdctl_run() {
    local _out _err _rc

    # stderr is kept apart from the value. Merging the two put any ssh or
    # etcdctl warning into the string the callers parse, and a successful read
    # would then hand jq something that is not the stored value.
    _err=$(mktemp 2>/dev/null) || _err=/dev/null
    _out=$(ssh_node "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} $*" 2>"$_err")
    _rc=$?
    if [[ $_rc -ne 0 ]]; then
        printf '[ERROR] etcd read failed against %s: %s\n' \
            "${ETCD_ENDPOINT}" "$(tr '\n' ' ' < "$_err" | cut -c 1-200)" >&2
        if [[ "$_err" != /dev/null ]]; then rm -f "$_err"; fi
        return 1
    fi
    if [[ "$_err" != /dev/null ]]; then rm -f "$_err"; fi
    printf '%s' "$_out"
}

etcdctl_get() {
    _etcdctl_run "get $*"
}

etcdctl_get_value() {
    # etcdctl prints key on one line, value on next; we want only the value
    _etcdctl_run "get --print-value-only $*"
}

# Same RPC as fastrg_grpc, except the caller finds out whether the node
# answered. fastrg_grpc ends in "|| true" because most callers only want a best
# effort; a loop that is waiting for something to happen needs to know the
# difference between "not yet" and "nobody answered".
fastrg_grpc_checked() {
    python3 "${GRPC_CLIENT_DIR}/fastrg_grpc_client.py" \
        --node "${FASTRG_NODE}:${FASTRG_GRPC_PORT}" "$@" 2>/dev/null
}

# How many unanswered checks in a row mean the resource is not coming back.
# More than one so a single hiccup still gets retried.
_E2E_UNANSWERED_LIMIT=3

# Wait until a condition holds, and stop early once whatever answers the
# condition has gone quiet.
#
# Every waiting loop in this suite asks something remote. While it answers, a
# retry is worth the wait; once it stops answering, every further attempt only
# burns its own timeout, and enough of those in a row is what leaves an
# interrupted run without time to finish cleaning up. Rather than each loop
# probing its own resource, the condition itself says which case it is:
#
#     0  the condition holds
#     1  not yet, ask again
#     2  could not ask at all
#
# _e2e_wait_for answers in the same three, so a caller can react to "never got
# an answer" differently from "asked plenty and it never became true". Neither
# of those is success: an unanswered check is never counted as a condition met.
#
# usage: _e2e_wait_for <label> <attempts> <seconds between> <condition> [args...]
_e2e_wait_for() {
    local _label="$1" _attempts="$2" _delay="$3"

    shift 3
    local _i _rc _unanswered=0

    for ((_i = 1; _i <= _attempts; _i++)); do
        _rc=0
        "$@" || _rc=$?
        if [[ "$_rc" -eq 0 ]]; then
            return 0
        elif [[ "$_rc" -ge 2 ]]; then
            _unanswered=$((_unanswered + 1))
            if [[ "$_unanswered" -ge "$_E2E_UNANSWERED_LIMIT" ]]; then
                warn "${_label}: no answer ${_unanswered} times running; giving up with the state unconfirmed."
                return 2
            fi
        else
            _unanswered=0
        fi
        # No wait after the last attempt: nothing follows it.
        if [[ "$_i" -lt "$_attempts" ]]; then
            sleep "$_delay"
        fi
    done

    warn "${_label}: still not true after ${_attempts} checks."
    return 1
}

# Condition: the etcd key is gone. Reading an absent key succeeds and returns
# nothing, so a failed read means etcd could not be asked.
_e2e_etcd_key_absent() {
    local _value

    _value=$(etcdctl_get_value "$1") || return 2
    [[ -z "$_value" ]]
}

# Remove an HSI config through the normal gRPC path, verify the etcd key is
# gone, and fall back to a direct etcd delete when the gRPC cleanup is lost.
# Cleanup callers should append `|| true`: a failure is deliberately loud but
# must not abort the EXIT trap or the remaining cleanup work.
remove_hsi_config_verified() {
    local _uid="$1"
    local _key="configs/${NODE_UUID}/hsi/${_uid}"
    local _remaining=""
    local _summary=""
    local _rc

    fastrg_grpc remove_config "${_uid}" >/dev/null 2>&1 || true
    _rc=0
    _e2e_wait_for "Cleanup: waiting for ${_key} to disappear" 5 1 _e2e_etcd_key_absent "${_key}" || _rc=$?
    if [[ "$_rc" -eq 0 ]]; then
        info "Cleanup: verified ${_key} is absent."
        return 0
    fi
    if [[ "$_rc" -ge 2 ]]; then
        warn "Cleanup: etcd never answered, so ${_key} is left unverified and may still exist."
        return 1
    fi

    warn "Cleanup: ${_key} still exists after RemoveConfig; deleting it directly from etcd."
    ssh_node "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} del ${_key}" >/dev/null 2>&1 || true
    if ! _remaining=$(etcdctl_get_value "${_key}"); then
        warn "Cleanup: cannot reach etcd to confirm the direct delete of ${_key}."
        return 1
    fi
    if [[ -z "$_remaining" ]]; then
        info "Cleanup: verified direct etcd delete removed ${_key}."
        return 0
    fi

    _summary=$(printf '%s' "$_remaining" | jq -c \
        '{user_id:(.config.user_id // null),vlan_id:(.config.vlan_id // null),updatedAt:(.metadata.updatedAt // null)}' \
        2>/dev/null || true)
    [[ -z "$_summary" ]] && _summary=$(printf '%s' "$_remaining" | tr '\n' ' ' | cut -c 1-200 || true)
    warn "Cleanup FAILED: ${_key} remains after direct etcd delete; value=${_summary:-<unavailable>}"
    return 1
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

# Read a user's desired PPPoE state from the controller. Empty on error.
_ctrl_desire_status() {
    fastrg_grpc ctrl_desire "$1" | jq -r '.desire_status // empty' 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Load phase scripts (each file defines one phase function)
# ---------------------------------------------------------------------------
_E2E_PHASES_DIR="${GRPC_CLIENT_DIR}/phases"
# shellcheck source=/dev/null
# Registration APIs for both validation layers; must be sourced before anything
# that registers a predicate or a sabotage entry.
source "${_E2E_PHASES_DIR}/local_validation_lib.sh"
source "${_E2E_PHASES_DIR}/case_validation_lib.sh"
# Shared /metrics sampling helpers — not a phase; must be sourced first.
source "${_E2E_PHASES_DIR}/metrics_lib.sh"
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
source "${_E2E_PHASES_DIR}/phase8_cli_config_sync.sh"
source "${_E2E_PHASES_DIR}/phase9_cli_fallback.sh"
source "${_E2E_PHASES_DIR}/phase10_desire_diff.sh"
source "${_E2E_PHASES_DIR}/phase11_kafka_pipeline.sh"
source "${_E2E_PHASES_DIR}/phase12_rollback.sh"
source "${_E2E_PHASES_DIR}/phase13_pdump.sh"
source "${_E2E_PHASES_DIR}/phase14_stress_test.sh"
source "${_E2E_PHASES_DIR}/phase15_metrics_route.sh"
source "${_E2E_PHASES_DIR}/phase16_rcu_concurrency.sh"
source "${_E2E_PHASES_DIR}/phase17_etcd_offline_queue.sh"
source "${_E2E_PHASES_DIR}/phase18_dns_cache.sh"
source "${_E2E_PHASES_DIR}/phase19_node_restart.sh"
source "${_E2E_PHASES_DIR}/phase20_nat_expiry.sh"
source "${_E2E_PHASES_DIR}/phase21_rpc_coverage.sh"
source "${_E2E_PHASES_DIR}/phase22_dhcp_lease.sh"
source "${_E2E_PHASES_DIR}/phase23_hsi_sweep.sh"
source "${_E2E_PHASES_DIR}/phase24_multi_lan.sh"
source "${_E2E_PHASES_DIR}/phase25_udp_icmp_traffic.sh"
source "${_E2E_PHASES_DIR}/phase26_heartbeat_reregister.sh"
source "${_E2E_PHASES_DIR}/phase27_link_flap.sh"
source "${_E2E_PHASES_DIR}/phase28_chap_auth.sh"
source "${_E2E_PHASES_DIR}/phase29_protocol_reject.sh"
source "${_E2E_PHASES_DIR}/phase30_keepalive_failure.sh"
source "${_E2E_PHASES_DIR}/phase31_subscriber_scale.sh"
source "${_E2E_PHASES_DIR}/phase32_metric_values.sh"
source "${_E2E_PHASES_DIR}/phase33_shutdown_inactive.sh"
source "${_E2E_PHASES_DIR}/phase34_wan_long_outage.sh"
source "${_E2E_PHASES_DIR}/phase35_ipv6.sh"
source "${_E2E_PHASES_DIR}/phase36_nat_capacity.sh"
source "${_E2E_PHASES_DIR}/phase37_ipv6_firewall.sh"
source "${_E2E_PHASES_DIR}/phase38_summary.sh"

# ---------------------------------------------------------------------------
# Cleanup — kill fastrg only if the script started it
# ---------------------------------------------------------------------------
# Running on the runner, this instance was started over ssh. The caller dying
# does not signal us, so nothing would run the cleanup below and a phase
# interrupted mid-sabotage would leave the bench altered. Watch for our own
# reparenting instead: when the ssh session ends we are handed to init, within a
# second of the client dying. The signal we send ourselves is deliberately left
# untrapped, because a TERM handler would be deferred until the current sleep
# returned and an interrupt can land anywhere.

# Progress notes for an interrupted run. The usual output path is the ssh
# channel back to the caller, which is gone in exactly the situation these
# notes are about, so they are written on the runner instead.
_trace_interrupt() {
    [[ -n "${_FASTRG_E2E_RELOCATED:-}" ]] || return 0
    printf '%s %s\n' "$(date '+%F %T.%3N')" "$*" >> "$_E2E_TRACE_LOG"
}

# An interrupt leaves whatever command was running in the foreground as a child
# of this script, and bash runs the exit trap without waiting for it, so that
# child would outlive the run as an orphan. Called at the very end of cleanup,
# once every cleanup step has returned, so that no ssh or helper a step was
# still using is taken down with it. The process tree is snapshotted once, so
# the processes the walk itself forks are never in the set.
_e2e_reap_descendants() {
    [[ -n "${_FASTRG_E2E_RELOCATED:-}" ]] || return 0

    local _snapshot _frontier="$$" _found="" _next _pid _ppid

    _snapshot=$(ps -eo pid=,ppid= 2>/dev/null)
    while [[ -n "$_frontier" ]]; do
        _next=""
        while read -r _pid _ppid; do
            [[ -z "$_pid" ]] && continue
            case " $_frontier " in
                *" $_ppid "*) _next="$_next $_pid"; _found="$_found $_pid" ;;
            esac
        done <<< "$_snapshot"
        _frontier="$_next"
    done

    for _pid in $_found; do
        [[ "$_pid" == "$$" ]] && continue
        # The snapshot also caught the short-lived processes the walk needed;
        # skip anything already gone so a recycled pid is never signalled.
        [[ -d "/proc/$_pid" ]] || continue
        kill -TERM "$_pid" 2>/dev/null
    done
}

start_orphan_watchdog() {
    [[ -n "${_FASTRG_E2E_RELOCATED:-}" ]] || return 0

    local _target=$$
    (
        while kill -0 "$_target" 2>/dev/null; do
            if [[ "$(ps -o ppid= -p "$_target" 2>/dev/null | tr -d ' ')" == "1" ]]; then
                # stderr goes back over the ssh channel that just died, so the
                # only record that survives is the one written here.
                printf '%s caller connection is gone; stopping so cleanup can run\n' \
                    "$(date '+%F %T')" >> "$_E2E_TRACE_LOG"
                printf '[INFO]  caller connection is gone; stopping so cleanup can run\n' >&2
                kill -TERM "$_target" 2>/dev/null
                return 0
            fi
            sleep 2
        done
    ) &
    _E2E_WATCHDOG_PID=$!
}

stop_orphan_watchdog() {
    [[ -n "${_E2E_WATCHDOG_PID:-}" ]] || return 0
    kill -TERM "$_E2E_WATCHDOG_PID" 2>/dev/null || true
    _E2E_WATCHDOG_PID=""
}

cleanup_fastrg() {
    set +eu  # Prevent set -e / set -u from interrupting cleanup, ensure all cleanup steps are executed

    # From here on the run is being torn down, and a signal arriving now would
    # kill bash in the middle of the exit trap: the remaining cleanup steps
    # would never run and the bench would keep the etcd block, the node process
    # and whatever else this run installed. Two senders can each deliver a
    # TERM -- the caller relaying the interrupt, and the runner-side watchdog
    # noticing the connection went away -- so the second one lands while
    # cleanup is already in progress. Ignoring both TERM and INT means whoever
    # arrives second cannot cut the cleanup short. The cost is that cleanup can
    # no longer be interrupted: stopping it now takes SIGKILL.
    trap '' TERM INT

    # Set before any cleanup step runs, so every message a step prints is
    # routed away from the possibly-dead caller channel.
    _E2E_IN_CLEANUP=1
    : > "$_E2E_CLEANUP_LOG" 2>/dev/null

    stop_orphan_watchdog
    _trace_interrupt "cleanup: entered cleanup_fastrg"

    # First, because it is what lifts the node->etcd block this phase installs.
    # Every later step that reads etcd pays a full timeout per read while the
    # block is up: with it still in place at the end of the list, a cleanup that
    # otherwise takes seconds took 216s, and each step after the unblock
    # finished in under a second. Its other work -- bringing the node back up
    # after the step-69c restart, removing its test user, restoring the
    # subscriber count -- is also better done before the steps that expect the
    # node to answer.
    _cleanup_phase17_etcd_offline_queue 2>/dev/null || true
    _cleanup_phase0_setup 2>/dev/null || true
    _cleanup_phase11_kafka_pipeline || true
    _cleanup_phase20_nat_expiry 2>/dev/null || true
    _cleanup_phase19_node_restart 2>/dev/null || true
    _cleanup_phase22_dhcp_lease 2>/dev/null || true
    _cleanup_phase23_hsi_sweep 2>/dev/null || true
    _cleanup_phase24_multi_lan 2>/dev/null || true
    _cleanup_phase25_udp_icmp_traffic 2>/dev/null || true
    _cleanup_phase26_heartbeat_reregister 2>/dev/null || true
    _cleanup_phase27_link_flap 2>/dev/null || true
    _cleanup_phase28_chap_auth 2>/dev/null || true
    _cleanup_phase29_protocol_reject 2>/dev/null || true
    _cleanup_phase30_keepalive_failure 2>/dev/null || true
    _cleanup_phase31_subscriber_scale 2>/dev/null || true
    _cleanup_phase33_shutdown_inactive 2>/dev/null || true
    _cleanup_phase34_wan_long_outage 2>/dev/null || true
    _cleanup_phase35_ipv6 2>/dev/null || true
    _cleanup_phase36_nat_capacity 2>/dev/null || true
    _cleanup_phase37_ipv6_firewall 2>/dev/null || true

    # Best-effort: remove new subscriber config if the test left it in etcd
    _cleanup_new_subscriber_config 2>/dev/null || true
    _cleanup_phase9_user 2>/dev/null || true
    _cleanup_phase9_cli_fallback 2>/dev/null || true
    _cleanup_phase12_rollback 2>/dev/null || true
    _cleanup_phase16_rcu_concurrency 2>/dev/null || true
    _cleanup_phase18_dns_cache 2>/dev/null || true

    if [[ "${_FASTRG_STARTED_BY_SCRIPT:-0}" -eq 1 ]]; then
        info "Stopping fastrg (started by this script)..."
        ssh_node "pkill -x fastrg 2>/dev/null || true" || true
        info "fastrg stopped."

        # A config-apply failure emitted while phase17 removes its offline
        # test user can race with controller rollback and resurrect an older
        # SSOT record after the first verified delete. Once the node is down,
        # temporarily make that user ID valid, delete it through the
        # controller, then restore the canonical count without another node
        # apply-failure event re-creating the key.
        if [[ -n "${_P17_UID:-}" ]] && [[ -n "${_P17_ORIG_SC:-}" ]]; then
            local _p17_post_stop_count=$(( _P17_UID + 1 ))
            info "Cleanup(phase17): clearing post-stop controller residue for user ${_P17_UID}..."
            fastrg_grpc set_subscriber_count "${_p17_post_stop_count}" >/dev/null 2>&1 || true
            remove_hsi_config_verified "${_P17_UID}" || true
            fastrg_grpc set_subscriber_count "${_P17_ORIG_SC}" >/dev/null 2>&1 || true
        fi
    fi

    # Kill dpdk-bras on the BRAS endpoint if this script launched it.
    if [[ "${_BRAS_STARTED_BY_SCRIPT:-0}" -eq 1 ]]; then
        info "Stopping dpdk-bras on BRAS endpoint (${BRAS_HOST})..."
        ssh_bras "pkill -x dpdk-bras 2>/dev/null || true" 2>/dev/null || true
        info "dpdk-bras stopped."
    fi

    # Restore USER_ID subscriber to desire_status="connect" so the next run starts
    # clean. Phase9's hangup may have left it "disconnect". Done via the controller
    # (the node is read-only on etcd); falls back to a direct etcd write.
    if [[ -n "${USER_ID:-}" ]] && [[ -n "${NODE_UUID:-}" ]]; then
        _cur_ds=$(_ctrl_desire_status "${USER_ID}" 2>/dev/null || true)
        if [[ "$_cur_ds" != "connect" ]]; then
            info "Cleanup: restoring USER_ID=${USER_ID} desire_status to 'connect' (was '${_cur_ds:-unknown}')..."
            python3 "${GRPC_CLIENT_DIR}/fastrg_grpc_client.py" \
                --node "${FASTRG_NODE}:${FASTRG_GRPC_PORT}" connect_hsi "${USER_ID}" \
                >/dev/null 2>&1 || true
        fi
    fi

    _e2e_reap_descendants

    _trace_interrupt "cleanup: finished normally"
    info "Cleanup complete."
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    _FASTRG_STARTED_BY_SCRIPT=0
    _BRAS_STARTED_BY_SCRIPT=0

    # Clean up anyway on exit
    trap 'cleanup_fastrg' EXIT
    start_orphan_watchdog

    printf "\n"
    bold "╔═════════════════════════════════════════════════════╗"
    bold "║   FastRG Node — E2E Data Plane Test                 ║"
    bold "╚═════════════════════════════════════════════════════╝"
    printf "\n"
    info "Subscribers: ${SUB_IDS[*]} (primary=${USER_ID}, secondaries='${SUB_SECONDARY_IDS[*]:-none}')"
    # Announced here, while the caller channel is known to be healthy, because
    # cleanup output is written on the runner and would otherwise be invisible
    # to whoever started the run.
    if [[ -n "${_FASTRG_E2E_RELOCATED:-}" ]]; then
        info "Cleanup messages are written on the runner: ${_E2E_CLEANUP_LOG} (interrupt trace: ${_E2E_TRACE_LOG})"
    fi
    printf "\n"

    # A broken assertion cannot be told from a passing system, so the suite
    # verifies its own predicates before it trusts any of them.
    if ! local_validation_run; then
        error "Local correctness validation failed — assertion logic is unreliable, aborting"
        exit 1
    fi

    if [[ "$VALIDATE_CASE_MODE" -eq 1 ]]; then
        phase0_setup
        [[ "$VALIDATE_CASE_IDS" == "all" ]] && VALIDATE_CASE_IDS=""
        case_validation_run "$VALIDATE_CASE_IDS"
        exit $?
    fi

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
    phase8_cli_config_sync
    phase9_cli_fallback
    phase10_desire_diff
    phase11_kafka_pipeline
    phase12_rollback
    phase13_pdump
    phase14_stress_test
    phase15_metrics_route
    phase16_rcu_concurrency
    phase17_etcd_offline_queue
    phase18_dns_cache
    phase19_node_restart
    phase20_nat_expiry
    phase21_rpc_coverage
    phase22_dhcp_lease
    phase23_hsi_sweep
    phase24_multi_lan
    phase25_udp_icmp_traffic
    phase26_heartbeat_reregister
    phase27_link_flap
    phase28_chap_auth
    phase29_protocol_reject
    phase30_keepalive_failure
    phase31_subscriber_scale
    phase32_metric_values
    phase33_shutdown_inactive
    phase34_wan_long_outage
    phase35_ipv6
    phase36_nat_capacity
    phase37_ipv6_firewall
    phase38_summary || true

    # Exit code mirrors the RESULT line: any failed step fails the run. Counted
    # here from the same results the summary printed, so a summary that breaks
    # still leaves the exit code right.
    local _fails=0
    if [[ ${#STEP_RESULTS[@]} -gt 0 ]]; then
        _fails=$(printf '%s\n' "${STEP_RESULTS[@]}" | grep -cx 'FAIL' || true)
    fi
    if [[ "$_fails" -gt 0 ]]; then
        # Drills deliberately break the system; running them on an already
        # broken one produces failures nobody can attribute.
        [[ "$RUN_CASE_MODE" -eq 1 ]] || printf 'VALIDATE SKIPPED: normal suite failed\n'
        exit 1
    fi
    [[ "$RUN_CASE_MODE" -eq 1 ]] && exit 0

    # No mode flag: the drills follow the clean suite, so a cleanup bug in one
    # of them cannot leak backwards into the run that just passed.
    case_validation_run "" || exit 1
    exit 0
}

main "$@"
