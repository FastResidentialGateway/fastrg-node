#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# Phase 11 — Kafka telemetry pipeline (Steps 41-44)
#
# Before the existing Kafka-fed PPPoE assertions, inject one invalid config for
# a synthetic user and verify both controller stages independently:
#   Step 41  Kafka consumer persists CONFIG_APPLY_FAIL in the controller DB
#   Step 42  rollback worker removes the invalid new-user key from etcd
#   Step 43  controller DB has a PPPoE status record for USER_ID (Kafka-fed)
#   Step 44  after a dial, the DB status reflects a connected node phase
# ---------------------------------------------------------------------------

_P11_HEALTH_UID=""

# Fetch the controller's Kafka-fed config-apply event records. The controller
# exposes no consumer-specific health/metrics signal; /api/failed-events is the
# DB-backed observation used by its own UI and is therefore the strongest
# available proof that the consumer processed this run's synthetic event.
_p11_controller_failed_events() {
    P11_CONTROLLER_REST="$CONTROLLER_REST" \
        P11_CONTROLLER_USER="$CONTROLLER_USER" \
        P11_CONTROLLER_PASS="$CONTROLLER_PASS" \
        python3 - <<'PY'
import json
import os
import ssl
import urllib.error
import urllib.parse
import urllib.request

context = ssl.create_default_context()
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE
token_cache = "/tmp/.fastrg_e2e_ctrl_token"


def login():
    body = json.dumps({
        "username": os.environ["P11_CONTROLLER_USER"],
        "password": os.environ["P11_CONTROLLER_PASS"],
    }).encode()
    request = urllib.request.Request(
        os.environ["P11_CONTROLLER_REST"] + "/api/login",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, context=context, timeout=10) as response:
        token = json.loads(response.read())["token"]
    with open(token_cache, "w", encoding="utf-8") as token_file:
        token_file.write(token)
    return token


def fetch(token):
    query = urllib.parse.urlencode({"event_type": "CONFIG_APPLY_FAIL"})
    request = urllib.request.Request(
        os.environ["P11_CONTROLLER_REST"] + "/api/failed-events?" + query,
        headers={"Authorization": token},
        method="GET",
    )
    with urllib.request.urlopen(request, context=context, timeout=10) as response:
        return json.loads(response.read())


try:
    with open(token_cache, encoding="utf-8") as token_file:
        token = token_file.read().strip()
    events = fetch(token)
except (FileNotFoundError, urllib.error.HTTPError):
    events = fetch(login())
print(json.dumps(events, separators=(",", ":")))
PY
}

# Idempotent and verified: normally the controller deletes this key inline.
# If the health check or run is interrupted, remove it directly and verify so
# no out-of-range synthetic user can poison a later run.
_cleanup_phase11_kafka_pipeline() {
    [[ -z "${NODE_UUID:-}" || -z "${_P11_HEALTH_UID:-}" ]] && return 0

    local _key="configs/${NODE_UUID}/hsi/${_P11_HEALTH_UID}"
    local _remaining
    _remaining=$(etcdctl_get_value "${_key}" 2>/dev/null || true)
    if [[ -z "$_remaining" ]]; then
        _P11_HEALTH_UID=""
        return 0
    fi

    info "Cleanup(phase11): removing synthetic Kafka health key ${_key}..."
    if ! ssh_node "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} del ${_key}" >/dev/null 2>&1; then
        warn "Cleanup(phase11) FAILED: etcd delete failed for ${_key}."
        return 1
    fi

    _remaining=$(etcdctl_get_value "${_key}" 2>/dev/null || true)
    if [[ -n "$_remaining" ]]; then
        warn "Cleanup(phase11) FAILED: ${_key} remains after direct etcd delete."
        return 1
    fi

    info "Cleanup(phase11): verified ${_key} is absent."
    _P11_HEALTH_UID=""
    return 0
}

