.. SPDX-License-Identifier: GPL-2.0

==============================
Parallel Fair Scheduler (PFS)
==============================

PFS is an alternative implementation of the normal scheduling class
(``SCHED_NORMAL``, ``SCHED_BATCH`` and ``SCHED_IDLE``) in which a cgroup CPU
limit is a cap on parallelism instead of a CPU time budget. It is a separate
scheduling class, ``pfs_sched_class`` in ``kernel/sched/pfs.c``, derived from
the EEVDF implementation in ``kernel/sched/fair.c``. It is built with
``CONFIG_SCHED_PFS`` and is off unless the kernel is booted with
``sched_pfs=1``; with it off, the normal class is the unmodified fair class.

.. contents::
   :local:


Motivation
==========

With CFS bandwidth control (Documentation/scheduler/sched-bwc.rst), the cgroup
v2 file ``cpu.max`` ("quota period") gives a cgroup a pool of CPU time which is
refilled every period. All threads of the cgroup draw from the pool and once
it is empty the whole cgroup is throttled until the next period. A cgroup
limited to 2 CPUs with 16 runnable threads runs on 16 CPUs for 1/8 of the
period and then stalls for the rest of it. Container orchestrators translate
every CPU limit into ``cpu.max`` with a 100 ms period, so multi-threaded
programs under a limit see stalls of tens of milliseconds even when their
average usage is well below the limit.

PFS reads the same ``cpu.max`` as a width: a cgroup limited to N CPUs may have
at most N of its threads running at any time. There is no time budget for
whole CPUs, so a cgroup which keeps its concurrency within the limit never
stalls, and threads beyond the limit wait in a queue of the cgroup instead of
running in a burst and then being throttled together.

Two earlier implementations of the same policy shaped this design:

- A width mode for CFS bandwidth throttling, where a slot was one CPU's
  ``cfs_rq`` of the group. Waiting threads were stuck on per-CPU queues, a
  freed slot was handed to another CPU by a timer and an IPI, and on the
  slot's CPU the group competed with its neighbours by the group entity's
  weight, so a group limited to 2 CPUs averaged 1.7 CPUs next to a busy
  neighbour and lost 10-27% of its throughput compared with plain CFS.

- A sched_ext BPF scheduler with one waiting queue per cgroup, admission of
  individual threads, a slot held only while the thread runs, and hand-off of
  a freed slot to the next waiting thread on the CPU which freed it. It beats
  CFS bandwidth control in the same tests, at the cost of running everything
  through BPF and a global run queue.

PFS implements the second policy natively, with per-CPU run queues.


Enabling
========

``CONFIG_SCHED_PFS`` depends on ``CONFIG_CFS_BANDWIDTH`` (for ``cpu.max``)
and on ``CONFIG_FAIR_GROUP_SCHED`` (for ``cpu.weight`` and the task group
tree). The class is selected once at boot:

``sched_pfs=1``
   All ``SCHED_NORMAL``, ``SCHED_BATCH`` and ``SCHED_IDLE`` tasks use PFS
   instead of the fair class, from the first task created onwards.

Switching at run time is not supported: every task would have to move between
two classes with different run queue and group state, which sched_ext shows
is possible but complex, and the use case (a node configured for a container
workload) does not need it. ``/sys/kernel/debug/sched/pfs/`` shows the state
and the counters described under `Statistics`_.

With ``sched_pfs`` absent or 0, a static key keeps every hook in the core
scheduler disabled, the class is skipped by the class iterators, and the
kernel behaves as one built without ``CONFIG_SCHED_PFS``.

sched_ext cannot be enabled while PFS is active (``scx_enable`` fails with
``-EBUSY``); both would want to own the normal class.


Semantics
=========

Limits
------

``cpu.max`` = "Q P" with Q != ``max`` sets the group's limit L = Q / P CPUs,
kept in milli-CPUs (rounded up).

- floor(L) *full slots*: up to floor(L) threads of the group may run at once
  without any time accounting.
- If L has a fractional part, one *budget slot*: one more thread may run while
  the group's CPU time in the current window is below L * window. The window
  is 10 ms by default (``window_us`` in debugfs), aligned to multiples of the
  window on the monotonic clock. Usage above the budget (a slice ends on a
  tick) is carried over into the next window, at most one budget's worth, so
  that the long-term average stays at L. A limit below 1 has only the budget
  slot, i.e. it is still a time budget, but over 10 ms instead of the 100 ms
  period of ``cpu.max``.
