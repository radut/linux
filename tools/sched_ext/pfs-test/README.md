# Testing CPU-limit enforcement without Kubernetes

This kit compares three ways of enforcing a cgroup v2 CPU limit (`cpu.max`) on
one machine, using plain cgroups, systemd units (`systemd-run`) or
Docker/Podman:

| Variant  | What enforces `cpu.max = Q P`                                                     |
|----------|-----------------------------------------------------------------------------------|
| `cfs`    | Stock CFS bandwidth control: Q µs of CPU time per P µs, shared by all threads.     |
| `scx`    | `scx_pfs` (sched_ext): at most floor(Q/P) threads on CPUs at once, no time budget; a fractional remainder is one extra slot with a budget per 10 ms window. `cpu.weight` sets the share under contention. |
| `kwidth` | The same policy inside CFS, enabled with `sysctl kernel.sched_cfs_bandwidth_width=1` on a kernel carrying that patch. |

Kubernetes, Docker, Podman and systemd all express a CPU limit as `cpu.max`,
so nothing else changes: a container started with `--cpus=1.5`, or a unit
with `CPUQuota=150%`, gets `150000 100000`.

A fourth implementation, kPFS (the same policy as a scheduling class of its
own, `CONFIG_SCHED_PFS`, booted with `sched_pfs=1`, see
`Documentation/scheduler/sched-pfs.rst` where that series is applied), needs
no variant of its own: on a `sched_pfs=1` boot the `cfs` variant measures
kPFS. The results below label those runs kPFS.

## Requirements

- cgroup v2 (`stat -fc %T /sys/fs/cgroup` prints `cgroup2fs`), root.
- `scx`: kernel 6.12 or later with sched_ext. Following later changes
  of `cpu.max` at run time needs the `cgroup_set_bandwidth` operation (6.17+).
- `kwidth`: a kernel built with the width-mode patch; the sysctl
  `/proc/sys/kernel/sched_cfs_bandwidth_width` exists.
- gcc with static glibc (`libc6-dev`), python3 (concurrency column), tracefs
  (optional, `WT_TRACE=0` skips the trace).
- Docker path: Docker or Podman (rootful or rootless), systemd or cgroupfs
  cgroup driver.
- systemd path: systemd as init (`systemd-run`).

### Kernel configuration

| Needed for | Options |
|------------|---------|
| every variant | `CONFIG_CGROUPS`, `CONFIG_CGROUP_SCHED`, `CONFIG_FAIR_GROUP_SCHED`, `CONFIG_CFS_BANDWIDTH` (without it `cpu.max` does not exist) |
| `scx` | `CONFIG_SCHED_CLASS_EXT=y`, `CONFIG_BPF=y`, `CONFIG_BPF_SYSCALL=y`, `CONFIG_BPF_JIT=y`, `CONFIG_BPF_JIT_ALWAYS_ON=y`, `CONFIG_BPF_JIT_DEFAULT_ON=y`, `CONFIG_DEBUG_INFO_BTF=y` (pahole 1.25 or later); `tools/sched_ext/Kconfig` lists them |
| `kwidth` | the width-mode patch on top of the above (sysctl `kernel.sched_cfs_bandwidth_width`) |
| kPFS | `CONFIG_SCHED_PFS=y` (needs `CONFIG_SMP`, `CONFIG_CFS_BANDWIDTH`, `CONFIG_FAIR_GROUP_SCHED`, `!CONFIG_SCHED_PROXY_EXEC`) and `sched_pfs=1` on the command line; sched_ext cannot load while it is on |
| concurrency column | `CONFIG_FTRACE` with the `sched:sched_switch` tracepoint and tracefs mounted |
| Docker, Podman, Kubernetes | what the runtime needs (`CONFIG_CGROUP_PIDS`, `CONFIG_MEMCG`, namespaces, overlayfs, ...) |

The measurements below used a distribution-like configuration with
`CONFIG_HZ=1000`, `CONFIG_PREEMPT_DYNAMIC=y` and no lock debugging.

## Build

    make -C tools/sched_ext                     # builds build/bin/scx_pfs
    make -C tools/sched_ext/pfs-test          # static spin, noisy, reqlat

