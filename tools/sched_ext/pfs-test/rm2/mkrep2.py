#!/usr/bin/env python3
"""mkrep2.py MAIN_JSON X_JSON OUT_TABLES_MD OUT_JSON: tables for remeasure-local-r2 from summ2.py output.
MAIN_JSON: res/ (main, sub-unit, kit paths, systemd VM); X_JSON: resx/ (kPFS configs, BASE=kpfs).
Writes the markdown tables (three-column round comparison, regressions vs CFS, sub-unit, throttle counters, full
tables) and the dashboard JSON (no internal names, IPs or paths)."""
import json, sys

d = json.load(open(sys.argv[1])); dx = json.load(open(sys.argv[2]))
OUTMD, OUTJ = sys.argv[3], sys.argv[4]
S = {(e["test"], e["cell"], e["metric"]): e for e in d["summary"]}
SX = {(e["test"], e["cell"], e["metric"]): e for e in dx["summary"]}
DIR = d["dir"]
LBL = {"cfs": "CFS", "cfs-pin": "CFS cpuset-pinned", "cfs-burst": "CFS + burst", "kwidth": "kwidth r2",
       "scx": "scx_pfs r2", "kpfs": "kPFS r2", "scxr1": "scx_pfs r1", "kpfsr1": "kPFS r1",
       "kpfsnoext": "kPFS EXT=n", "kpfsrt": "kPFS PREEMPT_RT", "kpfssmt": "kPFS SMT topology",
       "kpfscore": "kPFS SMT + core sched"}
UNIT = dict(thr="M iter/s", stall="worst stall ms", stall_a="worst stall ms (child a)", p50="p50 ms", p99="p99 ms",
            mx="max ms", ops="ops/s", gc_p99="GC pause p99 ms", time="s", wake_p99="wake p99 us",
            req_p99="request p99 us", rps="requests/s", tps="tps", lat="latency ms", cpus="CPUs (cpu.stat)",
            trc="CPUs (trace)", cpus_stat="CPUs (cpu.stat)", conc="avg running threads", cpu_ms="CPU ms",
            ratio="B/A", a="CPUs a", b="CPUs b", par="CPUs parent", par_tr="parent CPUs (trace)",
            a_tr="a CPUs (trace)", b_tr="b CPUs (trace)", sw="CPUs (trace)", w100_max="max 100 ms window CPUs",
            w1s_max="max 1 s window CPUs", tw100_max="max 100 ms window CPUs (trace)", tw1s_max="max 1 s window CPUs (trace)",
            tw100_over="share of 100 ms windows > limit (trace)", w100_over="share of 100 ms windows > limit", held="held share",
            t_n="nr_throttled", t_per="nr_periods", t_frac="throttled time / wall", p_n="scx_pfs nr_throttled",
            p_per="scx_pfs nr_periods", p_frac="scx_pfs throttled / wall", over="time share above limit (trace)",
            late="late requests")
