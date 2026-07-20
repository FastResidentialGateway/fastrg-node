#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 0 — Prerequisites
# ---------------------------------------------------------------------------
_FASTRG_DAEMON="/root/fastrg-node/fastrg"
_FASTRG_START_CMD="${_FASTRG_DAEMON} -l 1-8 -n 4 --socket-mem 17408 -a 0000:07:00.0 -a 0000:08:00.0"

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
    # Controller connectivity (writes go through the controller now).
    # Clear any stale cached token, then verify login works.
    # ------------------------------------------------------------------
    rm -f /tmp/.fastrg_e2e_ctrl_token 2>/dev/null || true
    info "Checking controller login at ${CONTROLLER_REST}..."
    # Use the python client (urllib) — the runner may not have curl, and the
    # client is exactly what the write paths use to authenticate.
    _ctrl_login_chk=$(fastrg_grpc ctrl_login 2>/dev/null \
        | python3 -c "import sys,json; print(json.load(sys.stdin).get('token_preview',''))" 2>/dev/null || true)
    if [[ -z "$_ctrl_login_chk" ]]; then
        error "Cannot log in to controller at ${CONTROLLER_REST} (user=${CONTROLLER_USER})."
        error "Config writes route through the controller — aborting."
        exit 1
    fi
    info "Controller login OK (token acquired)."

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
    # Ensure the canonical USER_ID test fixture exists in etcd. The suite
    # assumes the subscriber config is pre-provisioned (HSI + DNS static).
    # If it is missing, seed it via restore_etcd_config.sh on the node so the
    # node picks it up on boot (must happen BEFORE the daemon starts).
    # ------------------------------------------------------------------
    info "Checking etcd HSI fixture for USER_ID=${USER_ID}..."
    _seed_hsi=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
    # Validate the fixture: it must exist AND have a non-zero vlan_id (vlan=0 means
    # a previous run left a bad/un-rolled-back test config — reseed in that case too).
    _seed_vlan=$(printf '%s' "$_seed_hsi" | jq -r '.config.vlan_id // empty' 2>/dev/null || true)
    if [[ -z "$_seed_hsi" ]] || [[ -z "$_seed_vlan" ]] || [[ "$_seed_vlan" == "0" ]]; then
        if [[ -z "$_seed_hsi" ]]; then
            warn "etcd HSI config for USER_ID=${USER_ID} missing — seeding..."
        else
            warn "etcd HSI config for USER_ID=${USER_ID} is corrupt/stale (vlan=${_seed_vlan:-empty}) — reseeding..."
        fi
        ssh_node "bash /root/fastrg-node/e2e_test/restore_etcd_config.sh --force" 2>&1 | sed 's/^/    /'
        _seed_hsi=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
        if [[ -z "$_seed_hsi" ]]; then
            error "Failed to seed etcd HSI fixture for USER_ID=${USER_ID}."
            exit 1
        fi
        info "Seeded etcd HSI fixture for USER_ID=${USER_ID}."
    else
        info "etcd HSI fixture present (vlan=${_seed_vlan})."
    fi

    # ------------------------------------------------------------------
    # Preflight: remove HSI keys whose user_id is beyond the canonical
    # subscriber count. Interrupted synthetic-user tests can otherwise leave
    # a VLAN-conflicting key that poisons controller reconciliation.
    # ------------------------------------------------------------------
    info "Preflight: sweeping stray HSI keys before starting test traffic..."
    local _preflight_count_json _preflight_count _preflight_keys _preflight_key
    local _preflight_uid _preflight_value _preflight_vlan _preflight_updated _preflight_after
    local _preflight_found=0
    _preflight_count_json=$(etcdctl_get_value "user_counts/${NODE_UUID}/" 2>/dev/null || true)
    _preflight_count=$(printf '%s' "$_preflight_count_json" | \
        jq -r '.subscriber_count // empty' 2>/dev/null | tr -d '[:space:]' || true)
    if [[ ! "$_preflight_count" =~ ^[0-9]+$ ]] || [[ "$_preflight_count" -lt 1 ]]; then
        _preflight_count=2
        warn "Preflight: subscriber count unavailable; using canonical fixture count ${_preflight_count}."
    else
        info "Preflight: canonical subscriber count is ${_preflight_count}."
    fi

    _preflight_keys=$(ssh_node \
        "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} get --prefix --keys-only configs/${NODE_UUID}/hsi/" \
        2>/dev/null || true)
    while IFS= read -r _preflight_key; do
        [[ -z "$_preflight_key" ]] && continue
        _preflight_uid="${_preflight_key##*/}"
        [[ "$_preflight_uid" =~ ^[0-9]+$ ]] || continue
        [[ "$_preflight_uid" -le "$_preflight_count" ]] && continue

        _preflight_found=1
        _preflight_value=$(etcdctl_get_value "${_preflight_key}" 2>/dev/null || true)
        _preflight_vlan=$(printf '%s' "$_preflight_value" | \
            jq -r '.config.vlan_id // empty' 2>/dev/null || true)
        _preflight_updated=$(printf '%s' "$_preflight_value" | \
            jq -r '.metadata.updatedAt // empty' 2>/dev/null || true)
        warn "Preflight stray HSI: key=${_preflight_key} vlan_id=${_preflight_vlan:-unknown} updatedAt=${_preflight_updated:-unknown}; deleting."
        ssh_node "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} del ${_preflight_key}" \
            >/dev/null 2>&1 || true
        _preflight_after=$(etcdctl_get_value "${_preflight_key}" 2>/dev/null || true)
        if [[ -z "$_preflight_after" ]]; then
            info "Preflight: verified stray key ${_preflight_key} was removed."
        else
            warn "Preflight FAILED: stray key ${_preflight_key} remains after direct etcd delete."
        fi
    done <<< "$_preflight_keys"
    if [[ $_preflight_found -eq 0 ]]; then
        info "Preflight: no stray HSI keys found."
    fi

    # ------------------------------------------------------------------
    # Start dpdk-bras on the BRAS endpoint BEFORE the node starts, so the
    # node's PPPoE sessions have a server to dial into on boot. dpdk-bras is
    # the PPPoE/BRAS simulator; it serves VLAN 3 (subscriber 2) and VLAN 5
    # (subscriber 1). It is killed again in cleanup_fastrg on exit.
    # ------------------------------------------------------------------
    info "Checking SSH connectivity to BRAS endpoint (${BRAS_HOST})..."
    if ! ssh_bras "true" 2>/dev/null; then
        error "Cannot SSH to BRAS endpoint at ${BRAS_HOST}"
        exit 1
    fi
    info "Starting dpdk-bras on BRAS endpoint (${BRAS_HOST})..."
    # /root/dpdk-bras is the repo dir; the binary is /root/dpdk-bras/dpdk-bras and
    # --drop-pcap ./test.pcap is relative to it, so launch from inside that dir.
    # Kill any stale instance first (match the exact process name, not the path —
    # pgrep/pkill -f dpdk-bras would also match our own ssh command line).
    ssh_bras "pkill -x dpdk-bras 2>/dev/null || true" 2>/dev/null || true
    sleep 1
    # dpdk-bras runs in the foreground on the bras host for the whole test and
    # streams stats forever, so it never closes the SSH channel. Detaching it
    # remotely does not help (DPDK re-opens fds, so setsid/nohup still wedge the
    # channel). Instead run it foreground over a locally-backgrounded SSH client:
    # this returns control to phase0 immediately while the SSH connection stays
    # up holding the process. cleanup_fastrg kills dpdk-bras on the bras host,
    # which makes this SSH exit.
    ssh_bras "cd /root/dpdk-bras && exec ./dpdk-bras -l 0-7 -n 4 -- --pri-dns 192.168.10.1 --drop-pcap ./test.pcap --vlans 3,5 >/var/log/dpdk-bras.log 2>&1" </dev/null >/dev/null 2>&1 &
    _BRAS_SSH_PID=$!
    _BRAS_STARTED_BY_SCRIPT=1

    # Wait for the process to come up (DPDK EAL init takes a few seconds).
    _bras_ok=0
    for _i in $(seq 1 12); do
        sleep 2
        if ssh_bras "pgrep -x dpdk-bras >/dev/null 2>&1"; then
            _bras_ok=1
            break
        fi
    done
    if [[ $_bras_ok -eq 1 ]]; then
        info "dpdk-bras is running on ${BRAS_HOST}."
    else
        error "dpdk-bras did not start on ${BRAS_HOST} within 24s."
        ssh_bras "tail -20 /var/log/dpdk-bras.log 2>/dev/null || true" 2>/dev/null || true
        exit 1
    fi

    # ------------------------------------------------------------------
    # The LAN peer holds a VF of enp1s0f0 on the WAN host, and the
    # dhcp_client_sim.py-based phases (22/24) emulate virtual clients with
    # arbitrary source MACs — the PF's MAC anti-spoof would drop their TX.
    # The setting resets on host reboot or ixgbe reload, so re-assert it.
    # ------------------------------------------------------------------
    if ssh_wan "ip link set enp1s0f0 vf 0 spoofchk off" >/dev/null 2>&1; then
        info "Preflight: LAN PF VF 0 spoofchk=off asserted."
    else
        warn "Preflight: failed to disable VF spoofchk on ${WAN_HOST}; phases 22/24 may fail."
    fi

    # ------------------------------------------------------------------
    # Ensure fastrg daemon is running; start it if not
    # ------------------------------------------------------------------
    info "Checking if fastrg daemon is running on ${FASTRG_NODE}..."
    FASTRG_PID=$(ssh_node "pgrep -x fastrg 2>/dev/null || pidof fastrg 2>/dev/null || true" | tr -d '[:space:]')
    if [[ -z "$FASTRG_PID" ]]; then
        local _rtemap_removed
        _rtemap_removed=$(ssh_node '
            _rtemap_before=0
            for _rtemap_file in /dev/hugepages/rtemap_*; do
                [ -e "$_rtemap_file" ] || continue
                _rtemap_before=$((_rtemap_before + 1))
            done
            rm -f -- /dev/hugepages/rtemap_* 2>/dev/null || true
            _rtemap_after=0
            for _rtemap_file in /dev/hugepages/rtemap_*; do
                [ -e "$_rtemap_file" ] || continue
                _rtemap_after=$((_rtemap_after + 1))
            done
            printf "%s\n" "$((_rtemap_before - _rtemap_after))"
        ' 2>/dev/null || true)
        [[ "$_rtemap_removed" =~ ^[0-9]+$ ]] || _rtemap_removed=0
        info "Preflight: removed ${_rtemap_removed} stale /dev/hugepages/rtemap_* file(s)."

        warn "fastrg is NOT running — attempting to start..."
        info "Starting: ${_FASTRG_START_CMD}"
        ssh_node "nohup ${_FASTRG_START_CMD} >/var/log/fastrg.log 2>&1 &"
        _FASTRG_STARTED_BY_SCRIPT=1

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
            info "  still waiting... (${_i}x5s, hsi_infos=${_cnt}, account='${_acct}')"
        done
        if [[ $_ready -eq 0 ]]; then
            error "fastrg gRPC did not become ready within 120 seconds."
            info "Last log output:"
            ssh_node "tail -20 /var/log/fastrg.log 2>/dev/null || true"
            exit 1
        fi

        # Additionally wait for DNS static records to be loaded (needed for step 4d)
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
            info "  waiting for DNS static records... (${_i}x5s)"
            sleep 5
        done
        [[ $_dns_ready -eq 0 ]] && info "  (DNS static not loaded yet — will verify in step 4d)"

        info "fastrg gRPC is ready."
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

        # Ensure desire_status=connect so fastrg dials immediately without
        # waiting up to 60s for the next reconcile cycle.
        info "Triggering connect_hsi for USER_ID=${USER_ID} to ensure desire_status=connect..."
        fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true

        # Wait for the primary subscriber PPPoE session to reach Data phase.
        info "Waiting for USER_ID=${USER_ID} PPPoE session to reach Data phase (up to 120s)..."
        _dp_ready=0
        for _i in $(seq 1 24); do
            _hsi=$(python3 "${GRPC_CLIENT_DIR}/fastrg_grpc_client.py" \
                      --node "${FASTRG_NODE}:${FASTRG_GRPC_PORT}" \
                      get_hsi_info 2>/dev/null || true)
            _phase=$(printf '%s' "$_hsi" | \
                python3 -c "import sys,json; d=json.load(sys.stdin); \
                u=[h for h in d.get('hsi_infos',[]) if h.get('user_id')==${USER_ID}]; \
                print(u[0].get('status','') if u else '')" 2>/dev/null || true)
            if [[ "$_phase" == "Data phase" ]]; then
                _dp_ready=1
                break
            fi
            info "  still waiting... (${_i}x5s, status='${_phase}')"
            sleep 5
        done
        if [[ $_dp_ready -eq 0 ]]; then
            error "USER_ID=${USER_ID} did not reach Data phase within 120s (status='${_phase}')."
            exit 1
        fi
        info "USER_ID=${USER_ID} PPPoE session is in Data phase."
    fi

    # ------------------------------------------------------------------
    # Refresh the LAN host's subscriber DHCP lease. After a node (re)start
    # the LAN device may still hold a stale lease from a previous node
    # instance (NetworkManager keeps the "valid" lease, so a plain
    # `netplan apply` won't re-acquire). Force a release+renew so the node's
    # DHCP server actually hands out the lease (otherwise Step 5 flakes).
    # vlan3 is NM-managed via the "netplan-vlan3" connection.
    # ------------------------------------------------------------------
    info "Refreshing LAN host (${LAN_HOST}) subscriber DHCP lease (vlan3)..."
    ssh_lan "nmcli con down netplan-vlan3 >/dev/null 2>&1; nmcli con up netplan-vlan3 >/dev/null 2>&1 || netplan apply >/dev/null 2>&1; true" 2>/dev/null || true
    # Give DHCP a moment to complete before the DHCP checks in later phases.
    for _i in $(seq 1 6); do
        _lan_ip=$(ssh_lan "ip -4 addr show vlan3 2>/dev/null | grep -oE '192\\.168\\.4\\.[0-9]+' | head -1" 2>/dev/null | tr -d '[:space:]')
        [[ -n "$_lan_ip" ]] && { info "  LAN host vlan3 lease: ${_lan_ip}"; break; }
        sleep 2
    done
    [[ -z "$_lan_ip" ]] && warn "  LAN host did not obtain a vlan3 lease yet (Step 5 may flake)."

    printf "\n"
    info "Configuration summary:"
    printf "  SUBSCRIBERS     : %s (primary=%s)\n" "${SUB_IDS[*]}" "$USER_ID"
    printf "  BRAS_HOST       : %s\n" "$BRAS_HOST"
    printf "  FASTRG_NODE     : %s\n" "$FASTRG_NODE"
    printf "  FASTRG_GRPC     : %s:%s\n" "$FASTRG_NODE" "$FASTRG_GRPC_PORT"
    printf "  LAN_HOST        : %s (user: the)\n" "$LAN_HOST"
    printf "  WAN_HOST        : %s\n" "$WAN_HOST"
    printf "  WAN_IP          : %s\n" "$WAN_IP"
    printf "  NODE_UUID       : %s\n" "$NODE_UUID"
    printf "  ETCD_ENDPOINT   : %s\n" "$ETCD_ENDPOINT"
    printf "  CONTROLLER_REST : %s\n" "$CONTROLLER_REST"
    printf "  CONTROLLER_GRPC : %s\n" "$CONTROLLER_GRPC"
    printf "\n"
}
