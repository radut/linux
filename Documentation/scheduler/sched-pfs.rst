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

Configuration
-------------

``CONFIG_SCHED_PFS`` depends on ``CONFIG_SMP``, ``CONFIG_CFS_BANDWIDTH`` and
``CONFIG_FAIR_GROUP_SCHED``, and is not available with
``CONFIG_SCHED_PROXY_EXEC``. The statistics and tunables below need
``CONFIG_DEBUG_FS``. Slices and the rotation of slots end on a tick unless
the hrtick ends them, so ``CONFIG_HZ`` sets their granularity; the
measurements below used ``CONFIG_HZ=1000`` and ``CONFIG_PREEMPT_DYNAMIC``.
With ``CONFIG_SCHED_PFS=n`` nothing of PFS is compiled in.

How the cgroup files map
------------------------

========================== ===================================================
``cpu.max`` "Q P"          limit L = Q / P: floor(L) slots without time
                           accounting, plus a budget slot for the fraction
``cpu.max`` "max P"        no limit at this level
``cpu.max.burst``          ignored
``cpu.weight``,            the group's weight W, used for the share under
``cpu.weight.nice``        contention and for sharing a limited parent's slots
``cpu.idle``               ignored; the weight applies as set
``cpu.stat`` usage_usec    CPU time of the group's tasks, including runtime
                           on credit
``cpu.stat`` nr_periods,   always 0 under PFS: tasks wait for a slot instead
nr_throttled,              of being throttled; see `Statistics`_
throttled_usec
``cpu.uclamp.min/max``     not applied to frequency selection (no per-task
                           utilisation under PFS)
cgroup v1 ``cpu.shares``,  the same as their cgroup v2 counterparts
``cpu.cfs_quota_us``,
``cpu.cfs_period_us``
========================== ===================================================

Examples, with the default 10 ms window:

- ``cpu.max`` "150000 100000" (1.5 CPUs): one thread runs at any time
  without accounting, a second one while the group used less than 15 ms of
  CPU in the current 10 ms window.
- "25000 100000" (0.25 CPUs): only the budget slot, one thread at a time,
  2.5 ms of CPU per 10 ms window.
- A pod limited to 4 CPUs with two containers limited to 2 and 3 CPUs: at
  most 4 threads of the pod run at once, at most 2 and 3 of each container,
  and when both containers want more, the pod's 4 slots are shared between
  them by their ``cpu.weight``.


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
  tick, tasks run on credit) is carried over into the following windows as
  debt, at most what the limit allows in one second, so that the long-term
  average stays at L. A limit below 1 has only the budget
  slot, i.e. it is still a time budget, but over 10 ms instead of the 100 ms
  period of ``cpu.max``.
- The period P only matters through the ratio Q / P.
- ``cpu.max.burst`` is accepted and ignored: a parallelism cap has no
  accumulated budget to burst from.
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

A task is only parked where it holds nothing others may wait for: when it is
preempted on its way back to user space. A task that wakes up, a new task, or
one moved to another cgroup is in the kernel at that point, may hold locks or
be inside an RCU read-side section (with preemptible RCU), and a long wait
for a slot would block everybody waiting for those, or stall RCU grace
periods. If such a task finds no free slot, it is admitted *on credit*,
without slots, and a task work makes it take its slots or park when it
returns to user space. This is the same model as the task based throttling
of CFS bandwidth control. Kernel threads, which never return to user space,
keep running on credit until they block.

Some tasks never wait for a slot and run on credit wherever they would park:
a task with a fatal signal pending or chosen by the OOM killer (it must exit
to free its memory), and a task that holds a PI lock (an rt_mutex or a PI
futex) with waiters, or is boosted by one. A waiting task that receives
``SIGKILL`` is admitted on credit at once (the signal code kicks it, and an
``irq_work`` on its CPU admits it), as is a waiting task whose priority a PI
waiter boosts. A PI waiter of the same priority as the holder does not boost
it, so a holder that is already waiting is then only admitted in turn.

