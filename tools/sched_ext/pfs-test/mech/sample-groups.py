#!/usr/bin/env python3
"""sample-groups.py OUT HZ SECS [PATHSUBSTR] : sample kPFS per-cgroup slots/waiting/tokens.

Reads /sys/kernel/debug/sched/pfs/groups, whose columns are
  limit_mcpu slots waiting tokens_us weight hweight active path   (pfs.c:3769)
and writes one line per sample for the cgroups whose path contains PATHSUBSTR.

Why this exists: the question is whether kPFS's missing CPU at limit 4 is work the scheduler
holds back or work the benchmark does not offer. slots < width while waiting > 0 is the first,
slots < width while waiting == 0 is the second. Nothing else distinguishes them.

The sampler's own CPU time is written to the trailer, so its cost is a measured number rather
than an assumption. It runs in the root cgroup, outside the limited pod, so it is not charged
to the cgroup under test.
"""
import os, resource, sys, time

out, hz, secs = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
want = sys.argv[4] if len(sys.argv) > 4 else "/rt/pod"
src = "/sys/kernel/debug/sched/pfs/groups"
period = 1.0 / hz
rows, n, t0 = [], 0, time.monotonic()
deadline = t0 + secs
try:
    while True:
        now = time.monotonic()
        if now >= deadline:
            break
        try:
            with open(src) as f:
                txt = f.read()
        except OSError as e:
            rows.append(f"# read failed: {e}")
            break
        for line in txt.splitlines():
            if line.startswith("#") or want not in line:
                continue
            c = line.split()
            if len(c) < 8:
                continue
            rows.append(f"S t={now - t0:.4f} limit={c[0]} slots={c[1]} waiting={c[2]} "
                        f"tokens_us={c[3]} active={c[6]} path={c[7]}")
        n += 1
        nxt = t0 + n * period
        if nxt > now:
            time.sleep(nxt - now)
finally:
    el = time.monotonic() - t0
    ru = resource.getrusage(resource.RUSAGE_SELF)
    with open(out, "w") as f:
        f.write("\n".join(rows) + "\n")
        f.write(f"# samples={n} elapsed={el:.3f} achieved_hz={n / el if el else 0:.1f} "
                f"sampler_cpu_s={ru.ru_utime + ru.ru_stime:.3f} "
                f"sampler_cpu_frac={(ru.ru_utime + ru.ru_stime) / el if el else 0:.4f}\n")
