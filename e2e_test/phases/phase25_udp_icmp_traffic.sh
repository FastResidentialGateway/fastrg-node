#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 25 — Sustained UDP / ICMP traffic across NAT timeout, plus RSS
# queue-distribution check (Steps 105-107b)
# ---------------------------------------------------------------------------

_P25_IPERF_PORT=5902
_P25_METRICS_PORT=""

# Step 107's ICMP echo run: 150 packets 0.1s apart, and the moment inside that
# run at which the live NAT table is read. 12s is past the 10s idle timeout, so
# whatever the dump reports there is a mapping the ping has kept alive.
_P25_PING_COUNT=150
_P25_PING_INTERVAL=0.1
_P25_NAT_DUMP_AT=12

# Filled in by _p25_icmp_attempt: the ping output plus the two NAT table reads
# that belong to that same attempt.
_P25_PING_OUT=""
_P25_NAT_DUMP_DURING=""
_P25_NAT_DUMP_AFTER=""

# RSS distribution probe (Step 107b)
_P25_RSS_ECHO_PORT=5903
_P25_RSS_FLOWS=16
_P25_RSS_PKTS_PER_FLOW=20
_P25_RSS_SRC_BASE=43000
_P25_RSS_ECHO_PID=/tmp/e2e_rss_echo.pid
_P25_RSS_ECHO_OUT=/tmp/e2e_rss_echo.out
_P25_RSS_ECHO_READY=/tmp/e2e_rss_echo.ready