DESC = {
    "mx": "limited group, LIMIT CPUs x THREADS busy threads, 5 s",
    "nn": "limit 2 group next to 8 unlimited busy threads, 5 s",
    "latlim": "reqlat (4 x 1 ms every 20 ms) in a limited group with 0 or 8 burners",
    "ulat": "reqlat without limits, 0 or 8 burners",
    "wlat": "periodic wakeup latency without limits",
    "wt": "two kubelet-style pods, requests 1:3 (weights), optional limits 4/6, 8 s",
    "starve": "weight-1 thread next to 16 weight-5000 spinners, 20 s",
    "lk": "lock probes in a two-level pod limited to LIMIT",
    "unl": "no limits (hackbench, pipe, barrier, schbench)",
    "kh": "limit check in a pod: 16 spinners / 16 urandom dd readers / 8 dd|dd pipe pairs; cpu.stat over 10 s, trace 5 s",
    "burst": "fan-out: a request every P ms fans out to 8 threads of 1-5 ms CPU, 10 s",
    "nest": "parent limit 4, children A+B busy (spin: 8 spinners each; lat: reqlat in a)",
    "pg": "Postgres in a limited pod, pgbench 32 clients from the root cgroup, 30 s",
    "thr": "throttle-counter cases, 10 s, pod and container levels",
    "su": "sub-unit suite (Poisson requests, bursts, spin) in cgroup su/ctr",
    "kit-cg": "in-tree kit, cgroup path", "kit-dock": "in-tree kit, Docker (cgroupfs driver)",
    "kit-systemd": "in-tree kit, systemd scope (systemd VM)",
    "kit-docker-systemd": "in-tree kit, Docker with the systemd driver (systemd VM)",
    "kit-podman-systemd": "in-tree kit, Podman with the systemd driver (systemd VM)",
}
LIMIT_TESTS = {"kh": ("cpus", "trc"), "pg": ("cpus", "trc"), "su": ("cpus", "sw")}


def fmt(x):
    if x is None:
        return "-"
    a = abs(x)
    if a >= 1e6:
        return f"{x / 1e6:.2f}M"
    if a >= 1e4:
        return f"{x / 1e3:.1f}k"
    if a >= 100:
        return f"{x:.0f}"
    if a >= 10:
        return f"{x:.1f}"
    if a >= 1:
        return f"{x:.2f}"
    return f"{x:.3f}"


def mark(e, v, base="cfs"):
    vc = e.get("vs_cfs", {}).get(v)
    if not vc or v == base:
        return ""
    m = {"up": "↑", "down": "↓"}.get(vc["mark"], "")
    return m + ("" if not m or vc["separated"] else "?")


def cellv(e, v, base="cfs", rng=True):
    if not e or v not in e["median"]:
        return "-"
    s = fmt(e["median"][v])
    if rng and e["n"][v] > 1:
        s += f" ({fmt(e['min'][v])}-{fmt(e['max'][v])})"
    m = mark(e, v, base)
    return s + (" " + m if m else "")


def cells(SS, test):
    seen = []
    for (t, c, m) in SS:
        if t == test and c not in seen:
            seen.append(c)
    return seen


def metrics(SS, test, cell):
    return [m for (t, c, m) in SS if t == test and c == cell]


md = []
J = dict(rows=[])


def table(title, note, header, rows):
    md.append(f"### {title}\n")
    if note:
        md.append(note + "\n")
    md.append("| " + " | ".join(header) + " |")
    md.append("|" + "---|" * len(header))
    for r in rows:
        md.append("| " + " | ".join(str(x) for x in r) + " |")
    md.append("")


def jrow(section, test, cell, met, e, vars_, base="cfs"):
    if not e:
        return
    vals = {}
    for v in vars_:
        if v in e["median"]:
            vals[LBL.get(v, v)] = dict(median=e["median"][v], min=e["min"][v], max=e["max"][v], n=e["n"][v],
                                       mark=mark(e, v, base))
    if vals:
        J["rows"].append(dict(section=section, test=test, setup=DESC.get(test, ""), case=cell,
                              metric=UNIT.get(met, met), lower_is_better=DIR.get(met) == -1 if met in DIR else None,
                              values=vals))


