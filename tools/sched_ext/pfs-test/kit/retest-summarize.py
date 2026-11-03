#!/usr/bin/env python3
"""summarize.py RESDIR OUT_JSON: parse retest guest outputs into rows + per-metric medians, winners, CFS ratios."""
import csv
import glob
import json
import os
import re
import statistics
import sys

RES, OUT = sys.argv[1], sys.argv[2]
VARS = ["cfs", "kwidth", "scx", "kpfs"]
HDR = ("path,variant,limit,threads,neighbours,secs,miter_s,nr_throttled,throttled_ms,worst_stall_ms,stalls_5ms,"
       "avg_conc,max_conc,pct_over_cap,p50_ms,p99_ms,max_ms,avg_cpus,busy_frac,trace_cpus").split(",")


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def kvs(s):
    return {k: v for k, v in re.findall(r"(\S+?)=(\S+)", s)}


rows = []  # {test, cell, variant, round, metric: value...}


def add(v, r, test, cell, **m):
    rows.append(dict(variant=v, round=int(r), test=test, cell=cell, **{k: x for k, x in m.items() if x is not None}))


for f in sorted(glob.glob(os.path.join(RES, "r[0-9]*-*.txt"))):
    for line in open(f, errors="replace"):
        if not line.startswith("R "):
            continue
        m = re.match(r"R v=(\S+) r=(\d+) test=(\S+) ?(.*)", line.strip())
        if not m:
            continue
        v, r, test, rest = m.groups()
        if test in ("mx", "nn", "latlim"):
            b = re.search(r"b=(\d+)", rest)
            c = re.search(r"csv=(\S*)", rest).group(1).split(",")
            if len(c) < 17:
                continue
            d = dict(zip(HDR, c))
            if test == "latlim":
                add(v, r, "latlim", f"limit {d['limit']}, {b.group(1)} burners",
                    p50=num(d["p50_ms"]), p99=num(d["p99_ms"]), cpus=num(d.get("trace_cpus")))
            else:
                add(v, r, test, f"limit {d['limit']} x {d['threads']} thr", thr=num(d["miter_s"]),
                    stall=num(d["worst_stall_ms"]), cpus=num(d.get("trace_cpus")), cpus_stat=num(d.get("avg_cpus")),
                    conc=num(d["avg_conc"]), maxconc=num(d["max_conc"]), throttled=num(d["nr_throttled"]))
            continue
        k = kvs(rest)
        if test.startswith("reqlat"):
            add(v, r, "ulat", test, p50=num(k.get("p50_ms")), p99=num(k.get("p99_ms")))
        elif test.startswith("wlat"):
            add(v, r, "wlat", test, p50=num(k.get("p50_us")), p99=num(k.get("p99_us")))
        elif test.startswith("wt-"):
            mm = re.search(r"\(([\d.]+) / ([\d.]+) CPUs\) B/A=([\d.]+)", rest)
            if mm:
                add(v, r, "wt", test, a=num(mm.group(1)), b=num(mm.group(2)), ratio=num(mm.group(3)))
        elif test == "starve":
            cpu = re.search(r"cpu=(\d+)us", rest)
            add(v, r, "starve", "besteffort", stall=num(k.get("max_stall_ms")),
                cpu_ms=num(cpu.group(1)) / 1000 if cpu else None, state=None)
        elif test.startswith(("ly", "flag", "gc")):
            cell = f"{test} @ limit {k.get('limit')}"
            add(v, r, "lk", cell, cpus=num(k.get("cpus")),
                ops=num(k.get("events_s") or k.get("ops_per_s")),
                gc_p50=num(k.get("pause_p50_ms")), gc_p99=num(k.get("pause_p99_ms")))
        elif test.startswith(("hb-", "pipe")):
            add(v, r, "unl", test, time=num(k.get("time")))
        elif test == "barrier6":
            add(v, r, "unl", test, ops=num(k.get("ops_per_s")))
        elif test.startswith("schbench"):
            add(v, r, "unl", test, wake_p99=num(k.get("wake_p99_us")), req_p50=num(k.get("req_p50_us")),
                req_p99=num(k.get("req_p99_us")), rps=num(k.get("rps")))


