#!/usr/bin/env python3
"""summ2.py RESDIR OUT_JSON [VARS]: parse round-2 remeasure guest outputs (main, sub-unit, kit paths, systemd VM)
-> rows + per-metric median/min/max/n and marks vs BASE (default cfs). Same rules as remeasure/summ.py, plus the
throttle counters (thr_*, pfs_*) and the sub-unit suite (test "su")."""
import os
import csv, glob, json, os, re, statistics, sys

RES, OUT = sys.argv[1], sys.argv[2]
VARS = (sys.argv[3] if len(sys.argv) > 3 else "cfs,cfs-pin,cfs-burst,kwidth,scx,kpfs,scxr1,kpfsr1").split(",")
HDR = ("path,variant,limit,threads,neighbours,secs,miter_s,nr_throttled,throttled_ms,worst_stall_ms,stalls_5ms,"
       "avg_conc,max_conc,pct_over_cap,p50_ms,p99_ms,max_ms,avg_cpus,busy_frac,trace_cpus,nr_periods,throttled_usec,"
       "pfs_nr_periods,pfs_nr_throttled,pfs_throttled_usec").split(",")
BASE = os.environ.get("BASE", "cfs")
num = lambda x: float(x) if re.fullmatch(r"-?[\d.]+(e[+-]?\d+)?", str(x or "")) else None
kvs = lambda s: dict(re.findall(r"(\S+?)=(\S+)", s))
rows = []


def add(v, r, test, cell, **m):
    rows.append(dict(variant=v, round=int(r), test=test, cell=cell, **{k: x for k, x in m.items() if x is not None}))


def thr(k, pfx=""):
    """throttle metrics of a key=value line: nr_throttled, throttled s, throttled/wall, scx_pfs counters"""
    g = lambda n: num(k.get(pfx + n))
    out = dict(t_per=g("thr_per"), t_n=g("thr_n"), t_frac=g("thr_frac"),
               p_per=g("pfs_per"), p_n=g("pfs_n"), p_frac=g("pfs_frac"))
    if g("thr_us") is not None:
        out["t_s"] = g("thr_us") / 1e6
    if g("pfs_us") is not None:
        out["p_s"] = g("pfs_us") / 1e6
    return out


def thr2(k):
    """pod level (limited level) and container level (c_) throttle metrics"""
    return dict(**thr(k), **{f"c_{x}": y for x, y in thr(k, "c_").items()})


