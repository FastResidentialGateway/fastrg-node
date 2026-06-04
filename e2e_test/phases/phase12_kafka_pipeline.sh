#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 12 — Kafka telemetry pipeline (Steps 41-42)
#
# The node reports config-apply results and PPPoE state transitions to the
# controller over Kafka; the controller persists them. Verify the controller's
# observed PPPoE status (DB, fed by Kafka) is populated and tracks the node.
#   Step 41  controller DB has a PPPoE status record for USER_ID (Kafka-fed)
#   Step 42  after a dial, the DB status reflects a connected node phase
# ---------------------------------------------------------------------------

phase12_kafka_pipeline() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 12 — Kafka telemetry pipeline (Steps 41-42)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Step 41 — controller DB has a PPPoE status record for USER_ID
    # ------------------------------------------------------------------
    info "Step 41: querying controller DB PPPoE status for USER_ID=${USER_ID}..."
    _p12_rec=$(fastrg_grpc ctrl_pppoe "${USER_ID}" 2>/dev/null || true)
    _p12_uid=$(printf '%s' "$_p12_rec" | jq -r '.user_id // empty' 2>/dev/null || true)
    info "  controller pppoe record: $(printf '%s' "$_p12_rec" | tr -d '\n')"

    if [[ -n "$_p12_uid" ]]; then
        pass "Step 41: controller DB PPPoE record (Kafka-fed)" \
            "controller has a pppoe status record for user ${USER_ID}"
    else
        fail "Step 41: controller DB PPPoE record (Kafka-fed)" \
            "no pppoe status record at controller for user ${USER_ID}"
    fi

    # ------------------------------------------------------------------
    # Step 42 — after a dial, DB status tracks the node reaching a session
    # ------------------------------------------------------------------
    info "Step 42: dial USER_ID=${USER_ID} and wait for controller DB status to track it..."
    fastrg_grpc connect_hsi "${USER_ID}" >/dev/null 2>&1 || true

    _p12_ok=0
    _p12_status=""
    for _i in $(seq 1 12); do
        sleep 5
        _p12_status=$(fastrg_grpc ctrl_pppoe "${USER_ID}" 2>/dev/null | \
            jq -r '.status // empty' 2>/dev/null || true)
        # Node phase strings flow through Kafka into the controller DB. Any
        # connected/in-progress phase (not "not configured"/empty) proves delivery.
        if [[ -n "$_p12_status" ]] && \
           printf '%s' "$_p12_status" | grep -qiE "Data phase|connect|LCP|IPCP|Auth|PPPoE phase"; then
            _p12_ok=1
            break
        fi
        info "  waiting for controller DB to reflect PPPoE progress... (${_i}x5s, status='${_p12_status}')"
    done

    if [[ $_p12_ok -eq 1 ]]; then
        pass "Step 42: PPPoE transition via Kafka" \
            "controller DB status='${_p12_status}' (delivered via Kafka)"
    else
        fail "Step 42: PPPoE transition via Kafka" \
            "controller DB status did not reflect a session within 60s (last='${_p12_status:-<empty>}'; remote PPPoE server may be slow)"
    fi
}