Runtime on credit is beyond the slots, so it is paid back. Every limited
level counts the CPU time of all tasks of its subtree, with or without slots,
in the windows of the budget slot. If the usage in the current window,
including what was carried over, exceeds what the full slots could have used
since the window began plus the budget of the budget slot, by more than a
margin, the level does not hand out its last full slot nor its budget slot,
and a slot holder whose slice ends while the level holds that many slots
gives its slots back. A timer retries when the remaining full slots would
have paid the excess back. Each task charges its runtime to the levels in
batches of 100 us, so that tasks which switch every few microseconds don't
bounce the counters between CPUs; what is left of a batch is charged when the
task exits, moves to another group or leaves the class. The margin, one tick per slot plus one
window's budget, absorbs runtime charged late (by a tick or a batch, or by
much more in a virtual machine whose CPU the host preempted), which would
otherwise look like an excess at the start of a window; it bounds the excess
only once.
Without this, a database with many backends, which wake up for every query
and run on credit until they return to user space, used 2-20% more than its
limit: each wakeup without a free slot costs a few microseconds in the
kernel (finishing the system call, the task work, two context switches),
and they add up to a tenth or more of a CPU. CFS bandwidth control charges
the same time to its quota.

While a level is over its limit in this sense, the tasks of its subtree that
run on credit are scheduled with the weight of ``SCHED_IDLE`` tasks (and the
running one is preempted at the next tick if others are queued), so they only
get CPU time nobody else wants. Without this, a group whose work runs mostly
in the kernel (long system calls that block and resume, I/O worker threads)
would run on as many CPUs as it has threads, since such tasks rarely reach
the return to user space where they would park. The debt is carried over up
to what the limit allows in one second, which bounds the wait it causes;
runtime on credit beyond that cap is only possible on CPUs that would
otherwise be idle. io_uring workers never return to user space but run the
task work between work items, where they park like user threads.

A task gives its slots back when it

- blocks, exits, or moves to another cgroup;
- calls ``sched_yield()`` while tasks wait for its slots, after it held them
  for 0.5 ms (since it took them or since its last round began): a thread
  spinning on a lock with ``sched_yield()`` may wait for the lock holder,
  which waits for a slot. A thread which yields while it holds the lock, on
  the other hand, must not hand its slots to the threads woken for that lock,
  which would find it still taken; the 0.5 ms let it finish first. The slots
  are given back on the way back to user space, right after the system call.
  ``yield()`` called inside the kernel (e.g. by netlink with RTNL held) does
  not park the task there: it gives its slots back only when it returns to
  user space, as with a slice that ends in the kernel;
- reaches the end of its slice (``sched_base_slice``) while one of its levels
  has waiting tasks, has no budget left, or holds more slots than its limit
  allows (after the limit was lowered). If the slice ends while the task runs
  in user space it rotates at once, otherwise when it returns to user space.
  It then has to be admitted again, and goes to the tail of the queue if it
  cannot be;
- is a waiting task's slot hand-off source, see below.

A task which is preempted by another task of its CPU keeps its slots while it
waits on that CPU's run queue, and is pushed to an idle CPU if one is allowed;
see `Slot holding while preempted`_ for why.

Wakeup priority
---------------

Waiting tasks are of two kinds: tasks that woke up and found no free slot
(typically a thread serving a request), and tasks that gave their slot back
at the end of a slice (busy threads taking turns). Each level keeps the first
kind in a separate queue which is served first, but not more than four times
in a row while tasks of the second kind wait. While a woken task waits, a
task holding a slot of the same level since a rotation gives it back after
0.5 ms instead of at the end of its slice; a task holding its slot since its
own wakeup is not cut short. A woken task whose burst is cut short, e.g. by
the budget of a fractional limit, keeps its priority for two more rounds.
With a limit of 1 CPU, four request threads (1 ms of work every 20 ms) next
to two busy threads of the same group see a median latency of 3-5 ms instead
of 10 ms with plain rotation.

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
tick for the running task, with its lag and deadline rescaled. Hierarchical
weights are cached per group and recomputed at most once per millisecond
(there is no global generation counter, which every CPU would write on every
change of the active set). w_eff is clamped to ``[scale_load(MIN_SHARES),
scale_load(MAX_SHARES)]``.

