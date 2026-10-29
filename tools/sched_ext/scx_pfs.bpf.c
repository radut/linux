// SPDX-License-Identifier: GPL-2.0
/*
 * scx_pfs, the Parallel Fair Scheduler: a demo sched_ext scheduler which
 * enforces cgroup CPU limits as a cap on concurrency instead of a time quota.
 *
 * With CFS bandwidth control, cpu.max "quota period" gives a cgroup a pool of
 * CPU time which is refilled every period. All threads of the cgroup draw from
 * the pool and once it is empty, the whole cgroup is throttled until the next
 * period. A cgroup limited to 4 CPUs with 32 runnable threads can run on 32
 * CPUs for 1/8 of the period and then stall for the remaining 7/8, which shows
 * up as tail latency even though the average usage is well below the limit.
 *
 * scx_pfs reads the same cpu.max values through ops.cgroup_init() and
 * ops.cgroup_set_bandwidth() and interprets quota / period as a width: a cgroup
 * limited to N CPUs may have at most N of its tasks admitted to run at any
 * time. There is no time budget for whole CPUs, so a cgroup which keeps its
 * concurrency within the limit is never throttled. Tasks beyond the width wait
 * in a per-cgroup FIFO DSQ and are admitted as running tasks of the cgroup give
 * their slot back. A task whose slice expires gives its slots back in
 * ops.dispatch(), so that the next waiting task runs on the same CPU at once.
 *
 * Waiting tasks are of two kinds, kept in two DSQs per cgroup: tasks which
 * woke up and found no free slot, and tasks which gave their slots back at the
 * end of a slice (rotated). Woken tasks are admitted first, but at most
 * WOKEN_STREAK_MAX in a row while rotated tasks wait: they are typically
 * request threads, lock holders, or the thread others spin or sleep for
 * (Redis' main thread, a JVM's VM or GC threads), while rotated tasks are CPU
 * bound and only take turns. While a woken task waits, a task holding its
 * slots since a rotation gives them back after cut_ns (-c, 0.5ms) instead of at
 * the end of its slice, and with -r while any task waits after rot_ns; a task
 * holding its slots since its own wakeup is not cut short. ops.tick()
 * checks this, so the cuts are rounded up to a tick.
 *
 * A task can hold kernel locks wherever it is enqueued, and one which waits
 * for a slot with a lock held stalls every task waiting for that lock, in any
 * cgroup. Tasks which woke up are in the kernel and may have been woken as the
 * new owner of a lock, so one which finds no free slot runs without one, on
 * credit, for up to credit_ns (-k, 1ms) if every limited cgroup on its path
 * has tokens left, see
 * credit_ok(). Credit thus comes out of the tokens the cgroup saved up while
 * it ran below its limit, e.g. while its threads slept, and lets the
 * concurrency exceed the width meanwhile but not the usage the limit. A
 * cgroup which keeps its slots busy saves no tokens and gives no credit once
 * they are spent; its woken tasks wait for a slot, are admitted before the
 * rotated ones and cut a rotated holder's turn after cut_ns. Credit beyond
 * the tokens, e.g. a budget per window, would bound the wait of a woken lock
 * owner further, but threads which take turns on a contended lock then spend
 * the cgroup's limit on futex wakeups that only block again, which cost them
 * more than 80% of their throughput, and the limit is then only met by
 * refusing slots for the debt. A task whose credit ran out waits with the
 * woken tasks. io_uring workers, which never return to user space, are treated
 * as woken. Exiting tasks, kernel threads and tasks with SIGKILL pending never
 * wait, as they are about to give up what they hold. Other tasks wait for a
 * slot when their slice ends, when they are preempted and when they yield,
 * also if that happens in the kernel with a sleeping lock held: sched_ext
 * doesn't tell whether a task is returning to user space when it is enqueued.
 *
 * sched_yield() doesn't give the slots away, unless the task has held them for
 * cut_ns and others wait for them: a thread which yields with a lock held
 * would otherwise wait a whole turn per yield, while the threads woken for the
 * lock take its slots only to block again.
 *
 * The window timer also sweeps the cgroups which have waiting tasks and retries any that
 * no retry walked for RESCUE_NS, which should never be needed and is counted.
 * Slots are given back through the cgroup IDs of the task's path remembered
 * when it joined its cgroup, so that the slots an exiting task holds on the
 * ancestors of its already removed cgroup aren't leaked.
 *
 * A fractional limit such as 1.5 is enforced as floor(1.5) == 1 slot without
 * any time budget plus one extra slot which is rate limited by a token bucket
 * per cgroup, see refill(). Tokens accrue at the limit and all runtime of the
 * cgroup's tasks is taken off; the extra slot can only be taken while there
 * are tokens. The bucket holds what the fractional part of the limit allows
 * in one cpu.max period (-b, 100% by default), so a cgroup limited to 0.1 CPU
 * with a period of 100ms can run a burst of 10ms at once after it ran below
 * its limit, as it can under CFS, while its long-run average stays at the
 * limit. Limits below one CPU only have the rate limited slot. Once a cgroup
 * used up its tokens, e.g. a task spinning at its limit, its extra slot is
 * retried every window (-w, 10ms by default) and gets what accrued meanwhile,
 * limit * window, rather than a period's quota after a long throttled stretch.
 *
 * Limits are hierarchical: a task needs a free slot in every limited cgroup
 * from its own up to the root. If a level is full, the task waits in that
 * level's DSQ, so tasks of sibling cgroups which compete for a parent's slots
 * queue together.
 *
 * Admitted tasks are scheduled from a single shared DSQ in vtime order as in
 * scx_simple, but the vtime a task is charged for its runtime is scaled so
 * that under contention CPU time is split between cgroups by cpu.weight, and
 * only within a cgroup between its tasks. The hierarchy is flattened as in
 * scx_flatcg: a cgroup is active while it, or a descendant, has runnable tasks,
 * and its hweight is the product of weight / sum of the active siblings'
 * weights along its path, e.g. with kubepods (100) and system.slice (100) both
 * active, pods of 1 and 3 CPUs requested (39 and 118) under kubepods get 1/8
 * and 3/8 of the machine. A task of a cgroup with runnable tasks of total
 * weight R is entitled to hweight * weight / R of the machine, so its runtime
 * is charged as runtime * R / (weight * hweight). All runnable tasks advance
 * their vtime at the same rate when every task gets its entitlement, so the
 * tasks of a cgroup get hweight of the machine between them however many
 * there are.
 *
 * R counts every runnable task of the cgroup, including the ones which wait
 * for a slot: they take turns on the slots in FIFO order, so each of them gets
 * its share over time and the slots are a ceiling on top of the weights. A
 * cgroup which can't use its share because of its limit or its number of
 * tasks leaves the rest to the others, the shares are only applied by the
 * order of the shared DSQ. Idle CPUs are still taken at once by
 * ops.select_cpu(), and tasks with restricted affinity, ENQ_LAST and the
 * handoff in ops.dispatch() work as without weights. A task whose slice
 * expires keeps its CPU if it would be picked next anyway, see keep_prev().
 * Tasks of the root cgroup and of too deep cgroups are charged their plain
 * runtime, which favours them as in scx_flatcg. A task's weight is sampled
 * when it becomes runnable.
 *
 * As the shares are in the vtime of individual tasks, they are only as precise
 * as the per-task vtime: a task which waited for more than a slice of runtime
 * is clamped as in scx_simple, with the slice scaled like its runtime, and
 * loses the rest of its credit. Shares are floored so that a task of a
 * cgroup with a tiny share still gets a slice about every WAIT_MAX_NS, see
 * scale_by_cgrp_share(). An admitted task holds its slots while it
 * waits on the shared DSQ, so a cgroup whose share is as large as its limit
 * runs below the limit whenever one of its admitted tasks waits there. The
 * hweights are cached and recomputed when the set of active cgroups or a
 * weight changes. cpu.max.burst is ignored.
 *
 * All runtime of a cgroup's tasks, with or without slots, is taken off the
 * token buckets of the limited cgroups on their path: on every tick, when a
 * task stops, and for a task which blocked, until ops.dispatch() is done with
 * its CPU. Accounting is still approximate: the slice of a task admitted to
 * the rate limited slot or on credit is clipped to the tokens left at
 * admission, but other running tasks of the cgroup consume the same tokens
 * concurrently, and the exiting tasks, kernel threads and killed tasks above
 * run anyway. Usage beyond the tokens leaves the bucket negative, a debt of
 * up to what the limit allows in DEBT_MAX_NS. While a cgroup's debt is above
 * a margin of a window's budget and a tick per slot, see over_limit(), its
 * last full slot and its rate limited slot aren't handed out and it gives no
 * credit, so that it pays the debt back and its average usage stays at the
 * limit. The bucket is refilled lazily by its users; refills and charges race
 * only in the clamping to the bucket's depth and debt cap.
 *
 * A task which is dequeued and enqueued again while it is running, e.g. on
 * sched_setaffinity(), a nice change or a cgroup move, gives its slots back at
 * that point and can keep running for up to the rest of its slice without
 * holding a slot.
 *
 * Only cgroups up to MAX_LEVELS - 1 deep are tracked. Tasks of deeper cgroups
 * are not limited at all, which is counted in PFS_STAT_TOO_DEEP.
 */
#include <scx/common.bpf.h>
#include "scx_pfs.h"

char _license[] SEC("license") = "GPL";

#ifndef RUNTIME_INF
#define RUNTIME_INF	((u64)~0ULL)
#endif

enum {
	/*
	 * Admitted tasks are queued on SHARED_DSQ. Waiting tasks are queued on
	 * a DSQ per cgroup whose ID is the cgroup ID, which is never 0.
	 */
	SHARED_DSQ		= 0,
	/*
	 * Levels 1 to MAX_LEVELS - 1 have a bit in the low half of a slot
	 * mask. The high half holds a sequence number, see publish_slots().
	 */
	MAX_LEVELS		= 32,
	RETRY_BATCH		= 8,
	RETRY_WALK_MAX		= 32,	/* tasks examined per retry_cgroup() */
	TIMER_BATCH		= 64,
	FRAC_ONE		= 1000,	/* limits are kept in milli-CPUs */
	WOKEN_STREAK_MAX	= 4,	/* woken admissions in a row, see retry */
};

/*
 * Each cgroup has two wait DSQs: rotated tasks wait in the DSQ with the
 * cgroup's ID, tasks which woke up in the one with WOKEN_DSQ set as well.
 * Cgroup IDs don't come close to bit 62.
 */
#define WOKEN_DSQ		(1LLU << 62)

#define SLOT_LEVELS_MASK	0xffffffffLLU
#define SLOT_SEQ_SHIFT		32
#define MIN_BUDGET_NS		(100 * 1000LLU)
/* runtime beyond the limit is carried over up to what it allows in this time */
#define DEBT_MAX_NS		(1000 * 1000 * 1000LLU)
/* a bucket not refilled for this long is full, see refill() */
#define REFILL_MAX_NS		(16 * DEBT_MAX_NS)
/* a tick at HZ=250: how late a slot holder's slice can end, see over_limit() */
#define TICK_SLACK_NS		(4 * 1000 * 1000LLU)
#define PFS_SIGKILL		9
#define HWEIGHT_REFRESH_NS	(1000 * 1000LLU)
/* a waiting cgroup not walked by any retry for this long is retried */
#define RESCUE_NS		(50 * 1000 * 1000LLU)
/* bounds the retries which wait for a park to land, see retry_cgroup() */
#define PARK_LAND_NS		(10 * 1000 * 1000LLU)

_Static_assert(MAX_LEVELS <= SLOT_SEQ_SHIFT, "level bits overlap the sequence");
_Static_assert((MAX_LEVELS & (MAX_LEVELS - 1)) == 0, "MAX_LEVELS must be a power of two");

/*
 * The level to index a task's cgroup path with. Every caller is inside
 * bpf_for(level, .., MAX_LEVELS), which bounds the level, but a verifier
 * which doesn't carry that bound into the register the access itself uses
 * rejects the array with "R1 unbounded memory access" (v6.19 and earlier,
 * tested back to v6.12). Mask the index so that the bound is on the register
 * being indexed; MAX_LEVELS is a power of two, so this is one AND. The
 * barrier keeps clang from dropping the mask, which is redundant for the
 * compiler, since it knows the loop bound, but not for those verifiers.
 */
