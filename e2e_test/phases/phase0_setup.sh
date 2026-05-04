#!/usr/bin/env bash
# shellcheck shell=bash
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
