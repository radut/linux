#!/usr/bin/env python3
"""final.py [ROUNDS_DIR [SEQ_DIR]] : the result over the boots the pre-registered rule keeps.

With no argument it reads res-r4b and res-seq beside this file, exactly as before. Given one
directory it reads that one for both sections; given two, the second is the sequence-test
directory. Nothing about WHAT is computed depends on the argument - only which directories
are read - so pointing it at res-mech / res-mechrev / res-mech2 reports those boots through
the same code path, and the family pairing below still keys on the -f1/-f2/-f3 suffix, so a
directory whose boots are named otherwise yields empty family tables until that pairing is
extended. See HARNESS.md for the pre-registered correction the kPFS arm needs.

Three things, each as its own number:

  1. POSITION EFFECT per arm, pos2/pos1 of tps, per cell. The arm order rotates with round
     parity, so over the kept boots each arm of an in-boot pair appears in both positions.
  2. TWO-FACTOR (arm + position) CONTRAST per cell: each arm's mean is taken within a position
     first and the two positions are then weighted equally, so an unequal split of kept boots
     between positions cannot tilt the arm contrast. Delivered CPU is printed beside every
     ratio, because a ratio only means "work at equal CPU" if both arms took their allowance.
  3. PAIRED per-boot ratio with a one-sample t test on log ratio, the design the power
     calibration sized for.

Position is the n-th `D boot ... arm=` line in the boot file, which works whether the two arms
differ (the rounds) or are the same arm twice (the sequence test). The `v=` field on an R row is
hardcoded by pg-guest.sh and is always `cfs` whatever arm is running - it is not an arm label.

Boots are included only if exclude.py keeps them. Nothing else is filtered.
"""
import math, os, re, statistics as st, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exclude

HERE = os.path.dirname(os.path.abspath(__file__))
R4B, SEQ = os.path.join(HERE, "res-r4b"), os.path.join(HERE, "res-seq")


def kept(res):
    # A directory that is not there must say so. "no boots kept" and "I could not look" print
    # the same empty table otherwise, which is the failure the BOOT_FILE pattern already taught us.
    if not os.path.isdir(res):
        print(f"MISSING result directory: {res}", file=sys.stderr)
        return []
    rl = exclude.runlog_loads(res)
    out = []
    for fn in sorted(os.listdir(res)):
        if not exclude.BOOT_FILE.match(fn):
            continue
        name = fn[:-4]
        if not exclude.check(os.path.join(res, fn), rl.get(name, [])):
            out.append(name)
    return out


def round_key(name):
    """sort key for the kept-boots line: (family suffix, round number).

    The round number is the digits after the leading letter. It is parsed tolerantly because a
    phase-2 boot is named p1b-kpfs-r1, where that field is "1b" and a bare int() would raise.
    For the r<N>-f<N> names this returns exactly what the previous inline key returned.
    """
    head = name[1:].split("-")[0]
    m = re.match(r"\d+", head)
    return (name[-2:], int(m.group(0)) if m else 0)


def read(path):
    """-> [(pos, arm, cell, tps, cpus, mid_cpus)] for the Postgres cells."""
    rows, arm, clients, pos = [], None, None, 0
    for line in open(path, errors="replace"):
        line = line.replace("\r", "")
        b = re.match(r"^D boot kernel=\S+ arm=(\S+)", line)
        if b:
            arm, clients, pos = b.group(1), None, pos + 1
            continue
        c = re.search(r"^D arm=\S+ cell=pg clients=(\d+)", line)
        if c:
            clients = int(c.group(1))
            continue
        g = re.match(r"^R v=\S+ lim=(\S+) wl=(\S+) rep=\S+ tps=(\S+) lat_ms=\S+ "
                     r"cpus=(\S+) mid_cpus=(\S+)", line)
        if g and arm and clients:
            lim, wl, tps, cpus, mid = g.groups()
            rows.append((pos, arm, f"{wl}@{lim:>1} c{clients}",
                         float(tps), float(cpus), float(mid)))
    return rows


