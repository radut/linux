#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Read an ftrace sched_switch dump and report how many CPUs ran tasks named
COMM at the same time.

  conc.py COMM TRACE_FILE [CAP]

Prints one key=value line: max_conc, avg_conc (CPUs busy with COMM, averaged
over the time at least one was), pct_over_cap (share of that busy time with
more than CAP CPUs running COMM; CAP is the limit rounded up).
Use the "mono" trace clock: the default "global" clock can lag on CPUs that
leave NOHZ idle in VMs, which reorders events.
"""
import math
import re
import sys
from collections import defaultdict

PAT = re.compile(r"\[(\d+)\].*?\s(\d+\.\d+): sched_switch: prev_comm=(.*?) prev_pid=\d+ "
                 r".*==> next_comm=(.*?) next_pid=\d+")


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: conc.py COMM TRACE_FILE [CAP]")
    comm, path = sys.argv[1], sys.argv[2]
    cap = math.ceil(float(sys.argv[3])) if len(sys.argv) > 3 else None

    running = {}
    level_time = defaultdict(float)
    cur, last_ts, max_conc = 0, None, 0
    with open(path, errors="replace") as f:
        for line in f:
            m = PAT.search(line)
            if not m:
                continue
            cpu, ts, nxt = int(m.group(1)), float(m.group(2)), m.group(4)
            if last_ts is not None:
                level_time[cur] += ts - last_ts
            last_ts = ts
            running[cpu] = nxt == comm
            cur = sum(running.values())
            max_conc = max(max_conc, cur)

    busy = sum(t for lvl, t in level_time.items() if lvl > 0)
    avg = sum(lvl * t for lvl, t in level_time.items()) / busy if busy else 0.0
    out = f"max_conc={max_conc} avg_conc={avg:.2f}"
    if cap is not None:
        over = sum(t for lvl, t in level_time.items() if lvl > cap)
        out += f" pct_over_cap={100 * over / busy if busy else 0.0:.1f}"
    print(out)


if __name__ == "__main__":
    main()