- The period P only matters through the ratio Q / P.
- ``cpu.max.burst`` is accepted and ignored: a parallelism cap has no
  accumulated budget to burst from. (The carry-over of the budget slot is
  bounded by one window independently of it.)
- ``cpu.stat`` keeps reporting CFS bandwidth fields, which stay at zero under
  PFS; waiting is reported in the PFS statistics instead.

Limits are hierarchical: a thread needs a slot in its own group and in every
limited ancestor. Each limited group counts the slots taken by threads of its
whole subtree.

Admission and waiting
---------------------

A runnable task of a limited hierarchy must be *admitted* before it is put on
a CPU run queue. Admission takes one slot at every limited level of its path,
deepest level first. If a level has no free slot, or its budget slot has no
budget left, or already has waiting tasks (so that newcomers do not overtake
them), the task is *parked*: it waits in that level's FIFO queue, is not on
any CPU run queue, is not counted in ``rq->nr_running`` and is invisible to
load balancing. Tasks of groups without a limit anywhere on their path are
admitted unconditionally and behave as described in `Scheduling`_.

A task gives its slots back when it

- blocks, exits, or moves to another cgroup;
- reaches the end of its slice (``sched_base_slice``) while one of its levels
  has waiting tasks, has no budget left, or holds more slots than its limit
  allows (after the limit was lowered). It then has to be admitted again, and
  goes to the tail of the queue if it cannot be;
- is a waiting task's slot hand-off source, see below.

A task which is preempted by another task of its CPU keeps its slots while it
waits on that CPU's run queue, and is pushed to an idle CPU if one is allowed;
see `Slot holding while preempted`_ for why.

Same-CPU hand-off
-----------------

When a task gives its slots back on CPU *C* because it blocks or rotates, the
scheduler on *C* is about to pick the next task. Before it does, it takes the
first waiting task of the deepest level that has waiting tasks, admits it, and
migrates it to *C* (double run queue lock, as the RT class pulls tasks), so
that the next thread of the group runs on the CPU that just became free,
without a timer or an IPI. A waiting task which can't run on *C* (affinity,
migration disabled, *C* going offline) is admitted where it is instead, and
an allowed idle CPU, if any, is kicked so that it pulls the task.

Slots freed elsewhere (a task leaving the group, a raised limit, a new budget
window) are handed out by a *retry*: the first waiting tasks are admitted on
their own CPUs as long as there are free slots, again kicking idle CPUs.

Weights
-------

``cpu.weight`` (and ``cpu.weight.nice``, and ``cpu.shares`` on cgroup v1)
sets the group's share under contention, hierarchically, as in the fair
class. PFS flattens the hierarchy as scx_flatcg does: a group is *active*
while a task of its subtree is admitted. A group g other than the root has the
hierarchical weight::

   H(g) = W(g)                              if g's parent is the root
   H(g) = H(parent) * W(g) / D(parent)      otherwise

   D(p) = sum of W(c) over the active children c of p
        + sum of w(t) over the admitted tasks t of p itself

where W is the group's weight (``tg->shares``, 1024 for ``cpu.weight`` 100)
and w(t) is the task's nice weight. An admitted task t of group g is
scheduled with the *effective weight*::

   w_eff(t) = H(g) * w(t) / D(g)            (g != root)
   w_eff(t) = w(t)                          (tasks of the root group)

So the admitted tasks of a group share its hierarchical weight. The key
difference from scx_flatcg and from the fair class is that only *admitted*
tasks count: a group limited to 2 CPUs with 64 runnable threads gives its whole
weight to the 2 threads holding its slots, which therefore keep their CPUs
against neighbours instead of getting a 1/32 share of them. The waiting
threads take turns on the slots at slice granularity, which gives them their
share of the group's weight over time.

Effective weights are computed when a task is admitted and refreshed on every
tick for the running task. Hierarchical weights are cached per group and
recomputed at most once per millisecond when the set of active groups or a
weight has changed (a global generation counter). w_eff is clamped to
``[scale_load(2), scale_load(MAX_SHARES)]``; the floor of 2 bounds the wait of
a task of a group with a tiny share (the starvation floor), as ``MIN_SHARES``
does in the fair class.


Scheduling
==========

Per CPU, admitted tasks are scheduled by EEVDF exactly as in the flat fair
class of this kernel: one augmented rbtree per CPU ordered by virtual deadline,
eligibility from the weighted average virtual runtime, lag preserved across
sleep, migration and reweighting, ``sched_base_slice`` requests, custom slices
from ``sched_attr::sched_runtime``, wakeup preemption of an eligible task with
an earlier deadline, no wakeup preemption by ``SCHED_BATCH`` and
``SCHED_IDLE`` tasks. The effective weight takes the place of the fair class'
``se->h_load`` weight.

