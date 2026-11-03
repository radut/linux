#!/usr/bin/env python3
"""gen_frag_rm.py JSON PROSE_HTML OUT_HTML: dashboard section tab-remeasure from remeasure.json.

Same conventions as the retest-cluster section: direction legend, a "Setup:" line per table, direction markers per
metric, best limited value per metric highlighted (<b class="sxb">), node A not ranked. PROSE_HTML holds the
hand-written headline / regression text (sanitized).
"""
import os
import html
import json
import re
import sys

J, PROSE, OUT = sys.argv[1:4]
d = json.load(open(J))
rows = d["rows"]
COLS = ["CFS-7.3", "CFS-10ms", "CFS-burst", "cpuset-pin", "kwidth", "scx_pfs", "kPFS", "kPFS-r1", "scx_pfs-r1",
        "kPFS-kpfs4", "scx_pfs-old", "ref"]
HEAD = ["CFS", "CFS 10 ms period", "CFS + burst", "cpuset pinning", "kwidth", "scx_pfs", "kPFS",
        "kPFS round 1 (kpfs5)", "scx_pfs round 1", "kPFS before fixes (kpfs4)", "scx_pfs before fixes (7.0)",
        "node A (4 vCPU), no limit, CFS"]
RANKED = set(COLS[:7])
HI = {"tps", "set_rps", "get_rps", "rps", "events_s", "rounds_s", "ops_s", "mut_miters_s"}
TARGET = {"cg_cpus", "sw_cpus", "cpus_used", "cpus"}


def f(x):
    if x is None:
        return "-"
    a = abs(x)
    if a >= 10000:
        return f"{x / 1000:.1f}k"
    return f"{x:.0f}" if a >= 100 else f"{x:.1f}" if a >= 10 else f"{x:.2f}"


def dir_of(c):
    return "target: limit" if c in TARGET else "↑" if c in HI else "↓"


def get(suite, case, limit, variant):
    for r in rows:
        if r.get("suite") == suite and r.get("case") == case and r.get("variant") == variant and \
                r.get("limit") == (limit if variant != "ref" else "none"):
            return r
    return None


def td(x, l=False):
    return f'<td class="l">{x}</td>' if l else f"<td>{x}</td>"


def table(title, setup, first, body):
    th = "".join(f'<th class="l">{h}</th>' for h in first) + "".join(f"<th>{html.escape(h)}</th>" for h in HEAD)
    return (f"  <h3>{html.escape(title)}</h3>\n  <p class=\"sx-dirnote\"><b>Setup:</b> {setup}</p>\n"
            f'    <div class="sx-tablewrap"><table data-sx-dir="1">\n    <thead><tr>{th}</tr></thead>\n    <tbody>\n'
            + "\n".join("      " + b for b in body) + "\n    </tbody>\n  </table></div>\n")


def row_cells(rs, cols, lim):
    """rs: variant -> row or None. Highlight the best limited value per directional metric."""
    best = {}
    for c in cols:
        if c in TARGET:
            continue
        cand = {v: r.get(c) for v, r in rs.items() if r and v in RANKED and r.get(c) is not None}
        if len(cand) >= 2:
            best[c] = (max if c in HI else min)(cand, key=cand.get)
    out = []
    for v in COLS:
        r = rs.get(v)
        if not r:
            out.append(td("-"))
            continue
        parts = []
        for c in cols:
            s = html.escape(f(r.get(c)))
            parts.append(f'<b class="sxb">{s}</b>' if best.get(c) == v else s)
        out.append(td(" · ".join(parts)))
    return out


def metric_label(labels, cols):
    return f'{html.escape(" · ".join(labels))} <span class="sx-dir">({" · ".join(dir_of(c) for c in cols)})</span>'