# ---- 1. three-column comparison: round 1 vs round 2 vs CFS -----------------------------------------------------
def three(sch, r1, r2):
    rows = []
    for (t, c, m), e in S.items():
        if t in ("su",) or t.startswith("kit-"):
            continue
        if m not in DIR and not (t in LIMIT_TESTS and m in LIMIT_TESTS[t]) and not (t == "nest" and m.endswith("_tr")):
            continue
        if r1 not in e["median"] or r2 not in e["median"]:
            continue
        a, b = e["median"][r1], e["median"][r2]
        ch = ""
        if m in DIR and a:
            q = b / a
            better = q > 1.05 if DIR[m] > 0 else q < 0.95
            worse = q < 0.95 if DIR[m] > 0 else q > 1.05
            sep = (e["min"][r2] > e["max"][r1] if DIR[m] > 0 else e["max"][r2] < e["min"][r1]) if better else \
                  (e["max"][r2] < e["min"][r1] if DIR[m] > 0 else e["min"][r2] > e["max"][r1]) if worse else True
            ch = f"{q:.2f}" + (" ↑" if better else " ↓" if worse else "") + ("" if sep or not (better or worse) else "?")
        rows.append([t, c, UNIT.get(m, m), cellv(e, "cfs"), cellv(e, r1), cellv(e, r2), ch])
        jrow(f"round 1 vs round 2, {sch}", t, c, m, e, ["cfs", r1, r2])
    rows.sort(key=lambda r: (r[0], r[1], r[2]))
    table(f"{sch}: CFS / round 1 / round 2", "Medians (min-max) over the boots. Arrows on the round columns: vs CFS; "
          "last column: round 2 / round 1 (↑ = round 2 better by more than 5%, ? = ranges overlap). Limit-check "
          "rows (CPUs) have no direction: the target is the limit.",
          ["Test", "Case", "Metric", "CFS", "round 1", "round 2", "r2 / r1"], rows)


md.append("## Round 1 vs round 2 vs CFS (same rounds)\n")
three("kPFS", "kpfsr1", "kpfs")
three("scx_pfs", "scxr1", "scx")

# ---- 2. regressions vs CFS ---------------------------------------------------------------------------------------
reg = {}
for (t, c, m), e in S.items():
    for v, vc in e.get("vs_cfs", {}).items():
        if v in ("kpfs", "scx", "kwidth") and vc["mark"] == "down":
            reg.setdefault(v, []).append((vc["separated"], t, c, m, e, vc["ratio"]))
over = {}
for (t, c, m), e in S.items():
    if t in LIMIT_TESTS and m in LIMIT_TESTS[t]:
        for v, x in e["median"].items():
            lim = S.get((t, c, "lim"), {}).get("median", {}).get(v)
            if lim and x > lim * 1.03:
                over.setdefault(v, []).append((t, c, m, x, lim, e))
    if t == "nest" and m in ("par_tr", "a_tr", "b_tr"):
        pass
md.append("## Remaining regressions vs CFS\n")
md.append("Every metric where the round-2 median is more than 5% worse than stock CFS. **Separated** = the min-max "
          "ranges do not overlap (a regression); otherwise the difference is within the spread (listed separately). "
          "Then every limit check whose median is more than 3% over the limit.\n")
for v in ("kpfs", "scx", "kwidth"):
    md.append(f"#### {LBL[v]}\n")
    rs = sorted(reg.get(v, []), key=lambda x: (not x[0], x[1], x[2]))
    sep = [x for x in rs if x[0]]; ovl = [x for x in rs if not x[0]]
    md.append(f"Separated ({len(sep)}):\n")
    for _, t, c, m, e, r in sep:
        md.append(f"- {t} / {c} / {UNIT.get(m, m)}: {cellv(e, v)} vs CFS {cellv(e, 'cfs')} ({r:.2f}x)")
        jrow(f"regression vs CFS, {LBL[v]}", t, c, m, e, ["cfs", v])
    md.append(f"\nWithin the spread ({len(ovl)}):\n")
    for _, t, c, m, e, r in ovl:
        md.append(f"- {t} / {c} / {UNIT.get(m, m)}: {cellv(e, v)} vs CFS {cellv(e, 'cfs')} ({r:.2f}x)")
    ov = over.get(v, [])
    md.append(f"\nLimit checks over the limit by more than 3% ({len(ov)}):\n")
    for t, c, m, x, lim, e in ov:
        md.append(f"- {t} / {c} / {UNIT.get(m, m)}: {cellv(e, v)} at limit {lim:g} (CFS {cellv(e, 'cfs')})")
    md.append("")

