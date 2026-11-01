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

A script that needs one of the required variables fails immediately with the
message in its `${VAR:?...}` expansion rather than falling back to a guess.

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
  over the threshold.
* `position.py` - the within-boot position effect as a number of its own.
* `headline.py` - the equal-CPU work ratio at one limit, three ways.
* `final.py` - the result over the kept boots: position effect, the **two-factor
  (arm + position) contrast** per cell with delivered CPU beside every ratio, and
  the paired per-boot ratio with a one-sample t test on the log ratio.
* `boots.py`, `report.py`, `sigma.py` - per-boot inventory, report and sigma.

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