For a container image, link `scx_pfs` statically (libbpf, libelf, zlib and
libzstd static archives installed):

    cd tools/sched_ext
    gcc -static -o build/bin/scx_pfs.static build/obj/sched_ext/scx_pfs.o \
        build/obj/libbpf/libbpf.a -lelf -lz -lzstd -lpthread

The scripts look for scx_pfs in `$SCX`, `pfs-test/bin/`, `../build/bin/`
and `$PATH`.

## Tools

| File               | Purpose |
|--------------------|---------|
| `spin THREADS SECS`  | Busy threads; prints total iterations and the worst gap between two loop iterations of one thread (a stall: throttled, or waiting for a slot). |
| `reqlat THREADS WORK_US PERIOD_US SECS [BURN]` | Open-loop requests: each thread does WORK_US of CPU work every PERIOD_US; latency from scheduled start to completion (p50/p90/p99/p99.9/max). BURN busy threads in the same process stand for GC or background work sharing the limit. |
| `conc.py COMM TRACE CAP` | From a `sched_switch` trace: how many CPUs ran COMM at once (average, maximum, share of time above the limit). |
| `cg-run.sh`        | One case on plain cgroup v2. |
| `systemd-run.sh`   | One case as a transient systemd scope or service (`CPUQuota=`, `CPUWeight=`). |
| `docker-run.sh`    | One case in a container (Docker or Podman). |
| `compare.sh`       | The whole matrix or chosen cells, repeated if asked; CSV and a summary table. |
| `summary.py`       | Summary of a results.csv: median (min-max) per case and variant. |
| `scx_pfs.service`  | Example systemd unit for running scx_pfs on a host. |
| `docker-image.sh`  | Builds a local FROM-scratch image with the static binaries (`docker import`, no registry). |
| `scx-container.sh` | Runs scx_pfs as a privileged container (`--privileged --pid=host`), the Docker shape of a DaemonSet. |

Every script prints its usage with `-h`, runs under `set -euo pipefail`, and on
exit or Ctrl-C removes its cgroups and containers, stops scx_pfs and restores
the sysctl.

### Options and environment

The three case scripts take `VARIANT LIMIT THREADS` and share these options:
`-s SECS` (default 5), `-w spin|reqlat`, `-n none|noisy` (an unlimited
neighbour with one busy thread per CPU), `-H` (CSV header).

| Script | Own options | How the limit is set |
|--------|-------------|----------------------|
| `cg-run.sh` | `-W WEIGHT` (`cpu.weight`, default 100) | writes `LIMIT*PERIOD_US PERIOD_US` to `cpu.max` of `/sys/fs/cgroup/$WT_CG/lim` |
| `systemd-run.sh` | `-m scope\|service`, `-P PERIOD_US` (`CPUQuotaPeriodSec=`), `-W WEIGHT` (`CPUWeight=`) | `CPUQuota=LIMIT*100%`, units in `$WT_SLICE` |
| `docker-run.sh` | `-P PERIOD_US` (`--cpu-period/--cpu-quota` instead of `--cpus`), `-S SHARES` (`--cpu-shares`) | `--cpus=LIMIT` |
| `compare.sh` | `-p cgroup\|docker\|systemd`, `-v`, `-l`, `-t`, `-C LIMIT:THREADS,...`, `-n`, `-L` (latency limits), `-r REPS`, `-s`, `-m`, `-o DIR` | per case, as above |

| Variable | Default | Meaning |
|----------|---------|---------|
| `PERIOD_US` | 100000 | `cpu.max` period of the cgroup path |
| `REQ_THREADS`, `REQ_WORK_US`, `REQ_PERIOD_US` | 4, 1000, 20000 | `reqlat` request threads and their work |
| `REQ_BURN` | 2 | burners next to the request threads in `compare.sh -L` cases |
| `WT_TRACE` | 1 | 0 skips the `sched_switch` trace (no concurrency columns) |
| `WT_CG`, `WT_SLICE` | `pfs-test`, `pfstest.slice` | cgroup and systemd slice used |
| `SCX` | search | path of the `scx_pfs` binary |
| `SCX_ARGS` | empty | extra scx_pfs options, e.g. `-s 2500 -c 500` |
| `SCX_MODE` | `host` | `host` (started by the script), `service` (`systemctl start $SCX_UNIT`), `container` (`scx-container.sh`) |
| `SCX_LOG`, `SCX_UNIT`, `SCX_CTR` | `/tmp/scx_pfs.log`, `scx_pfs.service`, `scx-pfs` | log, unit and container names |
| `DOCKER`, `IMAGE` | `docker`, `pfs-test:local` | runtime command (Podman, rootless Podman) and image |

