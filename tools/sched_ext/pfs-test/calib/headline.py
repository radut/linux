#!/usr/bin/env python3
"""headline.py RESDIR : the equal-CPU work ratio scx3/cfs at limit 4, three ways.

The question the peer raised is whether the headline has to drop position-1 CFS runs. That
depends on whether those runs took their allowance, so delivered CPU is printed next to every
ratio rather than assumed. "fit" is the two-factor (arm + position) contrast, which for a
balanced 2x2 layout is the difference of the arm means over both positions.
"""
import os, re, sys, statistics as st

res = sys.argv[1]
rows = []
for fn in sorted(os.listdir(res)):
    m = re.match(r"^r(\d+)-(f1)\.txt$", fn)
    if not m:
        continue
    rnd = int(m.group(1))
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
        g = re.search(r"^R v=\S+ lim=4 wl=(\S+) rep=\S+ tps=(\S+) lat_ms=\S+ cpus=(\S+) mid_cpus=(\S+)", line)
        if g and arm and clients:
            wl, tps, cpus, mid = g.groups()
            rows.append((rnd, pos, arm, f"{wl}@4 c{clients}", float(tps), float(cpus), float(mid)))

def sel(arm, cell, pos=None):
    return [r for r in rows if r[2] == arm and r[3] == cell and (pos is None or r[1] == pos)]

for cell in sorted({r[3] for r in rows}):
    print(f"\n{cell}")
    for label, pos in (("pos1 only", 1), ("pos2 only", 2), ("pooled", None)):
        c, s = sel("cfs", cell, pos), sel("scx3", cell, pos)
        if not c or not s:
            continue
        ct, sct = st.mean(x[4] for x in c), st.mean(x[4] for x in s)
        cc, scc = st.mean(x[5] for x in c), st.mean(x[5] for x in s)
        print(f"  {label:<10} cfs {ct:8.0f} tps @ {cc:.3f} cpu (n={len(c)})"
              f"   scx3 {sct:8.0f} tps @ {scc:.3f} cpu (n={len(s)})   scx3/cfs {sct/ct:.3f}")
    # two-factor: mean over positions of each arm, positions weighted equally
    r = {}
    for arm in ("cfs", "scx3"):
        per = [st.mean(x[4] for x in sel(arm, cell, p)) for p in (1, 2) if sel(arm, cell, p)]
        r[arm] = st.mean(per) if len(per) == 2 else None
    if r["cfs"] and r["scx3"]:
        print(f"  {'fit':<10} position-balanced arm means: cfs {r['cfs']:.0f}  scx3 {r['scx3']:.0f}"
              f"   scx3/cfs {r['scx3']/r['cfs']:.3f}")