static __always_inline int path_level(int level)
{
	barrier_var(level);
	return level & (MAX_LEVELS - 1);
}

enum reserve_result {
	RESERVE_OK,
	RESERVE_FULL,		/* all slots of a level are taken */
	RESERVE_BUDGET,		/* a level's rate limited slot is out of tokens */
};

const volatile u64 slice_ns;
const volatile u64 window_ns;
/* bucket depth in percent of the fractional quota per period, see refill() */
const volatile u64 burst_pct = 100;
/* a rotated holder gives its slots to a woken waiter after this, see tick */
const volatile u64 cut_ns = 500 * 1000;
/* and to any waiting task after this, 0 (default) for the end of the slice */
const volatile u64 rot_ns;
const volatile u64 min_share = 1;	/* see scale_by_cgrp_share() */
/* the longest a task runs on credit before it waits for a slot */
const volatile u64 credit_ns = 1000 * 1000;

static u64 vtime_now;

/* number of cgroups with a limit, scheduling is plain vtime while it's 0 */
static u64 nr_limited;

/* entries in budget_q, bounds the timer's pass to the ones queued before it */
static u64 nr_budget_q;

/* cgroups marked lost since the last rescue_lost(), see mark_lost() */
static u64 nr_lost;

/* the root cgroup, where rescue_lost() starts */
static u64 root_cgid;

/* bumped when the active cgroups or a weight change, expires the hweights */
static u64 hweight_gen = 1;

/* protects ->nr_active and ->child_weight_sum of all cgroups */
private(PFS_WEIGHT) struct bpf_spin_lock weight_lock;

UEI_DEFINE(uei);

struct pfs_cgrp_ctx {
	/* first, user space reads it, see scx_pfs.h */
	struct pfs_cgrp_stat stat;
	u64		limit;		/* milli-CPUs from cpu.max, 0 if none */
	u32		nr_slots;	/* slots taken by tasks of the subtree */
	u32		retry_queued;
	u32		retry_running;	/* a retry_cgroup() walks the DSQ */
	u32		retry_walked;	/* tasks examined by the walker */
	u32		budget_queued;
	u32		woken_streak;	/* woken tasks admitted in a row */
	u32		in_waitset;	/* listed in waitset */
	u32		no_dsq;		/* wait DSQs not created, never limited */
	u32		lost;		/* a queue or waitset was full, see mark_lost() */
	s32		nr_parking;	/* parks not yet seen by a walk, see park() */
	u64		park_at;	/* when a task last parked here */
	u64		retry_at;	/* when a retry last walked the DSQs */
	/* token bucket of the limit, see refill() */
	s64		tokens;		/* runtime allowed now, a debt if < 0 */
	u64		refill_at;	/* when tokens were last refilled */
	u64		depth;		/* the most tokens saved up */

	/* cpu.weight, see the top of the file */
	u32		weight;
	s32		nr_active;	/* active children, 1 for own tasks */
	u64		child_weight_sum; /* weights of the active children */
	u64		hweight;	/* share of the machine, of HWEIGHT_ONE */
	u64		hweight_gen;
	u64		hweight_at;	/* when hweight was last refreshed */
	u32		nr_runnable;	/* runnable tasks of the cgroup itself */
	u64		runnable_weight; /* and their weights */
};

struct {
	__uint(type, BPF_MAP_TYPE_CGRP_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct pfs_cgrp_ctx);
} cgrp_ctx SEC(".maps");

struct pfs_task_ctx {
	u64		cgid;		/* cgroup the task belongs to */
	u64		slot_mask;	/* levels of cgid's path holding a slot */
	u64		slot_seq;
	u64		started_at;
	u64		run_cgid;	/* cgroup counting @p as runnable */
	u64		run_weight;	/* weight @p is counted with */
	u64		admitted_at;	/* when the slots were taken */
	u32		rotated;	/* admitted after a rotation, not a wakeup */
	u32		credit;		/* runs on credit since a wakeup */
	u32		yielded;	/* gave its slots up in ops.yield(), see pfs_yield() */
	u64		parking_cgid;	/* parked there, not yet seen by a walk */
	u64		wait_mask;	/* levels counted as waiting, see wait_begin() */
	/*
	 * IDs of the cgroups on cgid's path by level, set with ->cgid. Slots
	 * are given back through them: the cgroup of an exiting task can be
	 * removed before the task's last switch, its ancestors are still there.
	 */
	u64		path_cgid[MAX_LEVELS];
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct pfs_task_ctx);
} task_ctx SEC(".maps");

/*
 * The cgroup queues and the waitset hold each cgroup at most once. User space
 * sizes them (-q, NR_CGRP_Q by default). A cgroup which doesn't fit is marked
 * lost and found again by the timer, see rescue_lost().
 */
struct {
	__uint(type, BPF_MAP_TYPE_QUEUE);
	__uint(max_entries, NR_CGRP_Q);
	__type(value, u64);
} retry_q SEC(".maps");	/* cgroups whose waiting tasks may be admittable */

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__uint(max_entries, NR_CGRP_Q);
	__type(key, u64);
	__type(value, u8);
} waitset SEC(".maps");	/* cgroups which had waiting tasks, see rescue_cb() */

struct {
	__uint(type, BPF_MAP_TYPE_QUEUE);
	__uint(max_entries, NR_CGRP_Q);
	__type(value, u64);
} budget_q SEC(".maps");	/* cgroups waiting for tokens, see the timer */

struct pfs_timer {
	struct bpf_timer	timer;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct pfs_timer);
} window_timer SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__type(key, u32);
	__type(value, u64);
	__uint(max_entries, PFS_NR_STATS);
} stats SEC(".maps");

static void stat_inc(enum pfs_stat_idx idx)
{
	u32 idx_v = idx;
	u64 *cnt_p = bpf_map_lookup_elem(&stats, &idx_v);

	if (cnt_p)
		(*cnt_p)++;
}

static u64 div_round_up(u64 dividend, u64 divisor)
{
	return (dividend + divisor - 1) / divisor;
}

static u64 bw_to_limit(u64 period_us, u64 quota_us)
{
	if (quota_us == RUNTIME_INF || !period_us)
		return 0;
	return div_round_up(quota_us * FRAC_ONE, period_us);
}

static struct pfs_task_ctx *find_task_ctx(struct task_struct *p)
{
	struct pfs_task_ctx *taskc;

	taskc = bpf_task_storage_get(&task_ctx, p, 0, 0);
	if (!taskc)
		scx_bpf_error("task_ctx lookup failed for %s[%d]", p->comm, p->pid);
	return taskc;
}

static struct pfs_cgrp_ctx *find_cgrp_ctx(struct cgroup *cgrp)
{
	struct pfs_cgrp_ctx *cgc;

	cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
	if (!cgc)
		scx_bpf_error("cgrp_ctx lookup failed for cgid %llu", cgrp->kn->id);
	return cgc;
}

/*
 * Look up the context of @cgrp's ancestor at @level. The ancestors are pinned
 * by @cgrp, so the context stays valid after the ancestor is released.
 */
static struct pfs_cgrp_ctx *find_ancestor_ctx(struct cgroup *cgrp, int level,
					      u64 *cgidp)
{
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *anc;

	anc = bpf_cgroup_ancestor(cgrp, level);
	if (!anc) {
		scx_bpf_error("ancestor cgroup lookup failed");
		return NULL;
	}

	*cgidp = anc->kn->id;
	cgc = find_cgrp_ctx(anc);
	bpf_cgroup_release(anc);
	return cgc;
}

/* tasks waiting for slots in @cgid's DSQs, rotated and woken */
static u32 nr_waiting(u64 cgid)
{
	s32 rotated = scx_bpf_dsq_nr_queued(cgid);
	s32 woken = scx_bpf_dsq_nr_queued(cgid | WOKEN_DSQ);

	return (rotated > 0 ? rotated : 0) + (woken > 0 ? woken : 0);
}

/*
 * A retry or budget wait of the cgroup of @cgc couldn't be queued, or the
 * cgroup couldn't be listed in waitset: more cgroups than the maps hold, e.g.
 * in a delegated subtree. Its waiting tasks would stall until the sched_ext
 * watchdog ejects the scheduler. Mark the cgroup for rescue_lost() instead,
 * which the timer runs within a window.
 */
static void mark_lost(struct pfs_cgrp_ctx *cgc)
{
	stat_inc(PFS_STAT_RETRY_OVERFLOW);
	cgc->lost = 1;
	__sync_fetch_and_add(&nr_lost, 1);
}

static bool queue_retry(struct pfs_cgrp_ctx *cgc, u64 cgid)
{
	/* paired with the clearing in retry_cgroup() */
	if (__sync_val_compare_and_swap(&cgc->retry_queued, 0, 1))
		return false;

	if (bpf_map_push_elem(&retry_q, &cgid, 0)) {
		cgc->retry_queued = 0;
		mark_lost(cgc);
		return false;
	}
	return true;
}

static void kick_idle_cpu(struct task_struct *p)
{
	s32 cpu;

	cpu = scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);
	if (cpu >= 0)
		scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
}

static void kick_idle_cpu_any(void)
{
	const struct cpumask *idle;
	s32 cpu;

	idle = scx_bpf_get_idle_cpumask();
	cpu = scx_bpf_pick_idle_cpu(idle, 0);
	scx_bpf_put_idle_cpumask(idle);
	if (cpu >= 0)
		scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
}

/* list @cgid in waitset for the timer's sweep, see rescue_cb() */
static void waitset_add(struct pfs_cgrp_ctx *cgc, u64 cgid)
{
	u8 one = 1;

	if (cgc->in_waitset ||
	    __sync_val_compare_and_swap(&cgc->in_waitset, 0, 1))
		return;
	if (bpf_map_update_elem(&waitset, &cgid, &one, BPF_ANY)) {
		cgc->in_waitset = 0;
		mark_lost(cgc);
	}
}

static void queue_budget(struct pfs_cgrp_ctx *cgc, u64 cgid)
{
	if (__sync_val_compare_and_swap(&cgc->budget_queued, 0, 1))
		return;

	if (!bpf_map_push_elem(&budget_q, &cgid, 0)) {
		__sync_fetch_and_add(&nr_budget_q, 1);
	} else {
		/* a retry queues it again while the budget is out */
		cgc->budget_queued = 0;
		mark_lost(cgc);
	}
}

/* tasks of cgroups deeper than the slot mask can describe aren't limited */
static bool too_deep(struct cgroup *cgrp)
{
	return cgrp->level >= MAX_LEVELS;
}

/*
 * Clock for the windows and for the runtime charged against them. Window IDs
 * are compared across CPUs and against the CLOCK_MONOTONIC timer, which
 * scx_bpf_now() doesn't allow: it is only monotonic per CPU. With an unstable
 * sched_clock, e.g. after the TSC was marked unstable in a VM, it lags behind
 * on CPUs coming out of NOHZ idle and stays frozen in the timer callback,
 * which kept the timer in an old window and charged runtime that was never
 * used.
 */
static u64 window_now(void)
{
	return bpf_ktime_get_ns();
}

/* the most runtime beyond @cgc's limit that is carried over, see refill() */
static s64 debt_max(struct pfs_cgrp_ctx *cgc)
{
	u64 budget = cgc->limit * window_ns / FRAC_ONE;
	u64 max = cgc->limit * DEBT_MAX_NS / FRAC_ONE;

	return max > 2 * budget ? max : 2 * budget;
}

/*
 * The most tokens a cgroup with @limit and a cpu.max period of @period_ns
 * saves up: what the fractional part of the limit allows in burst_pct percent
 * of the period, so that a burst which CFS runs at once from the quota of a
 * period runs at once too, but at least one window's budget of the whole
 * limit, which is the credit an integer limit can give, see credit_ok().
 */
static u64 bucket_depth(u64 limit, u64 period_ns)
{
	u64 budget = limit * window_ns / FRAC_ONE;
	u64 depth = (limit % FRAC_ONE) * period_ns / FRAC_ONE * burst_pct / 100;

	return depth > budget ? depth : budget;
}