def load(res, names):
    data = {}
    for n in names:
        data[n] = read(os.path.join(res, f"{n}.txt"))
    return data


def ttest1(xs, mu=0.0):
    n = len(xs)
    if n < 2:
        return None, None
    m, s = st.mean(xs), st.stdev(xs)
    if s == 0:
        return float("inf"), 0.0
    t = (m - mu) / (s / math.sqrt(n))
    # two-sided p from the t distribution via the incomplete beta function
    df = n - 1
    x = df / (df + t * t)
    p = _betainc(df / 2.0, 0.5, x)
    return t, p


def _betainc(a, b, x):
    """Regularised incomplete beta I_x(a,b), continued fraction (Lentz)."""
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    lbeta = math.lgamma(a) + math.lgamma(b) - math.lgamma(a + b)
    front = math.exp(math.log(x) * a + math.log(1 - x) * b - lbeta) / a
    f, c, d = 1.0, 1.0, 0.0
    for i in range(0, 300):
        m = i // 2
        if i == 0:
            num = 1.0
        elif i % 2 == 0:
            num = (m * (b - m) * x) / ((a + 2 * m - 1) * (a + 2 * m))
        else:
            num = -((a + m) * (a + b + m) * x) / ((a + 2 * m) * (a + 2 * m + 1))
        d = 1.0 + num * d
        if abs(d) < 1e-30:
            d = 1e-30
        d = 1.0 / d
        c = 1.0 + num / c
        if abs(c) < 1e-30:
            c = 1e-30
        f *= c * d
        if abs(1.0 - c * d) < 1e-12:
            break
    r = front * (f - 1.0)
    return r if x < (a + 1) / (a + b + 2) else 1.0 - _betainc(b, a, 1 - x)


def positions(data, arm_of):
    """pos2/pos1 of tps, per arm per cell, over boots that ran that arm in each position."""
    by = {}
    for boot, rows in data.items():
        for pos, arm, cell, tps, cpus, mid in rows:
            by.setdefault((arm, cell), {}).setdefault(pos, []).append((boot, tps))
    print(f"{'arm':<8} {'cell':<16} {'pos1 mean':>10} {'n':>3} {'pos2 mean':>10} {'n':>3} "
          f"{'pos2/pos1':>10}")
    for (arm, cell) in sorted(by):
        s = by[(arm, cell)]
        if 1 not in s or 2 not in s:
            continue
        m1 = st.mean(t for _, t in s[1]); m2 = st.mean(t for _, t in s[2])
        print(f"{arm:<8} {cell:<16} {m1:>10.0f} {len(s[1]):>3} {m2:>10.0f} {len(s[2]):>3} "
              f"{m2/m1:>10.3f}")


def contrast(data, ref, test):
    cells = sorted({r[2] for rows in data.values() for r in rows})
    print(f"{'cell':<16} {'two-factor ' + test + '/' + ref:>22} {'cpu ' + ref:>9} "
          f"{'cpu ' + test:>10} {'paired':>8} {'n':>3} {'t':>7} {'p':>8}")
    for cell in cells:
        pick = lambda arm, pos: [r for b, rows in data.items() for r in rows
                                 if r[1] == arm and r[2] == cell and r[0] == pos]
        fit, cpu = {}, {}
        ok = True
        for arm in (ref, test):
            per, pcpu = [], []
            for pos in (1, 2):
                v = pick(arm, pos)
                if not v:
                    ok = False
                    continue
                per.append(st.mean(x[3] for x in v))
                pcpu.append(st.mean(x[5] for x in v))
            if not per:
                ok = False
                break
            fit[arm], cpu[arm] = st.mean(per), st.mean(pcpu)
        if not ok or ref not in fit or test not in fit:
            continue
        # paired per boot: both arms live in the same boot file
        ratios = []
        for boot, rows in data.items():
            a = [r[3] for r in rows if r[1] == ref and r[2] == cell]
            b = [r[3] for r in rows if r[1] == test and r[2] == cell]
            if a and b:
                ratios.append(st.mean(b) / st.mean(a))
        t, p = ttest1([math.log(x) for x in ratios], 0.0) if len(ratios) > 1 else (None, None)
        pr = st.mean(ratios) if ratios else float("nan")
        print(f"{cell:<16} {fit[test] / fit[ref]:>22.3f} {cpu[ref]:>9.3f} {cpu[test]:>10.3f} "
              f"{pr:>8.3f} {len(ratios):>3} "
              f"{(f'{t:.2f}' if t is not None else '-'):>7} "
              f"{(f'{p:.4f}' if p is not None else '-'):>8}")


