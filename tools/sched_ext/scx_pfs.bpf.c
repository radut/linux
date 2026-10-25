/* SPDX-License-Identifier: GPL-2.0 */
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
 * A fractional limit such as 1.5 is enforced as floor(1.5) == 1 slot without
 * any time budget plus one extra slot which is rate limited. The cgroup's total
 * usage is accounted in short windows (-w, 10ms by default) and the extra slot
 * can only be taken while the usage in the current window is below
 * limit * window. Limits below one CPU only have the rate limited slot, so
 * they are still throttled, but over a window much shorter than the 100ms
 * default period of cpu.max.
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
 * Usage accounting for the rate limited slot is approximate: the slice of a
 * task admitted to it is clipped to the budget left at admission, but other
 * running tasks of the cgroup consume the same budget concurrently, so the
 * cgroup can overrun its budget by up to one slice per running task per
 * window. The overrun is carried into the next window. The budget is reset
 * lazily by the first user in a new window, and a charge racing against the
 * reset can land in the new window, which can delay the rate limited slot by
 * up to one window.
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
};

#define SLOT_LEVELS_MASK	0xffffffffLLU
#define SLOT_SEQ_SHIFT		32
#define MIN_BUDGET_NS		(100 * 1000LLU)
#define HWEIGHT_REFRESH_NS	(1000 * 1000LLU)

_Static_assert(MAX_LEVELS <= SLOT_SEQ_SHIFT, "level bits overlap the sequence");

enum reserve_result {
	RESERVE_OK,
	RESERVE_FULL,		/* all slots of a level are taken */
	RESERVE_BUDGET,		/* a level's rate limited slot is out of budget */
};

const volatile u64 slice_ns;
const volatile u64 window_ns;
const volatile u64 min_share = 1;	/* see scale_by_cgrp_share() */

static u64 vtime_now;

/* number of cgroups with a limit, scheduling is plain vtime while it's 0 */
static u64 nr_limited;

/* entries in budget_q, bounds the timer's pass to the ones queued before it */
static u64 nr_budget_q;

/* bumped when the active cgroups or a weight change, expires the hweights */
static u64 hweight_gen = 1;

/* protects ->nr_active and ->child_weight_sum of all cgroups */
private(PFS_WEIGHT) struct bpf_spin_lock weight_lock;

UEI_DEFINE(uei);

struct pfs_cgrp_ctx {
	u64		limit;		/* milli-CPUs from cpu.max, 0 if none */
	u32		nr_slots;	/* slots taken by tasks of the subtree */
	u32		retry_queued;
	u32		retry_running;	/* a retry_cgroup() walks the DSQ */
	u32		retry_walked;	/* tasks examined by the walker */
	u32		budget_queued;
	u64		window_id;
	u64		window_used;	/* runtime charged in window_id */

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
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct pfs_task_ctx);
} task_ctx SEC(".maps");

/* cgroups whose waiting tasks may be admittable now */
struct {
	__uint(type, BPF_MAP_TYPE_QUEUE);
	__uint(max_entries, 16384);
	__type(value, u64);
} retry_q SEC(".maps");

/* cgroups whose waiting tasks need the next window's budget */
struct {
	__uint(type, BPF_MAP_TYPE_QUEUE);
	__uint(max_entries, 16384);
	__type(value, u64);
} budget_q SEC(".maps");

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

static void queue_retry(struct pfs_cgrp_ctx *cgc, u64 cgid)
{
	/* paired with the clearing in retry_cgroup() */
	if (__sync_val_compare_and_swap(&cgc->retry_queued, 0, 1))
		return;

	/* a lost retry can stall the cgroup's waiting tasks forever */
	if (bpf_map_push_elem(&retry_q, &cgid, 0)) {
		cgc->retry_queued = 0;
		stat_inc(PFS_STAT_RETRY_OVERFLOW);
		scx_bpf_error("retry_q overflow for cgid %llu", cgid);
	}
}

