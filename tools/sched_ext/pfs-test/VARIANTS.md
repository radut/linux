<!-- SPDX-License-Identifier: GPL-2.0 -->
# The four variants: how to select one, and how to see which one is running

Four different pieces of code can enforce the same CPU limit. They are not
alternatives you configure per workload - three of them are chosen when the
machine boots or when a scheduler is loaded, and they then apply to everything
on that machine. This document says how to turn each on, how to **verify** it is
on rather than assume it, and what each one shows you while it works.

If you only want one answer: run
[`which-variant.sh`](which-variant.sh), which prints the active variant and the
evidence it used.

## What they all have in common

Every one of them reads **one** setting: the cgroup v2 file `cpu.max`, holding a
quota and a period in microseconds.

```
# 2.5 CPUs: 250 ms of CPU time allowed per 100 ms of wall clock
$ cat /sys/fs/cgroup/my.slice/cpu.max
250000 100000
```

Nothing else is consulted, which is why the same limit reaches all four from any
of the usual places:

| You write | It becomes | Where |
|-----------|------------|-------|
| `resources.limits.cpu: "2500m"` | `250000 100000` | Kubernetes pod spec |
| `--cpus=2.5` | `250000 100000` | `docker run` / `podman run` |
| `--cpu-period=100000 --cpu-quota=250000` | `250000 100000` | `docker run`, explicit form |
| `CPUQuota=250%` | `250000 100000` | systemd unit or `systemd-run` |
| `echo '250000 100000' > cpu.max` | itself | plain cgroup v2 |

So **you do not change your manifests to try a different variant.** You change
the machine and run the same workload.

A caveat worth knowing before you compare anything: a limit of `250000 100000`
is 2.5 CPUs to CFS, which may use them in any pattern, and 2.5 *slots* to the
other three - two tasks on CPUs at all times plus a third for part of each
window. The two are the same ceiling on consumption and a different shape of
delivery, and that is the whole point of the comparison.

## Telling them apart in four commands

Run these in order; the first one that answers decides, because the lower ones
cannot be active while a higher one is.

```sh
# 1. kPFS - a scheduling class of its own, chosen at boot
grep -qw sched_pfs=1 /proc/cmdline && echo "kPFS" 

# 2. scx_pfs - a sched_ext BPF scheduler, loaded at run time
[ "$(cat /sys/kernel/sched_ext/state 2>/dev/null)" = enabled ] &&
	echo "sched_ext: $(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)"

# 3. kwidth - the width policy inside CFS bandwidth control, a sysctl
[ "$(cat /proc/sys/kernel/sched_cfs_bandwidth_width 2>/dev/null)" = 1 ] &&
	echo "kwidth"

# 4. otherwise: stock CFS bandwidth control
echo "cfs"
```

Why the order matters: on a `sched_pfs=1` boot sched_ext cannot load at all, and
the width sysctl governs the fair class, which is not what runs your tasks. A
script that checked the sysctl first would report `kwidth` on a kPFS machine and
be wrong. **This is the ordinary way to get this wrong**, so prefer the script
over writing the check again.

## Each variant

### `cfs` - stock CFS bandwidth control

The kernel default, in every distribution kernel since cgroup v2. The group may
run on every CPU; when it has spent its quota for the current period, **every**
one of its tasks is taken off CPU until the period rolls over.

| | |
|---|---|
| Needs | `CONFIG_CFS_BANDWIDTH` (without it `cpu.max` does not exist) |
| Turn on | nothing - it is the default |
| Verify | the three checks above all answer no |
| Watch | `cpu.stat` in the cgroup |

```sh
$ cat /sys/fs/cgroup/my.slice/cpu.stat
usage_usec 41203551      # cumulative: difference it over a window, never read once
nr_periods 4120
nr_throttled 2831        # periods in which the group was frozen
throttled_usec 9044120   # total time frozen
```

`nr_throttled` climbing is the thing people hit: the group is inside its average
limit and still stalls, in bursts, because the quota ran out early in the
period. That is the problem the other three exist to avoid.