# kit result CSVs (in-tree kit, no trace_cpus column)
def kit_rows(pattern, path_label):
    for d in sorted(glob.glob(pattern)):
        mm = re.search(r"-(cfs|kwidth|scx|kpfs)-(\d+)$", d)
        if not mm or not os.path.exists(os.path.join(d, "results.csv")):
            continue
        v, r = mm.groups()
        for c in csv.DictReader(open(os.path.join(d, "results.csv"))):
            if c["p50_ms"] not in ("-", ""):
                add(v, r, path_label, f"reqlat limit {c['limit']}", p50=num(c["p50_ms"]), p99=num(c["p99_ms"]))
            else:
                add(v, r, path_label, f"limit {c['limit']} x {c['threads']} {c['neighbours']}", thr=num(c["miter_s"]),
                    stall=num(c["worst_stall_ms"]), conc=num(c["avg_conc"]), maxconc=num(c["max_conc"]),
                    over=num(c["pct_over_cap"]), throttled=num(c["nr_throttled"]))


kit_rows(os.path.join(RES, "kit-cg-*"), "kit-cg")
kit_rows(os.path.join(RES, "kit-dock-*"), "kit-dock")
kit_rows(os.path.join(RES, "sysvm", "sd-*"), "kit-systemd")
kit_rows(os.path.join(RES, "sysvm", "dk-*"), "kit-docker-systemd")
rows = [x for x in rows if x["round"] > 0]

# metric direction: +1 higher is better, -1 lower is better
DIR = dict(thr=1, stall=-1, p50=-1, p99=-1, ops=1, gc_p50=-1, gc_p99=-1, time=-1, wake_p99=-1, req_p50=-1,
           req_p99=-1, rps=1, cpu_ms=1)
agg = {}
for x in rows:
    for met, val in x.items():
        if met in ("variant", "round", "test", "cell"):
            continue
        agg.setdefault((x["test"], x["cell"], met), {}).setdefault(x["variant"], []).append(val)

summary = []
for (test, cell, met), byv in agg.items():
    e = dict(test=test, cell=cell, metric=met, n={}, median={}, min={}, max={})
    for v in VARS:
        vals = byv.get(v)
        if not vals:
            continue
        e["n"][v] = len(vals)
        e["median"][v] = round(statistics.median(vals), 4)
        e["min"][v] = min(vals)
        e["max"][v] = max(vals)
    if met == "ratio":  # weights: closest to 3.0 (the requests ratio; the 4/6 caps allow 2:6)
        e["winner"] = min(e["median"], key=lambda v: abs(e["median"][v] - 3.0))
    elif met in DIR and e["median"]:
        e["winner"] = (max if DIR[met] > 0 else min)(e["median"], key=lambda v: e["median"][v])
    c = e["median"].get("cfs")
    if c:
        e["vs_cfs"] = {v: round(e["median"][v] / c, 3) for v in e["median"]}
        if met in DIR:
            # regression: worse than CFS by >5% at the median and the ranges don't overlap
            reg = []
            for v in e["median"]:
                if v == "cfs":
                    continue
                r = e["median"][v] / c
                worse = r < 0.95 if DIR[met] > 0 else r > 1.05
                sep = (e["max"][v] < e["min"]["cfs"]) if DIR[met] > 0 else (e["min"][v] > e["max"]["cfs"])
                if worse:
                    reg.append(dict(variant=v, ratio=round(r, 3), separated=sep))
            if reg:
                e["regress"] = reg
    summary.append(e)

json.dump(dict(rows=rows, summary=summary), open(OUT, "w"), indent=1)
print(f"{len(rows)} rows, {len(summary)} metrics -> {OUT}")
