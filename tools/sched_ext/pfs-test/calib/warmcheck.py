#!/usr/bin/env python3
"""warmcheck.py RESDIR : did the per-boot warm-up remove the first-run penalty?

Two checks, because they become available at different times.

1. Absolute, from round 1 alone. In the un-warmed calibration, CFS running FIRST in its boot read
   2702 (F1) / 2422 (F3) tps on tpcb@2 c16, and CFS running SECOND read 4670 / 4838. If the warm-up
   absorbed the cost, CFS in position 1 should now read near the position-2 numbers.
2. The same-arm position ratio, which needs both round parities and so lands with round 2. For each
   arm, mean tps in position 1 vs position 2 on tpcb@2 c16. Un-warmed this was 1.66-2.00; the
   target is the 1.01-1.19 band every later pg cell already showed.
"""
import glob, os, re, statistics, sys

resdir = sys.argv[1]
UNWARMED_POS1 = {("f1", "cfs"): 2702, ("f3", "cfs"): 2422, ("f1", "scx3"): 1949, ("f3", "kwidth"): 1379}
UNWARMED_POS2 = {("f1", "cfs"): 4670, ("f3", "cfs"): 4838, ("f1", "scx3"): 3245, ("f3", "kwidth"): 2498}

obs = {}     # (fam, arm, pos) -> [tps]
warm = []    # warm-up rows
for path in sorted(glob.glob(os.path.join(resdir, "r*-f*.txt"))):
    m = re.match(r"r(\d+)-(f\d)", os.path.basename(path))
    if not m:
        continue
    rnd, fam = int(m.group(1)), m.group(2)
    arm, cl, order, seen = None, "", [], set()
    for line in open(path, errors="replace"):
        w = re.search(r"^D warmup rc=(\S+) secs=(\S+) tps=(\S*)", line)
        if w:
            warm.append((f"r{rnd}-{fam}", w.group(1), w.group(2), w.group(3) or "none"))
            continue
        d = re.search(r"^D arm=(\S+)", line)
        if d:
            arm = d.group(1)
            if arm not in seen and "cleanup" not in line and "unloaded" not in line:
                seen.add(arm); order.append(arm)
            c = re.search(r"clients=(\d+)", line); cl = c.group(1) if c else ""
            continue
        if arm is None:
            continue
        g = re.search(r"lim=2 wl=tpcb rep=\S+ tps=(\S+)", line)
        if g and cl == "16":
            obs.setdefault((fam, arm, order.index(arm) + 1), []).append(float(g.group(1)))

print("warm-up runs:")
for name, rc, secs, tps in warm:
    print(f"  {name:<8} rc={rc} secs={secs:<4} its own tps={tps}")
if not warm:
    print("  none found - the warm-up did not run")

print("\ncheck 1: tpcb@2 c16 for the arm in position 1, vs the un-warmed position-1 and -2 values")
for (fam, arm, pos), v in sorted(obs.items()):
    if pos != 1:
        continue
    k = (fam, arm)
    if k not in UNWARMED_POS1:
        continue
    mu = statistics.mean(v)
    print(f"  {fam} {arm:<7} n={len(v)} now={mu:>7.0f}   un-warmed pos1={UNWARMED_POS1[k]:>5} "
          f"pos2={UNWARMED_POS2[k]:>5}   now/pos1={mu / UNWARMED_POS1[k]:.2f} now/pos2={mu / UNWARMED_POS2[k]:.2f}")

print("\ncheck 2: same arm, position 2 / position 1 (target 1.01-1.19, was 1.66-2.00)")
any2 = False
for fam in ("f1", "f3"):
    for arm in ("cfs", "scx3", "kwidth", "kpfs"):
        p1 = obs.get((fam, arm, 1)); p2 = obs.get((fam, arm, 2))
        if p1 and p2:
            any2 = True
            r = statistics.mean(p2) / statistics.mean(p1)
            verdict = "in band" if r <= 1.19 else ("still cold" if r >= 1.4 else "between")
            print(f"  {fam} {arm:<7} pos1={statistics.mean(p1):>7.0f} (n={len(p1)}) "
                  f"pos2={statistics.mean(p2):>7.0f} (n={len(p2)})  ratio={r:.2f}  {verdict}")
if not any2:
    print("  needs both round parities - available once round 2 lands")
