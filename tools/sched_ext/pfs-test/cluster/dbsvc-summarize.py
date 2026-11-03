#!/usr/bin/env python3
"""Summarise db-services results: one row per run (runs.csv) and grouped markdown tables (summary.md).

summarize.py RESULTS_DIR
Groups runs by (workload, node, mode, limit, variant) and reports mean and [min-max] over repetitions.
"""
import csv
import glob
import json
import os
import re
import statistics
import sys
from collections import defaultdict

ROOT = sys.argv[1]


def kv_file(path: str) -> dict:
    out = {}
    try:
        for line in open(path):
            key, _, val = line.replace("=", " ", 1).strip().partition(" ")
            out[key] = val
    except OSError:
        pass
    return out


def cg_delta(d: str, pod: str, secs: float) -> dict:
    a, b = kv_file(f"{d}/cg-{pod}.t0"), kv_file(f"{d}/cg-{pod}.t1")

    def dl(key: str) -> int:
        try:
            return int(b.get(key, 0)) - int(a.get(key, 0))
        except ValueError:
            return 0
    return {"cpus": dl("usage_usec") / 1e6 / secs if secs else 0, "nr_periods": dl("nr_periods"),
            "nr_throttled": dl("nr_throttled"), "throttled_s": dl("throttled_usec") / 1e6,
            "cpu_max": b.get("cpu.max", "?")}


def scx_delta(d: str) -> dict:
    """Delta of scx_width cumulative counters between the first and last stats block in the window."""
    try:
        text = open(f"{d}/scx-width.log").read()
    except OSError:
        return {}
    recs = []
    for line in text.splitlines():
        pairs = re.findall(r"(\w+)=(\d+)", line)
        if pairs and pairs[0][0] == "direct":
            recs.append({})
        if recs:
            recs[-1].update({k: int(v) for k, v in pairs})
    if len(recs) < 2:
        return {}
    first, last = recs[0], recs[-1]
    return {k: last[k] - first.get(k, 0) for k in last}


def server_pod(d: str) -> str:
    specs = [os.path.basename(p)[5:-5] for p in glob.glob(f"{d}/spec-*.json")]
    servers = [s for s in specs if s not in ("pgcli", "tools", "rcli", "fcli")]
    return servers[0] if servers else ""


def secs_of(d: str) -> float:
    m = re.search(r"secs=(\d+)", open(f"{d}/meta.txt").read())
    return float(m.group(1)) if m else 0.0


def parse_pg(d: str) -> dict:
    t = open(f"{d}/run.out").read()
    r = {}
    m = re.search(r"^tps = ([\d.]+)", t, re.M)
    r["tps"] = float(m.group(1)) if m else None
    m = re.search(r"^n=(\d+) avg=(\d+)(.*)$", t, re.M)
    if m:
        r["lat_avg_ms"] = int(m.group(2)) / 1e3
        for k, v in re.findall(r"p([\d.]+)=(\d+)", m.group(3)):
            r[f"p{k}_ms"] = int(v) / 1e3
    rows = re.findall(r"^(\d{10}) (\d+) (\d+)$", t, re.M)
    if len(rows) > 4:
        rows = rows[1:-1]  # drop partial first/last second
        cnt = [int(c) for _, c, _ in rows]
        r["min_sec_frac"] = min(cnt) / statistics.mean(cnt)
        r["worst_sec_max_ms"] = max(int(x) for _, _, x in rows) / 1e3
    return r


def hist_pct(hist: list, qs: tuple) -> dict:
    total = sum(c for _, c in hist)
    out, acc, i = {}, 0, 0
    for v, c in hist:
        acc += c
        while i < len(qs) and acc >= qs[i] * total:
            out[f"p{qs[i] * 100:g}_ms"] = v
            i += 1
    return out


