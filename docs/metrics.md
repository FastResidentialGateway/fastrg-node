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
| `fastrg_node_snapshot_persist_ok` | gauge | `node_uuid` | `1` when the last config snapshot persist to disk succeeded (also `1` at boot before any persist happened), `0` while the last persist attempt failed — surfaces disk-full snapshot failures on the dashboard. |
| `fastrg_node_max_user_count` | gauge | `node_uuid` | Maximum subscriber capacity computed at startup from free hugepage memory after a 512 MiB reserve, using the measured per-subscriber cost. |
| `fastrg_node_subscriber_cost_bytes` | gauge | `node_uuid` | Per subscriber hugepage usage, measured at startup by building one subscriber and adding its share of the memory pools; free hugepage memory divided by this gives max_user_count. |

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

### TX queue shortfalls

Labels: `node_uuid`, `nic_index`, `queue`.

A shortfall is `rte_eth_tx_burst()` taking fewer packets than it was offered;
the dropped packets are already counted in the per-subscriber dropped packet counters.
Per-queue stats are needed so that when a single lcore gets stuck, it can be located.

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_tx_queue_full_total` | gauge | Packet count refused by the NIC queue. |
| `fastrg_node_tx_queue_burst_short_total` | gauge | Count of `rte_eth_tx_burst()` calls that did not send all packets. |
| `fastrg_node_tx_handoff_dropped_total` | gauge | Packet count dropped during handoff between lcores. |

A handoff can drop packets when the ring is full; this keeps a slow consumer
from stalling the producer.

When `rte_eth_tx_burst()` leaves packets unsent, only the first occurrence on
each queue writes an INFO log line; this avoids log flooding.

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

### Per-subscriber IPv6 firewall pool health — labels: `node_uuid`, `user_id`

IPv6 is routed, not NAT translated, so a stateful session table supplies the
protection NAT gives IPv4 for free: an inbound packet is denied unless it
answers a session a LAN host opened. Each subscriber owns one, holding up to
65536 sessions; the five `_total` rows reset on subscriber re-init as well as
node restart.

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_per_user_ipv6_firewall_entries_used` | gauge | Live IPv6 firewall sessions held by this subscriber. |
| `fastrg_node_per_user_ipv6_firewall_alloc_fail_total` | gauge | Sessions that could not be opened: pool dry or hash full. The packet is forwarded anyway, so only its reply is denied. |
| `fastrg_node_per_user_ipv6_firewall_gc_reclaimed_total` | gauge | Expired sessions removed by the periodic cleanup. |
| `fastrg_node_per_user_ipv6_firewall_evicted_total` | gauge | Live sessions evicted, least recently used first, to make room for a new one. |
| `fastrg_node_per_user_ipv6_firewall_icmp6_err_passed_total` | gauge | ICMPv6 error notifications (e.g. "packet too big") from the WAN, original packet is in ipv6 firewall session table. |
| `fastrg_node_per_user_ipv6_firewall_icmp6_err_dropped_total` | gauge | ICMPv6 error notifications from the WAN, original packet is not in ipv6 firewall session table or is malformed. |

A non-zero eviction rate means the subscriber holds more concurrent sessions
than the pool fits. Every packet the firewall denies is counted in
`fastrg_node_per_user_dropped_packets_total` for the WAN port.

## 6. DHCP

### Per-subscriber leases — labels: `node_uuid`, `user_id` (emitted only for configured pools)

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_per_user_dhcp_cur_lease_count` | gauge | Currently leased addresses in the subscriber's pool. |
| `fastrg_node_per_user_dhcp_max_lease_count` | gauge | Leasable pool capacity (addresses, excluding .0/.255). |

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

### Per-lcore traffic (RSS queue distribution)

Labels: `node_uuid`, `lcore_id`, `role`, `nic_index`. Each row is the lcore's
`per_subscriber_stats` row summed over the subscriber axis (unknown-user slot
included) — the same data source as the per-subscriber traffic metrics,
aggregated along the lcore axis instead of the user axis. Data lcores map 1:1
to RSS queues, so these rows expose the per-queue traffic distribution: with
RSS healthy, `role="wan_data"` / `role="lan_data"` rows share the session
traffic; if RSS silently degrades, one queue absorbs it all.

| Metric | Type | Description |
|--------|------|-------------|
| `fastrg_node_lcore_rx_packets_total` | gauge | Packets received on `nic_index` and processed by this lcore. |
| `fastrg_node_lcore_tx_packets_total` | gauge | Packets transmitted on `nic_index` by this lcore. |

For a given `nic_index`, summing these rows over all lcores equals the
per-subscriber total plus the unknown-user row for the same port.

```promql
# per-queue share of WAN session RX (RSS balance check)
fastrg_node_lcore_rx_packets_total{role="wan_data",nic_index="1"}
  / on(node_uuid) group_left
    sum(fastrg_node_lcore_rx_packets_total{role="wan_data",nic_index="1"})
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
