SCHED_EXT EXAMPLE SCHEDULERS
============================

# Introduction

This directory contains a number of example sched_ext schedulers. These
schedulers are meant to provide examples of different types of schedulers
that can be built using sched_ext, and illustrate how various features of
sched_ext can be used.

Some of the examples are performant, production-ready schedulers. That is, for
the correct workload and with the correct tuning, they may be deployed in a
production environment with acceptable or possibly even improved performance.
Others are just examples that in practice, would not provide acceptable
performance (though they could be improved to get there).

This README will describe these example schedulers, including describing the
types of workloads or scenarios they're designed to accommodate, and whether or
not they're production ready. For more details on any of these schedulers,
please see the header comment in their .bpf.c file.


# Compiling the examples

There are a few toolchain dependencies for compiling the example schedulers.

## Toolchain dependencies

1. clang >= 16.0.0

The schedulers are BPF programs, and therefore must be compiled with clang. gcc
is actively working on adding a BPF backend compiler as well, but are still
missing some features such as BTF type tags which are necessary for using
kptrs.

2. pahole >= 1.25

You may need pahole in order to generate BTF from DWARF.

3. rust >= 1.70.0

Rust schedulers uses features present in the rust toolchain >= 1.70.0. You
should be able to use the stable build from rustup, but if that doesn't
work, try using the rustup nightly build.

There are other requirements as well, such as make, but these are the main /
non-trivial ones.

## Compiling the kernel

In order to run a sched_ext scheduler, you'll have to run a kernel compiled
with the patches in this repository, and with a minimum set of necessary
Kconfig options:

```
CONFIG_BPF=y
CONFIG_SCHED_CLASS_EXT=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_DEBUG_INFO_BTF=y
CONFIG_BPF_JIT_ALWAYS_ON=y
CONFIG_BPF_JIT_DEFAULT_ON=y
```

There is a `Kconfig` file in this directory whose contents you can append to
your local `.config` file, as long as there are no conflicts with any existing
options in the file.

## Getting a vmlinux.h file

You may notice that most of the example schedulers include a "vmlinux.h" file.
This is a large, auto-generated header file that contains all of the types
defined in some vmlinux binary that was compiled with
[BTF](https://docs.kernel.org/bpf/btf.html) (i.e. with the BTF-related Kconfig
options specified above).

The header file is created using `bpftool`, by passing it a vmlinux binary
compiled with BTF as follows:

```bash
$ bpftool btf dump file /path/to/vmlinux format c > vmlinux.h
```

`bpftool` analyzes all of the BTF encodings in the binary, and produces a
header file that can be included by BPF programs to access those types.  For
example, using vmlinux.h allows a scheduler to access fields defined directly
in vmlinux as follows:

```c
#include "vmlinux.h"
// vmlinux.h is also implicitly included by scx_common.bpf.h.
#include "scx_common.bpf.h"

/*
 * vmlinux.h provides definitions for struct task_struct and
 * struct scx_enable_args.
 */
void BPF_STRUCT_OPS(example_enable, struct task_struct *p,
		    struct scx_enable_args *args)
{
	bpf_printk("Task %s enabled in example scheduler", p->comm);
}

// vmlinux.h provides the definition for struct sched_ext_ops.
SEC(".struct_ops.link")
struct sched_ext_ops example_ops {
	.enable	= (void *)example_enable,
	.name	= "example",
}
```

The scheduler build system will generate this vmlinux.h file as part of the
scheduler build pipeline. It looks for a vmlinux file in the following
dependency order:

1. If the O= environment variable is defined, at `$O/vmlinux`
2. If the KBUILD_OUTPUT= environment variable is defined, at
   `$KBUILD_OUTPUT/vmlinux`
3. At `../../vmlinux` (i.e. at the root of the kernel tree where you're
   compiling the schedulers)
3. `/sys/kernel/btf/vmlinux`
4. `/boot/vmlinux-$(uname -r)`

In other words, if you have compiled a kernel in your local repo, its vmlinux
file will be used to generate vmlinux.h. Otherwise, it will be the vmlinux of
the kernel you're currently running on. This means that if you're running on a
kernel with sched_ext support, you may not need to compile a local kernel at
all.

