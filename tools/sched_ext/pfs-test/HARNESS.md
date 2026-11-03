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
| `PFS_IMG_KWIDTH` | the kwidth kernel build, for `run-calib.sh`'s family f3 | `$PFS_WORK/fix3/b-pkgkwidth4` |
| `ARMS_M1`, `ARMS_M2` | which arms `mech.sh`/`mech-make.sh` run per family | `kpfs`, `cfs scx3` |
| `FAMS` | which `mech.sh` families to boot; an unknown name aborts rather than booting nothing | `m1 m2` |
| `CL` | clients per cell, passed into the guest by `mech.sh` | `16` |
| `BOOT_TIMEOUT` | seconds one `mech.sh` boot may take; the default scales with the arm count | `600 + 900 x arms` |

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
* **`vng --exec` does not carry the host environment into the guest.** Anything
  a guest-side script requires must therefore be baked into the generated
  `*.guest.sh` line, which is why those lines spell out `PFS_WORK`, `BIN`,
  `LIMITS`, `KB` and `KSRC` explicitly. A guest-side script may rely on the
  environment only for a variable that has a working `${VAR:-default}`. The
  test for this is to run a generated line under `env -i`: if it trips a
  `${VAR:?}` guard there, it will die in the guest too, however well it works
  from the launcher's shell.

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
* `slots.py RESDIR [ARM...]` - where kPFS refuses a slot, per **limit** and per slot
  index, from the per-cell `refuse_*` deltas on the R rows. It prints no table it
  cannot stand behind: boot files whose R rows carry no `refuse_keys=` came from a
  harness that did not take the counters, and `refuse_keys=0` everywhere means the
  kernel has none; both exit non-zero and say which, rather than rendering as zeros.
  It also checks the identity `refuse_debt_guar_* + refuse_debt_frac_* == park_debt`
  per limit, which is what makes a zero in those buckets trustworthy. `park_budget`
  is **not** the same relation and is not checked as one: the debt counters are
  incremented inside `pfs_take_slot()` beside the refusal, while `park_budget` is
  incremented in `pfs_waiter_linked()` and so counts only the budget refusals that
  actually parked a task. Its share of the refusals is printed instead - measured at
  0.08% (limit 2.5) and 1.81% (limit 3.5), i.e. the window-timer retry absorbs
  almost all of them.
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
`PFS_SCXBIN_scx3=<path>` is enough. The arms `mech-guest.sh` accepts are `cfs`,
`kwidth`, `kpfs`, `scx3` and `scx3b`; anything else is `R ABORT: unknown arm`.
`mech.sh` checks the names itself **before** it pays for a boot, and also
rejects the two placements that cannot work - `kpfs` outside the `sched_pfs=1`
family, and anything but `kpfs` inside it. The per-arm output directory is keyed
on the arm name, so the same name twice in one family would collide.

`mech2.sh`, `calib/run-calib.sh` and `calib/seqtest.sh` take the same
variables and print the same measured guard. `run-calib.sh` boots two images and
so prints two guard lines, `role=f1f2` for the PFS build and `role=f3` for the
`PFS_IMG_KWIDTH` build, and each boot's `D prov` row carries the image **that
boot** used rather than a global. No driver in the tree asserts a commit any
more.

### Two cgroups report throttling, and they disagree

`mech-pg.sh` reads `cpu.stat` twice per cell: from the **parent** that carries
`cpu.max` (`thr_*`) and from the **container leaf** below it (`c_thr_*`).
`delivery.py` shows both, because reading only one of them has already produced
a wrong conclusion once.

| arm | parent `thr_cpu` | leaf | parent events/s |
|-----|------------------|------|-----------------|
| `cfs` | 0.48 - 3.50, falls as the limit rises | 0.42 - 2.72 | 9.8, once per 100 ms period |
| `kwidth` | 2.08 - 6.25 | 2.12 avg | 98.5, ten times per period |
| `kpfs` | **0.00 at every limit** | **0.98 - 0.99 at every limit** | 0.1 - 3.5 |
| `scx3` | not accounted | **0.00, genuinely measured** | not accounted |

Three things to take from it:

* `thr_cpu` is **not a percentage and not bounded by 1**. It is
  `throttled_usec / 1e6 / window`, and `throttled_usec` accumulates per
  `cfs_rq`, so a group spread over several CPUs exceeds 1 routinely. An earlier
  version of this script printed the column under a `thr%` header, where stock
  CFS's 3.50 read as 3.5% and meant 350%.

* Under kPFS the limit-bearing parent accumulates **no throttled time at all**
  while the leaf sits at ~0.99. The leaf's figure is the same at a limit of 1
  and at a limit of 4, and a quantity that does not move with the limit is not
  measuring the limit: it looks like one `cfs_rq` left permanently marked
  throttled, not like the group being stopped. Neither number on its own
  supports "kPFS does not throttle" - the parent counts 0.1-3.5 **events** per
  second while accumulating ~0 throttled **time**, i.e. it enters the state and
  leaves it immediately.

* The leaf is **not** a way round the sched_ext `n/a`. Under `scx3` both
  cgroups read a real 0.00, so the parent's `n/a` is not hiding a usable signal
  somewhere else. For that arm the throttling signal is only in the loader's own
  counters.

### `calib/delivery.py` - the reduction that needs no position balance

Delivered CPU is an absolute measurement of one arm against its own cgroup limit, not a
contrast between two arms, so it is valid in a run with any number of arms - including the
three-arm runs `mech.sh` refuses to contrast. Those runs are not wasted; they are delivery
runs, and `ALLOW_UNBALANCED=1` is how you ask for one on purpose.

```
python3 calib/delivery.py <resdir>
```

Per arm and cell: delivered CPU against the limit, throttle events per second, the
throttled fraction, and **work per delivered CPU** - the column that separates "did not
get the CPU it was allowed" from "got it and did less with it". Then a per-arm summary:
worst and mean delivery, how many cells fall below 95%, and the mean throttle rate.

Two numbers it will not print:

* **`n/a`, not `0.0`, for a sched_ext arm's throttling.** Loading a BPF scheduler takes the
  tasks out of the fair class, so the CFS bandwidth counters cannot move. `cpu.stat` still
  *holds* a zero, and `mech-pg.sh` duly records it, so "the field is missing" is the wrong
  test. A printed `0.0` there would read as "never throttled" when it means "not accounted
  here". The rule is structural and named in `NO_CPU_STAT_THR`.
* **An unflagged all-zero column.** An arm that does report those fields and never moved
  them on any cell is called out, because a counter that is never incremented looks
  exactly like one that had nothing to count.

### `which-variant.sh`, and the test that covers its precedence

`which-variant.sh` answers "what is enforcing `cpu.max` on this machine" in the
four reads that `VARIANTS.md` explains, printing the evidence and then the
variant name on a line of its own, so `$(which-variant.sh -q)` is the name and
nothing else.

Its four paths come from the environment (`WV_PFSD`, `WV_SCX`, `WV_KW`,
`WV_CMDLINE`) for exactly one reason: so that every branch can be exercised
against fixtures. Nothing in normal use overrides them.

`which-variant-test.sh` does that. It needs no root, no kernel features, no
network, and exits non-zero on the first wrong answer. The branches it covers
that no single machine can show are the **precedence** ones, where the width
sysctl reads `1` while something else is running the tasks:

| fixture | answer | why it is not `kwidth` |
|---------|--------|------------------------|
| width=1, `sched_pfs=1`, pfs debugfs present | `kPFS` | the fair class, which the sysctl steers, is not what runs the tasks |
| width=1, `sched_ext/state` = `enabled` | `scx_pfs` | the same, for the BPF scheduler |
| width=1, `sched_ext/state` = `disabled` | `kwidth` | sched_ext present but not attached must fall through, not match on the file existing |

A checker that read the cheap sysctl first would answer `kwidth` for the first
two with every file reading exactly as it should. That is why the order in
`VARIANTS.md` is normative and why these three cases have a test rather than a
sentence.

