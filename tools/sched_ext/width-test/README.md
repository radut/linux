# Testing CPU-limit enforcement without Kubernetes

This kit compares three ways of enforcing a cgroup v2 CPU limit (`cpu.max`) on
one machine, using either plain cgroups or Docker/Podman:

| Variant  | What enforces `cpu.max = Q P`                                                     |
|----------|-----------------------------------------------------------------------------------|
| `cfs`    | Stock CFS bandwidth control: Q µs of CPU time per P µs, shared by all threads.     |
| `scx`    | `scx_width` (sched_ext): at most floor(Q/P) threads on CPUs at once, no time budget; a fractional remainder is one extra slot with a budget per 10 ms window. `cpu.weight` sets the share under contention. |
| `kwidth` | The same policy inside CFS, enabled with `sysctl kernel.sched_cfs_bandwidth_width=1` on a kernel carrying that patch. |

Kubernetes, Docker and Podman all express a CPU limit as `cpu.max`, so nothing
else changes: a container started with `--cpus=1.5` gets `150000 100000`.

## Requirements

- cgroup v2 (`stat -fc %T /sys/fs/cgroup` prints `cgroup2fs`), root.
- `scx`: kernel 6.12 or later with `CONFIG_SCHED_CLASS_EXT=y`,
  `CONFIG_BPF_SYSCALL=y`, `CONFIG_DEBUG_INFO_BTF=y`. Following later changes
  of `cpu.max` at run time needs the `cgroup_set_bandwidth` operation (6.17+).
- `kwidth`: a kernel built with the width-mode patch; the sysctl
  `/proc/sys/kernel/sched_cfs_bandwidth_width` exists.
- gcc with static glibc (`libc6-dev`), python3 (concurrency column), tracefs
  (optional, `WT_TRACE=0` skips the trace).
- Docker path: Docker or Podman, any cgroup driver.

## Build

    make -C tools/sched_ext                     # builds build/bin/scx_width
    make -C tools/sched_ext/width-test          # static spin, noisy, reqlat

For a container image, link `scx_width` statically (libbpf, libelf, zlib and
libzstd static archives installed):

    cd tools/sched_ext
    gcc -static -o build/bin/scx_width.static build/obj/sched_ext/scx_width.o \
        build/obj/libbpf/libbpf.a -lelf -lz -lzstd -lpthread

The scripts look for scx_width in `$SCX`, `width-test/bin/`, `../build/bin/`
and `$PATH`.

## Tools

| File               | Purpose |
|--------------------|---------|
| `spin THREADS SECS`  | Busy threads; prints total iterations and the worst gap between two loop iterations of one thread (a stall: throttled, or waiting for a slot). |
| `reqlat THREADS WORK_US PERIOD_US SECS [BURN]` | Open-loop requests: each thread does WORK_US of CPU work every PERIOD_US; latency from scheduled start to completion (p50/p90/p99/p99.9/max). BURN busy threads in the same process stand for GC or background work sharing the limit. |
| `conc.py COMM TRACE CAP` | From a `sched_switch` trace: how many CPUs ran COMM at once (average, maximum, share of time above the limit). |
| `cg-run.sh`        | One case on plain cgroup v2. |
| `docker-run.sh`    | One case in a container. |
| `compare.sh`       | The whole matrix, CSV and a summary table. |
| `docker-image.sh`  | Builds a local FROM-scratch image with the static binaries (`docker import`, no registry). |
| `scx-container.sh` | Runs scx_width as a privileged container (`--privileged --pid=host`), the Docker shape of a DaemonSet. |

Every script prints its usage with `-h`, runs under `set -euo pipefail`, and on
exit or Ctrl-C removes its cgroups and containers, stops scx_width and restores
the sysctl.

## Path A: plain cgroup v2

What the scripts do by hand:

    echo +cpu > /sys/fs/cgroup/cgroup.subtree_control
    mkdir /sys/fs/cgroup/width-test
    echo +cpu > /sys/fs/cgroup/width-test/cgroup.subtree_control
    mkdir /sys/fs/cgroup/width-test/lim
    echo "150000 100000" > /sys/fs/cgroup/width-test/lim/cpu.max    # 1.5 CPUs
    echo 100 > /sys/fs/cgroup/width-test/lim/cpu.weight
    sh -c 'echo $$ > /sys/fs/cgroup/width-test/lim/cgroup.procs; exec ./bin/spin 8 10'
    cat /sys/fs/cgroup/width-test/lim/cpu.stat

One case per variant:

    sudo ./cg-run.sh -H cfs 1 8              # stock CFS, 1 CPU, 8 threads
    sudo ./cg-run.sh scx 1 8                 # loads scx_width for the run
    sudo ./cg-run.sh kwidth 1 8              # sysctl on for the run
    sudo ./cg-run.sh -n noisy scx 2 64       # with an unlimited busy neighbour
    sudo ./cg-run.sh -w reqlat -n noisy cfs 0.5 2   # request latency, 2 burners

To try scx_width on your own workload instead, start it in one terminal
(`sudo ./bin/scx_width -i 5`, Ctrl-C to stop) and set `cpu.max` as usual.
kwidth applies at the moment `cpu.max` is written: flip the sysctl, then
rewrite existing limits (`cat cpu.max > cpu.max`).

## Path B: Docker (or Podman)

    sudo ./docker-image.sh                   # image width-test:local
    sudo docker run --rm --cpus=1.5 width-test:local /spin 8 10
    sudo docker run --rm --cpu-period=100000 --cpu-quota=50000 --cpu-shares=512 \
         width-test:local /reqlat 4 1000 20000 10 2

