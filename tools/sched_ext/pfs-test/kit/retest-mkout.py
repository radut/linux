#!/usr/bin/env python3
"""mkout.py JSON PROSE_JSON OUT_MD_TABLES OUT_HTML: tables (markdown + dashboard fragment) from summarize.py output.
PROSE_JSON: {"intro": [html paragraphs], "findings": [li], "winners": [[test, winner, note]], "regress": [li],
             "blocked": [li], "method": [li]}"""
import html
import json
import sys

d = json.load(open(sys.argv[1]))
prose = json.load(open(sys.argv[2]))
VARS = ["cfs", "kwidth", "scx", "kpfs"]
LBL = {"cfs": "stock CFS", "kwidth": "kwidth", "scx": "scx_pfs", "kpfs": "kPFS"}
S = {(e["test"], e["cell"], e["metric"]): e for e in d["summary"]}
E = html.escape


def cells(test):
    seen = []
    for e in d["summary"]:
        if e["test"] == test and e["cell"] not in seen:
            seen.append(e["cell"])
    return seen


def fmt(x, f):
    if x is None:
        return "-"
    if f == "k":
        return f"{x / 1000:.0f}k" if x < 1e6 else f"{x / 1e6:.2f}M"
    return f % x


def val(test, cell, met, v, f, rng=False):
    e = S.get((test, cell, met))
    if not e or v not in e["median"]:
        return "-"
    s = fmt(e["median"][v], f)
    if rng and e["n"][v] > 1:
        s += f" ({fmt(e['min'][v], f)}-{fmt(e['max'][v], f)})"
    return s


def win(test, cell, met):
    e = S.get((test, cell, met))
    return LBL.get(e.get("winner"), "-") if e else "-"


def ratio(test, cell, met, v):
    e = S.get((test, cell, met))
    if not e or "vs_cfs" not in e or v not in e["vs_cfs"]:
        return "-"
    return f"{e['vs_cfs'][v]:.2f}"


tables = []  # (id, title, note, header, rows)


def T(tid, title, note, header, rows):
    tables.append((tid, title, note, header, rows))


# 1 limited matrix
rows = []
for c in cells("mx"):
    lim = float(c.split()[1])
    rows.append([c] + [f"{val('mx', c, 'thr', v, '%.2f')} / {val('mx', c, 'cpus', v, '%.2f')} / {val('mx', c, 'stall', v, '%.0f')}"
                       for v in VARS] + [win("mx", c, "thr"), win("mx", c, "stall")])
T("mx", "1. Limited matrix (no neighbour)", "Median of 5 boots per variant. Each cell: throughput M iter/s / CPUs used "
  "(from the sched_switch trace, all threads of the group, over the run) / worst stall ms (longest gap between two loop "
  "iterations of one thread). Winners: highest throughput, lowest worst stall.",
  ["Cell"] + [LBL[v] for v in VARS] + ["Best throughput", "Lowest stall"], rows)

# 2 latency
rows = []
for c in cells("latlim"):
    rows.append([c + " (limited)"] + [f"{val('latlim', c, 'p50', v, '%.2f')} / {val('latlim', c, 'p99', v, '%.2f')}" for v in VARS]
                + [win("latlim", c, "p50"), win("latlim", c, "p99")])
for c in cells("ulat"):
    rows.append([c.replace("reqlat-b", "unlimited, burners ")] + [f"{val('ulat', c, 'p50', v, '%.2f')} / {val('ulat', c, 'p99', v, '%.2f')}" for v in VARS]
                + [win("ulat", c, "p50"), win("ulat", c, "p99")])
T("lat", "2a. Request latency, p50 / p99 ms", "reqlat: 4 request threads, 1 ms of CPU every 20 ms. Limited cells: in a group "
  "limited to 1 or 2 CPUs with 0 or 8 busy threads in the same group. Unlimited: no limit, 0 or 8 busy threads next to it. Median of 5 (limited) or 9 (unlimited) boots.",
  ["Cell"] + [LBL[v] for v in VARS] + ["Best p50", "Best p99"], rows)
rows = []
for c in cells("wlat"):
    rows.append([c] + [f"{val('wlat', c, 'p50', v, '%.0f')} / {val('wlat', c, 'p99', v, '%.0f')}" for v in VARS]
                + [win("wlat", c, "p50"), win("wlat", c, "p99")])