### `kwidth` - the width policy inside CFS (v1)

The same concurrency-cap idea, implemented in CFS bandwidth control and switched
on by a sysctl. Instead of a time budget the group gets a **width**: at most
`floor(quota/period)` of its tasks on CPUs at once, with the fractional
remainder as one extra slot carrying a budget.

| | |
|---|---|
| Needs | a kernel carrying the width-mode patch |
| Turn on | `sysctl -w kernel.sched_cfs_bandwidth_width=1` |
| Verify | **read the sysctl back** - see below |
| Watch | `cpu.stat`, as for `cfs` |

```sh
$ sysctl -w kernel.sched_cfs_bandwidth_width=1
$ cat /proc/sys/kernel/sched_cfs_bandwidth_width
1
```

Read it back. If the file does not exist your kernel does not have the patch and
you are running stock `cfs`; if the write did not take you are also running
stock `cfs`. Either way nothing announces it, and a measurement labelled
`kwidth` that is really `cfs` is worse than no measurement. The harness aborts
on both cases rather than reporting a number
(`mech/mech-guest.sh`, arm `kwidth`).

It is a global switch, not per cgroup: turning it on changes every group that
has a `cpu.max`.

### `scx_pfs` - a sched_ext BPF scheduler

The policy as a loadable BPF scheduler. Nothing is rebuilt and nothing reboots;
you run a program, and while it runs it schedules the machine.

| | |
|---|---|
| Needs | kernel 6.12+ with `CONFIG_SCHED_CLASS_EXT=y`; following later `cpu.max` changes needs 6.17+ |
| Turn on | run the binary: `scx_pfs` |
| Verify | `/sys/kernel/sched_ext/state` reads `enabled` |
| Watch | the loader's own counters |

```sh
$ scx_pfs -i 1 -S /run/scx_pfs.cgroups &      # -i: counters every second
$ cat /sys/kernel/sched_ext/state
enabled
$ cat /sys/kernel/sched_ext/root/ops
pfs
```

It stops when the process stops - including if it crashes, after which the
machine is back on CFS. That is the one property no other variant has, and it is
why this is the variant to try first on a machine you care about.

**Its throttling is not in `cpu.stat`.** Under a BPF scheduler the fair class is
not running your tasks, so `nr_throttled` and `throttled_usec` stay zero -
measured, zero on every cell of our data set. Read the loader instead:

```
admit        a parked task let back on
park_full    parked: every slot of the cgroup was taken
park_budget  parked: the fractional slot's budget was spent
park_debt    slot refused: the cgroup had run beyond its limit
credit       ran without a slot instead of waiting
rotate       slots handed from one task to another
```

`-S FILE` additionally dumps the same counters **per cgroup**, which is the only
way to attribute a park to a particular container. The full list is in
`tools/sched_ext/scx_pfs.h` (`enum pfs_stat_idx`).

### `kPFS` - a scheduling class

The same policy as a scheduling class beside CFS, `CONFIG_SCHED_PFS`, selected
on the kernel command line. On such a boot it replaces CFS for normal tasks
entirely.

| | |
|---|---|
| Needs | `CONFIG_SCHED_PFS=y` (needs `CONFIG_SMP`, `CONFIG_CFS_BANDWIDTH`, `CONFIG_FAIR_GROUP_SCHED`, and `!CONFIG_SCHED_PROXY_EXEC`) |
| Turn on | `sched_pfs=1` on the kernel command line, then reboot |
| Verify | `grep -w sched_pfs=1 /proc/cmdline` **and** that the debugfs directory exists |
| Watch | `/sys/kernel/debug/sched/pfs/` |

```sh
$ grep -o 'sched_pfs=1' /proc/cmdline
sched_pfs=1
$ ls /sys/kernel/debug/sched/pfs/
burst_pct  features  groups  stats  waiters  wakeup_gran_us  window_us
$ cat /sys/kernel/debug/sched/pfs/stats
admit 182394
park_full 90118
park_budget 1509
park_debt 737387
refuse_budget_frac_2 1882151
refuse_debt_guar_1 737387
...
```