static void queue_budget(struct pfs_cgrp_ctx *cgc, u64 cgid)
{
	if (__sync_val_compare_and_swap(&cgc->budget_queued, 0, 1))
		return;

	if (!bpf_map_push_elem(&budget_q, &cgid, 0)) {
		__sync_fetch_and_add(&nr_budget_q, 1);
	} else {
		cgc->budget_queued = 0;
		stat_inc(PFS_STAT_RETRY_OVERFLOW);
		scx_bpf_error("budget_q overflow for cgid %llu", cgid);
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

/*
 * Return the part of @cgc's budget left in the current window. Windows are
 * aligned to multiples of window_ns so that the periodic timer finds the
 * budgets of all cgroups refilled when it fires. The reset can race against
 * concurrent charging, which loses at most a few slices worth of charge.
 *
 * Slices end on a tick, so admissions overrun the budget. The overrun of a
 * window is carried into the next one, up to a full budget, so that the
 * average stays at the limit.
 */
static u64 window_left(struct pfs_cgrp_ctx *cgc, u64 now)
{
	u64 id = now / window_ns;
	u64 old_id = cgc->window_id;
	u64 budget, used;

	budget = cgc->limit * window_ns / FRAC_ONE;

	if (old_id != id &&
	    __sync_val_compare_and_swap(&cgc->window_id, old_id, id) == old_id) {
		used = cgc->window_used;
		if (id == old_id + 1 && used > budget)
			used = used - budget < budget ? used - budget : budget;
		else
			used = 0;
		cgc->window_used = used;
	}

	used = cgc->window_used;
	return used < budget ? budget - used : 0;
}

/*
 * Take a slot in a single limited cgroup. The first floor(limit) slots are
 * free. The last slot of a fractional limit needs budget left in the window and
 * clips *@slicep to it. Less than MIN_BUDGET_NS left counts as none to avoid
 * tiny slices, unless the whole budget of a window is smaller than that.
 */
static enum reserve_result take_slot(struct pfs_cgrp_ctx *cgc, u64 now,
				     u64 *slicep)
{
	u64 width = div_round_up(cgc->limit, FRAC_ONE);
	u64 full = cgc->limit / FRAC_ONE;
	u64 taken, left, min_left;

	taken = __sync_fetch_and_add(&cgc->nr_slots, 1);
	if (taken >= width) {
		__sync_fetch_and_sub(&cgc->nr_slots, 1);
		return RESERVE_FULL;
	}

	if (taken >= full) {
		left = window_left(cgc, now);
		min_left = cgc->limit * window_ns / FRAC_ONE;
		if (min_left > MIN_BUDGET_NS)
			min_left = MIN_BUDGET_NS;
		if (!left || left < min_left) {
			__sync_fetch_and_sub(&cgc->nr_slots, 1);
			return RESERVE_BUDGET;
		}
		if (left < *slicep)
			*slicep = left;
	}

	return RESERVE_OK;
}

/*
 * Give back the slots in @mask taken on @cgrp's path. All walks over the path
 * cover levels 1 to @cgrp->level, which is below MAX_LEVELS for a cgroup whose
 * tasks can hold slots.
 */
static void put_slots(struct cgroup *cgrp, u64 mask, bool kick_waiters)
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

		__sync_fetch_and_sub(&cgc->nr_slots, 1);
		if (kick_waiters && scx_bpf_dsq_nr_queued(cgid) > 0)
			queue_retry(cgc, cgid);
	}
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

		if (fifo && scx_bpf_dsq_nr_queued(cgid) > 0) {
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
		put_slots(cgrp, mask, false);
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
 * Give back the slots @p holds. The xchg makes sure that the slots are put
 * once when several paths try to release them concurrently.
 */
static void release_slots(struct pfs_task_ctx *taskc)
{
	struct cgroup *cgrp;
	u64 mask;

	mask = __sync_lock_test_and_set(&taskc->slot_mask, 0);
	if (!mask)
		return;

	cgrp = bpf_cgroup_from_id(taskc->cgid);
	if (!cgrp)
		return;

	put_slots(cgrp, mask & SLOT_LEVELS_MASK, true);
	bpf_cgroup_release(cgrp);
}

static void park(struct task_struct *p, u64 cgid, enum reserve_result ret,
		 u64 vtime, u64 enq_flags)
{
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *wait_cgrp;

	/*
	 * Waiting tasks are admitted in FIFO order, which bounds the wait to
	 * about one slice per task queued ahead divided by the width. In vtime
	 * order, the tick-granular run lengths make the vtimes of the waiting
	 * tasks drift apart and a task which got ahead waits several turns.
	 * @vtime is kept for the shared DSQ. The preemption flags imply
	 * SCX_ENQ_HEAD and are cleared with it.
	 */
	scx_bpf_task_set_dsq_vtime(p, vtime);
	scx_bpf_dsq_insert(p, cgid, slice_ns,
			   enq_flags & ~(SCX_ENQ_HEAD | SCX_ENQ_PREEMPT |
					 SCX_ENQ_PREEMPT_LAZY));

	wait_cgrp = bpf_cgroup_from_id(cgid);
	if (!wait_cgrp)
		return;

	cgc = find_cgrp_ctx(wait_cgrp);
	if (cgc) {
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

/*
 * Try to admit the tasks waiting in @cgid's DSQ. A task which can't be
 * admitted because another level of its path refuses is moved to that level's
 * DSQ. Stops at the first task refused by @cgid itself.
 *
 * The first *@nr_nokick admitted tasks which can run on this CPU don't kick an
 * idle CPU, the calling dispatch picks them up itself. *@nokick_pid is set to
 * the pid of the last such task so that dispatch can kick for it if it takes
 * another task instead.
 *
 * Only one walker scans a DSQ at a time so that two walkers can't reserve
 * slots for the same task. A retry which finds the DSQ busy, or which stops
 * after RETRY_WALK_MAX tasks with more left, is queued again so that it isn't
 * lost.
 */
static void retry_cgroup(u64 cgid, u32 *nr_nokick, s32 *nokick_pid)
{
	enum reserve_result ret = RESERVE_OK;
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *cgrp;
	struct task_struct *p;
	bool requeue = false;

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

	/*
	 * The count is kept in @cgc, which is owned by the walker, instead of
	 * on the stack. The verifier doesn't know its value, so it doesn't
	 * have to walk the loop once for each possible count.
	 */
	cgc->retry_walked = 0;

	bpf_for_each(scx_dsq, p, cgid, 0) {
		struct pfs_task_ctx *taskc;
		struct cgroup *task_cgrp;
		u64 slice = slice_ns, mask, val, fail_cgid = 0;
		u32 walked;

		walked = READ_ONCE(cgc->retry_walked);
		if (walked >= RETRY_WALK_MAX) {
			requeue = true;
			break;
		}
		WRITE_ONCE(cgc->retry_walked, walked + 1);

		taskc = bpf_task_storage_get(&task_ctx, p, 0, 0);
		if (!taskc)
			break;

		/* the slots are put through @task_cgrp, keep it until done */
		task_cgrp = bpf_cgroup_from_id(taskc->cgid);
		if (!task_cgrp)
			continue;

		ret = reserve_slots(task_cgrp, false, &slice, &mask, &fail_cgid);
		if (ret == RESERVE_OK) {
			/* publish before @p can run and give the slots back */
			if (!publish_slots(taskc, mask, &val)) {
				stat_inc(PFS_STAT_MOVE_RACE);
				put_slots(task_cgrp, mask, true);
				bpf_cgroup_release(task_cgrp);
				continue;
			}

			scx_bpf_dsq_move_set_slice(BPF_FOR_EACH_ITER, slice);
			if (scx_bpf_dsq_move_vtime(BPF_FOR_EACH_ITER, p,
						   SHARED_DSQ, 0)) {
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
					put_slots(task_cgrp, mask, true);
			}
			bpf_cgroup_release(task_cgrp);
			continue;
		}
		bpf_cgroup_release(task_cgrp);

		if (fail_cgid == cgid)
			break;

		/* refused by another level, wait there instead */
		if (scx_bpf_dsq_move(BPF_FOR_EACH_ITER, p, fail_cgid, 0)) {
			struct pfs_cgrp_ctx *fail_cgc;
			struct cgroup *fail_cgrp;

			stat_inc(PFS_STAT_REPARK);
			fail_cgrp = bpf_cgroup_from_id(fail_cgid);
			if (!fail_cgrp)
				continue;
			fail_cgc = bpf_cgrp_storage_get(&cgrp_ctx, fail_cgrp, 0, 0);
			if (fail_cgc) {
				if (ret == RESERVE_BUDGET)
					queue_budget(fail_cgc, fail_cgid);
				else
					queue_retry(fail_cgc, fail_cgid);
			}
			bpf_cgroup_release(fail_cgrp);
		}
		ret = RESERVE_OK;
	}

	/* paired with the cmpxchg above, fully ordered */
	__sync_lock_test_and_set(&cgc->retry_running, 0);

	if (requeue)
		queue_retry(cgc, cgid);
	if (ret == RESERVE_BUDGET)
		queue_budget(cgc, cgid);

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
	if (!taskc)
		return cpu;

	/* a waking task holds no slot, the target is only taken if one is free */
	cgrp = scx_bpf_task_cgroup(p);
	taskc->cgid = cgrp->kn->id;
	if (reserve_slots(cgrp, true, &slice, &mask, &fail_cgid) == RESERVE_OK) {
		if (publish_slots(taskc, mask, &val)) {
			stat_inc(PFS_STAT_DIRECT);
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, 0);
		} else {
			/* @p still holds slots, enqueue releases them */
			put_slots(cgrp, mask, true);
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
	 * for an attribute change. Start over.
	 */
	release_slots(taskc);

	/* release_slots() and retries find the slots through ->cgid */
	cgrp = scx_bpf_task_cgroup(p);
	taskc->cgid = cgrp->kn->id;
	vtime = clamp_vtime(p, cgrp);

	ret = reserve_slots(cgrp, true, &slice, &mask, &fail_cgid);
	if (ret == RESERVE_OK && !publish_slots(taskc, mask, &val)) {
		/*
		 * A retry which saw @p before it was dequeued published slots
		 * for it after the release above. The retry's move fails and
		 * takes them back. Wait in @p's own cgroup for another retry.
		 */
		stat_inc(PFS_STAT_MOVE_RACE);
		put_slots(cgrp, mask, true);
		ret = RESERVE_FULL;
		fail_cgid = taskc->cgid;
	}

	if (ret == RESERVE_OK) {
		stat_inc(PFS_STAT_SHARED);
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
		park(p, fail_cgid, ret, vtime, enq_flags);
		/* run a dispatch cycle on the CPU to admit a waiting task */
		if (enq_flags & SCX_ENQ_LAST)
			scx_bpf_kick_cpu(scx_bpf_task_cpu(p), 0);
	}
	bpf_cgroup_release(cgrp);
}

/* charge @delta of runtime to the windows of the limited cgroups on the path */
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

		window_left(cgc, now);
		__sync_fetch_and_add(&cgc->window_used, delta);
	}
}

/*
 * Charge @p's runtime since it started running, or since it was last charged,
 * to its vtime, scaled by its weight and @cgrp's share, and to the windows on
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
		put_slots(cgrp, mask, true);
		goto out_release;
	}

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
	 * Charge @prev's runtime first, otherwise the retries see the window
	 * usage from before it ran and can admit a task to a rate limited slot
	 * whose budget @prev just used up. ops.stopping() charges the rest.
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
}

void BPF_STRUCT_OPS(pfs_running, struct task_struct *p)
{
	struct pfs_task_ctx *taskc;

	taskc = find_task_ctx(p);
	if (taskc)
		taskc->started_at = window_now();

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
	if (taskc)
		put_runnable(taskc);
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
		put_runnable(taskc);
	}
}

s32 BPF_STRUCT_OPS(pfs_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	struct pfs_task_ctx *taskc;

	taskc = bpf_task_storage_get(&task_ctx, p, 0,
				     BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!taskc)
		return -ENOMEM;

	taskc->cgid = args->cgroup->kn->id;
	taskc->slot_mask = 0;
	taskc->run_cgid = 0;
	return 0;
}

void BPF_STRUCT_OPS(pfs_cgroup_move, struct task_struct *p,
		    struct cgroup *from, struct cgroup *to)
{
	struct pfs_task_ctx *taskc;

	taskc = find_task_ctx(p);
	if (!taskc)
		return;

	/* the slots are on @from's path */
	release_slots(taskc);
	taskc->cgid = to->kn->id;
}

static void set_limit(struct pfs_cgrp_ctx *cgc, u64 limit)
{
	if (!cgc->limit && limit)
		__sync_fetch_and_add(&nr_limited, 1);
	else if (cgc->limit && !limit)
		__sync_fetch_and_sub(&nr_limited, 1);

	cgc->limit = limit;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(pfs_cgroup_init, struct cgroup *cgrp,
			     struct scx_cgroup_init_args *args)
{
	struct pfs_cgrp_ctx *cgc;
	u64 cgid = cgrp->kn->id;
	s32 ret;

	/*
	 * Technically incorrect as cgroup ID is full 64bit while dsq ID is
	 * 63bit. Should not be a problem in practice and easy to spot in the
	 * unlikely case that it breaks.
	 */
	ret = scx_bpf_create_dsq(cgid, -1);
	if (ret) {
		scx_bpf_error("scx_bpf_create_dsq failed (%d)", ret);
		return ret;
	}

	cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0,
				   BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!cgc) {
		scx_bpf_destroy_dsq(cgid);
		return -ENOMEM;
	}

	cgc->weight = args->weight;
	cgc->hweight = HWEIGHT_ONE;
	set_limit(cgc, bw_to_limit(args->bw_period_us, args->bw_quota_us));
	return 0;
}

void BPF_STRUCT_OPS(pfs_cgroup_exit, struct cgroup *cgrp)
{
	struct pfs_cgrp_ctx *cgc;

	cgc = bpf_cgrp_storage_get(&cgrp_ctx, cgrp, 0, 0);
	if (cgc)
		set_limit(cgc, 0);

	scx_bpf_destroy_dsq(cgrp->kn->id);
}

void BPF_STRUCT_OPS(pfs_cgroup_set_bandwidth, struct cgroup *cgrp,
		    u64 period_us, u64 quota_us, u64 burst_us)
{
	struct pfs_cgrp_ctx *cgc;

	cgc = find_cgrp_ctx(cgrp);
	if (!cgc)
		return;

	set_limit(cgc, bw_to_limit(period_us, quota_us));

	/* the limit may have grown, give the waiting tasks a chance */
	if (scx_bpf_dsq_nr_queued(cgrp->kn->id) > 0)
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

static int window_timerfn(void *map, int *key, struct bpf_timer *timer)
{
	struct pfs_cgrp_ctx *cgc;
	struct cgroup *cgrp;
	u64 cgid, nr = nr_budget_q;
	u32 nr_nokick = 0;
	s32 nokick_pid = 0;
	int i, ret;

	/*
	 * Budgets are refilled at window boundaries. Give the cgroups waiting
	 * for budget another try. A cgroup which is still out of budget queues
	 * itself again, and is only retried in the next window: the pass is
	 * limited to the entries queued before it started.
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

	/*
	 * Retries are normally processed by dispatch. Drain some here too so
	 * that they don't wait for a CPU to run out of work on a busy system.
	 */
	bpf_for(i, 0, RETRY_BATCH) {
		if (bpf_map_pop_elem(&retry_q, &cgid))
			break;
		retry_cgroup(cgid, &nr_nokick, &nokick_pid);
	}

	/* without the timer, cgroups waiting for budget would stall */
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
	       .quiescent		= (void *)pfs_quiescent,
	       .enable			= (void *)pfs_enable,
	       .disable			= (void *)pfs_disable,
	       .init_task		= (void *)pfs_init_task,
	       .cgroup_init		= (void *)pfs_cgroup_init,
	       .cgroup_exit		= (void *)pfs_cgroup_exit,
	       .cgroup_move		= (void *)pfs_cgroup_move,
	       .cgroup_set_weight	= (void *)pfs_cgroup_set_weight,
	       .cgroup_set_bandwidth	= (void *)pfs_cgroup_set_bandwidth,
	       .init			= (void *)pfs_init,
	       .exit			= (void *)pfs_exit,
	       .flags			= SCX_OPS_ENQ_EXITING | SCX_OPS_ENQ_LAST,
	       .name			= "pfs");