scx_pfs itself takes `-s SLICE_US` (2500), `-c CUT_US` (500), `-r ROT_US`
(0, off), `-w WINDOW_US` (10000) and `-i SECS` (report interval); see
`tools/sched_ext/README.md`.

## Path A: plain cgroup v2

What the scripts do by hand:

    echo +cpu > /sys/fs/cgroup/cgroup.subtree_control
    mkdir /sys/fs/cgroup/pfs-test
    echo +cpu > /sys/fs/cgroup/pfs-test/cgroup.subtree_control
    mkdir /sys/fs/cgroup/pfs-test/lim
    echo "150000 100000" > /sys/fs/cgroup/pfs-test/lim/cpu.max    # 1.5 CPUs
    echo 100 > /sys/fs/cgroup/pfs-test/lim/cpu.weight
    sh -c 'echo $$ > /sys/fs/cgroup/pfs-test/lim/cgroup.procs; exec ./bin/spin 8 10'
    cat /sys/fs/cgroup/pfs-test/lim/cpu.stat

One case per variant:

    sudo ./cg-run.sh -H cfs 1 8              # stock CFS, 1 CPU, 8 threads
    sudo ./cg-run.sh scx 1 8                 # loads scx_pfs for the run
    sudo ./cg-run.sh kwidth 1 8              # sysctl on for the run
    sudo ./cg-run.sh -n noisy scx 2 64       # with an unlimited busy neighbour
    sudo ./cg-run.sh -w reqlat -n noisy cfs 0.5 2   # request latency, 2 burners

To try scx_pfs on your own workload instead, start it in one terminal
(`sudo ./bin/scx_pfs -i 5`, Ctrl-C to stop) and set `cpu.max` as usual.
kwidth applies at the moment `cpu.max` is written: flip the sysctl, then
rewrite existing limits (`cat cpu.max > cpu.max`).

## Path B: systemd units, no containers

`CPUQuota=` writes `cpu.max` (period 100 ms, `CPUQuotaPeriodSec=` changes
it), `CPUWeight=` writes `cpu.weight`:

    sudo systemd-run --scope -p CPUQuota=150% -p CPUWeight=100 ./bin/spin 8 10
    sudo systemd-run --unit=wt-demo -p CPUQuota=50% ./bin/spin 8 10
    cat /sys/fs/cgroup$(systemctl show -P ControlGroup wt-demo.service)/cpu.stat

    sudo ./systemd-run.sh -H cfs 1.5 8               # transient scope
    sudo ./systemd-run.sh -m service -n noisy scx 1.5 8
    sudo ./compare.sh -p systemd -m scope

The units go into `pfstest.slice` (a `-` in a slice name means nesting to
systemd, `a-b.slice` is a child of `a.slice`).

## Path C: Docker (or Podman)

    sudo ./docker-image.sh                   # image pfs-test:local
    sudo docker run --rm --cpus=1.5 pfs-test:local /spin 8 10
    sudo docker run --rm --cpu-period=100000 --cpu-quota=50000 --cpu-shares=512 \
         pfs-test:local /reqlat 4 1000 20000 10 2

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

Start the scheduler on the host (`sudo ./bin/scx_pfs`), as a systemd
unit, or as a container, which mirrors the Kubernetes DaemonSet:

    sudo install -m 755 bin/scx_pfs /usr/local/sbin/scx_pfs
    sudo install -m 644 scx_pfs.service /etc/systemd/system/
    sudo systemctl daemon-reload
    sudo systemctl start scx_pfs          # stop: back to the kernel scheduler


    sudo ./scx-container.sh start            # needs a static scx_pfs in the image
    sudo ./scx-container.sh status
    sudo ./scx-container.sh stop

    # by hand:
    docker run -d --name scx-pfs --privileged --pid=host --network none \
        pfs-test:local /scx_pfs -i 10

Cases and the matrix:

    sudo ./docker-run.sh -H cfs 1.5 8
    sudo ./docker-run.sh -P 100000 -S 512 kwidth 0.5 8
    sudo SCX_MODE=container ./docker-run.sh scx 1 64
    sudo SCX_MODE=service ./docker-run.sh scx 1 64   # uses scx_pfs.service
    sudo ./compare.sh -p docker