def parse_line(v, r, test, rest):
    if test in ("mx", "nn", "latlim"):
        b = re.search(r"\bb=(\d+)", rest)
        c = re.search(r"csv=(\S*)", rest).group(1).split(",")
        if len(c) < 17:
            return
        d = dict(zip(HDR, c))
        secs = num(d["secs"]) or 5
        tu = num(d.get("throttled_usec"))
        t = dict(t_per=num(d.get("nr_periods")), t_n=num(d.get("nr_throttled")),
                 t_s=tu / 1e6 if tu is not None else None, t_frac=tu / 1e6 / secs if tu is not None else None,
                 p_per=num(d.get("pfs_nr_periods")), p_n=num(d.get("pfs_nr_throttled")))
        pu = num(d.get("pfs_throttled_usec"))
        if pu is not None:
            t.update(p_s=pu / 1e6, p_frac=pu / 1e6 / secs)
        if test == "latlim":
            add(v, r, "latlim", f"limit {d['limit']}, {b.group(1)} burners", p50=num(d["p50_ms"]),
                p99=num(d["p99_ms"]), cpus=num(d.get("trace_cpus")), **t)
        else:
            add(v, r, test, f"limit {d['limit']} x {d['threads']} thr", thr=num(d["miter_s"]),
                stall=num(d["worst_stall_ms"]), cpus=num(d.get("trace_cpus")), cpus_stat=num(d.get("avg_cpus")),
                conc=num(d["avg_conc"]), **t)
        return
    k = kvs(rest)
    if test.startswith("reqlat"):
        add(v, r, "ulat", test, p50=num(k.get("p50_ms")), p99=num(k.get("p99_ms")))
    elif test.startswith("wlat"):
        add(v, r, "wlat", test, p50=num(k.get("p50_us")), p99=num(k.get("p99_us")))
    elif test.startswith("wt-"):
        mm = re.search(r"\(([\d.]+) / ([\d.]+) CPUs\) B/A=([\d.]+)", rest)
        if mm:
            add(v, r, "wt", test, a=num(mm.group(1)), b=num(mm.group(2)), ratio=num(mm.group(3)),
                a_t_n=num(k.get("a_thr_n")), a_t_frac=num(k.get("a_thr_frac")),
                b_t_n=num(k.get("b_thr_n")), b_t_frac=num(k.get("b_thr_frac")))
    elif test == "starve":
        cpu = re.search(r"cpu=(\d+)us", rest)
        add(v, r, "starve", "besteffort", stall=num(k.get("max_stall_ms")),
            cpu_ms=num(cpu.group(1)) / 1000 if cpu else None)
    elif test.startswith(("ly", "flag", "gc")):
        add(v, r, "lk", f"{test} @ limit {k.get('limit')}", cpus=num(k.get("cpus")),
            ops=num(k.get("events_s") or k.get("ops_per_s")), gc_p99=num(k.get("pause_p99_ms")), **thr2(k))
    elif test.startswith("fan-"):
        add(v, r, "burst", f"{test} @ limit {k.get('limit')}", p50=num(k.get("p50_ms")), p99=num(k.get("p99_ms")),
            cpus=num(k.get("cpus")), **thr2(k))
    elif test.startswith("kh-"):
        add(v, r, "kh", f"{test[3:]} @ limit {k.get('limit')}", lim=num(k.get("limit")), cpus=num(k.get("cpus")),
            trc=num(k.get("mean_running")), over=num(k.get("frac_over_limit")), **thr2(k))
    elif test.startswith("nest-"):
        mode = test[5:]
        a_ = re.search(r" a: (.*?) \|", rest); b_ = re.search(r"\| b: (.*)$", rest)
        ak, bk = kvs(a_.group(1) if a_ else ""), kvs(b_.group(1) if b_ else "")
        tp = {f"par_{x}": y for x, y in thr(k, "par_").items()}
        ta = {f"a_{x}": y for x, y in thr(k, "a_").items()}
        tb = {f"b_{x}": y for x, y in thr(k, "b_").items()}
        add(v, r, "nest", f"{mode} {k.get('cell')}", par=num(k.get("par_cpus")), a=num(k.get("a_cpus")),
            b=num(k.get("b_cpus")), par_tr=num(k.get("par_tr")), a_tr=num(k.get("a_tr")), b_tr=num(k.get("b_tr")),
            p50=num(ak.get("p50_ms")), p99=num(ak.get("p99_ms")), stall_a=num(ak.get("max_stall_ms")),
            stall=num(bk.get("max_stall_ms")), **tp, **ta, **tb)
    elif test == "pg":
        add(v, r, "pg", f"{k.get('wl')} @ limit {k.get('lim')}", lim=num(k.get("lim")), tps=num(k.get("tps")),
            cpus=num(k.get("cpus")), trc=num(k.get("mean_running")), lat=num(k.get("lat_ms")), **thr2(k))
    elif test == "thr":
        def tri(s):
            m = re.fullmatch(r"(\d+)/(\d+)/(\d+)ms", s or "")
            return [float(x) for x in m.groups()] if m else [None] * 3
        w = num(k.get("wall_s")) or 1
        cp, cc, pp, pc = (tri(k.get(x)) for x in ("cpustat_pod", "cpustat_ctr", "pfs_pod", "pfs_ctr"))
        f = lambda t: t[2] / 1000 / w if t[2] is not None else None
        add(v, r, "thr", f"{k.get('case')} @ limit {k.get('limit')}", pod_t_per=cp[0], pod_t_n=cp[1],
            pod_t_s=cp[2] / 1000 if cp[2] is not None else None, pod_t_frac=f(cp), ctr_t_n=cc[1], ctr_t_frac=f(cc),
            pod_p_per=pp[0], pod_p_n=pp[1], pod_p_frac=f(pp), ctr_p_n=pc[1], ctr_p_frac=f(pc))
    elif test == "su":
        lim = num(k.get("limit"))
        rr = rest.split(" | ", 1)
        wk = kvs(rr[1]) if len(rr) > 1 else {}
        secs = num(k.get("secs")) or 1
        tu = num(k.get("d_throttled_usec")); pu = num(k.get("pfs_throttled_usec"))
        m = dict(lim=lim, cpus=num(k.get("cpus")), sw=num(k.get("sw_cpus")), w100_max=num(k.get("w100_max")),
                 w100_over=num(k.get("w100_over")), w1s_max=num(k.get("w1s_max")), held=num(k.get("held_frac")),
                 tw100_max=num(k.get("tw100_max")), tw100_over=num(k.get("tw100_over")), tw1s_max=num(k.get("tw1s_max")),
                 t_per=num(k.get("d_nr_periods")), t_n=num(k.get("d_nr_throttled")),
                 t_frac=tu / 1e6 / secs if tu is not None else None,
                 p_per=num(k.get("pfs_nr_periods")), p_n=num(k.get("pfs_nr_throttled")),
                 p_frac=pu / 1e6 / secs if pu is not None else None)
        if lim:
            m["cpus_rel"] = m["cpus"] / lim if m["cpus"] is not None else None
            m["w1s_rel"] = m["w1s_max"] / lim if m["w1s_max"] is not None else None
            m["w100_rel"] = m["w100_max"] / lim if m["w100_max"] is not None else None
        wl = k.get("wl", "")
        if wl.startswith("req"):
            m.update(p50=num(wk.get("p50_ms")), p99=num(wk.get("p99_ms")), mx=num(wk.get("max_ms")))
        elif wl.startswith("burst"):
            m.update(p50=num(wk.get("p50_ms")), p99=num(wk.get("p99_ms")), mx=num(wk.get("max_ms")),
                     late=num(wk.get("late")))
        else:
            m.update(thr=num(wk.get("iters")), stall=num(wk.get("max_stall_ms")))
        add(v, r, "su", f"{wl} @ limit {k.get('limit')}", **m)
    elif test.startswith(("hb-", "pipe")):
        add(v, r, "unl", test, time=num(k.get("time")))
    elif test == "barrier6":
        add(v, r, "unl", test, ops=num(k.get("ops_per_s")))
    elif test.startswith("schbench"):
        add(v, r, "unl", test, wake_p99=num(k.get("wake_p99_us")), req_p99=num(k.get("req_p99_us")),
            rps=num(k.get("rps")))


