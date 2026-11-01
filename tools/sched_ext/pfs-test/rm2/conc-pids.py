#!/usr/bin/env python3
"""conc.py TRACE PIDFILE LIMIT: time-weighted number of CPUs running a task of PIDFILE, from sched_switch."""
import re, sys
tr, pidf, lim = sys.argv[1], sys.argv[2], float(sys.argv[3])
pids = {int(x) for x in open(pidf).read().split()}
pat = re.compile(r'\[(\d+)\].*?\s(\d+\.\d+): sched_switch: .*?prev_pid=(\d+) .*?next_pid=(\d+)')
cur = {}      # cpu -> running pid in set?
ev = []
for line in open(tr, errors='replace'):
    m = pat.search(line)
    if not m:
        continue
    cpu, ts, prev, nxt = int(m[1]), float(m[2]), int(m[3]), int(m[4])
    ev.append((ts, cpu, prev in pids, nxt in pids))
ev.sort()
if not ev:
    print("no events"); sys.exit()
n = 0; last = ev[0][0]; hist = {}; state = {}
t0 = ev[0][0]
# skip the first 0.05 s while per-CPU state is unknown: assume state from first prev
for ts, cpu, pin, nin in ev:
    if cpu not in state:
        state[cpu] = pin
        n += pin
    hist[n] = hist.get(n, 0) + (ts - last)
    last = ts
    n += (nin - state[cpu]); state[cpu] = nin
T = sum(hist.values())
mean = sum(k * v for k, v in hist.items()) / T
over = sum(v for k, v in hist.items() if k > lim) / T
top = sorted(hist.items())
print(f"trace_secs={T:.2f} mean_running={mean:.3f} frac_over_limit={over:.4f} max={max(k for k,v in hist.items() if v>0)} "
      + "hist=" + ",".join(f"{k}:{v/T:.3f}" for k, v in top if v / T >= 0.001))