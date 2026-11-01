"""Merge one ops batch into a timeline: events.tsv (node clock), nodemon.jsonl (state changes,
per-second cgroup throttling), spin-loop JSON lines (1 s stalls) and k6/pgbench chunk lines.
For every event: time to the next sched_ext state change (fallback) and to 'enabled' again
(recovery), plus what the limited pods saw between the event and recovery + 10 s.

    python3 -I timeline.py OUTDIR
"""
import glob
import json
import os
import re
import sys

d = sys.argv[1]


def jl(path, prefix=""):
    rows = []
    for line in open(path, errors="replace"):
        i = line.find(prefix + "{")
        if i < 0:
            continue
        try:
            rows.append(json.loads(line[i + len(prefix):]))
        except ValueError:
            pass
    return rows


events = []
if os.path.exists(os.path.join(d, "events.tsv")):
    for line in open(os.path.join(d, "events.tsv")):
        t, _, name = line.rstrip("\n").partition("\t")
        events.append((float(t), name))

mon = jl(os.path.join(d, "nodemon.jsonl")) if os.path.exists(os.path.join(d, "nodemon.jsonl")) else []
states = [(r["t"], r["state"], r.get("ops")) for r in mon if r.get("ev") == "state"]
ticks = [r for r in mon if r.get("ev") == "tick"]

spins = {}
for f in glob.glob(os.path.join(d, "ops-spin-*.log")):
    if f.endswith(".prev.log"):
        continue
    rows = [r for r in jl(f) if "ts" in r and not r.get("final")]
    if rows:
        spins[os.path.basename(f).rsplit("-", 2)[0]] = rows

k6 = []
for f in glob.glob(os.path.join(d, "ops-k6-*.log")):
    k6 += jl(f, "K6 ")

print(f"# {d}")
print("## sched_ext state changes (node clock)")
for t, s, o in states:
    print(f"  {t:.3f} {s} {o or ''}")

print("## events")
for t, name in events:
    after = [(st, s) for st, s, _ in states if st > t]
    first = after[0] if after else None
    back = next(((st, s) for st, s in after if s == "enabled"), None)
    end = (back[0] if back else t + 30) + 10
    line = f"  {t:.3f} {name}:"
    if first:
        line += f" first state change +{(first[0] - t) * 1000:.0f} ms -> {first[1]};"
    if back:
        line += f" enabled again +{back[0] - t:.2f} s;"
    for name_, rows in spins.items():
        w = [r for r in rows if t - 1 <= r["ts"] <= end]
        if w:
            line += (f" {name_}: max_stall {max(r['max_stall_ms'] for r in w):.1f} ms,"
                     f" cpus {min(r['cpu_s'] / r['interval_s'] for r in w):.2f}..{max(r['cpu_s'] / r['interval_s'] for r in w):.2f};")
    tw = [r for r in ticks if t - 1 <= r["t"] <= end]
    if len(tw) >= 2:
        for cg in tw[-1]["cg"]:
            if cg in tw[0]["cg"]:
                a, b = tw[0]["cg"][cg], tw[-1]["cg"][cg]
                if b[1] - a[1]:
                    line += f" cg {cg}: throttled {b[1] - a[1]}x {(b[2] - a[2]) / 1000:.0f} ms;"
    print(line)

print("## per-spin-pod 1 s samples with stalls > 50 ms or throttling")
for name_, rows in spins.items():
    for r in rows:
        if r["max_stall_ms"] > 50:
            print(f"  {r['ts']:.1f} {name_} stall {r['max_stall_ms']:.1f} ms cpus {r['cpu_s'] / r['interval_s']:.2f}")

print("## k6 chunks")
for r in k6:
    print("  " + json.dumps(r))
errs = 0
for f in glob.glob(os.path.join(d, "ops-k6-*.log")):
    errs += sum(1 for line in open(f, errors="replace") if re.search("level=error", line))
print(f"  k6 error lines: {errs}")