One expectation in that test is uid-dependent, and it is the interesting one.
With `sched_pfs=1` on the boot line and no pfs debugfs, a **root** caller gets
`cfs`, because the absence is conclusive; a **non-root** caller gets
`kPFS-unconfirmed`, because it cannot tell "the directory is not there" from "I
am not allowed to look", and those mean opposite things. The test asserts the
right one for the uid it runs as and says when the other is not exercised. An
earlier version of it expected `cfs` at any uid and failed: the script was
right and the test was wrong.

### The four variants, and the `kwidth` arm

All four implementations of the same policy are reachable from one kernel image,
because `b-kpfs10` carries both `CONFIG_SCHED_PFS` and the width-mode sysctl:

| arm | family | what enforces `cpu.max` | how it is selected |
|-----|--------|-------------------------|--------------------|
| `cfs` | `m2` (plain boot) | stock CFS bandwidth control | `sched_cfs_bandwidth_width=0` |
| `kwidth` | `m2` (plain boot) | the width policy inside CFS bandwidth control, v1 | `sched_cfs_bandwidth_width=1` |
| `scx3` | `m2` (plain boot) | `scx_pfs`, a sched_ext BPF scheduler | the loader attaches |
| `kpfs` | `m1` (`sched_pfs=1`) | `CONFIG_SCHED_PFS`, a scheduling class | the boot parameter |

`cfs` and `kwidth` differ **only** in that sysctl, so the contrast between them
is the width policy and nothing else, measured inside one boot against the same
Postgres instance.

Three ways the `kwidth` arm could quietly measure stock CFS and publish it under
the kwidth label, each of them fatal rather than reported:

* the kernel does not carry the patch, so `/proc/sys/kernel/sched_cfs_bandwidth_width`
  does not exist - `R ABORT: ... this kernel has no width mode`;
* the file exists but the write did not take - the arm **reads the sysctl back**
  and requires `1`;
* the boot is a `sched_pfs=1` boot, where the fair class is not what runs the
  group and the sysctl governs nothing - refused before the cells run.

The `cfs` arm reads the sysctl back the same way and requires `0`, because the
`kwidth` arm of the same boot can have left it at `1`. For the same reason
`kwidth` restores it to `0` when its cells are done: without that the arm
**order** would decide whether a boot completed, which is not a property a
harness should have.

```
ARMS_M2="cfs kwidth scx3" LIMITS="1 1.5 2 2.5 3 3.5 4" CL=32 \
  bash mech/mech.sh <outdir>
```

That is three arms in one boot. The boot timeout is no longer the constant
`2400` that was sized for two of them - it is `600 + 900` per arm, which
reproduces `2400` at two arms exactly, so nothing already measured was timed
differently. `BOOT_TIMEOUT` overrides it.

### Comparing two scheduler builds inside one boot

`mech-guest.sh` accepts a second scx arm, **`scx3b`**, handled by the same
branch as `scx3` and differing in nothing but the name - same cells, same `BIN`
handling, same row conventions - so a `scx3` vs `scx3b` contrast measures the
two binaries and not the harness. Point them at the two builds and ask for both:

```
ARMS_M2="scx3 scx3b" \
PFS_SCXBIN_scx3=<build A> PFS_SCXBIN_scx3b=<build B> \
  bash mech/mech.sh <outdir>
```

Each arm gets its own output directory (`<boot>-scx3`, `<boot>-scx3b`) and so
its own `scx-stats.log`, and the `D prov` row's `arm_bins` field records which
binary each arm actually loaded. The scx teardown now waits for
`/sys/kernel/sched_ext/state` to leave `enabled` before an arm's process exits,
bounded at 10 s, because the next arm's loader attaches immediately after.

**The locality run is two arms, and three would be wrong - not merely
discouraged.** `final.py` and `position.py` compute a contrast between position
1 and position 2 and nothing else (`final.py:170`, `position.py:43-44`). With
three arms there are three positions, so one arm's cells sit outside the only
contrast that exists: `ARMS_M2="cfs scx3 scx3b"` would put `cfs` and `scx3` in
the compared pair and leave `scx3b` - one of the two builds the run exists to
compare - in position 3, contributing rows that no number is computed from. And
`cfs` is not wanted in that boot for its own sake either: the question is build
A against build B at equal CPU, which the `scx3`/`scx3b` pair answers directly.
So:

```
round 1, 3, 5 …   ARMS_M2="scx3 scx3b"
round 2, 4, 6 …   ARMS_M2="scx3b scx3"
```

Alternating the order across rounds puts each build in each position equally
often, which cancels position exactly the way the reversed cell order cancels
it for the kPFS arm, and the two-factor fit in `final.py` then weights the two
positions equally. One consequence to be aware of: with no `cfs` arm in the
boot, the exclusion rule's delivery test (A) cannot fire - it is restricted to
`cfs` on purpose - so those boots are evaluated by the load test (B) alone.

**What the analysis knows about arm names.** `exclude.py`, `final.py`'s
`read()`/`positions()`, and `position.py` treat the arm as an opaque label, so
`scx3b` flows through them unchanged. The two pairing lists carry it
explicitly, **appended** to what was there - no existing pairing was altered,
reordered or removed, because this is a pre-registered analysis surface:

* `calib/final.py:208-212` - `("m2", "scx3", "scx3b", "M2 …")` after the
  existing `f1` and `f3` entries.
* `calib/report.py:48-52` - `("scx3b", "scx3", "in-boot")` after the existing
  three.

`calib/headline.py` is deliberately untouched: it is the scx3-vs-cfs headline at
one limit by definition, and the locality contrast is not that.
`rm2/summ2.py:9` has a fixed default variant list but takes it on argv and keys
on the `v=` field of an `R` row rather than on the arm, so it is unaffected.

### Which files are boot files, and whose - one definition

`exclude.BOOT_FILE` decides what the exclusion rule is applied to.
`exclude.boot_id()` beside it answers the related question of **which family and
round** a boot name carries, and `position.py` and `report.py` both import it.
They each used to keep their own copy, and each silently saw zero `m*`/`p*`
boots as a result - `position.py` matched only `^r(\d+)-(f\d)\.txt$` and
`report.py` globbed only `r*-f*.txt`, so a `res-mech` or `res-mechrev`
directory produced an empty table rather than a result.

The leading token **is** the family: `mech.sh`'s header documents `m1` as the
kPFS family and `m2` as the cfs+scx family, and `mech2.sh` continues them as
`p1`, `p1b` and `p2`, exactly as `f1`/`f2`/`f3` name the calibration families. A
boot with no `-r<N>` suffix reports round 0.

`boot_id()` deliberately does **not** match the `s<N>-` sequence boots that
`BOOT_FILE` does: those carry neither a family nor a round, have never been in
these two reports, and have their own section in `final.py`. That is the only
way the two disagree, and it is asserted in the checks rather than left to
inspection.

`final.py`'s family selector accepts both spellings - an `f<N>` family is
matched against the calibration shape `r<N>-f<N>` only, and `m*`/`p*` families
against the leading token - so a mech boot that happens to end in `f1`, such as
`m2-f1`, cannot be pulled into family `f1`.

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
  delta over the cell: the aggregates (`admit`, `credit`, `park_full`,
  `park_budget`, `park_fifo`, `rotate`, `handoff`, `user_park`, `credit_ns`,
  `slot_ns`, `park_debt`, `wake_park`) **and** the per-slot refusal counters
  `refuse_budget_frac_<i>`, `refuse_debt_guar_<i>`, `refuse_debt_frac_<i>` for
  `i` in `0..7` plus `_hi` (`pfs.c`: `pfs_refuse_names`, `PFS_K_SLOT_IDX_MAX`).
  Taking the refusal counters **per cell** is the only thing that attributes a
  refusal to a **limit** - they are cumulative and machine-wide, so the single
  dump `mech.sh` takes at warm-up covers one limit and is not comparable with
  the per-cell `scx_pfs` numbers. `calib/slots.py` reads them.
* `refuse_keys=` and `refuse_nobase=` are published beside them and are not
  measurements: `refuse_keys` is how many refusal counters the dump carried, so
  a kernel without them (`refuse_keys=0`) is distinguishable from a measured
  zero, and `refuse_nobase` counts the ones that appeared only in the second
  snapshot, whose "delta" would be their whole history. Reading an absent
  counter as a zero is the single failure this harness has paid for most.

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
