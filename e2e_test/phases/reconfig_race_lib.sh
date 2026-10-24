#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Reconfig under load: drop and re-add a subscriber whose tables are in use.
#
# The primary subscriber is dropped and re-added through the subscriber count
# while the LAN host opens a new NAT mapping and a new IPv6 firewall session
# with every packet. Once the subscriber stops forwarding its two tables can
# only shrink, so a sampler on the node reads /metrics back to back (about one
# read per millisecond) and the predicates below fail a round whose tables
# ever read more than before the drop, or do not end empty.
#
# Not a phase. Phase 36 runs it on RSS, phase 41 on the software distributor.
# It uses phase 37's IPv6 fixture and helpers.
# ---------------------------------------------------------------------------

_E2E_RR_ROUNDS=10
_E2E_RR_PPS=2000            # new flows per second, per address family
# A source port comes back every 15s, after its 10s mapping has expired, so
# every packet is a new flow; the destination port moves on each sweep.
_E2E_RR_SRC_BASE=30000
_E2E_RR_SRC_COUNT=30000
_E2E_RR_DST_BASE=40700
_E2E_RR_DST_COUNT=4
_E2E_RR_MAX_NAT=262144      # MAX_NAT_ENTRIES in src/pppd/pppd.h
_E2E_RR_MAX_FW6=65536       # IPV6_FIREWALL_MAX_ENTRIES in src/pppd/pppd.h
_E2E_RR_ADDR_WAIT=30        # seconds for the LAN host's address in a new prefix
# Sampler health per round, from the drop to Data phase.
_E2E_RR_MIN_SAMPLES=300
_E2E_RR_MAX_MEDIAN_US=2000

# Remote scratch files; every rm below spells these out.
_E2E_RR_SAMPLES=/tmp/e2e_reconfig_race_samples.log        # node
_E2E_RR_SAMPLER_PID=/tmp/e2e_reconfig_race_sampler.pid    # node
_E2E_RR_V4_PID=/tmp/e2e_reconfig_race_v4.pid              # LAN host
_E2E_RR_V4_OUT=/tmp/e2e_reconfig_race_v4.out
_E2E_RR_V6_PID=/tmp/e2e_reconfig_race_v6.pid
_E2E_RR_V6_OUT=/tmp/e2e_reconfig_race_v6.out

# Kept across a drill's re-source: the cleanup may still owe this restore.
: "${_E2E_RR_ORIG_COUNT:=}"
: "${_E2E_RR_COUNT_CHANGED:=0}"

_E2E_RR_ROUND_ISSUE=""
_E2E_RR_ROUND_VERDICT=""
_E2E_RR_ROUND_ENTRIES=""
_E2E_RR_ROUND_ADDR_WAIT=""

# e2e_reconfig_race_samples RAW USER_ID — one row per sampler line:
# "<epoch us> <fastrg pid> <nat used> <fw6 used> <data sessions>".
# A value the scrape did not carry is "-": the user's two values when the
# user is beyond the subscriber count, everything when the scrape failed.
e2e_reconfig_race_samples() {
    printf '%s\n' "${1:-}" | awk -v want="user_id=\"${2:-}\"" '
        BEGIN { FS = " [|] " }
        {
            split($1, head, " ")
            if (head[1] != "@")
                next
            nat = "-"; fw6 = "-"; data = "-"
            for (i = 2; i <= NF; i++) {
                n = split($i, f, " ")
                if (index($i, "fastrg_node_total_pppoe_data_sessions{") == 1)
                    data = f[n]
                else if (index($i, "fastrg_node_per_user_nat_entries_used{") == 1 && index($i, want) > 0)
                    nat = f[n]
                else if (index($i, "fastrg_node_per_user_ipv6_firewall_entries_used{") == 1 && index($i, want) > 0)
                    fw6 = f[n]
            }
            print head[2], (head[3] == "" ? "-" : head[3]), nat, fw6, data
        }
    ' || true
}

