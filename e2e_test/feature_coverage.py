#!/usr/bin/env python3
"""Per-feature coverage view over an lcov tracefile.

Reads one lcov .info tracefile (FN/FNDA/DA records; lcov >= 2.0 FN records
with start,end line numbers are required for line attribution) and groups
functions into product features by name/path rules, printing per-feature
line and function coverage.

Honesty mechanics:
  - A function may match SEVERAL features (e.g. nat_gc_idle_tick_by_ccb is
    both NAT and dp-core by residence); it is counted in each, so the
    per-feature rows must NOT be summed into a total.
  - Functions matched by no rule land in the "unclassified" bucket and are
    listed explicitly, so gaps in the rule table stay visible instead of
    silently vanishing.

Usage: feature_coverage.py <tracefile.info>
"""

import re
import sys
from collections import defaultdict

# ---------------------------------------------------------------------------
# Classification rules.
#
# Each rule: (feature, path_regex, name_regex). A function belongs to the
# feature if its source path matches path_regex OR its name matches
# name_regex (either may be None). All matching rules apply (multi-count).
# C++ symbols in northbound/ are mangled, so those areas rely on the path
# side of the rules only.
# ---------------------------------------------------------------------------
RULES = [
    ("SNAT/NAT + conntrack",
     r"src/pppd/(nat\.h|tcp_conntrack\.(c|h))$",
     r"^(nat_|port_fwd_|tcp_|compute_initial_nat_port)"),
    ("PPPoE control plane",
     r"src/pppd/(codec|fsm|pppd)\.(c|h)$",
     r"^(PPP_|ppp_|pppd_|pppoe_|exit_ppp$)"),
    ("PPPoE data plane (encap/decap)",
     r"src/dp_codec\.h$",
     r"^(encaps_|decaps_|insert_pppoes_hdr$)"),
    ("dp core (rx/tx loops, distributor, flow rules)",
     r"src/(dp\.(c|h)|dp_flow\.c)$",
     r"^(wan_(ctrl|data|dist)_|lan_(ctrl|data|dist)_|drop_packet$|count_(rx|tx)_packet$)"),
    ("DHCP server",
     r"src/dhcpd/",
     r"^(dhcp_|dhcpd_)"),
    ("DNS proxy",
     r"src/dnsd/",
     r"^(dns_|dnsd_)"),
    ("MAC/ARP resolution",
     r"src/mac_table\.c$",
     r"^(mac_table_|arp_|encode_arp_|send_arp_)"),
    ("Metrics/observability",
     r"src/(metrics|lighthttp|pdump_capture|trace)\.(c|h)$",
     r"^(metrics_|lighthttp_|pdump_|trace_|fastrg_pdump_)"),
    ("Controller sync (SDN)",
     r"(northbound/(controller|grpc)/|src/(etcd_integration|controller)\.c$)",
     r"^(etcd_|kafka_|controller_|config_snapshot_)"),
    ("Config/lifecycle",
     r"src/(fastrg|northbound|config|init|main|utils|dbg)\.(c|h)$",
     r"^(config_|sys_|init_|fastrg_(?!rcu_dp_|pdump_))"),
    ("CLI",
     r"northbound/cmdline/",
     None),
]

UNCLASSIFIED = "unclassified"


def parse_tracefile(path):
    """Return {source_file: {"functions": {name: (start, end, hits)},
                             "lines": {lineno: hits}}}."""
    files = {}
    cur = None
    with open(path, encoding="utf-8") as fp:
        for raw in fp:
            line = raw.strip()
            if line.startswith("SF:"):
                cur = files.setdefault(line[3:], {"functions": {}, "lines": {}})
            elif cur is None:
                continue
            elif line.startswith("FN:"):
                parts = line[3:].split(",")
                if len(parts) < 3:
                    sys.exit(f"error: FN record without an end line ({line!r}); "
                             "an lcov >= 2.0 tracefile is required")
                start, end = int(parts[0]), int(parts[1])
                name = ",".join(parts[2:])
                cur["functions"][name] = [start, end, 0]
            elif line.startswith("FNDA:"):
                hits, _, name = line[5:].partition(",")
                if name in cur["functions"]:
                    cur["functions"][name][2] += int(hits)
            elif line.startswith("DA:"):
                parts = line[3:].split(",")
                lineno, hits = int(parts[0]), int(parts[1])
                cur["lines"][lineno] = cur["lines"].get(lineno, 0) + hits
            elif line == "end_of_record":
                cur = None
    return files


def classify(rel_path, name):
    feats = []
    for feat, path_re, name_re in RULES:
        if (path_re and re.search(path_re, rel_path)) or \
           (name_re and re.match(name_re, name)):
            feats.append(feat)
    return feats or [UNCLASSIFIED]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    files = parse_tracefile(sys.argv[1])

    # Per-feature accumulators.
    fn_total = defaultdict(int)
    fn_hit = defaultdict(int)
    ln_total = defaultdict(int)
    ln_hit = defaultdict(int)
    unclassified = []

    total_functions = 0
    for path, data in sorted(files.items()):
        m = re.search(r"/((src|northbound)/.*)$", path)
        rel = m.group(1) if m else path
        lines = data["lines"]
        for name, (start, end, hits) in sorted(data["functions"].items()):
            total_functions += 1
            feats = classify(rel, name)
            if feats == [UNCLASSIFIED]:
                unclassified.append((rel, name))
            # DA lines inside the function's [start, end] range.
            body = {ln: h for ln, h in lines.items() if start <= ln <= end}
            for feat in feats:
                fn_total[feat] += 1
                fn_hit[feat] += 1 if hits > 0 else 0
                ln_total[feat] += len(body)
                ln_hit[feat] += sum(1 for h in body.values() if h > 0)

    order = [feat for feat, _, _ in RULES] + [UNCLASSIFIED]
    print(f"{'Feature':<48} {'Lines':>18} {'Functions':>18}")
    for feat in order:
        if fn_total[feat] == 0:
            continue
        lr = 100.0 * ln_hit[feat] / ln_total[feat] if ln_total[feat] else 0.0
        fr = 100.0 * fn_hit[feat] / fn_total[feat] if fn_total[feat] else 0.0
        print(f"{feat:<48} {lr:5.1f}% ({ln_hit[feat]}/{ln_total[feat]})"
              f"{'':>2} {fr:5.1f}% ({fn_hit[feat]}/{fn_total[feat]})")

    n_uncl = len(unclassified)
    share = 100.0 * n_uncl / total_functions if total_functions else 0.0
    print()
    print(f"Functions total: {total_functions}; "
          f"unclassified: {n_uncl} ({share:.1f}%)")
    if unclassified:
        print("Unclassified functions:")
        for rel, name in unclassified:
            print(f"  {rel}: {name}")
    print()
    print("Note: shared functions are counted in every matching feature, so "
          "per-feature rows must not be summed. Line% covers only lines that "
          "fall inside a function body known to the tracefile.")


if __name__ == "__main__":
    main()