_p25_stop_echo_server() {
    local _i

    ssh_wan "if [ -s '${_P25_RSS_ECHO_PID}' ]; then
            _pid=\$(cat '${_P25_RSS_ECHO_PID}' 2>/dev/null || true)
            [ -n \"\$_pid\" ] && kill -0 \"\$_pid\" 2>/dev/null && kill -TERM \"\$_pid\" 2>/dev/null || true
        fi" >/dev/null 2>&1 || true
    for _i in $(seq 1 5); do
        if ! ssh_wan "_pid=\$(cat '${_P25_RSS_ECHO_PID}' 2>/dev/null); \
            test -n \"\$_pid\" && kill -0 \"\$_pid\" 2>/dev/null" 2>/dev/null; then
            ssh_wan "rm -f '${_P25_RSS_ECHO_PID}' '${_P25_RSS_ECHO_OUT}' '${_P25_RSS_ECHO_READY}'" \
                >/dev/null 2>&1 || true
            return 0
        fi
        sleep 1
    done
    warn "Phase 25 UDP echo server did not exit within 5s after SIGTERM"
    return 1
}

_p25_snippet() {
    printf '%s' "$1" | tr '\n' ' ' | cut -c 1-500 || true
}

_p25_fetch_metric() {
    local _family="$1"
    local _body

    _body=$(ssh_node \
        "curl -fsS --max-time 5 http://127.0.0.1:${_P25_METRICS_PORT}/metrics" \
        2>/dev/null || true)
    printf '%s\n' "$_body" | awk -v family="$_family" -v uid="$USER_ID" '
        $1 ~ ("^" family "\\{") && $1 ~ ("user_id=\"" uid "\"") { print $2; exit }
    ' || true
}

_p25_is_uint() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

_p25_stop_client() {
    local _i

    ssh_lan "pkill -TERM -f '[i]perf3 -u -c ${WAN_IP} -p ${_P25_IPERF_PORT}' 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    for _i in $(seq 1 5); do
        if ! ssh_lan "pgrep -f '[i]perf3 -u -c ${WAN_IP} -p ${_P25_IPERF_PORT}' >/dev/null" \
            2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    warn "Phase 25 iperf3 client did not exit within 5s after SIGTERM"
    return 1
}

_p25_stop_server() {
    local _i

    ssh_wan "pkill -TERM -f '[i]perf3 -s -B ${WAN_IP} -p ${_P25_IPERF_PORT}' 2>/dev/null || true" \
        >/dev/null 2>&1 || true
    for _i in $(seq 1 5); do
        if ! ssh_wan "pgrep -f '[i]perf3 -s -B ${WAN_IP} -p ${_P25_IPERF_PORT}' >/dev/null" \
            2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    warn "Phase 25 iperf3 server did not exit within 5s after SIGTERM"
    return 1
}

_cleanup_phase25_udp_icmp_traffic() {
    _p25_stop_client || true
    _p25_stop_server || true
    _p25_stop_echo_server || true
    return 0
}

# ---------------------------------------------------------------------------
# Traffic-counter assertions.
#
# rx is counted on the port a packet arrives on and tx on the port it leaves by
# (see the count_rx_packet/count_tx_packet calls in src/dp.c), so a LAN->WAN
# flow must show up as per-subscriber rx on nic_index 0 and tx on nic_index 1,
# and the reverse flow the other way round. The PPPoE session counters follow
# the encapsulation instead: tx on encapsulate, rx on decapsulate.
#
# The containment check compares packets only, never bytes: the NIC byte
# counters and the per-subscriber byte counters measure the frame at different
# points of the encap/decap path, so only the packet counts are comparable.
# _P25_PORT_SKEW allows for the fact that one scrape gathers the NIC port stats
# before the per-subscriber rows, so a few packets can land between the two
# reads while a flow is running.
# ---------------------------------------------------------------------------
_P25_PORT_SKEW=100
_P25_TRAFFIC_ISSUE=""

# Signed delta of one metric row between two scrapes; prints nothing when
# either sample is unreadable.
_p25_delta() {
    local _before="$1" _after="$2" _family="$3"
    local _b="" _a=""

    shift 3
    _b=$(e2e_metric_value "$_before" "$_family" "$@")
    _a=$(e2e_metric_value "$_after" "$_family" "$@")
    e2e_all_uint "$_b" "$_a" || return 0
    printf '%d' $(( _a - _b ))
}

# Same, summed over every row matching the label filters.
_p25_delta_sum() {
    local _before="$1" _after="$2" _family="$3"
    local _b="" _a=""

    shift 3
    _b=$(e2e_metric_sum "$_before" "$_family" "$@")
    _a=$(e2e_metric_sum "$_after" "$_family" "$@")
    e2e_all_uint "$_b" "$_a" || return 0
    printf '%d' $(( _a - _b ))
}

_p25_want_growth() {
    local _label="$1" _delta="$2"

    if ! [[ "$_delta" =~ ^-?[0-9]+$ ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }${_label} unreadable"
    elif [[ "$_delta" -le 0 ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }${_label} delta=${_delta}, expected > 0"
    fi
}

_p25_want_contains() {
    local _label="$1" _port_delta="$2" _user_delta="$3"

    if ! [[ "$_port_delta" =~ ^-?[0-9]+$ && "$_user_delta" =~ ^-?[0-9]+$ ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }${_label} unreadable"
    elif [[ "$_port_delta" -lt $(( _user_delta - _P25_PORT_SKEW )) ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }${_label}: port delta=${_port_delta} < per-subscriber total=${_user_delta}"
    fi
}

# _p25_check_flow BEFORE AFTER RX_NIC TX_NIC SESSION_DIR
_p25_check_flow() {
    local _before="$1" _after="$2" _rx_nic="$3" _tx_nic="$4" _session_dir="$5"

    _P25_TRAFFIC_ISSUE=""

    _p25_want_growth "per_user rx_packets{nic=${_rx_nic},user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_per_user_rx_packets_total "nic_index=${_rx_nic}" "user_id=${USER_ID}")"
    _p25_want_growth "per_user rx_bytes{nic=${_rx_nic},user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_per_user_rx_bytes_total "nic_index=${_rx_nic}" "user_id=${USER_ID}")"
    _p25_want_growth "per_user tx_packets{nic=${_tx_nic},user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_per_user_tx_packets_total "nic_index=${_tx_nic}" "user_id=${USER_ID}")"
    _p25_want_growth "per_user tx_bytes{nic=${_tx_nic},user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_per_user_tx_bytes_total "nic_index=${_tx_nic}" "user_id=${USER_ID}")"

    _p25_want_growth "per_pppoe_session ${_session_dir}_packets{user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" "fastrg_node_per_pppoe_session_${_session_dir}_packets_total" "user_id=${USER_ID}")"
    _p25_want_growth "per_pppoe_session ${_session_dir}_bytes{user=${USER_ID}}" \
        "$(_p25_delta "$_before" "$_after" "fastrg_node_per_pppoe_session_${_session_dir}_bytes_total" "user_id=${USER_ID}")"

    _p25_want_growth "nic rx_packets{nic=${_rx_nic}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_rx_packets_total "nic_index=${_rx_nic}")"
    _p25_want_growth "nic rx_bytes{nic=${_rx_nic}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_rx_bytes_total "nic_index=${_rx_nic}")"
    _p25_want_growth "nic tx_packets{nic=${_tx_nic}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_tx_packets_total "nic_index=${_tx_nic}")"
    _p25_want_growth "nic tx_bytes{nic=${_tx_nic}}" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_tx_bytes_total "nic_index=${_tx_nic}")"

    _p25_want_contains "nic ${_rx_nic} rx_packets vs per-subscriber rx" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_rx_packets_total "nic_index=${_rx_nic}")" \
        "$(_p25_delta_sum "$_before" "$_after" fastrg_node_per_user_rx_packets_total "nic_index=${_rx_nic}")"
    _p25_want_contains "nic ${_tx_nic} tx_packets vs per-subscriber tx" \
        "$(_p25_delta "$_before" "$_after" fastrg_node_tx_packets_total "nic_index=${_tx_nic}")" \
        "$(_p25_delta_sum "$_before" "$_after" fastrg_node_per_user_tx_packets_total "nic_index=${_tx_nic}")"

    # Dropped counters are cumulative gauges no healthy flow should move; the
    # checkable contract without injecting a fault is that they never regress.
    local _dropped_pkts _dropped_bytes
    _dropped_pkts=$(_p25_delta_sum "$_before" "$_after" fastrg_node_per_user_dropped_packets_total)
    _dropped_bytes=$(_p25_delta_sum "$_before" "$_after" fastrg_node_per_user_dropped_bytes_total)
    if ! [[ "$_dropped_pkts" =~ ^-?[0-9]+$ && "$_dropped_bytes" =~ ^-?[0-9]+$ ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }per_user dropped counters unreadable"
    elif [[ "$_dropped_pkts" -lt 0 || "$_dropped_bytes" -lt 0 ]]; then
        _P25_TRAFFIC_ISSUE="${_P25_TRAFFIC_ISSUE:+${_P25_TRAFFIC_ISSUE}; }per_user dropped counters regressed (packets=${_dropped_pkts}, bytes=${_dropped_bytes})"
    fi

    [[ -z "$_P25_TRAFFIC_ISSUE" ]]
}

_p25_parse_udp_json() {
    printf '%s\n' "$1" | jq -r '
        [(.end.sum.lost_percent // .end.sum_received.lost_percent //
          .end.streams[0].udp.lost_percent // null),
         (.end.sum.bytes // .end.sum_received.bytes // .end.streams[0].udp.bytes // null)] |
        @tsv
    ' 2>/dev/null || true
}

_p25_udp_result_ok() {
    local _lost="$1"
    local _bytes="$2"

    [[ "$_lost" =~ ^[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?$ ]] || return 1
    [[ "$_bytes" =~ ^[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?$ ]] || return 1
    awk -v lost="$_lost" -v bytes="$_bytes" 'BEGIN { exit !(lost < 1 && bytes > 0) }'
}

# True when the loss figure is zero however ping spelled it (0, 0.0, 0.00).
# A string compare against "0" and "0.0" misses the rest, and a substring
# match on "0% packet loss" would accept the "0%" inside "100%".
_p25_loss_is_zero() {
    [[ "$1" =~ ^0([.]0+)?$ ]]
}

_p25_ping_loss() {
    printf '%s\n' "$1" | sed -nE \
        's/.* ([0-9]+([.][0-9]+)?)% packet loss.*/\1/p' | tail -1 || true
}

# The NAT identifier of the one live ICMP mapping toward DST_IP in a
# GetNatEntries reply (proto 1 is IPPROTO_ICMP). Filtering on the type and the
# destination is what keeps the subscriber's background UDP and DNS flows out
# of the answer. Prints one of:
#   <nat_port>  exactly one such mapping, and this is its translated identifier
#   none        a readable reply that holds no ICMP mapping toward DST_IP
#   multi       more than one, so no single mapping can be pointed at
#   err         not a readable reply, or no destination was given
# Only the identifier case returns 0.
e2e_icmp_nat_ident() {
    local _json="${1:-}" _dst_ip="${2:-}" _idents="" _count=0 _ident=""

    [[ -n "$_json" && -n "$_dst_ip" ]] || { printf 'err'; return 1; }
    _idents=$(printf '%s' "$_json" | jq -r --arg ip "$_dst_ip" '
        if (.entries | type) != "array" then error("not a GetNatEntries reply")
        else .entries[] | select(.proto == 1 and .dst_ip == $ip) | .nat_port
        end' 2>/dev/null) || { printf 'err'; return 1; }

    _count=$(printf '%s\n' "$_idents" | grep -c . || true)
    [[ "$_count" == "0" ]] && { printf 'none'; return 1; }
    [[ "$_count" == "1" ]] || { printf 'multi'; return 1; }

    _ident="$_idents" # one match, so the captured output is that single value
    e2e_is_uint "$_ident" || { printf 'err'; return 1; }
    printf '%s' "$_ident"
    return 0
}

local_validation_register icmp_nat_ident e2e_icmp_nat_ident \
    icmp_nat_ident_good icmp_nat_ident_background_only \
    icmp_nat_ident_other_destination icmp_nat_ident_two_matches \
    icmp_nat_ident_no_destination icmp_nat_ident_empty_input \
    icmp_nat_ident_unreadable icmp_nat_ident_missing_nat_port

# The subscriber's live NAT table, as GetNatEntries reports it. Kept apart from
# the reading of it so a drill can spoil the artifact without touching how it
# is judged.
_p25_get_nat_entries() {
    fastrg_grpc get_nat_entries "$USER_ID"
}

# One ICMP echo run plus the two NAT table reads that belong to it: one taken
# _P25_NAT_DUMP_AT seconds in, one right after the run. Both reads are redone
# on a retry, so the pair always describes the attempt whose loss figure is
# the one being judged.
_p25_icmp_attempt() {
    local _dump_file="" _dump_pid=""

    _P25_PING_OUT=""
    _P25_NAT_DUMP_DURING=""
    _P25_NAT_DUMP_AFTER=""

    _dump_file=$(mktemp) || _dump_file=""
    if [[ -n "$_dump_file" ]]; then
        ( sleep "$_P25_NAT_DUMP_AT"; _p25_get_nat_entries >"$_dump_file" 2>/dev/null ) &
        _dump_pid=$!
    fi
    _P25_PING_OUT=$(ssh_lan \
        "ping -c ${_P25_PING_COUNT} -i ${_P25_PING_INTERVAL} -W 2 ${WAN_IP}" 2>&1 || true)
    if [[ -n "$_dump_pid" ]]; then
        wait "$_dump_pid" 2>/dev/null || true
    fi
    if [[ -n "$_dump_file" ]]; then
        _P25_NAT_DUMP_DURING=$(cat "$_dump_file" 2>/dev/null || true)
        rm -f "$_dump_file"
    fi
    _P25_NAT_DUMP_AFTER=$(_p25_get_nat_entries)
}

# Drill: drop every ICMP row from the NAT table reads. The reply stays a valid
# GetNatEntries answer and the ping still comes back clean, so only an
# assertion that really looks for the ping's own mapping can tell the
# difference.
_p25_inject_nat_dump_without_icmp() {
    sabotage_copy_function _p25_get_nat_entries _p25_get_nat_entries_real
    sabotage_override_function _p25_get_nat_entries \
        '_p25_get_nat_entries_real | jq -c "if (.entries | type) == \"array\"
             then .entries |= map(select(.proto != 1)) else . end" 2>/dev/null || true'
}

_p25_cleanup_nat_dump_without_icmp() {
    restore_phase_functions phase25_udp_icmp_traffic.sh
}

case_validation_register icmp_nat_entry_missing phase25_udp_icmp_traffic \
    _p25_inject_nat_dump_without_icmp _p25_cleanup_nat_dump_without_icmp \
    'Step 107:'

# Where a sustained-flow packet went missing. One snapshot covers both
# directions: each step reads it once before its flow and again only when it
# fails, so a passing step prints nothing extra. Names carry the place they
# were read from; a source that cannot be reached contributes nothing, and the
# evidence then says so instead of printing a wrong delta.

# WAN->LAN receiver side. LAN_FLAP_NIC on LAN_FLAP_HOST is the PF the node's
# LAN port sends into: a frame arriving there with a bad CRC is discarded by
# the MAC before any host sees it, which looks like loss no node counter can
# account for.
_p25_pf_rx_counters() {
    ssh_lan_flap "ethtool -S ${LAN_FLAP_NIC} 2>/dev/null | awk -F: '
        \$1 ~ /rx_errors|rx_crc_errors|rx_missed_errors|rx_no_buffer_count/ {
            gsub(/[ \t]/, \"\", \$1); gsub(/[ \t]/, \"\", \$2);
            printf \"pf_%s=%s \", \$1, \$2
        }'" 2>/dev/null || true
}

# Losses that happen after the frame is safely inside the LAN host: socket
# receive overruns and interface RX drops.
_p25_lan_rx_counters() {
    ssh_lan "sed -n '/^Udp:/{n;s/^Udp: //p}' /proc/net/snmp | \
            awk '{ printf \"lan_udp_in_errors=%s lan_udp_rcvbuf_errors=%s \", \$3, \$5 }'
        ip -s -s link show ${LAN_PEER_VLAN} 2>/dev/null | \
            awk '/RX: *bytes/ { getline; printf \"lan_vlan_rx_dropped=%s lan_vlan_rx_missed=%s \", \$4, \$5; exit }'
        ip -s -s link show ${LAN_PEER_NIC} 2>/dev/null | \
            awk '/RX: *bytes/ { getline; printf \"lan_peer_rx_dropped=%s lan_peer_rx_missed=%s \", \$4, \$5; exit }'" \
        2>/dev/null || true
}

# LAN->WAN receiver side: the iperf3 server's own socket and the NIC it
# listens on.
_p25_wan_rx_counters() {
    ssh_wan "sed -n '/^Udp:/{n;s/^Udp: //p}' /proc/net/snmp | \
            awk '{ printf \"wan_udp_in_errors=%s wan_udp_rcvbuf_errors=%s \", \$3, \$5 }'
        for _f in rx_errors rx_dropped rx_missed_errors rx_crc_errors; do
            _v=\$(cat /sys/class/net/${WAN_NIC}/statistics/\$_f 2>/dev/null) || continue
            printf 'wan_nic_%s=%s ' \"\$_f\" \"\$_v\"
        done" 2>/dev/null || true
}

# The node's own view of both ports: wire-level RX errors from the NIC, plus
# the packets it dropped itself once they were inside.
_p25_node_rx_counters() {
    local _body

    # protobuf JSON leaves zero-valued fields out, so port 0 arrives with no
    # port_id at all and an untouched counter with no value.
    fastrg_grpc get_system_xstats 2>/dev/null | jq -r '
        .nic_xstats[]? |
        (if ((.port_id // 0) | tonumber) == 0 then "lan" else "wan" end) as $side |
        .xstats[]? |
        select(.name | test("^(rx_errors|rx_crc_errors|rx_missed_errors)$")) |
        "node_\($side)_\(.name)=\(.value // 0)"' 2>/dev/null | tr '\n' ' ' || true

    _body=$(e2e_metrics_body) || true
    [[ -n "$_body" ]] || return 0
    printf 'node_user_dropped_lan=%s node_user_dropped_wan=%s ' \
        "$(e2e_metric_value "$_body" fastrg_node_per_user_dropped_packets_total nic_index=0 user_id=${USER_ID})" \
        "$(e2e_metric_value "$_body" fastrg_node_per_user_dropped_packets_total nic_index=1 user_id=${USER_ID})"
    printf 'node_unknown_dropped_lan=%s node_unknown_dropped_wan=%s ' \
        "$(e2e_metric_value "$_body" fastrg_node_unknown_user_dropped_packets_total nic_index=0)" \
        "$(e2e_metric_value "$_body" fastrg_node_unknown_user_dropped_packets_total nic_index=1)"
}

# The hop beyond the node on the WAN side, from the BRAS's own periodic
# statistics line.
_p25_bras_counters() {
    ssh_bras "tail -200 /var/log/dpdk-bras.log 2>/dev/null | grep 'DP: stats:' | tail -1" 2>/dev/null | \
        sed -nE 's/.*drop=([0-9]+) \(txfull=([0-9]+).*/bras_drop=\1 bras_txfull=\2 /p' || true
}

_p25_loss_counters() {
    printf '%s%s%s%s%s' \
        "$(_p25_pf_rx_counters)" "$(_p25_lan_rx_counters)" "$(_p25_wan_rx_counters)" \
        "$(_p25_node_rx_counters)" "$(_p25_bras_counters)"
}

# One group's counters out of two snapshots, as "name +delta". Only names that
# both readings carry are printed, so a source that answered once and not the
# other time reads as missing instead of as a huge jump.
_p25_counter_delta() {
    awk -v before="$1" -v after="$2" -v prefix="$3" 'BEGIN {
        split(before, _b, " ");
        for (i in _b) { split(_b[i], _kv, "="); base[_kv[1]] = _kv[2] }
        n = split(after, _a, " ");
        for (i = 1; i <= n; i++) {
            split(_a[i], _kv, "=");
            if (index(_kv[1], prefix) != 1 || !(_kv[1] in base)) continue;
            printf "%s%s %+d", (shown++ ? ", " : ""), _kv[1], _kv[2] - base[_kv[1]];
        }
    }'
}

# Printed only on a FAIL. Answers "which counter on the way to the receiver
# grew by roughly the number of packets iperf3 says it never got".
_p25_loss_evidence() {
    local _label="$1" _before="$2" _json="$3"
    local _after _group _where _delta _report

    _after=$(_p25_loss_counters)
    for _group in pf lan wan node bras; do
        case "$_group" in
            pf)   _where="${LAN_FLAP_HOST}:${LAN_FLAP_NIC}" ;;
            lan)  _where="${LAN_HOST}" ;;
            wan)  _where="${WAN_HOST}:${WAN_NIC}" ;;
            node) _where="${FASTRG_NODE}" ;;
            bras) _where="${BRAS_HOST}" ;;
        esac
        _delta=$(_p25_counter_delta "$_before" "$_after" "${_group}_")
        if [[ -n "$_delta" ]]; then
            info "  ${_label} evidence: ${_where} ${_delta}"
        else
            info "  ${_label} evidence: ${_where} counters unavailable"
        fi
    done

    # out_of_order is reported per stream, not in the summary, so a plain
    # .end.sum read would always print null and hide reordering.
    _report=$(printf '%s\n' "$_json" | jq -c '{
        lost_packets: .end.sum.lost_packets,
        packets: .end.sum.packets,
        lost_percent: .end.sum.lost_percent,
        jitter_ms: .end.sum.jitter_ms,
        out_of_order: (.end.sum.out_of_order // .end.streams[0].udp.out_of_order)
    }' 2>/dev/null || true)
    info "  ${_label} evidence: iperf3 report ${_report:-unparsable}"
}

phase25_udp_icmp_traffic() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 25 — Sustained UDP / ICMP Traffic (Steps 105-107b)"
    bold "═══════════════════════════════════════════════════════"

    local _i _server_ready=0
    local _entries_base="" _entries_after="" _alloc_base="" _alloc_after=""
    local _iperf_out="" _iperf_rc=0 _parsed="" _lost="" _bytes="" _client_stopped=0
    local _ping_out="" _ping_retry_out="" _ping_loss="" _entries_delta=""
    local _ident_during="" _ident_after="" _icmp_issue=""
    local _entries_seq="" _entries_seq_file="" _entries_seq_pid=""
    local _traffic_before="" _traffic_after=""
    local _loss_base=""

    _P25_METRICS_PORT=$(ssh_node \
        "grep 'MetricsListenPort' /etc/fastrg/config.cfg 2>/dev/null" | \
        awk -F'"' '{print $2}' | awk -F: '{print $NF}' || true)

    _cleanup_phase25_udp_icmp_traffic
    ssh_wan "iperf3 -s -B ${WAN_IP} -p ${_P25_IPERF_PORT} -D --forceflush" \
        >/dev/null 2>&1 || true
    for _i in $(seq 1 10); do
        if ssh_wan "ss -ltn 2>/dev/null | grep -q ':${_P25_IPERF_PORT}'" 2>/dev/null; then
            _server_ready=1
            break
        fi
        sleep 1
    done

    info "Step 105: running 20s UDP LAN→WAN flow at 50 Mbps..."
    _entries_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")
    _alloc_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_alloc_fail_total")
    _traffic_before=$(e2e_metrics_body)
    _loss_base=$(_p25_loss_counters)
    if [[ $_server_ready -eq 1 ]]; then
        if _iperf_out=$(ssh_lan \
            "iperf3 -u -c ${WAN_IP} -p ${_P25_IPERF_PORT} -t 20 -b 50M -J" 2>&1); then
            _iperf_rc=0
        else
            _iperf_rc=$?
        fi
    else
        _iperf_rc=1
        _iperf_out="iperf3 server did not listen on port ${_P25_IPERF_PORT} within 10s"
    fi
    _entries_after=$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")
    _alloc_after=$(_p25_fetch_metric "fastrg_node_per_user_nat_alloc_fail_total")
    _traffic_after=$(e2e_metrics_body)
    _parsed=$(_p25_parse_udp_json "$_iperf_out")
    IFS=$'\t' read -r _lost _bytes <<< "$_parsed" || true
    _client_stopped=0
    _p25_stop_client && _client_stopped=1
    # LAN->WAN: rx on nic 0, tx on nic 1, PPPoE session tx (encapsulation).
    _p25_check_flow "$_traffic_before" "$_traffic_after" 0 1 tx || true
    if [[ $_iperf_rc -eq 0 && $_client_stopped -eq 1 ]] && \
       _p25_udp_result_ok "$_lost" "$_bytes" && \
       _p25_is_uint "$_entries_base" && _p25_is_uint "$_entries_after" && \
       _p25_is_uint "$_alloc_base" && [[ "$_alloc_after" == "$_alloc_base" ]] && \
       (( _entries_after - _entries_base <= 3 )) && \
       [[ -z "$_P25_TRAFFIC_ISSUE" ]]; then
        pass "Step 105: sustained UDP LAN→WAN" \
            "loss=${_lost}%, bytes=${_bytes}, entries delta=$(( _entries_after - _entries_base )), alloc_fail delta=0; per-subscriber/session/NIC counters advanced in the LAN→WAN direction"
    else
        fail "Step 105: sustained UDP LAN→WAN" \
            "server=${_server_ready} rc=${_iperf_rc} stopped=${_client_stopped}; loss=${_lost:-NA}% bytes=${_bytes:-NA}; entries=${_entries_base:-NA}->${_entries_after:-NA}; alloc_fail=${_alloc_base:-NA}->${_alloc_after:-NA}${_P25_TRAFFIC_ISSUE:+; traffic counters: ${_P25_TRAFFIC_ISSUE}}; output='$(_p25_snippet "$_iperf_out")'"
        _p25_loss_evidence "Step 105" "$_loss_base" "$_iperf_out"
    fi

    info "Step 106: running 20s reverse UDP WAN→LAN flow at 50 Mbps..."
    _alloc_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_alloc_fail_total")
    _traffic_before=$(e2e_metrics_body)
    _loss_base=$(_p25_loss_counters)
    if [[ $_server_ready -eq 1 ]]; then
        if _iperf_out=$(ssh_lan \
            "iperf3 -u -c ${WAN_IP} -p ${_P25_IPERF_PORT} -t 20 -b 50M -R -J" 2>&1); then
            _iperf_rc=0
        else
            _iperf_rc=$?
        fi
    else
        _iperf_rc=1
        _iperf_out="iperf3 server did not listen on port ${_P25_IPERF_PORT} within 10s"
    fi
    _alloc_after=$(_p25_fetch_metric "fastrg_node_per_user_nat_alloc_fail_total")
    _traffic_after=$(e2e_metrics_body)
    _parsed=$(_p25_parse_udp_json "$_iperf_out")
    IFS=$'\t' read -r _lost _bytes <<< "$_parsed" || true
    _client_stopped=0
    _p25_stop_client && _client_stopped=1
    local _server_stopped=0
    _p25_stop_server && _server_stopped=1
    # WAN->LAN: rx on nic 1, tx on nic 0, PPPoE session rx (decapsulation).
    _p25_check_flow "$_traffic_before" "$_traffic_after" 1 0 rx || true
    if [[ $_iperf_rc -eq 0 && $_client_stopped -eq 1 && $_server_stopped -eq 1 ]] && \
       _p25_udp_result_ok "$_lost" "$_bytes" && \
       _p25_is_uint "$_alloc_base" && [[ "$_alloc_after" == "$_alloc_base" ]] && \
       [[ -z "$_P25_TRAFFIC_ISSUE" ]]; then
        pass "Step 106: sustained reverse UDP WAN→LAN" \
            "loss=${_lost}%, bytes=${_bytes}, alloc_fail delta=0; full 20s reverse flow completed; per-subscriber/session/NIC counters advanced in the WAN→LAN direction"
    else
        fail "Step 106: sustained reverse UDP WAN→LAN" \
            "server=${_server_ready}/${_server_stopped} rc=${_iperf_rc} stopped=${_client_stopped}; loss=${_lost:-NA}% bytes=${_bytes:-NA}; alloc_fail=${_alloc_base:-NA}->${_alloc_after:-NA}${_P25_TRAFFIC_ISSUE:+; traffic counters: ${_P25_TRAFFIC_ISSUE}}; output='$(_p25_snippet "$_iperf_out")'"
        _p25_loss_evidence "Step 106" "$_loss_base" "$_iperf_out"
    fi

    # What this step is for: the ICMP ident mapping must stay alive for longer
    # than ten seconds. Two things say so here.
    #
    # 150 packets spread over about fifteen seconds all returning shows it from
    # the outside — if the mapping were recycled or its translation broken part
    # way through, the replies stop coming back and the loss figure says so.
    #
    # The NAT table itself says so from the inside: read once past the 10s
    # timeout and again after the run, it must hold exactly one ICMP mapping
    # toward the WAN peer both times, carrying the same translated identifier.
    # Selecting on type and destination is what makes this immune to the
    # subscriber's background traffic, which is why the plain entry count below
    # was never assertable.
    #
    # The subscriber's live NAT entry count is recorded alongside, not
    # asserted. It answers to anything that puts a flow through this
    # subscriber, and the LAN host raises its own (DNS to the node's proxy,
    # DHCP renewal, NTP, periodic distribution traffic), so its value here
    # tracks the environment as much as the mapping. The per-second sequence
    # is kept because a future surprise is much easier to read with the shape
    # of the count than with two endpoints.
    info "Step 107: running approximately 15s of ICMP echo traffic..."
    _entries_base=$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")
    _entries_seq_file=$(mktemp) || _entries_seq_file=""
    if [[ -n "$_entries_seq_file" ]]; then
        ( for _p25_i in $(seq 1 18); do
              printf '%s ' "$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")" \
                  >> "$_entries_seq_file" 2>/dev/null
              sleep 1
          done ) >/dev/null 2>&1 &
        _entries_seq_pid=$!
    fi
    _p25_icmp_attempt
    _ping_out="$_P25_PING_OUT"
    if [[ -n "${_entries_seq_pid}" ]]; then
        kill "$_entries_seq_pid" 2>/dev/null || true
        wait "$_entries_seq_pid" 2>/dev/null || true
    fi
    if [[ -n "$_entries_seq_file" ]]; then
        _entries_seq=$(tr -s ' ' < "$_entries_seq_file" 2>/dev/null || true)
        rm -f "$_entries_seq_file"
    fi
    _ping_loss=$(_p25_ping_loss "$_ping_out")
    if ! _p25_loss_is_zero "$_ping_loss"; then
        info "Step 107: first ping reported ${_ping_loss:-unparseable}% loss; retrying once..."
        _p25_icmp_attempt
        _ping_retry_out="$_P25_PING_OUT"
        _ping_loss=$(_p25_ping_loss "$_ping_retry_out")
    fi
    _ident_during=$(e2e_icmp_nat_ident "$_P25_NAT_DUMP_DURING" "$WAN_IP") || \
        _icmp_issue="read ${_P25_NAT_DUMP_AT}s into the ping: ${_ident_during}"
    _ident_after=$(e2e_icmp_nat_ident "$_P25_NAT_DUMP_AFTER" "$WAN_IP") || \
        _icmp_issue="${_icmp_issue:+${_icmp_issue}; }read after the ping: ${_ident_after}"
    if [[ -z "$_icmp_issue" && "$_ident_during" != "$_ident_after" ]]; then
        _icmp_issue="identifier changed from ${_ident_during} to ${_ident_after} — the mapping was recycled mid-run"
    fi
    _entries_after=$(_p25_fetch_metric "fastrg_node_per_user_nat_entries_used")
    if _p25_is_uint "$_entries_base" && _p25_is_uint "$_entries_after"; then
        _entries_delta=$(( _entries_after - _entries_base ))
    fi
    if _p25_loss_is_zero "$_ping_loss" && [[ -z "$_icmp_issue" ]]; then
        pass "Step 107: sustained ICMP echo" \
            "loss=${_ping_loss}% over ${_P25_PING_COUNT} packets in ~15s; one ICMP mapping to ${WAN_IP} with identifier ${_ident_during}, live both ${_P25_NAT_DUMP_AT}s into the ping and after it, so it survived the 10s idle timeout; recorded only: nat entries ${_entries_base:-NA}->${_entries_after:-NA} (delta ${_entries_delta:-NA}), during the ping: ${_entries_seq:-unavailable}"
    else
        fail "Step 107: sustained ICMP echo" \
            "loss=${_ping_loss:-NA}%${_icmp_issue:+; ICMP mapping to ${WAN_IP}: ${_icmp_issue}}; recorded only: nat entries ${_entries_base:-NA}->${_entries_after:-NA} (delta ${_entries_delta:-NA}), during the ping: ${_entries_seq:-unavailable}; first='$(_p25_snippet "$_ping_out")' retry='$(_p25_snippet "$_ping_retry_out")'"
        if [[ -n "$_icmp_issue" ]]; then
            info "  Step 107 evidence: NAT read during the ping: $(_p25_snippet "${_P25_NAT_DUMP_DURING:-unavailable}")"
            info "  Step 107 evidence: NAT read after the ping: $(_p25_snippet "${_P25_NAT_DUMP_AFTER:-unavailable}")"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 107b — RSS spreads PPPoE session traffic across the data lcores.
    #
    # Each data lcore polls exactly one RSS queue, so the per-lcore traffic
    # rows expose the per-queue distribution. A WAN-side UDP echo server
    # reflects >= 16 LAN flows with distinct source ports; each flow gets a
    # distinct NAT external port, so the echoed (downstream) packets are >= 16
    # distinct inner tuples that WAN RSS must spread over queues 1..N. The
    # assertions:
    #   - at least 2 wan_data lcores saw their WAN rx grow (with 16 flows and
    #     uniform 2-queue hashing, all-on-one-queue happens with probability
    #     ~2^-15, so the check is deterministic in practice);
    #   - the wan_ctrl lcore (queue 0) did not absorb the bulk of the session
    #     data (its delta stays below a quarter of the data-lcore total).
    # A collapse to a single queue or to queue 0 — the silent RSS/DDP
    # degradation this step exists for — fails both.
    # ------------------------------------------------------------------
    info "Step 107b: probing RSS distribution with ${_P25_RSS_FLOWS} UDP echo flows..."
    local _rss_issue="" _rss_before="" _rss_after="" _rss_client_out=""
    local _wd_lcores="" _wc_lcores="" _wd_count=0 _wd_hit=0 _wd_sum=0 _wc_delta=0
    local _lid="" _delta="" _flows_ok="" _replies="" _echo_ready=0

    _rss_before=$(e2e_metrics_body)
    _wd_lcores=$(e2e_metric_label_values "$_rss_before" \
        fastrg_node_lcore_rx_packets_total lcore_id "role=wan_data" "nic_index=1")
    _wc_lcores=$(e2e_metric_label_values "$_rss_before" \
        fastrg_node_lcore_rx_packets_total lcore_id "role=wan_ctrl" "nic_index=1")
    _wd_count=$(printf '%s\n' "$_wd_lcores" | grep -c . || true)
    if [[ "$_wd_count" -lt 2 ]]; then
        _rss_issue="expected >= 2 wan_data lcore rows, found ${_wd_count} (RSS datapath inactive or single data queue; the spread assertion needs a redesign, not a waiver)"
    fi

    if [[ -z "$_rss_issue" ]]; then
        ssh_wan "rm -f '${_P25_RSS_ECHO_PID}' '${_P25_RSS_ECHO_OUT}' '${_P25_RSS_ECHO_READY}';
nohup python3 -u - '${WAN_IP}' '${_P25_RSS_ECHO_PORT}' '${_P25_RSS_ECHO_READY}' \
    >'${_P25_RSS_ECHO_OUT}' 2>&1 <<'PY' &
import socket
import sys
import time

bind_ip, port, ready_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind((bind_ip, port))
sock.settimeout(0.5)
with open(ready_path, 'w', encoding='ascii') as ready:
    ready.write('ready\n')
deadline = time.monotonic() + 60
while time.monotonic() < deadline:
    try:
        payload, peer = sock.recvfrom(2048)
    except socket.timeout:
        continue
    sock.sendto(payload, peer)
PY
echo \$! >'${_P25_RSS_ECHO_PID}'" >/dev/null 2>&1 || true
        for _i in $(seq 1 5); do
            if ssh_wan "test -s '${_P25_RSS_ECHO_READY}'" 2>/dev/null; then
                _echo_ready=1
                break
            fi
            sleep 1
        done
        [[ $_echo_ready -eq 1 ]] || _rss_issue="UDP echo server did not become ready within 5s"
    fi

    if [[ -z "$_rss_issue" ]]; then
        _rss_client_out=$(ssh_lan "python3 -u - '${WAN_IP}' '${_P25_RSS_ECHO_PORT}' \
            '${_P25_RSS_FLOWS}' '${_P25_RSS_SRC_BASE}' '${_P25_RSS_PKTS_PER_FLOW}' <<'PY'
import socket
import sys
import time

wan_ip = sys.argv[1]
echo_port = int(sys.argv[2])
flows = int(sys.argv[3])
src_base = int(sys.argv[4])
pkts = int(sys.argv[5])

socks = []
for i in range(flows):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', src_base + i))
    sock.setblocking(False)
    socks.append(sock)

def drain(got):
    for i, sock in enumerate(socks):
        while True:
            try:
                sock.recvfrom(2048)
                got[i] += 1
            except (BlockingIOError, OSError):
                break

got = [0] * flows
sent = 0
for _round in range(pkts):
    for i, sock in enumerate(socks):
        try:
            sock.sendto(b'rss-probe-flow-%d' % i, (wan_ip, echo_port))
            sent += 1
        except OSError:
            pass
    time.sleep(0.05)
    drain(got)
deadline = time.monotonic() + 3
while time.monotonic() < deadline:
    time.sleep(0.1)
    drain(got)
print('flows_with_reply=%d replies=%d sent=%d'
      % (sum(1 for count in got if count > 0), sum(got), sent))
PY" 2>&1 || true)
        _rss_after=$(e2e_metrics_body)
        _flows_ok=$(printf '%s\n' "$_rss_client_out" | \
            sed -nE 's/.*flows_with_reply=([0-9]+).*/\1/p' | tail -1 || true)
        _replies=$(printf '%s\n' "$_rss_client_out" | \
            sed -nE 's/.*replies=([0-9]+).*/\1/p' | tail -1 || true)
        if ! e2e_is_uint "$_flows_ok" || (( _flows_ok < 12 )); then
            _rss_issue="echo traffic did not run end-to-end (flows_with_reply='${_flows_ok:-NA}' of ${_P25_RSS_FLOWS}, replies='${_replies:-NA}'); fix the probe path before judging RSS; client output='$(_p25_snippet "$_rss_client_out")'"
        fi
    fi

    if [[ -z "$_rss_issue" ]]; then
        _wd_hit=0
        _wd_sum=0
        for _lid in $_wd_lcores; do
            _delta=$(_p25_delta "$_rss_before" "$_rss_after" \
                fastrg_node_lcore_rx_packets_total "lcore_id=${_lid}" "nic_index=1")
            if ! [[ "$_delta" =~ ^-?[0-9]+$ ]]; then
                _rss_issue="${_rss_issue:+${_rss_issue}; }wan_data lcore ${_lid} rx row unreadable"
                continue
            fi
            (( _delta > 0 )) && _wd_hit=$(( _wd_hit + 1 ))
            _wd_sum=$(( _wd_sum + _delta ))
        done
        _wc_delta=0
        for _lid in $_wc_lcores; do
            _delta=$(_p25_delta "$_rss_before" "$_rss_after" \
                fastrg_node_lcore_rx_packets_total "lcore_id=${_lid}" "nic_index=1")
            [[ "$_delta" =~ ^-?[0-9]+$ ]] && _wc_delta=$(( _wc_delta + _delta ))
        done
        if (( _wd_hit < 2 )); then
            _rss_issue="${_rss_issue:+${_rss_issue}; }only ${_wd_hit}/${_wd_count} wan_data lcore(s) received session traffic (data-lcore rx deltas sum=${_wd_sum}) — RSS is not spreading"
        fi
        if (( _wc_delta * 4 > _wd_sum )); then
            _rss_issue="${_rss_issue:+${_rss_issue}; }wan_ctrl rx delta=${_wc_delta} is not clearly below the data-lcore total=${_wd_sum} — session data is landing on queue 0"
        fi
    fi

    _p25_stop_echo_server || true
    if [[ -z "$_rss_issue" ]]; then
        pass "Step 107b: RSS queue distribution" \
            "${_wd_hit}/${_wd_count} wan_data lcores took rx (deltas sum=${_wd_sum}); wan_ctrl delta=${_wc_delta}; flows_with_reply=${_flows_ok}/${_P25_RSS_FLOWS}"
    else
        fail "Step 107b: RSS queue distribution" "$_rss_issue"
    fi

    _cleanup_phase25_udp_icmp_traffic
    return 0
}