# e2e_reconfig_race_verdict ROWS MAX_NAT MAX_FW6 MIN_SAMPLES MAX_MEDIAN_US —
# judge one round. The user row leaving /metrics marks the drop, its return
# the re-add; the round ends when the data sessions are back to the count
# before the drop. From the drop to that point:
#   - neither table reads above its peak over the last 1 s before the drop,
#     nor above its MAX;
#   - there are MIN_SAMPLES samples or more, median gap MAX_MEDIAN_US or less;
#   - after the re-add both tables read 0 within 1 s and stay 0.
# Prints "ok samples=... median_gap=... ..." or what broke.
e2e_reconfig_race_verdict() {
    local _out=""

    _out=$(printf '%s\n' "${1:-}" | awk -v maxnat="${2:-}" -v maxfw6="${3:-}" \
            -v minsamples="${4:-}" -v maxmedian="${5:-}" '
        function uint(v) { return v ~ /^[0-9]+$/ }
        function ms(us) { return sprintf("%.1fms", us / 1000) }
        NF == 5 { n++; T[n] = $1; P[n] = $2; N[n] = $3; F[n] = $4; D[n] = $5; next }
        NF > 0  { bad = 1 }
        END {
            if (!uint(maxnat) || !uint(maxfw6) || !uint(minsamples) || !uint(maxmedian)) {
                print "bad_limits"; exit 1
            }
            if (bad) { print "bad_row"; exit 1 }
            if (n == 0) { print "no_samples"; exit 1 }
            t0 = T[1]
            for (i = 1; i <= n; i++) {
                if (!uint(T[i]) || (i > 1 && T[i] + 0 < T[i - 1] + 0)) { print "bad_row"; exit 1 }
                if (i > 1 && T[i] - T[i - 1] > 1000000) {
                    printf "sampler_gap %s at +%s\n", ms(T[i] - T[i - 1]), ms(T[i - 1] - t0); exit 1
                }
                if (P[i] == "-") { printf "node_down at +%s\n", ms(T[i] - t0); exit 1 }
                if (P[i] != P[1]) {
                    printf "node_restarted at +%s (pid %s -> %s)\n", ms(T[i] - t0), P[1], P[i]; exit 1
                }
                if (!uint(D[i])) { printf "scrape_failed at +%s\n", ms(T[i] - t0); exit 1 }
                if ((N[i] == "-") != (F[i] == "-")) {
                    printf "partial_row at +%s (nat=%s fw6=%s)\n", ms(T[i] - t0), N[i], F[i]; exit 1
                }
                if (N[i] != "-" && (!uint(N[i]) || !uint(F[i]) || N[i] + 0 > maxnat + 0 || F[i] + 0 > maxfw6 + 0)) {
                    printf "out_of_range at +%s (nat=%s fw6=%s)\n", ms(T[i] - t0), N[i], F[i]; exit 1
                }
            }
            # The drop: the first sample without the user row.
            a = 0
            for (i = 1; i <= n; i++) if (N[i] == "-") { a = i; break }
            last_pre = a ? a - 1 : n
            peak_nat = 0; peak_fw6 = 0; load = 0
            for (i = 1; i <= last_pre; i++) {
                if (N[i] + 0 > peak_nat) peak_nat = N[i] + 0
                if (F[i] + 0 > peak_fw6) peak_fw6 = F[i] + 0
                if (N[i] > 0 && F[i] > 0) load = 1
            }
            if (!load) { printf "no_load before the drop (peak nat=%d fw6=%d)\n", peak_nat, peak_fw6; exit 1 }
            if (!a) { printf "no_trigger: the user row never left /metrics (last nat=%s fw6=%s)\n", N[n], F[n]; exit 1 }
            # Before the drop: peaks over its last second. The scrape that
            # straddles the drop can already count the session as down.
            pn = 0; pf = 0; expect = 0
            for (i = last_pre; i >= 1 && T[a] - T[i] <= 1000000; i--) {
                if (N[i] + 0 > pn) pn = N[i] + 0
                if (F[i] + 0 > pf) pf = F[i] + 0
                if (D[i] + 0 > expect) expect = D[i] + 0
            }
            # The re-add: the first sample with the row back.
            r = 0
            for (i = a + 1; i <= n; i++) if (N[i] != "-") { r = i; break }
            if (!r) { printf "row_never_returned (dropped at +%s)\n", ms(T[a] - t0); exit 1 }
            tr = T[r]
            for (i = r; i <= n; i++) if (N[i] == "-") { printf "row_vanished_again at +%s\n", ms(T[i] - tr); exit 1 }
            d = 0
            for (i = r; i <= n; i++) if (D[i] + 0 == expect) { d = i; break }
            if (!d) { printf "no_data_phase (data sessions %s, expected %d, +%s)\n", D[n], expect, ms(T[n] - tr); exit 1 }
            # The tables cannot grow once the subscriber stopped forwarding.
            above = 0; first = 0; mn = 0; mf = 0
            for (i = a; i < d; i++) {
                if (N[i] == "-") continue
                if (N[i] + 0 > mn) mn = N[i] + 0
                if (F[i] + 0 > mf) mf = F[i] + 0
                if (N[i] + 0 > pn || F[i] + 0 > pf) { above++; if (!first) first = i }
            }
            if (above) {
                printf "above_pre_drop at +%s after the drop (nat=%s fw6=%s; before the drop nat=%d fw6=%d; %d of %d samples above, peak nat=%d fw6=%d)\n",
                    ms(T[first] - T[a]), N[first], F[first], pn, pf, above, d - a, mn, mf
                exit 1
            }
            # Sampler health: enough samples, median gap small enough.
            g = 0; maxg = 0
            for (i = a + 1; i <= d; i++) { G[++g] = T[i] - T[i - 1]; if (G[g] > maxg) maxg = G[g] }
            lo = 0; hi = maxg; need = int((g + 1) / 2)
            while (lo < hi) {
                mid = int((lo + hi) / 2); le = 0
                for (i = 1; i <= g; i++) if (G[i] <= mid) le++
                if (le >= need) hi = mid; else lo = mid + 1
            }
            if (d - a < minsamples + 0 || lo > maxmedian + 0) {
                printf "sampler_sparse (%d samples from the drop to Data phase, median gap %dus; need >= %d and <= %dus)\n",
                    d - a, lo, minsamples, maxmedian
                exit 1
            }
            # After the re-add: empty within 1 s, and empty until Data phase.
            z = 0; last = r
            for (i = r; i <= n && T[i] - tr <= 1000000; i++) {
                last = i
                if (N[i] == 0 && F[i] == 0) { z = i; break }
            }
            if (!z) { printf "not_zero_within_1s (nat=%s fw6=%s at +%s)\n", N[last], F[last], ms(T[last] - tr); exit 1 }
            if (d <= z) { printf "data_before_zero at +%s\n", ms(T[d] - tr); exit 1 }
            for (i = z; i < d; i++) {
                if (N[i] != 0 || F[i] != 0) {
                    printf "nonzero_before_data at +%s (nat=%s fw6=%s)\n", ms(T[i] - tr), N[i], F[i]; exit 1
                }
            }
            printf "ok samples=%d median_gap=%dus peak_after_drop=%d/%d pre_drop=%d/%d zero_after=%s data_after=%s\n",
                d - a, lo, mn, mf, pn, pf, ms(T[z] - tr), ms(T[d] - tr)
        }
    ') || { printf '%s' "${_out:-awk_failed}"; return 1; }
    printf '%s' "$_out"
}

# e2e_nat_entries_unique JSON — GetNatEntries carries no slot index, so a slot
# handed out twice shows up as two live entries under one key. Checks the
# reverse key (proto, dst ip, dst port, nat port) and the forward key (proto,
# src ip, src port, dst ip, dst port). Prints "ok <entries>" or the duplicate.
e2e_nat_entries_unique() {
    local _out=""

    _out=$(printf '%s' "${1:-}" | jq -r '
        def dup(f): [.entries[] | f] | group_by(.) | map(select(length > 1)) | first;
        if (.entries | type) != "array" then "unreadable"
        else
            dup([.proto, .dst_ip, .dst_port, .nat_port]) as $r
            | dup([.proto, .src_ip, .src_port, .dst_ip, .dst_port]) as $f
            | if $r != null then "dup_reverse \($r[0] | map(tostring) | join(" ")) x\($r | length)"
              elif $f != null then "dup_forward \($f[0] | map(tostring) | join(" ")) x\($f | length)"
              else "ok \(.entries | length)" end
        end' 2>/dev/null) || _out=""
    printf '%s' "${_out:-unreadable}"
    [[ "$_out" == "ok "* ]]
}

# e2e_reconfig_race_node_verdict EVIDENCE — the node after the rounds:
# ok | collector_failed | node_down | node_restarted | crash_line |
# apport_record | kernel_trap | log_stalled.
e2e_reconfig_race_node_verdict() {
    local _evidence="${1:-}" _source _p0="" _p1="" _s0="" _s1=""

    for _source in log apport kernel; do
        if ! printf '%s\n' "$_evidence" | grep -qx "${_source}: ok"; then
            printf 'collector_failed'
            return 1
        fi
    done
    _p0=$(printf '%s\n' "$_evidence" | sed -n 's/^pid_start: //p' | tail -n 1)
    _p1=$(printf '%s\n' "$_evidence" | sed -n 's/^pid_now: //p' | tail -n 1)
    _s0=$(printf '%s\n' "$_evidence" | sed -n 's/^log_size: //p' | tail -n 1)
    _s1=$(printf '%s\n' "$_evidence" | sed -n 's/^log_size_after_ping: //p' | tail -n 1)
    if ! e2e_is_uint "$_p0"; then
        printf 'collector_failed'
        return 1
    fi
    if ! e2e_is_uint "$_p1"; then
        printf 'node_down'
        return 1
    fi
    if [[ "$_p1" != "$_p0" ]]; then
        printf 'node_restarted'
        return 1
    fi
    if printf '%s\n' "$_evidence" | grep -q '^crash_line: '; then
        printf 'crash_line'
        return 1
    fi
    if printf '%s\n' "$_evidence" | grep -q '^apport_new: '; then
        printf 'apport_record'
        return 1
    fi
    if printf '%s\n' "$_evidence" | grep -q '^kernel_new: '; then
        printf 'kernel_trap'
        return 1
    fi
    if ! e2e_counter_increased "$_s0" "$_s1"; then
        printf 'log_stalled'
        return 1
    fi
    printf 'ok'
}

local_validation_register reconfig_race_samples e2e_reconfig_race_samples \
    reconfig_race_samples_good \
    reconfig_race_samples_user_beyond_count \
    reconfig_race_samples_scrape_failed \
    reconfig_race_samples_other_user_ignored \
    reconfig_race_samples_empty_input
local_validation_register reconfig_race_verdict e2e_reconfig_race_verdict \
    reconfig_race_verdict_good \
    reconfig_race_verdict_good_dense \
    reconfig_race_verdict_pre_drop_peak_of_last_second \
    reconfig_race_verdict_pre_drop_window_is_1s \
    reconfig_race_verdict_above_pre_drop_nat \
    reconfig_race_verdict_above_pre_drop_fw6 \
    reconfig_race_verdict_sampler_too_few \
    reconfig_race_verdict_sampler_gaps_too_wide \
    reconfig_race_verdict_sampler_median_not_min \
    reconfig_race_verdict_sampler_one_long_gap_ok \
    reconfig_race_verdict_no_trigger \
    reconfig_race_verdict_not_zero_within_1s \
    reconfig_race_verdict_zero_too_late \
    reconfig_race_verdict_nonzero_before_data \
    reconfig_race_verdict_no_data_phase \
    reconfig_race_verdict_out_of_range \
    reconfig_race_verdict_no_load \
    reconfig_race_verdict_node_restarted \
    reconfig_race_verdict_node_down \
    reconfig_race_verdict_scrape_failed \
    reconfig_race_verdict_sampler_gap \
    reconfig_race_verdict_row_never_returned \
    reconfig_race_verdict_row_vanished_again \
    reconfig_race_verdict_empty_input
local_validation_register nat_entries_unique e2e_nat_entries_unique \
    nat_entries_unique_good \
    nat_entries_unique_dup_reverse \
    nat_entries_unique_dup_forward \
    nat_entries_unique_other_proto \
    nat_entries_unique_unreadable \
    nat_entries_unique_empty_input
local_validation_register reconfig_race_node_verdict e2e_reconfig_race_node_verdict \
    reconfig_race_node_ok \
    reconfig_race_node_restarted \
    reconfig_race_node_down \
    reconfig_race_node_crash_line \
    reconfig_race_node_apport_record \
    reconfig_race_node_kernel_trap \
    reconfig_race_node_log_stalled \
    reconfig_race_node_collector_failed \
    reconfig_race_node_empty_input

# The reconfig itself. A drill replaces this to leave the node untouched.
_e2e_rr_set_count() {
    fastrg_grpc_checked set_subscriber_count "$1" >/dev/null
}

_e2e_rr_local_count() {
    fastrg_grpc get_system_info | jq -r '.num_users // empty' 2>/dev/null | tr -d '[:space:]' || true
}

# Seconds the sampler waits between reads; 0 reads back to back. A drill
# replaces this to slow the sampler down.
_e2e_rr_sampler_gap() {
    printf '0'
}

# Sampler on the node: one line per /metrics read, stamped with the node
# clock in microseconds, keeping only the lines the predicates read.
_e2e_rr_sampler_start() {
    local _port _gap _i

    _port=$(e2e_metrics_port)
    [[ -n "$_port" ]] || return 1
    _gap=$(_e2e_rr_sampler_gap)
    ssh_node "rm -f /tmp/e2e_reconfig_race_samples.log /tmp/e2e_reconfig_race_sampler.pid;
nohup python3 -u - '${_port}' '${USER_ID}' '${_gap}' >'${_E2E_RR_SAMPLES}' 2>/dev/null <<'PY' &
import os
import re
import socket
import subprocess
import sys
import time

port, user, gap = int(sys.argv[1]), sys.argv[2], float(sys.argv[3])
req = b'GET /metrics HTTP/1.1' + bytes([13, 10]) + b'Host: 127.0.0.1' + bytes([13, 10]) + \
    b'Connection: close' + bytes([13, 10, 13, 10])
pat = re.compile(rb'^(?:fastrg_node_per_user_(?:nat_entries_used|ipv6_firewall_entries_used)'
                 rb'\{[^}]*user_id=\x22' + re.escape(user.encode()) + rb'\x22\}'
                 rb'|fastrg_node_total_pppoe_data_sessions\{[^}]*\}) \S+\$', re.M)

def find_pid():
    return subprocess.run(['pgrep', '-xo', 'fastrg'], capture_output=True,
                          text=True).stdout.strip() or '-'

pid = find_pid()
buf = []
flushed = time.monotonic()
while True:
    if pid == '-' or not os.path.isdir('/proc/' + pid):
        pid = find_pid()
    parts = ['@ %d %s' % (time.time_ns() // 1000, pid)]
    try:
        sock = socket.create_connection(('127.0.0.1', port), timeout=1)
        sock.sendall(req)
        chunks = []
        while True:
            chunk = sock.recv(262144)
            if not chunk:
                break
            chunks.append(chunk)
        sock.close()
        parts.extend(m.decode() for m in pat.findall(b''.join(chunks)))
    except OSError as exc:
        parts.append('# scrape failed: ' + type(exc).__name__)
        time.sleep(0.05)
    buf.append(' | '.join(parts) + chr(10))
    now = time.monotonic()
    if now - flushed >= 0.02:
        os.write(1, ''.join(buf).encode())
        buf = []
        flushed = now
    if gap > 0:
        time.sleep(gap)
PY
echo \$! >'${_E2E_RR_SAMPLER_PID}'" >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        sleep 0.5
        ssh_node "test -s '${_E2E_RR_SAMPLES}'" 2>/dev/null && return 0
    done
    return 1
}

# Complete sampler lines so far. A line still being written has no newline yet
# and is left out.
_e2e_rr_mark() {
    ssh_node "wc -l < '${_E2E_RR_SAMPLES}'" 2>/dev/null | tr -d '[:space:]' || true
}

_e2e_rr_rows_since() {
    local _from=$(( ${1:-0} + 1 ))

    ssh_node "n=\$(wc -l < '${_E2E_RR_SAMPLES}'); [ \"\$n\" -ge ${_from} ] && sed -n \"${_from},\${n}p\" '${_E2E_RR_SAMPLES}'" \
        2>/dev/null || true
}

# The newest complete sample: the last line may still be partly written.
_e2e_rr_last_row() {
    local _raw

    _raw=$(ssh_node "tail -n 2 '${_E2E_RR_SAMPLES}' | head -n 1" 2>/dev/null || true)
    e2e_reconfig_race_samples "$_raw" "$USER_ID" | tail -n 1
}

# Poll the newest sample until it shows WHAT: load (both tables in use), nat
# (NAT table in use), dropped (user row gone), back (row present) or data:N
# (row present and N data sessions). Returns non-zero after TIMEOUT seconds.
_e2e_rr_wait() {
    local _what="$1" _timeout="$2" _start=$SECONDS _row _t _p _nat _fw6 _data

    while (( SECONDS - _start < _timeout )); do
        _row=$(_e2e_rr_last_row)
        read -r _t _p _nat _fw6 _data <<< "$_row" || true
        case "$_what" in
            load)    e2e_counter_increased 0 "${_nat:-}" && e2e_counter_increased 0 "${_fw6:-}" && return 0 ;;
            nat)     e2e_counter_increased 0 "${_nat:-}" && return 0 ;;
            dropped) [[ "${_nat:-}" == "-" ]] && e2e_is_uint "${_data:-}" && return 0 ;;
            back)    e2e_is_uint "${_nat:-}" && return 0 ;;
            data:*)  e2e_is_uint "${_nat:-}" && [[ "${_data:-}" == "${_what#data:}" ]] && return 0 ;;
        esac
        sleep 0.2
    done
    return 1
}

