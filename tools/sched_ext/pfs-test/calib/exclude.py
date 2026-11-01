#!/usr/bin/env python3
"""exclude.py RESDIR [RESDIR...] : the boot exclusion rule, applied mechanically.

THE RULE, fixed 2026-10-31. A boot is excluded whole, with every arm in it, if EITHER test fires:

  (A) DELIVERY. Any `cfs` arm run in the boot delivers mid_cpus < 0.90 x its cgroup limit on any
      Postgres cell while thr_frac < 0.01 - a cgroup taking far less than its limit without being
      throttled. Restricted to `cfs` arms on purpose: cfs is the reference and the only arm known
      to be able to take its limit. kwidth's chronic under-delivery (2.28-3.11 CPUs at limit 4) is
      the result being measured, not a disturbance, and kpfs/scx3 are the arms under test, so
      applying (A) to them would exclude boots for the very behaviour the run exists to measure.

  (B) LOAD. Any field of the boot's recorded load_before or load_after (1-, 5- or 15-minute) is
      >= 8.0. load_before alone is not sufficient: the worst boot observed had load_before
      "1.08 3.81 4.09" and was still destroyed, which is why (A) is the primary test and (B) the
      auxiliary one.

mid_cpus, not cpus, because the measured-interval discipline is the midpoint-to-midpoint delta of
cpu.stat usage_usec.

PARSER DEFECT, found and fixed 2026-10-31 after s1-s3 landed. (B) reads the load record from the
per-boot .txt file, where run-calib.sh writes it as a `D boot=` line. seqtest.sh never wrote that
line - it printed the load only to run.log - so for s1-s3 the load list was EMPTY and (B) could not
fire at all, silently passing a boot whose load_after read "7.52 24.40 24.24". The rule is unchanged;
the parser now also reads each result dir's run.log, keyed by boot name, and a boot with no load
record anywhere is reported as UNEVALUABLE rather than kept, so missing data can never read as a
pass again.

HONESTY NOTE: rounds 1-3 of res-r4b had already been inspected when this rule was written, so for
those three rounds it is retrospective, not blind. It is blind for rounds 4-9 and for s1-s3. The
rule keys on a mechanism (under-delivery without throttling) rather than on the arm contrast the
run is measuring, and it is applied to every boot without exception.
"""
import os, re, sys

THRESH_FRAC = 0.90
THRESH_THR = 0.01
THRESH_LOAD = 8.0

def runlog_loads(res):
    """boot name -> loadavg fields, from a result dir's run.log.

    Covers boots whose .txt has no `D boot=` line (seqtest.sh writes none). Both formats are
    matched: run-calib.sh prints `arms_done=2/warm=1`, seqtest.sh `arms_done=2 warm=1`, so the
    name is taken from the start of the line and the load fields by their own labels.
    """
    out = {}
    try:
        fh = open(os.path.join(res, "run.log"), errors="replace")
    except OSError:
        return out
    for line in fh:
        m = re.match(r"^(\S+)\s+rc=", line)
        if not m:
            continue
        vals = []
        for fld in re.findall(r'load_(?:before|after)="([^"]*)"', line):
            vals += [float(x) for x in fld.split()]
        if vals:
            out.setdefault(m.group(1), []).extend(vals)
    return out


def check(path, extra_loads):
    arm, reasons = None, []
    loads = []
    for line in open(path, errors="replace"):
        line = line.replace("\r", "")
        d = re.search(r"^D arm=(\S+)", line)
        if d:
            arm = d.group(1)
            continue
        for fld in re.findall(r'load_(?:before|after)="([^"]*)"', line):
            loads += [float(x) for x in fld.split()]
        g = re.search(r"^R v=\S+ lim=(\S+) wl=(\S+) rep=\S+ tps=\S+ lat_ms=\S+ cpus=\S+ "
                      r"mid_cpus=(\S+)", line)
        if g and arm == "cfs":
            lim, wl, mid = float(g.group(1)), g.group(2), float(g.group(3))
            t = re.search(r"\bthr_frac=(\S+)", line)
            thr = float(t.group(1)) if t else 0.0
            if mid < THRESH_FRAC * lim and thr < THRESH_THR:
                reasons.append(f"A: cfs {wl}@{lim:g} took {mid:.3f} of {lim:g} "
                               f"({mid/lim:.3f}) at thr_frac={thr:.3f}")
    loads += extra_loads
    if not loads:
        reasons.append("UNEVALUABLE: no load record in the boot file or run.log, so (B) "
                       "cannot be tested - treated as a drop, never as a pass")
    elif max(loads) >= THRESH_LOAD:
        reasons.append(f"B: load {max(loads):.2f} >= {THRESH_LOAD}")
    return reasons

def main(argv):
  rows = []
  for res in argv:
    if not os.path.isdir(res):
        continue
    rl = runlog_loads(res)
    for fn in sorted(os.listdir(res)):
        if not re.match(r"^(r\d+-f\d|s\d-[a-z0-9-]+)\.txt$", fn):
            continue
        name = fn[:-4]
        rows.append((os.path.basename(res), name,
                     check(os.path.join(res, fn), rl.get(name, []))))

  kept = [r for r in rows if not r[2]]
  drop = [r for r in rows if r[2]]
  print(f"rule: (A) cfs mid_cpus < {THRESH_FRAC} x limit while thr_frac < {THRESH_THR}"
        f"  (B) any recorded loadavg field >= {THRESH_LOAD}")
  print(f"boots seen {len(rows)}  kept {len(kept)}  dropped {len(drop)}\n")
  for res, name, reasons in drop:
      print(f"DROP {res}/{name}")
      for r in reasons:
          print(f"       {r}")
  print()
  print("KEEP " + " ".join(f"{res}/{name}" for res, name, _ in kept))


# Importable: final.py reuses check() and runlog_loads() for the keep set, so the report
# only runs when this file is the program.
if __name__ == "__main__":
    main(sys.argv[1:])
