#!/usr/bin/env python3
"""combine.py S OUT_MD OUT_JSON: retest tables (median [min-max]) per suite, winners, regressions vs CFS.

Variants: ref (node A, 4 vCPU, no limit), CFS-7.0, CFS-7.3, kwidth, scx_pfs (7.0), scx_pfs-7.3, kPFS.
DB/service sources: results3/retest/db (this retest) plus the same-node runs of the scx-fix suite with the same
final scx_pfs build (results2/scx-fix/final), its CFS reruns (results2/scx-fix) and the db-services CFS / node A
runs (results2/db-services). Web: results3/retest/web. Probes: results3/retest/probe. Scale/micro: parse_*.py.
"""
import csv
import glob
import json
import os
import re
import statistics
import subprocess
import sys
from collections import defaultdict

S, OUT_MD, OUT_JSON = sys.argv[1:4]
RT = f"{S}/results3/retest"
ORDER = ["ref", "CFS-7.0", "CFS-7.3", "kwidth", "scx_pfs", "scx_pfs-7.3", "kPFS"]
LIMITED = ORDER[1:]
MAP_RT = {"cfs": "CFS-7.0", "scx": "scx_pfs", "cfs73": "CFS-7.3", "scx73": "scx_pfs-7.3", "kwidth": "kwidth",
          "kpfs": "kPFS", "ref": "ref"}


def base_of(v):
    """CFS baseline used for regressions: same kernel where measured."""
    return "CFS-7.3" if v in ("kwidth", "kPFS", "scx_pfs-7.3") else "CFS-7.0"


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def stats(vals):
    v = [x for x in vals if x is not None]
    if not v:
        return None
    return {"med": statistics.median(v), "min": min(v), "max": max(v), "n": len(v)}


def f(x):
    if x is None:
        return "-"
    a = abs(x)
    return f"{x:.0f}" if a >= 100 else f"{x:.1f}" if a >= 10 else f"{x:.2f}"


def fs(st):
    if not st:
        return "-"
    if st["n"] == 1 or st["min"] == st["max"]:
        return f(st["med"])
    return f"{f(st['med'])} [{f(st['min'])}-{f(st['max'])}]"


# ---------------------------------------------------------------- DB / services
def load(path, fn):
    out = []
    if not os.path.exists(path):
        return out
    for r in csv.DictReader(open(path)):
        mode = r["mode"]
        hog = mode.endswith("-hog")
        v = fn(mode[:-4] if hog else mode)
        if not v:
            continue
        r = dict(r)
        r["V"] = v
        r["hog"] = hog
        out.append(r)
    return out


def summarize_db():
    if os.path.isdir(f"{RT}/db"):
        subprocess.run([sys.executable, "-I", f"{RT}/kit/summarize.py", f"{RT}/db"], check=False,
                       stdout=subprocess.DEVNULL)
    rows = []
    rows += load(f"{S}/results2/db-services/runs.csv", lambda m: {"cfs": "CFS-7.0", "ref": "ref"}.get(m))
    rows += load(f"{S}/results2/scx-fix/runs.csv", lambda m: {"cfs": "CFS-7.0"}.get(m))
    rows += load(f"{S}/results2/scx-fix/final/runs.csv", lambda m: {"cfs": "CFS-7.0", "scx": "scx_pfs"}.get(m))
    rows += load(f"{RT}/db/runs.csv", lambda m: MAP_RT.get(m))
    return rows