# LAN-side load: one UDP packet per new source port from the given family's
# address. For IPv6 the source is the LAN host's address inside PREFIX, the
# current session's delegation, waited for up to _E2E_RR_ADDR_WAIT seconds.
# Prints "ready <source> waited=<seconds>" once sending.
_e2e_rr_sender_start() {
    local _family="$1" _prefix="${2:-}" _dst _pid _out _i

    if [[ "$_family" == "6" ]]; then
        _dst="$_P37_WAN6_HOST_ADDR"; _pid="$_E2E_RR_V6_PID"; _out="$_E2E_RR_V6_OUT"
        ssh_lan "rm -f /tmp/e2e_reconfig_race_v6.pid /tmp/e2e_reconfig_race_v6.out" >/dev/null 2>&1 || true
    else
        _dst="$WAN_IP"; _pid="$_E2E_RR_V4_PID"; _out="$_E2E_RR_V4_OUT"
        ssh_lan "rm -f /tmp/e2e_reconfig_race_v4.pid /tmp/e2e_reconfig_race_v4.out" >/dev/null 2>&1 || true
    fi
    ssh_lan "nohup python3 -u - '${_family}' '${_dst}' '${_E2E_RR_DST_BASE}' '${_E2E_RR_DST_COUNT}' \
    '${_E2E_RR_SRC_BASE}' '${_E2E_RR_SRC_COUNT}' '${_E2E_RR_PPS}' '${_prefix:--}' '${_P37_LAN_VLAN}' \
    '${_E2E_RR_ADDR_WAIT}' \
    >'${_out}' 2>&1 <<'PY' &
import ipaddress
import socket
import subprocess
import sys
import time

family_arg, dst = sys.argv[1], sys.argv[2]
dst_base, dst_count = int(sys.argv[3]), int(sys.argv[4])
src_base, src_count = int(sys.argv[5]), int(sys.argv[6])
pps, prefix, dev = int(sys.argv[7]), sys.argv[8], sys.argv[9]
addr_wait = float(sys.argv[10])

family = socket.AF_INET
src = '0.0.0.0'
if family_arg == '6':
    family = socket.AF_INET6
    net = ipaddress.ip_network(prefix, strict=False)
    src = ''
    waited_from = time.monotonic()
    deadline = waited_from + addr_wait
    while not src and time.monotonic() < deadline:
        out = subprocess.run(['ip', '-6', '-o', 'addr', 'show', 'dev', dev, 'scope', 'global'],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            if 'tentative' in line or 'deprecated' in line or 'dadfailed' in line:
                continue
            addr = line.split()[3].split('/')[0]
            if ipaddress.ip_address(addr) in net:
                src = addr
                break
        if not src:
            time.sleep(0.5)
    if not src:
        print('no_address in %s after %.0fs' % (prefix, addr_wait), flush=True)
        sys.exit(1)
    print('ready %s waited=%.1f' % (src, time.monotonic() - waited_from), flush=True)
else:
    print('ready %s waited=0.0' % src, flush=True)

per_tick = max(1, pps // 100)
seq = sent = failed = 0
started = time.monotonic()
next_tick = started
report = started + 1
while True:
    for _ in range(per_tick):
        sport = src_base + seq % src_count
        dport = dst_base + (seq // src_count) % dst_count
        seq += 1
        sock = socket.socket(family, socket.SOCK_DGRAM)
        try:
            sock.bind((src, sport))
            sock.sendto(b'reconfig-race', (dst, dport))
            sent += 1
        except OSError:
            failed += 1
        finally:
            sock.close()
    next_tick += 0.01
    now = time.monotonic()
    if next_tick > now:
        time.sleep(next_tick - now)
    else:
        next_tick = now
    if now >= report:
        print('sent=%d failed=%d elapsed=%.1f' % (sent, failed, now - started), flush=True)
        report = now + 1
PY
echo \$! >'${_pid}'" >/dev/null 2>&1 || true
    for _i in $(seq 1 $(( _E2E_RR_ADDR_WAIT * 2 + 10 ))); do
        if ssh_lan "grep -qE '^(ready|no_address)' '${_out}'" 2>/dev/null; then
            ssh_lan "grep -q '^ready' '${_out}'" 2>/dev/null && return 0
            return 1
        fi
        sleep 0.5
    done
    return 1
}

_e2e_rr_sender_stop() {
    if [[ "$1" == "6" ]]; then
        _p37_stop_remote ssh_lan "$_E2E_RR_V6_PID" "reconfig race IPv6 sender" || true
    else
        _p37_stop_remote ssh_lan "$_E2E_RR_V4_PID" "reconfig race IPv4 sender" || true
    fi
}

_e2e_rr_sender_report() {
    local _out="$_E2E_RR_V4_OUT"

    [[ "$1" == "6" ]] && _out="$_E2E_RR_V6_OUT"
    ssh_lan "grep -E '^(ready|no_address|sent=)' '${_out}' 2>/dev/null | tail -n 1" 2>/dev/null || true
}

# Samples for the failure report: around the first one above its pre-drop
# peak, else around the re-add.
_e2e_rr_rows_excerpt() {
    printf '%s\n' "${1:-}" | awk '
        {
            line[NR] = $0; t[NR] = $1; nat[NR] = $3; fw6[NR] = $4
            if (!a && $3 == "-") a = NR
            if (a && !r && $3 != "-") r = NR
        }
        END {
            for (i = a - 1; a && i >= 1 && t[a] - t[i] <= 1000000; i--) {
                if (nat[i] + 0 > pn) pn = nat[i] + 0
                if (fw6[i] + 0 > pf) pf = fw6[i] + 0
            }
            for (i = a; a && i <= NR && !hit; i++)
                if (nat[i] != "-" && (nat[i] + 0 > pn || fw6[i] + 0 > pf)) hit = i
            mid = hit ? hit : (r ? r : (a ? a : NR))
            for (i = mid - 5; i <= mid + 15; i++) if (i >= 1 && i <= NR) print line[i]
        }'
}

# One drop/re-add round. Leaves what broke in _E2E_RR_ROUND_ISSUE, the verdict
# in _E2E_RR_ROUND_VERDICT, the GetNatEntries answer in _E2E_RR_ROUND_ENTRIES
# and the seconds spent waiting for the IPv6 source in _E2E_RR_ROUND_ADDR_WAIT.
_e2e_rr_round() {
    local _count="$1" _prefix="" _mark="" _row="" _expect="" _rows="" _entries="" _i
    local _t _p _nat _fw6

    _E2E_RR_ROUND_ISSUE=""
    _E2E_RR_ROUND_VERDICT=""
    _E2E_RR_ROUND_ENTRIES=""
    _E2E_RR_ROUND_ADDR_WAIT=""

    # IPv6 load from this session's delegated prefix.
    for _i in $(seq 1 30); do
        _prefix=$(_p37_hsi_field "${USER_ID}" 'ipv6_pd_prefix')
        [[ -n "$_prefix" ]] && break
        sleep 1
    done
    if [[ -z "$_prefix" ]]; then
        _E2E_RR_ROUND_ISSUE="no delegated IPv6 prefix within 30s"
        return 1
    fi
    _e2e_rr_sender_stop 6
    if ! _e2e_rr_sender_start 6 "$_prefix"; then
        _E2E_RR_ROUND_ISSUE="IPv6 sender in ${_prefix}: $(_e2e_rr_sender_report 6)"
        return 1
    fi
    _E2E_RR_ROUND_ADDR_WAIT=$(ssh_lan "grep -m 1 '^ready' '${_E2E_RR_V6_OUT}'" 2>/dev/null | \
        sed -nE 's/.* waited=([0-9.]+).*/\1/p' || true)

    _mark=$(_e2e_rr_mark)
    if ! e2e_is_uint "$_mark"; then
        _E2E_RR_ROUND_ISSUE="cannot read the sampler file on the node"
        return 1
    fi
    if _e2e_rr_wait load 20; then
        _row=$(_e2e_rr_last_row)
        read -r _t _p _nat _fw6 _expect <<< "$_row" || true
        _E2E_RR_COUNT_CHANGED=1
        _e2e_rr_set_count $(( _count - 1 )) || \
            warn "reconfig race: set_subscriber_count $(( _count - 1 )) failed"
        _e2e_rr_wait dropped 20 || true
        # Every session delegates a new prefix and the guest keeps the old
        # addresses until its per-interface limit (max_addresses=16) refuses
        # the next one. Dropped here, while the slot is gone, so the new
        # session's first advertisement always lands after the flush.
        ssh_lan "ip -6 addr flush dev ${_P37_LAN_VLAN} scope global; \
            ip -6 route flush dev ${_P37_LAN_VLAN} proto ra" >/dev/null 2>&1 || true
        _e2e_rr_set_count "$_count" || warn "reconfig race: set_subscriber_count ${_count} failed"
        # Stopped before the session returns: its IPv6 gate can open ahead of
        # Data phase, and a send then would read as a stale session.
        _e2e_rr_sender_stop 6
        _e2e_rr_wait back 20 || true
        sleep 3
        # Re-sends the unchanged config so the re-added slot is configured now
        # rather than at the next 60s reconcile.
        fastrg_grpc_checked set_ipv6 "${USER_ID}" true >/dev/null 2>&1 || true
        _e2e_rr_wait "data:${_expect:-x}" 90 || true
    fi

    _rows=$(e2e_reconfig_race_samples "$(_e2e_rr_rows_since "$_mark")" "$USER_ID")
    if ! _E2E_RR_ROUND_VERDICT=$(e2e_reconfig_race_verdict "$_rows" "$_E2E_RR_MAX_NAT" "$_E2E_RR_MAX_FW6" \
            "$_E2E_RR_MIN_SAMPLES" "$_E2E_RR_MAX_MEDIAN_US"); then
        _E2E_RR_ROUND_ISSUE="$_E2E_RR_ROUND_VERDICT"
        info "Samples (us pid nat fw6 data):"
        _e2e_rr_rows_excerpt "$_rows" | while read -r _row; do info "  ${_row}"; done
        return 1
    fi
    # The duplicate check needs entries to look at: wait for NAT learning to
    # come back with the session, then require GetNatEntries to show some.
    if ! _e2e_rr_wait nat 10; then
        _E2E_RR_ROUND_ISSUE="NAT learning not back within 10s of Data phase (newest sample: $(_e2e_rr_last_row))"
        return 1
    fi
    _entries=$(fastrg_grpc_checked get_nat_entries "${USER_ID}" 2>/dev/null) || _entries=""
    if ! _E2E_RR_ROUND_ENTRIES=$(e2e_nat_entries_unique "$_entries"); then
        _E2E_RR_ROUND_ISSUE="GetNatEntries ${_E2E_RR_ROUND_ENTRIES}"
        return 1
    fi
    if ! e2e_counter_increased 0 "${_E2E_RR_ROUND_ENTRIES#ok }"; then
        _E2E_RR_ROUND_ISSUE="GetNatEntries returned no entry while /metrics shows NAT in use"
        return 1
    fi
    return 0
}

# Node-side half of the liveness evidence: pids, stdout size, crash lines
# written since the step started.
_E2E_RR_NODE_EVIDENCE_SCRIPT='
pid=$(pgrep -xo fastrg)
printf "pid_start: %s\npid_now: %s\n" "$PID0" "${pid:--}"
if [ -f "$LOG" ] && [ -r "$LOG" ]; then
    printf "log_size: %s\n" "$(stat -c %s "$LOG")"
    tail -c +"$((OFFSET + 1))" "$LOG" | grep -aE "Segmentation|PANIC in |Assertion .*failed" |
        head -n 3 | sed "s/^/crash_line: /"
fi
'

# Idempotent: called at the end of the step and from the top-level EXIT trap.
_e2e_reconfig_race_cleanup() {
    local _i _now=""

    _p37_stop_remote ssh_lan "$_E2E_RR_V4_PID" "reconfig race IPv4 sender" || true
    _p37_stop_remote ssh_lan "$_E2E_RR_V6_PID" "reconfig race IPv6 sender" || true
    _p37_stop_remote ssh_node "$_E2E_RR_SAMPLER_PID" "reconfig race sampler" || true
    ssh_lan "rm -f /tmp/e2e_reconfig_race_v4.pid /tmp/e2e_reconfig_race_v4.out \
        /tmp/e2e_reconfig_race_v6.pid /tmp/e2e_reconfig_race_v6.out" >/dev/null 2>&1 || true
    ssh_node "rm -f /tmp/e2e_reconfig_race_sampler.pid /tmp/e2e_reconfig_race_samples.log" \
        >/dev/null 2>&1 || true

    if [[ "${_E2E_RR_COUNT_CHANGED:-0}" -eq 1 && -n "${_E2E_RR_ORIG_COUNT:-}" ]]; then
        info "Cleanup(reconfig race): restoring the subscriber count to ${_E2E_RR_ORIG_COUNT}..."
        fastrg_grpc set_subscriber_count "${_E2E_RR_ORIG_COUNT}" >/dev/null 2>&1 || true
        for _i in $(seq 1 15); do
            _now=$(_e2e_rr_local_count)
            [[ "$_now" == "$_E2E_RR_ORIG_COUNT" ]] && break
            sleep 2
        done
        [[ "$_now" == "$_E2E_RR_ORIG_COUNT" ]] || \
            warn "Cleanup(reconfig race): node still reports ${_now:-nothing} subscribers"
        _E2E_RR_COUNT_CHANGED=0
    fi
    return 0
}

# e2e_reconfig_race_step NAME — the whole step, recorded as NAME.
e2e_reconfig_race_step() {
    local _name="$1" _issue="" _count="" _round=0 _done=0 _mark="" _pid0=""
    local _log="" _inode="" _size="" _since="" _offset="" _evidence="" _node="" _s1=""
    local _entries=0 _addr_wait=0 _oks="" _sum="" _v=""

    _count=$(_e2e_rr_local_count)
    if ! e2e_is_uint "$_count" || [[ "$_count" != "$USER_ID" ]]; then
        fail "$_name" "needs user ${USER_ID} to be the last subscriber slot, node has ${_count:-no} subscribers"
        return 0
    fi
    _E2E_RR_ORIG_COUNT="$_count"

    _p37_ipv6_fixture_up || true
    _issue="$_P37_FIXTURE_ISSUE"

    if [[ -z "$_issue" ]]; then
        _mark=$(e2e_node_stop_mark)
        read -r _log _inode _size _since <<< "$_mark" || true
        _pid0=$(ssh_node "pgrep -xo fastrg" 2>/dev/null | tr -d '[:space:]' || true)
        _offset=$(ssh_node "stat -c %s '${_log}'" 2>/dev/null | tr -d '[:space:]' || true)
        e2e_is_uint "$_offset" || _issue="cannot read the node stdout log (mark '${_mark}')"
    fi
    if [[ -z "$_issue" ]] && ! _e2e_rr_sampler_start; then
        _issue="the /metrics sampler on the node did not start"
    fi
    if [[ -z "$_issue" ]] && ! _e2e_rr_sender_start 4; then
        _issue="IPv4 sender: $(_e2e_rr_sender_report 4)"
    fi

    while [[ -z "$_issue" && $_round -lt $_E2E_RR_ROUNDS ]]; do
        _round=$(( _round + 1 ))
        if _e2e_rr_round "$_count"; then
            _done=$(( _done + 1 ))
            info "Round ${_round}/${_E2E_RR_ROUNDS}: ${_E2E_RR_ROUND_VERDICT}; GetNatEntries ${_E2E_RR_ROUND_ENTRIES}; IPv6 source ready after ${_E2E_RR_ROUND_ADDR_WAIT:-?}s"
            _addr_wait=$(awk -v a="$_addr_wait" -v b="${_E2E_RR_ROUND_ADDR_WAIT:-0}" \
                'BEGIN { print (b + 0 > a + 0) ? b : a }')
            _oks="${_oks}${_E2E_RR_ROUND_VERDICT}
"
            _v="${_E2E_RR_ROUND_ENTRIES#ok }"
            e2e_counter_increased "$_entries" "$_v" && _entries=$_v
        else
            _issue="round ${_round}/${_E2E_RR_ROUNDS}: ${_E2E_RR_ROUND_ISSUE}"
        fi
    done

    if [[ -n "$_pid0" ]]; then
        _evidence=$(_e2e_node_shutdown_evidence "$_log" "$_inode" "$_size" "$_since" | \
            grep -E '^(log|apport|kernel): |^(apport|kernel)_new: ' || true)
        _evidence="${_evidence}
$(ssh_node "PID0=$(printf '%q' "$_pid0"); LOG=$(printf '%q' "$_log");
            OFFSET=$(printf '%q' "$_offset"); ${_E2E_RR_NODE_EVIDENCE_SCRIPT}" 2>/dev/null || true)"
        fastrg_grpc get_node_status >/dev/null 2>&1 || true
        _s1=$(ssh_node "stat -c %s '${_log}'" 2>/dev/null | tr -d '[:space:]' || true)
        _evidence="${_evidence}
log_size_after_ping: ${_s1}"
        if ! _node=$(e2e_reconfig_race_node_verdict "$_evidence"); then
            _issue="${_issue:+${_issue}; }node ${_node}: $(printf '%s' "$_evidence" | \
                grep -E '^(pid_|crash_line|apport_new|kernel_new|log_size)' | tr '\n' ' ' | cut -c 1-300)"
        fi
    fi

    _v="v4 $(_e2e_rr_sender_report 4)"
    _e2e_reconfig_race_cleanup
    _cleanup_phase37_ipv6_firewall || true

    # Worst case over the rounds, from the "ok ..." verdict lines.
    _sum=$(printf '%s' "$_oks" | awk '
        {
            for (i = 2; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
            split(v["peak_after_drop"], pk, "/")
            if (!seen++ || v["samples"] + 0 < samples) samples = v["samples"] + 0
            if (v["median_gap"] + 0 > gap) gap = v["median_gap"] + 0
            if (pk[1] + 0 > pn) pn = pk[1] + 0
            if (pk[2] + 0 > pf) pf = pk[2] + 0
            if (v["zero_after"] + 0 > zero) zero = v["zero_after"] + 0
            if (v["data_after"] + 0 > data) data = v["data_after"] + 0
        }
        END {
            printf "sampler >= %d samples per round, median gap <= %dus; after the drop nat <= %d and fw6 <= %d, never above the pre-drop peak; both tables 0 within %.1fms of the re-add and until Data phase (back within %.1fms)",
                samples, gap, pn, pf, zero, data
        }')

    if [[ -z "$_issue" ]]; then
        pass "$_name" \
            "${_done}/${_E2E_RR_ROUNDS} rounds at ${_E2E_RR_PPS}+${_E2E_RR_PPS} new flows/s (${_v}): ${_sum}; GetNatEntries up to ${_entries} entries, no duplicate key; IPv6 source in each new prefix ready within ${_addr_wait}s (limit ${_E2E_RR_ADDR_WAIT}s); fastrg pid ${_pid0} alive, log still written"
    else
        fail "$_name" "$_issue"
    fi
    return 0
}
