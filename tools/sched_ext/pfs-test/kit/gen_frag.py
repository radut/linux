#!/usr/bin/env python3
"""gen_frag.py JSON PROSE_HTML OUT_HTML: dashboard section tab-retest-cluster from retest_cluster.json.

Key-cell tables per suite (median, [min-max] when repeated) for CFS-7.0, CFS-7.3, kwidth, scx_pfs, kPFS and the
4-vCPU node A reference. PROSE_HTML holds the hand-written headline/regression text (already sanitized).
"""
import os
import html
import json
import re
import sys

J, PROSE, OUT = sys.argv[1:4]
d = json.load(open(J))
rows = d["rows"]
COLS = ["CFS-7.0", "CFS-7.3", "kwidth", "scx_pfs", "kPFS", "ref"]
HEAD = ["CFS 7.0", "CFS 7.3", "kwidth", "scx_pfs", "kPFS", "node A, 4 vCPU, no limit"]


def f(x):
    if x is None:
        return "-"
    a = abs(x)
    if a >= 10000:
        return f"{x / 1000:.1f}k"
    return f"{x:.0f}" if a >= 100 else f"{x:.1f}" if a >= 10 else f"{x:.2f}"


def get(suite, case, limit, variant):
    for r in rows:
        if r.get("suite") == suite and r.get("case") == case and r.get("variant") == variant and \
                r.get("limit") == (limit if variant != "ref" else "none"):
            return r
    return None


def cell(suite, case, limit, variant, cols, better):
    r = get(suite, case, limit, variant)
    if not r:
        return None, "-"
    vals = [r.get(c) for c in cols]
    return vals, " · ".join(f(v) for v in vals)


# (suite, case title in json, limit, metric cols, label, better per first metric: hi/lo)
KEY = [
    ("db", "Postgres TPC-B, 32 clients", "4", ["tps", "p99_ms", "cg_cpus"], "Postgres TPC-B c32", "tps · p99 ms · CPUs"),
    ("db", "Postgres TPC-B, 32 clients", "2", ["tps", "p99_ms", "cg_cpus"], "Postgres TPC-B c32", "tps · p99 ms · CPUs"),
    ("db", "Postgres TPC-B, 32 clients", "1.5", ["tps", "p99_ms"], "Postgres TPC-B c32", "tps · p99 ms"),
    ("db", "Postgres select-only, 32 clients", "4", ["tps", "p99_ms"], "Postgres select-only c32", "tps · p99 ms"),
    ("db", "Postgres select-only, 32 clients", "2", ["tps", "p99_ms"], "Postgres select-only c32", "tps · p99 ms"),
    ("db", "MariaDB read-write, 32 threads", "4", ["tps", "p99_ms"], "MariaDB read-write t32", "tps · p99 ms"),
    ("db", "MariaDB read-write, 32 threads", "2", ["tps", "p99_ms"], "MariaDB read-write t32", "tps · p99 ms"),
    ("db", "MariaDB read-only, 32 threads", "4", ["tps", "p99_ms"], "MariaDB read-only t32", "tps · p99 ms"),
    ("db-coloc", "Postgres TPC-B c32, limit 2, beside an unlimited 16-thread CPU hog", "2", ["tps", "p99_ms"],
     "Postgres TPC-B beside a CPU hog", "tps · p99 ms"),
    ("db", "Redis, io-threads 4", "4", ["set_rps", "set_p99_ms"], "Redis io-threads 4", "SET ops/s · p99 ms"),
    ("db", "Redis, io-threads 4", "2", ["set_rps", "set_p99_ms"], "Redis io-threads 4", "SET ops/s · p99 ms"),
    ("db", "Redis, io-threads 1", "4", ["set_rps", "set_p99_ms"], "Redis io-threads 1", "SET ops/s · p99 ms"),
    ("db", "nginx gzip, fixed rate", "4", ["p99_ms", "p99.9_ms"], "nginx gzip, fixed rate", "p99 · p99.9 ms"),
    ("db", "nginx gzip, fixed rate", "2", ["p99_ms", "p99.9_ms"], "nginx gzip, fixed rate", "p99 · p99.9 ms"),
    ("db", "JVM ParallelGC, fixed rate", "4", ["gc_full_avg_ms", "gc_max_ms", "p99.9_ms"], "JVM ParallelGC",
     "full GC avg · worst GC · req p99.9 ms"),
    ("db", "JVM ParallelGC, fixed rate", "2", ["gc_full_avg_ms", "gc_max_ms", "p99.9_ms"], "JVM ParallelGC",
     "full GC avg · worst GC · req p99.9 ms"),
]
WEB = [("0.25", "8"), ("1", "8"), ("1", "256"), ("2", "64"), ("4", "8"), ("4", "256")]
PROBES = [("ly16-4-100", ["events_s", "cpus"], "lock+yield 16 thr / 4 locks", "events/s · CPUs"),
          ("ly8-8-100", ["events_s"], "lock+yield 8 thr / 8 locks", "events/s"),
          ("sw4-l2", ["rounds_s"], "4 spinning threads, limit 2", "rounds/s"),
          ("gc-l4", ["pause_p99_ms"], "GC model, limit 4", "pause p99 ms"),
          ("db64-l4", ["ops_s", "op_p99_ms"], "64 DB-like backends, limit 4", "ops/s · p99 ms")]