### Aside on CO-RE

One of the cooler features of BPF is that it supports
[CO-RE](https://nakryiko.com/posts/bpf-core-reference-guide/) (Compile Once Run
Everywhere). This feature allows you to reference fields inside of structs with
types defined internal to the kernel, and not have to recompile if you load the
BPF program on a different kernel with the field at a different offset. In our
example above, we print out a task name with `p->comm`. CO-RE would perform
relocations for that access when the program is loaded to ensure that it's
referencing the correct offset for the currently running kernel.

## Compiling the schedulers

Once you have your toolchain setup, and a vmlinux that can be used to generate
a full vmlinux.h file, you can compile the schedulers using `make`:

```bash
$ make -j($nproc)
```

# Example schedulers

This directory contains the following example schedulers. These schedulers are
for testing and demonstrating different aspects of sched_ext. While some may be
useful in limited scenarios, they are not intended to be practical.

For more scheduler implementations, tools and documentation, visit
https://github.com/sched-ext/scx.

## scx_simple

A simple scheduler that provides an example of a minimal sched_ext scheduler.
scx_simple can be run in either global weighted vtime mode, or FIFO mode.

Though very simple, in limited scenarios, this scheduler can perform reasonably
well on single-socket systems with a unified L3 cache.

## scx_qmap

Another simple, yet slightly more complex scheduler that provides an example of
a basic weighted FIFO queuing policy. It also provides examples of some common
useful BPF features, such as arena-backed doubly-linked lists threaded through
per-task context and `bpf_res_spin_lock` for per-queue synchronization. It also
illustrates how core-sched support could be implemented.

## scx_central

A "central" scheduler where scheduling decisions are made from a single CPU.
This scheduler illustrates how scheduling decisions can be dispatched from a
single CPU, allowing other cores to run with infinite slices, without timer
ticks, and without having to incur the overhead of making scheduling decisions.

The approach demonstrated by this scheduler may be useful for any workload that
benefits from minimizing scheduling overhead and timer ticks. An example of
where this could be particularly useful is running VMs, where running with
infinite slices and no timer ticks allows the VM to avoid unnecessary expensive
vmexits.

## scx_flatcg

A flattened cgroup hierarchy scheduler. This scheduler implements hierarchical
weight-based cgroup CPU control by flattening the cgroup hierarchy into a single
layer, by compounding the active weight share at each level. The effect of this
is a much more performant CPU controller, which does not need to descend down
cgroup trees in order to properly compute a cgroup's share.

Similar to scx_simple, in limited scenarios, this scheduler can perform
reasonably well on single socket-socket systems with a unified L3 cache and show
significantly lowered hierarchical scheduling overhead.

## scx_pfs

The Parallel Fair Scheduler: cgroup v2 CPU limits (`cpu.max`) as a cap on the
number of threads of a cgroup that run at once, instead of a CPU time budget
per period. A cgroup limited to N CPUs has floor(N) slots without any time
accounting; threads beyond them wait in a queue of the cgroup and take a slot
as soon as one is given back, on the CPU that gave it back. A fractional
limit adds one slot whose CPU time is limited by a token bucket: it accrues
at the limit, up to what the fraction allows in one `cpu.max` period (10 ms for
a limit of 0.1 with the usual 100 ms period), so a burst that CFS runs at once
from the quota of a period runs at once here too.
Limits apply at every level of the hierarchy, and `cpu.weight` splits the
machine between cgroups under contention as in scx_flatcg. The header comment
of `scx_pfs.bpf.c` describes the design.

The motivation is container CPU limits: with CFS bandwidth control a pod
limited to 4 CPUs with 32 runnable threads runs on 32 CPUs for 1/8 of the
100 ms period and then stalls for the rest of it, which shows up as tail
latency although the average usage is below the limit.

### Options

```
scx_pfs [-s SLICE_US] [-c CUT_US] [-r ROT_US] [-k CREDIT_US] [-w WINDOW_US] [-b BURST_PCT] [-q NR] [-i SECS] [-S FILE] [-v]
```

