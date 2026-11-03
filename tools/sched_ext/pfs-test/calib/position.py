#!/usr/bin/env python3
"""position.py RESDIR : the within-boot position effect, reported as its own measurement.

The arm order rotates with round parity, so over an even number of rounds every arm of an
in-boot pair sits in position 1 and position 2 equally often. This prints, per arm and per
cell, the tps of each position and their ratio, so the effect is a number in its own right
rather than something hidden inside the paired ratios.

The arm label comes from the 'D arm=' line. The 'v=' field on an R row is hardcoded by
pg-guest.sh and is always 'cfs', whatever arm is running - it is not an arm label.
"""
import os, re, sys, statistics as st

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exclude import boot_id   # the one definition of "which files are boots, and whose"

res = sys.argv[1]
rows = []  # (round, family, position, arm, cell, metric) -> value
for fn in sorted(os.listdir(res)):
    ident = boot_id(fn)
    if ident is None:
        continue
    rnd, fam = ident
    arm, clients, pos, seen = None, None, 0, set()
    for line in open(os.path.join(res, fn), errors="replace"):
        line = line.replace("\r", "")
        d = re.search(r"^D arm=(\S+)", line)
        if d:
            arm = d.group(1)
            if arm not in seen:
                seen.add(arm); pos += 1
            c = re.search(r"cell=pg clients=(\d+)", line)
            clients = int(c.group(1)) if c else None
            continue
        g = re.search(r"^R v=\S+ lim=(\S+) wl=(\S+) rep=\S+ tps=(\S+) lat_ms=\S+ cpus=(\S+) mid_cpus=(\S+)", line)
        if g and arm and clients:
            lim, wl, tps, cpus, mid = g.groups()
            cell = f"{wl}@{lim} c{clients}"
            rows.append((rnd, fam, pos, arm, cell, float(tps), float(cpus), float(mid)))

cells = sorted({r[4] for r in rows})
arms = sorted({(r[1], r[3]) for r in rows})
print(f"{'fam/arm':<14} {'cell':<16} {'pos1 tps':>22} {'pos2 tps':>22} {'ratio':>7} {'n':>3}")
for fam, arm in arms:
    for cell in cells:
        p1 = [r[5] for r in rows if r[1] == fam and r[3] == arm and r[4] == cell and r[2] == 1]
        p2 = [r[5] for r in rows if r[1] == fam and r[3] == arm and r[4] == cell and r[2] == 2]
        if not p1 or not p2:
            continue
        m1, m2 = st.mean(p1), st.mean(p2)
        v1 = ",".join(f"{x:.0f}" for x in p1); v2 = ",".join(f"{x:.0f}" for x in p2)
        print(f"{fam+'/'+arm:<14} {cell:<16} {v1:>22} {v2:>22} {m2/m1:>7.2f} {min(len(p1),len(p2)):>3}")

print()
print("delivered CPUs at limit 4 (cpus / mid_cpus), by position")
print(f"{'fam/arm':<14} {'cell':<16} {'pos1':>26} {'pos2':>26}")
for fam, arm in arms:
    for cell in [c for c in cells if c.endswith("@4 c16") or c.endswith("@4 c32")]:
        for p in (1, 2):
            pass
        s = {}
        for p in (1, 2):
            v = [(r[6], r[7]) for r in rows if r[1] == fam and r[3] == arm and r[4] == cell and r[2] == p]
            s[p] = " ".join(f"{a:.2f}/{b:.2f}" for a, b in v)
        if s[1] and s[2]:
            print(f"{fam+'/'+arm:<14} {cell:<16} {s[1]:>26} {s[2]:>26}")