for f in sorted(glob.glob(os.path.join(RES, "r[0-9]*-*.txt")) + glob.glob(os.path.join(RES, "su[0-9]*-*.txt"))):
    for line in open(f, errors="replace"):
        if not line.startswith("R "):
            continue
        m = re.match(r"R v=(\S+) r=(\d+) (?:base=(\S+) )?test=(\S+) ?(.*)", line.strip())
        if not m:
            continue
        v, r, base, test, rest = m.groups()
        bm = re.search(r"\bbase=(pin|burst)\b", rest)
        base = base or (bm.group(1) if bm else None)
        if base:
            v = f"{v}-{base}"
        parse_line(v, r, test, rest)


def kit_rows(pattern, label):
    for d in sorted(glob.glob(pattern)):
        mm = re.search(r"-([a-z0-9]+)-(\d+)$", d)
        if not mm or not os.path.exists(os.path.join(d, "results.csv")):
            continue
        v, r = mm.groups()
        for c in csv.DictReader(open(os.path.join(d, "results.csv"))):
            secs = num(c.get("secs")) or 5
            tms = num(c.get("throttled_ms")); pms = num(c.get("pfs_throttled_ms"))
            t = dict(t_n=num(c.get("nr_throttled")), t_frac=tms / 1000 / secs if tms is not None else None,
                     p_n=num(c.get("pfs_nr_throttled")), p_frac=pms / 1000 / secs if pms is not None else None)
            if c["p50_ms"] not in ("-", ""):
                add(v, r, label, f"reqlat limit {c['limit']}", p50=num(c["p50_ms"]), p99=num(c["p99_ms"]), **t)
            else:
                add(v, r, label, f"limit {c['limit']} x {c['threads']} {c['neighbours']}", thr=num(c["miter_s"]),
                    stall=num(c["worst_stall_ms"]), conc=num(c["avg_conc"]), **t)


kit_rows(os.path.join(RES, "kit-cg-*"), "kit-cg")
kit_rows(os.path.join(RES, "kit-dock-*"), "kit-dock")
kit_rows(os.path.join(RES, "sysvm", "sd-*"), "kit-systemd")
kit_rows(os.path.join(RES, "sysvm", "dk-*"), "kit-docker-systemd")
kit_rows(os.path.join(RES, "sysvm", "pm-*"), "kit-podman-systemd")
rows = [x for x in rows if x["round"] > 0]

DIR = dict(thr=1, stall=-1, p50=-1, p99=-1, mx=-1, ops=1, gc_p99=-1, time=-1, wake_p99=-1, req_p99=-1, rps=1,
           cpu_ms=1, tps=1, lat=-1, stall_a=-1)
agg = {}
for x in rows:
    for met, val in x.items():
        if met not in ("variant", "round", "test", "cell"):
            agg.setdefault((x["test"], x["cell"], met), {}).setdefault(x["variant"], []).append(val)
summary = []
for (test, cell, met), byv in agg.items():
    e = dict(test=test, cell=cell, metric=met, n={}, median={}, min={}, max={})
    for v in VARS:
        vals = byv.get(v)
        if vals:
            e["n"][v] = len(vals); e["median"][v] = round(statistics.median(vals), 4)
            e["min"][v] = min(vals); e["max"][v] = max(vals)
    c = e["median"].get(BASE)
    if c and met in DIR:
        e["vs_cfs"] = {}
        for v in e["median"]:
            rr = e["median"][v] / c
            better = (rr > 1.05) if DIR[met] > 0 else (rr < 0.95)
            worse = (rr < 0.95) if DIR[met] > 0 else (rr > 1.05)
            sep = ((e["max"][v] < e["min"][BASE]) if DIR[met] > 0 else (e["min"][v] > e["max"][BASE])) if worse else \
                  ((e["min"][v] > e["max"][BASE]) if DIR[met] > 0 else (e["max"][v] < e["min"][BASE])) if better else False
            e["vs_cfs"][v] = dict(ratio=round(rr, 3), mark="up" if better else "down" if worse else "", separated=sep)
    summary.append(e)
json.dump(dict(vars=VARS, dir=DIR, rows=rows, summary=summary), open(OUT, "w"), indent=1)
print(f"{len(rows)} rows, {len(summary)} metrics -> {OUT}")