| Option | Default | Meaning |
|--------|---------|---------|
| `-s` | 2500 | Slice. A thread whose slice ends while others of its cgroup wait gives its slots back and queues again. |
| `-c` | 500 | A thread holding slots since a rotation gives them to a thread that woke up after this long; also the least time a thread keeps its slots on `sched_yield()`. |
| `-r` | 0 (off) | Like `-c`, for any waiting thread. 1000 makes spin hand-offs about 1.5x faster but splits a contended machine less precisely by `cpu.weight`. |
| `-k` | 1000 | A thread that woke up and found no free slot runs without one for up to this long, if its cgroup has saved up CPU time (see below); 0 makes it wait. |
| `-w` | 10000 | How often a cgroup out of CPU time for its fractional slot gets another try, the debt margin, and the window of the throttling counters (minimum 1000). |
| `-b` | 100 | Depth of the token bucket of a cgroup, in percent of what the fractional part of its limit allows per `cpu.max` period, but at least one window's worth of the whole limit. 0 gives about the 10 ms windows of earlier versions; at most 1000. |
| `-q` | 65536 | Size of the queues of cgroups with waiting threads (16 to 16777216). Cgroups beyond it are found by a slower sweep of all cgroups once per window; `overflow` counts them. |
| `-i` | 1 | Interval of the counter report, in seconds, which includes the throttling counters of each cgroup. |
| `-S` | off | Also write the throttling counters to FILE every second (replaced atomically). |

`-s` takes 100 to 1000000 us, `-c`, `-r` and `-k` at most 1000000 us, `-w` 1000
to 1000000 us, `-b` 0 to 1000 and `-i` 1 to 86400 s; scx_pfs refuses other
values.

### Counters

