# PFS measurement harness

`README.md` describes the single-machine kit (`cg-run.sh`, `lib.sh`, `compare.sh`,
`summary.py`, `conc.py`, `spin.c`, `reqlat.c`, the container and systemd runners).
This file describes the four subdirectories added beside it, which drive the
repeated, multi-boot measurements the kit alone cannot do, and reduce their output.

Nothing here runs a benchmark on import; every script is driven from a work
directory that the caller names.

## Parameters

No host, address, node name or label domain is compiled in. Each script takes
these from the environment:

| Variable       | Meaning                                              | Default |
|----------------|------------------------------------------------------|---------|
| `PFS_WORK`     | writable work directory holding builds and results   | required |
| `CTX`          | kubectl context, for the cluster scripts             | required |
| `NODE`/`NODE_B`| name of the node under test                          | `nodeB` |
| `NODE_A`       | name of the reference node                           | unset |
| `NODE_IP`      | address of the node being installed                  | required by `cluster/install-node.sh` |
| `SSH_A`        | `user@host` of the reference node                     | required where used |
| `SSH_B`        | `user@host` of the node under test                    | required where used |
| `LABEL_DOMAIN` | domain prefix for the throwaway k8s labels            | `pfs-test.example` |
| `PFS_PUBLISH_DENY` | regex the fragment generators refuse to publish   | addresses, `@`, absolute paths |
| `PFS_IMG`      | kernel build dir holding `arch/x86/boot/bzImage`      | `$PFS_WORK/fix3/b-pkgkpfs8` |
| `PFS_BINDIR`   | directory holding the `scx_pfs` binaries              | `$PFS_WORK/fix3/hp/bin` |
| `PFS_SCXBIN`   | the `scx_pfs` binary every scx arm loads             | `$PFS_BINDIR/cand-charge` |
| `PFS_SCXBIN_<arm>` | that one arm's binary instead, e.g. `PFS_SCXBIN_scx3` | falls back to `PFS_SCXBIN` |

A script that needs one of the required variables fails immediately with the
message in its `${VAR:?...}` expansion rather than falling back to a guess.

## Deployment: these scripts are not run from a bare checkout

The host-side drivers address each other through `$PFS_WORK`, not relative to
themselves - `run-calib.sh`, `seqtest.sh`, the queue drivers, `mech.sh`,
`mech-rev.sh`, `mech-make.sh` and `mech2.sh` all spell their guest-side
counterpart as **`$PFS_WORK/fix3/<dir>/<script>`**. They have to: the host
environment does not reach the guest through `vng --exec`, so a boot's guest
script is generated with absolute paths baked in. So the expected layout is

```
$PFS_WORK/fix3/calib/      \
$PFS_WORK/fix3/mech/        > this directory's contents, copied or symlinked
$PFS_WORK/fix3/rm2/        /
$PFS_WORK/fix3/hp/bin/     <- the scx_pfs binary every driver checks for
$PFS_WORK/fix3/b-*/        <- the kernel build trees the boots run
$PFS_WORK/fix3/*/res-*/    <- where results land; the queue drivers poll these
$PFS_WORK/p5/root/         <- the Postgres rootfs the cell driver runs
$PFS_WORK/kbuild-*-src/    <- kernel source for the build-load cell
```

Running `mech.sh` or `run-calib.sh` straight out of a git checkout will not
work, and the failure is a missing-file error from inside a guest rather than
anything obvious.

Two details worth knowing before chasing such an error:

* The `rm2/` suite's **own** drivers (`rm2/su.sh`, `rm2/vmx.sh`) address
  `$PFS_WORK/rm2/...`, not `$PFS_WORK/fix3/rm2/...`, because they predate the
  `fix3/` generation. A work directory that runs both suites therefore has
  `rm2/` in both places, and they are not required to be the same copy.
