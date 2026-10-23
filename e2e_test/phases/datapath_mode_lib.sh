#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Node data-plane mode: read it from the node's stdout, check the lcore roles
# that go with it, switch it via EnableDDP.
# A saved config copy on the node means a restore is still owed. Not a phase.
# ---------------------------------------------------------------------------

# e2e_node_datapath_mode LOG — mode in the last "datapath:" line of LOG:
# rss | distributor | err.
e2e_node_datapath_mode() {
    local _line

    _line=$(printf '%s\n' "${1:-}" | grep -a 'datapath: ' | tail -n 1 || true)
    case "$_line" in
        *'datapath: RSS multi-queue,'*)       printf 'rss' ;;
        *'datapath: software distributor,'*)  printf 'distributor' ;;
        *)                                     printf 'err'; return 1 ;;
    esac
    return 0
}

local_validation_register node_datapath_mode e2e_node_datapath_mode \
    datapath_mode_rss \
    datapath_mode_distributor \
    datapath_mode_last_start_wins \
    datapath_mode_line_missing \
    datapath_mode_unknown_mode \
    datapath_mode_empty_input

# e2e_dist_worker_roles WAN_WORKERS LAN_WORKERS DATA_LCORES — true when both
# distributor worker roles have an lcore and no RSS data lcore is left.
e2e_dist_worker_roles() {
    e2e_all_uint "${1:-}" "${2:-}" "${3:-}" || return 1
    (( 10#$1 >= 1 && 10#$2 >= 1 && 10#$3 == 0 ))
}

local_validation_register dist_worker_roles e2e_dist_worker_roles \
    dist_worker_roles_good \
    dist_worker_roles_wan_missing \
    dist_worker_roles_lan_missing \
    dist_worker_roles_data_lcores_present \
    dist_worker_roles_empty_input \
    dist_worker_roles_non_numeric

# The "datapath:" lines of the running fastrg's own stdout file.
_E2E_DATAPATH_LINES_SCRIPT='
pid=$(pgrep -xo fastrg) || exit 0
out=$(readlink -f "/proc/$pid/fd/1" 2>/dev/null) || exit 0
[ -f "$out" ] && grep -a "datapath: " "$out"
'

# Save the config once, then set EnableDDP = false through an atomic rename.
_E2E_DDP_OFF_SCRIPT='
cfg=/etc/fastrg/config.cfg
orig=/etc/fastrg/config.cfg.e2e-datapath-orig
tmp=/etc/fastrg/config.cfg.e2e-datapath-tmp
[ -f "$cfg" ] || { echo "no $cfg"; exit 1; }
if [ ! -e "$orig" ]; then
    cp -p "$cfg" "$orig" || { echo "cannot save $cfg"; exit 1; }
fi
cp -p "$cfg" "$tmp" || { echo "cannot stage $tmp"; exit 1; }
if grep -qE "^[[:space:]]*EnableDDP[[:space:]]*=" "$cfg"; then
    sed -E "s/^[[:space:]]*EnableDDP[[:space:]]*=.*/EnableDDP = false;/" "$cfg" > "$tmp"
else
    { cat "$cfg"; [ -z "$(tail -c 1 "$cfg")" ] || echo; echo "EnableDDP = false;"; } > "$tmp"
fi || { echo "cannot write $tmp"; exit 1; }
mv -f "$tmp" "$cfg" || { echo "cannot install $tmp"; exit 1; }
grep -E "^[[:space:]]*EnableDDP" "$cfg"
'

# Put the saved config back; drop the copy only once both compare equal.
_E2E_DDP_RESTORE_SCRIPT='
cfg=/etc/fastrg/config.cfg
orig=/etc/fastrg/config.cfg.e2e-datapath-orig
tmp=/etc/fastrg/config.cfg.e2e-datapath-tmp
[ -e "$orig" ] || { echo none; exit 0; }
if cp -p "$orig" "$tmp" && mv -f "$tmp" "$cfg" && cmp -s "$orig" "$cfg"; then
    rm -f "$orig"
    echo restored
else
    echo failed
    exit 1
fi
'

# "<config sha256> <owed|clear> <original EnableDDP line, or absent>".
_E2E_DDP_STATE_SCRIPT='
cfg=/etc/fastrg/config.cfg
orig=/etc/fastrg/config.cfg.e2e-datapath-orig
sum=$(sha256sum "$cfg" 2>/dev/null | cut -d" " -f1)
src=$cfg
owed=clear
if [ -e "$orig" ]; then owed=owed; src=$orig; fi
line=$(grep -E "^[[:space:]]*EnableDDP[[:space:]]*=" "$src" 2>/dev/null | head -n 1)
printf "%s %s %s\n" "${sum:-unreadable}" "$owed" "${line:-absent}"
'

# Mode of the running node; err when it is down or has not printed it yet.
e2e_node_running_datapath_mode() {
    local _lines

    _lines=$(ssh_node "$_E2E_DATAPATH_LINES_SCRIPT" 2>/dev/null || true)
    e2e_node_datapath_mode "$_lines"
}

e2e_node_ddp_state() {
    ssh_node "$_E2E_DDP_STATE_SCRIPT" 2>/dev/null | tail -n 1 || true
}

# Prints the EnableDDP line now in force; non-zero when the switch failed.
e2e_node_ddp_off() {
    ssh_node "$_E2E_DDP_OFF_SCRIPT" 2>&1
}

# Prints none, restored or failed; non-zero only for failed.
e2e_node_ddp_restore() {
    local _out

    _out=$(ssh_node "$_E2E_DDP_RESTORE_SCRIPT" 2>/dev/null | tail -n 1 || true)
    printf '%s' "${_out:-failed}"
    [[ "$_out" == "none" || "$_out" == "restored" ]]
}

# SIGTERM the node, start it with phase0's command and wait for its mode line.
# Sets _E2E_DP_RESTART_MODE; on failure also _E2E_DP_RESTART_ISSUE.
_E2E_DP_RESTART_MODE=""
_E2E_DP_RESTART_ISSUE=""
e2e_node_restart_for_datapath() {
    local _mark="" _crash="" _i _stopped=0 _mode="err"

    _E2E_DP_RESTART_MODE="err"
    _E2E_DP_RESTART_ISSUE=""
    if ssh_node "pgrep -x fastrg >/dev/null 2>&1" 2>/dev/null; then
        _mark=$(e2e_node_stop_mark)
        ssh_node "pkill -x fastrg" >/dev/null 2>&1 || true
        for _i in $(seq 1 30); do
            if ! ssh_node "pgrep -x fastrg >/dev/null 2>&1" 2>/dev/null; then
                _stopped=1
                break
            fi
            sleep 1
        done
        if [[ $_stopped -ne 1 ]]; then
            _E2E_DP_RESTART_ISSUE="fastrg still running 30s after SIGTERM"
            return 1
        fi
        if ! _crash=$(e2e_node_shutdown_check "$_mark"); then
            _E2E_DP_RESTART_ISSUE="unclean stop: ${_crash}"
        fi
    fi

    if ! e2e_start_node >/dev/null 2>&1; then
        _E2E_DP_RESTART_ISSUE="${_E2E_DP_RESTART_ISSUE:+${_E2E_DP_RESTART_ISSUE}; }start command failed"
    fi
    _FASTRG_STARTED_BY_SCRIPT=1
    for _i in $(seq 1 30); do
        sleep 2
        _mode=$(e2e_node_running_datapath_mode || true)
        [[ "$_mode" != "err" ]] && break
    done
    _E2E_DP_RESTART_MODE="$_mode"
    if [[ "$_mode" == "err" ]]; then
        _E2E_DP_RESTART_ISSUE="${_E2E_DP_RESTART_ISSUE:+${_E2E_DP_RESTART_ISSUE}; }no datapath line from the new start within 60s"
    fi
    [[ -z "$_E2E_DP_RESTART_ISSUE" ]]
}