`SCX_MODE` (host, service, container) works with every script. The `path`
column names the runtime and its cgroup driver (`docker-systemd`,
`podman-cgroupfs`, `podman-rootless-systemd`, ...).

Podman: `DOCKER=podman` for every script; the flags are the same.

    sudo DOCKER=podman ./docker-image.sh
    sudo DOCKER=podman ./compare.sh -p docker
    sudo DOCKER="podman --cgroup-manager=cgroupfs" ./docker-run.sh cfs 1 8

Rootless Podman: the scripts still run as root (tracing, sysctl, scheduler)
and drive the user's Podman through `DOCKER`:

    R="sudo -u USER XDG_RUNTIME_DIR=/run/user/$(id -u USER) podman"
    sudo DOCKER="$R" ./docker-image.sh
    sudo DOCKER="$R" ./compare.sh -p docker

`--cpus` needs the cpu controller delegated to the user's systemd instance:
`cat /sys/fs/cgroup/user.slice/user-UID.slice/user@UID.service/cgroup.controllers`
must list `cpu` (default on recent systemd; otherwise add a drop-in for
`user@.service` with `[Service]` `Delegate=cpu cpuset io memory pids`).
`loginctl enable-linger USER` keeps the user's systemd running between logins.
Loading scx_pfs always needs root.

## Path D: Kubernetes

The kit has no Kubernetes driver, but the pieces are the same: the kubelet
writes `cpu.max` for the pod and for each container (`limits.cpu`) and
`cpu.weight` from `requests.cpu` (1 CPU becomes weight 39, 3 CPUs 118), and
scx_pfs runs as a DaemonSet in the shape of `scx-container.sh`:

    apiVersion: apps/v1
    kind: DaemonSet
    metadata: {name: scx-pfs, namespace: kube-system}
    spec:
      selector: {matchLabels: {app: scx-pfs}}
      template:
        metadata: {labels: {app: scx-pfs}}
        spec:
          priorityClassName: system-node-critical
          containers:
          - name: scx-pfs
            image: IMAGE_WITH_STATIC_SCX_PFS
            command: ["/scx_pfs", "-i", "10"]
            securityContext: {privileged: true}
            resources:
              requests: {cpu: 10m, memory: 32Mi}
              limits: {memory: 2Gi}     # loading verifies the BPF program
            readinessProbe:
              exec:
                command: ["/bin/sh", "-c",
                  "grep -qx enabled /sys/kernel/sched_ext/state"]

Deleting the DaemonSet (or its pod) unloads scx_pfs and the node falls back
to CFS within about a second; a killed or restarted scheduler pod did not
fail any transaction of a running database in the tests below. Give the
container at least a few hundred MiB: a 256 MiB limit was not enough to
load an earlier build. For kwidth, set the sysctl on the node and rewrite
the limits (`cat cpu.max > cpu.max` for every pod and container cgroup, or
recreate the pods). For kPFS, boot the node with `sched_pfs=1`.

The database and service results below come from Guaranteed pods (requests
equal to limits) on a 16-vCPU VM, clients in separate Burstable pods (request
1, limit 8) on the same VM, and one workload at a time.

## The matrix

    sudo ./compare.sh                        # cgroup path, all available variants
    sudo ./compare.sh -v cfs,scx -l 0.5,1 -t 8 -s 10
    sudo ./compare.sh -C 1:8,1.5:8,2:64 -L 0.5 -r 3   # key cells, 3 runs each

Limits 0.5, 1, 2 and 4 CPUs, 1, 8 and 64 threads, no neighbour or a noisy
one, then the latency test at 0.5 and 1 CPU. `summary.txt` gives median
(min-max) when cases repeat. Results go to
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
  scx_pfs parks waiting threads in its own queues and never uses CFS
  throttling. Stalls are bounded by the slice (2.5 ms) times the number of
  waiting threads per slot. Throughput is usually higher at the same limit
  because no quota is lost at period boundaries.
- `kwidth`: `avg_conc` at the limit as well, but `nr_throttled` and
  `throttled_usec` grow: a run queue waiting for a slot is parked with CFS's
  own throttling, so here they count slot waits, not exhausted quota.