T("wlat", "2b. Wakeup latency, p50 / p99 us", "wlat: periodic threads sleep to an absolute deadline and measure how late "
  "they run. idle: 8 threads alone; b8: 8 threads + 8 busy threads; busy16: 16 threads + 4 busy. No limits. Median of 9 boots.",
  ["Cell"] + [LBL[v] for v in VARS] + ["Best p50", "Best p99"], rows)

# 3 weights + 4 starvation
rows = []
for c, name in (("wt-req", "requests 1:3, no limits"), ("wt-lim", "requests 1:3, limits 4 / 6")):
    rows.append([name] + [f"{val('wt', c, 'a', v, '%.2f')} / {val('wt', c, 'b', v, '%.2f')} = {val('wt', c, 'ratio', v, '%.2f')}"
                          for v in VARS] + [win("wt", c, "ratio")])
rows.append(["starvation: weight-1 thread, worst stall ms / CPU ms in 20 s"] +
            [f"{val('starve', 'besteffort', 'stall', v, '%.0f')} / {val('starve', 'besteffort', 'cpu_ms', v, '%.0f')}" for v in VARS]
            + [win("starve", "besteffort", "stall")])
T("wt", "3-4. Weights and starvation", "Two pods of 16 busy threads each, cpu.weight from requests 1 and 3 CPUs as the "
  "kubelet sets it; 8 CPUs. Cells: pod A CPUs / pod B CPUs = B/A. By weight the split is 2 / 6 = 3.0 in both cases "
  "(B's limit 6 is not binding at 6). Starvation: one thread of weight 1 next to two pods of 8 busy threads with weight 5000. Median of 5 boots.",
  ["Case"] + [LBL[v] for v in VARS] + ["Closest to 3.0 / lowest stall"], rows)

# 5 noisy neighbour
rows = []
for c in cells("nn"):
    rows.append([c] + [f"{val('nn', c, 'thr', v, '%.2f')} / {val('nn', c, 'cpus', v, '%.2f')} / {val('nn', c, 'stall', v, '%.0f')}"
                       for v in VARS] + [win("nn", c, "thr"), win("nn", c, "stall")])
T("nn", "5. Noisy neighbour: limit 2 next to 8 unlimited busy threads", "Throughput M iter/s / CPUs used by the limited "
  "group (trace) / worst stall ms. Median of 5 boots.",
  ["Cell"] + [LBL[v] for v in VARS] + ["Best throughput", "Lowest stall"], rows)

# 6 lock probes
rows = []
for c in cells("lk"):
    if c.startswith("gc"):
        rows.append([c + ": pause p50 / p99 ms, CPUs"] + [f"{val('lk', c, 'gc_p50', v, '%.1f')} / {val('lk', c, 'gc_p99', v, '%.1f')}, {val('lk', c, 'cpus', v, '%.2f')}"
                                                          for v in VARS] + [win("lk", c, "gc_p99")])
    else:
        rows.append([c + ": ops/s, CPUs"] + [f"{val('lk', c, 'ops', v, 'k')}, {val('lk', c, 'cpus', v, '%.2f')}" for v in VARS]
                    + [win("lk", c, "ops")])
T("lk", "6. Lock probes in a limited pod", "ly16-4-Y: 16 threads, 4 mutexes, Y sched_yield() calls with the mutex held per "
  "event (sysbench threads). flag16: 8 pairs of threads passing a flag, busy waiting. gc: 8 spinning mutators with a "
  "safepoint, GC workers on a futex, spin+yield termination (JVM ParallelGC). CPUs from cpu.stat. Median of 5 boots.",
  ["Probe"] + [LBL[v] for v in VARS] + ["Best"], rows)

# 7 unlimited
rows = []
for c in cells("unl"):
    for met, f, unit in (("time", "%.3f", "s"), ("ops", "k", "ops/s"), ("wake_p99", "%.0f", "wake p99 us"),
                         ("req_p99", "%.0f", "req p99 us"), ("rps", "%.0f", "rps")):
        if (("unl", c, met)) in S:
            rows.append([f"{c} {unit}"] + [f"{val('unl', c, met, v, f)} ({ratio('unl', c, met, v)})" for v in VARS] + [win("unl", c, met)])
