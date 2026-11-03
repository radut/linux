#!/usr/bin/env python3
"""delivery.py RESDIR : does each variant actually deliver the limit it was given, and at
what cost in throttling - per arm, per cell, over every boot in RESDIR.

This is the reduction that needs NO position balance. Delivered CPU is an absolute
measurement of one arm against its own cgroup limit, not a contrast between two arms, so
it is valid in a run of any number of arms - including the three-arm runs that final.py
correctly refuses to contrast (the middle arm is confounded with position 2; see
mech.sh's ALLOW_UNBALANCED). Those runs are not wasted, they are delivery runs.

Four columns earn their place:

  deliv%    mid_cpus / limit. Below 100% the variant is not using the CPU it was allowed.
            mid_cpus is the mid-window cpu.stat DELTA, not a cumulative reading.
  thr/s     cpu.stat throttle events per second. The period is 100 ms, so 10/s is "once
            per period" - stock bandwidth control's normal state. Much above that means
            the group is being stopped and restarted many times inside one period.
  thr%      fraction of the cell spent throttled, from the same counters.
  tps/CPU   work per delivered CPU. This is the comparison that is not confounded by a
            variant failing to take its limit: a variant can lose throughput either by
            not getting CPU or by getting it and doing less with it, and these two
            columns separate those.

WHAT IT REFUSES TO DO. Under a sched_ext scheduler the fair class is not what runs the
tasks, so cpu.stat's throttling fields are never incremented - measured, exactly zero on
every scx cell of every run. The field is still PRESENT, because mech-pg.sh reads cpu.stat
either way and gets a 0, so "the field is missing" is the wrong test and would print 0.0
here. A 0.0 in this column reads as "never throttled" when it means "this is not where
this scheduler accounts throttling", and that is the misreading the column exists to
prevent. The scx arms therefore print n/a from a structural rule, named in NO_CPU_STAT_THR
below, and the footnote says why. Any other arm whose cells are all exactly zero is
flagged too, so a dead counter cannot pass as a measured zero. An empty result set exits
non-zero rather than printing a table of zeros.
"""
import os
import re
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exclude import boot_id   # the one definition of "which files are boots, and whose"

# Arms whose throttling is not accounted in cpu.stat, so that a zero there says nothing. These are
# the sched_ext arms: loading a BPF scheduler moves the tasks out of the fair class, and the CFS
# bandwidth counters belong to the fair class. Their throttling signal is the loader's own
# counters - scx_pfs -i for the totals, -S FILE per cgroup (see VARIANTS.md).
NO_CPU_STAT_THR = ("scx3", "scx3b")

# mech-pg.sh writes thr_* from the group's own cpu.stat and c_thr_* from the container leaf.
# thr_n is a count of throttle events over thr_win seconds; both are deltas over the cell.
R_ROW = re.compile(r"^R v=\S+ lim=(?P<lim>\S+) wl=(?P<wl>\S+) rep=\S+ tps=(?P<tps>\S+) "
                   r"lat_ms=(?P<lat>\S+) cpus=(?P<cpus>\S+) mid_cpus=(?P<mid>\S+)")
D_ARM = re.compile(r"^D boot kernel=\S+ arm=(\S+)")
FIELD = re.compile(r"(\w+)=([-\d.]+)")


def read(path):
    """-> [(arm, cell, limit, tps, mid_cpus, thr_n, thr_win, thr_frac, mean_running)]"""
    out, arm, clients = [], None, None
    for line in open(path, errors="replace"):
        line = line.replace("\r", "")
        d = D_ARM.match(line)
        if d:
            arm, clients = d.group(1), None
            continue
        c = re.search(r"^D arm=\S+ cell=pg clients=(\d+)", line)
        if c:
            clients = int(c.group(1))
            continue
        g = R_ROW.match(line)
        if not (g and arm and clients):
            continue
        f = dict(FIELD.findall(line))
        out.append((arm, f"{g['wl']}@{g['lim']} c{clients}", float(g["lim"]),
                    float(g["tps"]), float(g["mid"]),
                    f.get("thr_n"), f.get("thr_win"), f.get("thr_frac"),
                    f.get("mean_running")))
    return out


def fnum(v):
    return None if v is None else float(v)