- scx_pfs counters: `sudo ./bin/scx_pfs -i 5` prints them every 5 s
  (`park_full` = a thread waited for a whole-CPU slot, `park_budget` = the
  fractional slot ran out of budget, `admit` = parked threads let in).
- `/sys/kernel/sched_ext/state` is `enabled` while scx_pfs runs;
  `/sys/kernel/sched_ext/root/ops` names it.

## Results

Direction: ↑ higher is better, ↓ lower is better, "target" means the value
should equal the given number. Medians; the full ranges are in the sources
listed under "Reproducing the numbers".

### 8-vCPU local VM

One kernel image for all variants (the kPFS series, which carries the width
mode and sched_ext; kPFS columns are boots with `sched_pfs=1`), scx_pfs
built from this tree, one variant per boot in rotating order, host load below
3 before each boot. Limited tests: median of 5 boots; tests without limits:
median of 9.

| # | Test | Limit × threads, neighbour | CFS | kwidth | scx_pfs | kPFS |
|---|------|----------------------------|-----|--------|---------|------|
| L1 | throughput, M iterations/s ↑ | 1 × 8, none | 0.61 | 0.81 | 0.83 | 0.85 |
| L2 | throughput, M iterations/s ↑ | 2 × 64, none | 1.22 | 1.61 | 1.62 | 1.65 |
| L3 | worst stall of a thread, ms ↓ | 1 × 64, none | 593 | 353 | 189 | 205 |
| L4 | CPUs running the group at once (target ≤ 1) | 1 × 8, none (cgroup path) | 6.51 | 1.00 | 1.00 | 1.00 |
| L5 | `nr_throttled` in 5 s (0 = never throttled) | 1 × 8, none (cgroup path) | 51 | 504 | 0 | 0 |
| L6 | throughput, M iterations/s ↑ | 2 × 8, noisy | 1.17 | 0.98 | 1.23 | 1.20 |
| L7 | CPUs used by the group (target 2) | 2 × 8, noisy | 1.94 | 1.54 | 1.89 | 1.94 |
| L8 | request latency p99, ms ↓ | 1, 4 request threads + 8 busy | 1036 | 56 | 3.1 | 5.0 |
| L9 | request latency p50, ms ↓ | 1, 4 request threads alone | 1.15 | 3.25 | 3.14 | 3.11 |
| L10 | request latency p99, ms ↓ | 0.5, 4 request threads + 2 busy (cgroup path) | 81.8 | 52.5 | 31.8 | 13.5 |
| L11 | CPU split B/A (target 3.0) | limits 4 and 6, requests 1:3, 16 threads each | 2.96 | 2.04 | 2.61 | 2.85 |
| L12 | worst stall of a weight-1 thread, ms ↓ | no limit, next to 16 busy threads of weight 5000 | 17471 | 14830 | 1472 | 750 |
| L13 | mutex held across 100 `sched_yield()`, events/s ↑ | 2, 16 threads, 4 mutexes | 8k | 38k | 25k | 2k |
| L14 | flag hand-off between spinning threads, ops/s ↑ | 2, 8 pairs | 1.91M | 1.17M | 154k | 1.23M |
| L15 | parallel GC model, pause p99, ms ↓ | 2, 8 mutators | 45.8 | 7.6 | 10.8 | 7.0 |
| L16 | hackbench, 16 groups of processes, s ↓ | no limit | 0.360 | 0.357 | 0.434 | 0.368 |
| L17 | pipe ping-pong, s ↓ | no limit | 3.29 | 3.41 | 0.71 | 3.19 |
| L18 | schbench wakeup p99, µs ↓ | no limit, 2 message threads × 4 workers | 629 | 739 | 157 | 2388 |

The kPFS column of L1-L18 is an earlier kPFS commit. The final kPFS code
against CFS on the same VM (5 boots each, 9 without limits):

