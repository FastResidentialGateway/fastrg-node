#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 2 — etcd Config Sync
# ---------------------------------------------------------------------------
phase2_etcd_config_sync() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 2 — etcd Config Sync (Steps 3–4)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 3 — etcd key exists for this subscriber
    # ------------------------------------------------------------------
    info "Step 3: Checking etcd HSI config key exists for USER_ID=${USER_ID}..."
    HSI_JSON=$(etcdctl_get_value "configs/${NODE_UUID}/hsi/${USER_ID}" 2>/dev/null || true)
    if [[ -z "$HSI_JSON" ]]; then
        fail "Step 3: etcd HSI key" "Key configs/${NODE_UUID}/hsi/${USER_ID} not found or empty"
        warn "Skipping Steps 2a-2d (no etcd data to compare against)"
        skip "Step 4a: PPPoE config match" "No etcd data"
        skip "Step 4b: DHCP config match"  "No etcd data"
        skip "Step 4c: Port-mapping match" "No etcd data"
        skip "Step 4d: DNS static match"   "No etcd data"
        return
    fi
    pass "Step 3: etcd HSI key" "configs/${NODE_UUID}/hsi/${USER_ID} exists"

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
    # Step 4a — PPPoE config loaded into fastrg
    # ------------------------------------------------------------------
    info "Step 4a: Comparing PPPoE config (gRPC GetFastrgHsiInfo vs etcd)..."
    HSI_GRPC=$(fastrg_grpc get_hsi_info)
    HSI_USER=$(printf '%s' "$HSI_GRPC" | jq -r ".hsi_infos[] | select(.user_id == ${USER_ID})" 2>/dev/null || true)

    if [[ -z "$HSI_USER" ]]; then
        fail "Step 4a: PPPoE config match" "User ID ${USER_ID} not found in gRPC GetFastrgHsiInfo response"
    else
        CLI_ACCOUNT=$(printf '%s' "$HSI_USER" | jq -r '.account // empty')
        CLI_VLAN=$(printf '%s'    "$HSI_USER" | jq -r '.vlan_id // empty')

        MISMATCH=""
        [[ "$CLI_ACCOUNT" != "$ETCD_ACCOUNT" ]] && MISMATCH="${MISMATCH} account(grpc=${CLI_ACCOUNT} etcd=${ETCD_ACCOUNT})"
        [[ "$CLI_VLAN"    != "$ETCD_VLAN"    ]] && MISMATCH="${MISMATCH} vlan(grpc=${CLI_VLAN} etcd=${ETCD_VLAN})"

        if [[ -z "$MISMATCH" ]]; then
            pass "Step 4a: PPPoE config match" "account=${CLI_ACCOUNT} vlan=${CLI_VLAN}"
        else
            fail "Step 4a: PPPoE config match" "Mismatch:${MISMATCH}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 4b — DHCP config
    # ------------------------------------------------------------------
    info "Step 4b: Comparing DHCP config (gRPC GetFastrgDhcpInfo vs etcd)..."
    DHCP_GRPC=$(fastrg_grpc get_dhcp_info)
    DHCP_USER=$(printf '%s' "$DHCP_GRPC" | jq -r ".dhcp_infos[] | select(.user_id == ${USER_ID})" 2>/dev/null || true)

    if [[ -z "$DHCP_USER" ]]; then
        fail "Step 4b: DHCP config match" "User ID ${USER_ID} not found in gRPC GetFastrgDhcpInfo response"
    else
        CLI_POOL=$(printf '%s'   "$DHCP_USER" | jq -r '.ip_range // empty')
        CLI_SUBNET=$(printf '%s' "$DHCP_USER" | jq -r '.subnet_mask // empty')
        CLI_GW=$(printf '%s'     "$DHCP_USER" | jq -r '.gateway // empty')

        # Normalize ip_range: gRPC returns "start - end", etcd uses "start~end"
        # Collapse to "start~end" for comparison
        CLI_POOL_NORM=$(printf '%s' "$CLI_POOL" | tr -d ' ' | tr '-' '~' | sed 's/~~/~/')
        ETCD_POOL_NORM=$(printf '%s' "$ETCD_POOL" | tr -d ' ' | tr '-' '~' | sed 's/~~/~/')

        MISMATCH=""
        [[ "$CLI_POOL_NORM" != "$ETCD_POOL_NORM" ]] && MISMATCH="${MISMATCH} ip_range(grpc=${CLI_POOL} etcd=${ETCD_POOL})"
        [[ "$CLI_SUBNET" != "$ETCD_SUBNET"  ]] && MISMATCH="${MISMATCH} subnet(grpc=${CLI_SUBNET} etcd=${ETCD_SUBNET})"
        [[ "$CLI_GW"     != "$ETCD_GATEWAY" ]] && MISMATCH="${MISMATCH} gateway(grpc=${CLI_GW} etcd=${ETCD_GATEWAY})"

        if [[ -z "$MISMATCH" ]]; then
            pass "Step 4b: DHCP config match" "pool=${CLI_POOL} subnet=${CLI_SUBNET} gw=${CLI_GW}"
        else
            fail "Step 4b: DHCP config match" "Mismatch:${MISMATCH}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 4c — Port-mapping config
    # ------------------------------------------------------------------
    info "Step 4c: Comparing port-mapping config (gRPC GetPortFwdInfo vs etcd)..."
    PM_COUNT=$(printf '%s' "$HSI_JSON" | jq -r '(.config["port-mapping"] // []) | length')

    if [[ "$PM_COUNT" -eq 0 ]]; then
        pass "Step 4c: Port-mapping match" "No port-mappings in etcd (nothing to verify)"
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
            pass "Step 4c: Port-mapping match" "${PM_DETAIL}"
        else
            fail "Step 4c: Port-mapping match" "${PM_DETAIL}"
        fi
    fi

    # ------------------------------------------------------------------
    # Step 4d — DNS static records
    # ------------------------------------------------------------------
    info "Step 4d: Comparing DNS static records (gRPC GetDnsStaticRecords vs etcd keys)..."
    DNS_KEYS=$(etcdctl_get_value "--prefix configs/${NODE_UUID}/${USER_ID}/dns/" 2>/dev/null || true)
    DNS_DOMAINS=$(printf '%s' "$DNS_KEYS" | jq -r '.domain // empty' 2>/dev/null || true)

    if [[ -z "$DNS_DOMAINS" ]]; then
        # Try raw key listing (key is the domain, value is JSON)
        DNS_KEYS_RAW=$(ssh_node "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} get --prefix --keys-only configs/${NODE_UUID}/${USER_ID}/dns/" 2>/dev/null || true)
        DNS_DOMAINS=$(printf '%s' "$DNS_KEYS_RAW" | awk -F'/' '{print $NF}' | grep -v '^$' || true)
    fi

    if [[ -z "$DNS_DOMAINS" ]]; then
        pass "Step 4d: DNS static match" "No DNS static keys in etcd (nothing to verify)"
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
            pass "Step 4d: DNS static match" "${DNS_DETAIL}"
        else
            fail "Step 4d: DNS static match" "${DNS_DETAIL}"
        fi
    fi
}