# ---- 3. sub-unit limits -------------------------------------------------------------------------------------------
SV = ["cfs", "kwidth", "scx", "kpfs"]
md.append("## Sub-unit limits (tables)\n")
rows = []
for c in cells(S, "su"):
    r = [c]
    for v in SV:
        g = lambda m: S.get(("su", c, m), {}).get("median", {}).get(v)
        lim = g("lim") or 1
        lat = g("p99") if not c.startswith("spin") else None
        it = g("thr") if c.startswith("spin") else None
        w1 = g("tw1s_max") if v == "scx" else g("w1s_max")
        w100o = g("tw100_over") if v == "scx" else g("w100_over")
        sw = g("sw")
        held = g("held"); tf = g("p_frac") if v == "scx" else g("t_frac")
        first = (f"p99 {fmt(lat)} ms" if lat is not None else f"{fmt(it)} iter") if (lat is not None or it is not None) else "-"
        r.append(f"{first}; CPUs {fmt(sw)}; 1 s max {fmt(w1 / lim if w1 is not None else None)}x; "
                 f"100 ms over {fmt(w100o)}; held {fmt(held)}; thr/wall {fmt(tf)}")
    rows.append(r)
table("Sub-unit overview (medians of 3 boots)", "Per cell: request or burst p99 (spin: iterations); CPUs from the "
      "sched_switch trace over the run; highest 1 s window as a multiple of the limit; share of 100 ms windows above "
      "1.05 x limit; held share (cpu.pressure some / wall); throttled time / wall (cpu.stat; scx_pfs: its own counter). "
      "Windows come from cpu.stat sampled every 100 ms, except scx_pfs, where cpu.stat over-reports and the windows come "
      "from the trace (valid only while the group is throttled; an unthrottled single spinner shows trace artefacts).",
      ["Case"] + [LBL[v] for v in SV], rows)
for grp, title in (("req", "Poisson requests"), ("burst", "Periodic bursts"), ("spin", "Spin")):
    rows = []
    for c in cells(S, "su"):
        if not c.startswith(grp):
            continue
        mets = ["p50", "p99", "mx"] if grp != "spin" else ["thr", "stall"]
        mets += ["cpus", "sw", "w1s_max", "w100_max", "w100_over", "tw1s_max", "tw100_max", "tw100_over", "held", "t_n", "t_frac", "p_n", "p_frac"]
        for m in mets:
            e = S.get(("su", c, m))
            if not e:
                continue
            rows.append([c, UNIT.get(m, m)] + [cellv(e, v) for v in SV])
            jrow("sub-unit limits", "su", c, m, e, SV)
    table(f"Sub-unit: {title}", "Medians (min-max) over 3 boots per variant.", ["Case", "Metric"] + [LBL[v] for v in SV], rows)

# ---- 4. throttle counters -----------------------------------------------------------------------------------------
md.append("## Throttle counters (tables)\n")
TV = ["cfs", "kwidth", "kpfs", "scx", "kpfsr1", "cfs-burst", "cfs-pin"]
rows = []
for (t, c, m), e in S.items():
    if t in ("su",) or t.startswith("kit-"):
        continue
    if m in ("t_n", "t_frac", "c_t_n", "c_t_frac", "p_n", "p_frac", "c_p_n", "c_p_frac", "pod_t_n", "pod_t_frac",
             "ctr_t_n", "ctr_t_frac", "pod_p_n", "pod_p_frac", "ctr_p_n", "ctr_p_frac", "par_t_n", "a_t_n", "b_t_n",
             "par_t_frac", "a_t_frac", "b_t_frac", "par_p_frac", "a_p_frac", "b_p_frac", "a_t_frac", "b_t_frac") \
            or (t == "wt" and m.endswith(("t_n", "t_frac"))):
        lvl = "container" if m.startswith(("c_", "ctr_")) else "pod" if not m.startswith(("par_", "a_", "b_")) else m.split("_")[0]
        kind = "scx_pfs " if "p_" in m.replace("pod_", "").replace("par_", "") and not m.endswith("t_n") and "_p_" in "_" + m or m.startswith(("p_", "c_p_")) else ""
        base = m.split("_", 1)[1] if m.startswith(("c_", "pod_", "ctr_", "par_", "a_", "b_")) else m
        rows.append([t, c, f"{lvl}: {UNIT.get(base, base)}"] + [cellv(e, v, rng=False) for v in TV])
        jrow("throttle counters", t, c, m, e, TV)