| # | Test | Setup | CFS | kPFS (final) |
|---|------|-------|-----|--------------|
| K1 | mutex held across 100 `sched_yield()`, events/s ↑ | 2, 16 threads, 4 mutexes | 8263 | 42250 |
| K2 | schbench wakeup p99, µs ↓ | no limit, 2 × 4 workers | 1282 | 351 |
| K3 | worst stall of a weight-1 thread, ms ↓ | as L12 | 9653 | 493 |
| K4 | flag hand-off, ops/s ↑ | 2, 8 pairs | 1.60M | 0.29M (bimodal, 12.6k-3.3M) |
| K5 | throughput, M iterations/s ↑ / CPUs (target 4) | 4 × 64, none | 2.39 / 3.97 | 2.88 / 3.94 |
| K6 | parallel GC model, pause p99, ms ↓ | 2, 8 mutators | 50.0 | 8.5 |
| K7 | CPU split B/A (target 3.0) | as L11 | 2.94 | 2.88 |

### 16-vCPU VM with Kubernetes pods

Limited pods on a 16-vCPU VM, clients in separate pods on the same VM, one
workload at a time, 3 runs or more per cell unless noted. Kernels: CFS
(7.0) and scx_pfs on kernel 7.0; CFS (7.3), kwidth and kPFS on kernel 7.3
(kwidth and kPFS builds of their series; 7.3 alone is 1-33% faster than 7.0
depending on the workload, so compare kwidth and kPFS with CFS (7.3)). The
reference is a 4-vCPU VM without limit.

| # | Test | Limit, setup | CFS (7.0) | CFS (7.3) | kwidth | scx_pfs | kPFS* |
|---|------|--------------|-----------|-----------|--------|---------|-------|
| C1 | PostgreSQL TPC-B, 32 clients, tps ↑ | 4 | 9003 | 9119 | 8261 | 6050 | 7319 |
| C2 | same, p99 ms ↓ (4-vCPU reference: 14.1 at 6673 tps) | 4 | 46.9 | 35.7 | 22.0 | 10.0 | 8.99 |
| C3 | same, CPUs used (target ≤ 4) | 4 | 3.91 | 3.88 | 3.52 | 3.02 | 4.19 |
| C4 | PostgreSQL TPC-B, 32 clients, tps ↑ / p99 ms ↓ | 2 | 4556 / 55.1 | 4673 / 79.9 | 5002 / 53.3 | 3087 / 18.7 | 4356 / 14.0 |
| C5 | PostgreSQL select-only, 32 clients, tps ↑ / p99 ms ↓ | 4 | 62666 / 12.4 | 63774 / 6.45 | 66764 / 12.6 | 39849 / 1.46 | 52682 / 1.56 |
| C6 | MariaDB read-write, 32 threads, tps ↑ / p99 ms ↓ | 4 | 2111 / 60.0 | 2297 / 45.8 | 2162 / 40.4 | 1542 / 30.5 | 1884 / 26.7 |
| C7 | PostgreSQL TPC-B beside an unlimited 16-thread CPU hog, tps ↑ / p99 ms ↓ | 2 | 4460 / 73.3 | - | 3070 / 65.3 | 2861 / 23.6 | 3591 / 24.9 |
| C8 | Redis, 4 I/O threads, SET ops/s ↑ | 4 | 177.7k | 210.5k | 222.2k | 95.0k | 210.5k |
| C9 | Redis, 4 I/O threads, SET ops/s ↑ | 2 | 85.8k | - | < 1.7k | 5.1k | 5.35k |
| C10 | JVM ParallelGC at a fixed rate, worst GC pause ms ↓ | 4 | 28.5 | 24.2 | 24.9 | 198 | 23.3 |
| C11 | nginx gzip at 2395 req/s, p99 ms ↓ | 4 | 2.94 | 2.83 | 3.26 | 5.82 | 3.00 |
| C12 | web server, 500 µs per request, max req/s ↑ | 4 × 8 threads, heavy neighbours | 5337 | 5406 | 4902 | 3197 | 5565 |
| C13 | same at a fixed rate, p99 ms ↓ | 0.25 × 8 threads, heavy neighbours | 347 | 47.9 | 123 | 64.7 | 145 |
| C14 | hackbench processes in a pod, s ↓ | 4 | 3.26 | 2.87 | 2.51 | 2.22 | 2.12 |
| C15 | mutex held across 100 `sched_yield()`, events/s ↑ | 4, 16 threads, 4 mutexes | 11.1k | 12.7k | 37.2k | 37.6k | 46.1k |
| C16 | 4 busy-polling threads, rounds/s ↑ | 2 | 10831 | 10770 | 86 | 167 | 167 |
| C17 | 100 pods all Ready, s ↓ | limits 0.25-1 | 45.5 | - | 33.0 | 22.4 | 31.2 |
| C18 | 1 ms sleeper outside the pods, overshoot p99 ms ↓ | 100 pods | 1.0 | - | 2.9 | 8.0 | 2.7 |
| C19 | two busy pods limited to 4 and 8, demand below the machine, CPUs (target 4 / 8) | 4 and 8 | 4.00 / 8.00 | - | 3.50 / 6.35 | 4.00 / 7.99 | 4.00 / 8.00 |
| C20 | unlimited pod's schbench wakeup p99, ms ↓ | next to 4 pods limited to 3 with 8 spinning threads each | 0.90 | - | 1.24 | 0.20 | 2.80 |