def parse_my(d: str) -> dict:
    t = open(f"{d}/run.out").read()
    r = {}
    m = re.search(r"transactions:\s+\d+\s+\(([\d.]+) per sec", t)
    r["tps"] = float(m.group(1)) if m else None
    m = re.search(r"avg:\s+([\d.]+)", t)
    r["lat_avg_ms"] = float(m.group(1)) if m else None
    hist = [(float(v), int(c)) for v, c in re.findall(r"^\s+([\d.]+) \|\*+\s+(\d+)$", t, re.M)]
    if hist:
        r.update(hist_pct(hist, (0.5, 0.95, 0.99, 0.999)))
        r["max_ms"] = hist[-1][0]
    secs = [float(x) for x in re.findall(r"^\[ \d+s \] thds: \d+ tps: ([\d.]+)", t, re.M)]
    if len(secs) > 4:
        r["min_sec_frac"] = min(secs[1:-1]) / statistics.mean(secs[1:-1])
    return r


def parse_redis(d: str) -> dict:
    r = {}
    for row in csv.reader(open(f"{d}/run.out")):
        if not row or row[0] == "test":
            continue
        name = row[0].split()[0].lower()
        r[f"{name}_rps"] = float(row[1])
        r[f"{name}_p50_ms"] = float(row[4])
        r[f"{name}_p99_ms"] = float(row[6])
        r[f"{name}_max_ms"] = float(row[7])
    return r


def parse_fortio(d: str) -> dict:
    try:
        j = json.load(open(f"{d}/run.out"))
    except (OSError, ValueError):
        return {}
    h = j["DurationHistogram"]
    r = {"qps": j["ActualQPS"], "req_qps": j.get("RequestedQPS"), "max_ms": h["Max"] * 1e3,
         "avg_ms": h["Avg"] * 1e3, "errors": sum(v for k, v in j.get("RetCodes", {}).items() if k != "200")}
    for p in h.get("Percentiles", []):
        r[f"p{p['Percentile']:g}_ms"] = p["Value"] * 1e3
    return r


def parse_gc(d: str) -> dict:
    try:
        lines = open(f"{d}/gc.log").read().splitlines()
    except OSError:
        return {}
    young, full = [], []
    for line in lines:
        m = re.search(r"Pause (Young|Full).* ([\d.]+)ms$", line)
        if m:
            (young if m.group(1) == "Young" else full).append(float(m.group(2)))
    allp = sorted(young + full)
    if not allp:
        return {}
    return {"gc_young": len(young), "gc_full": len(full), "gc_young_avg_ms": statistics.mean(young) if young else 0,
            "gc_max_ms": allp[-1], "gc_full_avg_ms": statistics.mean(full) if full else 0,
            "gc_total_ms": sum(allp)}


def parse_netperf(d: str) -> dict:
    last = [ln for ln in open(f"{d}/run.out").read().splitlines() if ln.strip()]
    try:
        v = [float(x) for x in last[-1].split(",")]
    except (ValueError, IndexError):
        return {}
    keys = ("min_us", "mean_us", "p50_us", "p90_us", "p99_us", "max_us", "tps")
    return dict(zip(keys, v))


PARSERS = {"pg": parse_pg, "my": parse_my, "redis": parse_redis, "ngx": parse_fortio, "jvm": parse_fortio,
           "netperf": parse_netperf}


def runs() -> list:
    out = []
    for wl, parse in PARSERS.items():
        for d in sorted(glob.glob(f"{ROOT}/{wl}/*/")):
            d = d.rstrip("/")
            name = os.path.basename(d)
            parts = name.split("-")
            node, mode = parts[0], parts[1]
            if parts[2] == "hog":
                mode += "-hog"
                parts = parts[:2] + parts[3:]
            rest = parts[2:-1]
            rep = rest[-1]
            variant = "-".join(rest[:-1])
            lim = rest[0][1:] if rest and rest[0].startswith("l") else ""
            variant = "-".join(rest[1:-1]) if lim else variant
            if not os.path.exists(f"{d}/run.out") or "smoke" in d:
                continue
            row = {"wl": wl, "node": node, "mode": mode, "limit": lim, "variant": variant, "rep": rep, "dir": name}
            try:
                row.update(parse(d))
            except (OSError, ValueError, ZeroDivisionError) as e:
                row["error"] = str(e)
            pod = server_pod(d)
            secs = secs_of(d)
            row.update({f"cg_{k}": v for k, v in cg_delta(d, pod, secs).items()})
            sx = scx_delta(d)
            if sx:
                row["scx_park_full"] = sx.get("park_full")
                row["scx_direct"] = sx.get("direct")
            if wl == "jvm":
                row.update(parse_gc(d))
            out.append(row)
    return out


