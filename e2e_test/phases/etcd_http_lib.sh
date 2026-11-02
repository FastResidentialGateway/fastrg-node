#!/usr/bin/env bash
# shellcheck shell=bash
# ---------------------------------------------------------------------------
# etcd's HTTP API, called from the runner. Reads and writes that have to work
# while the node is cut off from etcd go this way: the block covers every
# process on the node, etcdctl included.
# ---------------------------------------------------------------------------

# Base URL of etcd's HTTP API.
e2e_etcd_http_url() {
    case "${ETCD_ENDPOINT}" in
        http://*|https://*) printf '%s' "${ETCD_ENDPOINT%/}" ;;
        *) printf 'http://%s' "${ETCD_ENDPOINT}" ;;
    esac
}

# One POST to etcd's HTTP API from the runner. Prints the reply, or "err" when
# no 200 reply came back.
e2e_etcd_http_post() {
    local _path="$1" _request="$2" _reply="" _code=""

    _reply=$(mktemp 2>/dev/null) || { printf 'err'; return 0; }
    _code=$(curl -sS --max-time 10 -o "$_reply" -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' --data-binary "$_request" \
        "$(e2e_etcd_http_url)${_path}" 2>/dev/null || true)
    if [[ "$_code" == "200" ]]; then
        cat "$_reply"
    else
        printf 'err'
    fi
    rm -f "$_reply"
    return 0
}

# The value an etcd HTTP range reply holds for KEY. Prints nothing when the key
# is absent, and "err" when BODY is not a range reply about KEY (a failed call,
# an error reply, a reply about another key), so a read that never happened is
# never taken for an absent key.
e2e_etcd_gateway_value() {
    local _body="${1-}" _key="${2-}" _value=""

    if [[ -z "$_key" ]] || ! _value=$(printf '%s' "$_body" | jq -ej --arg k "$_key" '
            if type != "object" or (.header | type) != "object" then error("not a range reply")
            elif (.kvs // []) | length == 0 then ""
            elif (.kvs | length) == 1 and (.kvs[0].key | @base64d) == $k
                then (.kvs[0].value // "") | @base64d
            else error("reply is about another key") end' 2>/dev/null); then
        printf 'err'
        return 1
    fi
    printf '%s' "$_value"
}

local_validation_register etcd_gateway_value e2e_etcd_gateway_value \
    etcd_gateway_value_found \
    etcd_gateway_value_absent \
    etcd_gateway_value_error_reply \
    etcd_gateway_value_unanswered \
    etcd_gateway_value_other_key

# KEY's value read from the runner. Same contract as etcdctl_get_value: empty
# for an absent key, non-zero (printing "err") when etcd could not be asked.
e2e_etcd_http_get() {
    local _request=""

    _request=$(jq -nc --arg k "$1" '{key: ($k | @base64)}') || { printf 'err'; return 1; }
    e2e_etcd_gateway_value "$(e2e_etcd_http_post /v3/kv/range "$_request")" "$1"
}

# Write KEY=VALUE from the runner and read it back. Non-zero unless etcd now
# holds exactly VALUE.
e2e_etcd_http_put() {
    local _key="$1" _value="$2" _request="" _now=""

    _request=$(jq -nc --arg k "$_key" --arg v "$_value" \
        '{key: ($k | @base64), value: ($v | @base64)}') || return 1
    [[ "$(e2e_etcd_http_post /v3/kv/put "$_request")" != "err" ]] || return 1
    _now=$(e2e_etcd_http_get "$_key") || return 1
    [[ "$_now" == "$_value" ]]
}

# Condition: KEY is absent from etcd, asked from the runner.
e2e_etcd_http_key_absent() {
    local _value=""

    _value=$(e2e_etcd_http_get "$1") || return 2
    [[ -z "$_value" ]]
}