def main(argv):
    if len(argv) < 2:
        sys.exit("usage: delivery.py RESDIR")
    res = argv[1]
    boots = sorted(f for f in os.listdir(res) if f.endswith(".txt") and boot_id(f))
    if not boots:
        sys.exit(f"delivery.py: {res} holds no boot files that exclude.boot_id recognises")

    rows = []
    for b in boots:
        rows += read(os.path.join(res, b))
    if not rows:
        sys.exit("delivery.py: no R rows in any boot file; refusing to print a table of zeros")

    # Which arms account throttling somewhere OTHER than cpu.stat. This is structural, not
    # empirical: a sched_ext scheduler takes the tasks out of the fair class, so the CFS bandwidth
    # counters cannot move, and mech-pg.sh still reads a 0 out of cpu.stat for them. Reporting that
    # 0 would say "never throttled" where the truth is "ask the loader, not cpu.stat".
    reports = {}
    for arm, _c, _l, _t, _m, thr_n, _w, _fr, _mr in rows:
        reports.setdefault(arm, arm not in NO_CPU_STAT_THR and thr_n is not None)
        if thr_n is not None and arm not in NO_CPU_STAT_THR:
            reports[arm] = True
    # An arm that DOES report but never moved the counter: flagged, not silently shown as 0.0.
    # A dead counter and a measured zero look identical in a table and must not read alike.
    allzero = set()
    for arm in reports:
        if not reports[arm]:
            continue
        vals = [x[5] for x in rows if x[0] == arm and x[5] is not None]
        if vals and not any(float(v) for v in vals):
            allzero.add(arm)

    by = {}
    for arm, cell, lim, tps, mid, thr_n, thr_win, thr_frac, mean_run in rows:
        by.setdefault((arm, cell), []).append(
            (lim, tps, mid, fnum(thr_n), fnum(thr_win), fnum(thr_frac), fnum(mean_run)))

    print(f"boots: {len(boots)}   cells: {len(by)}   R rows: {len(rows)}")
    print(f"\n{'arm':<8} {'cell':<17} {'lim':>4} {'CPU':>6} {'deliv%':>7} "
          f"{'thr/s':>7} {'thr%':>6} {'tps':>9} {'tps/CPU':>8} {'mean_run':>8} {'n':>3}")
    quiet = set()
    for arm, cell in sorted(by, key=lambda k: (k[0], k[1])):
        v = by[(arm, cell)]
        lim = v[0][0]
        mid = st.mean(x[2] for x in v)
        tps = st.mean(x[1] for x in v)
        mr = [x[6] for x in v if x[6] is not None]
        if reports[arm]:
            # events per second, not a raw count: cells of different lengths are compared.
            rate = st.mean(x[3] / x[4] for x in v if x[3] is not None and x[4])
            frac = st.mean(x[5] for x in v if x[5] is not None)
            thr_s, thr_p = f"{rate:.1f}", f"{frac:.2f}"
        else:
            quiet.add(arm)
            thr_s = thr_p = "n/a"
        print(f"{arm:<8} {cell:<17} {lim:>4.1f} {mid:>6.2f} {100 * mid / lim:>6.1f}% "
              f"{thr_s:>7} {thr_p:>6} {tps:>9.0f} {tps / mid:>8.0f} "
              f"{(f'{st.mean(mr):.2f}' if mr else '-'):>8} {len(v):>3}")

    # Per-arm summary over the cells, which is what answers "does this variant reach its limit".
    print(f"\n{'arm':<8} {'cells':>5} {'worst deliv%':>12} {'mean deliv%':>12} "
          f"{'cells < 95%':>11} {'mean thr/s':>10}")
    for arm in sorted({a for a, _ in by}):
        ds, rs = [], []
        for (a, _cell), v in by.items():
            if a != arm:
                continue
            lim = v[0][0]
            ds.append(100 * st.mean(x[2] for x in v) / lim)
            if reports[arm]:
                rs += [x[3] / x[4] for x in v if x[3] is not None and x[4]]
        under = sum(1 for d in ds if d < 95.0)
        print(f"{arm:<8} {len(ds):>5} {min(ds):>11.1f}% {st.mean(ds):>11.1f}% {under:>11} "
              f"{(f'{st.mean(rs):.1f}' if rs else 'n/a'):>10}")

    if quiet:
        print(f"\nthr/s and thr% are n/a for {', '.join(sorted(quiet))}: a sched_ext scheduler "
              f"takes the tasks out of\nthe fair class, so cpu.stat's throttling counters cannot "
              f"move. cpu.stat does hold a 0 for\nthem, and that 0 means \"not accounted here\", "
              f"not \"never throttled\" - read the loader's own\ncounters instead (scx_pfs -i, and "
              f"-S FILE per cgroup). Do not plot it as a zero.")
    if allzero:
        print(f"\nNOTE: {', '.join(sorted(allzero))} report the cpu.stat throttling fields and "
              f"never moved them on any\ncell of this run. Treat that as a result to check, not "
              f"as a confirmed zero: a counter that is\nnever incremented looks exactly like one "
              f"that had nothing to count.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