/*
 * Refill @cgc's token bucket for the time since its last refill and return
 * its tokens, the runtime its limit allows the subtree right now. Tokens
 * accrue continuously at the limit, limit / FRAC_ONE ns per ns, up to the
 * depth of the bucket, see bucket_depth(), and all runtime of the subtree,
 * with or without slots, is taken off, see charge_runtime(). Usage beyond
 * the tokens (slices end on a tick, tasks run on credit) leaves the bucket
 * negative: a debt that is paid back by running below the limit, so that the
 * average stays at the limit, see over_limit(). The debt is capped at what
 * the limit allows in DEBT_MAX_NS, which bounds the wait after an overrun.
 *
 * Callers sample @now before they get here, so another CPU may have refilled
 * up to a later time already; refill_at never goes back. The clamps to the
 * depth and the debt cap can race against concurrent charges, which loses at
 * most a few slices worth of charge.
 */
static s64 refill(struct pfs_cgrp_ctx *cgc, u64 now)
{
	u64 at = cgc->refill_at, dt;
	s64 tokens, depth, add, max;

	if ((s64)(now - at) <= 0 ||
	    __sync_val_compare_and_swap(&cgc->refill_at, at, now) != at)
		return cgc->tokens;

	depth = cgc->depth;
	dt = now - at;
	if (dt >= REFILL_MAX_NS) {
		cgc->tokens = depth;
		return depth;
	}

	add = dt * cgc->limit / FRAC_ONE;
	tokens = __sync_fetch_and_add(&cgc->tokens, add) + add;
	max = debt_max(cgc);
	if (tokens > depth)
		__sync_val_compare_and_swap(&cgc->tokens, tokens, depth);
	else if (tokens < -max)
		__sync_val_compare_and_swap(&cgc->tokens, tokens, -max);
	return cgc->tokens;
}

/* the runtime @cgc's limit allows right now, see refill() */
static u64 tokens_left(struct pfs_cgrp_ctx *cgc, u64 now)
{
	s64 tokens = refill(cgc, now);

	return tokens > 0 ? tokens : 0;
}

/*
 * Return whether @cgc has tokens left and clip *@slicep to them. Less than
 * MIN_BUDGET_NS left counts as none to avoid tiny slices, unless the whole
 * budget of a window is smaller than that.
 */
static bool budget_left(struct pfs_cgrp_ctx *cgc, u64 now, u64 *slicep)
{
	u64 left, min_left;

	left = tokens_left(cgc, now);
	min_left = cgc->limit * window_ns / FRAC_ONE;
	if (min_left > MIN_BUDGET_NS)
		min_left = MIN_BUDGET_NS;
	if (!left || left < min_left)
		return false;
	if (left < *slicep)
		*slicep = left;
	return true;
}

/*
 * Count the window of @now in @cgc's nr_periods, and in nr_throttled if
 * @throttled, once per window each.
 */
static void count_window(struct pfs_cgrp_ctx *cgc, u64 now, bool throttled)
{
	u64 id = now / window_ns, old;

	old = cgc->stat.period_id;
	if (old < id &&
	    __sync_val_compare_and_swap(&cgc->stat.period_id, old, id) == old)
		__sync_fetch_and_add(&cgc->stat.nr_periods, 1);
	if (!throttled)
		return;
	old = cgc->stat.throttled_id;
	if (old < id &&
	    __sync_val_compare_and_swap(&cgc->stat.throttled_id, old, id) == old)
		__sync_fetch_and_add(&cgc->stat.nr_throttled, 1);
}

/*
 * @taskc starts waiting in a DSQ of @wait_cgrp, which refused it a slot. The
 * cgroup is throttled from its first waiter's start to its last waiter's end,
 * and so is each limited ancestor whose slots are all taken too, as CFS's
 * cpu.stat counts a throttled parent: the task is held back by its limit as
 * well. The levels counted are remembered in ->wait_mask, by the cgroup IDs
 * of the task's path.
 */
static void wait_begin(struct pfs_task_ctx *taskc, struct cgroup *wait_cgrp,
		       u64 now)
{
	struct pfs_cgrp_ctx *cgc;
	u64 mask = 0, cgid;
	int level;

	if (taskc->wait_mask || too_deep(wait_cgrp))
		return;

	bpf_for(level, 1, MAX_LEVELS) {
		if (level > wait_cgrp->level)
			break;
		cgc = find_ancestor_ctx(wait_cgrp, level, &cgid);
		if (!cgc)
			return;
		if (!cgc->limit || cgid != taskc->path_cgid[path_level(level)])
			continue;
		if (level < wait_cgrp->level &&
		    cgc->nr_slots < div_round_up(cgc->limit, FRAC_ONE))
			continue;
		mask |= 1LLU << level;
		if (!__sync_fetch_and_add(&cgc->stat.nr_waiting, 1))
			cgc->stat.wait_since = now;
		count_window(cgc, now, true);
	}
	taskc->wait_mask = mask;
}

/* @taskc no longer waits, if it did, see wait_begin() */
static void wait_end(struct pfs_task_ctx *taskc, u64 now)
{
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *cgrp;
	u64 mask, since;
	int level;

	if (!taskc->wait_mask)
		return;
	mask = __sync_lock_test_and_set(&taskc->wait_mask, 0);

	bpf_for(level, 1, MAX_LEVELS) {
		if (!(mask >> level))
			break;
		if (!(mask & (1LLU << level)))
			continue;
		cgrp = bpf_cgroup_from_id(taskc->path_cgid[path_level(level)]);
		if (!cgrp)
			continue;
		cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
		if (cgc) {
			since = cgc->stat.wait_since;
			if (__sync_sub_and_fetch(&cgc->stat.nr_waiting, 1) <= 0 &&
			    since && now > since)
				__sync_fetch_and_add(&cgc->stat.throttled_ns,
						     now - since);
		}
		bpf_cgroup_release(cgrp);
	}
}

/*
 * Has @cgc's subtree run beyond its limit, i.e. is its token bucket in debt,
 * by more than a margin? Runtime is charged late, by a tick or, in a virtual
 * machine whose CPU was preempted, by much more, so a margin of a tick per
 * slot plus one window's budget absorbs that. The margin only bounds the
 * excess once.
 */
static bool over_limit(struct pfs_cgrp_ctx *cgc, u64 now)
{
	u64 width = div_round_up(cgc->limit, FRAC_ONE);
	u64 margin = cgc->limit * window_ns / FRAC_ONE + width * TICK_SLACK_NS;

	return refill(cgc, now) < -(s64)margin;
}

/*
 * Take a slot in a single limited cgroup. The first floor(limit) slots are
 * free. The last slot of a fractional limit needs tokens, see budget_left().
 * The last full slot and the rate limited slot also need the cgroup to be
 * within its limit, see over_limit(), so that a cgroup which ran beyond it,
 * e.g. with tasks on credit, pays the excess back. Once the debt reached its
 * cap, no slot is handed out until it went down.
 */
static enum reserve_result take_slot(struct pfs_cgrp_ctx *cgc, u64 now,
				     u64 *slicep)
{
	u64 width = div_round_up(cgc->limit, FRAC_ONE);
	u64 full = cgc->limit / FRAC_ONE;
	u64 taken;

	taken = __sync_fetch_and_add(&cgc->nr_slots, 1);
	if (taken >= width) {
		__sync_fetch_and_sub(&cgc->nr_slots, 1);
		return RESERVE_FULL;
	}

	if (taken >= full && !budget_left(cgc, now, slicep)) {
		__sync_fetch_and_sub(&cgc->nr_slots, 1);
		return RESERVE_BUDGET;
	}

	/* the window timer retries the cgroups refused for budget */
	if ((taken + 1 >= full || cgc->tokens <= -debt_max(cgc)) &&
	    over_limit(cgc, now)) {
		__sync_fetch_and_sub(&cgc->nr_slots, 1);
		stat_inc(PFS_STAT_PARK_DEBT);
		return RESERVE_BUDGET;
	}

	return RESERVE_OK;
}

/*
 * Give back the slots in @mask taken on @cgrp's path. All walks over the path
 * cover levels 1 to @cgrp->level, which is below MAX_LEVELS for a cgroup whose
 * tasks can hold slots.
 */
enum put_flags {
	PUT_RETRY	= 1 << 0,	/* queue a retry for levels with waiters */
};

static void put_one_slot(struct pfs_cgrp_ctx *cgc, u64 cgid, u32 flags)
{
	__sync_fetch_and_sub(&cgc->nr_slots, 1);
	if ((flags & PUT_RETRY) && nr_waiting(cgid))
		queue_retry(cgc, cgid);
}

static void put_slots(struct cgroup *cgrp, u64 mask, u32 flags)
{
	struct pfs_cgrp_ctx *cgc;
	u64 cgid;
	int level;

	if (!mask)
		return;

	bpf_for(level, 1, MAX_LEVELS) {
		if (level > cgrp->level)
			break;
		if (!(mask & (1LLU << level)))
			continue;

		cgc = find_ancestor_ctx(cgrp, level, &cgid);
		if (!cgc)
			return;

		put_one_slot(cgc, cgid, flags);
	}
}

/*
 * Give back the slots in @mask through the cgroup IDs remembered in @taskc,
 * which still works for the levels which exist when the task's own cgroup
 * has already been removed.
 */
static void put_path_slots(struct pfs_task_ctx *taskc, u64 mask, u32 flags)
{
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *cgrp;
	u64 cgid;
	int level;

	bpf_for(level, 1, MAX_LEVELS) {
		if (!(mask >> level))
			break;
		if (!(mask & (1LLU << level)))
			continue;

		cgid = taskc->path_cgid[path_level(level)];
		cgrp = bpf_cgroup_from_id(cgid);
		if (!cgrp) {
			stat_inc(PFS_STAT_LOST_PATH);
			continue;
		}
		cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
		if (cgc)
			put_one_slot(cgc, cgid, flags);
		bpf_cgroup_release(cgrp);
	}
}

/*
 * Make @cgrp the cgroup of @taskc. The path is only looked up when the
 * cgroup changes. The caller makes sure that @taskc holds no slots.
 */
static void set_task_cgrp(struct pfs_task_ctx *taskc, struct cgroup *cgrp)
{
	u64 cgid = cgrp->kn->id;
	int level;

	if (taskc->cgid == cgid && taskc->path_cgid[0] == cgid)
		return;

	bpf_for(level, 1, MAX_LEVELS) {
		struct cgroup *anc;

		if (level > cgrp->level)
			break;
		anc = bpf_cgroup_ancestor(cgrp, level);
		if (!anc) {
			scx_bpf_error("ancestor cgroup lookup failed");
			return;
		}
		taskc->path_cgid[path_level(level)] = anc->kn->id;
		bpf_cgroup_release(anc);
	}
	/* level 0 is the root, its slot is unused: mark the path valid */
	taskc->path_cgid[0] = cgid;
	taskc->cgid = cgid;
}

/*
 * Take a slot in every limited cgroup on @cgrp's path, starting from @cgrp.
 * On success, *@maskp has a bit set for each level which holds a slot. On
 * failure, nothing is held and *@fail_cgidp is the cgroup which refused.
 *
 * If @fifo, a level which already has waiting tasks is treated as full so that
 * a new task doesn't overtake them. A retry admitting the waiting tasks
 * themselves clears it.
 */
static enum reserve_result reserve_slots(struct cgroup *cgrp, bool fifo,
					 u64 *slicep, u64 *maskp,
					 u64 *fail_cgidp)
{
	enum reserve_result ret = RESERVE_OK;
	struct pfs_cgrp_ctx *cgc;
	u64 now = window_now();
	u64 mask = 0, cgid;
	int i;

	*maskp = 0;
	if (!nr_limited)
		return RESERVE_OK;

	/* rather not limit at all than enforce only part of the path */
	if (too_deep(cgrp)) {
		stat_inc(PFS_STAT_TOO_DEEP);
		return RESERVE_OK;
	}

	/* from @cgrp->level down to 1, the same levels as put_slots() */
	bpf_for(i, 0, MAX_LEVELS) {
		int level = cgrp->level - i;

		if (level < 1)
			break;

		cgc = find_ancestor_ctx(cgrp, level, &cgid);
		if (!cgc) {
			ret = RESERVE_FULL;
			break;
		}
		if (!cgc->limit)
			continue;

		if (fifo && nr_waiting(cgid)) {
			ret = RESERVE_FULL;
			*fail_cgidp = cgid;
			break;
		}

		ret = take_slot(cgc, now, slicep);
		if (ret != RESERVE_OK) {
			*fail_cgidp = cgid;
			break;
		}
		mask |= 1LLU << level;
	}

	if (ret != RESERVE_OK) {
		put_slots(cgrp, mask, 0);
		return ret;
	}

	*maskp = mask;
	return RESERVE_OK;
}