Not carried over: delayed dequeue, next/short buddies, protected slices
(``RUN_TO_PARITY``), PELT per task and per group, util_est, energy aware
placement, misfit migration, NUMA balancing and ``cpu.idle`` (treated as a
plain weight).


Data structures
===============

``struct sched_pfs_entity`` (``task_struct::pfs``)
   rbtree node, virtual runtime, deadline, lag, slice, effective weight, the
   state (``PFS_NONE``, ``PFS_QUEUED``, ``PFS_WAITING``, ``PFS_PARKING``),
   the mask of hierarchy levels where the task holds a slot, the group it is
   accounted to, its wait queue link and the runtime at admission (for slice
   rotation). Accounting fields shared with other classes (``exec_start``,
   ``sum_exec_runtime``) stay in ``task_struct::se``.

``struct pfs_rq`` (``rq::pfs``)
   the EEVDF tree and its sums, the number of queued (admitted) tasks and the
   sum of their effective weights (the CPU's load for balancing), the list of
   tasks waiting while their ``task_cpu()`` is this CPU (for hotplug), a
   pending hand-off and balancing timestamps.

``struct pfs_group`` (``task_group::pfs``)
   limit in milli-CPUs, slots taken (atomic), the FIFO wait queue and its lock,
   the budget window (window id, usage, hrtimer for the next window), weight,
   active children count and their weight sum, own admitted tasks' count and
   weight, cached hierarchical weight with its generation, depth.

A waiting task stays on the run queue of its CPU from the core scheduler's
point of view (``p->on_rq == TASK_ON_RQ_QUEUED``, ``task_cpu(p)`` unchanged);
only PFS knows that it is in a group's queue instead of a CPU tree. All PFS
state of a task is protected by ``task_rq(p)->lock``, so the core paths that
dequeue and enqueue a task (affinity, priority and cgroup changes, migration)
work on waiting tasks too: dequeue removes the task from its wait queue,
enqueue admits it again.


Locking
=======

Lock order::

   p->pi_lock
     rq->__lock (two run queues: double_rq_lock() order)
       pfs_group::lock (wait queue; never two at once)
         pfs_weight_lock (active sums, hierarchical weights)

- Slot counts are atomics, taken with increment-and-check and given back with
  a decrement. A task's slot mask, state and group are protected by its run
  queue lock.
- Lost wake-ups between a parking task and a releasing task are prevented as
  with wait queues: the parking side adds itself to the queue, then
  (``smp_mb()``) checks for a free slot and starts a retry if there is one;
  the releasing side gives the slot back, then (``smp_mb__after_atomic()``)
  checks the queue.
- To admit a waiting task found under ``pfs_group::lock``, the scheduler takes
  a reference, drops the group lock, takes the task's run queue lock (plus its
  own, with ``double_lock_balance()``), and checks that the task is still
  waiting in that queue before touching it.
- The running task is parked only from ``put_prev_task()``, after which the
  run queue lock is held until ``finish_task_switch()`` has cleared
  ``p->on_cpu``. A remote CPU holding a run queue lock therefore never sees a
  waiting task that is still switching out. A task parked while it keeps
  running (a cgroup move of the running task) is admitted in place by a hand-
  off or retry, never migrated.
- ``pick_task()`` and ``balance()`` drop the run queue lock only through
  ``rq_unpin_lock()``/``double_lock_balance()`` after ``rq_modified_begin()``,
  and return ``RETRY_TASK`` when a higher class became runnable meanwhile, as
  the fair class' newidle balance does.


Hand-off and rotation in detail
===============================

The previous task *prev* on CPU *C* gives its slots back in one of two places,
both followed by a pick on *C*:

- ``dequeue_task(DEQUEUE_SLEEP)`` from ``__schedule()`` when it blocks;
- ``balance()`` when its slice has ended and it must rotate (marked by the
  tick, which also requests the reschedule). *prev* becomes ``PFS_PARKING``:
  no slots, not pickable, on no list.

``balance()`` then hands the freed slots to the first waiting task(s) with
*C* as the target. Finally ``put_prev_task()`` resolves a parking *prev*: it
tries to admit it again (FIFO, so it only succeeds when nobody waits and
budget is left) and otherwise appends it to the refusing level's queue.

Retries (``pfs_group_retry()``) run from the ``cpu.max`` write, from the
budget window hrtimer, and from an ``irq_work`` when a release happens in a
context without a following pick (a waiting or queued task leaving the group,
a hand-off that found no task for its CPU).