T("unl", "7. Tasks without limits", "No cpu.max anywhere. hackbench = perf bench sched messaging -l 400 (time, lower is "
  "better); pipe = perf bench sched pipe -l 100000; barrier6 = spin-then-futex barrier, 6 threads; schbench -m 2 -t 4 (8 "
  "workers on 8 CPUs) and -t 8 (16 workers). Median of 9 boots; (ratio to stock CFS).",
  ["Workload"] + [LBL[v] for v in VARS] + ["Best"], rows)

# 8 kit paths
for path, title in (("kit-cg", "8a. Kit, cgroup path (cg-run.sh via compare.sh)"),
                    ("kit-dock", "8b. Kit, Docker path (dockerd, cgroupfs driver)"),
                    ("kit-systemd", "8c. Kit, systemd path (systemd-run --scope, full systemd VM)"),
                    ("kit-docker-systemd", "8d. Kit, Docker path with the systemd cgroup driver (full systemd VM)")):
    rows = []
    for c in cells(path):
        if c.startswith("reqlat"):
            rows.append([c + " p50 / p99 ms"] + [f"{val(path, c, 'p50', v, '%.2f')} / {val(path, c, 'p99', v, '%.2f')}" for v in VARS] + [win(path, c, "p99")])
        else:
            rows.append([c] + [f"{val(path, c, 'thr', v, '%.2f')} / {val(path, c, 'conc', v, '%.2f')} / {val(path, c, 'stall', v, '%.0f')} / {val(path, c, 'throttled', v, '%.0f')}"
                               for v in VARS] + [win(path, c, "thr")])
    if rows:
        T(path, title, "In-tree kit at the final scx_pfs commit, key cells. Cells: M iter/s / average concurrency while "
          "running (trace) / worst stall ms / nr_throttled. kPFS runs the kit's cfs variant on a sched_pfs=1 boot (the kit "
          "has no kpfs variant). Median of 5 boots.", ["Cell"] + [LBL[v] for v in VARS] + ["Best throughput / p99"], rows)

# markdown
md = []
for tid, title, note, header, rws in tables:
    md.append(f"### {title}\n\n{note}\n\n| " + " | ".join(header) + " |\n|" + "---|" * len(header))
    for r in rws:
        md.append("| " + " | ".join(r) + " |")
    md.append("")
open(sys.argv[3], "w").write("\n".join(md))

# html fragment
p = ['<section class="sx-page" id="tab-retest-local" role="tabpanel" aria-labelledby="tabbtn-retest-local" hidden>',
     '  <div class="sx-prose">', '    <h2>Four-way retest on a local VM</h2>']
p += ["    <p>" + x + "</p>" for x in prose["intro"]]
for key, h in (("winners", None),):
    pass
p.append("    <h3>Winner per test</h3>")
p.append('    <div class="sx-tablewrap"><table><thead><tr><th class="l">Test</th><th class="l">Winner</th><th class="l">Notes</th></tr></thead><tbody>')
for t, w, n in prose["winners"]:
    p.append(f'      <tr><td class="l">{t}</td><td class="l">{w}</td><td class="l">{n}</td></tr>')
p.append("    </tbody></table></div>")
for key, h in (("findings", "Findings"), ("regress", "Regressions against stock CFS"), ("blocked", "Not run or blocked"),
               ("method", "Method")):
    if prose.get(key):
        p.append(f"    <h3>{h}</h3>")
        p.append("    <ul>")
        p += [f"      <li>{x}</li>" for x in prose[key]]
        p.append("    </ul>")
p.append("  </div>")
for tid, title, note, header, rws in tables:
    p.append(f'  <div class="sx-card" id="retest-local-{tid}">')
    p.append(f"    <h2>{E(title)}</h2>")
    p.append(f'    <p class="note">{E(note)}</p>')
    p.append('    <div class="sx-tablewrap"><table>')
    p.append("      <thead><tr>" + "".join(f'<th class="l">{E(h)}</th>' if i == 0 else f"<th>{E(h)}</th>" for i, h in enumerate(header)) + "</tr></thead>")
    p.append("      <tbody>")
    for r in rws:
        p.append("        <tr>" + "".join(f'<td class="l">{E(x)}</td>' if i == 0 else f"<td>{E(x)}</td>" for i, x in enumerate(r)) + "</tr>")
    p.append("      </tbody>")
    p.append("    </table></div>")
    p.append("  </div>")
p.append("</section>")
open(sys.argv[4], "w").write("\n".join(p) + "\n")
print(f"{len(tables)} tables")
