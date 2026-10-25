#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Summarise a results.csv from compare.sh: one line per case and variant,
median with min-max when a case was repeated.

  summary.py results.csv
"""
import csv
import statistics
import sys


def fmt(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return "-"
    med = statistics.median(vals)
    if len(vals) == 1:
        return f"{med:g}"
    return f"{med:g} ({min(vals):g}-{max(vals):g})"


def num(v):
    try:
        return float(v)
    except ValueError:
        return None


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: summary.py results.csv")
    groups = {}
    with open(sys.argv[1]) as f:
        for r in csv.DictReader(f):
            wl = "reqlat" if r["p50_ms"] != "-" else "spin"
            key = (wl, r["path"], r["limit"], r["threads"], r["neighbours"], r["variant"])
            groups.setdefault(key, []).append(r)
    if not groups:
        print("no rows")
        return
    reps = max(len(v) for v in groups.values())
    print(f"median (min-max) over up to {reps} runs per case")
    print()
    print("Throughput (M iterations/s), worst stall (ms), nr_throttled, CPUs used:")
    print(f"{'path':<24} {'limit':<5} {'thr':<4} {'neigh':<6} {'variant':<7} "
          f"{'tput':<20} {'stall_ms':<24} {'throttled':<16} cpus")
    for key in sorted(k for k in groups if k[0] == "spin"):
        rows = groups[key]
        _, path, lim, thr, neigh, var = key
        print(f"{path:<24} {lim:<5} {thr:<4} {neigh:<6} {var:<7} "
              f"{fmt([num(r['miter_s']) for r in rows]):<20} "
              f"{fmt([num(r['worst_stall_ms']) for r in rows]):<24} "
              f"{fmt([num(r['nr_throttled']) for r in rows]):<16} "
              f"{fmt([num(r['avg_conc']) for r in rows])}")
    lat = sorted(k for k in groups if k[0] == "reqlat")
    if lat:
        print()
        print("Request latency (ms): p50, p99, max; burner throughput (M it/s):")
        for key in lat:
            rows = groups[key]
            _, path, lim, thr, neigh, var = key
            print(f"{path:<24} {lim:<5} {neigh:<6} {var:<7} "
                  f"p50={fmt([num(r['p50_ms']) for r in rows]):<20} "
                  f"p99={fmt([num(r['p99_ms']) for r in rows]):<22} "
                  f"max={fmt([num(r['max_ms']) for r in rows]):<22} "
                  f"burn={fmt([num(r['miter_s']) for r in rows])}")


if __name__ == "__main__":
    main()