def report(rounds_dir, seq_dir):
  k4 = kept(rounds_dir)
  ks = kept(seq_dir) if seq_dir != rounds_dir else k4
  fam = lambda f: {n: d for n, d in load(rounds_dir, [x for x in k4 if x.endswith(f)]).items()}

  print("kept boots")
  print(f"  {os.path.basename(rounds_dir):<7}  {len(k4):2d}  {' '.join(sorted(k4, key=round_key))}")
  if seq_dir != rounds_dir:
      print(f"  {os.path.basename(seq_dir):<7}  {len(ks):2d}  {' '.join(sorted(ks))}")

  for f, ref, test, label in (("f1", "cfs", "scx3", "F1  cfs vs scx_pfs, same boot"),
                              ("f3", "cfs", "kwidth", "F3  cfs vs kwidth, same boot")):
      d = fam(f)
      print(f"\n=== {label} ===  boots: {' '.join(sorted(d))}")
      print("\n-- position effect, pos2/pos1 of tps --")
      positions(d, None)
      print("\n-- arm contrast, position-balanced --")
      contrast(d, ref, test)

  d2 = fam("f2")
  print(f"\n=== F2  kpfs alone (boot-only, no in-boot cfs) ===  boots: {' '.join(sorted(d2))}")
  cells = sorted({r[2] for rows in d2.values() for r in rows})
  print(f"{'cell':<16} {'kpfs tps':>10} {'n':>3} {'cpu':>7}    per-boot tps")
  for cell in cells:
      v = [r for rows in d2.values() for r in rows if r[2] == cell]
      print(f"{cell:<16} {st.mean(x[3] for x in v):>10.0f} {len(v):>3} "
            f"{st.mean(x[5] for x in v):>7.3f}    " + ",".join(f"{x[3]:.0f}" for x in v))

  print("\n=== sequence test: same arm twice in one boot ===")
  for n in sorted(ks):
      rows = read(os.path.join(seq_dir, f"{n}.txt"))
      print(f"\n{n}")
      print(f"  {'cell':<16} {'run1 tps':>10} {'run2 tps':>10} {'run2/run1':>10} "
            f"{'cpu1':>7} {'cpu2':>7}")
      for cell in sorted({r[2] for r in rows}):
          a = [r for r in rows if r[2] == cell and r[0] == 1]
          b = [r for r in rows if r[2] == cell and r[0] == 2]
          if not a or not b:
              continue
          t1, t2 = st.mean(x[3] for x in a), st.mean(x[3] for x in b)
          print(f"  {cell:<16} {t1:>10.0f} {t2:>10.0f} {t2/t1:>10.3f} "
                f"{st.mean(x[5] for x in a):>7.3f} {st.mean(x[5] for x in b):>7.3f}")


if __name__ == "__main__":
    # Result directories on the command line, as exclude.py already takes them. The defaults
    # keep an argument-free run byte-identical to what it did before.
    a = sys.argv[1:]
    rounds = a[0] if a else R4B
    seq = a[1] if len(a) > 1 else (rounds if a else SEQ)
    report(rounds, seq)