Starvation floor: w_eff of a task of a non-root group is at least the load of
its CPU times ``slice / 1 s``, so a task of a group with a tiny share still
gets a slice about once a second. Without it, a thread of a group with
``cpu.weight`` 1 next to groups of weight 10000 would wait for many seconds
(as it does under the fair class). ``SCHED_IDLE`` tasks are exempt.

Weights inside a limited group
------------------------------

The slots of a limited group are themselves a contended resource. Waiting
tasks of a limited level are therefore not admitted in plain FIFO order: the
runtime of tasks holding slots is accounted to the child of each limited level
they belong to, scaled by that child's weight (*slot service*), and the next
task admitted at the level is the first waiting task of the child with the
least slot service, among the first 16 waiting tasks. Within one child the
order is FIFO. A child that comes back after a pause starts at most two slices
behind the last admitted one. Two children of a group limited to 3 CPUs with
``cpu.weight`` 100 and 300, each with 8 busy threads, get 0.77 and 2.27 CPUs.


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

As with ``RUN_TO_PARITY`` and ``PREEMPT_SHORT`` in the fair class, a running
task is not preempted by a waking task before its request is used up, unless
the waking task has a shorter slice; a waking task which doesn't preempt
shortens the protection (``update_protect_slice()``), and once it ends the
running task is picked again at the next update of its runtime. As in the
fair class, the hrtick (``HRTICK`` scheduler feature) ends a request on time
while other tasks are queued, instead of at the next tick.

A task which slept for less than the wakeup granularity (1 ms by default)
preempts only if the running task is also ahead of it by more than that much
virtual time, as with the fair class' wakeup granularity before EEVDF.
Without it, tasks which sleep for microseconds, such as the readers and
writers of a pipe, preempt each other on every wakeup and run in lockstep on
different CPUs: twice the context switches of the fair class, contention on
the run queue locks of their wakeups and on the pipe's mutex, and hackbench
with processes 25-40% slower. The fair class avoids most of these
preemptions through the state delayed dequeue leaves in its run queues.
Tasks which slept longer, such as request handlers, preempt as EEVDF decides.
The granularity applies to synchronous wakeups only (pipes, sockets), whose
waker is about to block. A thread which wakes several others with a futex and
keeps running would otherwise leave each wakee that doesn't preempt it queued
behind another task for a whole slice: with schbench (message threads waking
workers), 16 workers on 8 CPUs saw a median wakeup latency of 2 ms instead of
a few microseconds.

Instead of delayed dequeue (``DELAY_DEQUEUE``, ``DELAY_ZERO``), a task which
goes to sleep with negative lag remembers when and at which rate its CPU's
virtual time advanced, and when it wakes up its negative lag is reduced by
the virtual time that passed, up to zero lag. The fair class gets the same
by keeping such a task queued until it is eligible; without either, a short
sleeper next to busy tasks wakes up with its old debt and waits behind them.

Not carried over: next/short buddies, PELT per task and per
group, util_est, energy aware placement, misfit migration, NUMA balancing and
``cpu.idle`` (treated as a plain weight). For schedutil, PFS keeps one PELT
utilisation signal per CPU, like the RT class does, and ``cpu_util_cfs()``
returns it while PFS is active. PFS calls ``cpufreq_update_util()`` on every
tick, when the signal decays at a context switch or when the first task is
queued on an idle CPU, and with ``SCHED_CPUFREQ_IOWAIT`` when a task that
waited for I/O is queued, as the fair class does. The blocked signals of idle
CPUs (PFS, RT, deadline, IRQ) are decayed by the NOHZ idle balance, which
under PFS only does that and moves no tasks.


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
   the window (window id, usage including runtime on credit, hrtimer for the
   next window or the end of a pay-back), weight,
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
       pfs_group::weight_lock of the parent (active sums; never two at once)