\* kPFS on the 16-vCPU VM is a build from before the fix that charges
runtime on credit (tasks running in the kernel without a slot) to the limit:
its database pods used 2-13% more than the limit (C3). With the fix, the same
databases on the 8-vCPU local VM stay at the limit and lose the matching
throughput (expected about -5% at limit 4 and -11% at limit 1.5 on the
16-vCPU VM). A remeasurement with the fixed build is in progress.

## Reproducing the numbers

- L1-L3, L6-L7: `sudo ./compare.sh -v cfs,scx,kwidth -l 0.5,1,2,4 -t 1,8,64
  -n none,noisy -L none -s 5`, once per boot; on a `sched_pfs=1` boot run
  `-v cfs` for kPFS. CPUs come from the `sched_switch` trace over the run,
  not from `cpu.stat`, see below.
- L4, L5, L10: `sudo ./compare.sh -p cgroup -C 1:8,1.5:8,2:64 -n none,noisy
  -L 0.5 -s 5` (and `-p docker`, `-p systemd` for the other paths, which gave
  the same picture within a few percent).
- L8, L9: `sudo ./cg-run.sh -w reqlat VARIANT 1 8` and `... VARIANT 1 0`.
- L11, K7: two pod-like trees (`kubepods.slice/.../podX.slice/ctr.scope`)
  with `cpu.weight = 1 + (shares - 2) * 9999 / 262142`, shares = request in
  milli-CPUs * 1024 / 1000, 16 busy threads each, limits 4 and 6; the ratio of
  `usage_usec` over 8 s.
- L12, K3: one thread in a group with `cpu.weight` 1 next to two groups of 8
  busy threads with weight 5000, 20 s; the longest gap of the weight-1 thread.
- L13-L15, K1, K4, K6, C15, C16: probe programs not in this kit, run in a
  two-level cgroup (pod and container, the same `cpu.max` on both): 16
  threads that take one of 4 mutexes and call `sched_yield()` 100 times
  before releasing it (as sysbench `threads`), 8 pairs of threads passing a
  flag in a busy loop, a parallel GC model (8 spinning mutators stopped at a
  safepoint, GC workers on a futex, spin+yield termination), 4 threads
  busy-polling flags (Redis I/O threads).
- L16-L18, K2: `perf bench sched messaging -p -g 16 -l 400`,
  `perf bench sched pipe -l 100000`, `schbench -m 2 -t 4 -r 5 -i 100`, no
  cgroup limits.
- C1-C7: pgbench scale 50 (in memory), `-j 4 -T 120`, `synchronous_commit=off`,
  `shared_buffers=512MB`; sysbench 1.0.20 `oltp_read_write`, 8 tables x 200k
  rows, 90 s; MariaDB 11.4 with a 1 GB buffer pool. The hog is a BestEffort
  pod with 16 spinning threads.
- C8-C11: `redis-benchmark -c 64 -n 2000000 -t set,get -d 256 --threads 4`
  against Redis 7.4 with `--io-threads 4`; nginx 1.27 compressing 48 KiB at
  gzip level 6 per request and a JVM (Temurin 21, ParallelGC, 512 MiB heap, 32
  handler threads) at 0.65 x the capacity of the 4-vCPU reference x limit/4,
  driven by `fortio -uniform -c 32` for 60 s.
- C12, C13: a server burning 500 µs of CPU per request, 256 connections,
  60 s, closed loop and at 0.6 x the closed-loop maximum. Heavy neighbours:
  two BestEffort pods with 16 spinning threads and 20 limited pods.
