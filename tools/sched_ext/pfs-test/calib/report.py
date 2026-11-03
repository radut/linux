#!/usr/bin/env python3
"""report.py RESDIR : sigma per cell, exact boots for 5%, and what a 21- or 36-boot budget resolves.

Boot counts come from boots.py (simulated one-sample t), not from the coarse lookup sigma.py used.
MDD = the smallest difference the cell can call at 80% power with that many boots.
"""
import glob, importlib.util, math, os, re, statistics, sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from exclude import boot_id   # the one definition of "which files are boots, and whose"
spec = importlib.util.spec_from_file_location("boots", os.path.join(here, "boots.py"))
B = importlib.util.module_from_spec(spec)
spec.loader.exec_module(B)

resdir = sys.argv[1]
BUDGETS = [int(x) for x in (sys.argv[2:] or ["21", "36"])]

rows = {}
def put(rnd, arm, key, val):
    rows.setdefault((rnd, arm), {})[key] = val

# Every boot file, not only r*-f*: the old glob meant a res-mech or res-mechrev directory
# produced an empty table rather than a result. The round is the one boot_id() reports.
for path in sorted(glob.glob(os.path.join(resdir, "*.txt"))):
    ident = boot_id(os.path.basename(path))
    if ident is None:
        continue
    rnd, arm = ident[0], None
    for line in open(path, errors="replace"):
        d = re.search(r"^D arm=(\S+)", line)
        if d:
            arm = d.group(1); continue
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

rounds = sorted({r for r, _ in rows})
# Appended to, never reordered or removed: this is a pre-registered analysis surface.
# ("scx3b", "scx3") is the within-boot contrast of two scx_pfs builds, from a boot run with
# ARMS_M2="scx3 scx3b".
PAIRS = [("scx3", "cfs", "in-boot"), ("kwidth", "cfs", "in-boot"), ("kpfs", "cfs", "boot-adjacent"),
         ("scx3b", "scx3", "in-boot")]
cells = sorted({k for v in rows.values() for k in v})

def mdd(n, sigma, want=0.80):
    lo, hi = 0.0, 200.0
    for _ in range(22):
        mid = 0.5 * (lo + hi)
        if B.power(n, sigma, mid, trials=6000) >= want:
            hi = mid
        else:
            lo = mid
    return 0.5 * (lo + hi)

hdr = f"{'cell':<30} {'arm':<7} {'ratio':>7} {'sigma%':>7} {'n@5%':>5}"
for b in BUDGETS:
    hdr += f" {'mdd@' + str(b):>8}"
print(f"rounds: {rounds}\n")
print(hdr)
for cell in cells:
    for arm, base, how in PAIRS:
        rs = []
        for r in rounds:
            a = rows.get((r, arm), {}).get(cell)
            b = rows.get((r, base), {}).get(cell)
            if a is not None and b not in (None, 0):
                rs.append(a / b)
        if len(rs) < 2:
            continue
        mu, sd = statistics.mean(rs), statistics.stdev(rs)
        sig = sd / mu * 100 if mu else float("nan")
        n5 = B.boots(sig, 5.0)
        line = f"{cell:<30} {arm:<7} {mu:>7.3f} {sig:>7.2f} {str(n5):>5}"
        for bud in BUDGETS:
            line += f" {mdd(bud, sig):>7.1f}%"
        print(line)