/*
 * Make the slots in @mask the ones @taskc holds. Fails if @taskc already holds
 * slots, in which case the caller still owns @mask and has to put it.
 *
 * The published value carries a per-task sequence number in its high half so
 * that unpublish_slots() can't take back slots which were released and then
 * published again by another path in the meantime.
 */
static bool publish_slots(struct pfs_task_ctx *taskc, u64 mask, u64 *valp)
{
	u64 seq = __sync_fetch_and_add(&taskc->slot_seq, 1);

	*valp = 0;
	if (!mask)
		return true;

	*valp = (seq << SLOT_SEQ_SHIFT) | mask;
	return !__sync_val_compare_and_swap(&taskc->slot_mask, 0, *valp);
}

/*
 * Take back the slots published as @val. Returns true if they were still
 * published, in which case the caller owns them again and has to put them.
 * Otherwise another path has already released them.
 */
static bool unpublish_slots(struct pfs_task_ctx *taskc, u64 val)
{
	return val &&
		__sync_val_compare_and_swap(&taskc->slot_mask, val, 0) == val;
}

/*
 * Tasks which never wait for a slot: exiting tasks, kernel threads and tasks
 * with SIGKILL pending are about to give up the locks and resources they hold,
 * which tasks of other cgroups may wait for.
 */
static bool must_not_park(struct task_struct *p)
{
	if (p->flags & (PF_EXITING | PF_KTHREAD))
		return true;
	return p->pending.signal.sig[0] & (1LLU << (PFS_SIGKILL - 1));
}

/*
 * Can @cgc let a task run on credit, without a slot, and for how long? Only
 * with the tokens it has, i.e. what it saved up while it ran below its limit,
 * and only while it is within its limit, see over_limit(). The holders of
 * full slots don't need tokens: they use at most floor(limit) CPUs while the
 * bucket refills at the limit. The rate limited slot draws on the same tokens
 * as credit. A cgroup which keeps its slots busy, e.g. threads that take
 * turns on a lock, saves nothing and gives no credit once its tokens are
 * spent; one whose tasks often sleep, e.g. a database or pipes, does.
 */
static bool credit_ok(struct pfs_cgrp_ctx *cgc, u64 now, u64 *slicep)
{
	u64 left, min_left;

	if (over_limit(cgc, now))
		return false;

	left = tokens_left(cgc, now);
	min_left = cgc->limit * window_ns / FRAC_ONE;
	if (min_left > MIN_BUDGET_NS)
		min_left = MIN_BUDGET_NS;
	if (!left || left < min_left)
		return false;
	if (left < *slicep)
		*slicep = left;
	return true;
}

/*
 * Whether a task of @cgrp which found no free slot may run on credit, without
 * slots: every limited cgroup on the path can give credit, see credit_ok(),
 * the smallest of which clips *@slicep. The runtime is taken off the
 * buckets, and any excess is paid back as debt.
 */
static bool credit_left(struct cgroup *cgrp, u64 now, u64 *slicep)
{
	struct pfs_cgrp_ctx *cgc;
	u64 cgid;
	int level;

	if (too_deep(cgrp))
		return true;

	bpf_for(level, 1, MAX_LEVELS) {
		if (level > cgrp->level)
			break;

		cgc = find_ancestor_ctx(cgrp, level, &cgid);
		if (!cgc)
			return false;
		if (cgc->limit && !credit_ok(cgc, now, slicep))
			return false;
	}
	return true;
}

/*
 * Give back the slots @p holds. The xchg makes sure that the slots are put
 * once when several paths try to release them concurrently.
 */
static void release_slots(struct pfs_task_ctx *taskc)
{
	u64 mask;

	mask = __sync_lock_test_and_set(&taskc->slot_mask, 0);
	if (!mask)
		return;

	put_path_slots(taskc, mask & SLOT_LEVELS_MASK, PUT_RETRY);
}

/* @taskc took its slots at @now, after a wakeup unless @rotated */
static void set_admitted(struct pfs_task_ctx *taskc, u64 now, bool rotated)
{
	taskc->admitted_at = now;
	taskc->rotated = rotated;
	taskc->credit = 0;
}

/*
 * A task parked by ops.enqueue() only lands in the wait DSQ after the op
 * returns: the kernel carries out the insertion then. A retry which runs on
 * another CPU in between finds the DSQ empty while the slots may already be
 * free, and nobody would queue another retry. park() counts the task in
 * ->nr_parking of the cgroup until a walk sees it in the DSQ, see
 * park_landed(), or it leaves the DSQ otherwise, see park_cancel(), and
 * retry_cgroup() queues the retry again while parks are in flight.
 */
static void park_landed(struct pfs_task_ctx *taskc, struct pfs_cgrp_ctx *cgc,
			u64 cgid)
{
	if (taskc->parking_cgid == cgid &&
	    __sync_val_compare_and_swap(&taskc->parking_cgid, cgid, 0) == cgid)
		__sync_fetch_and_sub(&cgc->nr_parking, 1);
}

static void park_cancel(struct pfs_task_ctx *taskc)
{
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *cgrp;
	u64 cgid;

	if (!taskc->parking_cgid)
		return;
	cgid = __sync_lock_test_and_set(&taskc->parking_cgid, 0);
	if (!cgid)
		return;

	cgrp = bpf_cgroup_from_id(cgid);
	if (!cgrp)
		return;
	cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
	if (cgc)
		__sync_fetch_and_sub(&cgc->nr_parking, 1);
	bpf_cgroup_release(cgrp);
}

/*
 * Wait in @cgid's DSQ for the woken (@woken) or the rotated tasks. Tasks
 * which woke up and found no free slot are admitted first, see
 * retry_cgroup(): they are typically request threads or the holders of what
 * other threads spin or sleep for, while rotated tasks used up their slice.
 */
static void park(struct task_struct *p, struct pfs_task_ctx *taskc, u64 cgid,
		 enum reserve_result ret, u64 vtime, u64 enq_flags, bool woken)
{
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *wait_cgrp;

	/*
	 * The DSQs of a removed cgroup are destroyed with it, see
	 * pfs_cgroup_exit(), and inserting into one is a scheduler error.
	 * Run without slots instead.
	 */
	wait_cgrp = bpf_cgroup_from_id(cgid);
	if (!wait_cgrp) {
		stat_inc(PFS_STAT_LOST_PATH);
		scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, slice_ns, vtime,
					 enq_flags);
		return;
	}

	/*
	 * Waiting tasks are admitted in FIFO order, which bounds the wait to
	 * about one slice per task queued ahead divided by the width. In vtime
	 * order, the tick-granular run lengths make the vtimes of the waiting
	 * tasks drift apart and a task which got ahead waits several turns.
	 * @vtime is kept for the shared DSQ. The preemption flags imply
	 * SCX_ENQ_HEAD and are cleared with it.
	 */
	if (woken)
		stat_inc(PFS_STAT_PARK_WOKEN);

	cgc = find_cgrp_ctx(wait_cgrp);
	if (cgc) {
		wait_begin(taskc, wait_cgrp, window_now());
		taskc->parking_cgid = cgid;
		cgc->park_at = window_now();
		__sync_fetch_and_add(&cgc->nr_parking, 1);
	}

	scx_bpf_task_set_dsq_vtime(p, vtime);
	scx_bpf_dsq_insert(p, woken ? cgid | WOKEN_DSQ : cgid, slice_ns,
			   enq_flags & ~(SCX_ENQ_HEAD | SCX_ENQ_PREEMPT |
					 SCX_ENQ_PREEMPT_LAZY));

	if (cgc) {
		waitset_add(cgc, cgid);
		/*
		 * The slots may have been given back between the failed
		 * reservation and the insertion above, in which case nobody
		 * would queue a retry. Queue one unconditionally.
		 */
		if (ret == RESERVE_BUDGET) {
			stat_inc(PFS_STAT_PARK_BUDGET);
			queue_budget(cgc, cgid);
		} else {
			stat_inc(PFS_STAT_PARK_FULL);
			queue_retry(cgc, cgid);
		}
	}
	bpf_cgroup_release(wait_cgrp);
}

/*
 * A retry admitted the task with @pid for this CPU without kicking an idle
 * CPU for it, but this CPU's dispatch may have taken another task from the
 * shared DSQ. Kick an idle CPU the task can run on if it is still queued
 * there. The task is found again by pid as the walker's pointer to it can't
 * be kept. If it is gone, fall back to any idle CPU.
 */
static void kick_leftover(s32 pid)
{
	struct task_struct *p;
	struct scx_dispatch_q *dsq;

	p = pid ? bpf_task_from_pid(pid) : NULL;
	if (!p) {
		kick_idle_cpu_any();
		return;
	}

	dsq = p->scx.dsq;
	if (dsq && dsq->id == SHARED_DSQ)
		kick_idle_cpu(p);
	bpf_task_release(p);
}

enum walk_result {
	WK_EMPTY,		/* every task of the DSQ was examined */
	WK_MORE,		/* stopped early, more tasks may be admittable */
	WK_FULL,		/* @cgid itself has no free slot */
	WK_BUDGET,		/* @cgid's rate limited slot is out of tokens */
};

/*
 * Admit the tasks waiting in @dsq_id, one of the two wait DSQs of the cgroup
 * whose context is cgc, see retry_cgroup(). The number of tasks examined is
 * counted in cgc, which is owned by the walker, instead of on the stack. The
 * verifier doesn't know its value, so it doesn't have to walk the loop once for
 * each possible count. A global function, verified once rather than for each
 * of its call sites, which made loading slow and memory hungry.
 */