# (title, wl, case, limits, metrics [(col, label, better)], primary tput col, primary latency col)
DB_TABLES = [
    ("Postgres TPC-B, 32 clients", "pg", "tpcb-c32", ["4", "2", "1.5"],
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo"), ("p99.9_ms", "p99.9 ms", "lo"), ("cg_cpus", "CPUs", None)]),
    ("Postgres select-only, 32 clients", "pg", "select-c32", ["4", "2", "1.5"],
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo"), ("p99.9_ms", "p99.9 ms", "lo"), ("cg_cpus", "CPUs", None)]),
    ("Postgres TPC-B, 16 clients", "pg", "tpcb-c16", ["4", "2", "1.5"],
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo"), ("cg_cpus", "CPUs", None)]),
    ("Postgres select-only, 16 clients", "pg", "select-c16", ["4", "2", "1.5"],
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo"), ("cg_cpus", "CPUs", None)]),
    ("MariaDB read-write, 32 threads", "my", "oltp_read_write-t32", ["4", "2"],
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo"), ("cg_cpus", "CPUs", None)]),
    ("MariaDB read-only, 32 threads", "my", "oltp_read_only-t32", ["4", "2"],
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo"), ("cg_cpus", "CPUs", None)]),
    ("MariaDB read-write, 16 threads", "my", "oltp_read_write-t16", ["4", "2"],
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo"), ("cg_cpus", "CPUs", None)]),
    ("Redis, io-threads 4", "redis", "io4", ["4", "2"],
     [("set_rps", "SET ops/s", "hi"), ("set_p99_ms", "SET p99 ms", "lo"), ("get_rps", "GET ops/s", "hi"),
      ("cg_cpus", "CPUs", None)]),
    ("Redis, io-threads 1", "redis", "io1", ["4", "2"],
     [("set_rps", "SET ops/s", "hi"), ("set_p99_ms", "SET p99 ms", "lo"), ("set_max_ms", "SET max ms", "lo"),
      ("cg_cpus", "CPUs", None)]),
    ("nginx gzip, fixed rate", "ngx", None, ["4", "2"],
     [("qps", "qps", None), ("p99_ms", "p99 ms", "lo"), ("p99.9_ms", "p99.9 ms", "lo"), ("cg_cpus", "CPUs", None)]),
    ("JVM ParallelGC, fixed rate", "jvm", None, ["4", "2"],
     [("p99_ms", "req p99 ms", "lo"), ("p99.9_ms", "req p99.9 ms", "lo"), ("gc_young_avg_ms", "young GC ms", "lo"),
      ("gc_full_avg_ms", "full GC ms", "lo"), ("gc_max_ms", "worst GC ms", "lo"), ("cg_cpus", "CPUs", None)]),
]
REF_LIMIT = "4"  # the 4-vCPU node A is shown next to limit 4


def table(title, groups, limits, metrics, key_of, jrows, suite, regs, extra_case=""):
    lines = [f"### {title}", ""]
    hdr = "| limit | variant | n | " + " | ".join(m[1] for m in metrics) + " |"
    lines += [hdr, "|---|---|---|" + "---|" * len(metrics)]
    winners = []
    for lim in limits:
        present = []
        for v in ORDER:
            if v == "ref" and lim != REF_LIMIT:
                continue
            g = groups.get(key_of(v, lim))
            if not g:
                continue
            sts = {c: stats([num(r.get(c)) for r in g]) for c, _, _ in metrics}
            present.append((v, sts, len(g)))
        if not present:
            continue
        # winners among limited variants per directional metric
        win = {}
        for c, lab, better in metrics:
            if not better:
                continue
            cand = [(v, s[c]["med"]) for v, s, _ in present if v != "ref" and s.get(c)]
            if len(cand) < 2:
                continue
            best = (max if better == "hi" else min)(cand, key=lambda t: t[1])
            win[c] = best[0]
            winners.append(f"{lim}: {lab} -> {best[0]}")
        for v, sts, n in present:
            cells = []
            for c, _, _ in metrics:
                s = fs(sts[c])
                cells.append(f"**{s}**" if win.get(c) == v else s)
            lab = "ref (node A, no limit)" if v == "ref" else v
            lines.append(f"| {lim if v != 'ref' else 'none'} | {lab} | {n} | " + " | ".join(cells) + " |")
            jr = {"suite": suite, "case": title + extra_case, "limit": lim if v != "ref" else "none", "variant": v,
                  "n": n}
            for c, _, _ in metrics:
                st = sts[c]
                jr[c] = round(st["med"], 4) if st else None
                if st and st["n"] > 1:
                    jr[c + "_min"], jr[c + "_max"] = round(st["min"], 4), round(st["max"], 4)
            jrows.append(jr)
        # regressions vs CFS baseline
        byv = {v: s for v, s, _ in present}
        for v, sts, _ in present:
            if v in ("ref", "CFS-7.0", "CFS-7.3"):
                continue
            b = base_of(v)
            if b not in byv:
                b = "CFS-7.0"
            if b not in byv:
                continue
            for c, lab, better in metrics:
                if not better or not sts.get(c) or not byv[b].get(c):
                    continue
                x, y = sts[c]["med"], byv[b][c]["med"]
                if not y:
                    continue
                r = x / y
                if (better == "hi" and r < 0.95) or (better == "lo" and r > 1.20 and x - y > 0.5):
                    regs.append(f"{title}, limit {lim}: {v} {lab} {f(x)} vs {b} {f(y)} ({(r - 1) * 100:+.0f}%)")
    lines.append("")
    if winners:
        lines += ["Winner per cell (median, limited variants): " + "; ".join(winners), ""]
    return lines


def db_section(jrows, regs):
    rows = summarize_db()
    lines = ["## Databases and services", ""]
    for title, wl, case, limits, metrics in DB_TABLES:
        groups = defaultdict(list)
        for r in rows:
            if r["wl"] != wl or r["hog"] or (case and r["variant"] != case):
                continue
            lim = r["limit"]
            if r["V"] == "ref":
                lim = REF_LIMIT
            groups[(r["V"], lim)].append(r)
        lines += table(title, groups, limits, metrics, lambda v, l: (v, l), jrows, "db", regs)
    # co-location
    for case, t in (("tpcb-c32", "TPC-B"), ("select-c32", "select-only")):
        groups = defaultdict(list)
        for r in rows:
            if r["wl"] == "pg" and r["hog"] and r["variant"] == case:
                groups[(r["V"], r["limit"])].append(r)
        lines += table(f"Postgres {t} c32, limit 2, beside an unlimited 16-thread CPU hog", groups, ["2"],
                       [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo"), ("p99.9_ms", "p99.9 ms", "lo"),
                        ("cg_cpus", "CPUs", None)], lambda v, l: (v, l), jrows, "db-coloc", regs)
    return lines


# ---------------------------------------------------------------- web
def web_section(jrows, regs):
    d = f"{RT}/web"
    lines = ["## Web RPS (websrv, 500 us of CPU per request; client wrk2p, 256 connections, 60 s)", ""]
    if not os.path.isdir(f"{d}/runs"):
        return lines + ["(no data)", ""]
    subprocess.run([sys.executable, "-I", f"{RT}/kit/web/summarize.py", "--csv", d], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rows = list(csv.DictReader(open(f"{d}/web-rps.csv"))) if os.path.exists(f"{d}/web-rps.csv") else []
    groups = defaultdict(list)
    for r in rows:
        v = MAP_RT.get(r["variant"])
        if not v:
            continue
        lim = REF_LIMIT if v == "ref" else r["limit"]
        groups[(v, lim, r["threads"], r["neighbours"], r["mode"])].append(r)
    cells = sorted({(k[1], k[2]) for k in groups if k[0] != "ref"}, key=lambda t: (float(t[0]), int(t[1])))
    for nb in ("none", "heavy"):
        lines += [f"### Closed loop (max RPS), neighbours: {nb}", ""]
        for lim, th in cells:
            pass
        for mode, metrics in (("closed", [("rps", "RPS", "hi"), ("p99", "p99 ms", None), ("cpus_used", "CPUs", None),
                                         ("nr_throttled", "throttled periods", None)]),
                              ("open", [("rps", "RPS", None), ("p50", "p50 ms", "lo"), ("p99", "p99 ms", "lo"),
                                        ("p999", "p99.9 ms", "lo"), ("cpus_used", "CPUs", None)])):
            if mode == "open":
                lines += [f"### Fixed rate (0.6 x earlier max RPS of the cell), neighbours: {nb}", ""]
            for lim, th in cells:
                sub = {(v, l): groups[(v, l, th, nb, mode)] for v in ORDER for l in (lim,)
                       if groups.get((v, l, th, nb, mode))}
                if not sub:
                    continue
                t = f"limit {lim}, {th} threads"
                tl = table(f"{t} ({mode}, {nb})", sub, [lim], metrics, lambda v, l: (v, l), jrows, "web", regs)
                lines += ["#" + tl[0]] + tl[1:]
        lines.append("")
    return lines


# ---------------------------------------------------------------- probes
PROBE_KEYS = {"ly": [("events_s", "events/s", "hi"), ("cpus", "CPUs", None)],
              "sw": [("rounds_s", "rounds/s", "hi"), ("round_p99_ms", "round p99 ms", "lo"), ("cpus", "CPUs", None)],
              "gc": [("pause_p50_ms", "pause p50 ms", "lo"), ("pause_p99_ms", "pause p99 ms", "lo"),
                     ("mut_miters_s", "mutator Miter/s", "hi"), ("cpus", "CPUs", None)],
              "ra": [("lat_p50_ms", "p50 ms", "lo"), ("lat_p99_ms", "p99 ms", "lo"), ("cpus", "CPUs", None)],
              "db": [("ops_s", "ops/s", "hi"), ("op_p99_ms", "p99 ms", "lo"), ("cpus", "CPUs", None)]}


def probe_section(jrows, regs):
    lines = ["## Lock and GC probes (plain two-level cgroups on node B, 5-8 s each)", "",
             "ly T/L/Y = T threads, L mutexes, Y sched_yield() calls with the mutex held; sw = 4 busy-polling "
             "threads (Redis io-threads model); gc = ParallelGC model (8 mutators, safepoints, spin+yield "
             "termination); rate = 16 workers at a fixed rate, 1 ms CPU per request; db = backends with a short "
             "critical section and 100-200 us sleeps.", ""]
    res = []
    for p in sorted(glob.glob(f"{RT}/probe/*-r*.txt")):
        for line in open(p):
            if line.strip().startswith("RES "):
                kv = dict(t.split("=", 1) for t in line.split()[1:] if "=" in t)
                res.append(kv)
    groups = defaultdict(list)
    for kv in res:
        v = MAP_RT.get(kv["mode"])
        if v:
            groups[(kv["test"], v)].append(kv)
    tests = []
    for kv in res:
        if kv["test"] not in tests:
            tests.append(kv["test"])
    for t in tests:
        metrics = PROBE_KEYS.get(t[:2], [("cpus", "CPUs", None)])
        sub = {(v, groups[(t, v)][0]["limit"]): groups[(t, v)] for v in ORDER if groups.get((t, v))}
        lim = next(iter(sub))[1]
        lines += table(f"probe {t}", sub, [lim], metrics, lambda v, l: (v, l), jrows, "probe", regs)
    return lines


# ---------------------------------------------------------------- scale / micro
def md_rows(md, suite, jrows):
    head = None
    sect = ""
    for line in md.splitlines():
        if line.startswith("#"):
            sect = line.strip("# ").strip()
            head = None
            continue
        if line.startswith("|") and "---" not in line:
            cells = [c.strip() for c in line.strip("|").split("|")]
            if head is None:
                head = cells
                continue
            r = {"suite": suite, "case": sect}
            for h, c in zip(head, cells):
                r[h] = c
            jrows.append(r)
        elif not line.startswith("|"):
            head = None if not line.strip() else head


def sm_section(jrows):
    lines = ["## Scale and micro-benchmarks (node B, limited pods; median [min-max] of 3)", ""]
    raw = f"{RT}/sm/raw"
    if not os.path.isdir(raw):
        return lines + ["(no data)", ""]
    for script in ("parse_micro.py", "parse_rest.py"):
        p = subprocess.run([sys.executable, f"{RT}/kit/sm/{script}", raw], capture_output=True, text=True)
        md = p.stdout
        md = re.sub(r"\| (cfs|scx|cfs73|kwidth|kpfs) \|",
                    lambda m: "| " + MAP_RT.get(m.group(1), m.group(1)) + " |", md)
        md_rows(md, "scale-micro", jrows)
        lines += [md, ""]
        if p.returncode:
            lines += [f"(parser {script} error: {p.stderr.strip().splitlines()[-1] if p.stderr.strip() else p.returncode})", ""]
    return lines


def main():
    jrows, regs = [], []
    lines = []
    lines += db_section(jrows, regs)
    lines += web_section(jrows, regs)
    lines += probe_section(jrows, regs)
    lines += sm_section(jrows)
    lines += ["## Regressions against CFS (median; throughput < -5%, latency > +20% and > 0.5 ms)",
              "(kwidth, kPFS and scx_pfs-7.3 against CFS-7.3 where measured, else CFS-7.0; scx_pfs against CFS-7.0)", ""]
    lines += [f"- {r}" for r in regs] + [""]
    open(OUT_MD, "w").write("\n".join(lines))
    json.dump({"source": "retest: node B (16 vCPU) limited pods; ref = node A (4 vCPU) without limits; "
                         "median and min/max over repetitions",
               "variants": ORDER, "rows": jrows, "regressions": regs}, open(OUT_JSON, "w"), indent=1)


if __name__ == "__main__":
    main()