KEY = [
    ("db", "Postgres TPC-B, 32 clients", ["4", "2", "1.5"], ["tps", "p99_ms", "cg_cpus", "sw_cpus"],
     ["tps", "p99 ms", "CPUs cpu.stat", "CPUs sched_switch"], "Postgres TPC-B c32"),
    ("db", "Postgres select-only, 32 clients", ["4", "2", "1.5"], ["tps", "p99_ms", "cg_cpus", "sw_cpus"],
     ["tps", "p99 ms", "CPUs cpu.stat", "CPUs sched_switch"], "Postgres select-only c32"),
    ("db", "Postgres TPC-B, 16 clients", ["4", "2", "1.5"], ["tps", "p99_ms"], ["tps", "p99 ms"], "Postgres TPC-B c16"),
    ("db", "Postgres select-only, 16 clients", ["4", "2", "1.5"], ["tps", "p99_ms"], ["tps", "p99 ms"],
     "Postgres select-only c16"),
    ("db-coloc", "Postgres TPC-B c32, limit 2, beside an unlimited 16-thread CPU hog", ["2"],
     ["tps", "p99_ms", "cg_cpus"], ["tps", "p99 ms", "CPUs cpu.stat"], "Postgres TPC-B beside a CPU hog"),
    ("db-coloc", "Postgres select-only c32, limit 2, beside an unlimited 16-thread CPU hog", ["2"],
     ["tps", "p99_ms", "cg_cpus"], ["tps", "p99 ms", "CPUs cpu.stat"], "Postgres select-only beside a CPU hog"),
    ("db", "MariaDB read-write, 32 threads", ["4", "2"], ["tps", "p99_ms", "cg_cpus", "sw_cpus"],
     ["tps", "p99 ms", "CPUs cpu.stat", "CPUs sched_switch"], "MariaDB read-write t32"),
    ("db", "MariaDB read-only, 32 threads", ["4", "2"], ["tps", "p99_ms", "cg_cpus", "sw_cpus"],
     ["tps", "p99 ms", "CPUs cpu.stat", "CPUs sched_switch"], "MariaDB read-only t32"),
    ("db", "Redis, io-threads 4", ["4", "2"], ["set_rps", "set_p99_ms"], ["SET ops/s", "p99 ms"], "Redis io-threads 4"),
    ("db", "Redis, io-threads 1", ["4", "2"], ["set_rps", "set_p99_ms"], ["SET ops/s", "p99 ms"], "Redis io-threads 1"),
    ("db", "nginx gzip, fixed rate", ["4", "2"], ["p99_ms", "p99.9_ms"], ["p99", "p99.9 ms"], "nginx gzip, fixed rate"),
    ("db", "JVM ParallelGC, fixed rate", ["4", "2"], ["gc_full_avg_ms", "gc_max_ms", "p99.9_ms"],
     ["full GC avg", "worst GC", "req p99.9 ms"], "JVM ParallelGC, fixed rate"),
]
WEB_LIMITS = ["0.25", "0.5", "1", "2", "4"]
WEB_THREADS = ["1", "8", "64", "256"]
PROBES = [("ly16-4-100", ["events_s", "cpus"], ["events/s", "CPUs"], "lock+yield 16 thr / 4 locks"),
          ("ly8-8-100", ["events_s"], ["events/s"], "lock+yield 8 thr / 8 locks"),
          ("ly16-16-100", ["events_s"], ["events/s"], "lock+yield 16 thr / 16 locks"),
          ("sw4-l2", ["rounds_s"], ["rounds/s"], "4 spinning threads"),
          ("gc-l4", ["pause_p99_ms", "cpus"], ["pause p99 ms", "CPUs"], "GC model"),
          ("gc-l2", ["pause_p99_ms"], ["pause p99 ms"], "GC model"),
          ("db32-l4", ["ops_s", "op_p99_ms", "cpus"], ["ops/s", "p99 ms", "CPUs"], "32 DB-like backends"),
          ("db64-l4", ["ops_s", "op_p99_ms", "cpus"], ["ops/s", "p99 ms", "CPUs"], "64 DB-like backends")]
OVERRIDE = json.loads(sys.argv[4]) if len(sys.argv) > 4 else {}


def mach(lim):
    return f"node B (16 vCPU), limit {lim}"


parts = ['<section class="sx-page" id="tab-remeasure" role="tabpanel" aria-labelledby="tabbtn-remeasure" hidden>',
         '  <p class="sx-dirlegend"><b>Reading the numbers:</b> ↑ higher is better · ↓ lower is better · "target: limit": '
         'closest to the limit is better (above it breaks the limit, below it is unused CPU) · <b class="sxb">highlighted</b> '
         '= best value of its row among the limited variants.</p>',
         '<div class="sx-card" id="remeasure">', open(PROSE).read()]
setup_db = ("every limited variant runs on node B (16 vCPU) with the pod limited to N CPUs (\"node B (16 vCPU), limit N\"); "
            "the last column is node A (4 vCPU), no limit, CFS, a separate machine, not ranked. CFS 7.0 and scx_pfs run "
            "kernel 7.0; CFS 7.3, kwidth and kPFS run kernel 7.3 (CFS 7.3 = the kPFS kernel booted without the kPFS "
            "switch). kPFS (kpfs4) is the new build with the CPU overrun fix; kPFS (kpfs3) is the earlier retest. "
            "CPUs: cpu.stat = cgroup usage over the run; sched_switch = on-CPU time of the pod's threads from a "
            "sched_switch trace (new runs only). Medians of up to 3 runs; \"-\" = not run.")
body = []
for suite, case, lims, cols, labels, name in KEY:
    for lim in lims:
        rs = {v: get(suite, case, lim, v) for v in COLS}
        if lim != "4":
            rs["ref"] = None
        if not any(rs.values()):
            continue
        cells = row_cells(rs, cols, lim)
        for i, v in enumerate(COLS):
            k = f"{case}|{lim}|{v}"
            if k in OVERRIDE:
                cells[i] = td(html.escape(OVERRIDE[k]))
        body.append("<tr>" + td(html.escape(name), True) + td(mach(lim), True) + td(metric_label(labels, cols), True)
                    + "".join(cells) + "</tr>")