__noinline int walk_waiters(u64 dsq_id, u32 *nr_nokick, s32 *nokick_pid)
{
	bool woken_dsq = dsq_id & WOKEN_DSQ;
	u64 cgid = dsq_id & ~WOKEN_DSQ;
	struct pfs_cgrp_ctx *cgc;
	struct task_struct *p;
	struct cgroup *cgrp;
	int res = WK_EMPTY;

	if (!nr_nokick || !nokick_pid)
		return WK_EMPTY;

	cgrp = bpf_cgroup_from_id(cgid);
	if (!cgrp)
		return WK_EMPTY;
	cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
	bpf_cgroup_release(cgrp);
	if (!cgc)
		return WK_EMPTY;

	/* global functions aren't verified as running under RCU */
	bpf_rcu_read_lock();
	bpf_for_each(scx_dsq, p, dsq_id, 0) {
		enum reserve_result ret;
		struct pfs_task_ctx *taskc;
		struct cgroup *task_cgrp;
		u64 slice = slice_ns, mask, val, fail_cgid = 0;
		u32 walked;

		walked = READ_ONCE(cgc->retry_walked);
		if (walked >= RETRY_WALK_MAX) {
			res = WK_MORE;
			break;
		}
		WRITE_ONCE(cgc->retry_walked, walked + 1);

		taskc = bpf_task_storage_get(&task_ctx, p, 0, 0);
		if (!taskc) {
			res = WK_MORE;
			break;
		}
		park_landed(taskc, cgc, cgid);

		/* e.g. SIGKILL arrived while @p waited, let it run on credit */
		if (must_not_park(p)) {
			set_admitted(taskc, window_now(), !woken_dsq);
			if (scx_bpf_dsq_move_vtime(BPF_FOR_EACH_ITER, p,
						   SHARED_DSQ, 0)) {
				wait_end(taskc, window_now());
				stat_inc(PFS_STAT_CREDIT);
				kick_idle_cpu(p);
			}
			continue;
		}

		/* the slots are put through @task_cgrp, keep it until done */
		task_cgrp = bpf_cgroup_from_id(taskc->cgid);
		if (!task_cgrp) {
			/*
			 * The cgroup was removed under an exiting task. Its
			 * slots can't be accounted anymore, let it run
			 * without them rather than wait forever.
			 */
			set_admitted(taskc, window_now(), !woken_dsq);
			if (scx_bpf_dsq_move_vtime(BPF_FOR_EACH_ITER, p,
						   SHARED_DSQ, 0)) {
				wait_end(taskc, window_now());
				stat_inc(PFS_STAT_LOST_PATH);
				kick_idle_cpu(p);
			}
			continue;
		}

		ret = reserve_slots(task_cgrp, false, &slice, &mask, &fail_cgid);
		if (ret == RESERVE_OK) {
			/* publish before @p can run and give the slots back */
			if (!publish_slots(taskc, mask, &val)) {
				stat_inc(PFS_STAT_MOVE_RACE);
				put_slots(task_cgrp, mask, PUT_RETRY);
				bpf_cgroup_release(task_cgrp);
				continue;
			}
			/*
			 * @p moved to another cgroup since ->cgid was read: the
			 * slots are on the old path, which release_slots()
			 * no longer knows. Take them back.
			 */
			if (taskc->cgid != task_cgrp->kn->id) {
				stat_inc(PFS_STAT_MOVE_RACE);
				if (unpublish_slots(taskc, val))
					put_slots(task_cgrp, mask, PUT_RETRY);
				bpf_cgroup_release(task_cgrp);
				continue;
			}
			set_admitted(taskc, window_now(), !woken_dsq);

			scx_bpf_dsq_move_set_slice(BPF_FOR_EACH_ITER, slice);
			if (scx_bpf_dsq_move_vtime(BPF_FOR_EACH_ITER, p,
						   SHARED_DSQ, 0)) {
				wait_end(taskc, window_now());
				stat_inc(PFS_STAT_ADMIT);
				if (*nr_nokick &&
				    bpf_cpumask_test_cpu(bpf_get_smp_processor_id(),
							 p->cpus_ptr)) {
					(*nr_nokick)--;
					*nokick_pid = p->pid;
				} else {
					kick_idle_cpu(p);
				}
			} else {
				/*
				 * @p left the DSQ. Whoever dequeued it may
				 * have released the slots already.
				 */
				stat_inc(PFS_STAT_MOVE_RACE);
				if (unpublish_slots(taskc, val))
					put_slots(task_cgrp, mask, PUT_RETRY);
			}
			bpf_cgroup_release(task_cgrp);

			/*
			 * At most WOKEN_STREAK_MAX woken tasks in a row while
			 * rotated ones wait, and back to the woken tasks after
			 * each rotated one.
			 */
			if (woken_dsq) {
				if (scx_bpf_dsq_nr_queued(cgid) > 0 &&
				    ++cgc->woken_streak >= WOKEN_STREAK_MAX) {
					res = WK_MORE;
					break;
				}
			} else {
				cgc->woken_streak = 0;
				if (scx_bpf_dsq_nr_queued(cgid | WOKEN_DSQ) > 0) {
					res = WK_MORE;
					break;
				}
			}
			continue;
		}
		bpf_cgroup_release(task_cgrp);

		if (fail_cgid == cgid) {
			res = ret == RESERVE_BUDGET ? WK_BUDGET : WK_FULL;
			break;
		}

		/* refused by another level, wait there instead */
		if (scx_bpf_dsq_move(BPF_FOR_EACH_ITER, p, woken_dsq ?
				     fail_cgid | WOKEN_DSQ : fail_cgid, 0)) {
			struct pfs_cgrp_ctx *fail_cgc;
			struct cgroup *fail_cgrp;

			stat_inc(PFS_STAT_REPARK);
			wait_end(taskc, window_now());
			fail_cgrp = bpf_cgroup_from_id(fail_cgid);
			if (!fail_cgrp)
				continue;
			fail_cgc = bpf_cgrp_storage_get(&cgrp_ctx, fail_cgrp, 0, 0);
			if (fail_cgc) {
				wait_begin(taskc, fail_cgrp, window_now());
				waitset_add(fail_cgc, fail_cgid);
				if (ret == RESERVE_BUDGET)
					queue_budget(fail_cgc, fail_cgid);
				else
					queue_retry(fail_cgc, fail_cgid);
			}
			bpf_cgroup_release(fail_cgrp);
		}
	}
	bpf_rcu_read_unlock();

	return res;
}

/*
 * Try to admit the tasks waiting in @cgid's DSQs, the woken ones first, but at
 * most WOKEN_STREAK_MAX in a row while rotated tasks wait. A task which can't
 * be admitted because another level of its path refuses is moved to that
 * level's DSQ of the same kind. Stops at the first task refused by @cgid
 * itself.
 *
 * The first *@nr_nokick admitted tasks which can run on this CPU don't kick an
 * idle CPU, the calling dispatch picks them up itself. *@nokick_pid is set to
 * the pid of the last such task so that dispatch can kick for it if it takes
 * another task instead.
 *
 * Only one walker scans a cgroup's DSQs at a time so that two walkers can't
 * reserve slots for the same task. A retry which finds them busy, or which
 * stops after RETRY_WALK_MAX tasks with more left, is queued again so that it
 * isn't lost.
 */
static void retry_cgroup(u64 cgid, u32 *nr_nokick, s32 *nokick_pid)
{
	int res = WK_EMPTY, i;
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *cgrp;

	cgrp = bpf_cgroup_from_id(cgid);
	if (!cgrp)
		return;

	cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
	if (!cgc)
		goto out_release;

	/* paired with the cmpxchg in queue_retry() */
	cgc->retry_queued = 0;

	if (__sync_val_compare_and_swap(&cgc->retry_running, 0, 1)) {
		queue_retry(cgc, cgid);
		goto out_release;
	}

	stat_inc(PFS_STAT_RETRY);
	cgc->retry_walked = 0;
	cgc->retry_at = window_now();

	bpf_for(i, 0, 2 * WOKEN_STREAK_MAX) {
		if (cgc->woken_streak >= WOKEN_STREAK_MAX &&
		    scx_bpf_dsq_nr_queued(cgid) > 0) {
			res = walk_waiters(cgid, nr_nokick, nokick_pid);
		} else {
			res = walk_waiters(cgid | WOKEN_DSQ, nr_nokick,
					   nokick_pid);
			if (res == WK_EMPTY)
				res = walk_waiters(cgid, nr_nokick, nokick_pid);
		}
		if (res != WK_MORE ||
		    READ_ONCE(cgc->retry_walked) >= RETRY_WALK_MAX)
			break;
	}

	/* paired with the cmpxchg above, fully ordered */
	__sync_lock_test_and_set(&cgc->retry_running, 0);

	if (res == WK_MORE) {
		queue_retry(cgc, cgid);
	} else if (res == WK_BUDGET) {
		queue_budget(cgc, cgid);
	} else if (res == WK_EMPTY && READ_ONCE(cgc->nr_parking) > 0 &&
		   window_now() - READ_ONCE(cgc->park_at) < PARK_LAND_NS) {
		/* a task is about to land in the DSQ, see park_landed() */
		stat_inc(PFS_STAT_PARK_RACE);
		queue_retry(cgc, cgid);
	}

out_release:
	bpf_cgroup_release(cgrp);
}

/* cgroups whose tasks are charged by their hweight */
static bool weighted(struct cgroup *cgrp)
{
	return cgrp->level && !too_deep(cgrp);
}

/*
 * Recompute the hweight of the active cgroup @cgrp and of its ancestors if a
 * weight or the set of active cgroups changed since it was last computed.
 *
 * hweight_gen is global, so on a host with many cgroups going idle and busy it
 * changes all the time. A cgroup's hweight is recomputed at most once every
 * HWEIGHT_REFRESH_NS, about a tick, and is that much out of date at worst.
 */
static void refresh_hweight(struct cgroup *cgrp, struct pfs_cgrp_ctx *cgc)
{
	u64 cgid, now;
	int level;

	if (cgc->nr_active <= 0 || cgc->hweight_gen == hweight_gen)
		return;

	now = window_now();
	if (now - cgc->hweight_at < HWEIGHT_REFRESH_NS)
		return;
	cgc->hweight_at = now;

	stat_inc(PFS_STAT_HWEIGHT);

	bpf_for(level, 0, MAX_LEVELS) {
		struct pfs_cgrp_ctx *acgc, *pcgc;
		bool is_active;

		if (level > cgrp->level)
			break;

		acgc = find_ancestor_ctx(cgrp, level, &cgid);
		if (!acgc)
			break;

		if (!level) {
			acgc->hweight = HWEIGHT_ONE;
			acgc->hweight_gen = hweight_gen;
			continue;
		}

		pcgc = find_ancestor_ctx(cgrp, level - 1, &cgid);
		if (!pcgc)
			break;

		bpf_spin_lock(&weight_lock);
		is_active = acgc->nr_active > 0;
		if (is_active) {
			acgc->hweight_gen = pcgc->hweight_gen;
			acgc->hweight = pcgc->hweight * acgc->weight /
					(pcgc->child_weight_sum ?: 1) ?: 1;
		}
		bpf_spin_unlock(&weight_lock);

		/* raced against a deactivation, the next charge recomputes */
		if (!is_active)
			break;
	}
}

/*
 * Count @cgrp active or inactive in its parent's active weight sum and
 * propagate upwards while the parents' states change. The increments and
 * decrements commute, so a cgroup's tasks racing in and out of runnable from
 * several CPUs leave consistent sums. If a decrement overtakes the increment
 * it pairs with, ->nr_active goes negative for a moment without propagating
 * and the increment brings it back to 0, again without propagating. A cgroup's
 * weight is in its parent's sum exactly while its ->nr_active is positive,
 * which is what pfs_cgroup_set_weight() tests.
 */
static void update_active(struct cgroup *cgrp, bool runnable)
{
	bool updated = false;
	u64 cgid;
	int i;

	bpf_for(i, 0, MAX_LEVELS) {
		struct pfs_cgrp_ctx *cgc, *pcgc;
		int level = cgrp->level - i;
		bool propagate = false;

		if (level < 1)
			break;

		cgc = find_ancestor_ctx(cgrp, level, &cgid);
		if (!cgc)
			break;
		pcgc = find_ancestor_ctx(cgrp, level - 1, &cgid);
		if (!pcgc)
			break;

		/* no function calls are allowed under the lock */
		bpf_spin_lock(&weight_lock);
		if (runnable) {
			if (!cgc->nr_active++) {
				pcgc->child_weight_sum += cgc->weight;
				propagate = true;
			}
		} else {
			if (!--cgc->nr_active) {
				pcgc->child_weight_sum -= cgc->weight;
				propagate = true;
			}
		}
		bpf_spin_unlock(&weight_lock);

		if (!propagate)
			break;
		updated = true;
	}

	if (updated) {
		stat_inc(PFS_STAT_ACTIVATE);
		__sync_fetch_and_add(&hweight_gen, 1);
	}
}

/*
 * Scale @vdelta, runtime already scaled by the task's own weight, by the share
 * of the machine a task of @cgrp with the default weight is entitled to:
 * hweight * 100 / R, R being the weight of all of @cgrp's runnable tasks.
 *
 * A task with share S of a machine of N CPUs waits about slice / (S * N) for
 * its next slice. A cgroup of weight 1 next to siblings of weight 10000 would
 * wait for seconds, the sched_ext watchdog ejects the scheduler after 30s. The
 * share is floored to min_share, set by user space to slice / (N * WAIT_MAX_NS)
 * of the machine, so that the wait stays around WAIT_MAX_NS. The price is that
 * a task entitled to less than slice / WAIT_MAX_NS of a CPU gets that much,
 * e.g. a cgroup with more than 400 runnable tasks per CPU of its share gets
 * more than its share. Slices can't be made shorter instead as they end on a
 * tick.
 *
 * The scaled value is at most @vdelta * HWEIGHT_ONE / min_share, the result
 * can't overflow for @vdelta below 2^64 / (WAIT_MAX_NS * N / slice), months of
 * runtime. The intermediate products are below 2^63 as share is below
 * HWEIGHT_ONE * 100.
 */