rows.sort(key=lambda r: (r[0], r[1], r[2]))
table("Throttle counters per case", "Medians. nr_throttled counts CFS periods (100 ms) for CFS and cpu.max-period "
      "units for kPFS, 10 ms windows for kwidth and scx_pfs. throttled time / wall: CFS and kwidth sum over CPUs and "
      "can exceed 1; kPFS and scx_pfs report wall time with at least one waiter. scx_pfs counters come from its -S file "
      "(round-2 binary only); cpu.stat throttle fields stay 0 under scx_pfs.",
      ["Test", "Case", "Level: counter"] + [LBL[v] for v in TV], rows)

# ---- 5. full tables ---------------------------------------------------------------------------------------------
md.append("## Full tables, main matrix\n")
FV = ["cfs", "cfs-pin", "cfs-burst", "kwidth", "scx", "kpfs", "scxr1", "kpfsr1"]
order = ["mx", "nn", "latlim", "ulat", "wlat", "wt", "starve", "lk", "unl", "kh", "burst", "nest", "pg", "thr",
         "kit-cg", "kit-dock", "kit-systemd", "kit-docker-systemd", "kit-podman-systemd"]
SKIP = ("t_per", "c_t_per", "p_per", "c_p_per", "t_s", "p_s", "c_t_s", "c_p_s", "lim", "pod_t_per", "pod_p_per",
        "pod_t_s")
for t in order:
    cs = cells(S, t)
    if not cs:
        continue
    vs = [v for v in FV if any(v in S[(t, c, m)]["median"] for c in cs for m in metrics(S, t, c))]
    rows = []
    for c in cs:
        for m in metrics(S, t, c):
            if m in SKIP or "_t_s" in m or "_p_s" in m or "_per" in m:
                continue
            e = S[(t, c, m)]
            rows.append([c, UNIT.get(m, m)] + [cellv(e, v) for v in vs])
            jrow("full tables", t, c, m, e, vs)
    table(f"{t}: {DESC.get(t, '')}", "", ["Case", "Metric"] + [LBL[v] for v in vs], rows)

md.append("## Full tables, kPFS configs (arrows vs the default kPFS config in the same rounds)\n")
XV = ["kpfs", "kpfsnoext", "kpfsrt", "kpfssmt", "kpfscore"]
for t in ["mx", "nn", "latlim", "ulat", "wlat", "wt", "starve", "lk", "unl", "burst", "kh", "nest"]:
    cs = cells(SX, t)
    if not cs:
        continue
    rows = []
    for c in cs:
        for m in metrics(SX, t, c):
            if m in SKIP or "_t_s" in m or "_p_s" in m or "_per" in m:
                continue
            e = SX[(t, c, m)]
            rows.append([c, UNIT.get(m, m)] + [cellv(e, v, base="kpfs") for v in XV])
            jrow("kPFS configs", t, c, m, e, XV, base="kpfs")
    table(f"kPFS configs: {t}", "", ["Case", "Metric"] + [LBL[v] for v in XV], rows)

open(OUTMD, "w").write("\n".join(md) + "\n")
json.dump(J, open(OUTJ, "w"), indent=1, ensure_ascii=False)
print(f"{len(md)} md lines, {len(J['rows'])} json rows")