Printed every `-i` seconds: `park_full` (a thread waited for a slot),
`park_budget` (the fractional slot was out of tokens), `park_woken` (a woken
thread waited), `admit` (waiting threads let in), `retry`, `cut` (slots
taken back for a woken thread), `yield_keep`/`yield_rotate` (what
`sched_yield()` did), `too_deep` (threads of cgroups deeper than 31 levels,
not limited), `rescue` (waiting cgroups the timer retried because no retry
walked them for 50 ms; it grows when admitted threads wait long for a CPU),
`lost_path` (threads whose cgroup was removed under them), `credit` (threads
that ran without a slot instead of waiting, see below), `overflow` (cgroups
that didn't fit in the queues, see `-q`), `drain` (threads still waiting
when their cgroup was removed, moved/kept; should stay 0/0), `no_dsq`
(cgroups not limited because their queue couldn't be allocated),
`park_race` (retries repeated for a thread about to start waiting),
`park_debt` (slots refused because the cgroup ran beyond its limit), and
internal ones (`direct`, `shared`, `repark`, `race`, `activate`, `hweight`,
`keep`). CFS's `nr_throttled` stays 0: waiting threads are queued by
scx_pfs, not throttled.

### Throttling counters

scx_pfs can't write `cpu.stat`, so it keeps `cpu.stat`-like counters per
cgroup in BPF. Each report (every `-i` seconds and at exit) ends with one line
per cgroup that was limited and ran or waited, in key=value form:

```
cgroup path=/kubepods/burstable/pod1234 nr_periods=1005 nr_throttled=1000 throttled_usec=10000420
```

- `path`: the cgroup's path below `/sys/fs/cgroup`.
- `nr_periods`: windows (`-w`, 10 ms) in which the cgroup ran or had a
  waiting thread.
- `nr_throttled`: windows in which at least one runnable thread of the cgroup
  waited, for a slot or for the tokens of the fractional slot.
- `throttled_usec`: wall time during which at least one thread of the cgroup
  waited, from the first waiter's start to the last waiter's end, including
  a wait still in progress. Unlike CFS's, which adds up the throttled time of
  each CPU, it can't exceed the elapsed time.

A thread waits in the cgroup whose limit refused it; that cgroup and every
limited ancestor whose slots are all taken at that moment count it, as
`cpu.stat` counts a throttled parent. Counters start at zero when the cgroup
is created or scx_pfs is loaded. `-S FILE` writes the same lines without the
`cgroup` prefix and with `key value` pairs, as in `cpu.stat`:
`/kubepods/burstable/pod1234 nr_periods 1005 nr_throttled 1000 throttled_usec 10000420`.

### Running it

Start it as root and stop it with Ctrl-C (the kernel scheduler takes over
again), or install `pfs-test/scx_pfs.service` as a sandboxed systemd unit,
or run it in a container with `CAP_SYS_ADMIN`, `CAP_BPF` and `CAP_PERFMON`
only (`pfs-test/scx-container.sh`), which is the shape of a Kubernetes
DaemonSet. It needs `CONFIG_SCHED_CLASS_EXT=y`;
following run-time changes of `cpu.max` needs the `cgroup_set_bandwidth`
operation (6.17 or later). v6.19 is the oldest tag it has been loaded and run
on: v6.12 and v6.13 are missing `cgroup_set_bandwidth` and several of the
kfuncs it calls. `pfs-test/` compares it with CFS bandwidth control
on plain cgroups, systemd units, Docker and Podman; its README has the
reproduction steps and the full result tables.

### Measurements

On an 8-vCPU local VM (median of 5 boots, same kernel image for both):

- Limit 1 with 8 busy threads: 0.83 vs 0.61 million iterations/s, worst stall
  of a thread 23 vs 93 ms, CPUs used 0.99 in both, `nr_throttled` 0.
- Four request threads with 1 ms of work every 20 ms next to 8 busy threads
  of the same cgroup, limit 1: p50/p99 2.2/3.1 ms vs 349/1036 ms.
- Request latency at limit 0.5 through the kit (cgroup, Docker and systemd
  paths): p99 30-34 ms vs 82-84 ms.

On a 16-vCPU VM running Kubernetes pods (kernel 7.0, 3 runs or more):

- PostgreSQL pgbench TPC-B, 32 clients, limit 4: p99 10.0 ms vs 46.9 ms, at
  6050 vs 9003 transactions/s; select-only p99 1.46 vs 12.4 ms.
- 16 threads calling `sched_yield()` 100 times while holding one of 4
  mutexes, limit 4: 37.6k vs 11.1k events/s; sysbench cpu p99 9.2 vs 44 ms.
- hackbench in a pod limited to 4 CPUs: 2.22 s vs 3.26 s (processes).

### Disadvantages

- Databases lose 27-37% of their throughput against CFS at the same limit
  (PostgreSQL, MariaDB at limits 4 and 2), with much better tails: the pod
  uses 2.8-3.0 of 4 CPUs. Against a 4-vCPU VM without limit, limit 4 gives
  89-91% of its transactions/s.
- More spinning threads than slots collapse: Redis with 4 I/O threads at
  limit 2 serves 5.1k requests/s vs 86k under CFS; two threads passing a flag
  in a busy loop, 8 pairs at limit 2, reach 0.08 of CFS's rate. Keep
  busy-polling threads at or below the limit.
- A web server at limit 4 with 8 threads next to heavy neighbours (two
  unlimited 16-thread spinners and 20 limited pods) reached only 3197 vs
  5337 requests/s using 2.23 of 4 CPUs, and missed its fixed rate (p99 14.9 s).
- With 50-100 pods on the node, a 1 ms sleeper outside the pods overshoots by
  4.7-8.0 ms at p99 vs 1.0-2.4 ms under CFS, and migrations are about 100
  times as frequent.
- On machines with an unstable `sched_clock` (e.g. KVM guests whose
  kvm-clock isn't marked stable, `sched_clock_stable()` 0 in
  `/sys/kernel/debug/sched/debug`), kernels without "softirq: Treat an
  interrupted idle task as idle in tick_irq_exit()" charge a waiting thread
  the idle time of the CPU it is woken on: `cpu.stat`, `getrusage()` and the
  thread CPU clocks over-report, by up to 20 times at limit 0.1. The limit
  itself holds; a `sched_switch` trace counts the CPUs correctly.
- Waiting for a slot costs latency even in an otherwise idle cgroup: four
  request threads at limit 1 have a median of 3.1 ms vs 1.15 ms under CFS.
- `cpu.max.burst` is ignored, and so are cgroups deeper than 31 levels.
- A thread waiting for a slot while it holds a kernel lock stalls every
  thread waiting for that lock, in any cgroup. Exiting threads, kernel threads
  and threads being killed never wait. A thread that woke up (or an io_uring
  worker) and finds no free slot runs without one for up to `-k` (1 ms) if its
  cgroup has tokens, i.e. time it saved up while it ran below its limit. Its
  concurrency can then exceed the limit, its usage can't. A cgroup that keeps
  all its slots busy saves nothing and gives no credit once its tokens are
  spent: its woken threads wait for a slot, ahead of the rotated ones, and take
  one from a rotated holder after `-c`. That is the trade-off: credit beyond
  the tokens, e.g. a budget every window, would bound a woken lock owner's wait
  further, but threads taking turns on a contended
  mutex then burn the limit on wakeups that block again (lock+yield at limit 4
  fell to 0.18 of its throughput). A thread preempted, rotated or yielding
  (also through the in-kernel `yield()`) in the kernel with a sleeping lock
  held can still wait: sched_ext doesn't tell whether a thread is returning to
  user space.
- All runtime is taken off the bucket, with or without a slot, and usage
  beyond the limit (credit, threads that never wait, slices ending on a tick)
  leaves it negative, a debt of up to one second's worth of the limit. While
  the debt is more than a window's budget, the last full slot and the
  fractional slot aren't handed out until it is paid back (`park_debt`).
  Usage averages at the limit, but pipe and database loads can run slightly
  above it for a window and below it in the next.
- Fractional limits are still enforced as time. A burst of up to the bucket's
  depth runs at once, but a thread that keeps the fractional slot busy runs
  about limit x 10 ms every 10 ms window. A cgroup whose bucket is full can
  use up to twice the fraction over one period, the bucket plus what accrues
  meanwhile, as a CFS group can over 100 ms that span two periods.


# Troubleshooting

There are a number of common issues that you may run into when building the
schedulers. We'll go over some of the common ones here.

## Build Failures

### Old version of clang

```
error: static assertion failed due to requirement 'SCX_DSQ_FLAG_BUILTIN': bpftool generated vmlinux.h is missing high bits for 64bit enums, upgrade clang and pahole
        _Static_assert(SCX_DSQ_FLAG_BUILTIN,
                       ^~~~~~~~~~~~~~~~~~~~
1 error generated.
```

This means you built the kernel or the schedulers with an older version of
clang than what's supported (i.e. older than 16.0.0). To remediate this:

1. `which clang` to make sure you're using a sufficiently new version of clang.

2. `make fullclean` in the root path of the repository, and rebuild the kernel
   and schedulers.

3. Rebuild the kernel, and then your example schedulers.

The schedulers are also cleaned if you invoke `make mrproper` in the root
directory of the tree.

### Stale kernel build / incomplete vmlinux.h file

As described above, you'll need a `vmlinux.h` file that was generated from a
vmlinux built with BTF, and with sched_ext support enabled. If you don't,
you'll see errors such as the following which indicate that a type being
referenced in a scheduler is unknown:

```
/path/to/sched_ext/tools/sched_ext/user_exit_info.h:25:23: note: forward declaration of 'struct scx_exit_info'

const struct scx_exit_info *ei)

^
```

In order to resolve this, please follow the steps above in
[Getting a vmlinux.h file](#getting-a-vmlinuxh-file) in order to ensure your
schedulers are using a vmlinux.h file that includes the requisite types.

## Misc

### llvm: [OFF]

You may see the following output when building the schedulers:

```
Auto-detecting system features:
...                         clang-bpf-co-re: [ on  ]
...                                    llvm: [ OFF ]
...                                  libcap: [ on  ]
...                                  libbfd: [ on  ]
```

Seeing `llvm: [ OFF ]` here is not an issue. You can safely ignore.