- Slot counts are atomics, taken with increment-and-check and given back with
  a decrement. A task's slot mask, state and group are protected by its run
  queue lock.
- Lost wake-ups between a parking task and a releasing task are prevented as
  with wait queues: the parking side adds itself to the queue, then
  (``smp_mb()``) checks for a free slot and starts a retry if there is one;
  the releasing side gives the slot back, then (``smp_mb__after_atomic()``)
  checks the queue.
- To admit a waiting task found under ``pfs_group::lock``, the scheduler drops
  the group lock, takes the task's run queue lock (plus its own, with
  ``double_lock_balance()``), and checks that the task is still waiting in
  that queue before touching it. A waiting task cannot exit, and its
  ``task_struct`` stays valid in the RCU read-side section around this.
- The running task is parked only from ``put_prev_task()``, after which the
  run queue lock is held until ``finish_task_switch()`` has cleared
  ``p->on_cpu``. A remote CPU holding a run queue lock therefore never sees a
  waiting task that is still switching out. Hand-offs and retries still check
  ``task_on_cpu()`` and admit such a task in place instead of migrating it.
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
  tick when it interrupted user space, or by the task work on the way back to
  user space, which also request the reschedule). *prev* becomes
  ``PFS_PARKING``: no slots, not pickable, on no list.

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
balancer does not run; ``SCHED_SOFTIRQ`` runs the PFS balancer and the
blocked load updates of the NOHZ idle balance.


Load balancing
==============

PFS has its own balancer, with parts ported from the fair class; the fair
class' sched domain balancer is not used.

- Wake-up placement (``select_task_rq``): the previous CPU if idle; for a
  synchronous wakeup from a CPU running only the waker, or for any wakeup
  from a CPU running no more tasks than the previous CPU, the waking CPU;
  else an idle CPU close to that choice, LLC first; else the choice itself.
  The waking CPU matters for a thread which wakes others and then waits for
  them (a dispatcher and its workers): its wakees run there as soon as it
  blocks, instead of waiting behind a busy task on their previous CPU for
  the rest of its slice. The
  idle CPU scan is skipped while no CPU went idle in its last pick (a global
  count of such CPUs), which is what keeps an overloaded machine from paying
  for scans that find nothing (``SIS_UTIL`` in the fair class). Fork and exec
  take an idle CPU or the one with the fewest tasks. A port of
  ``wake_wide()``/``wake_affine()``/``select_idle_sibling()`` is available
  (feature bit 0) but measured slower here and is off. A task of a limited
  group whose levels have no free slot stays on its previous CPU (it would
  only park).
- Periodic balance (``SCHED_SOFTIRQ``, raised by the tick every 32 ms on a
  busy CPU, 4 ms on an idle one): compare the PFS load with the busiest CPU
  of the LLC (and, at a quarter of the rate, of the machine) and pull queued
  tasks which aren't cache hot while the imbalance exceeds 17%. As the loads
  are instantaneous rather than averaged, the busiest CPU must also have at
  least two more admitted tasks, or two tasks and a load more than twice as
  high (the same with equal weights): a slot holder of a limited group,
  which carries the group's weight, then gets a CPU of its own next to the
  light tasks of an unlimited neighbour.
- Newly idle pull (``pick_task`` with an empty tree): pull a queued, not
  running, allowed task from the busiest CPU (by queued count), searching the
  LLC first. As in the fair class, a CPU whose idle periods are usually
  shorter than ``sched_migration_cost`` (``rq->avg_idle``) doesn't pull, so
  that two tasks handing work back and forth don't get migrated all the
  time.
- Push (a balance callback after a context switch, at most once per tick per
  CPU, and right away for a preempted task of a limited group): move a
  queued task to an idle CPU; idle CPUs don't tick (NOHZ) and don't run the
  periodic balance.
