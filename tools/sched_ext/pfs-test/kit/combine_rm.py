#!/usr/bin/env python3
"""combine_rm.py S OUT_MD OUT_JSON: remeasure tables (median [min-max]) per suite, winners, regressions vs CFS.

Variants: ref (node A, 4 vCPU, no limit), CFS-7.0, CFS-7.3, kwidth, scx_pfs (7.0), kPFS-kpfs3 (old), kPFS-kpfs4 (new).
Old data: everything retest-cluster used (results2 db-services / scx-fix, results3/retest). New data:
results4/remeasure (kPFS on kpfs4, CFS-7.3 on the kpfs4 kernel without sched_pfs, scx_pfs, kwidth Redis).
CFS-7.3 pools the kpfs2-kernel runs of retest-cluster and the kpfs4-kernel runs (CFS code is the same).
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
RM = f"{S}/results4/remeasure"
ORDER = ["ref", "CFS-7.0", "CFS-7.3", "kwidth", "scx_pfs", "kPFS-kpfs3", "kPFS-kpfs4"]
MAP_OLD = {"cfs": "CFS-7.0", "scx": "scx_pfs", "cfs73": "CFS-7.3", "kwidth": "kwidth", "kpfs": "kPFS-kpfs3",
           "ref": "ref"}
MAP_NEW = {"cfs": "CFS-7.0", "scx": "scx_pfs", "cfs73": "CFS-7.3", "kwidth": "kwidth", "kpfs": "kPFS-kpfs4",
           "kpfs3": "kPFS-kpfs3", "ref": "ref"}
ARROW = {"hi": " ↑", "lo": " ↓", None: ""}
MACHINE = "node B (16 vCPU)"


def base_of(v):
    return "CFS-7.3" if v in ("kwidth", "kPFS-kpfs3", "kPFS-kpfs4") else "CFS-7.0"


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
def load(path, fn, src):
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
        r["V"], r["hog"], r["src"] = v, hog, src
        out.append(r)
    return out


def summarize_db():
    if os.path.isdir(f"{RM}/db"):
        subprocess.run([sys.executable, "-I", f"{RM}/kit/summarize.py", f"{RM}/db"], check=False,
                       stdout=subprocess.DEVNULL)
    rows = []
    rows += load(f"{S}/results2/db-services/runs.csv", lambda m: {"cfs": "CFS-7.0", "ref": "ref"}.get(m), "old")
    rows += load(f"{S}/results2/scx-fix/runs.csv", lambda m: {"cfs": "CFS-7.0"}.get(m), "old")
    rows += load(f"{S}/results2/scx-fix/final/runs.csv", lambda m: {"cfs": "CFS-7.0", "scx": "scx_pfs"}.get(m), "old")
    rows += load(f"{RT}/db/runs.csv", lambda m: MAP_OLD.get(m), "old")
    if os.path.isdir(f"{RM1}/db"):
        subprocess.run([sys.executable, "-I", f"{RM}/kit/summarize.py", f"{RM1}/db"], check=False,
                       stdout=subprocess.DEVNULL)
    rows += load(f"{RM1}/db/runs.csv", lambda m: MAP_R1.get(m), "old")
    rows += load(f"{RM}/db/runs.csv", lambda m: MAP_NEW.get(m), "new")
    return rows


CPU = [("cg_cpus", "CPUs (cpu.stat)", None), ("sw_cpus", "CPUs (sched_switch)", None)]
DB_TABLES = [
    ("Postgres TPC-B, 32 clients", "pg", "tpcb-c32", ["4", "2", "1.5"], "32 clients",
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo"), ("p99.9_ms", "p99.9 ms", "lo")] + CPU),
    ("Postgres select-only, 32 clients", "pg", "select-c32", ["4", "2", "1.5"], "32 clients",
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo"), ("p99.9_ms", "p99.9 ms", "lo")] + CPU),
    ("Postgres TPC-B, 16 clients", "pg", "tpcb-c16", ["4", "2", "1.5"], "16 clients",
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo")] + CPU),
    ("Postgres select-only, 16 clients", "pg", "select-c16", ["4", "2", "1.5"], "16 clients",
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo")] + CPU),
    ("MariaDB read-write, 32 threads", "my", "oltp_read_write-t32", ["4", "2"], "32 threads",
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo")] + CPU),
    ("MariaDB read-only, 32 threads", "my", "oltp_read_only-t32", ["4", "2"], "32 threads",
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo")] + CPU),
    ("MariaDB read-write, 16 threads", "my", "oltp_read_write-t16", ["4", "2"], "16 threads",
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo")] + CPU),
    ("MariaDB read-only, 16 threads", "my", "oltp_read_only-t16", ["4", "2"], "16 threads",
     [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo")] + CPU),
    ("Redis, io-threads 4", "redis", "io4", ["4", "2"], "io-threads 4, 64 conns",
     [("set_rps", "SET ops/s", "hi"), ("set_p99_ms", "SET p99 ms", "lo"), ("get_rps", "GET ops/s", "hi")] + CPU),
    ("Redis, io-threads 1", "redis", "io1", ["4", "2"], "io-threads 1, 64 conns",
     [("set_rps", "SET ops/s", "hi"), ("set_p99_ms", "SET p99 ms", "lo"), ("set_max_ms", "SET max ms", "lo")] + CPU),
    ("nginx gzip, fixed rate", "ngx", None, ["4", "2"], "fixed rate, 32 conns",
     [("qps", "qps", None), ("p99_ms", "p99 ms", "lo"), ("p99.9_ms", "p99.9 ms", "lo")] + CPU),
    ("JVM ParallelGC, fixed rate", "jvm", None, ["4", "2"], "fixed rate, 32 conns",
     [("p99_ms", "req p99 ms", "lo"), ("p99.9_ms", "req p99.9 ms", "lo"), ("gc_young_avg_ms", "young GC ms", "lo"),
      ("gc_full_avg_ms", "full GC ms", "lo"), ("gc_max_ms", "worst GC ms", "lo")] + CPU),
]
REF_LIMIT = "4"


def table(title, groups, limits, metrics, jrows, suite, regs, case_desc, neigh="none"):
    lines = [f"### {title}", ""]
    hdr = "| limit | variant | n | " + " | ".join(m[1] + ARROW[m[2]] for m in metrics) + " |"
    lines += [hdr, "|---|---|---|" + "---|" * len(metrics)]
    winners = []
    for lim in limits:
        present = []
        for v in ORDER:
            if v == "ref" and lim != REF_LIMIT:
                continue
            g = groups.get((v, lim))
            if not g:
                continue
            sts = {c: stats([num(r.get(c)) for r in g]) for c, _, _ in metrics}
            present.append((v, sts, len(g)))
        if not present:
            continue
        win = {}
        for c, lab, better in metrics:
            if not better:
                continue
            cand = [(v, s[c]["med"]) for v, s, _ in present if v in MAIN and s.get(c)]
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
            lab = "node A (4 vCPU), no limit" if v == "ref" else v
            lines.append(f"| {lim if v != 'ref' else 'none'} | {lab} | {n} | " + " | ".join(cells) + " |")
            jr = {"suite": suite, "case": title, "limit": lim if v != "ref" else "none", "variant": v, "n": n,
                  "winner_of": [c for c in win if win[c] == v]}
            for c, _, _ in metrics:
                st = sts[c]
                jr[c] = round(st["med"], 4) if st else None
                if st and st["n"] > 1:
                    jr[c + "_min"], jr[c + "_max"] = round(st["min"], 4), round(st["max"], 4)
            jrows.append(jr)
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
                    regs.append({"variant": v, "suite": suite, "text":
                                 f"{title} ({MACHINE}, limit {lim}, {case_desc}, neighbours {neigh}): {v} {lab} "
                                 f"{f(x)} vs {b} {f(y)} ({(r - 1) * 100:+.0f}%)"})
    lines.append("")
    if winners:
        lines += ["Winner per cell (median, fixed-build variants): " + "; ".join(winners), ""]
    return lines


def db_section(rows, jrows, regs):
    lines = ["## Databases and services", "",
             "Direction: ↑ higher is better, ↓ lower is better; CPUs: target = the limit (cpu.stat = cgroup usage over "
             "the whole run incl. setup; sched_switch = on-CPU time of the pod's threads from a bpftrace sched_switch "
             "trace, new runs only).", ""]
    for title, wl, case, limits, desc, metrics in DB_TABLES:
        groups = defaultdict(list)
        for r in rows:
            if r["wl"] != wl or r["hog"] or (case and r["variant"] != case):
                continue
            lim = REF_LIMIT if r["V"] == "ref" else r["limit"]
            groups[(r["V"], lim)].append(r)
        lines += table(title, groups, limits, metrics, jrows, "db", regs, desc)
    for case, t in (("tpcb-c32", "TPC-B"), ("select-c32", "select-only")):
        groups = defaultdict(list)
        for r in rows:
            if r["wl"] == "pg" and r["hog"] and r["variant"] == case:
                groups[(r["V"], r["limit"])].append(r)
        lines += table(f"Postgres {t} c32, limit 2, beside an unlimited 16-thread CPU hog", groups, ["2"],
                       [("tps", "tps", "hi"), ("p99_ms", "p99 ms", "lo"), ("p99.9_ms", "p99.9 ms", "lo")] + CPU,
                       jrows, "db-coloc", regs, "32 clients", "unlimited 16-thread CPU hog")
    return lines


def limit_check(rows, webrows):
    """CPUs used vs limit for the new runs (runs of >= 20 s)."""
    lines = ["## CPU limit check (new runs; runs of 20 s or longer)", "",
             "Per run: CPUs from cpu.stat over the run, and from the sched_switch trace: average, steady state "
             "(median of 1 s rates) and the highest 5 s window. Over = cpu.stat, trace average or steady above limit x 1.02. "
             "The 5 s window is an upper bound only: the trace books a thread's on-CPU time when it switches out, so a "
             "thread that runs for a long stretch without switching lands in one window (a 1-thread server at limit 1 "
             "shows 1.3).", "",
             "| suite | variant | runs | limit | cpu.stat median [max] | sched_switch avg median [max] | steady median [max] | "
             "max 5 s window [max] | runs over |", "|---|---|---|---|---|---|---|---|---|"]
    out = []
    groups = defaultdict(list)
    for r in rows:
        if r.get("src") != "new" or r["V"] == "ref" or not r["limit"] or r["limit"] == "none":
            continue
        groups[(r["wl"] + ("-hog" if r["hog"] else ""), r["V"], r["limit"])].append(
            (num(r.get("cg_cpus")), num(r.get("sw_cpus")), num(r.get("sw_steady")), num(r.get("sw_max5s"))))
    for r in webrows:
        if r.get("src") != "new" or r["V"] == "ref":
            continue
        groups[(f"web-{r['neighbours']}-{r['mode']}", r["V"], r["limit"])].append(
            (num(r.get("cpus_used")), num(r.get("sw_cpus")), num(r.get("sw_steady")), num(r.get("sw_max5s"))))

    def mm(v):
        v = [x for x in v if x is not None]
        return f"{f(statistics.median(v))} [{f(max(v))}]" if v else "-"
    for (suite, v, lim), g in sorted(groups.items(), key=lambda t: (t[0][0], ORDER.index(t[0][1]), -float(t[0][2]))):
        L = float(lim)
        over = sum(1 for cg, sw, st, _ in g if max(cg or 0, sw or 0, st or 0) > L * 1.02)
        lines.append(f"| {suite} | {v} | {len(g)} | {lim} | {mm([x[0] for x in g])} | {mm([x[1] for x in g])} | "
                     f"{mm([x[2] for x in g])} | {mm([x[3] for x in g])} | {over} |")
        out.append({"suite": "limit-check", "case": suite, "variant": v, "limit": lim, "runs": len(g), "over": over,
                    "cg_cpus_med": statistics.median([x[0] for x in g if x[0] is not None]) if any(x[0] is not None for x in g) else None,
                    "sw_steady_med": statistics.median([x[2] for x in g if x[2] is not None]) if any(x[2] is not None for x in g) else None,
                    "sw_max5s_max": max([x[3] for x in g if x[3] is not None], default=None)})
    return lines + [""], out


def kheavy_section(jrows):
    """Limits under kernel-heavy load (plain cgroup on node B): CPUs used, cpu.stat and sched_switch trace."""
    lines = ["## Limits under kernel-heavy load (plain cgroup on node B, 20 s per run, 3 runs)", "",
             "urandom = 16 dd readers of 32 MB per read(2) from /dev/urandom; pipe = 8 dd pairs, 16 MB per write(2) "
             "into a pipe; spin = 16 spinning loops. Target: at or below the limit (over = above limit x 1.02).", "",
             "| load | limit | variant | n | CPUs cpu.stat median [max] | CPUs sched_switch median [max] | runs over |",
             "|---|---|---|---|---|---|---|"]
    res = defaultdict(list)
    for p in sorted(glob.glob(f"{RM}/kheavy/*.txt")) + sorted(glob.glob(f"{RM1}/kheavy/*.txt")):
        for line in open(p):
            if line.startswith("KH "):
                kv = dict(x.split("=", 1) for x in line.split()[1:] if "=" in x)
                v = (MAP_R1 if p.startswith(RM1) else MAP_NEW).get(kv["mode"])
                if v:
                    res[(kv["load"], kv["limit"], v)].append((num(kv.get("cpus")), num(kv.get("sw_cpus"))))

    def mm(v):
        v = [x for x in v if x is not None]
        return f"{f(statistics.median(v))} [{f(max(v))}]" if v else "-"
    for load in ("urandom", "pipe", "spin"):
        for lim in ("1.5", "4"):
            for v in ORDER:
                g = res.get((load, lim, v))
                if not g:
                    continue
                L = float(lim)
                over = sum(1 for a, b in g if max(a or 0, b or 0) > L * 1.02)
                lines.append(f"| {load} | {lim} | {v} | {len(g)} | {mm([a for a, _ in g])} | {mm([b for _, b in g])} | {over} |")
                ca = [a for a, _ in g if a is not None]
                cb = [b for _, b in g if b is not None]
                jrows.append({"suite": "kheavy", "case": load, "limit": lim, "variant": v, "n": len(g), "over": over,
                              "cpus": statistics.median(ca) if ca else None, "cpus_max": max(ca) if ca else None,
                              "sw_cpus": statistics.median(cb) if cb else None, "sw_cpus_max": max(cb) if cb else None})
    return lines + [""]


SU_LIMS = ["0.05", "0.1", "0.2", "0.25", "0.5", "0.75", "1.25", "1.5"]


def subunit_section(jrows):
    """Sub-unit limits: per workload and limit, completion latency, CPU over the run vs 100 ms / 1 s windows,
    throttle counters and held-back time (cpu.pressure some: runnable work not running)."""
    lines = ["## Sub-unit limits (plain cgroup on node B; one run per cell)", "",
             "Workloads: req-N = single requests needing N ms of CPU, Poisson arrivals at about 50% of the limit, 4 "
             "workers; burst-tT-pP = T threads x 2-10 ms every P ms; spin-tT = T spinning threads, 20 s. CPUs: "
             "cpu.stat over the run, sched_switch trace over the run, and the highest CPU rate over 100 ms and over "
             "1 s windows (cpu.stat sampled every 100 ms; over 100 ms = share of 100 ms windows above limit x 1.05). "
             "held = time with runnable work held back (cgroup cpu.pressure some), as a share of the run; thr = "
             "nr_throttled delta and throttled time from cpu.stat (kwidth and kPFS report theirs there; scx_pfs per-"
             "cgroup counters are in its loader log).", ""]
    res = defaultdict(list)
    for p in sorted(glob.glob(f"{RM}/subunit/*.txt")):
        for line in open(p):
            if line.startswith("SU "):
                kv = dict(x.split("=", 1) for x in line.split()[1:] if "=" in x)
                v = MAP_NEW.get(kv.get("mode"))
                if v:
                    res[(kv["wl"], kv["limit"], v)].append(kv)
    wls = sorted({k[0] for k in res}, key=lambda w: (w.split("-")[0], [int(x) if x.isdigit() else x for x in re.split(r"(\d+)", w)]))
    for wl in wls:
        lat = ("p50_ms", "p99_ms", "max_ms")
        cols = [("p50_ms", "p50 ms", "lo"), ("p99_ms", "p99 ms", "lo"), ("max_ms", "max ms", "lo")] if not wl.startswith("spin") else []
        if wl.startswith("burst"):
            cols.append(("late", "late reqs", "lo"))
        cols += [("cpus", "CPUs run", None), ("sw_cpus", "CPUs trace", None), ("w100_max", "max 100 ms", None),
                 ("w100_over", "over 100 ms", None), ("w1s_max", "max 1 s", None), ("held_frac", "held share", "lo"),
                 ("d_nr_throttled", "thr periods", None), ("d_throttled_usec", "thr ms", None)]
        lines += [f"### {wl}", "", "| limit | variant | " + " | ".join(c[1] + ARROW[c[2]] for c in cols) + " |",
                  "|---|---|" + "---|" * len(cols)]
        for lim in SU_LIMS:
            present = [(v, res[(wl, lim, v)][-1]) for v in ORDER if res.get((wl, lim, v))]
            if not present:
                continue
            win = {}
            for c, _, b in cols:
                if not b:
                    continue
                cand = [(v, num(r.get(c))) for v, r in present if v in MAIN and num(r.get(c)) is not None]
                if len(cand) >= 2:
                    win[c] = min(cand, key=lambda x: x[1])[0]
            for v, r in present:
                cells = []
                for c, _, _ in cols:
                    x = num(r.get(c))
                    if c == "d_throttled_usec" and x is not None:
                        x = x / 1000
                    s = f(x) if x is not None else "-"
                    cells.append(f"**{s}**" if win.get(c) == v else s)
                lines.append(f"| {lim} | {v} | " + " | ".join(cells) + " |")
                jr = {"suite": "subunit", "case": wl, "limit": lim, "variant": v}
                for c, _, _ in cols:
                    jr[c] = num(r.get(c))
                jrows.append(jr)
        lines.append("")
    return lines


# ---------------------------------------------------------------- web
def web_rows():
    rows = []
    for base, mp, src in ((f"{RT}", MAP_OLD, "old"), (f"{RM}", MAP_NEW, "new")):
        d = f"{base}/web"
        if not os.path.isdir(f"{d}/runs"):
            continue
        subprocess.run([sys.executable, "-I", f"{RM}/kit/web/summarize.py", "--csv", d], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not os.path.exists(f"{d}/web-rps.csv"):
            continue
        for r in csv.DictReader(open(f"{d}/web-rps.csv")):
            v = mp.get(r["variant"])
            if v:
                r = dict(r)
                r["V"], r["src"] = v, src
                rows.append(r)
    return rows


def web_section(rows, jrows, regs):
    lines = ["## Web RPS (websrv, 500 us of CPU per request; client wrk2p on node A, 256 connections, 60 s)", "",
             "Closed loop = max RPS; fixed rate = 0.6 x the earlier max RPS of the cell. Heavy neighbours = 2 "
             "BestEffort pods x 16 spinning threads + 20 limited pods (0.5/1 CPU, 4 spinning threads each).", ""]
    groups = defaultdict(list)
    for r in rows:
        lim = REF_LIMIT if r["V"] == "ref" else r["limit"]
        groups[(r["V"], lim, r["threads"], r["neighbours"], r["mode"])].append(r)
    cells = sorted({(k[1], k[2]) for k in groups if k[0] != "ref"}, key=lambda t: (float(t[0]), int(t[1])))
    for nb in ("none", "heavy"):
        for mode, metrics in (("closed", [("rps", "RPS", "hi"), ("cpus_used", "CPUs (cpu.stat)", None),
                                          ("sw_cpus", "CPUs (sched_switch)", None)]),
                              ("open", [("rps", "RPS", None), ("p50", "p50 ms", "lo"), ("p99", "p99 ms", "lo"),
                                        ("p999", "p99.9 ms", "lo"), ("cpus_used", "CPUs (cpu.stat)", None),
                                        ("sw_cpus", "CPUs (sched_switch)", None)])):
            lines += [f"### {'Closed loop (max RPS)' if mode == 'closed' else 'Fixed rate'}, neighbours: {nb}", ""]
            for lim, th in cells:
                sub = {(v, lim): groups[(v, lim, th, nb, mode)] for v in ORDER
                       if groups.get((v, lim, th, nb, mode)) and (v != "ref" or nb == "none")}
                if not sub:
                    continue
                tl = table(f"limit {lim}, {th} threads ({mode}, {nb})", sub, [lim], metrics, jrows, "web", regs,
                           f"{th} server threads, {mode}", nb)
                lines += ["#" + tl[0]] + tl[1:]
    return lines


# ---------------------------------------------------------------- probes
PROBE_KEYS = {"ly": [("events_s", "events/s", "hi"), ("cpus", "CPUs", None)],
              "sw": [("rounds_s", "rounds/s", "hi"), ("round_p99_ms", "round p99 ms", "lo"), ("cpus", "CPUs", None)],
              "gc": [("pause_p50_ms", "pause p50 ms", "lo"), ("pause_p99_ms", "pause p99 ms", "lo"),
                     ("mut_miters_s", "mutator Miter/s", "hi"), ("cpus", "CPUs", None)],
              "ra": [("lat_p50_ms", "p50 ms", "lo"), ("lat_p99_ms", "p99 ms", "lo"), ("cpus", "CPUs", None)],
              "db": [("ops_s", "ops/s", "hi"), ("op_p99_ms", "p99 ms", "lo"), ("cpus", "CPUs", None)]}


def probe_section(jrows, regs):
    lines = ["## Lock and GC probes (plain two-level cgroups on node B, 5-8 s each)", ""]
    res = []
    for base, mp in ((RT, MAP_OLD), (RM, MAP_NEW)):
        for p in sorted(glob.glob(f"{base}/probe/*-r*.txt")):
            for line in open(p):
                if line.strip().startswith("RES "):
                    kv = dict(t.split("=", 1) for t in line.split()[1:] if "=" in t)
                    kv["V"] = mp.get(kv["mode"])
                    res.append(kv)
    groups = defaultdict(list)
    tests = []
    for kv in res:
        if kv["V"]:
            groups[(kv["test"], kv["V"])].append(kv)
        if kv["test"] not in tests:
            tests.append(kv["test"])
    for t in tests:
        metrics = PROBE_KEYS.get(t[:2], [("cpus", "CPUs", None)])
        sub = {(v, groups[(t, v)][0]["limit"]): groups[(t, v)] for v in ORDER if groups.get((t, v))}
        if not sub:
            continue
        lim = next(iter(sub))[1]
        lines += table(f"probe {t}", sub, [lim], metrics, jrows, "probe", regs, "plain cgroup")
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
    merged = f"{RM}/sm/merged"
    for kind in ("micro", "neighbours", "overcommit", "scale"):
        os.makedirs(f"{merged}/{kind}", exist_ok=True)
        for m in ("cfs", "scx", "cfs73", "kwidth"):
            src = f"{RT}/sm/raw/{kind}/{m}"
            if os.path.isdir(src) and not os.path.exists(f"{merged}/{kind}/{m}"):
                os.symlink(src, f"{merged}/{kind}/{m}")
        if os.path.isdir(f"{RT}/sm/raw/{kind}/kpfs") and not os.path.exists(f"{merged}/{kind}/kpfs3"):
            os.symlink(f"{RT}/sm/raw/{kind}/kpfs", f"{merged}/{kind}/kpfs3")
        if os.path.isdir(f"{RM}/sm/raw/{kind}/kpfs") and not os.path.exists(f"{merged}/{kind}/kpfs"):
            os.symlink(f"{RM}/sm/raw/{kind}/kpfs", f"{merged}/{kind}/kpfs")
    for script in ("parse_micro.py", "parse_rest.py"):
        p = subprocess.run([sys.executable, f"{RM}/kit/sm/{script}", merged], capture_output=True, text=True)
        md = re.sub(r"\| (cfs|scx|cfs73|kwidth|kpfs3|kpfs) \|",
                    lambda m: "| " + MAP_NEW.get(m.group(1), m.group(1)) + " |", p.stdout)
        md_rows(md, "scale-micro", jrows)
        lines += [md, ""]
        if p.returncode:
            lines += [f"(parser {script} error: {p.stderr.strip().splitlines()[-1] if p.stderr.strip() else p.returncode})", ""]
    return lines


def main():
    jrows, regs = [], []
    db = summarize_db()
    web = web_rows()
    lines = []
    lines += db_section(db, jrows, regs)
    lc, lcrows = limit_check(db, web)
    lines += lc
    jrows += lcrows
    lines += kheavy_section(jrows)
    lines += subunit_section(jrows)
    lines += web_section(web, jrows, regs)
    lines += probe_section(jrows, regs)
    lines += sm_section(jrows)
    lines += ["## Regressions against CFS (median; throughput < -5%, latency > +20% and > 0.5 ms)",
              "(kwidth and kPFS against CFS-7.3 where measured, else CFS-7.0; scx_pfs against CFS-7.0)", ""]
    for v in ("kPFS-kpfs4", "kPFS-kpfs3", "kwidth", "scx_pfs"):
        rr = [r["text"] for r in regs if r["variant"] == v]
        lines += [f"### {v} ({len(rr)})", ""] + [f"- {t}" for t in rr] + [""]
    open(OUT_MD, "w").write("\n".join(lines))
    json.dump({"source": "remeasure: node B (16 vCPU) limited pods; ref = node A (4 vCPU) without limits; "
                         "median and min/max over repetitions",
               "variants": ORDER, "rows": jrows, "regressions": regs}, open(OUT_JSON, "w"), indent=1)


if __name__ == "__main__":
    main()