- C14, C17-C20: hackbench (rt-tests), schbench and sysbench in pods with
  requests equal to limits; 50 and 100 small pods with limits 0.25-1 (busy,
  30% duty and idle) for scale; for overcommit 8 pods (request 1, limit 4)
  and 2 pods (request 3, limit 8), of which one of each kind is busy in C19.

## Known disadvantages

Of all width variants (a cap on concurrency instead of a time budget):

- CFS lets a limited group burst onto every CPU for part of a period; a width
  cap never runs more than the limit. Short parallel bursts and requests in
  an otherwise idle group therefore wait for a slot: four request threads at
  limit 1 have a median latency of about 3.1 ms instead of 1.15 ms (L9).
- More spinning or busy-polling threads than slots collapse: each hand-off
  waits for a rotation (C9, C16, L14). Keep busy-polling threads (Redis
  `io-threads`, spin-wait locks) at or below the limit.
- Database throughput is below CFS at the same limit (C1, C5, C6), with much
  better tails.
- `cpu.max.burst` is not honoured.

scx_pfs:

- Databases lose 27-37% of their throughput against CFS (pods use 2.8-3.0 of
  4 CPUs); against the 4-vCPU reference, limit 4 gives 89-91% of its
  throughput.
- `cpu.stat` over-reports the usage of cgroups with many threads under
  sched_ext (64-thread cells showed 1.34 CPUs at limit 1 while the trace
  showed the cap); the kit counts CPUs from the trace.
- Flag hand-off between spinning threads at limit 2: 0.08 of CFS (L14).
- A web server at limit 4 with 8 threads next to heavy neighbours collapsed
  (C12: 3197 req/s using 2.23 of 4 CPUs, fixed rate missed with p99 14.9 s).
- With 50-100 pods, the host's 1 ms sleeper overshoots by 4.7-8.0 ms at p99
  (C18) and migrations are about 100 times as frequent as under CFS.
- Redis with 4 I/O threads at limit 4 runs at about half of CFS (C8).

kwidth:

- Slots are per-CPU run queues: next to a busy neighbour the group gets
  1.54-1.82 of 2 CPUs (L7) and 0.74-0.94 of CFS's throughput.
- Weights and limits interact badly (L11: 2.04 instead of 3.0), overcommitted
  limits are off the weighted shares by up to 16%, and limits are not reached
  when demand is below the machine (C19).
- With 100 pods, kubectl exec takes 500 ms and the kubelet health check
  197 ms at the median (CFS: 216 and 73 ms).
- Fractional limits keep a time budget per 10 ms window and `nr_throttled`
  counts slot waits (L5).

kPFS:

- `nr_throttled` is always 0; waiting is shown in
  `/sys/kernel/debug/sched/pfs/stats` and `groups`.
- Runtime in the kernel on credit can push short windows (below 100 ms) a
  few percent over the limit; runs of seconds stay at the limit with the fix.
- An unlimited pod next to busy limited pods has a worse wakeup tail (C20:
  2.8 ms vs 0.9 ms) and 17% fewer schbench requests/s.
- Web tails at fractional limits with heavy neighbours are worse than CFS on
  7.3 (C13: 145 vs 48 ms p99 at limit 0.25).

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
  (`docker info | grep -i cgroup`): `system.slice/docker-<id>.scope` or
  `docker/<id>`; Podman uses `machine.slice/libpod-<id>.scope`,
  `libpod_parent/libpod-<id>` (cgroupfs) or
  `user.slice/user-UID.slice/user@UID.service/user.slice/libpod-<id>.scope`
  (rootless). Always find it through the container's PID as above.
- Rootless Podman fails with a cpu controller error: the controller is not
  delegated to the user, see above.
- scx_pfs exits with a stall error: the kernel watchdog ejects a sched_ext
  scheduler that leaves a runnable task waiting too long
  (`runnable task stall` in `dmesg`), and all tasks fall back to the normal
  scheduler at once. The workload keeps running, now under CFS. Report the
  log and the case.
- Another scheduler already loaded (`state` is `enabled` and `ops` names a
  different one): only one sched_ext scheduler can run at a time.
- kwidth seems inactive: the sysctl applies when `cpu.max` is written;
  rewrite existing limits after enabling it.
- `cpu.max.burst` is honoured by `cfs` only.
