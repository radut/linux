#!/usr/bin/env python3
"""swlat.py TRACE PIDFILE: context-switch and wake-up-latency statistics for the pids in PIDFILE.

Phase 2a.1 and 2a.2. Reads one tracefs trace that has BOTH sched_switch and sched_wakeup
enabled and reports, for the threads of the container cgroup only:

  2a.1  switches per second, and how many of them are INVOLUNTARY - sched_switch prints
        prev_state, and a preempted task leaves in state R while one that blocked leaves in
        S/D. scx_pfs parks a task by taking its slot away, which shows up as prev_state R.
  2a.2  wake-up to run latency: the delay from a sched_wakeup naming one of our pids to the
        sched_switch that puts that pid on a CPU. Reported as mean/p50/p90/p99 in microseconds,
        split by whether the task ran on the CPU the waker targeted or on another one, because
        scx_pfs dispatches about half its wake-ups through a shared DSQ rather than straight to
        a CPU and a migration is the cost of that.

Latencies are only counted for a wake-up followed by a switch-in with no intervening switch-in
of the same pid; a wake-up still outstanding when the trace ends is dropped, and the number
dropped is reported so a truncated buffer cannot look like a good result.
"""
import re, sys

tr, pidf = sys.argv[1], sys.argv[2]
pids = {int(x) for x in open(pidf).read().split()}

# "... [003] ..... 12345.678901: sched_switch: prev_comm=x prev_pid=1 prev_prio=120 prev_state=R ==> next_comm=y next_pid=2 next_prio=120"
# The flags column between the CPU and the timestamp contains digits ("d..2."), so it is skipped
# with .*? and not \D*; the timestamp is pinned by the ": sched_switch: " that follows it.
sw = re.compile(r'\[(\d+)\].*?\s(\d+\.\d+): sched_switch: .*?prev_pid=(\d+) .*?prev_state=(\S+) ==> .*?next_pid=(\d+)')
# "... [003] ..... 12345.678901: sched_wakeup: comm=x pid=1 prio=120 target_cpu=003"
wk = re.compile(r'\s(\d+\.\d+): sched_wakeup:\s.*?\bpid=(\d+)\b.*?target_cpu=(\d+)')

t_lo = t_hi = None
n_in = n_out = n_invol = 0
pend = {}          # pid -> (wakeup ts, target cpu)
lat_same, lat_other = [], []
dropped_wk = 0

for line in open(tr, errors='replace'):
    m = sw.search(line)
    if m:
        cpu, ts, prev, state, nxt = int(m[1]), float(m[2]), int(m[3]), m[4], int(m[5])
        t_lo = ts if t_lo is None else min(t_lo, ts)
        t_hi = ts if t_hi is None else max(t_hi, ts)
        if prev in pids:
            n_out += 1
            # A preempted task leaves runnable; R+ is the same state with preempt set.
            if state.startswith('R'):
                n_invol += 1
        if nxt in pids:
            n_in += 1
            w = pend.pop(nxt, None)
            if w is not None:
                (lat_same if w[1] == cpu else lat_other).append((ts - w[0]) * 1e6)
        continue
    m = wk.search(line)
    if m:
        ts, pid, tcpu = float(m[1]), int(m[2]), int(m[3])
        if pid in pids:
            if pid in pend:
                dropped_wk += 1      # an earlier wake-up never reached a CPU
            pend[pid] = (ts, tcpu)

if t_lo is None:
    print("swlat no_events=1")
    sys.exit()

T = t_hi - t_lo
dropped_wk += len(pend)


def q(v, p):
    if not v:
        return float('nan')
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))]


all_lat = lat_same + lat_other
f = []
f.append(f"trace_secs={T:.3f}")
f.append(f"sw_in={n_in} sw_out={n_out} sw_invol={n_invol}")
f.append(f"sw_in_per_s={n_in / T:.0f} invol_frac={(n_invol / n_out if n_out else float('nan')):.4f}")
f.append(f"wk_n={len(all_lat)} wk_same={len(lat_same)} wk_other={len(lat_other)} wk_dropped={dropped_wk}")
f.append(f"wk_same_frac={(len(lat_same) / len(all_lat) if all_lat else float('nan')):.4f}")
for nm, v in (("all", all_lat), ("same", lat_same), ("other", lat_other)):
    if v:
        f.append(f"lat_{nm}_us_mean={sum(v) / len(v):.1f} lat_{nm}_us_p50={q(v, .50):.1f} "
                 f"lat_{nm}_us_p90={q(v, .90):.1f} lat_{nm}_us_p99={q(v, .99):.1f}")
print("swlat " + " ".join(f))