* Everything reached *through* a generated guest script resolves relative to
  itself, not through `$PFS_WORK`: `mech-guest.sh` -> `mech-pg.sh` ->
  `sample-groups.py` and `rm2/conc-pids.py`, and `calib-guest.sh` /
  `warmup-guest.sh` -> `rm2/pg-guest.sh`. Those are overridable with `MD`,
  `CONC` and `PG` respectively. So only the placement of the directory *as a
  whole* matters, and the warm-up provably runs the same cell driver as the
  cells it protects.

## `calib/` - per-boot calibration and the two-factor fit

The paired ratio between two schedulers has a sigma only across boots, so this
drives several boots per kernel family and keeps one file per boot, so a run cut
short still yields a smaller usable n.

* `run-calib.sh [ROUNDS] [OUT] [FIRST]` - the per-boot runner. Boots each family
  `ROUNDS` times, records the host load on both sides of every boot, and runs one
  discarded warm-up cell first so the first measured arm does not pay the
  first-run cost. The arm order rotates with round parity.
* `calib-guest.sh`, `warmup-guest.sh` - what runs inside the guest per arm.
* `warmcheck.py` - what the discarded first run was worth.
* `seqtest.sh`, `seq-queue.sh`, `tail-queue.sh`, `rerun-queue.sh` - the sequence
  test (the same arm twice in a boot) and the queue drivers.
* `exclude.py` - the boot exclusion rule, applied mechanically: a boot is dropped
  whole on under-delivery by the reference arm, or on a recorded load average
  over the threshold. Which files it treats as boot files is the single
  `exclude.BOOT_FILE` pattern, reused by `final.py`; it covers the calibration
  rounds, the sequence test and the `mech`/phase-2 boots. That pattern is a
  filename filter only - it decides what the rule is applied to, never what the
  rule decides - and it is widened whenever a driver adds a boot-name shape,
  because an unmatched shape is skipped in silence and the run then reports
  `boots seen 0`, which reads like a clean result instead of like missing
  data.
* `position.py` - the within-boot position effect as a number of its own.
* `final.py [ROUNDS_DIR [SEQ_DIR]]` - with no argument it reads `res-r4b` and
  `res-seq` beside itself, exactly as before. Given directories it runs the
  same code over those, so `res-mech`, `res-mechrev` and `res-mech2` can be
  reported without copying anything. What it computes does not depend on the
  argument: the family pairing still keys on the `-f1`/`-f2`/`-f3` suffix, so a
  directory whose boots are named `m*`/`p*` yields empty F1/F3/F2 tables and a
  populated per-boot position table, until that pairing is extended.
* `headline.py` - the equal-CPU work ratio at one limit, three ways.
* `final.py` - the result over the kept boots: position effect, the **two-factor
  (arm + position) contrast** per cell with delivered CPU beside every ratio, and
  the paired per-boot ratio with a one-sample t test on the log ratio.
* `boots.py`, `report.py`, `sigma.py` - per-boot inventory, report and sigma.

### The pre-registered correction for the kPFS arm

Written down **before** the reversed-order rounds are reduced, so the choice
cannot be made after seeing the numbers.

**(a) Position is cancelled by the balanced order plus the within-boot fit.**
The `m1` boots are run in both cell orders - forward (`4 4.001 2 2.001`) and
reversed (`2.001 2 4.001 4`, `mech-rev.sh`) - in equal numbers, so every cell
appears in both positions across the round set. On top of that, the
**two-factor (arm + position)** fit already in `final.py` is applied *within*
the `m1` boots: each arm's mean is taken inside a position first and the two
positions are then weighted equally, so an unequal split of kept boots between
positions cannot tilt the result. Those two together are the whole correction
for kPFS.

**(b) The CFS step divisor applies only to arms that have an in-boot CFS
reference, i.e. the `m2` arms.** For `m2` (`cfs` + `scx3` in one boot) the CFS
arm's own step from one cell to the next measures the order effect directly,
and dividing by it is legitimate. For `m1` it is not available and **must not
be borrowed from another boot**: a cross-boot divisor would carry that other
boot's variance into the one number the run exists to produce. The `m2` CFS
arm's step is still **reported separately, as a cross-check** on whether the
balanced order did its job - never as a divisor for `m1`.