static u64 scale_by_cgrp_share(struct cgroup *cgrp, u64 vdelta)
{
	struct pfs_cgrp_ctx *cgc;
	u64 share;

	if (!weighted(cgrp))
		return vdelta;

	cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
	if (!cgc || !cgc->runnable_weight)
		return vdelta;

	refresh_hweight(cgrp, cgc);

	share = cgc->hweight * 100 / cgc->runnable_weight;
	if (share < min_share)
		share = min_share;
	return vdelta / share * HWEIGHT_ONE +
		vdelta % share * HWEIGHT_ONE / share;
}

static u64 clamp_vtime(struct task_struct *p, struct cgroup *cgrp)
{
	u64 vtime = p->scx.dsq_vtime;
	u64 budget = scale_by_cgrp_share(cgrp, slice_ns);

	/*
	 * Limit the amount of budget that an idling task can accumulate to one
	 * slice. The slice is scaled like runtime so that a task of a cgroup
	 * with a small share keeps the same credit in runtime, e.g. a task of
	 * a cgroup which waits for a slot it can't get while it lags behind.
	 */
	if (time_before(vtime, vtime_now - budget))
		vtime = vtime_now - budget;
	return vtime;
}

s32 BPF_STRUCT_OPS(pfs_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	struct pfs_task_ctx *taskc;
	struct cgroup *cgrp;
	u64 slice = slice_ns, mask, val, fail_cgid;
	bool is_idle = false;
	s32 cpu;

	cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
	if (!is_idle)
		return cpu;

	taskc = find_task_ctx(p);
	if (!taskc || taskc->slot_mask)	/* enqueue releases the slots */
		return cpu;

	/* a waking task holds no slot, the target is only taken if one is free */
	cgrp = scx_bpf_task_cgroup(p);
	set_task_cgrp(taskc, cgrp);
	if (reserve_slots(cgrp, true, &slice, &mask, &fail_cgid) == RESERVE_OK) {
		if (publish_slots(taskc, mask, &val)) {
			set_admitted(taskc, window_now(), false);
			stat_inc(PFS_STAT_DIRECT);
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, 0);
		} else {
			/* @p still holds slots, enqueue releases them */
			put_slots(cgrp, mask, PUT_RETRY);
		}
	}
	bpf_cgroup_release(cgrp);

	return cpu;
}

void BPF_STRUCT_OPS(pfs_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct pfs_task_ctx *taskc;
	struct cgroup *cgrp;
	enum reserve_result ret;
	u64 slice = slice_ns, mask, val, fail_cgid = 0;
	u64 vtime;
	bool woken, credit = false;

	/*
	 * Per-cpu kthreads can't be queued behind anything else, run them
	 * locally. They live in the root cgroup which can't be limited.
	 */
	if (p->nr_cpus_allowed == 1 && (p->flags & PF_KTHREAD)) {
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice_ns, enq_flags);
		return;
	}

	taskc = find_task_ctx(p);
	if (!taskc)
		return;

	/*
	 * @p may have been admitted and then dequeued without running, e.g.
	 * for an attribute change, or dequeued while it waited. Start over.
	 */
	release_slots(taskc);
	park_cancel(taskc);
	wait_end(taskc, window_now());

	/* release_slots() and retries find the slots through ->cgid */
	cgrp = scx_bpf_task_cgroup(p);
	set_task_cgrp(taskc, cgrp);
	vtime = clamp_vtime(p, cgrp);
	/*
	 * A task whose credit ran out still serves its wakeup: it waits with
	 * the woken tasks, not behind the rotated ones.
	 */
	woken = (enq_flags & SCX_ENQ_WAKEUP) || taskc->credit;

	ret = reserve_slots(cgrp, true, &slice, &mask, &fail_cgid);
	if (ret == RESERVE_OK && !publish_slots(taskc, mask, &val)) {
		/*
		 * A retry which saw @p before it was dequeued published slots
		 * for it after the release above. The retry's move fails and
		 * takes them back. Wait in @p's own cgroup for another retry.
		 */
		stat_inc(PFS_STAT_MOVE_RACE);
		put_slots(cgrp, mask, PUT_RETRY);
		ret = RESERVE_FULL;
		fail_cgid = taskc->cgid;
	}

	/*
	 * Don't let a task wait which may hold a lock others wait for, see
	 * the top of the file. Woken tasks run on credit while the tokens
	 * last, as do io_uring workers, which never return to user space.
	 *
	 * A task that has just yielded runs on credit whether or not tokens are
	 * left: an in-kernel yield() holds locks (RTNL in
	 * netlink_broadcast_filtered()), and a cgroup which has spent its
	 * tokens is exactly the one whose tasks would wait longest. The runtime
	 * is charged as always, so the bucket goes into debt and the cgroup
	 * pays it back; it gains no CPU over a window.
	 */
	if (ret != RESERVE_OK) {
		bool yielded = taskc->yielded;

		slice = credit_ns && credit_ns < slice_ns ? credit_ns : slice_ns;
		if (must_not_park(p) || yielded ||
		    (credit_ns &&
		     ((enq_flags & SCX_ENQ_WAKEUP) || (p->flags & PF_IO_WORKER)) &&
		     credit_left(cgrp, window_now(), &slice))) {
			stat_inc(yielded ? PFS_STAT_YIELD_CREDIT : PFS_STAT_CREDIT);
			ret = RESERVE_OK;
			credit = true;
		}
	}
	taskc->yielded = 0;

	if (ret == RESERVE_OK) {
		stat_inc(PFS_STAT_SHARED);
		set_admitted(taskc, window_now(), !woken);
		taskc->credit = credit;
		/*
		 * SCX_ENQ_LAST: @p's slice expired and nothing else is queued
		 * for the CPU. Keep it running, the CPU would idle otherwise.
		 */
		if (enq_flags & SCX_ENQ_LAST)
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, enq_flags);
		else
			scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, slice, vtime,
						 enq_flags);
	} else {
		park(p, taskc, fail_cgid, ret, vtime, enq_flags, woken);
		/* run a dispatch cycle on the CPU to admit a waiting task */
		if (enq_flags & SCX_ENQ_LAST)
			scx_bpf_kick_cpu(scx_bpf_task_cpu(p), 0);
	}
	bpf_cgroup_release(cgrp);
}

/* take @delta of runtime off the buckets of the limited cgroups on the path */
static void charge_runtime(struct cgroup *cgrp, u64 now, u64 delta)
{
	struct pfs_cgrp_ctx *cgc;
	u64 cgid;
	int level;

	if (too_deep(cgrp))
		return;

	bpf_for(level, 1, MAX_LEVELS) {
		if (level > cgrp->level)
			break;

		cgc = find_ancestor_ctx(cgrp, level, &cgid);
		if (!cgc)
			return;
		if (!cgc->limit)
			continue;

		refill(cgc, now);
		__sync_fetch_and_add(&cgc->tokens, -(s64)delta);
		count_window(cgc, now, cgc->stat.nr_waiting > 0);
	}
}

/*
 * Charge @p's runtime since it started running, or since it was last charged,
 * to its vtime, scaled by its weight and @cgrp's share, and to the buckets on
 * @cgrp's path.
 */
static void charge_task(struct task_struct *p, struct pfs_task_ctx *taskc,
			struct cgroup *cgrp, u64 now)
{
	u64 delta, vdelta;

	if (now <= taskc->started_at)
		return;

	delta = now - taskc->started_at;
	taskc->started_at = now;

	vdelta = scale_by_task_weight_inverse(p, delta);
	scx_bpf_task_set_dsq_vtime(p, p->scx.dsq_vtime +
				   scale_by_cgrp_share(cgrp, vdelta));
	if (nr_limited)
		charge_runtime(cgrp, now, delta);
}

/*
 * Charge the runtime of the task @prev which ops.dispatch() is called for.
 * scx_bpf_task_cgroup() only works on the tasks an op is called for, which
 * doesn't include @prev, so the cgroup is looked up through ->cgid, which
 * follows @prev's moves.
 */
static void charge_prev(struct task_struct *prev, struct pfs_task_ctx *taskc)
{
	struct cgroup *cgrp;

	cgrp = bpf_cgroup_from_id(taskc->cgid);
	if (!cgrp)
		return;

	charge_task(prev, taskc, cgrp, window_now());
	bpf_cgroup_release(cgrp);
}

/* vtime of the first task on the shared DSQ, false if it is empty */
static bool shared_head_vtime(u64 *vtimep)
{
	struct task_struct *p;

	bpf_for_each(scx_dsq, p, SHARED_DSQ, 0) {
		*vtimep = p->scx.dsq_vtime;
		return true;
	}
	return false;
}

/*
 * Let @prev, whose slice expired and which is still runnable, run for another
 * slice if it would be picked next anyway: its vtime isn't after the first one
 * on the shared DSQ and it gets its slots again, which it doesn't while tasks
 * wait for them or the rate limited slot is out of budget. Without this, @prev
 * always gives the CPU to the shared DSQ's first task, as it is only queued
 * again after dispatch, and the tasks of a cgroup with fewer tasks than its
 * share of CPUs wait out a turn after every slice they run.
 */
static bool keep_prev(struct task_struct *prev, struct pfs_task_ctx *taskc)
{
	u64 slice = slice_ns, mask, val, fail_cgid, head;
	struct cgroup *cgrp;
	bool keep = false;

	if (!(prev->scx.flags & SCX_TASK_QUEUED))
		return false;

	/* ops.enqueue() runs them locally without slots, it can do so again */
	if ((prev->flags & PF_KTHREAD) && prev->nr_cpus_allowed == 1)
		return false;

	cgrp = bpf_cgroup_from_id(taskc->cgid);
	if (!cgrp)
		return false;

	/*
	 * Compare with the runtime of the expired slice charged. Its credit
	 * isn't clamped: a task which keeps its CPU while it lags is getting
	 * less than its share, e.g. one of few tasks of a cgroup entitled to
	 * more CPUs than it has tasks.
	 */
	charge_task(prev, taskc, cgrp, window_now());

	if (shared_head_vtime(&head) &&
	    time_before(head, prev->scx.dsq_vtime))
		goto out_release;

	if (reserve_slots(cgrp, true, &slice, &mask, &fail_cgid) != RESERVE_OK)
		goto out_release;
	if (!publish_slots(taskc, mask, &val)) {
		put_slots(cgrp, mask, PUT_RETRY);
		goto out_release;
	}
	set_admitted(taskc, window_now(), true);

	/*
	 * A kept task doesn't go through ops.running(), advance the global
	 * vtime for it here, otherwise tasks waking up later get the whole kept
	 * stretch as credit. Only for a kept task: one which goes back to the
	 * shared DSQ is far ahead after a slice if its share is small.
	 */
	if (time_before(vtime_now, prev->scx.dsq_vtime))
		vtime_now = prev->scx.dsq_vtime;

	stat_inc(PFS_STAT_KEEP);
	prev->scx.slice = slice;
	keep = true;
out_release:
	bpf_cgroup_release(cgrp);
	return keep;
}

void BPF_STRUCT_OPS(pfs_dispatch, s32 cpu, struct task_struct *prev)
{
	struct pfs_task_ctx *taskc = NULL;
	u32 nr_nokick = 1;
	s32 nokick_pid = 0;
	u64 cgid;
	int i;

	/*
	 * Dispatch isn't called while @prev has slice left, so @prev is about
	 * to block or its slice expired. Give its slots back here instead of
	 * in ops.stopping(), which runs after dispatch, so that the retries
	 * below can hand them to a waiting task which then runs on this CPU
	 * without an idle gap and a kick. If @prev is still runnable and
	 * comes first, keep_prev() reserves again and keeps it running, if it
	 * ends up being the only task for the CPU, ops.enqueue() does.
	 *
	 * Charge @prev's runtime first, otherwise the retries see the tokens
	 * from before it ran and can admit a task to a rate limited slot
	 * whose tokens @prev just used up. ops.stopping() charges the rest.
	 * @prev is NULL if it isn't an SCX task.
	 */
	if (prev) {
		taskc = bpf_task_storage_get(&task_ctx, prev, 0, 0);
		if (taskc) {
			if (nr_limited && taskc->slot_mask)
				charge_prev(prev, taskc);
			release_slots(taskc);
		}
	}

	bpf_for(i, 0, RETRY_BATCH) {
		if (bpf_map_pop_elem(&retry_q, &cgid))
			break;
		retry_cgroup(cgid, &nr_nokick, &nokick_pid);
	}

	if (taskc && keep_prev(prev, taskc)) {
		if (!nr_nokick)
			kick_leftover(nokick_pid);
		return;
	}

	scx_bpf_dsq_move_to_local(SHARED_DSQ, 0);

	/* an admitted task which wasn't kicked for may be left over */
	if (!nr_nokick && scx_bpf_dsq_nr_queued(SHARED_DSQ) > 0)
		kick_leftover(nokick_pid);

	/*
	 * A @prev which blocked was charged up to ops.stopping(), which runs
	 * when it is dequeued, before this. It is still the CPU's current task
	 * while the retries above run, and with tasks which block often, e.g.
	 * pipes, that time added up to a quarter of the cgroup's usage, which
	 * nobody charged. Charge it to @prev.
	 */
	if (taskc && nr_limited && !(prev->scx.flags & SCX_TASK_QUEUED))
		charge_prev(prev, taskc);
}