Three things it has that the others do not:

* `groups` - one line per limited cgroup: its width, slots taken, tokens;
* `waiters` - what is parked right now, and behind which group;
* `refuse_<reason>_<slot>` - **which slot index** a refusal fell on, so a
  refusal can be attributed to a limit rather than counted globally.

`window_us` and `burst_pct` are writable, so the window the fractional slot is
budgeted over can be changed without a rebuild.

There is **no per-pod or per-container opt-in, and no mixing**: a `sched_pfs=1`
boot runs the whole machine this way, and sched_ext cannot load while it is on.
If you need some workloads on CFS and some on the slot policy on one machine,
`scx_pfs` scoped to a cgroup subtree is the only variant that can do it.

## Setting the same limit from where you actually work

All four variants read `cpu.max`, so these are interchangeable ways of writing
the same test. The repo has one script per entry point; each takes the same
three arguments.

### Plain cgroup v2

```sh
# cg-run.sh VARIANT LIMIT THREADS
./cg-run.sh cfs 2 8
```

By hand:

```sh
mkdir -p /sys/fs/cgroup/test
echo '200000 100000' > /sys/fs/cgroup/test/cpu.max
echo $$ > /sys/fs/cgroup/test/cgroup.procs
```

### systemd

`CPUQuota=` is `cpu.max`; `CPUWeight=` is `cpu.weight`.

```sh
./systemd-run.sh cfs 2 8
# by hand:
systemd-run --unit=test -p CPUQuota=200% -p CPUWeight=100 ./workload
```

### Docker and Podman

```sh
./docker-run.sh cfs 2 8          # DOCKER=podman for Podman
# by hand:
docker run --cpus=2 --rm my-image
docker run --cpu-period=100000 --cpu-quota=200000 --rm my-image
```

To load `scx_pfs` itself from a container - the Docker equivalent of a
DaemonSet - `scx-container.sh start` runs it with only `CAP_SYS_ADMIN`,
`CAP_BPF` and `CAP_PERFMON`. Note what that means: the scheduler then applies to
**every task on the host**, not only to containers.

### Kubernetes

Nothing in the kit is needed to set the limit; the pod spec is enough.

```yaml
resources:
  limits:
    cpu: "2"        # -> cpu.max "200000 100000" in the container's cgroup
```

The variant is a property of the **node**, not of the pod, so comparing
variants under Kubernetes means either two node pools or draining and rebooting
a node between arms. For `scx_pfs` a DaemonSet on a labelled node pool is
enough, because loading it needs no reboot.

Finding the cgroup a running container ended up in:

```sh
# the path is runtime- and driver-specific, so read it rather than construct it
$ cat /proc/<pid>/cgroup
0::/kubepods.slice/kubepods-burstable.slice/.../cri-containerd-<id>.scope
$ cat /sys/fs/cgroup/kubepods.slice/.../cpu.max
200000 100000
```

## What each one tells you when it limits you

| | `cfs` | `kwidth` | `scx_pfs` | `kPFS` |
|---|---|---|---|---|
| selected by | default | sysctl | running a binary | boot parameter |
| reboot to change | no | no | no | **yes** |
| per-cgroup scope possible | n/a | no, global | **yes** | no, whole machine |
| survives its own crash | n/a | n/a | yes, falls back to CFS | no |
| `cpu.stat` throttling fields | yes | yes | **always zero** | no |
| per-cgroup counters | `cpu.stat` | `cpu.stat` | loader `-S FILE` | debugfs `groups` |
| which slot was refused | - | - | per-cgroup totals | **yes**, per slot index |
| tunable at run time | `cpu.max` | the sysctl | loader flags | debugfs `window_us`, `burst_pct` |

The row that catches people is `cpu.stat` under `scx_pfs`: every dashboard that
alerts on `nr_throttled` goes quiet, and silence there means the field is not
being written, not that nothing is being throttled.
