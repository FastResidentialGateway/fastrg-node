# Packet Capture

FastRG offers two ways to capture subscriber traffic for troubleshooting:

1. **CLI capture (`exec pdump`)** — built into `fastrg-cli`, per-subscriber and
   per-direction, with an optional tcpdump-style filter. Best for "I want this
   subscriber's packets, now".
2. **Host `dpdk-dumpcap`** — DPDK's standard capture tool run on the host as a
   secondary process. Best for ad-hoc, rich BPF expressions or full
   port-level captures.

Both write standard pcap files that open in Wireshark/tcpdump.

---

## 1. CLI capture — `exec pdump`

```
exec pdump <start|stop> <WAN|LAN|ALL> subscriber <id|all> [filter "<bpf expr>"] [size <MB>]
```

- `WAN` / `LAN` / `ALL` — which physical port(s) to capture. `WAN` is the
  PPPoE/uplink side, `LAN` the subscriber side, `ALL` both.
- `subscriber <id>` — a single 1-based subscriber, or `all` for every subscriber.
- `filter "<bpf expr>"` — optional. A tcpdump-style expression; only matching
  packets are written. Stop commands ignore the filter.
- `size <MB>` — optional. Maximum pcap file size in megabytes. The default and
  the hard cap is **2048 MB (2 GB)** — at line rate a capture fills fast, so the
  file never grows past this. When the limit is reached the capture stops
  writing; run `stop` to release resources. The limit is fixed when the capture
  file is first opened (a later `start` that joins the same file keeps it).

Captures are **independent**: starting `ALL subscriber all` and then stopping
`LAN subscriber 2` keeps every other capture running and only drops LAN traffic
for subscriber 2. All matching packets (every direction/subscriber) are written
to a **single merged pcap** under the node's log directory
(`<LogPath dir>/pdump-<timestamp>.pcap`); the path is printed when capture starts.

### Examples

```
exec pdump start ALL subscriber all
exec pdump start WAN subscriber 2 filter "tcp and port 80"
exec pdump stop  LAN subscriber 2
exec pdump stop  ALL subscriber all
```

### Filter caveat (important)

The filter runs against the frame **as it appears on the wire at the capture
point**, before/after FastRG's own decap/encap:

- **LAN** frames are 802.1Q VLAN-tagged.
- **WAN** frames are PPPoE-encapsulated (also VLAN-tagged on the outside).

So a bare `tcp port 80` will not see through the VLAN/PPPoE headers. Write
encapsulation-aware expressions, e.g. `vlan and tcp port 80`, exactly as you
would when capturing on a trunk port with tcpdump.

### Cost when not capturing

Capture uses DPDK RX/TX callbacks that are **only registered while a capture is
active**. With no capture running, the data plane registers no callbacks and
pays zero cost — the forwarding fast path is unchanged.

### Capture file size

The pcap is written under the node's log directory and grows for as long as the
capture runs. At line rate this is fast — capturing a saturating flow (e.g. an
iperf test) for a couple of minutes can produce **several GB**. Narrow the
capture with a `filter`, target a single subscriber/direction, and `stop`
promptly; make sure the log filesystem has room.

---

## 2. Host `dpdk-dumpcap`

`dpdk-dumpcap` is built by `boot.sh` (DPDK is configured with
`-Denable_apps="pdump,dumpcap,..."`) and installed at
`/usr/local/bin/dpdk-dumpcap`. FastRG already calls `rte_pdump_init()` at
startup, so the primary side is ready — no FastRG changes are needed.

`dpdk-dumpcap` attaches as a DPDK **secondary process**. The primary (FastRG)
and the secondary must share:

- the same hugepage mount,
- the same DPDK runtime directory (`/var/run/dpdk/<prefix>/`),
- the same `--file-prefix`,
- the same DPDK major version (this tree uses **v24.11.6**).

### FastRG on the host

Run FastRG with a fixed file-prefix so the secondary can find it
(EAL args precede the `--` that separates FastRG's own args):

```bash
# host shell A — FastRG (primary)
fastrg --file-prefix fastrg ... -- <fastrg app args>

# host shell B — capture (secondary)
dpdk-dumpcap --file-prefix fastrg -- -i <port> -w /tmp/wan.pcap -f "vlan and tcp port 80"
```

`dpdk-dumpcap` supports the full tcpdump `-f` filter syntax (the same
VLAN/PPPoE caveat above applies to what's on the wire).

### FastRG inside Docker, `dpdk-dumpcap` on the host

`dpdk-dumpcap` does **not** need to run inside the container — run it on the
host and attach to the containerized FastRG. The container must expose its DPDK
environment to the host:

```bash
docker run --privileged \
    -v /dev/hugepages:/dev/hugepages \
    -v /var/run/dpdk:/var/run/dpdk \
    ... fastrg:latest fastrg --file-prefix fastrg -- <fastrg app args>
```

Then on the host:

```bash
dpdk-dumpcap --file-prefix fastrg -- -i <port> -w /tmp/wan.pcap
```

Notes:
- The host's DPDK build must match the container's DPDK major version (v24.11.6).
- `--ipc=host` can be used instead of bind-mounting `/var/run/dpdk`, as long as
  the hugepage mount is shared.
- The runtime image does **not** ship `dpdk-dumpcap`; it is intentionally run
  from the host.