void BPF_STRUCT_OPS(pfs_running, struct task_struct *p)
{
	struct pfs_task_ctx *taskc;

	taskc = find_task_ctx(p);
	if (taskc) {
		taskc->started_at = window_now();
		/* moved out of a wait DSQ without a walk, e.g. by a drain */
		wait_end(taskc, taskc->started_at);
	}

	/*
	 * Global vtime always progresses forward as tasks start executing. The
	 * test and update can be performed concurrently from multiple CPUs and
	 * thus racy. Any error should be contained and temporary. Let's just
	 * live with it.
	 */
	if (time_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;
}

void BPF_STRUCT_OPS(pfs_stopping, struct task_struct *p, bool runnable)
{
	struct pfs_task_ctx *taskc;
	struct cgroup *cgrp;
	u64 now = window_now();

	taskc = find_task_ctx(p);
	if (!taskc)
		return;

	cgrp = scx_bpf_task_cgroup(p);
	charge_task(p, taskc, cgrp, now);
	bpf_cgroup_release(cgrp);

	release_slots(taskc);
}

/*
 * Whether a level of @taskc's slots in @mask has waiting tasks, only woken ones
 * if @woken_only.
 */
static bool slots_wanted(struct pfs_task_ctx *taskc, u64 mask, bool woken_only)
{
	int level;

	bpf_for(level, 1, MAX_LEVELS) {
		u64 cgid;

		if (!(mask >> level))
			break;
		if (!(mask & (1LLU << level)))
			continue;
		cgid = taskc->path_cgid[path_level(level)];
		if (scx_bpf_dsq_nr_queued(cgid | WOKEN_DSQ) > 0 ||
		    (!woken_only && scx_bpf_dsq_nr_queued(cgid) > 0))
			return true;
	}
	return false;
}

/*
 * Rotation on wakeup: a task which holds its slots since a rotation, i.e. it
 * is CPU bound and took its turn, gives them back after cut_ns instead of at
 * the end of its slice while a task which woke up waits for one of them. The
 * woken task is typically what the others wait for: a lock holder, the main
 * thread of a spin-wait hand-off, a GC or a VM thread. A task holding its
 * slots since its own wakeup is not cut short.
 *
 * With -r, rotated holders also give their slots to rotated waiting tasks after
 * rot_ns instead of the whole slice. Threads which spin until another thread
 * of their cgroup sets a flag make progress once per rotation, e.g. Redis I/O
 * threads with more threads than slots: -r 1000 made such a hand-off about
 * 1.5x faster, but also split a contended machine less precisely by
 * cpu.weight between limited cgroups, so it is off by default.
 */
void BPF_STRUCT_OPS(pfs_tick, struct task_struct *p)
{
	struct pfs_task_ctx *taskc;
	struct cgroup *cgrp;
	u64 mask, held;

	taskc = bpf_task_storage_get(&task_ctx, p, 0, 0);
	if (!taskc)
		return;

	/*
	 * Charge the runtime as the task runs rather than only when it stops,
	 * so that credit and the debt see it within a tick, see over_limit().
	 */
	if (nr_limited) {
		cgrp = scx_bpf_task_cgroup(p);
		charge_task(p, taskc, cgrp, window_now());
		bpf_cgroup_release(cgrp);
	}

	if (!taskc->rotated)
		return;

	mask = taskc->slot_mask & SLOT_LEVELS_MASK;
	if (!mask)
		return;

	held = window_now() - taskc->admitted_at;
	if (held < cut_ns)
		return;

	if (slots_wanted(taskc, mask, !rot_ns || held < rot_ns)) {
		stat_inc(PFS_STAT_CUT);
		p->scx.slice = 0;
	}
}

/*
 * A yielding task which holds slots keeps them, and keeps running, unless it
 * has held them for cut_ns and other tasks wait for them; then it rotates to
 * the end of the queue. Giving the slots away on every yield made a thread
 * which yields with a lock held wait a whole turn for each yield, with the
 * threads woken for the lock taking its slots only to block again, and the
 * pod used a fraction of its limit. Spinners which yield in a loop, e.g. GC
 * threads offering termination, still hand their slots over after cut_ns.
 * Tasks without slots yield as without ops.yield().
 */
bool BPF_STRUCT_OPS(pfs_yield, struct task_struct *from, struct task_struct *to)
{
	struct pfs_task_ctx *taskc;
	u64 mask;

	if (to)
		return false;

	taskc = bpf_task_storage_get(&task_ctx, from, 0, 0);
	mask = taskc ? taskc->slot_mask & SLOT_LEVELS_MASK : 0;
	if (mask) {
		bool wanted = slots_wanted(taskc, mask, false);

		if (wanted && window_now() - taskc->admitted_at >= cut_ns) {
			stat_inc(PFS_STAT_YIELD_ROTATE);
		} else if (wanted ||
			   (scx_bpf_dsq_nr_queued(SHARED_DSQ) <= 0 &&
			    scx_bpf_dsq_nr_queued(SCX_DSQ_LOCAL) <= 0)) {
			stat_inc(PFS_STAT_YIELD_KEEP);
			return false;
		}
		/*
		 * Nobody waits for the slots but admitted tasks wait for a
		 * CPU: yield it, ops.enqueue() takes the free slots again.
		 */
	}

	/*
	 * yield() is called from inside the kernel too, with locks held:
	 * netlink_broadcast_filtered() yields with RTNL held when a listener is
	 * congested, and gfs2 and jfs yield in kernel paths. The enqueue that
	 * follows this must therefore not park the task - everybody waiting for
	 * that lock, in any cgroup, would wait for this cgroup's slot queue.
	 * Mark it so reserve_slots() lets it run on credit instead, as it does
	 * for a task that woke up as the new owner of a lock. kPFS defers the
	 * same decision to the task's return to user space, which has no
	 * counterpart here.
	 */
	if (taskc)
		taskc->yielded = 1;

	from->scx.slice = 0;
	return false;
}

/*
 * Count @p in its cgroup's runnable weight. The cgroup and the weight are
 * remembered so that ops.quiescent() takes back exactly what was added, also
 * across a cgroup move, which is bracketed by ops.quiescent() and
 * ops.runnable() for a runnable task.
 */
void BPF_STRUCT_OPS(pfs_runnable, struct task_struct *p, u64 enq_flags)
{
	struct pfs_task_ctx *taskc;
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *cgrp;

	taskc = find_task_ctx(p);
	if (!taskc || taskc->run_cgid)
		return;

	cgrp = scx_bpf_task_cgroup(p);
	if (!weighted(cgrp))
		goto out_release;

	cgc = find_cgrp_ctx(cgrp);
	if (!cgc)
		goto out_release;

	taskc->run_cgid = cgrp->kn->id;
	taskc->run_weight = p->scx.weight;
	__sync_fetch_and_add(&cgc->runnable_weight, taskc->run_weight);
	if (!__sync_fetch_and_add(&cgc->nr_runnable, 1))
		update_active(cgrp, true);
out_release:
	bpf_cgroup_release(cgrp);
}

static void put_runnable(struct pfs_task_ctx *taskc)
{
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *cgrp;
	u64 cgid = taskc->run_cgid;

	if (!cgid)
		return;
	taskc->run_cgid = 0;

	cgrp = bpf_cgroup_from_id(cgid);
	if (!cgrp)
		return;

	cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
	if (cgc) {
		__sync_fetch_and_sub(&cgc->runnable_weight, taskc->run_weight);
		if (!__sync_sub_and_fetch(&cgc->nr_runnable, 1))
			update_active(cgrp, false);
	}
	bpf_cgroup_release(cgrp);
}

void BPF_STRUCT_OPS(pfs_quiescent, struct task_struct *p, u64 deq_flags)
{
	struct pfs_task_ctx *taskc;

	taskc = find_task_ctx(p);
	if (taskc) {
		wait_end(taskc, window_now());
		put_runnable(taskc);
	}
}

void BPF_STRUCT_OPS(pfs_enable, struct task_struct *p)
{
	scx_bpf_task_set_dsq_vtime(p, vtime_now);
}

void BPF_STRUCT_OPS(pfs_disable, struct task_struct *p)
{
	struct pfs_task_ctx *taskc;

	taskc = bpf_task_storage_get(&task_ctx, p, 0, 0);
	if (taskc) {
		release_slots(taskc);
		park_cancel(taskc);
		wait_end(taskc, window_now());
		put_runnable(taskc);
	}
}

s32 BPF_STRUCT_OPS(pfs_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	struct pfs_task_ctx *taskc;
	struct cgroup *cgrp;

	taskc = bpf_task_storage_get(&task_ctx, p, 0,
				     BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!taskc)
		return -ENOMEM;

	taskc->slot_mask = 0;
	taskc->credit = 0;
	taskc->parking_cgid = 0;
	taskc->wait_mask = 0;
	taskc->run_cgid = 0;
	taskc->cgid = 0;

	/*
	 * @args->cgroup can't be passed to kfuncs, look it up. An exiting
	 * task's cgroup may be gone already; ops.select_cpu() and
	 * ops.enqueue() set the path before the task takes any slot.
	 */
	cgrp = bpf_cgroup_from_id(args->cgroup->kn->id);
	if (!cgrp)
		return 0;
	set_task_cgrp(taskc, cgrp);
	bpf_cgroup_release(cgrp);
	return 0;
}

void BPF_STRUCT_OPS(pfs_cgroup_move, struct task_struct *p,
		    struct cgroup *from, struct cgroup *to)
{
	struct pfs_task_ctx *taskc;

	taskc = find_task_ctx(p);
	if (!taskc)
		return;

	/* the slots and the waits are on @from's path */
	release_slots(taskc);
	wait_end(taskc, window_now());
	set_task_cgrp(taskc, to);
}

/*
 * Set @cgc's limit to @limit milli-CPUs with a cpu.max period of @period_us.
 * A cgroup which becomes limited starts with a full bucket, as CFS starts
 * with the quota of a period; a change of the limit keeps the tokens or debt.
 */
static void set_limit(struct pfs_cgrp_ctx *cgc, u64 limit, u64 period_us)
{
	u64 depth;

	/* without wait DSQs the cgroup's tasks can't wait for slots */
	if (cgc->no_dsq)
		limit = 0;

	if (!cgc->limit && limit)
		__sync_fetch_and_add(&nr_limited, 1);
	else if (cgc->limit && !limit)
		__sync_fetch_and_sub(&nr_limited, 1);

	depth = limit ? bucket_depth(limit, period_us * 1000) : 0;
	if (!cgc->limit && limit) {
		cgc->refill_at = window_now();
		cgc->tokens = depth;
	}
	cgc->depth = depth;
	if (cgc->tokens > (s64)depth)
		cgc->tokens = depth;
	cgc->limit = limit;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(pfs_cgroup_init, struct cgroup *cgrp,
			     struct scx_cgroup_init_args *args)
{
	struct pfs_cgrp_ctx *cgc;
	u64 cgid = cgrp->kn->id;
	s32 ret;

	if (!cgrp->level)
		root_cgid = cgid;

	/*
	 * Technically incorrect as cgroup ID is full 64bit while dsq ID is
	 * 63bit. Should not be a problem in practice and easy to spot in the
	 * unlikely case that it breaks.
	 */
	ret = scx_bpf_create_dsq(cgid, -1);
	if (!ret) {
		ret = scx_bpf_create_dsq(cgid | WOKEN_DSQ, -1);
		if (ret)
			scx_bpf_destroy_dsq(cgid);
	}

	cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0,
				   BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!cgc) {
		if (!ret) {
			scx_bpf_destroy_dsq(cgid);
			scx_bpf_destroy_dsq(cgid | WOKEN_DSQ);
		}
		return -ENOMEM;
	}

	/*
	 * Failing the scheduler for one cgroup, which allocation failures
	 * would allow anyone who may create cgroups to do, is worse than not
	 * limiting that cgroup. Its limit is ignored, counted in no_dsq.
	 */
	if (ret) {
		stat_inc(PFS_STAT_NO_DSQ);
		cgc->no_dsq = 1;
	}

	cgc->weight = args->weight;
	cgc->hweight = HWEIGHT_ONE;
	set_limit(cgc, bw_to_limit(args->bw_period_us, args->bw_quota_us),
		  args->bw_period_us);
	return 0;
}

/*
 * Let the tasks still waiting in @dsq_id run on credit and destroy it. The
 * cgroup going offline only waits for its live tasks, an exiting task may
 * still be queued, and destroying a DSQ with tasks in it is a scheduler error.
 * A DSQ which isn't empty after the drain is left alone rather than destroyed.
 */
static void drain_destroy_dsq(u64 dsq_id)
{
	struct pfs_task_ctx *taskc;
	struct task_struct *p;

	bpf_for_each(scx_dsq, p, dsq_id, 0) {
		taskc = bpf_task_storage_get(&task_ctx, p, 0, 0);
		if (taskc) {
			taskc->parking_cgid = 0;
			set_admitted(taskc, window_now(),
				     !(dsq_id & WOKEN_DSQ));
		}
		if (scx_bpf_dsq_move_vtime(BPF_FOR_EACH_ITER, p, SHARED_DSQ,
					   0)) {
			stat_inc(PFS_STAT_DRAIN);
			kick_idle_cpu(p);
		}
	}

	if (scx_bpf_dsq_nr_queued(dsq_id) > 0) {
		stat_inc(PFS_STAT_DRAIN_FAIL);
		return;
	}
	scx_bpf_destroy_dsq(dsq_id);
}

void BPF_STRUCT_OPS(pfs_cgroup_exit, struct cgroup *cgrp)
{
	struct pfs_cgrp_ctx *cgc;
	u64 cgid = cgrp->kn->id;

	cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
	if (cgc)
		set_limit(cgc, 0, 0);

	if (!cgc || !cgc->no_dsq) {
		drain_destroy_dsq(cgid);
		drain_destroy_dsq(cgid | WOKEN_DSQ);
	}
	bpf_map_delete_elem(&waitset, &cgid);
}

void BPF_STRUCT_OPS(pfs_cgroup_set_bandwidth, struct cgroup *cgrp,
		    u64 period_us, u64 quota_us, u64 burst_us)
{
	struct pfs_cgrp_ctx *cgc;

	cgc = find_cgrp_ctx(cgrp);
	if (!cgc)
		return;

	set_limit(cgc, bw_to_limit(period_us, quota_us), period_us);

	/* the limit may have grown, give the waiting tasks a chance */
	if (nr_waiting(cgrp->kn->id))
		queue_retry(cgc, cgrp->kn->id);
}

void BPF_STRUCT_OPS(pfs_cgroup_set_weight, struct cgroup *cgrp, u32 weight)
{
	struct pfs_cgrp_ctx *cgc, *pcgc = NULL;
	u64 cgid;

	cgc = find_cgrp_ctx(cgrp);
	if (!cgc)
		return;

	if (cgrp->level) {
		pcgc = find_ancestor_ctx(cgrp, cgrp->level - 1, &cgid);
		if (!pcgc)
			return;
	}

	bpf_spin_lock(&weight_lock);
	if (pcgc && cgc->nr_active > 0)
		pcgc->child_weight_sum += (s64)weight - cgc->weight;
	cgc->weight = weight;
	bpf_spin_unlock(&weight_lock);

	/* expire the cached hweights so that the new weight propagates */
	__sync_fetch_and_add(&hweight_gen, 1);
}

/*
 * Sweep of the cgroups which had waiting tasks: forget the ones without, and
 * retry the ones whose DSQs no retry walked for RESCUE_NS. That should never
 * be needed, retries are queued whenever slots are given back or a task
 * parks, but a waiting task that is never retried stalls until the sched_ext
 * watchdog ejects the scheduler, and the counter tells if it happens.
 */
static long rescue_cb(struct bpf_map *map, u64 *key, u8 *val, u64 *now)
{
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *cgrp;
	u64 cgid = *key;

	cgrp = bpf_cgroup_from_id(cgid);
	if (!cgrp) {
		bpf_map_delete_elem(map, key);
		return 0;
	}

	cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
	if (!cgc) {
		bpf_map_delete_elem(map, key);
	} else if (!nr_waiting(cgid)) {
		bpf_map_delete_elem(map, key);
		cgc->in_waitset = 0;
		/* a task may have parked after the test, list it again */
		if (nr_waiting(cgid))
			waitset_add(cgc, cgid);
	} else {
		/* a cgroup whose tasks only wait doesn't charge runtime */
		count_window(cgc, *now, cgc->stat.nr_waiting > 0);
		if ((s64)(*now - cgc->retry_at) > (s64)RESCUE_NS &&
		    !cgc->retry_running) {
			stat_inc(PFS_STAT_RESCUE);
			cgc->retry_queued = 0;
			queue_retry(cgc, cgid);
		}
	}

	bpf_cgroup_release(cgrp);
	return 0;
}

/*
 * Find the cgroups marked by mark_lost() with a walk over all cgroups, which
 * is only needed after the maps overflowed, and list and retry them again. A
 * cgroup which still doesn't fit is marked again for the next window.
 */
static void rescue_lost(void)
{
	struct cgroup_subsys_state *pos;
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *root;
	u64 cgid;

	root = bpf_cgroup_from_id(root_cgid);
	if (!root)
		return;

	bpf_rcu_read_lock();
	bpf_for_each(css, pos, &root->self, BPF_CGROUP_ITER_DESCENDANTS_PRE) {
		cgc = bpf_cgrp_storage_get(&cgrp_ctx, pos->cgroup, 0, 0);
		if (!cgc || !cgc->lost)
			continue;

		cgc->lost = 0;
		cgid = pos->cgroup->kn->id;
		if (!nr_waiting(cgid))
			continue;

		stat_inc(PFS_STAT_RESCUE);
		waitset_add(cgc, cgid);
		queue_retry(cgc, cgid);
	}
	bpf_rcu_read_unlock();
	bpf_cgroup_release(root);
}

static int window_timerfn(void *map, int *key, struct bpf_timer *timer)
{
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *cgrp;
	u64 cgid, nr = nr_budget_q, now;
	u32 nr_nokick = 0;
	s32 nokick_pid = 0;
	int i, ret;

	/*
	 * Tokens accrue continuously, see refill(). Give the cgroups waiting
	 * for them another try every window. A cgroup which is still out of
	 * tokens queues itself again, and is only retried in the next window:
	 * the pass is limited to the entries queued before it started.
	 */
	bpf_for(i, 0, TIMER_BATCH) {
		if (i >= nr || bpf_map_pop_elem(&budget_q, &cgid))
			break;
		__sync_fetch_and_sub(&nr_budget_q, 1);

		cgrp = bpf_cgroup_from_id(cgid);
		if (!cgrp)
			continue;
		cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
		if (cgc)
			cgc->budget_queued = 0;
		bpf_cgroup_release(cgrp);

		retry_cgroup(cgid, &nr_nokick, &nokick_pid);
	}

	now = window_now();
	bpf_for_each_map_elem(&waitset, rescue_cb, &now, 0);

	if (nr_lost && __sync_lock_test_and_set(&nr_lost, 0))
		rescue_lost();

	/*
	 * Retries are normally processed by dispatch. Drain some here too so
	 * that they don't wait for a CPU to run out of work on a busy system.
	 */
	bpf_for(i, 0, RETRY_BATCH) {
		if (bpf_map_pop_elem(&retry_q, &cgid))
			break;
		retry_cgroup(cgid, &nr_nokick, &nokick_pid);
	}

	/* without the timer, cgroups waiting for tokens would stall */
	ret = bpf_timer_start(timer, window_ns - window_now() % window_ns, 0);
	if (ret)
		scx_bpf_error("bpf_timer_start failed (%d)", ret);
	return 0;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(pfs_init)
{
	struct bpf_timer *timer;
	u32 key = 0;
	s32 ret;

	ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (ret) {
		scx_bpf_error("failed to create DSQ %d (%d)", SHARED_DSQ, ret);
		return ret;
	}

	timer = bpf_map_lookup_elem(&window_timer, &key);
	if (!timer)
		return -ESRCH;

	bpf_timer_init(timer, &window_timer, CLOCK_MONOTONIC);
	bpf_timer_set_callback(timer, window_timerfn);
	ret = bpf_timer_start(timer, window_ns, 0);
	if (ret)
		scx_bpf_error("bpf_timer_start failed (%d)", ret);
	return ret;
}

/*
 * A CPU going on or offline. Nothing here has to be resynced: the slot
 * accounting is per cgroup (cgrp_ctx.nr_slots) and per task
 * (pfs_task_ctx.slot_mask), the wait DSQs are per cgroup, dispatch goes to
 * SCX_DSQ_LOCAL and the core moves a dying CPU's queue itself, the idle mask is
 * read live in pfs_select_cpu(), and the statistics map is a per-CPU array that
 * user space sums. Defining the two ops is the point: without them the core
 * ejects the scheduler on every hotplug event (handle_hotplug() reaches
 * scx_exit() when neither op is set), and the loader then has to reopen,
 * reload and reattach, which measured 565ms of BPF load per attempt with the
 * group's tasks on CFS meanwhile.
 *
 * min_share, which user space scales by the online CPU count at open, is left
 * as it was: it is a heuristic for how long a waiting task may be ahead, it is
 * rodata and cannot be written from here, and the count it was computed from is
 * the one the operator sized the scheduler for.
 */
void BPF_STRUCT_OPS(pfs_cpu_online, s32 cpu)
{
	stat_inc(PFS_STAT_HOTPLUG);
}

void BPF_STRUCT_OPS(pfs_cpu_offline, s32 cpu)
{
	stat_inc(PFS_STAT_HOTPLUG);
}

void BPF_STRUCT_OPS(pfs_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

/*
 * Without SCX_OPS_ENQ_LAST, a task whose slice expires with nothing else
 * queued for its CPU keeps running and never gives its slots back to the tasks
 * waiting for them.
 */
SCX_OPS_DEFINE(pfs_ops,
	       .select_cpu		= (void *)pfs_select_cpu,
	       .enqueue			= (void *)pfs_enqueue,
	       .dispatch		= (void *)pfs_dispatch,
	       .runnable		= (void *)pfs_runnable,
	       .running			= (void *)pfs_running,
	       .stopping		= (void *)pfs_stopping,
	       .tick			= (void *)pfs_tick,
	       .yield			= (void *)pfs_yield,
	       .quiescent		= (void *)pfs_quiescent,
	       .enable			= (void *)pfs_enable,
	       .disable			= (void *)pfs_disable,
	       .init_task		= (void *)pfs_init_task,
	       .cgroup_init		= (void *)pfs_cgroup_init,
	       .cgroup_exit		= (void *)pfs_cgroup_exit,
	       .cgroup_move		= (void *)pfs_cgroup_move,
	       .cgroup_set_weight	= (void *)pfs_cgroup_set_weight,
	       .cgroup_set_bandwidth	= (void *)pfs_cgroup_set_bandwidth,
	       .cpu_online		= (void *)pfs_cpu_online,
	       .cpu_offline		= (void *)pfs_cpu_offline,
	       .init			= (void *)pfs_init,
	       .exit			= (void *)pfs_exit,
	       .flags			= SCX_OPS_ENQ_EXITING | SCX_OPS_ENQ_LAST,
	       .name			= "pfs");
