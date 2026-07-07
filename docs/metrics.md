# FastRG Node Prometheus Metrics

This document describes the metrics a **FastRG Node** exposes to Prometheus, with
each metric's type, labels, and meaning. It is generated from the node's own
metrics implementation ([src/metrics.c](../src/metrics.c)) — the node serves its
`/metrics` endpoint directly and Prometheus scrapes it.

## Overview

- **Endpoint**: `http://<node>:<MetricsListenPort>/metrics`, served by the node's
  built-in `lighthttp` server ([src/lighthttp.c](../src/lighthttp.c)) on a
  dedicated thread. The listen address is the `MetricsListenPort` field in the
  node `config.cfg` (e.g. `"55178"`).
- **Scrape model**: Prometheus scrapes each node **directly** (job `fastrg-node`).
  Node liveness is therefore the native `up{job="fastrg-node"}` series — the
  controller no longer polls and re-publishes these metrics.
- **Format**: Prometheus text exposition (`text/plain; version=0.0.4`).
- **Metric prefix**: `fastrg_node_*` for node/datapath stats, `fastrg_nic_*` for
  per-port NIC link state and metadata.
- Only `GET /metrics` is served; any other path returns `404`.

### A note on `_total` metric types

Most `_total`-suffixed metrics here (NIC traffic, per-user/per-session traffic,
lcore cycles) are **Gauges**, not Counters: their value is the cumulative figure
the datapath reports, so it increases monotonically but **resets to 0 when the
node restarts**. This matches the historical controller schema for dashboard
compatibility. Use `rate()` / `irate()` for rates — both handle the restart
reset:

```promql
rate(fastrg_node_rx_packets_total[1m])     # received packets/sec for a NIC port
```

Two metrics are genuine **Counters**: `fastrg_node_restart_total` and
`fastrg_nic_link_flaps_total`.

## Common labels

| Label | Description |
|-------|-------------|
| `node_uuid` | Node UUID. Present on **every** `fastrg_*` metric. |
| `nic_index` | NIC port index for traffic stats: `0` = LAN, `1` = WAN. |
| `port_id` | NIC port index for link-state metrics: `0` = LAN, `1` = WAN. |
| `user_id` | Subscriber / HSI user ID (1-based). |
| `lcore_id` | DPDK logical core ID. |
| `role` | Datapath role of the lcore (`ctrl`, `wan_ctrl`, `lan_ctrl`, `wan_data`, `lan_data`). |
| `socket_id` | NUMA socket ID. |
| `pool` | DPDK mempool name. |

Prometheus also adds `instance` (the scraped `host:port`) and `job` automatically.

---

## 1. Node liveness

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `fastrg_node_start_time_seconds` | gauge | `node_uuid` | Unix time (seconds) the process started; changes on restart. |
| `fastrg_node_restart_total` | counter | `node_uuid` | Cumulative process start count, persisted across restarts (`/var/lib/fastrg/restart_count`) — crashloop detection. |

```promql
increase(fastrg_node_restart_total[15m]) > 2      # crashlooping
changes(fastrg_node_start_time_seconds[15m]) > 2  # (alternative) restarted repeatedly
```

> **Node up/down across the cluster** is the native scrape signal, not a node
> metric:
> ```promql
> up{job="fastrg-node"} == 0                 # node offline
> changes(up{job="fastrg-node"}[10m]) > 4    # node flapping (network-level)
> ```

## 2. NIC link state & metadata

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `fastrg_nic_link_up` | gauge | `node_uuid`, `port_id` | NIC link state: `1` = up, `0` = down. |
| `fastrg_nic_link_speed_mbps` | gauge | `node_uuid`, `port_id` | Link speed in Mbps (`0` when down). |
| `fastrg_nic_link_flaps_total` | counter | `node_uuid`, `port_id` | Cumulative link up/down transitions (incremented in the LSI callback, so sub-scrape flaps are not lost). |
| `fastrg_nic_info` | gauge | `node_uuid`, `port_id`, `model`, `driver`, `pci`, `mac` | NIC metadata; value is always `1` (info metric — join on `node_uuid,port_id`). |