- Idle CPUs that need to pull are kicked with ``resched_cpu()`` from an
  ``irq_work`` (the kicking side holds a run queue lock); they pull in their
  newly idle pick.

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
a task on the tree of an inactive CPU. The dying CPU has flushed its
``irq_work`` by then, so retries that moving the waiting tasks starts are
queued on an active CPU instead.

When a cgroup is removed, its group's window timer is cancelled and its retry
``irq_work`` synchronised from ``css_free``, which may sleep, before the
task group is freed after an RCU grace period.


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

Under ``/sys/kernel/debug/sched/pfs/``:

``stats``
   sums of per-CPU counters: ``admit`` (slots taken), ``park_full``,
   ``park_budget``, ``park_fifo`` (waits by reason), ``park_debt`` (slots
   refused while runtime beyond the limit is paid back), ``rotate`` (slots given
   back at the end of a slice), ``handoff`` (waiting task pulled to the CPU
   that freed the slot), ``handoff_local`` (it was already there), ``inplace``
   (admitted on its own CPU), ``retry``, ``pull``, ``push``, ``kick``,
   ``credit`` (admitted without slots while in the kernel), ``user_park``
   (parked or rotated on the way back to user space), ``slot_ns`` and
   ``credit_ns`` (runtime of tasks holding slots and of tasks of limited
   groups on credit); plus the number of limited groups and of waiting tasks.

``groups``
   limited groups and groups with waiting tasks: limit in milli-CPUs, slots
   taken, waiting tasks, CPU time used in the current window, weight,
   hierarchical weight, active count and path.

``waiters``
   the waiting tasks of each CPU with their state and the state of the level
   they wait at (root only).

``window_us``
   length of the budget window, 1000 to 1000000 (default 10000).

``features``
   behaviour switches for comparisons within one boot: 1 the ported fair
   wakeup placement (off), 2 periodic balance, 4 negative lag served while
   asleep, 8 slice protection updates, 16 periodic balance of unequal
   weights by load alone, 32 periodic balance of task counts, 64 slot
   holders keep their slots on a yield for 0.5 ms, 128 wakeups on the
   waking CPU if it is no busier than the previous one, 256 the wakeup
   granularity for synchronous wakeups only, 512 the hrtick ends requests
   (default 1022).

``wakeup_gran_us``
   wakeup granularity for short sleepers, see above; 0 turns it off
   (default 1000).


Testing
=======

Functional checks, run on every change of ``pfs.c``:

- Smoke test in an 8-CPU virtual machine booted with ``sched_pfs=1``: busy
  threads with and without a limit (``cpu.stat`` shows the limit held and
  ``nr_throttled`` 0), ``perf bench sched messaging`` with threads and with
  processes, a fork storm, renice and affinity changes of running threads,
  and CPUs taken offline and back online under load; ``dmesg`` must stay free
  of warnings. The same with ``sched_pfs=0``, and on a kernel built with
  ``CONFIG_SCHED_PFS=n``, which must build without warnings and behave like
  one without the series.
- Stress, 60 s, on a debug kernel with ``CONFIG_PROVE_LOCKING``,
  ``CONFIG_DEBUG_ATOMIC_SLEEP`` and ``CONFIG_PROVE_RCU`` (twice) and on one
  with ``CONFIG_KASAN``, ``CONFIG_DETECT_HUNG_TASK``,
  ``CONFIG_SOFTLOCKUP_DETECTOR``, ``CONFIG_DEBUG_LIST``,
  ``CONFIG_DEBUG_PREEMPT`` and ``CONFIG_DEBUG_OBJECTS``: hackbench in limited
  and unlimited groups, fork and exit loops in limited groups, a
  multi-threaded spinner moved between groups every 20 ms, ``cpu.max`` and
  ``cpu.weight`` rewritten every 2 ms (down to 0.01 CPU), renice and affinity
  changes, random CPUs taken offline and online, limits nested three levels
  deep, then a weight-1 thread next to two groups of weight 10000. Pass: no
  lockdep, ``WARNING``, ``BUG``, RCU stall, hung task or KASAN report, and the
  weight-1 thread never waits more than about a second (250-810 ms on the
  debug kernels).
