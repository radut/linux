#!/usr/bin/env python3
"""slots.py RESDIR [ARM...] : where kPFS refuses a slot, ATTRIBUTED TO A LIMIT.

The kernel's per-slot refusal counters (pfs.c: refuse_budget_frac, refuse_debt_guar,
refuse_debt_frac, buckets _0.._7 and _hi) are cumulative and machine-wide. mech-pg.sh
takes their DELTA per cell and appends it to the cell's R row, which is the only thing
that says which LIMIT a refusal happened under: the single dump mech.sh takes at warm-up
covers one limit and is not comparable with the per-cell scx numbers.

Printed per arm: one block per reason, rows = limit, columns = slot index. A refusal in
bucket i means the (i+1)'th concurrent slot of the group was refused, so the boundary
between the guaranteed slots and the fractional one is visible directly - at limit 3.5 the
guaranteed slots are 0,1,2 and the fraction is slot 3.

WHAT THIS REFUSES TO DO. An empty table is not printed as zeros. A boot file whose R rows
carry no refuse_keys= field was produced by a harness that did not take the counters;
refuse_keys=0 on every cell means the KERNEL has none. Both exit non-zero and say which,
because a missing counter that renders as 0 is indistinguishable from a measured zero and
has already cost this work a day.
"""
import os, re, sys, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exclude import boot_id   # the one definition of "which files are boots, and whose"

REASONS = ("refuse_budget_frac", "refuse_debt_guar", "refuse_debt_frac")
BUCKETS = [str(i) for i in range(8)] + ["hi"]
# park_budget counts the same event as the refuse_budget_frac buckets, park_debt the two
# refuse_debt_* ones. Summing each way is the integrity check that a zero is a real zero.
AGG = {"refuse_budget_frac": ("park_budget",), "refuse_debt_guar": ("park_debt",),
       "refuse_debt_frac": ("park_debt",)}

res = sys.argv[1]
want = set(sys.argv[2:])

# (arm, limit, reason, bucket) -> refusals;  (arm, limit) -> {aggregate: count}
cnt = collections.Counter()
agg = collections.defaultdict(collections.Counter)
cells = collections.Counter()           # (arm, limit) -> number of cells
keyed = instrumented = nobase = 0       # cells with the field / with >0 keys / baseline misses
boots = 0

for fn in sorted(os.listdir(res)):
    if boot_id(fn) is None:
        continue
    boots += 1
    arm = None
    for line in open(os.path.join(res, fn), errors="replace"):
        line = line.replace("\r", "")
        d = re.search(r"^D arm=(\S+)", line)
        if d:
            arm = d.group(1)
            continue
        g = re.match(r"^R v=\S+ lim=(\S+) ", line)
        if not g or not arm or (want and arm not in want):
            continue
        lim = g.group(1)
        kv = dict(re.findall(r"(\w+)=(-?\d+)(?=\s|$)", line))
        if "refuse_keys" not in kv:
            continue
        keyed += 1
        nobase += int(kv.get("refuse_nobase", 0))
        if int(kv["refuse_keys"]) > 0:
            instrumented += 1
        cells[(arm, lim)] += 1
        for r in REASONS:
            for b in BUCKETS:
                v = kv.get(f"{r}_{b}")
                if v is not None:
                    cnt[(arm, lim, r, b)] += int(v)
        for a in ("park_budget", "park_debt", "admit"):
            if a in kv:
                agg[(arm, lim)][a] += int(kv[a])

if not boots:
    sys.exit(f"slots.py: {res} holds no boot files that exclude.boot_id recognises")
if not keyed:
    sys.exit(f"slots.py: {boots} boot(s), but no R row carries refuse_keys=; "
             "these were measured by a harness that did not take the per-slot counters, "
             "so there is nothing to attribute - remeasure, do not read this as zero")
if not instrumented:
    sys.exit(f"slots.py: {keyed} cell(s) all report refuse_keys=0; the kernel that ran them "
             "has no per-slot refusal counters (needs the slot-stats kernel), "
             "so there is nothing to attribute - do not read this as zero")

print(f"boots {boots}   cells with counters {instrumented}/{keyed}"
      + (f"   WARNING: {nobase} counter(s) appeared without a baseline snapshot" if nobase else ""))
if instrumented != keyed:
    print(f"   NOTE: {keyed - instrumented} cell(s) report refuse_keys=0 and are counted as "
          "uninstrumented, not as zero")

def limkey(l):
    try:
        return (0, float(l))
    except ValueError:
        return (1, 0.0)

for arm in sorted({k[0] for k in cells}):
    lims = sorted({l for a, l in cells if a == arm}, key=limkey)
    print(f"\n=== arm {arm} ===")
    for r in REASONS:
        if not any(cnt[(arm, l, r, b)] for l in lims for b in BUCKETS):
            print(f"\n-- {r}: zero in every bucket at every limit "
                  f"(measured: {sum(cells[(arm, l)] for l in lims)} cells) --")
            continue
        print(f"\n-- {r} --")
        print(f"{'limit':>6} {'cells':>5} " + " ".join(f"{('slot ' + b):>11}" for b in BUCKETS)
              + f" {'total':>13} {'aggregate':>13}")
        for l in lims:
            row = [cnt[(arm, l, r, b)] for b in BUCKETS]
            tot = sum(row)
            a = " ".join(f"{k}={agg[(arm, l)][k]}" for k in AGG[r])
            print(f"{l:>6} {cells[(arm, l)]:>5} " + " ".join(f"{v:>11,}" for v in row)
                  + f" {tot:>13,} {a:>13}")
    # The identity that makes a zero trustworthy: the two debt buckets must sum to park_debt.
    for l in lims:
        debt = sum(cnt[(arm, l, r, b)] for r in ("refuse_debt_guar", "refuse_debt_frac")
                   for b in BUCKETS)
        pd = agg[(arm, l)].get("park_debt")
        if pd is not None and debt != pd:
            print(f"   MISMATCH at limit {l}: refuse_debt_* sum to {debt:,} but park_debt is "
                  f"{pd:,}; one of the two is not counting what it says")
