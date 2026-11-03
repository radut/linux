#!/usr/bin/env python3
"""combine.py: before/after tables for the scx-fix report and dash/dbfix.json.

Sources (runs.csv written by summarize.py in each root):
  before = results2/db-services (old scx build, its cfs and node-A reference runs)
  cfs2   = results2/scx-fix and results2/scx-fix/final cfs runs (same node, 2026-10-25)
  kicks  = results2/scx-fix scx runs (intermediate build with idle-CPU kicks)
  after  = results2/scx-fix/final scx runs (final build)
"""
import csv
import json
import statistics
import sys
from collections import defaultdict

S = sys.argv[1]
OUT_JSON = sys.argv[2]


def load(path, tag_fn):
    rows = []
    try:
        for r in csv.DictReader(open(path)):
            t = tag_fn(r)
            if t:
                r["src"] = t
                rows.append(r)
    except OSError:
        pass
    return rows


rows = []
rows += load(f"{S}/results2/db-services/runs.csv",
             lambda r: {"scx": "before", "cfs": "cfs", "ref": "ref", "scx-hog": "before-hog",
                        "cfs-hog": "cfs-hog"}.get(r["mode"]))
rows += load(f"{S}/results2/scx-fix/runs.csv",
             lambda r: {"scx": "kicks", "cfs": "cfs2"}.get(r["mode"]))
rows += load(f"{S}/results2/scx-fix/final/runs.csv",
             lambda r: {"scx": "after", "cfs": "cfs2", "scx-hog": "after-hog",
                        "cfs-hog": "cfs2-hog"}.get(r["mode"]))

groups = defaultdict(list)
for r in rows:
    groups[(r["wl"], r["variant"], r["limit"], r["src"])].append(r)


def val(g, col):
    v = []
    for r in g:
        try:
            v.append(float(r[col]))
        except (KeyError, ValueError, TypeError):
            pass
    return v


def fmt(v):
    if not v:
        return "-"
    m = statistics.mean(v)
    f = (lambda x: f"{x:.0f}") if abs(m) >= 100 else (lambda x: f"{x:.2f}") if abs(m) < 10 else (lambda x: f"{x:.1f}")
    return f(m) if len(v) == 1 else f"{f(m)} [{f(min(v))}-{f(max(v))}]"


SRCS = ["ref", "cfs", "cfs2", "before", "kicks", "after"]
TABLES = [
    ("Postgres TPC-B c32", "pg", "tpcb-c32", ["tps", "p50_ms", "p99_ms", "p99.9_ms", "cg_cpus"]),
    ("Postgres select-only c32", "pg", "select-c32", ["tps", "p50_ms", "p99_ms", "p99.9_ms", "cg_cpus"]),
    ("Postgres TPC-B c16", "pg", "tpcb-c16", ["tps", "p99_ms", "cg_cpus"]),
    ("Postgres select-only c16", "pg", "select-c16", ["tps", "p99_ms", "cg_cpus"]),
    ("MariaDB oltp_read_write t32", "my", "oltp_read_write-t32", ["tps", "p95_ms", "p99_ms", "cg_cpus"]),
    ("MariaDB oltp_read_only t32", "my", "oltp_read_only-t32", ["tps", "p95_ms", "p99_ms", "cg_cpus"]),
    ("Redis io-threads 4", "redis", "io4", ["set_rps", "set_p99_ms", "get_rps", "get_p99_ms", "cg_cpus"]),
    ("Redis io-threads 1", "redis", "io1", ["set_rps", "set_p99_ms", "set_max_ms", "cg_cpus"]),
    ("nginx fixed rate", "ngx", None, ["qps", "p50_ms", "p99_ms", "p99.9_ms", "max_ms", "cg_cpus"]),
    ("JVM ParallelGC fixed rate", "jvm", None,
     ["p99_ms", "p99.9_ms", "max_ms", "gc_young_avg_ms", "gc_full_avg_ms", "gc_max_ms", "cg_cpus"]),
]
lines = []
jrows = []
for title, wl, variant, cols in TABLES:
    keys = sorted({(k[1], k[2]) for k in groups if k[0] == wl and (variant is None or k[1] == variant)})
    if not keys:
        continue
    lines += [f"### {title}", "", "| limit | variant | n | " + " | ".join(cols) + " |", "|---|---|---|" + "---|" * len(cols)]
    for var, lim in sorted(keys, key=lambda k: (k[1] != "none", -float(k[1]) if k[1] not in ("none", "") else 0, k[0])):
        for src in SRCS:
            g = groups.get((wl, var, lim, src))
            if not g:
                continue
            if src == "ref" and wl in ("ngx", "jvm"):
                pass
            lines.append(f"| {lim} | {src} ({var}) | {len(g)} | " + " | ".join(fmt(val(g, c)) for c in cols) + " |")
            jr = {"workload": wl, "case": var, "limit": lim, "variant": src, "n": len(g)}
            for c in cols:
                v = val(g, c)
                jr[c] = round(statistics.mean(v), 3) if v else None
            jrows.append(jr)
    lines.append("")
# co-location
for wl, var in (("pg", "tpcb-c32"), ("pg", "select-c32")):
    pass
hog = [k for k in groups if k[3].endswith("hog")]
if hog:
    lines += ["### Postgres limit 2 beside an unlimited 16-thread CPU hog", "",
              "| case | variant | n | tps | p99_ms | p99.9_ms | cg_cpus |", "|---|---|---|---|---|---|---|"]
    for k in sorted(hog):
        g = groups[k]
        lines.append(f"| {k[1]} | {k[3]} | {len(g)} | " + " | ".join(fmt(val(g, c)) for c in ("tps", "p99_ms", "p99.9_ms", "cg_cpus")) + " |")
        jr = {"workload": "pg-coloc", "case": k[1], "limit": k[2], "variant": k[3], "n": len(g)}
        for c in ("tps", "p99_ms", "p99.9_ms", "cg_cpus"):
            v = val(g, c)
            jr[c] = round(statistics.mean(v), 3) if v else None
        jrows.append(jr)
    lines.append("")
print("\n".join(lines))
json.dump({"source": "scx-fix cluster runs: before = old scx build (db-services), cfs/cfs2 = CFS quota, "
                     "kicks = intermediate build, after = final build, ref = node A without limits",
           "rows": jrows}, open(OUT_JSON, "w"), indent=1)