Slot holding while preempted
============================

A strict "slot only while running" rule would release the slot of a task that
another task of its CPU preempts. The freed slot would then need a CPU that
can run a waiting task right now; if there is an idle one, PFS gets the same
effect by pushing the preempted task there with its slot. If there is none,
every CPU is busy and the task competes on its CPU with its effective weight,
which is the whole weight of the group split over its slots, so it gets the
CPU back quickly; moving the slot to another waiting thread would only change
which thread waits. A preempted task does not consume runtime and so does not
rotate; slots are bounded by the number of admitted tasks either way.


Interaction with cgroups and task groups
========================================

PFS keeps using ``struct task_group``: ``cpu.max``, ``cpu.weight`` and
``cpu.weight.nice`` (and the cgroup v1 files) are unchanged, so container
runtimes, systemd and Kubernetes need no change. The core calls into PFS from
``tg_set_bandwidth()`` and the weight writers after the fair class has stored
its values, and from group creation and destruction. Autogroups are task
groups and are handled the same way. A task moving between groups is dequeued
(giving its slots back in the old group) and enqueued again (admission in the
new one) by ``sched_move_task()``.

When PFS is active the fair class has no tasks: CFS bandwidth state is still
configured by ``cpu.max`` writes but its timers never run, and the fair load
balancer softirq is not raised.


Load balancing
==============

PFS has its own, deliberately simple balancer; the fair class' sched domain
balancer is not used.

- Wake-up placement (``select_task_rq``): the previous CPU if idle, else an
  idle CPU in the previous CPU's LLC, else an idle CPU anywhere in the task's
  affinity mask, else the previous CPU. A task of a limited group whose
  levels have no free slot stays on its previous CPU (it would only park).
- Newly idle pull (``pick_task`` with an empty tree): pull a queued, not
  running, allowed task from the busiest CPU (by queued count, then load),
  searching the LLC first.
- Push (a balance callback after a context switch, at most once per tick per
  CPU, and right away for a preempted task of a limited group): move a
  queued task to an idle CPU, or to a CPU whose load is lower by more than the
  task's weight.
- Idle CPUs that need to pull are kicked with ``resched_cpu()``; they pull in
  their newly idle pick.

Waiting tasks are never on a CPU tree, so no balancer can run them before
they are admitted. Load is the sum of the effective weights of the admitted
tasks of a CPU, which makes slot holders of limited groups heavy: CPUs running
them do not receive other tasks while lighter CPUs exist.


CPU hotplug
===========

Admitted tasks leave a CPU going offline through the core's ``balance_push``
like tasks of other classes. Waiting tasks whose ``task_cpu()`` is the dying
CPU are moved to an active CPU of their affinity mask in
``sched_cpu_dying()`` and admitted there or parked again. Admission never puts
a task on the tree of an inactive CPU.


Shared and forked code
======================

Forked from ``fair.c`` and adapted to a single per-CPU tree of tasks with
effective weights: virtual runtime averaging, eligibility, lag and placement,
deadlines, the augmented tree pick, reweighting with lag scaling, yield.

Reused from the core and other classes: ``update_curr_common()`` for runtime
accounting (cputime, cgroup accounting), the per-CPU ``fair_server`` deadline
server (which serves the normal class, whichever implements it), PELT for a
per-CPU utilisation signal for schedutil (as for the RT class),
``set_cpus_allowed_common()``, ``double_lock_balance()``, balance callbacks.

Not used: ``cfs_rq`` hierarchy, group scheduling entities, CFS bandwidth
throttling, the fair load balancer, task PELT.


Statistics
==========

``/sys/kernel/debug/sched/pfs/stats`` sums per-CPU counters: admissions,
parks (full, budget, FIFO), hand-offs (same CPU, in place), retries,
rotations, pulls and pushes. ``/sys/kernel/debug/sched/pfs/groups`` lists
limited groups with limit, slots taken, waiting tasks and budget usage.


Limits of this implementation
=============================

- The balancer scans CPU masks linearly; it is meant for machines up to a few
  hundred CPUs, not for large NUMA systems.
- Active-set changes of groups take one global spin lock (only on 0 <-> 1
  transitions of a group's admitted count).
- The budget slot is accounted per tick; a group can overrun its budget by up
  to a tick per running thread per window, which is carried over.
- Effective weights of queued, not running tasks are refreshed only when they
  are next admitted or start running.
- Group hierarchies deeper than 63 levels are only limited on their top 63
  levels.