```promql
fastrg_nic_link_up == 0                              # ports currently down
count(fastrg_nic_link_up == 0)                       # how many ports down
increase(fastrg_nic_link_flaps_total[10m]) > 3       # flapping link
# enrich up/down with model/pci:
fastrg_nic_link_up * on(node_uuid,port_id) group_left(model,pci,mac) fastrg_nic_info
```

## 3. NIC traffic statistics (per port)

Labels: `node_uuid`, `nic_index` (`0`=LAN, `1`=WAN). Source: `rte_eth_stats_get()`.

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_rx_packets_total` | gauge | Total received packets. |
| `fastrg_node_tx_packets_total` | gauge | Total transmitted packets. |
| `fastrg_node_rx_bytes_total` | gauge | Total received bytes. |
| `fastrg_node_tx_bytes_total` | gauge | Total transmitted bytes. |
| `fastrg_node_rx_errors_total` | gauge | Total receive errors (`ierrors`). |
| `fastrg_node_tx_errors_total` | gauge | Total transmit errors (`oerrors`). |
| `fastrg_node_rx_dropped_total` | gauge | RX packets dropped — no mbuf / ring full (`imissed`). |

## 4. Per-subscriber traffic

Labels: `node_uuid`, `nic_index`, `user_id`.

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_per_user_rx_packets_total` | gauge | Per-subscriber received packets. |
| `fastrg_node_per_user_rx_bytes_total` | gauge | Per-subscriber received bytes. |
| `fastrg_node_per_user_tx_packets_total` | gauge | Per-subscriber transmitted packets. |
| `fastrg_node_per_user_tx_bytes_total` | gauge | Per-subscriber transmitted bytes. |
| `fastrg_node_per_user_dropped_packets_total` | gauge | Per-subscriber dropped packets. |
| `fastrg_node_per_user_dropped_bytes_total` | gauge | Per-subscriber dropped bytes. |

### Unmapped ("unknown user") traffic

Labels: `node_uuid`, `nic_index`. Traffic that did not map to a known subscriber.

| Metric | Type |
|--------|------|
| `fastrg_node_unknown_user_rx_packets_total` | gauge |
| `fastrg_node_unknown_user_rx_bytes_total` | gauge |
| `fastrg_node_unknown_user_tx_packets_total` | gauge |
| `fastrg_node_unknown_user_tx_bytes_total` | gauge |
| `fastrg_node_unknown_user_dropped_packets_total` | gauge |
| `fastrg_node_unknown_user_dropped_bytes_total` | gauge |

## 5. PPPoE sessions

### Phase tallies — labels: `node_uuid`

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_total_pppoe_data_sessions` | gauge | Sessions in Data phase (established). |
| `fastrg_node_total_pppoe_ipcp_sessions` | gauge | Sessions in IPCP phase. |
| `fastrg_node_total_pppoe_auth_sessions` | gauge | Sessions in Auth phase. |
| `fastrg_node_total_pppoe_lcp_sessions` | gauge | Sessions in LCP phase. |
| `fastrg_node_total_pppoe_init_sessions` | gauge | Sessions in PPPoE discovery (Init). |
| `fastrg_node_total_pppoe_terminated_sessions` | gauge | Sessions in End phase (terminated). |
| `fastrg_node_total_pppoe_not_configured_sessions` | gauge | Subscriber slots with no PPPoE configured. |
| `fastrg_node_total_pppoe_error_sessions` | gauge | Sessions in an unknown/error phase. |

### Per-session traffic — labels: `node_uuid`, `user_id`

| Metric | Type |
|--------|------|
| `fastrg_node_per_pppoe_session_rx_packets_total` | gauge |
| `fastrg_node_per_pppoe_session_rx_bytes_total` | gauge |
| `fastrg_node_per_pppoe_session_tx_packets_total` | gauge |
| `fastrg_node_per_pppoe_session_tx_bytes_total` | gauge |

### Per-subscriber NAT pool health — labels: `node_uuid`, `user_id`

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_per_user_nat_entries_used` | gauge | Live NAT mappings held by this subscriber (pool fill, out of 262144). |
| `fastrg_node_per_user_nat_alloc_fail_total` | gauge | NAT learning failures: ports exhausted, entry pool dry or hash full. A non-zero rate means new flows are being dropped. Resets on subscriber re-init as well as node restart. |
| `fastrg_node_per_user_nat_gc_reclaimed_total` | gauge | Expired NAT mappings reclaimed by the amortized data-lcore GC. Resets on subscriber re-init as well as node restart. |