def fmt(vals: list) -> str:
    vals = [v for v in vals if isinstance(v, (int, float))]
    if not vals:
        return "-"
    m = statistics.mean(vals)
    f = (lambda x: f"{x:.0f}") if abs(m) >= 100 else (lambda x: f"{x:.2f}") if abs(m) < 10 else (lambda x: f"{x:.1f}")
    if len(vals) == 1:
        return f(m)
    return f"{f(m)} [{f(min(vals))}-{f(max(vals))}]"


COLS = {
    "pg": ["tps", "lat_avg_ms", "p50_ms", "p99_ms", "p99.9_ms", "p100_ms", "worst_sec_max_ms", "min_sec_frac",
           "cg_cpus", "cg_nr_throttled", "cg_throttled_s", "scx_park_full"],
    "my": ["tps", "lat_avg_ms", "p95_ms", "p99_ms", "p99.9_ms", "max_ms", "min_sec_frac", "cg_cpus",
           "cg_nr_throttled", "cg_throttled_s", "scx_park_full"],
    "redis": ["set_rps", "set_p50_ms", "set_p99_ms", "set_max_ms", "get_rps", "get_p50_ms", "get_p99_ms",
              "get_max_ms", "cg_cpus", "cg_nr_throttled", "cg_throttled_s"],
    "ngx": ["req_qps", "qps", "p50_ms", "p99_ms", "p99.9_ms", "max_ms", "errors", "cg_cpus", "cg_nr_throttled",
            "cg_throttled_s"],
    "jvm": ["req_qps", "qps", "p50_ms", "p99_ms", "p99.9_ms", "max_ms", "gc_young", "gc_full", "gc_young_avg_ms",
            "gc_full_avg_ms", "gc_max_ms", "cg_cpus", "cg_nr_throttled", "cg_throttled_s"],
    "netperf": ["tps", "mean_us", "p50_us", "p90_us", "p99_us", "max_us"],
}
MODE_ORDER = {"ref": 0, "cfs": 1, "scx": 2, "kwidth": 3, "cfs-hog": 4, "scx-hog": 5, "kwidth-hog": 6}


def main() -> None:
    rows = runs()
    keys = sorted({k for r in rows for k in r})
    with open(f"{ROOT}/runs.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)
    groups = defaultdict(list)
    for r in rows:
        groups[(r["wl"], r["variant"], r["limit"], r["node"], r["mode"])].append(r)
    lines = ["# db-services summary (mean [min-max] over repetitions)", ""]
    for wl, cols in COLS.items():
        ks = sorted((k for k in groups if k[0] == wl),
                    key=lambda k: (k[1], k[2] != "none", -float(k[2]) if k[2] not in ("", "none") else 0,
                                   MODE_ORDER.get(k[4], 9)))
        if not ks:
            continue
        lines += [f"## {wl}", "", "| variant | limit | node | mode | n | " + " | ".join(cols) + " |",
                  "|" + "---|" * (5 + len(cols))]
        for k in ks:
            g = groups[k]
            lines.append(f"| {k[1]} | {k[2]} | {k[3]} | {k[4]} | {len(g)} | "
                         + " | ".join(fmt([r.get(c) for r in g]) for c in cols) + " |")
        lines.append("")
    open(f"{ROOT}/summary.md", "w").write("\n".join(lines))
    print("\n".join(lines))


if __name__ == "__main__":
    main()