Find the container's cgroup and read its counters while it runs:

    pid=$(docker inspect -f '{{.State.Pid}}' CONTAINER)
    cg=/sys/fs/cgroup$(sed -n 's/^0:://p' /proc/$pid/cgroup)
    cat $cg/cpu.max $cg/cpu.weight $cg/cpu.stat

With the systemd cgroup driver the path is
`/sys/fs/cgroup/system.slice/docker-<id>.scope`; with cgroupfs it is
`/sys/fs/cgroup/docker/<id>`. `--cpus` and `--cpu-period/--cpu-quota` write
`cpu.max`; `--cpu-shares` becomes `cpu.weight` with the runtime's own
conversion (512 shares gave weight 59 with Docker 29). The cgroup disappears
with the container, so read it while the container runs; `docker-run.sh`
polls `cpu.stat` every 0.2 s and reports the last reading.

Start the scheduler either on the host (`sudo ./bin/scx_width`) or as a
container, which mirrors the Kubernetes DaemonSet:

    sudo ./scx-container.sh start            # needs a static scx_width in the image
    sudo ./scx-container.sh status
    sudo ./scx-container.sh stop

    # by hand:
    docker run -d --name scx-width --privileged --pid=host --network none \
        width-test:local /scx_width -i 10

Cases and the matrix:

    sudo ./docker-run.sh -H cfs 1.5 8
    sudo ./docker-run.sh -P 100000 -S 512 kwidth 0.5 8
    sudo SCX_MODE=container ./docker-run.sh scx 1 64
    sudo ./compare.sh -p docker

Podman: `DOCKER=podman` for every script; the flags are the same. Rootless
Podman needs the cpu controller delegated to the user (systemd
`Delegate=cpu`), and loading scx_width always needs root.

## The matrix

    sudo ./compare.sh                        # cgroup path, all available variants
    sudo ./compare.sh -v cfs,scx -l 0.5,1 -t 8 -s 10

Limits 0.5, 1, 2 and 4 CPUs, 1, 8 and 64 threads, no neighbour or a noisy
one, then the latency test at 0.5 and 1 CPU. Results go to
`results/<timestamp>-<path>/results.csv` with these columns:

| Column | Meaning |
|--------|---------|
| `miter_s` | Throughput, millions of `spin` loop iterations per second (for `reqlat`: the burners). |
| `nr_throttled`, `throttled_ms` | From `cpu.stat`. `throttled_usec` adds up throttled time over CPUs, so it can exceed the run time. |
| `worst_stall_ms`, `stalls_5ms` | Longest pause of one thread, and the number of pauses over 5 ms. |
| `avg_conc`, `max_conc`, `pct_over_cap` | CPUs running the workload at once (trace; average over the time it ran), and the share of time above the limit rounded up. |
| `p50_ms`, `p99_ms`, `max_ms` | `reqlat` request latency. |

## Reading the results

- `cfs`: `avg_conc` well above the limit and `pct_over_cap` high (bursts on
  every CPU), `nr_throttled` close to one per period, worst stalls near
  `period - quota` (90 ms at 1 CPU with 8 threads).
- `scx`: `avg_conc` at the limit, `pct_over_cap` near 0, `nr_throttled` 0:
  scx_width parks waiting threads in its own queues and never uses CFS
  throttling. Stalls are bounded by the slice (2.5 ms) times the number of
  waiting threads per slot. Throughput is usually higher at the same limit
  because no quota is lost at period boundaries.
- `kwidth`: `avg_conc` at the limit as well, but `nr_throttled` and
  `throttled_usec` grow: a run queue waiting for a slot is parked with CFS's
  own throttling, so here they count slot waits, not exhausted quota.
- scx_width counters: `sudo ./bin/scx_width -i 5` prints them every 5 s
  (`park_full` = a thread waited for a whole-CPU slot, `park_budget` = the
  fractional slot ran out of budget, `admit` = parked threads let in).
- `/sys/kernel/sched_ext/state` is `enabled` while scx_width runs;
  `/sys/kernel/sched_ext/root/ops` names it.

## Troubleshooting

- `/sys/kernel/sched_ext` missing: the kernel lacks `CONFIG_SCHED_CLASS_EXT`
  (or is older than 6.12). Use a distribution kernel that enables it, or build
  one.
- `sched_ext_ops.dump() missing, kernel too old?`: the same, from the loader.
- cgroup v1 or hybrid hosts: `cpu.max` does not exist there (v1 has
  `cpu.cfs_quota_us`), and the kit and both schedulers were only tested on
  v2. Boot with `systemd.unified_cgroup_hierarchy=1`; Docker then needs no
  other change.
- `echo +cpu > cgroup.subtree_control` fails with "Device or resource busy" or
  "Invalid argument": a cgroup with processes in it cannot enable controllers
  for children (no internal processes rule), or a real-time task holds the cpu
  controller back.
- Docker cgroupfs vs systemd driver: only the cgroup path differs
  (`docker info | grep -i cgroup`); always find it through the container's PID
  as above.
- scx_width exits with a stall error: the kernel watchdog ejects a sched_ext
  scheduler that leaves a runnable task waiting too long
  (`runnable task stall` in `dmesg`), and all tasks fall back to the normal
  scheduler at once. The workload keeps running, now under CFS. Report the
  log and the case.
- Another scheduler already loaded (`state` is `enabled` and `ops` names a
  different one): only one sched_ext scheduler can run at a time.
- kwidth seems inactive: the sysctl applies when `cpu.max` is written;
  rewrite existing limits after enabling it.
- `cpu.max.burst` is honoured by `cfs` only.