def tr(cells, cls=""):
    return "<tr>" + "".join(cells) + "</tr>"


def td(x, l=False, b=False):
    x = html.escape(x)
    if b:
        x = f"<b>{x}</b>"
    return f'<td class="l">{x}</td>' if l else f"<td>{x}</td>"


def table(title, header_first, body):
    th = "".join(f'<th class="l">{h}</th>' for h in header_first) + "".join(f"<th>{html.escape(h)}</th>" for h in HEAD)
    return (f'  <h3>{html.escape(title)}</h3>\n  <div class="sx-tablewrap"><table>\n    <thead><tr>{th}</tr></thead>\n'
            f'    <tbody>\n' + "\n".join("      " + b for b in body) + "\n    </tbody>\n  </table></div>\n")


def best(vals_by_col, better):
    c = {k: v[0] for k, v in vals_by_col.items() if v and v[0] is not None and k != "ref"}
    if len(c) < 2:
        return None
    return (max if better == "hi" else min)(c, key=c.get)


def key_rows(spec, better_of):
    body = []
    for suite, case, lim, cols, label, metric in spec:
        vals, txt = {}, {}
        for v in COLS:
            if v == "ref" and lim != "4":
                vals[v], txt[v] = None, "-"
                continue
            vals[v], txt[v] = cell(suite, case, lim, v, cols, None)
            if txt[v].strip("- ·") == "" and (case, lim, v) in OVERRIDE:
                txt[v] = OVERRIDE[(case, lim, v)]
        b = best(vals, better_of(cols[0]))
        body.append(tr([td(label, True), td(lim, True), td(metric, True)] +
                       [td(txt[v], b=(v == b)) for v in COLS]))
    return body


OVERRIDE = {("Redis, io-threads 4", "2", "kwidth"): "< 1.7k (runs timed out)"}
HI = {"tps", "set_rps", "rps", "events_s", "rounds_s", "ops_s"}


def better_of(c):
    return "hi" if c in HI else "lo"


parts = ['<section class="sx-page" id="tab-retest-cluster" role="tabpanel" aria-labelledby="tabbtn-retest-cluster" hidden>',
         '<div class="sx-card" id="retest-cluster">', open(PROSE).read()]
parts.append(table("Databases and services (node B limited pods)", ["Workload", "Limit", "Metric"], key_rows(KEY, better_of)))
# web
body = []
for nb in ("none", "heavy"):
    for lim, th in WEB:
        for mode, cols, metric in (("closed", ["rps"], "max RPS"), ("open", ["p99"], "p99 ms at fixed rate")):
            case = f"limit {lim}, {th} threads ({mode}, {nb})"
            vals, txt = {}, {}
            for v in COLS:
                r = get("web", case, lim, v)
                if v == "ref":
                    r = get("web", f"limit 4, {th} threads ({mode}, none)", "4", "ref") if (lim == "4" and nb == "none") else None
                vals[v] = [r.get(c) for c in cols] if r else None
                txt[v] = " · ".join(f(x) for x in vals[v]) if r else "-"
            b = best(vals, "hi" if mode == "closed" else "lo")
            body.append(tr([td(f"{th} threads, neighbours {nb}", True), td(lim, True), td(metric, True)] +
                           [td(txt[v], b=(v == b)) for v in COLS]))
parts.append(table("Web RPS (500 us of CPU per request)", ["Cell", "Limit", "Metric"], body))
# probes
body = []
for t, cols, label, metric in PROBES:
    vals, txt = {}, {}
    for v in COLS:
        r = None
        for x in rows:
            if x.get("suite") == "probe" and x.get("case") == f"probe {t}" and x.get("variant") == v:
                r = x
        vals[v] = [r.get(c) for c in cols] if r else None
        txt[v] = " · ".join(f(x) for x in vals[v]) if r else "-"
    b = best(vals, better_of(cols[0]))
    body.append(tr([td(label, True), td(t.split("-l")[-1] if "-l" in t else "4", True), td(metric, True)] +
                   [td(txt[v], b=(v == b)) for v in COLS]))
parts.append(table("Lock and GC probes (plain cgroups on node B)", ["Probe", "Limit", "Metric"], body))
parts.append('  <p class="note">Medians; repeated cells (3 runs for the database c32/t32 cells, 3 for the web 1 and 4 CPU, 8-thread cells, 2 for the probes) also have min-max in the report. Bold = best limited variant for the first metric of the row. CFS 7.3 is the same 7.3 kernel as kPFS booted without the kPFS switch.</p>')
parts.append("</div>\n</section>\n")
out = "\n".join(parts)
# sanitize check: no IPs, hostnames, user or repo names, paths
DENY = os.environ.get("PFS_PUBLISH_DENY",
                      r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b|@|/tmp/|/home/")
bad = re.findall(DENY, out, re.I)
if bad:
    sys.exit(f"sanitize check failed: {sorted(set(bad))}")
open(OUT, "w").write(out)