- A hierarchy test (nested limits and weights, usage per group from
  ``cpu.stat``) and fractional limits (0.5 and 1.5 CPUs with 8 threads,
  traced).

Comparisons with CFS bandwidth control use ``tools/sched_ext/pfs-test``: on
a ``sched_pfs=1`` boot its ``cfs`` variant measures PFS, so the same scripts
run on plain cgroups, systemd units, Docker and Podman. Comparisons within
one boot can switch individual changes with the ``features`` bits.


Measurements
============

8-CPU virtual machine
---------------------

8 vCPUs on an 8-CPU host, non-debug kernel (``CONFIG_HZ=1000``), the same
kernel image booted alternately with and without ``sched_pfs=1``, one variant
per boot, host load below 3 before each boot. Limited tests: median of 5
boots; tests without limits: median of 9. Groups are two-level (a pod and a
container, both with the limit), ratios are PFS / CFS bandwidth control.

- The group stays at its limit, busy all the time, where CFS bandwidth
  control runs it on 6.5 CPUs at once on average and then throttles it
  (limit 1, 8 threads): 1.00 CPUs at once, ``nr_throttled`` 0.
- Throughput of busy threads 1.21-1.27 times that of CFS bandwidth control
  without a neighbour (limit 2 with 8 and 64 threads, limit 4 with 64
  threads; 3.94 of 4 CPUs used), 0.98 next to an unlimited neighbour with
  one busy thread per CPU.
- Request latency (4 threads doing 1 ms of work every 20 ms) next to 8 busy
  threads of the same group, limit 1: p50/p99 3.0/5.0 ms vs 349/1036 ms. At
  limit 0.5 with 2 busy threads, through the test kit on plain cgroups,
  systemd and Docker: p99 13-27 ms vs 82-84 ms. (This item and the previous
  one were measured on an earlier commit of this series, in a run that
  also compared the width mode of CFS bandwidth control and scx_pfs, see
  ``tools/sched_ext/pfs-test/README.md``.)
- 16 threads which call ``sched_yield()`` 100 times while holding one of 4
  mutexes: 5.1 and 6.3 times the rate of CFS at limits 2 and 4. A model of a
  parallel garbage collector at limit 2: pause p99 8.5 vs 50 ms. A thread of
  weight 1 next to 16 busy threads of weight 5000: worst wait 493 ms vs 9.7 s.
  Two groups with weights 1:3 and limits 4 and 6 on 8 CPUs: 2.88 (CFS 2.94).
- Tasks without limits: schbench (2 message threads, 4 workers) wakeup p99
  0.27 and request rate 1.14 times CFS; with 8 workers wakeup p99 1.02;
  hackbench with processes 0.78-0.99, with threads 0.61-1.15 (host noise in
  the 16-group cell); pipe ping-pong 1.01; spin-then-futex barrier 1.08;
  request latency next to 8 busy threads p99 0.67.
- PostgreSQL 16 (pgbench TPC-B and select-only, 32 clients over TCP from
  outside the group, scale 20, data in memory) in a group limited to 2 and
  1.5 CPUs: 1.95-2.00 and 1.50 CPUs, as CFS bandwidth control (1.98-2.00 and
  1.49-1.50).

16-CPU virtual machine with Kubernetes pods
-------------------------------------------

16 vCPUs, Kubernetes, limited pods with requests equal to limits, clients in
separate pods on the same machine, one workload at a time, 3 runs per cell
unless noted, kernel 7.3 with and without ``sched_pfs=1``; the reference is
a 4-vCPU virtual machine without limit. These numbers come from a build
before the change that pays back runtime on credit (see `Admission and
waiting`_): its database pods used 2-13% more than their limit (4.18-4.19
CPUs at limit 4, 2.05-2.08 at 2, 1.64-1.70 at 1.5). The fix brings the
average to the limit and costs about the extra CPU time in throughput
(expected about -5% at limit 4 and -11% at limit 1.5). A measurement of the
fixed build is in progress.

