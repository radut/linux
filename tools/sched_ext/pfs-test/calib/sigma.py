#!/usr/bin/env python3
"""sigma.py RESDIR : the paired ratio's sigma per cell, and the boot count it implies.

Reads the per-boot files written by run-calib.sh. Lines are keyed to an arm by the "D arm=" marker
that precedes them, because both sub-harnesses infer and print a variant of their own which is wrong
for an scx arm on a sched_pfs=0 boot.

Pairing: F1 scx3/cfs and F3 kwidth/cfs are within one boot. kPFS has no in-boot CFS - it is
early_param only - so it is paired with the F1 cfs run of the same round, which is stated in the
output rather than hidden.
"""
import glob, os, re, statistics, sys

resdir = sys.argv[1]
# rows[(round, arm)][cell_key] = value
rows = {}

def put(rnd, arm, key, val):
    rows.setdefault((rnd, arm), {})[key] = val

for path in sorted(glob.glob(os.path.join(resdir, "r*-f*.txt"))):
    m = re.match(r"r(\d+)-f(\d)", os.path.basename(path))
    if not m:
        continue
    rnd = int(m.group(1))
    arm = None
    for line in open(path, errors="replace"):
        d = re.search(r"^D arm=(\S+)", line)
        if d:
            arm = d.group(1)
            continue
        if arm is None:
            continue
        k = re.search(r"^R test=kheavy2 .*load=(\S+) limit=(\S+) trace_cpus=(\S+) cpu_stat_rate=(\S+)", line)
        if k:
            load, lim, tr, cs = k.groups()
            put(rnd, arm, f"kheavy/{load}@{lim}/cpu_stat", float(cs))
            put(rnd, arm, f"kheavy/{load}@{lim}/trace", float(tr))
            continue
        g = re.search(r"^R v=\S+ lim=(\S+) wl=(\S+) rep=(\S+) tps=(\S+) lat_ms=(\S+) cpus=(\S+)", line)
        if g:
            lim, wl, rep, tps, lat, cpus = g.groups()
            try:
                for name, v in (("tps", tps), ("lat_ms", lat), ("cpus", cpus)):
                    put(rnd, arm, f"pg/{wl}@{lim}/{name}", float(v))
            except ValueError:
                pass

if not rows:
    print("no rows parsed - is the run still going?")
    sys.exit(0)

rounds = sorted({r for r, _ in rows})
arms = sorted({a for _, a in rows})
print(f"rounds seen: {rounds}   arms: {arms}\n")

# boots needed for 80% power at a two-sided 0.05, from the simulated table in the plan
NEEDED = [(1.0, 3), (2.0, 4), (3.0, 6), (5.0, 10), (8.0, 25)]
def boots_for(sig):
    for s, n in NEEDED:
        if sig <= s:
            return n
    return ">25"

PAIRS = [("scx3", "cfs", "in-boot"), ("kwidth", "cfs", "in-boot"), ("kpfs", "cfs", "boot-adjacent")]
cells = sorted({k for v in rows.values() for k in v})
print(f"{'cell':<34} {'arm':<7} {'n':>2}  {'mean ratio':>10} {'sigma %':>8}  {'boots for 5%':>12}  pairing")
for cell in cells:
    for arm, base, how in PAIRS:
        ratios = []
        for r in rounds:
            a = rows.get((r, arm), {}).get(cell)
            b = rows.get((r, base), {}).get(cell)
            if a is not None and b not in (None, 0):
                ratios.append(a / b)
        if len(ratios) < 2:
            continue
        mu = statistics.mean(ratios)
        sd = statistics.stdev(ratios)
        sig = sd / mu * 100 if mu else float("nan")
        print(f"{cell:<34} {arm:<7} {len(ratios):>2}  {mu:>10.4f} {sig:>8.2f}  {str(boots_for(sig)):>12}  {how}")