phase11_kafka_pipeline() {
    bold "═══════════════════════════════════════════════════════"
    bold " Phase 11 — Kafka telemetry pipeline (Steps 41-44)"
    bold "═══════════════════════════════════════════════════════"

    # ------------------------------------------------------------------
    # Steps 41-42 — one synthetic apply failure proves the controller's
    # consumer and rollback worker are both making progress.
    # ------------------------------------------------------------------
    local _p11_count _p11_uid _p11_vlan _p11_key _p11_started _p11_payload
    local _p11_events _p11_baseline_id _p11_event="" _p11_event_id=""
    local _p11_event_seen=0 _p11_rollback_seen=0 _p11_event_elapsed=0 _p11_rollback_elapsed=0
    local _p11_put_ok=0 _p11_value _i

    _p11_count=$(fastrg_grpc get_system_info 2>/dev/null | \
        jq -r '.num_users // empty' 2>/dev/null || true)
    [[ "$_p11_count" =~ ^[0-9]+$ ]] || _p11_count=2
    _p11_uid=$(( _p11_count + 60 ))
    _p11_vlan=$(( 3000 + (_p11_uid % 900) ))
    _p11_key="configs/${NODE_UUID}/hsi/${_p11_uid}"
    _P11_HEALTH_UID="$_p11_uid"

    _p11_events=$(_p11_controller_failed_events 2>/dev/null || true)
    _p11_baseline_id=$(printf '%s' "$_p11_events" | \
        jq -r '[.events[]?.id // 0] | max // 0' 2>/dev/null || true)
    [[ "$_p11_baseline_id" =~ ^[0-9]+$ ]] || _p11_baseline_id=0

    _p11_started=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    _p11_payload=$(printf \
        '{"config":{"account_name":"e2e_kafka_health","dhcp_addr_pool":"10.98.0.2-10.98.0.10","dhcp_gateway":"10.98.0.1","dhcp_subnet":"255.255.255.0","password":"x","user_id":"%s","vlan_id":"%s","desire_status":"connect"},"metadata":{"node":"%s","resourceVersion":"1","updatedAt":"%s","updatedBy":"e2e_kafka_health"}}' \
        "${_p11_uid}" "${_p11_vlan}" "${NODE_UUID}" "${_p11_started}")

    info "Steps 41-42: injecting synthetic user=${_p11_uid} vlan=${_p11_vlan} \
        (baseline event id=${_p11_baseline_id})..."
    if printf '%s' "$_p11_payload" | ssh_node \
        "ETCDCTL_API=3 etcdctl --endpoints=${ETCD_ENDPOINT} put ${_p11_key}" \
        >/dev/null 2>&1; then
        _p11_put_ok=1
    fi
    if [[ $_p11_put_ok -eq 0 ]]; then
        fail "Step 41: controller Kafka consumer health" \
            "controller-side: synthetic CONFIG_APPLY_FAIL injection could not be established"
        fail "Step 42: controller rollback worker health" \
            "controller-side: synthetic key was unavailable for the rollback check"
        _cleanup_phase11_kafka_pipeline || \
            warn "Cleanup(phase11): manual cleanup may be required for ${_p11_key}."
    else
        for _i in $(seq 1 30); do
            sleep 2

            if [[ $_p11_event_seen -eq 0 ]]; then
                _p11_events=$(_p11_controller_failed_events 2>/dev/null || true)
                _p11_event=$(printf '%s' "$_p11_events" | \
                    jq -c --argjson baseline "$_p11_baseline_id" \
                        --arg node "$NODE_UUID" --arg uid "$_p11_uid" --arg started "$_p11_started" \
                        '.events[]? |
                         select((.id // 0) > $baseline and .node_uuid == $node and
                                ((.user_id // "") | tostring) == $uid and
                                .event_type == "CONFIG_APPLY_FAIL" and
                                (.event_time // "") >= $started)' \
                        2>/dev/null | head -1 || true)
                if [[ -n "$_p11_event" ]]; then
                    _p11_event_seen=1
                    _p11_event_elapsed=$(( _i * 2 ))
                    _p11_event_id=$(printf '%s' "$_p11_event" | jq -r '.id // empty' 2>/dev/null || true)
                fi
            fi

            if [[ $_p11_rollback_seen -eq 0 ]]; then
                _p11_value=$(etcdctl_get_value "${_p11_key}" 2>/dev/null || true)
                if [[ -z "$_p11_value" ]]; then
                    _p11_rollback_seen=1
                    _p11_rollback_elapsed=$(( _i * 2 ))
                fi
            fi

            [[ $_p11_event_seen -eq 1 && $_p11_rollback_seen -eq 1 ]] && break
            info "  ${_i}x2s: consumer_event=${_p11_event_seen} rollback_delete=${_p11_rollback_seen}"
        done

        if [[ $_p11_event_seen -eq 1 ]]; then
            pass "Step 41: controller Kafka consumer health" \
                "controller DB recorded CONFIG_APPLY_FAIL id=${_p11_event_id} \
                for user ${_p11_uid} after ${_p11_event_elapsed}s"
        else
            fail "Step 41: controller Kafka consumer health" \
                "controller-side: CONFIG_APPLY_FAIL for synthetic user ${_p11_uid} \
                was not persisted within 60s; inspect the Kafka consumer"
        fi

        if [[ $_p11_rollback_seen -eq 1 ]]; then
            pass "Step 42: controller rollback worker health" \
                "controller removed ${_p11_key} after ${_p11_rollback_elapsed}s"
        else
            fail "Step 42: controller rollback worker health" \
                "controller-side: ${_p11_key} remained after 60s \
                (consumer_event=${_p11_event_seen}); inspect the rollback worker"
        fi

        _cleanup_phase11_kafka_pipeline || \
            warn "Cleanup(phase11): manual cleanup may be required for ${_p11_key}."
    fi

    # ------------------------------------------------------------------
    # Step 43 — controller DB has a PPPoE status record for USER_ID
    # ------------------------------------------------------------------
    info "Step 43: querying controller DB PPPoE status for USER_ID=${USER_ID}..."
    _p12_rec=$(fastrg_grpc ctrl_pppoe "${USER_ID}" 2>/dev/null || true)
    _p12_uid=$(printf '%s' "$_p12_rec" | jq -r '.user_id // empty' 2>/dev/null || true)
    info "  controller pppoe record: $(printf '%s' "$_p12_rec" | tr -d '\n')"

    if [[ -n "$_p12_uid" ]]; then
        pass "Step 43: controller DB PPPoE record (Kafka-fed)" \
            "controller has a pppoe status record for user ${USER_ID}"
    else
        fail "Step 43: controller DB PPPoE record (Kafka-fed)" \
            "no pppoe status record at controller for user ${USER_ID}"
    fi

    # ------------------------------------------------------------------
    # Step 44 — after a dial, DB status tracks the node reaching a session
    # ------------------------------------------------------------------
    info "Step 44: dial USER_ID=${USER_ID} and wait for controller DB status to track it..."
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
        pass "Step 44: PPPoE transition via Kafka" \
            "controller DB status='${_p12_status}' (delivered via Kafka)"
    else
        fail "Step 44: PPPoE transition via Kafka" \
            "controller DB status did not reflect a session within 60s (last='${_p12_status:-<empty>}'; remote PPPoE server may be slow)"
    fi
}