- PostgreSQL pgbench TPC-B, 32 clients, limit 4: 7319 transactions/s with
  p99 9.0 ms (CFS bandwidth control 9119 with 35.7 ms, the 4-vCPU reference
  6673 with 14.1 ms); select-only 52.7k with 1.56 ms (63.8k with 6.45 ms).
  Limit 2: TPC-B 4356 with 14.0 ms (4673 with 79.9 ms).
- MariaDB sysbench read-write, 32 threads, limit 4: 1884 transactions/s with
  p99 26.7 ms (2297 with 45.8 ms).
- JVM with ParallelGC at a fixed request rate, limit 4: worst GC pause
  23.3 ms (24.2 ms), full GC 21.6 ms (22.5 ms).
- Redis with 4 I/O threads, limit 4: 210k SET/s with p99 0.62 ms (the same
  210k with 0.60 ms).
- Micro-benchmarks in a pod limited to 4 CPUs: hackbench with processes
  2.12 s (2.87 s), sysbench cpu p99 9.2 ms (64 ms), 16 threads holding a
  mutex across 100 ``sched_yield()`` calls 46.1k events/s (12.7k).
- 100 small pods (limits 0.25-1, busy, 30% busy and idle): all Ready after
  31 s (45 s on kernel 7.0 with CFS).
- A pod killed and recreated in a loop (26 times 10 pods with spinners and
  300 fork/exec each) during a PostgreSQL run: no failed transaction and no
  kernel warning.


Disadvantages and when CFS is better
====================================

- **Bursts.** CFS bandwidth control lets a limited group run on every CPU
  for part of a period; PFS never runs more than the limit at once. A short
  parallel burst therefore takes longer, and requests in an otherwise idle
  limited group wait for a slot: four request threads at limits 1 and 2
  have a median latency 1.8-2.7 times that of CFS (3.11 vs 1.15 ms at
  limit 1). Groups that are idle most of the time and need many CPUs for a
  few milliseconds are better served by CFS bandwidth control. A burst
  width (more slots for a short time, paid back from the window) would
  address this and is not implemented.
- **Spinning above the limit.** Threads which busy-poll for each other
  collapse when there are more of them than slots: every hand-off waits for
  a rotation. Redis with 4 I/O threads at limit 2 serves 5.35k SET/s vs
  85.8k under CFS bandwidth control; 4 threads passing work through spinning
  at limit 2 make 167 vs 10770 rounds/s; 8 pairs of threads passing a flag
  at limit 2 run at 0.18 of CFS, with some runs collapsing to a hand-off per
  rotation (the rate varies from 12.6k to 3.3M per second between boots).
  Keep busy-polling threads at or below the limit.
- **Database throughput.** At the same limit, databases get fewer
  transactions than under CFS bandwidth control, which runs them on all 16
  CPUs for part of each period: PostgreSQL -20% (TPC-B) and -17%
  (select-only) at limit 4, -7% at limit 2, MariaDB -18 to -21% at limit 4,
  and -19% beside an unlimited CPU hog. Their p99 latencies are 1.7-6
  times lower.
- **Unlimited neighbours.** A pod without a limit next to busy limited pods
  has a worse wakeup tail than under CFS: schbench wakeup p99 2.8 ms vs
  0.9 ms, 17% fewer requests/s (16-vCPU machine). A 1 ms sleeper outside the
  pods overshoots by 2.7-2.9 ms at p99 with 50-100 pods vs 1.0-2.4 ms.