## 6. DHCP

### Per-subscriber leases — labels: `node_uuid`, `user_id` (emitted only for configured pools)

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_per_user_dhcp_cur_lease_count` | gauge | Currently leased addresses in the subscriber's pool. |
| `fastrg_node_per_user_dhcp_max_lease_count` | gauge | Pool capacity (number of addresses). |

### Server status tallies — labels: `node_uuid`

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_total_running_dhcp_server` | gauge | DHCP servers running (`dhcp_bool == 1`). |
| `fastrg_node_total_stopped_dhcp_server` | gauge | DHCP servers configured but stopped. |
| `fastrg_node_total_not_configured_dhcp_server` | gauge | Subscriber slots with no DHCP server configured. |

## 7. Datapath lcore usage

Labels: `node_uuid`, `lcore_id`, `role`. Cumulative TSC cycle counts; compute the
busy ratio in PromQL.

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_lcore_busy_cycles_total` | gauge | Cycles a lcore spent processing packets/events. |
| `fastrg_node_lcore_total_cycles_total` | gauge | Total cycles a lcore polled (busy + idle). |

```promql
# per-lcore busyness (%)
100 * rate(fastrg_node_lcore_busy_cycles_total[1m])
    / rate(fastrg_node_lcore_total_cycles_total[1m])
```

## 8. DPDK memory

### Heap — labels: `node_uuid`, `socket_id`

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_heap_total_bytes` | gauge | DPDK heap size on hugepages per NUMA socket. |
| `fastrg_node_heap_used_bytes` | gauge | Allocated (in-use) heap bytes. |
| `fastrg_node_heap_free_bytes` | gauge | Free heap bytes. |
| `fastrg_node_heap_largest_free_block_bytes` | gauge | Largest contiguous free block (fragmentation gauge). |

### Mempool — labels: `node_uuid`, `pool`

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_mempool_size` | gauge | Total element capacity. |
| `fastrg_node_mempool_avail_count` | gauge | Free elements. |
| `fastrg_node_mempool_in_use_count` | gauge | In-use elements. |

### Hugepage — labels: `node_uuid`

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_hugepage_pinned_bytes` | gauge | Total hugepage memory locked by DPDK. |

---

## Prometheus scrape config

The node is scraped directly. Use a stable service discovery (file_sd / etcd_sd)
that reflects the **desired** node inventory, so a node going offline shows
`up == 0` rather than the series disappearing.

```yaml
scrape_configs:
  - job_name: fastrg-node
    scrape_interval: 15s
    file_sd_configs:
      - files: ['/etc/prometheus/fastrg_nodes.json']
```

## Grafana / PromQL quick reference

```promql
# NIC link up/down — Stat or State-timeline panel; value-map 1->UP(green) 0->DOWN(red)
fastrg_nic_link_up
# down ports, enriched with NIC model/pci
fastrg_nic_link_up == 0
  * on(node_uuid,port_id) group_left(model,pci,mac) fastrg_nic_info

# node offline / flapping
up{job="fastrg-node"} == 0
changes(up{job="fastrg-node"}[10m]) > 4

# throughput (bps) per NIC port
8 * rate(fastrg_node_rx_bytes_total[1m])

# lcore busyness (%)
100 * rate(fastrg_node_lcore_busy_cycles_total[1m])
    / rate(fastrg_node_lcore_total_cycles_total[1m])

# RX drops (overload signal)
rate(fastrg_node_rx_dropped_total[1m]) > 0
```

## Suggested alerts

```yaml
- alert: FastrgNodeOffline
  expr: up{job="fastrg-node"} == 0
  for: 2m
- alert: FastrgNodeFlapping
  expr: changes(up{job="fastrg-node"}[10m]) > 4
- alert: FastrgNodeCrashloop
  expr: increase(fastrg_node_restart_total[15m]) > 2
- alert: FastrgNicLinkDown
  expr: fastrg_nic_link_up == 0
  for: 30s
- alert: FastrgNicLinkFlapping
  expr: increase(fastrg_nic_link_flaps_total[10m]) > 3
```