**(c) Why no in-boot CFS arm exists under `sched_pfs=1`.** It is not an
oversight in the harness, it is the kernel: with `sched_pfs=1` every normal
task is on PFS, and the fair class is not even iterated.

* `kernel/sched/sched.h:3094` - `normal_sched_class()` returns
  `&pfs_sched_class` when `pfs_enabled()`, otherwise `&fair_sched_class`.
* `kernel/sched/sched.h:3074` - `next_active_class()` skips
  `fair_sched_class` when `pfs_enabled()` ("PFS replaces the fair class when
  enabled"), so `for_each_active_class()` never visits it.
* `kernel/sched/sched.h:2046` - `pfs_enabled()` is a static key set once at
  boot, from the `early_param("sched_pfs", ...)` at `kernel/sched/pfs.c:59`
  via `pfs_boot_enable()` at `:62`. It cannot be toggled at run time, which is
  why `run-calib.sh`'s F2 family has no in-boot baseline either.

**(d) The warm-up stays, whatever the correction does.** `calib/warmup-guest.sh`
and the cell driver's own warm-up cell shrink the position effect; they do not
remove it - the measured first-run cost was 1.66-2.00x on `tpcb@2 c16` before
the warm-up existed, and a residual position effect was still visible with it
in place, which is why the balanced order exists at all. One does not replace
the other: drop the warm-up and the first measured cell of every boot pays the
first-run cost again; drop the balanced order and the residual stays
uncorrected. Both are required.

## `rm2/` - the repeat-measurement suite

* `su.sh`, `waitn.sh`, `build.sh` - suite driver, boot barrier, image build.
* `pg-guest.sh`, `guest-remeasure.sh`, `retest-guest.sh`, `vmx.sh` - in-guest
  drivers and the VM wrapper.
* `summ2.py`, `mkrep2.py` - reduction and report generation.
* `conc2.bt` - bpftrace probe for concurrent CPUs of a cgroup.
* `conc-pids.py` - time-weighted concurrent CPUs from a `sched_switch` dump,
  selecting by **PID set**. The in-tree `conc.py` does the same by **comm**; both
  are kept because the suites that select by pidfile cannot use a comm filter.
* `diag-acct.sh`, `foreign.sh` - accounting diagnosis, foreign-task check.

## `mech/` - the mechanism phase

`mech.sh`, `mech2.sh`, `mech-rev.sh`, `mech-make.sh` with their guest side
(`mech-guest.sh`) and queue drivers, plus `sample-groups.py` and `swlat.py`
(switch-latency distribution from a trace).

### What is being measured is read, not asserted

`mech.sh` and `mech-make.sh` take the kernel image and the scheduler binary
from `PFS_IMG`, `PFS_BINDIR`, `PFS_SCXBIN` and the per-arm `PFS_SCXBIN_<arm>`,
defaulting to the paths the harness has always used. The guard line they print
before the first boot states what is actually there:

```
guard: img=<path> release=<str|unknown> release_src=<kernel.release|file|none> \
       bzimage_sha=<16 hex> scxbin=<path> scxbin_sha=<16 hex>
```

`release` comes from `$PFS_IMG/include/config/kernel.release`, which is what
kbuild writes for the tree that produced the image (`Makefile:1410-1412`, part
of `archprepare` at `:1429`); `file(1)` on the bzImage is the fallback, and if
neither can be read the field says `release=unknown` with `release_src=none`.
It is never omitted: a missing field reads as "not checked". There is no
hardcoded commit in the guard any more - asserting a commit that is not the one
under test silently mislabels every row the run produces, which matters most in
exactly the runs that compare builds.

Each boot's result file then carries its own provenance, so a reader never has
to trust a directory name:

```
D prov boot=<name> img=<path> release=<str> release_src=<src> \
       bzimage_sha=<hex> scxbin=<path> scxbin_sha=<hex> arm_bins="<arm>=<path> ..."
```

The row is inert to every analysis script: it does not match `^D arm=`, nor
`^D boot kernel=\S+ arm=` (`final.py`'s position counter), nor
`^D arm=\S+ cell=pg clients=`, nor the `load_before`/`load_after` scan, nor
`^(\S+)\s+rc=` in `run.log` - checked against all of them.

**Per-arm binaries.** `mech-guest.sh` takes the binary from `BIN` in the
environment and bakes no path in, and `mech-pg.sh` never looks at it at all, so
pointing one arm at its own build needs no change to the call chain:
`PFS_SCXBIN_scx3=<path>` is enough. Two scx builds in **one boot** additionally
need a second arm name, and `mech-guest.sh` does not accept one yet - it aborts
with `R ABORT: unknown arm` on anything but `cfs`, `scx3` and `kpfs`, and the
per-arm output directory is keyed on the arm name, so the same name twice would
collide. Until that is extended, two builds are compared across two boots of
the same family rather than within one.

`mech2.sh`, `calib/run-calib.sh` and `calib/seqtest.sh` still hardcode the
image, the binary and a commit in their guard lines. They are untouched here on
purpose; the same four variables would apply to them unchanged.

## `kit/` - shared reduction and reporting

`combine.py`, `combine_rm.py` merge result trees; `gen_frag.py`,
`gen_frag_rm.py` render report fragments and refuse to emit anything matching
`PFS_PUBLISH_DENY`; `retest-mkout.py`, `retest-summarize.py`,
`scx-fix-combine.py`, `finalize.sh`, `remeasure5-finalize.sh` finish a round.
`conc.bt` is the cgroup concurrency probe and `fanout.c` a thread-fanout load.

## `cluster/` - the Kubernetes-node harness

For the runs that need a real node and real services rather than a guest:
`install-node.sh`, `fallback-node.sh`, `drain.sh`, `ops.sh`, `ops-lib.sh`,
`fail-check.sh`, `timeline.py`, `a-kill.sh`, `b-churn.sh`, and the database and
service workloads `dbsvc-*.sh` / `dbsvc-*.py`.

All objects these create carry a `${LABEL_DOMAIN}` owner/expiry label so a run
that dies leaves something sweepable behind.

## `mech/mech-pg.sh` - the Postgres cell driver

One invocation is one arm's Postgres cells inside one boot: Postgres in a
two-level pod cgroup (`/sys/fs/cgroup/rt/pod/ctr`), pgbench driven from the
root cgroup, one cell per (limit, workload, rep) **in the order given**.

`mech-guest.sh`, `mech-make.sh` and the warm-up path all call it; nothing else
in the tree does.

### Call

```
[env] bash mech/mech-pg.sh "LIMITS" "WLS" SECS
```

| Position/variable | Meaning | Default |
|---|---|---|
| `$1` `LIMITS` | `cpu.max` limits in CPUs, **in cell order** | `4 1.5` |
| `$2` `WLS` | workloads in cell order: `tpcb`, `select` | `tpcb select` |
| `$3` `SECS` | measured seconds per cell | `30` |
| `PFS_WORK` | work directory; the Postgres rootfs is `$PFS_WORK/p5` | required |
| `MD` | directory holding `sample-groups.py` | the script's own directory |
| `CONC` | the concurrency reducer | `../rm2/conc-pids.py` |
| `CL` | pgbench clients (the harness uses 16 and 32) | `32` |
| `REPS`, `SCALE`, `GRPHZ` | reps per cell, `pgbench -i` scale, sampler Hz | `1`, `20`, `100` |
| `PGV` | the `v=` label on every `R` row | `cfs` |
| `REV` | `1` reverses both `LIMITS` and `WLS` in place | `0` |
| `WARM` | `1` force / `0` off / `auto` once per boot | `auto` |
| `WARMSECS`, `PG_WARM_STAMP` | warm-up cell length, per-boot stamp file | `10`, `/tmp/pg-warmed` |
| `PG_GATE` | if set, wait **unboundedly** for the guest 1-minute load to fall below it | unset = no gate |
| `SCXS`, `SF`, `SCXL` | set by the caller for an `scx_pfs` arm: enable the per-cgroup dump, its path, the loader log | unset |

Limits are fractional, and the integer part may be 2 or more: `1.5 2 2.001 2.1
2.5 2.9 2.999 3 3.5 4 4.001` all convert exactly through
`printf "%d", l * 100000` into a quota against a 100 ms period. `setlim` now
**reads `cpu.max` back** and aborts on any mismatch, and prints the quota it
wrote as `D limset`, so a rounding surprise is visible rather than silent.

Cell order is a parameter because a fixed order confounds every step
comparison: `mech-rev.sh` passes `LIMITS="2.001 2 4.001 4"`, and the order in
force is echoed in the `D pgdriver` row.

### Rows

Every rate is a **delta across a measured window, with the window printed
beside it**. `cpu.stat`'s `usage_usec` is cumulative and is never divided by a
nominal duration.

```
R v= lim= wl= rep= tps= lat_ms= cpus= mid_cpus= cell_secs= mid_secs= cpu_src= conc_pids= \
  trace_secs= mean_running= frac_over_limit= max= hist= \
  thr_win= thr_per= thr_n= thr_us= thr_frac= [pfs_per= pfs_n= pfs_us= pfs_frac=] \
  c_thr_win= c_thr_per= c_thr_n= c_thr_us= c_thr_frac= [c_pfs_*] [kPFS stats delta]
```

* `cpus` = delta of `/rt/pod` `usage_usec` over `cell_secs`; `mid_cpus` = the
  same delta over the 5 s mid window, `mid_secs`. `cpu_src` names the cgroup
  both come from and `conc_pids` the size of the thread set the trace filter
  matched, so each denominator states its interval **and** its task set.
* `trace_secs mean_running frac_over_limit max hist` come from the concurrency
  reducer over the mid-window `sched_switch` dump.
* `thr_*` are `/rt/pod` `cpu.stat` throttling; `c_thr_*` the `ctr` leaf.
  `pfs_*`/`c_pfs_*` appear only when the `scx_pfs` dump is enabled.
  `thr_win`/`c_thr_win` is the measured snapshot-to-snapshot interval the
  `_frac` fields are divided by.
* The trailing bare `key=value` tokens are the kPFS `debug/sched/pfs/stats`
  delta over the cell.

Field order up to `mid_cpus=` is fixed, because `calib/exclude.py:75`,
`calib/final.py:55`, `calib/report.py:38`, `calib/position.py:32` and
`calib/headline.py:29` all anchor on that prefix; `exclude.py` additionally
reads `\bthr_frac=` by name, and `rm2/summ2.py` reads `lim`, `wl`, `tps`,
`cpus`, `lat_ms`, `mean_running` and the `thr_*`/`c_thr_*`/`pfs_*` set as an
unordered `key=value` map (`summ2.py:98`, `:23`). Every field added since the
original is appended **after** `mid_cpus=`, so all five regexes still match -
verified against each one.

`D` rows: `D pgdriver` (one per invocation: kernel, variant, cell order, every
knob), `D limset` (per cell: quota written and read back), `D pgload`
(loadavg on both sides of the run, into this stream and so into the per-boot
file the exclusion rule reads), `D pgwarm` (the discarded warm-up cell, with
its own tps and window), `D cell` (window, transactions, `tokens_ns` either
side), `D pgprog` (the `pgbench -P 1` progress series), `D scx0`/`D scx1`
(scx_pfs global counters either side), `D grpsample` (the sampler's own
trailer, including its CPU cost), `D cgstale` (a cgroup an earlier arm left
behind) and `D warn`.

`G` rows are the 100 Hz per-cgroup samples across the mid window:
`G lim= wl= t= limit= slots= waiting= tokens_us= active= path=`. `slots <
width` with `waiting > 0` is work the scheduler held back; `slots < width`
with `waiting == 0` is work the benchmark did not offer. Nothing else
separates them. `path=` is on every row because without it a sample's pod and
leaf rows cannot be told apart, which is how the first run lost slot
attribution.

The driver ends with `PG-DONE`. Callers filter its output with
`grep -E '^R |^D |^G |failed|PG-DONE'`.

### Recovered verbatim vs reconstructed

This file was **rebuilt after the original was lost**, and the two halves are
not equally trustworthy.

**Recovered verbatim** (the original is `rm2/pg-guest.sh` plus eight
replacements, and the rebuild reproduces the original's own line numbering -
`D grpsample` at line 113 and the `G` row `awk` at 114, matching the recovered
listing of the live file):

* the header's phase-1 argument and its `pfs.c` / `scx_pfs.bpf.c` citations;
* the cgroup layout, `setlim`'s conversion, the tmpfs, `initdb`, the Postgres
  command line and every `pgbench` invocation including `-n -c $CL -j 4 -T
  $SECS -P 1`;
* `tsnap`, `toksnap` (`tokens_ns`, field 9 of the scx_pfs `PLAINFMT` line) and
  `scxsnap` with its `drain=N/M` split;
* the cell loop, the `vacuum analyze` + `checkpoint` before each cell, the
  1.1 s settles around the scx dump, the 5 s mid window with the group sampler
  started inside it and waited for before it closes;
* the `R`, `D cell`, `D pgprog`, `D scx0`, `D scx1`, `D grpsample` and `G` rows
  and the whole-cell transaction parse that handles both `pgbench` forms.

**Reconstructed** - derived from the call sites and the measurement rules, not
recovered. Each site is marked `# RECONSTRUCTED:` in the file:

* `PFS_WORK`-rooted paths, `set -u` with the previously unguarded `SCXS`,
  `SF`, `SCXL`, `P5RES`, `TAG` defaulted, `MD` defaulting to the script's own
  directory and `CONC` naming the reducer (the original called it `conc5.py`
  in its own directory; the in-tree name is `rm2/conc-pids.py`, same output);
* `cell_secs`, `mid_secs`, `cpu_src`, `conc_pids`, `thr_win`, `c_thr_win` on
  the `R` row, and `thr_frac`/`pfs_frac` divided by the **measured snapshot
  interval** rather than the pgbench interval. This last one is a deliberate
  change of value, not just of presentation: the original divided by `t1-t0`,
  which under an scx arm is short of the snapshot span by the two 1.1 s
  settles. For `cfs` and `kpfs` arms no settle runs and the two denominators
  agree to within microseconds, so only scx `pfs_frac` moves;
* `setlim`'s read-back and the `D limset` row;
* the stale-cgroup check. Nothing here resets a counter by recreating a
  cgroup, but a leftover `/rt` tree means an earlier arm's teardown failed, so
  the removal is attempted and **a failure is fatal**. An older harness let
  exactly this `rmdir` fail silently and read 4.9 CPUs where the limit was
  1.5;
* `D pgload` and the `PG_GATE` wait, which has no cap and no fall-through;
* `REV`, and the `warmcell` discarded cell in the shape of the **first cell of
  the order in force**. `calib/warmup-guest.sh` also runs a discarded cell per
  boot, but at a fixed shape (limit 2, `tpcb`, 16 clients) that stops matching
  as soon as a caller reverses the order, which is why the driver has its own;
  `WARM=auto` pays it once per boot via a stamp file and `WARM=0` says
  `warm=off` rather than omitting it in silence.

**Known defects carried over on purpose**, so a re-measurement is comparable
with the data already collected:

* `v=` is `${PGV:-cfs}` and is *not* an arm label: on an `scx3` arm the rows
  still read `v=cfs`. `calib/final.py:17` says so and keys on the `D arm=`
  line instead. Set `PGV` to override.
* When the `sched_switch` dump is empty the reducer prints `no events` and the
  `R` row then carries no `trace_secs`/`mean_running`/`hist`.