- **Web tails at fractional limits with heavy neighbours.** A web server
  (500 us of CPU per request) at a fixed rate, next to two unlimited
  16-thread spinners and 20 limited pods: p99 145 ms at limit 0.25 (CFS
  bandwidth control on the same kernel 48 ms), 91 ms at limit 0.5 (27 ms),
  242 ms at limit 1 with 256 threads (37 ms) and 139 ms at limit 2 with 64
  threads (30 ms).
- **Throttling statistics.** ``nr_throttled`` and ``throttled_usec`` in
  ``cpu.stat`` are always 0, so dashboards and alerts built on them see
  nothing; waiting is in ``park_*`` of ``stats`` and in ``groups``.
- **Short windows.** Tasks that wake up without a free slot finish their
  system call on credit, so the number of threads of a group on CPUs can
  exceed the limit for a moment, and windows shorter than about 100 ms can
  show the limit plus a few percent. Runs of seconds stay at the limit.
- **cpu.max.burst** is not honoured.
- Overcommitted limits (2.6 times the machine) give groups with a small
  weight more than their weighted share: 1.35 instead of 1.22 CPUs.
- Kernel threads in limited groups run on credit until they block, and a
  switch between PFS and the fair class needs a reboot.


Limits of this implementation
=============================

- The balancer scans CPU masks linearly; it is meant for machines up to a few
  hundred CPUs, not for large NUMA systems. The newly idle pull scans its LLC
  and looks at the rest of the machine at most every 4 ms, and fork and exec
  look for the least loaded CPU within the forking CPU's LLC, but wakeups and
  pushes still scan the whole machine for an idle CPU while the global count
  of idle CPUs is non-zero; that count is one atomic written on every idle
  entry and exit of a CPU. Using the sched domain hierarchy and per-LLC idle
  hints, as the fair class does, would need its own balancer design.
- Active-set changes of a group (0 <-> 1 transitions of its admitted count)
  take the parent's weight lock, so sibling groups share a lock and a cache
  line: all pods of a Kubernetes QoS class have the same parent.
- The budget slot is accounted per tick; a group can overrun its budget by up
  to a tick per running thread per window, which is carried over (as debt of
  up to one second's worth of the limit, so that limits much smaller than a
  tick per window are still met on average).
- Tasks running in the kernel on credit run beyond the slots for as long as
  they stay in the kernel, so more than L threads of a group can be on a CPU
  at once, as under CFS bandwidth control. Their runtime is paid back
  afterwards, which keeps the average at the limit; once the group is over
  its limit they only run with the weight of ``SCHED_IDLE`` tasks, and debt
  beyond one second's worth of the limit is not carried over, so a group
  can exceed its limit by using CPUs that are idle otherwise.
- Effective weights of queued, not running tasks are refreshed only when they
  start running or are admitted again.
- Within a limited group, slots are shared by weight between its children, but
  among the threads of one child in FIFO order, i.e. per thread, not by nice
  value.
- CPU utilisation for schedutil is a per-CPU signal; there is no per-task
  utilisation, so util_est, uclamp of individual tasks on frequency selection
  and energy aware placement don't apply to PFS tasks.
- NUMA balancing is not run for PFS tasks.
- Core scheduling (``CONFIG_SCHED_CORE``) orders PFS tasks of SMT siblings
  by their virtual runtimes relative to each CPU's average, without the
  snapshot the fair class takes at forced idle, so the fairness between
  cookies across siblings is approximate. Tasks waiting for a slot are
  never picked through the core tree.
- With ``nohz_full``, a CPU running a single task of a limited group keeps its
  tick so that the group's slots can rotate.
- Group hierarchies deeper than 63 levels are only limited on their top 63
  levels.

Possible future work:

- A burst width: let a group exceed its slots for a short time, paid back
  from its window like runtime on credit, for groups that are mostly idle
  and need many CPUs for a few milliseconds.
- Give the slot of a holder that stays preempted for more than a slice to a
  waiting task, which would shorten the stalls next to busy neighbours.
- Per-task utilisation signals, for util_est, uclamp and energy aware
  placement.