parts.append(table("Databases and services", setup_db, ["Workload", "Machine, limit", "Metric"], body))

# CPU limit check (kPFS kpfs4 and the other new runs)
lc = [r for r in rows if r.get("suite") == "limit-check"]
if lc:
    b = []
    for r in lc:
        if r["variant"] not in RANKED:
            continue
        L = float(r["limit"])
        st, m5 = r.get("sw_steady_med"), r.get("sw_max5s_max")
        ok = "yes" if r["over"] == 0 else f'no ({r["over"]} of {r["runs"]} runs)'
        b.append("<tr>" + td(html.escape(r["case"]), True) + td(html.escape(r["variant"]), True) + td(mach(r["limit"]), True) + td(str(r["runs"]))
                 + td(f(r.get("cg_cpus_med"))) + td(f(st)) + td(f(m5)) + td(ok) + "</tr>")
    th = "".join(f'<th class="l">{h}</th>' for h in ["Suite", "Variant", "Machine, limit"]) + "".join(
        f"<th>{h}</th>" for h in ["runs", "CPUs cpu.stat (median)", "CPUs sched_switch, steady (median)",
                                  "highest 5 s window (sched_switch)", "at or below the limit"])
    parts.append("  <h3>CPUs used against the limit (fixed builds)</h3>\n  <p class=\"sx-dirnote\"><b>Setup:</b> "
                 "database, service and web runs on node B (16 vCPU); runs of 20 s or longer; steady = median of the 1 s rates from the "
                 "sched_switch trace without the first and last 3 s; a run counts as over when its cpu.stat, trace average "
                 "or steady rate is above the limit by more than 2%. The highest 5 s window is an upper bound only (the "
                 "trace books on-CPU time at switch-out, so long uninterrupted runs land in one window). Target: at or below the limit.</p>\n"
                 f'    <div class="sx-tablewrap"><table>\n    <thead><tr>{th}</tr></thead>\n    <tbody>\n'
                 + "\n".join("      " + x for x in b) + "\n    </tbody>\n  </table></div>\n")

# web matrix
for nb in ("none", "heavy"):
    body = []
    for lim in WEB_LIMITS:
        for th in WEB_THREADS:
            for mode, cols, labels in (("closed", ["rps"], ["max RPS"]), ("open", ["p99"], ["p99 ms at fixed rate"])):
                case = f"limit {lim}, {th} threads ({mode}, {nb})"
                rs = {v: get("web", case, lim, v) for v in COLS if v != "ref"}
                rs["ref"] = get("web", f"limit 4, {th} threads ({mode}, none)", "4", "ref") \
                    if (lim == "4" and nb == "none") else None
                if not any(rs.values()):
                    continue
                body.append("<tr>" + td(f"{th} threads", True) + td(mach(lim), True)
                            + td(metric_label(labels, cols), True) + "".join(row_cells(rs, cols, lim)) + "</tr>")
    nbtxt = "no neighbours" if nb == "none" else ("heavy neighbours (2 unlimited pods with 16 spinning threads each "
                                                 "+ 20 limited pods with 4 spinning threads each)")
    parts.append(table(f"Web RPS, {nbtxt}",
                       f"web server pod on node B (16 vCPU) limited to N CPUs with T server threads, 500 us of CPU per "
                       f"request; client on node A, 256 connections, 60 s; {nbtxt}. Max RPS = closed loop; p99 at a "
                       "fixed rate of 0.6 x the earlier max RPS of the cell. Node A column = the server on node A "
                       "(4 vCPU, no limit) at the limit-4 rows. 3 runs for the 8 earlier cells, 1 for the 12 new ones.",
                       ["Server threads", "Machine, limit", "Metric"], body))

# probes
body = []
for t, cols, labels, name in PROBES:
    rs = {}
    lim = None
    for v in COLS:
        r = None
        for x in rows:
            if x.get("suite") == "probe" and x.get("case") == f"probe {t}" and x.get("variant") == v:
                r = x
        rs[v] = r
        if r:
            lim = r.get("limit")
    if not any(rs.values()):
        continue
    body.append("<tr>" + td(html.escape(name), True) + td(mach(lim), True) + td(metric_label(labels, cols), True)
                + "".join(row_cells(rs, cols, lim)) + "</tr>")
parts.append(table("Lock and GC probes", "plain two-level cgroups on node B (16 vCPU) limited to N CPUs (no pod), "
                   "5-8 s per probe, 2 runs per variant. ly = threads taking mutexes and yielding with the mutex held; "
                   "sw = 4 busy-polling threads; gc = ParallelGC model; db = backends with short critical sections.",
                   ["Probe", "Machine, limit", "Metric"], body))
parts.append("</div>\n</section>\n")
out = "\n".join(parts)
DENY = os.environ.get("PFS_PUBLISH_DENY",
                      r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b|@|/tmp/|/home/")
bad = re.findall(DENY, out, re.I)
if bad:
    sys.exit(f"sanitize check failed: {sorted(set(bad))}")
open(OUT, "w").write(out)
