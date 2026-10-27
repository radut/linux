// SPDX-License-Identifier: GPL-2.0
/*
 * Parallel Fair Scheduler (PFS) Class (SCHED_NORMAL/SCHED_BATCH/SCHED_IDLE)
 *
 * A scheduling class which replaces the fair class when the kernel is booted
 * with sched_pfs=1. Per CPU it schedules tasks by EEVDF like the fair class;
 * the EEVDF code below is derived from kernel/sched/fair.c. The difference is
 * in how cgroup CPU limits are enforced: cpu.max caps the number of threads
 * of a group that run at once, threads beyond the limit wait in a queue of
 * the group, and a freed slot is handed to the next waiting thread on the CPU
 * that freed it.
 *
 * See Documentation/scheduler/sched-pfs.rst.
 */
#include <linux/rbtree_augmented.h>
#include <linux/sched/clock.h>
#include <linux/sched/cputime.h>
#include <linux/sched/isolation.h>
#include <linux/sched/nohz.h>
#include <linux/sched/prio.h>
#include <linux/cpumask_api.h>
#include <linux/spinlock_api.h>
#include <linux/lockdep_api.h>
#include <linux/hrtimer.h>
#include <linux/irq_work.h>
#include <linux/topology.h>

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/task_work.h>
#include <linux/oom.h>
#include <asm/irq_regs.h>

#include <uapi/linux/sched/types.h>

#include "sched.h"
#include "stats.h"
#include "autogroup.h"
#include "pelt.h"

DEFINE_STATIC_KEY_FALSE(__pfs_enabled);

static int __init setup_sched_pfs(char *str)
{
	bool on = true;

	if (str && kstrtobool(str, &on))
		return -EINVAL;
	if (on)
		static_branch_enable(&__pfs_enabled);
	return 0;
}
early_param("sched_pfs", setup_sched_pfs);

/*
 * Behaviour switches, /sys/kernel/debug/sched/pfs/features, for comparing
 * variants within one boot.
 */
enum {
	PFS_F_WAKE_AFFINE	= 1 << 0,	/* fair's wakeup placement */
	PFS_F_BALANCE		= 1 << 1,	/* periodic balance */
	PFS_F_SLEEP_LAG		= 1 << 2,	/* negative lag served while asleep */
	PFS_F_PROTECT		= 1 << 3,	/* fair's slice protection updates */
	PFS_F_BALANCE_WEIGHT	= 1 << 4,	/* balance unequal weights */
	PFS_F_BALANCE_COUNT	= 1 << 5,	/* balance task counts */
	PFS_F_YIELD_KEEP	= 1 << 6,	/* keep slots on an early yield */
	PFS_F_WAKE_WAKER	= 1 << 7,	/* waking CPU before a busier prev */
	PFS_F_GRAN_SYNC		= 1 << 8,	/* wakeup granularity: sync only */
	PFS_F_HRTICK		= 1 << 9,	/* end requests with the hrtick */
};

static unsigned int pfs_features __read_mostly =
	PFS_F_BALANCE | PFS_F_SLEEP_LAG | PFS_F_PROTECT | PFS_F_BALANCE_WEIGHT |
	PFS_F_BALANCE_COUNT | PFS_F_YIELD_KEEP | PFS_F_WAKE_WAKER |
	PFS_F_GRAN_SYNC | PFS_F_HRTICK;

static inline bool pfs_feat(unsigned int f)
{
	return READ_ONCE(pfs_features) & f;
}

enum pfs_state {
	PFS_NONE,	/* not runnable, or not on a PFS run queue */
	PFS_QUEUED,	/* admitted: on the CPU tree, or running */
	PFS_WAITING,	/* waiting for a slot in pfs_group::waiters */
	PFS_PARKING,	/* running task which gave its slots back */
};

enum pfs_stat {
	PFS_STAT_ADMIT,		/* slots taken for a task */
	PFS_STAT_PARK_FULL,	/* waits: no free slot */
	PFS_STAT_PARK_BUDGET,	/* waits: no budget */
	PFS_STAT_PARK_FIFO,	/* waits: others wait already */
	PFS_STAT_PARK_DEBT,	/* slot refused: the group ran beyond its limit */
	PFS_STAT_ROTATE,	/* gave its slots back at the end of a slice */
	PFS_STAT_HANDOFF,	/* waiting task pulled to the CPU that freed slots */
	PFS_STAT_HANDOFF_LOCAL,	/* ... which was already on that CPU */
	PFS_STAT_INPLACE,	/* waiting task admitted on its own CPU */
	PFS_STAT_RETRY,		/* retry runs */
	PFS_STAT_PULL,		/* newly idle pulls */
	PFS_STAT_PUSH,		/* pushes */
	PFS_STAT_KICK,		/* idle CPUs kicked */
	PFS_STAT_CREDIT,	/* admitted without slots, in the kernel */
	PFS_STAT_USER_PARK,	/* parked or rotated on the way to user space */
	PFS_STAT_YIELD,		/* yielded: gives its slots back at user return */
	PFS_STAT_BALANCE,	/* tasks moved by the periodic balancer */
	PFS_STAT_SLOT_NS,	/* runtime of tasks holding slots, ns */
	PFS_STAT_CREDIT_NS,	/* runtime of tasks of limited groups on credit, ns */
	PFS_NR_STATS,
};

static const char * const pfs_stat_names[PFS_NR_STATS] = {
	"admit", "park_full", "park_budget", "park_fifo", "park_debt", "rotate", "handoff",
	"handoff_local", "inplace", "retry", "pull", "push", "kick", "credit",
	"user_park", "yield", "balance", "slot_ns", "credit_ns",
};

static DEFINE_PER_CPU(unsigned long [PFS_NR_STATS], pfs_stats);

static inline void pfs_stat(enum pfs_stat idx)
{
	this_cpu_inc(pfs_stats[idx]);
}

static inline void pfs_stat_add(enum pfs_stat idx, u64 val)
{
	this_cpu_add(pfs_stats[idx], val);
}

/* groups with a limit; while there are none, admission is a no-op */
static atomic_t pfs_nr_limited;

/* earliest interval between two pushes from the same CPU */
#define PFS_PUSH_INTERVAL	1	/* jiffies */
/* tasks examined when looking for one to migrate */
#define PFS_MIGRATE_SCAN	16

static inline struct task_struct *pfs_task_of(struct sched_pfs_entity *pe)
{
	return container_of(pe, struct task_struct, pfs);
}

#define __node_2_pe(node) \
	rb_entry((node), struct sched_pfs_entity, run_node)

static inline u64 pfs_task_slice(struct task_struct *p)
{
	return p->se.custom_slice ? p->se.slice : sysctl_sched_base_slice;
}

/**************************************************************
 * Virtual time, derived from the EEVDF code in fair.c.
 */

/* delta / w, in units of NICE_0_LOAD */
static inline u64 pfs_calc_delta(u64 delta, struct sched_pfs_entity *pe)
{
	unsigned long w = pe->weight.weight;

	if (unlikely(w != NICE_0_LOAD))
		delta = mul_u64_u64_div_u64(delta, NICE_0_LOAD, w);
	return delta;
}

static inline unsigned long pfs_avg_weight(struct sched_pfs_entity *pe)
{
	return max(scale_load_down(pe->weight.weight), 2UL);
}

static inline bool pfs_vruntime_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

static inline bool pfs_entity_before(struct sched_pfs_entity *a,
				     struct sched_pfs_entity *b)
{
	return pfs_vruntime_before(a->deadline, b->deadline);
}

static inline s64 pfs_key(struct pfs_rq *prq, struct sched_pfs_entity *pe)
{
	return (s64)(pe->vruntime - prq->zero_vruntime);
}

/* The running task's entity if it is admitted, it is not in the tree. */
static inline struct sched_pfs_entity *pfs_curr_entity(struct pfs_rq *prq)
{
	struct task_struct *curr = prq->curr;

	if (curr && curr->pfs.state == PFS_QUEUED)
		return &curr->pfs;
	return NULL;
}

/*
 * As in fair.c, the weighted average of the virtual runtimes of all admitted
 * entities is tracked relative to zero_vruntime:
 *
 *   V = \Sum (v_i - v0) * w_i / \Sum w_i + v0
 */
static void pfs_sum_add(struct pfs_rq *prq, struct sched_pfs_entity *pe)
{
	unsigned long w = pfs_avg_weight(pe);

	prq->sum_w_vruntime += pfs_key(prq, pe) * w;
	prq->sum_weight += w;
}

static void pfs_sum_sub(struct pfs_rq *prq, struct sched_pfs_entity *pe)
{
	unsigned long w = pfs_avg_weight(pe);

	prq->sum_w_vruntime -= pfs_key(prq, pe) * w;
	prq->sum_weight -= w;
}

static inline void pfs_update_zero_vruntime(struct pfs_rq *prq, s64 delta)
{
	prq->sum_w_vruntime -= prq->sum_weight * delta;
	prq->zero_vruntime += delta;
}

static u64 pfs_avg_vruntime(struct pfs_rq *prq)
{
	struct sched_pfs_entity *curr = pfs_curr_entity(prq);
	long weight = prq->sum_weight;
	s64 delta = 0;

	if (weight) {
		s64 runtime = prq->sum_w_vruntime;

		if (curr) {
			unsigned long w = pfs_avg_weight(curr);

			runtime += pfs_key(prq, curr) * w;
			weight += w;
		}

		/* sign flips effective floor / ceiling */
		if (runtime < 0)
			runtime -= (weight - 1);

		delta = div64_long(runtime, weight);
	} else if (curr) {
		delta = curr->vruntime - prq->zero_vruntime;
	}

	pfs_update_zero_vruntime(prq, delta);

	return prq->zero_vruntime;
}

/* An entity is eligible when it is owed service: V >= v_i. */
static bool pfs_vruntime_eligible(struct pfs_rq *prq, u64 vruntime)
{
	struct sched_pfs_entity *curr = pfs_curr_entity(prq);
	s64 avg = prq->sum_w_vruntime;
	long load = prq->sum_weight;
	s64 key;

	if (curr) {
		unsigned long w = pfs_avg_weight(curr);

		avg += pfs_key(prq, curr) * w;
		load += w;
	}

	key = (s64)(vruntime - prq->zero_vruntime);

#ifdef CONFIG_ARCH_SUPPORTS_INT128
	return avg >= (__int128)key * load;
#else
	{
		s64 rhs;

		if (check_mul_overflow(key, (s64)load, &rhs))
			return key <= 0;
		return avg >= rhs;
	}
#endif
}

static inline bool pfs_entity_eligible(struct pfs_rq *prq,
				       struct sched_pfs_entity *pe)
{
	return pfs_vruntime_eligible(prq, pe->vruntime);
}

/* lag_i = w_i * (V - v_i), clamped to about a slice */
static void pfs_update_entity_lag(struct pfs_rq *prq,
				  struct sched_pfs_entity *pe)
{
	s64 vlag, limit;

	vlag = pfs_avg_vruntime(prq) - pe->vruntime;
	limit = pfs_calc_delta(max_t(u64, 2 * pe->slice, TICK_NSEC), pe);
	pe->vlag = clamp(vlag, -limit, limit);
}

static inline u64 pfs_min_vruntime(u64 a, u64 b)
{
	return pfs_vruntime_before(a, b) ? a : b;
}

static inline bool __pfs_entity_less(struct rb_node *a, const struct rb_node *b)
{
	return pfs_entity_before(__node_2_pe(a), __node_2_pe((struct rb_node *)b));
}

RB_DECLARE_CALLBACKS(static, pfs_min_vruntime_cb,
		     struct sched_pfs_entity, run_node,
		     RB_AUG(vruntime, min_vruntime, pfs_min_vruntime));

static void __pfs_enqueue_entity(struct pfs_rq *prq,
				 struct sched_pfs_entity *pe)
{
	WARN_ON_ONCE(pe->on_tree);
	pfs_sum_add(prq, pe);
	pe->min_vruntime = pe->vruntime;
	rb_add_augmented_cached(&pe->run_node, &prq->tasks_timeline,
				__pfs_entity_less, &pfs_min_vruntime_cb);
	pe->on_tree = 1;
}

static void __pfs_dequeue_entity(struct pfs_rq *prq,
				 struct sched_pfs_entity *pe)
{
	WARN_ON_ONCE(!pe->on_tree);
	rb_erase_augmented_cached(&pe->run_node, &prq->tasks_timeline,
				  &pfs_min_vruntime_cb);
	pfs_sum_sub(prq, pe);
	pe->on_tree = 0;
}

static struct sched_pfs_entity *__pfs_pick_first(struct pfs_rq *prq)
{
	struct rb_node *left = rb_first_cached(&prq->tasks_timeline);

	return left ? __node_2_pe(left) : NULL;
}

/*
 * Place an entity which joins the competition, preserving its lag as in
 * fair.c's place_entity(): the lag is inflated by the entity's own weight so
 * that it comes out right after the entity is added to the average.
 */
static void pfs_place_entity(struct pfs_rq *prq, struct sched_pfs_entity *pe,
			     int flags)
{
	struct sched_pfs_entity *curr = pfs_curr_entity(prq);
	u64 vslice, vruntime = pfs_avg_vruntime(prq);
	bool update_zero = false;
	s64 lag = 0;

	pe->slice = pfs_task_slice(pfs_task_of(pe));
	vslice = pfs_calc_delta(pe->slice, pe);

	if ((prq->sum_weight || curr) && pe->vlag) {
		long load, weight;

		lag = pe->vlag;

		load = prq->sum_weight;
		if (curr)
			load += pfs_avg_weight(curr);
		if (WARN_ON_ONCE(!load))
			load = 1;

		weight = pfs_avg_weight(pe);
		lag += div64_long(lag * weight, load);
		if (weight > load)
			update_zero = true;
	}

	pe->vruntime = vruntime - lag;

	if (update_zero)
		pfs_update_zero_vruntime(prq, -lag);

	if (pe->rel_deadline) {
		pe->deadline += pe->vruntime;
		pe->rel_deadline = 0;
		return;
	}

	/* new tasks start halfway through their first slice */
	if (flags & ENQUEUE_INITIAL)
		vslice /= 2;

	pe->deadline = pe->vruntime + vslice;
}

/* Start a new request once the current one is used up. */
static bool pfs_update_deadline(struct pfs_rq *prq, struct sched_pfs_entity *pe)
{
	if (pfs_vruntime_before(pe->vruntime, pe->deadline))
		return false;

	pe->slice = pfs_task_slice(pfs_task_of(pe));
	pe->deadline = pe->vruntime + pfs_calc_delta(pe->slice, pe);
	pfs_avg_vruntime(prq);
	return true;
}

/*
 * Earliest Eligible Virtual Deadline First, see pick_eevdf() in fair.c. The
 * tree is ordered by deadline and augmented with the minimum vruntime of
 * each subtree, which prunes subtrees without eligible entities.
 */
/*
 * As with RUN_TO_PARITY in fair.c, the running task keeps the CPU against
 * wakeups until it used up its request (vprot is set to its deadline when it
 * is picked), unless a task with a shorter slice wakes up.
 */
static inline bool pfs_protected(struct sched_pfs_entity *pe)
{
	return pfs_vruntime_before(pe->vruntime, pe->vprot);
}

static struct sched_pfs_entity *pfs_pick_eevdf(struct pfs_rq *prq, bool protect)
{
	struct rb_node *node = prq->tasks_timeline.rb_root.rb_node;
	struct sched_pfs_entity *pe = __pfs_pick_first(prq);
	struct sched_pfs_entity *curr = pfs_curr_entity(prq);
	struct sched_pfs_entity *best = NULL;

	if (!pe)
		return curr;
	if (!curr && prq->nr_queued == 1)
		return pe;

	if (curr && !pfs_entity_eligible(prq, curr))
		curr = NULL;

	if (curr && protect && pfs_protected(curr))
		return curr;

	if (pfs_entity_eligible(prq, pe)) {
		best = pe;
		goto found;
	}

	while (node) {
		struct rb_node *left = node->rb_left;

		if (left && pfs_vruntime_eligible(prq,
					__node_2_pe(left)->min_vruntime)) {
			node = left;
			continue;
		}

		pe = __node_2_pe(node);
		if (pfs_entity_eligible(prq, pe)) {
			best = pe;
			break;
		}
		node = node->rb_right;
	}
found:
	if (!best || (curr && pfs_entity_before(curr, best)))
		best = curr;
	/* rounding can leave nothing eligible, don't idle because of it */
	if (!best)
		best = __pfs_pick_first(prq);
	return best;
}

static int pfs_find_idle_cpu(struct task_struct *p, int target);

/*
 * CPUs which found nothing to run in their last PFS pick, a hint that lets
 * wakeups and pushes skip the idle CPU scan on an overloaded machine.
 */
static atomic_t pfs_idle_cpus;

static inline void pfs_set_idle_hint(struct pfs_rq *prq, unsigned int idle)
{
	if (prq->idle_hint == idle)
		return;
	prq->idle_hint = idle;
	if (idle)
		atomic_inc(&pfs_idle_cpus);
	else
		atomic_dec(&pfs_idle_cpus);
}

/**************************************************************
 * Groups and slots.
 */

/* see sched-pfs.rst: window of the budget slot, debugfs window_us */
static u64 pfs_window_ns = 10 * NSEC_PER_MSEC;
/* less budget than this left counts as none (unless the budget is smaller) */
#define PFS_MIN_BUDGET_NS	(100 * NSEC_PER_USEC)
/* waiting tasks examined per hand-off and per retry */
#define PFS_HANDOFF_TRIES	8
#define PFS_RETRY_MAX		32
/* levels which can hold a slot, see sched_pfs_entity::slot_mask */
#define PFS_MAX_DEPTH		63

enum pfs_reserve {
	PFS_RES_OK,
	PFS_RES_FULL,		/* all slots of a level are taken */
	PFS_RES_BUDGET,		/* the budget slot has no budget left */
	PFS_RES_FIFO,		/* others wait at the level */
	PFS_RES_DEBT,		/* the level ran beyond its limit, see pfs_over_limit() */
};

static inline struct task_group *pfs_tg(struct pfs_group *g)
{
	return container_of(g, struct task_group, pfs);
}

static inline struct pfs_group *pfs_parent(struct pfs_group *g)
{
	struct task_group *parent = pfs_tg(g)->parent;

	return parent ? &parent->pfs : NULL;
}

static inline struct pfs_group *task_pfs_group(struct task_struct *p)
{
	return &p->sched_task_group->pfs;
}

/* walk @g and its ancestors which can hold slots: not the root */
#define for_each_pfs_level(lg, g) \
	for (lg = (g); lg && lg->depth > 0; lg = pfs_parent(lg))

static inline bool pfs_level_limited(struct pfs_group *lg)
{
	return READ_ONCE(lg->limit) && lg->depth <= PFS_MAX_DEPTH;
}

static inline unsigned int pfs_width(unsigned int limit)
{
	return DIV_ROUND_UP(limit, 1000);
}

static inline u64 pfs_now(void)
{
	return ktime_get_mono_fast_ns();
}

/* the window length, read once per use: debugfs may change it */
static inline u64 pfs_window(void)
{
	return READ_ONCE(pfs_window_ns);
}

/* the start of the window of length @w that @now is in */
static inline u64 pfs_window_start(u64 now, u64 w)
{
	u64 rem;

	div64_u64_rem(now, w, &rem);
	return now - rem;
}

static inline u64 pfs_budget(unsigned int limit, u64 w)
{
	return div_u64((u64)limit * w, 1000);
}

/* time over which runtime beyond the limit is paid back at most */
#define PFS_MAX_DEBT_NS		NSEC_PER_SEC

/*
 * The part of @lg's budget left in the current window. Windows are aligned
 * to multiples of the window length so that one timer per group finds the
 * budget refilled. Usage beyond the budget (slices end on a tick, tasks run
 * on credit in the kernel) is carried over as debt and paid back with the
 * budgets of the following windows, so that the average stays at the limit
 * even when the budget of a window is smaller than a tick. The debt is
 * capped at what the limit allows in PFS_MAX_DEBT_NS, or two ticks, which
 * bounds the wait after an overrun to about PFS_MAX_DEBT_NS; runtime on
 * credit beyond that is only possible on otherwise idle CPUs, see
 * pfs_effective_weight().
 */
static u64 pfs_window_left(struct pfs_group *lg, u64 now)
{
	u64 w = pfs_window(), start = pfs_window_start(now, w);
	s64 old = atomic64_read(&lg->window_start);
	u64 budget = pfs_budget(READ_ONCE(lg->limit), w);
	u64 used, elapsed, debt_max;

	if ((u64)old != start &&
	    atomic64_try_cmpxchg(&lg->window_start, &old, start)) {
		used = atomic64_read(&lg->window_used);
		/*
		 * Windows are tracked by their start time, so that a change
		 * of the window length doesn't wipe the usage: after the
		 * window was made longer, the new window may start before the
		 * old one, and nothing has elapsed.
		 */
		elapsed = (s64)(start - (u64)old) > 0 ?
			  div64_u64(start - (u64)old, w) : 0;
		debt_max = max_t(u64, div_u64((u64)READ_ONCE(lg->limit) *
					      PFS_MAX_DEBT_NS, 1000),
				 2 * TICK_NSEC);
		/* elapsed * budget <= used, it can't overflow */
		if (budget && elapsed <= div64_u64(used, budget))
			used = min(used - elapsed * budget, debt_max);
		else
			used = 0;
		atomic64_set(&lg->window_used, used);
	}

	used = atomic64_read(&lg->window_used);
	return used < budget ? budget - used : 0;
}

/*
 * Has @lg's subtree used more CPU time in the current window, including the
 * debt carried over, than its limit allows? Every limited level counts the
 * runtime of all tasks of its subtree, with or without slots: tasks running
 * in the kernel on credit (see pfs_admit_or_park()) are beyond the slots, and
 * with many short wakeups, as in a database, their runtime adds up to a
 * tenth or more of the limit. The full slots may have used floor(limit)
 * CPUs since the window began, the budget slot its whole budget. Runtime is
 * charged late, by a tick or, in a virtual machine whose CPU was preempted,
 * by much more, and usage below the budget is not carried over, so late
 * charges look like an excess at the start of a window: a margin of a tick
 * per slot plus one window's budget absorbs them. While the excess is above
 * the margin, the last full slot and the budget slot are not handed out, so
 * that the group pays it back; the margin only bounds the excess once.
 */
static u64 pfs_over_limit(struct pfs_group *lg, u64 now)
{
	unsigned int limit = READ_ONCE(lg->limit);
	u64 full = limit / 1000, allowed, elapsed, used, w = pfs_window();

	pfs_window_left(lg, now);
	elapsed = now - pfs_window_start(now, w);
	allowed = full * elapsed + 2 * pfs_budget(limit, w) - full * w +
		  pfs_width(limit) * TICK_NSEC;
	used = atomic64_read(&lg->window_used);
	return used > allowed ? used - allowed : 0;
}

static void pfs_queue_retry(struct pfs_group *lg);
static bool pfs_slot_free(struct pfs_group *lg);
static void pfs_arm_window_timer(struct pfs_group *lg, u64 now, u64 delay);

/*
 * Undo the increment of a refused pfs_take_slot(). A task parking at @lg
 * meanwhile may have seen the level full because of it, so check for
 * waiters as a release does, see pfs_put_slots().
 */
static void pfs_untake_slot(struct pfs_group *lg)
{
	atomic_dec(&lg->nr_slots);
	/* order the decrement before the waiter check */
	smp_mb__after_atomic();
	if (READ_ONCE(lg->nr_waiting) && pfs_slot_free(lg))
		pfs_queue_retry(lg);
}

/*
 * Take one slot at the limited level @lg. The first floor(limit) slots are
 * free, the last slot of a fractional limit needs budget left in the window,
 * and clips *@run_limit to it. The last full slot and the budget slot also
 * need the group to be within its limit, see pfs_over_limit().
 */
static enum pfs_reserve pfs_take_slot(struct pfs_group *lg, u64 now,
				      u64 *run_limit)
{
	unsigned int limit = READ_ONCE(lg->limit);
	unsigned int taken;
	u64 left, min_left;

	taken = atomic_inc_return(&lg->nr_slots) - 1;
	if (taken >= pfs_width(limit)) {
		pfs_untake_slot(lg);
		return PFS_RES_FULL;
	}

	if (taken >= limit / 1000) {
		left = pfs_window_left(lg, now);
		min_left = min_t(u64, pfs_budget(limit, pfs_window()),
				 PFS_MIN_BUDGET_NS);
		if (!left || left < min_left) {
			/* the window timer retries, see pfs_arm_window_timer() */
			atomic_dec(&lg->nr_slots);
			return PFS_RES_BUDGET;
		}
		*run_limit = min(*run_limit, left);
	}

	if (taken + 1 >= limit / 1000) {
		u64 excess = pfs_over_limit(lg, now);

		if (excess) {
			pfs_stat(PFS_STAT_PARK_DEBT);
			atomic_dec(&lg->nr_slots);
			/*
			 * Retry when the full slots, if any, would have paid
			 * the excess back by running below the limit.
			 */
			pfs_arm_window_timer(lg, now, limit >= 1000 ?
					     div_u64(excess, limit / 1000) : 0);
			return PFS_RES_DEBT;
		}
	}
	return PFS_RES_OK;
}

/*
 * Give back the slots in @mask on @g's path. Like a wait queue wake-up, the
 * waiters are checked after the slot counts went down, see pfs_park().
 * Returns true if a level has waiting tasks.
 */
static bool pfs_put_slots(struct pfs_group *g, u64 mask, bool retry)
{
	struct pfs_group *lg;
	bool waiters = false;

	if (!mask)
		return false;

	for_each_pfs_level(lg, g) {
		if (lg->depth <= PFS_MAX_DEPTH && (mask & BIT_ULL(lg->depth)))
			atomic_dec(&lg->nr_slots);
	}

	/* order the slot counts before the waiter check, see pfs_waiter_linked() */
	smp_mb__after_atomic();

	for_each_pfs_level(lg, g) {
		if (lg->depth > PFS_MAX_DEPTH || !(mask & BIT_ULL(lg->depth)))
			continue;
		if (READ_ONCE(lg->nr_waiting)) {
			waiters = true;
			if (retry)
				pfs_queue_retry(lg);
		}
	}
	return waiters;
}

/*
 * Take a slot at every limited level of @g's path, deepest first. On failure
 * nothing is held and *@fail is the refusing level. With @fifo a level with
 * waiting tasks refuses, so that newcomers don't overtake them.
 */
static enum pfs_reserve pfs_reserve(struct task_struct *p, struct pfs_group *g,
				    bool fifo, struct pfs_group **fail)
{
	enum pfs_reserve ret = PFS_RES_OK;
	struct sched_pfs_entity *pe = &p->pfs;
	u64 run_limit = pfs_task_slice(p);
	u64 now = 0, mask = 0;
	struct pfs_group *lg;

	/* slots held already would leak */
	if (WARN_ON_ONCE(pe->slot_mask)) {
		pfs_put_slots(pe->grp, pe->slot_mask, true);
		pe->slot_mask = 0;
	}

	for_each_pfs_level(lg, g) {
		if (!pfs_level_limited(lg))
			continue;

		if (fifo && READ_ONCE(lg->nr_waiting)) {
			ret = PFS_RES_FIFO;
			break;
		}

		if (!now)
			now = pfs_now();
		ret = pfs_take_slot(lg, now, &run_limit);
		if (ret != PFS_RES_OK)
			break;
		mask |= BIT_ULL(lg->depth);
	}

	if (ret != PFS_RES_OK) {
		/* a level above may have refused while waiters were held off */
		pfs_put_slots(g, mask, true);
		*fail = lg;
		return ret;
	}

	if (mask)
		pfs_stat(PFS_STAT_ADMIT);
	pe->slot_mask = mask;
	pe->unslotted = 0;
	pe->woken = 0;
	pe->grp = g;
	pe->run_start = p->se.sum_exec_runtime;
	pe->run_limit = run_limit;
	return PFS_RES_OK;
}

/* A limited level of @g's path has no free slot or waiters, roughly. */
static bool pfs_path_full(struct pfs_group *g)
{
	struct pfs_group *lg;

	for_each_pfs_level(lg, g) {
		if (!pfs_level_limited(lg))
			continue;
		if (READ_ONCE(lg->nr_waiting) ||
		    (unsigned int)atomic_read(&lg->nr_slots) >=
		    pfs_width(READ_ONCE(lg->limit)))
			return true;
	}
	return false;
}

/* A level of @lg can take one more task right now, roughly. */
static bool pfs_slot_free(struct pfs_group *lg)
{
	unsigned int limit = READ_ONCE(lg->limit);

	if (!limit)
		return true;
	return (unsigned int)atomic_read(&lg->nr_slots) < pfs_width(limit);
}

/*
 * The running task @p has used up its run limit: should it give its slots
 * back? Yes if a level of its path has waiting tasks, holds more slots than
 * its limit allows (the limit went down), is out of budget while its budget
 * slot is taken, ran beyond its limit while its last full slot is taken, or
 * became limited after @p took its slots.
 */
static bool pfs_should_rotate(struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;
	struct pfs_group *lg;
	u64 now = 0;

	for_each_pfs_level(lg, pe->grp) {
		unsigned int limit = READ_ONCE(lg->limit);
		unsigned int taken;

		if (!pfs_level_limited(lg))
			continue;
		if (!(pe->slot_mask & BIT_ULL(lg->depth)))
			return true;
		if (READ_ONCE(lg->nr_waiting))
			return true;
		taken = atomic_read(&lg->nr_slots);
		if (taken > pfs_width(limit))
			return true;
		if (taken < limit / 1000)
			continue;
		if (!now)
			now = pfs_now();
		if (taken > limit / 1000 && !pfs_window_left(lg, now))
			return true;
		if (pfs_over_limit(lg, now))
			return true;
	}
	return false;
}

/*
 * Charge @delta of runtime of a task of @g, with or without slots, to the
 * limited levels of its path: to the window of the level, see
 * pfs_window_left() and pfs_over_limit(), and to the slot service of the
 * child of the level the task belongs to, which shares the level's slots
 * between its children by weight, see pfs_next_waiter(). Returns true if a
 * level is limited.
 */
static bool pfs_charge(struct pfs_group *g, u64 delta)
{
	struct pfs_group *lg, *child = NULL;
	u64 now = 0;

	for (lg = g; lg && lg->depth > 0; child = lg, lg = pfs_parent(lg)) {
		if (!pfs_level_limited(lg))
			continue;

		if (!now)
			now = pfs_now();
		pfs_window_left(lg, now);
		atomic64_add(delta, &lg->window_used);

		if (child)
			atomic64_add(div64_ul(delta * scale_load_down(NICE_0_LOAD),
					      max(scale_load_down(READ_ONCE(child->weight)), 1UL)),
				     &child->slot_vtime);
		else
			atomic64_add(delta, &lg->own_slot_vtime);
	}
	return now != 0;
}

/**************************************************************
 * Waiting.
 */

static enum hrtimer_restart pfs_window_timer_fn(struct hrtimer *timer);
static void pfs_group_retry(struct pfs_group *g);

/*
 * Start @lg's timer for the next window, for tasks waiting for budget, or
 * after @delay if it is non-zero and earlier (at least PFS_MIN_BUDGET_NS),
 * for tasks waiting for the group to pay back runtime beyond its limit.
 */
static void pfs_arm_window_timer(struct pfs_group *lg, u64 now, u64 delay)
{
	u64 next, w = pfs_window();

	if (!now)
		now = pfs_now();
	next = pfs_window_start(now, w) + w;
	if (delay)
		next = min(next, now + max_t(u64, delay, PFS_MIN_BUDGET_NS));

	if (atomic_cmpxchg(&lg->timer_armed, 0, 1)) {
		/* armed already, bring it forward if needed */
		if (!delay || ktime_before(hrtimer_get_expires(&lg->window_timer),
					   ns_to_ktime(next)))
			return;
	}
	hrtimer_start(&lg->window_timer, ns_to_ktime(next), HRTIMER_MODE_ABS_PINNED_HARD);
}

static enum hrtimer_restart pfs_window_timer_fn(struct hrtimer *timer)
{
	struct pfs_group *lg = container_of(timer, struct pfs_group, window_timer);

	atomic_set(&lg->timer_armed, 0);
	pfs_group_retry(lg);
	return HRTIMER_NORESTART;
}

static void pfs_retry_workfn(struct irq_work *work)
{
	pfs_group_retry(container_of(work, struct pfs_group, retry_work));
}

/*
 * Admit @lg's waiting tasks soon, from a context without run queue locks. A
 * CPU going offline has flushed its irq_work already when it moves its
 * waiting tasks away (sched_cpu_dying()), and a hard irq_work queued there
 * would stay pending until the CPU comes back, so that no retry of the group
 * could be queued anymore: use an active CPU instead.
 */
static void pfs_queue_retry(struct pfs_group *lg)
{
	int cpu = raw_smp_processor_id();

	if (likely(cpu_active(cpu))) {
		irq_work_queue(&lg->retry_work);
		return;
	}
	cpu = cpumask_any_but(cpu_active_mask, cpu);
	if (cpu < nr_cpu_ids)
		irq_work_queue_on(&lg->retry_work, cpu);
}

static DEFINE_PER_CPU(struct cpumask, pfs_kick_mask);

static void pfs_kick_workfn(struct irq_work *work)
{
	struct cpumask *mask = this_cpu_ptr(&pfs_kick_mask);
	int cpu;

	for_each_cpu(cpu, mask) {
		cpumask_clear_cpu(cpu, mask);
		resched_cpu(cpu);
	}
}

static DEFINE_PER_CPU(struct irq_work, pfs_kick_work) =
	IRQ_WORK_INIT_HARD(pfs_kick_workfn);

/*
 * Make @cpu reschedule, e.g. an idle CPU which should pull a task. Callers
 * hold a run queue lock, so it is done from an irq_work.
 */
static void pfs_kick_cpu(int cpu)
{
	/* not from a CPU going offline, see pfs_queue_retry() */
	if (unlikely(!cpu_active(smp_processor_id())))
		return;
	pfs_stat(PFS_STAT_KICK);
	cpumask_set_cpu(cpu, this_cpu_ptr(&pfs_kick_mask));
	irq_work_queue(this_cpu_ptr(&pfs_kick_work));
}

static void __pfs_link_waiter(struct sched_pfs_entity *pe, struct pfs_group *lg)
{
	raw_spin_lock(&lg->lock);
	pe->wake_list = pe->woken;
	if (pe->wake_list) {
		list_add_tail(&pe->wait_node, &lg->wake_waiters);
		WRITE_ONCE(lg->nr_woken, lg->nr_woken + 1);
	} else {
		list_add_tail(&pe->wait_node, &lg->waiters);
	}
	WRITE_ONCE(lg->nr_waiting, lg->nr_waiting + 1);
	raw_spin_unlock(&lg->lock);
	pe->wait_grp = lg;
}

static void __pfs_unlink_waiter(struct sched_pfs_entity *pe)
{
	struct pfs_group *lg = pe->wait_grp;

	raw_spin_lock(&lg->lock);
	list_del_init(&pe->wait_node);
	if (pe->wake_list)
		WRITE_ONCE(lg->nr_woken, lg->nr_woken - 1);
	pe->wake_list = 0;
	WRITE_ONCE(lg->nr_waiting, lg->nr_waiting - 1);
	raw_spin_unlock(&lg->lock);
	pe->wait_grp = NULL;
}

/*
 * After linking a waiter, check for a free slot that a concurrent release
 * may have left without seeing the waiter (paired with pfs_put_slots()).
 */
static void pfs_waiter_linked(struct pfs_group *lg, enum pfs_reserve why)
{
	switch (why) {
	case PFS_RES_BUDGET:
		pfs_stat(PFS_STAT_PARK_BUDGET);
		pfs_arm_window_timer(lg, 0, 0);
		break;
	case PFS_RES_DEBT:
		/* pfs_take_slot() counted it and armed the timer */
		break;
	case PFS_RES_FIFO:
		pfs_stat(PFS_STAT_PARK_FIFO);
		break;
	default:
		pfs_stat(PFS_STAT_PARK_FULL);
		break;
	}

	/* order linking before the slot check, see pfs_put_slots() */
	smp_mb();
	if (pfs_slot_free(lg))
		pfs_queue_retry(lg);
}

/* @p, runnable on @rq, waits for a slot at @lg. */
static void pfs_park(struct rq *rq, struct task_struct *p, struct pfs_group *lg,
		     enum pfs_reserve why)
{
	struct sched_pfs_entity *pe = &p->pfs;

	lockdep_assert_rq_held(rq);

	pe->state = PFS_WAITING;
	__pfs_link_waiter(pe, lg);
	list_add_tail(&pe->rq_wait_node, &rq->pfs.waiters);
	rq->pfs.nr_waiting++;
	pfs_waiter_linked(lg, why);
}

static void pfs_unpark(struct rq *rq, struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;

	lockdep_assert_rq_held(rq);

	__pfs_unlink_waiter(pe);
	list_del_init(&pe->rq_wait_node);
	rq->pfs.nr_waiting--;
	pe->state = PFS_NONE;
}

/*
 * Try to reserve slots for the task @p waiting at @lg, @rq is task_rq(p) and
 * locked. Returns 1 when @p holds its slots (still linked), 0 when another
 * level refused and @p now waits there, -1 when @lg itself refused.
 */
static int pfs_try_waiter(struct rq *rq, struct task_struct *p,
			  struct pfs_group *lg)
{
	struct sched_pfs_entity *pe = &p->pfs;
	struct pfs_group *g = task_pfs_group(p), *fail = NULL;
	bool woken = pe->woken;
	enum pfs_reserve ret;

	lockdep_assert_rq_held(rq);

	ret = pfs_reserve(p, g, false, &fail);
	if (ret == PFS_RES_OK) {
		pe->fresh = woken;
		return 1;
	}

	if (fail == lg) {
		if (ret == PFS_RES_BUDGET)
			pfs_arm_window_timer(lg, 0, 0);
		return -1;
	}

	/* wait at the level which refused */
	__pfs_unlink_waiter(pe);
	__pfs_link_waiter(pe, fail);
	pfs_waiter_linked(fail, ret);
	return 0;
}

static bool pfs_waiting_at(struct rq *rq, struct task_struct *p,
			   struct pfs_group *lg)
{
	return task_rq(p) == rq && task_on_rq_queued(p) &&
	       p->pfs.state == PFS_WAITING && p->pfs.wait_grp == lg;
}

/* waiting tasks of a level compared by pfs_next_waiter() */
#define PFS_WAITER_SCAN		16

/*
 * The slot service of the child of @lg on @p's path, or of @lg's own tasks,
 * floored to a bit less than the service of the last child admitted at @lg:
 * a child coming back after a pause gets a bounded head start.
 */
static u64 pfs_waiter_vtime(struct pfs_group *lg, struct task_struct *p)
{
	struct pfs_group *g = task_pfs_group(p), *child = NULL;
	u64 vtime, floor;

	for (; g && g != lg; g = pfs_parent(g))
		child = g;

	if (!g)
		return 0;
	vtime = child ? atomic64_read(&child->slot_vtime) :
			atomic64_read(&lg->own_slot_vtime);
	floor = READ_ONCE(lg->slot_vmin) - 2 * sysctl_sched_base_slice;
	return (s64)(vtime - floor) < 0 ? floor : vtime;
}

/* woken waiting tasks admitted in a row while others wait */
#define PFS_WOKEN_STREAK	4
/* rotations after a wakeup which keep the wakeup priority */
#define PFS_FRESH_ROUNDS	2

/*
 * The next task to admit at @lg. Tasks which woke up and found no slot wait
 * in ->wake_waiters and go first (but not more than PFS_WOKEN_STREAK times in
 * a row while tasks which gave their slot back at the end of a slice wait):
 * they slept, while the others just used up a slice. Within the chosen list,
 * among the first PFS_WAITER_SCAN tasks, the first one of the child (or own
 * tasks) of @lg which received the least slot service for its weight; within
 * a child the order is FIFO. Waiting tasks are freed after a grace period,
 * the caller is in an RCU read-side section.
 */
static struct task_struct *pfs_next_waiter(struct pfs_group *lg)
{
	struct task_struct *best = NULL;
	struct sched_pfs_entity *pe;
	struct list_head *list;
	unsigned long flags;
	u64 best_vt = 0;
	int n = 0;

	raw_spin_lock_irqsave(&lg->lock, flags);
	list = &lg->wake_waiters;
	if (list_empty(list) ||
	    (lg->woken_streak >= PFS_WOKEN_STREAK && !list_empty(&lg->waiters)))
		list = &lg->waiters;
	if (list == &lg->wake_waiters)
		lg->woken_streak++;
	else
		lg->woken_streak = 0;

	list_for_each_entry(pe, list, wait_node) {
		struct task_struct *p = pfs_task_of(pe);
		u64 vt;

		if (n++ >= PFS_WAITER_SCAN)
			break;
		/* one waiting group is the common case */
		if (n == 1 && list_is_singular(list)) {
			best = p;
			break;
		}
		vt = pfs_waiter_vtime(lg, p);
		if (!best || (s64)(vt - best_vt) < 0) {
			best = p;
			best_vt = vt;
		}
	}
	if (best && (s64)(best_vt - READ_ONCE(lg->slot_vmin)) > 0)
		WRITE_ONCE(lg->slot_vmin, best_vt);
	raw_spin_unlock_irqrestore(&lg->lock, flags);
	return best;
}

/* A limited level of @g's path has woken tasks waiting. */
static bool pfs_woken_waiting(struct pfs_group *g)
{
	struct pfs_group *lg;

	for_each_pfs_level(lg, g) {
		if (pfs_level_limited(lg) && READ_ONCE(lg->nr_woken))
			return true;
	}
	return false;
}

static void pfs_queue_task(struct rq *rq, struct task_struct *p, int flags);
static void pfs_hrtick_update(struct rq *rq);

/*
 * Admit the waiting task @p, which holds its slots now, on its own CPU, and
 * kick an idle CPU if @p has to wait behind other tasks there.
 */
static void pfs_admit_in_place(struct rq *rq, struct task_struct *p)
{
	int cpu;

	pfs_stat(PFS_STAT_INPLACE);
	pfs_unpark(rq, p);
	pfs_queue_task(rq, p, 0);
	wakeup_preempt(rq, p, 0);

	if (rq->curr == rq->idle || task_current_donor(rq, p) ||
	    rq->pfs.nr_queued < 2)
		return;

	cpu = pfs_find_idle_cpu(p, cpu_of(rq));
	if (cpu >= 0)
		pfs_kick_cpu(cpu);
}

/*
 * Admit waiting tasks of @lg on their own CPUs while it has free slots. Runs
 * without run queue locks held: from the cpu.max write, the window timer and
 * the retry irq_work. A waiting task can't exit, and its task_struct stays
 * valid under RCU while it is looked up.
 */
static void pfs_group_retry(struct pfs_group *lg)
{
	int n;

	pfs_stat(PFS_STAT_RETRY);
	for (n = 0; n < PFS_RETRY_MAX; n++) {
		struct task_struct *p;
		struct rq_flags rf;
		struct rq *rq;
		int ret = -1;

		rcu_read_lock();
		p = pfs_next_waiter(lg);
		if (!p) {
			rcu_read_unlock();
			return;
		}

		rq = task_rq_lock(p, &rf);
		if (pfs_waiting_at(rq, p, lg)) {
			if (!sched_task_cpu_allowed(p, cpu_of(rq))) {
				/* hotplug moves it, see pfs_get_dying_waiter() */
				ret = -1;
			} else {
				update_rq_clock(rq);
				ret = pfs_try_waiter(rq, p, lg);
				if (ret > 0)
					pfs_admit_in_place(rq, p);
			}
		} else {
			/* raced with a hand-off, look again */
			ret = 0;
		}
		task_rq_unlock(rq, p, &rf);
		rcu_read_unlock();

		if (ret < 0)
			return;
	}

	/* more waiters, continue later */
	pfs_queue_retry(lg);
}

/**************************************************************
 * Weights.
 *
 * The hierarchy is flattened (see sched-pfs.rst): a group is active while a
 * task of its subtree is admitted, and its hierarchical weight is
 *
 *   H(g) = W(g)                          if g's parent is the root
 *   H(g) = H(parent) * W(g) / D(parent)  otherwise
 *
 * with D(p) the weights of p's active children plus the nice weights of p's
 * own admitted tasks. An admitted task t of g gets H(g) * w(t) / D(g), so the
 * admitted tasks of a group share its weight, however many wait.
 */

#define PFS_HWEIGHT_REFRESH_NS	NSEC_PER_MSEC

/*
 * Count @g in or out of its parent's active weight sum and propagate while
 * the parents' states change. Increments and decrements commute, so the
 * races of a group's tasks going in and out from several CPUs leave the sums
 * consistent: a group's weight is in its parent's sum exactly while its
 * nr_active is positive. Each level takes its parent's weight_lock, which
 * serialises only siblings, never two of them at once.
 */
static void pfs_update_active(struct pfs_group *g, bool active)
{
	struct pfs_group *parent;
	bool propagate;

	for (; g->depth > 0; g = parent) {
		parent = pfs_parent(g);
		propagate = false;

		raw_spin_lock(&parent->weight_lock);
		if (active) {
			if (!g->nr_active++) {
				/* autogroup nice changes don't go through the hook */
				g->weight = max(READ_ONCE(pfs_tg(g)->shares),
						scale_load(MIN_SHARES));
				parent->child_weight_sum += g->weight;
				propagate = true;
			}
		} else {
			if (!--g->nr_active) {
				parent->child_weight_sum -= g->weight;
				propagate = true;
			}
		}
		raw_spin_unlock(&parent->weight_lock);

		if (!propagate)
			break;
	}
}

static inline unsigned long pfs_group_load(struct pfs_group *g)
{
	return READ_ONCE(g->child_weight_sum) + atomic_long_read(&g->own_weight);
}

/*
 * @g's hierarchical weight, recomputed at most once per
 * PFS_HWEIGHT_REFRESH_NS: on a busy host groups go idle and busy all the
 * time, and a global generation of the active set would be a cache line
 * written by every CPU. The inputs are read without the locks, the result is
 * approximate by nature.
 */
static unsigned long pfs_hweight(struct pfs_group *g, u64 now)
{
	struct pfs_group *parent = pfs_parent(g);
	unsigned long h, d;

	if (READ_ONCE(g->hweight) &&
	    now - READ_ONCE(g->hweight_at) < PFS_HWEIGHT_REFRESH_NS)
		return READ_ONCE(g->hweight);

	h = READ_ONCE(g->weight);
	if (parent->depth > 0 && g->depth <= PFS_MAX_DEPTH) {
		d = pfs_group_load(parent);
		if (d)
			h = mul_u64_u64_div_u64(pfs_hweight(parent, now), h, d);
	}
	h = max(h, scale_load(MIN_SHARES));

	WRITE_ONCE(g->hweight, h);
	WRITE_ONCE(g->hweight_at, now);
	return h;
}

/*
 * A limited level of @g's path ran beyond its limit, see pfs_over_limit():
 * its tasks on credit then run with the lowest weight.
 */
static bool pfs_path_over_limit(struct pfs_group *g)
{
	struct pfs_group *lg;
	u64 now = 0;

	for_each_pfs_level(lg, g) {
		if (!pfs_level_limited(lg))
			continue;
		if (!now)
			now = pfs_now();
		if (pfs_over_limit(lg, now))
			return true;
	}
	return false;
}

/* a task of a group with a tiny share runs at least about this often */
#define PFS_WAIT_MAX_NS		NSEC_PER_SEC

/*
 * The weight @p is scheduled with on a CPU where the other admitted tasks
 * weigh @load; @p is counted in its group. The starvation floor keeps the
 * share of a task of a group with a tiny weight at slice / PFS_WAIT_MAX_NS of
 * the CPU, so it gets a slice about every PFS_WAIT_MAX_NS: cpu.weight 1 next
 * to groups of weight 10000 would otherwise wait for many seconds.
 */
static unsigned long pfs_effective_weight(struct task_struct *p,
					  unsigned long load)
{
	struct sched_pfs_entity *pe = &p->pfs;
	struct pfs_group *g = pe->grp;
	unsigned long w = p->se.load.weight, d, floor;

	if (!g || !g->depth)
		return w;

	/*
	 * A task on credit is beyond its group's slots. While the group is
	 * over its limit (it doesn't get slots back before it paid back the
	 * excess), its tasks on credit only get CPU time that nobody else
	 * wants, as SCHED_IDLE tasks: otherwise a group whose work runs mostly
	 * in the kernel would run on as many CPUs as it has threads, and
	 * with debt capped, use more than its limit.
	 */
	if (pe->unslotted && !pe->slot_mask && atomic_read(&pfs_nr_limited) &&
	    pfs_path_over_limit(g))
		return scale_load(WEIGHT_IDLEPRIO);

	d = pfs_group_load(g);
	if (d)
		w = mul_u64_u64_div_u64(pfs_hweight(g, pfs_now()), w, d);

	floor = mul_u64_u64_div_u64(load, pfs_task_slice(p), PFS_WAIT_MAX_NS);
	if (!task_has_idle_policy(p) && w < floor)
		w = floor;
	return clamp(w, scale_load(MIN_SHARES), scale_load(MAX_SHARES));
}

/* @p becomes or stops being an admitted task of its group */
static void pfs_account(struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;
	struct pfs_group *g = pe->grp;

	if (!g || !g->depth)
		return;
	pe->run_weight = p->se.load.weight;
	atomic_long_add(pe->run_weight, &g->own_weight);
	if (atomic_inc_return(&g->nr_own) == 1)
		pfs_update_active(g, true);
}

static void pfs_unaccount(struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;
	struct pfs_group *g = pe->grp;

	if (!g || !g->depth)
		return;
	atomic_long_sub(pe->run_weight, &g->own_weight);
	if (atomic_dec_return(&g->nr_own) == 0)
		pfs_update_active(g, false);
}

/* Rescale the lag and a relative deadline of @pe to the weight @w. */
static void pfs_set_weight(struct sched_pfs_entity *pe, unsigned long w)
{
	unsigned long old = pe->weight.weight;

	if (old && old != w) {
		long o = max(scale_load_down(old), 1UL);
		long n = max(scale_load_down(w), 1UL);

		pe->vlag = div64_long(pe->vlag * o, n);
		if (pe->rel_deadline)
			pe->deadline = div64_long((s64)pe->deadline * o, n);
	}
	pe->weight.weight = w;
	pe->weight.inv_weight = 0;
}

/*
 * Refresh the effective weight of the running task, preserving its lag and
 * its relative deadline (see reweight_eevdf() in fair.c). It is not in the
 * tree, its weight enters the averages only through pfs_curr_entity().
 */
static void pfs_reweight_curr(struct rq *rq, struct task_struct *curr)
{
	struct pfs_rq *prq = &rq->pfs;
	struct sched_pfs_entity *pe = &curr->pfs;
	unsigned long old = pe->weight.weight, w;
	u64 avruntime;
	s64 vlag, vdl;
	long o, n;

	w = pfs_effective_weight(curr, prq->load - old);
	if (w == old)
		return;

	avruntime = pfs_avg_vruntime(prq);
	vlag = (s64)(avruntime - pe->vruntime);
	vdl = (s64)(pe->deadline - avruntime);
	o = max(scale_load_down(old), 1UL);
	n = max(scale_load_down(w), 1UL);

	pe->weight.weight = w;
	pe->weight.inv_weight = 0;
	prq->load += w - old;

	pe->vruntime = avruntime - div64_long(vlag * o, n);
	pe->deadline = avruntime + div64_long(vdl * o, n);
}

/**************************************************************
 * Admitted tasks on a CPU.
 */

/*
 * Pay back negative lag for the time asleep, at the rate the virtual time of
 * the CPU it slept on advanced, and at most up to zero lag: what the fair
 * class gets from keeping such a task queued until it is eligible.
 */
static void pfs_serve_sleep_lag(struct rq *rq, struct sched_pfs_entity *pe)
{
	s64 elapsed, served;

	if (!pe->sleep_stamp) {
		pe->slept = U64_MAX;
		return;
	}
	elapsed = rq_clock(rq) - pe->sleep_stamp;
	pe->sleep_stamp = 0;
	pe->slept = max_t(s64, elapsed, 0);
	if (elapsed <= 0 || !pe->sleep_load || pe->vlag >= 0)
		return;
	served = div64_u64((u64)elapsed * scale_load_down(NICE_0_LOAD),
			   max(pe->sleep_load, 1UL));
	pe->vlag = min_t(s64, pe->vlag + served, 0);
}

/* Put an admitted task on @rq: the tree, or as the running task. */
static void pfs_queue_task(struct rq *rq, struct task_struct *p, int flags)
{
	struct pfs_rq *prq = &rq->pfs;
	struct sched_pfs_entity *pe = &p->pfs;
	unsigned long w;

	lockdep_assert_rq_held(rq);
	pfs_account(p);
	w = pfs_effective_weight(p, prq->load);
	pfs_set_weight(pe, w);
	if (flags & ENQUEUE_WAKEUP)
		pfs_serve_sleep_lag(rq, pe);
	else
		pe->slept = U64_MAX;

	pfs_place_entity(prq, pe, flags);
	pe->state = PFS_QUEUED;
	if (prq->curr != p)
		__pfs_enqueue_entity(prq, pe);

	pe->rotate = 0;

	if (!prq->nr_queued++)
		dl_server_start(&rq->fair_server);
	prq->load += w;
	prq->nr_idle_policy += task_has_idle_policy(p);
	pfs_set_idle_hint(prq, 0);
	add_nr_running(rq, 1);
}

/* Take an admitted task off @rq, keeping its lag. */
static void pfs_unqueue_task(struct rq *rq, struct task_struct *p, int flags)
{
	struct pfs_rq *prq = &rq->pfs;
	struct sched_pfs_entity *pe = &p->pfs;

	lockdep_assert_rq_held(rq);
	pfs_update_entity_lag(prq, pe);
	if (!(flags & DEQUEUE_SLEEP)) {
		pe->deadline -= pe->vruntime;
		pe->rel_deadline = 1;
	}
	if (pe->on_tree)
		__pfs_dequeue_entity(prq, pe);

	/*
	 * The fair class keeps a task which sleeps with negative lag queued
	 * until the lag is paid back (DELAY_DEQUEUE, DELAY_ZERO): the lag is
	 * served while it sleeps. Remember when it went to sleep and how fast
	 * the CPU's virtual time advanced, see pfs_serve_sleep_lag().
	 */
	pe->sleep_stamp = 0;
	pe->sleep_load = 0;
	if (flags & DEQUEUE_SLEEP) {
		pe->sleep_stamp = rq_clock(rq);
		if (pe->vlag < 0 && pfs_feat(PFS_F_SLEEP_LAG))
			pe->sleep_load = prq->sum_weight + pfs_avg_weight(pe);
	}

	pe->state = PFS_NONE;
	prq->nr_queued--;
	prq->load -= pe->weight.weight;
	prq->nr_idle_policy -= task_has_idle_policy(p);
	sub_nr_running(rq, 1);
	pfs_unaccount(p);
}

/* Ask for retries of the levels of @g's path which have waiting tasks. */
static void pfs_retry_path(struct pfs_group *g)
{
	struct pfs_group *lg;

	for_each_pfs_level(lg, g) {
		if (READ_ONCE(lg->nr_waiting) && pfs_slot_free(lg))
			pfs_queue_retry(lg);
	}
}

/*
 * Give back the slots @p holds. With @handoff, @p is the previous task of a
 * schedule() on @rq: the pick that follows hands the slots to waiting tasks,
 * see pfs_handoff(), otherwise a retry does.
 */
static void pfs_release_slots(struct rq *rq, struct task_struct *p, bool handoff)
{
	struct sched_pfs_entity *pe = &p->pfs;
	u64 mask = pe->slot_mask;

	lockdep_assert_rq_held(rq);
	pe->slot_mask = 0;
	pe->granted = 0;
	if (!mask || !pfs_put_slots(pe->grp, mask, !handoff) || !handoff)
		return;

	if (rq->pfs.handoff && rq->pfs.handoff != pe->grp)
		pfs_retry_path(rq->pfs.handoff);
	rq->pfs.handoff = pe->grp;
}

static void update_curr_pfs(struct rq *rq);

static inline bool pfs_has_user_work(struct task_struct *p)
{
	return p->pfs.user_work.next != &p->pfs.user_work;
}

/*
 * Make @p park, or rotate, when it next returns to user space, see
 * pfs_user_workfn(). Kernel threads and exiting tasks don't go back to user
 * space; they keep running until they block. io_uring workers
 * (PF_IO_WORKER) don't either, but run the task work between work items,
 * where they hold no locks.
 */
static void pfs_request_user_park(struct task_struct *p)
{
	if (pfs_has_user_work(p) || (p->flags & (PF_KTHREAD | PF_EXITING)))
		return;
	task_work_add(p, &p->pfs.user_work, TWA_RESUME);
}

/*
 * @p must not wait for a slot: it is being killed (SIGKILL, or chosen by the
 * OOM killer, which needs it to exit and free its memory), or it holds a PI
 * lock (an rt_mutex or a PI futex) that others wait for, or is boosted by
 * one. Such a task runs on credit; its runtime is charged to its group as
 * any runtime on credit.
 */
static inline bool pfs_must_run(struct task_struct *p)
{
	if (fatal_signal_pending(p) || tsk_is_oom_victim(p))
		return true;
#ifdef CONFIG_RT_MUTEXES
	if (p->prio != p->normal_prio ||
	    !RB_EMPTY_ROOT(&p->pi_waiters.rb_root))
		return true;
#endif
	return false;
}

/* Queue @p on @rq without slots, see pfs_admit_or_park(). */
static void pfs_admit_credit(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_pfs_entity *pe = &p->pfs;

	lockdep_assert_rq_held(rq);

	pfs_stat(PFS_STAT_CREDIT);
	pe->grp = task_pfs_group(p);
	pe->unslotted = 1;
	pe->woken = !!(flags & ENQUEUE_WAKEUP);
	if (pe->woken)
		pe->fresh_rounds = 0;
	pe->run_start = p->se.sum_exec_runtime;
	pe->run_limit = 0;
	pfs_queue_task(rq, p, flags);
	pfs_request_user_park(p);
}

/*
 * Admit @p on @rq if it gets its slots. Otherwise it waits, but only where it
 * holds nothing that others may wait for: a task is parked only when it is
 * preempted on its way back to user space (@can_park). Anywhere else (a
 * wakeup, fork, a cgroup move) the task may hold kernel locks or be inside an
 * RCU read-side section, so it runs on credit, without slots, and parks when
 * it returns to user space. This is what CFS bandwidth control does with its
 * throttling since the task based throttle model. A task which must not wait,
 * see pfs_must_run(), runs on credit too.
 */
static void pfs_admit_or_park(struct rq *rq, struct task_struct *p, int flags,
			      bool can_park)
{
	struct pfs_group *g = task_pfs_group(p), *fail = NULL;
	struct sched_pfs_entity *pe = &p->pfs;
	enum pfs_reserve ret;

	lockdep_assert_rq_held(rq);

	ret = pfs_reserve(p, g, true, &fail);
	if (ret == PFS_RES_OK) {
		pe->fresh = !!(flags & ENQUEUE_WAKEUP);
		if (pe->fresh)
			pe->fresh_rounds = 0;
		pfs_queue_task(rq, p, flags);
		return;
	}

	if (can_park && !pfs_must_run(p)) {
		pfs_park(rq, p, fail, ret);
		return;
	}

	pfs_admit_credit(rq, p, flags);
}

/* The waiting task @p must not wait anymore: run it on credit. */
static void pfs_unpark_credit(struct rq *rq, struct task_struct *p)
{
	pfs_unpark(rq, p);
	pfs_admit_credit(rq, p, 0);
	wakeup_preempt(rq, p, 0);
}

/*
 * A waiting task got a fatal signal: the signal code found it running (a
 * waiting task is TASK_RUNNING) and kicks it, see kick_process(). Admit the
 * waiting tasks of this CPU which must not wait.
 */
static void pfs_fatal_workfn(struct irq_work *work)
{
	struct rq *rq = this_rq();
	struct sched_pfs_entity *pe, *tmp;
	struct rq_flags rf;

	rq_lock(rq, &rf);
	update_rq_clock(rq);
	list_for_each_entry_safe(pe, tmp, &rq->pfs.waiters, rq_wait_node) {
		struct task_struct *p = pfs_task_of(pe);

		if (pe->state == PFS_WAITING && pfs_must_run(p))
			pfs_unpark_credit(rq, p);
	}
	rq_unlock(rq, &rf);
}

static DEFINE_PER_CPU(struct irq_work, pfs_fatal_work) =
	IRQ_WORK_INIT_HARD(pfs_fatal_workfn);

/* Called by kick_process() with interrupts disabled. */
void pfs_kick_waiter(struct task_struct *p)
{
	int cpu;

	if (READ_ONCE(p->pfs.state) != PFS_WAITING || !pfs_must_run(p))
		return;
	cpu = task_cpu(p);
	if (cpu_active(cpu))
		irq_work_queue_on(&per_cpu(pfs_fatal_work, cpu), cpu);
}

/* Is the tick handling an interrupt from user space? */
static inline bool pfs_tick_from_user(void)
{
	struct pt_regs *regs = get_irq_regs();

	return regs && user_mode(regs);
}

/*
 * Task work on the way back to user space: a task which runs on credit takes
 * its slots now or parks, a task whose rotation was deferred because it was
 * in the kernel rotates now. Both happen through a reschedule, in which
 * balance_pfs() and put_prev_task_pfs() park it.
 */
static void pfs_user_workfn(struct callback_head *work)
{
	struct task_struct *p = current;
	struct sched_pfs_entity *pe = &p->pfs;
	struct pfs_group *fail = NULL;
	struct rq_flags rf;
	struct rq *rq;

	WARN_ON_ONCE(work != &pe->user_work);
	work->next = work;
	if (p->flags & PF_EXITING)
		return;

	rq = task_rq_lock(p, &rf);
	if (p->sched_class != &pfs_sched_class || pe->state != PFS_QUEUED)
		goto unlock;

	update_rq_clock(rq);
	update_curr_pfs(rq);
	if (pe->unslotted && !pe->slot_mask) {
		bool woken = pe->woken;

		if (pfs_reserve(p, task_pfs_group(p), true, &fail) == PFS_RES_OK) {
			/* ran on credit since it woke up (not since fork) */
			pe->fresh = woken;
			goto unlock;
		}
	} else if (!pfs_should_rotate(p)) {
		goto unlock;
	}
	if (pfs_must_run(p))
		goto unlock;

	pfs_stat(PFS_STAT_USER_PARK);
	pe->rotate = 1;
	resched_curr(rq);
unlock:
	task_rq_unlock(rq, p, &rf);
}

/* a task which woke up waits for a slot at most about this long */
#define PFS_WAKE_GRAN_NS	(NSEC_PER_MSEC / 2)

/*
 * The running task used up its run limit, or ran at least PFS_WAKE_GRAN_NS
 * while a task which woke up waits for one of its slots. A task which got its
 * slot after a wakeup itself keeps it for its run limit, it is not cut short
 * by the next one.
 */
static bool pfs_run_limit_reached(struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;
	u64 ran = p->se.sum_exec_runtime - pe->run_start;

	if (ran >= pe->run_limit)
		return true;
	return ran >= PFS_WAKE_GRAN_NS && pe->slot_mask && !pe->fresh &&
	       pfs_woken_waiting(pe->grp);
}

/* The run limit of a task which keeps its slots for another round. */
static u64 pfs_run_limit(struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;
	u64 limit = pfs_task_slice(p), now = 0;
	struct pfs_group *lg;

	for_each_pfs_level(lg, pe->grp) {
		unsigned int l = READ_ONCE(lg->limit);

		if (!pfs_level_limited(lg) || !(l % 1000))
			continue;
		if ((unsigned int)atomic_read(&lg->nr_slots) <= l / 1000)
			continue;
		if (!now)
			now = pfs_now();
		limit = min(limit, pfs_window_left(lg, now));
	}
	return max_t(u64, limit, TICK_NSEC);
}

/**************************************************************
 * Runtime accounting.
 */

/* runtime charged to the group at once, see update_curr_pfs() */
#define PFS_CHARGE_BATCH_NS	(100 * NSEC_PER_USEC)

/* Charge @p's runtime not charged yet to the group it accrued in. */
static void pfs_flush_charge(struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;
	u64 pending = pe->charge_pending;

	if (!pending)
		return;
	pe->charge_pending = 0;
	if (pe->slot_mask) {
		pfs_charge(pe->grp, pending);
		pfs_stat_add(PFS_STAT_SLOT_NS, pending);
	} else if (atomic_read(&pfs_nr_limited) &&
		   pfs_charge(task_pfs_group(p), pending)) {
		/*
		 * On credit, or switching out after giving its slots back:
		 * beyond the slots, but it counts all the same.
		 */
		pfs_stat_add(PFS_STAT_CREDIT_NS, pending);
	}
}

/*
 * @p moves to another group, with its run queue locked. A queued task was
 * dequeued with DEQUEUE_MOVE already; this is for a sleeping one.
 */
void pfs_task_change_group(struct task_struct *p)
{
	lockdep_assert_rq_held(task_rq(p));
	pfs_flush_charge(p);
}

static void update_curr_pfs(struct rq *rq)
{
	struct pfs_rq *prq = &rq->pfs;
	struct task_struct *curr = prq->curr;
	struct sched_pfs_entity *pe;
	bool resched;
	s64 delta;

	if (!curr)
		return;

	delta = update_curr_common(rq);
	if (delta <= 0)
		return;

	dl_server_update(&rq->fair_server, delta);

	pe = &curr->pfs;
	/*
	 * Charge the group in batches: tasks which switch every few
	 * microseconds would otherwise bounce the group's counters between
	 * CPUs on every update; the margin of pfs_over_limit() absorbs the
	 * delay.
	 */
	pe->charge_pending += delta;
	if (pe->charge_pending >= PFS_CHARGE_BATCH_NS)
		pfs_flush_charge(curr);
	if (pe->state != PFS_QUEUED)
		return;

	pe->vruntime += pfs_calc_delta(delta, pe);
	resched = pfs_update_deadline(prq, pe);
	if (prq->nr_queued == 1)
		return;
	/* as update_curr() in fair.c: pick again once the protection ends */
	if (resched || (!pfs_protected(pe) && pfs_feat(PFS_F_PROTECT)))
		resched_curr_lazy(rq);
}

/**************************************************************
 * Scheduling class methods.
 */

static void enqueue_task_pfs(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_pfs_entity *pe = &p->pfs;

	/* a waiting task stays in its queue across a save/restore */
	if (pe->state == PFS_WAITING)
		return;
	if (WARN_ON_ONCE(pe->state != PFS_NONE))
		return;

	update_curr_pfs(rq);

	/* as enqueue_task_fair(): schedutil boosts after I/O waits */
	if (p->in_iowait)
		cpufreq_update_util(rq, SCHED_CPUFREQ_IOWAIT);
	/* the CPU starts running PFS tasks: decay the signal first */
	if (!rq->pfs.nr_queued &&
	    update_pfs_rq_load_avg(rq_clock_pelt(rq), rq, 0))
		cpufreq_update_util(rq, 0);

	/* tasks migrated here aren't cache hot */
	if (flags & ENQUEUE_MIGRATED)
		p->se.exec_start = 0;

	/* kept its slots across a migration or an attribute change */
	if (pe->granted && pe->grp == task_pfs_group(p)) {
		pe->granted = 0;
		pfs_queue_task(rq, p, flags);
		pfs_hrtick_update(rq);
		return;
	}
	if (pe->slot_mask)
		pfs_release_slots(rq, p, false);

	pfs_admit_or_park(rq, p, flags, false);
	pfs_hrtick_update(rq);
}

/* A dequeue which is followed by an enqueue into the same group. */
static inline bool pfs_dequeue_keeps(struct task_struct *p, int flags)
{
	if (flags & (DEQUEUE_MOVE | DEQUEUE_CLASS))
		return false;
	return (flags & DEQUEUE_SAVE) || task_on_rq_migrating(p);
}

static bool dequeue_task_pfs(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_pfs_entity *pe = &p->pfs;

	if (pe->state == PFS_QUEUED)
		update_curr_pfs(rq);
	/*
	 * Runtime below the batch size is charged later, see
	 * update_curr_pfs(), but not after the task left its group or died:
	 * a storm of short-lived tasks would otherwise charge nothing.
	 */
	if ((flags & (DEQUEUE_MOVE | DEQUEUE_CLASS)) || (p->flags & PF_EXITING))
		pfs_flush_charge(p);

	switch (pe->state) {
	case PFS_QUEUED:
		pfs_unqueue_task(rq, p, flags);
		if (pe->slot_mask && pfs_dequeue_keeps(p, flags))
			pe->granted = 1;
		else
			pfs_release_slots(rq, p, (flags & DEQUEUE_SLEEP) &&
					  task_current_donor(rq, p));
		break;
	case PFS_WAITING:
		/*
		 * sched_change holds the run queue lock until the enqueue, so
		 * nobody admits the task meanwhile: keep its place.
		 */
		if ((flags & DEQUEUE_SAVE) && !task_on_rq_migrating(p) &&
		    !(flags & (DEQUEUE_MOVE | DEQUEUE_CLASS)))
			break;
		pfs_unpark(rq, p);
		break;
	case PFS_PARKING:
		/* gave its slots back already */
		pe->state = PFS_NONE;
		break;
	case PFS_NONE:
		/* moved with its slots by a hand-off */
		break;
	}
	return true;
}

/*
 * Should a slot holder which calls sched_yield() give its slots to a waiting
 * task? A thread spinning on a lock with sched_yield() may wait for the lock
 * holder, which waits for a slot, so the slots go to the next waiting task.
 * But the yielding thread may be the lock holder itself, e.g. a thread which
 * yields with a mutex held: the threads woken for that mutex would take its
 * slots and find the mutex still taken. So it keeps its slots until it held
 * them for PFS_WAKE_GRAN_NS since it took them or started a new round, as a
 * task holding slots since a rotation does for a woken waiter.
 */
static bool pfs_yield_rotates(struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;

	if (pfs_feat(PFS_F_YIELD_KEEP) &&
	    p->se.sum_exec_runtime - pe->run_start < PFS_WAKE_GRAN_NS)
		return false;
	return pfs_should_rotate(p);
}

static void yield_task_pfs(struct rq *rq)
{
	struct task_struct *curr = rq->donor;
	struct sched_pfs_entity *pe = &curr->pfs;
	struct pfs_rq *prq = &rq->pfs;

	if (pe->state != PFS_QUEUED)
		return;

	update_rq_clock(rq);
	update_curr_pfs(rq);
	rq_clock_skip_update(rq);

	/*
	 * yield() is also called from inside the kernel, with locks held
	 * (e.g. netlink_broadcast() under RTNL), so don't park the task here:
	 * it gives its slots back when it returns to user space, as with a
	 * slice that ends in the kernel. For sched_yield() that is right
	 * after this schedule().
	 */
	if (pe->slot_mask && atomic_read(&pfs_nr_limited) &&
	    pfs_yield_rotates(curr)) {
		pfs_request_user_park(curr);
		pfs_stat(PFS_STAT_YIELD);
	}

	if (unlikely(rq->nr_running == 1))
		return;

	/* forfeit the rest of the request, if it is owed service at all */
	if (pfs_entity_eligible(prq, pe)) {
		pe->vruntime = pe->deadline;
		pfs_update_deadline(prq, pe);
	}
	pe->vprot = pe->vruntime;
}

/* debugfs wakeup_gran_us, see pfs_wakeup_far_ahead() */
static unsigned int pfs_wakeup_gran_us __read_mostly = 1000;

/*
 * Wakeup granularity for short sleepers, as the fair class had before EEVDF:
 * a task which slept for less than this preempts only if the running task is
 * ahead of it by more than this much (scaled to the waking task's weight).
 * Without it, tasks which sleep for microseconds, e.g. readers of a pipe,
 * preempt the writer on every wakeup and run in lockstep with it; with it
 * they batch up. A task which slept longer, e.g. a request handler, preempts
 * as EEVDF decides. It applies to synchronous wakeups, whose waker is about
 * to block (pipes, sockets), and not to others, e.g. a futex wakeup by a
 * thread which wakes several and keeps running: there a wakee which doesn't
 * preempt may wait behind another task for a whole slice.
 */
static bool pfs_wakeup_far_ahead(struct task_struct *curr,
				 struct task_struct *p, int wake_flags)
{
	u64 gran = (u64)READ_ONCE(pfs_wakeup_gran_us) * NSEC_PER_USEC;
	s64 ahead = (s64)(curr->pfs.vruntime - p->pfs.vruntime);

	if (!gran || p->pfs.slept >= gran)
		return true;
	if (!(wake_flags & WF_SYNC) && pfs_feat(PFS_F_GRAN_SYNC))
		return true;
	return ahead > (s64)pfs_calc_delta(gran, &p->pfs);
}

static void wakeup_preempt_pfs(struct rq *rq, struct task_struct *p, int wake_flags)
{
	struct task_struct *donor = rq->donor;
	struct pfs_rq *prq = &rq->pfs;

	if (p->sched_class != &pfs_sched_class ||
	    donor->sched_class != &pfs_sched_class)
		return;

	if (donor == p || p->pfs.state != PFS_QUEUED)
		return;

	/* the running task isn't admitted, it must not keep the CPU */
	if (donor->pfs.state != PFS_QUEUED)
		goto preempt;

	if (test_tsk_need_resched(rq->curr))
		return;

	if (task_has_idle_policy(donor) && !task_has_idle_policy(p))
		goto preempt;

	/* BATCH and IDLE tasks don't preempt */
	if (!normal_policy(p->policy))
		return;

	update_curr_pfs(rq);

	/* a shorter request may cut the running task's protected slice */
	if (p->pfs.slice < donor->pfs.slice) {
		if (pfs_entity_eligible(prq, &p->pfs) &&
		    pfs_entity_before(&p->pfs, &donor->pfs))
			goto preempt;
		return;
	}

	if (wake_flags & WF_FORK)
		return;

	if (pfs_wakeup_far_ahead(donor, p, wake_flags) &&
	    pfs_pick_eevdf(prq, true) == &p->pfs)
		goto preempt;

	/* see update_protect_slice() in fair.c */
	if (pfs_feat(PFS_F_PROTECT)) {
		struct sched_pfs_entity *se = &donor->pfs;
		u64 v = pfs_min_vruntime(se->vruntime, pfs_avg_vruntime(prq));
		u64 slice = min(se->slice, p->pfs.slice);

		se->vprot = pfs_min_vruntime(se->vprot, v + pfs_calc_delta(slice, se));
	}
	return;

preempt:
	donor->pfs.vprot = donor->pfs.vruntime;
	resched_curr_lazy(rq);
}

static int pfs_newidle(struct rq *this_rq, struct rq_flags *rf);
static int pfs_handoff(struct rq *rq, struct rq_flags *rf);

static struct task_struct *pick_task_pfs(struct rq *rq, struct rq_flags *rf)
{
	struct pfs_rq *prq = &rq->pfs;
	struct sched_pfs_entity *pe;
	int pulled;

again:
	if (prq->handoff && rf && !sched_core_enabled(rq) &&
	    pfs_handoff(rq, rf) < 0)
		return RETRY_TASK;

	if (prq->nr_queued) {
		if (pfs_curr_entity(prq))
			update_curr_pfs(rq);

		pe = pfs_pick_eevdf(prq, true);
		if (pe)
			return pfs_task_of(pe);
	}

	if (sched_core_enabled(rq) || !rf) {
		pfs_set_idle_hint(prq, 1);
		return NULL;
	}

	pulled = pfs_newidle(rq, rf);
	if (pulled < 0)
		return RETRY_TASK;
	if (pulled > 0)
		goto again;
	pfs_set_idle_hint(prq, 1);
	return NULL;
}

static void pfs_queue_push(struct rq *rq);

static void put_prev_task_pfs(struct rq *rq, struct task_struct *prev,
			      struct task_struct *next)
{
	struct pfs_rq *prq = &rq->pfs;
	struct sched_pfs_entity *pe = &prev->pfs;

	if (pe->state == PFS_QUEUED || pe->state == PFS_PARKING)
		update_curr_pfs(rq);
	if (update_pfs_rq_load_avg(rq_clock_pelt(rq), rq, 1))
		cpufreq_update_util(rq, 0);

	WARN_ON_ONCE(prq->curr != prev);
	prq->curr = NULL;

	switch (pe->state) {
	case PFS_QUEUED:
		if (!pe->on_tree)
			__pfs_enqueue_entity(prq, pe);
		/* preempted with slots: move it to an idle CPU, see pfs_push() */
		if (pe->slot_mask && task_on_rq_queued(prev))
			prq->push_preempted = 1;
		break;
	case PFS_PARKING:
		/*
		 * Its slice ended while others wait: admit it again only if
		 * it doesn't overtake anybody, else it waits at the tail. The
		 * run queue lock is held until it is off the CPU, see
		 * finish_task_switch(), so nobody sees it waiting before.
		 */
		pe->state = PFS_NONE;
		pfs_admit_or_park(rq, prev, 0, true);
		/* admitted after all, don't let this CPU idle with it queued */
		if (pe->state == PFS_QUEUED && next &&
		    sched_class_above(&pfs_sched_class, next->sched_class))
			pfs_kick_cpu(cpu_of(rq));
		break;
	default:
		break;
	}

	/* a hand-off that no pick consumed */
	if (prq->handoff) {
		pfs_retry_path(prq->handoff);
		prq->handoff = NULL;
	}
}

#ifdef CONFIG_SCHED_HRTICK
/*
 * As hrtick_start_fair(): with other tasks queued, end the running task's
 * request on time instead of at the next tick.
 */
static void pfs_hrtick_start(struct rq *rq, struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;
	s64 vdelta;

	if (!pfs_feat(PFS_F_HRTICK) || !hrtick_enabled_fair(rq) ||
	    rq->pfs.nr_queued <= 1 || pe->state != PFS_QUEUED)
		return;

	vdelta = (s64)(pe->deadline - pe->vruntime);
	if (vdelta < 0) {
		if (task_current_donor(rq, p))
			resched_curr(rq);
		return;
	}
	hrtick_start(rq, mul_u64_u64_div_u64(vdelta, pe->weight.weight,
					     NICE_0_LOAD));
}

/* A task was queued: start the hrtick for the running task. */
static void pfs_hrtick_update(struct rq *rq)
{
	struct task_struct *donor = rq->donor;

	if (donor->sched_class != &pfs_sched_class || hrtick_active(rq))
		return;
	pfs_hrtick_start(rq, donor);
}
#else
static inline void pfs_hrtick_start(struct rq *rq, struct task_struct *p) { }
static void pfs_hrtick_update(struct rq *rq) { }
#endif

static void set_next_task_pfs(struct rq *rq, struct task_struct *p,
			      enum snt_e type)
{
	struct pfs_rq *prq = &rq->pfs;
	struct sched_pfs_entity *pe = &p->pfs;

	if (type == SNT_REPICK) {
		pfs_hrtick_start(rq, p);
		return;
	}

	if (pe->on_tree)
		__pfs_dequeue_entity(prq, pe);

	p->se.exec_start = rq_clock_task(rq);
	p->se.prev_sum_exec_runtime = p->se.sum_exec_runtime;
	prq->curr = p;
	if (type == SNT_PICK) {
		pe->vprot = pe->deadline;
		/* the CPU didn't run PFS tasks until now */
		update_pfs_rq_load_avg(rq_clock_pelt(rq), rq, 0);
	}

	if (type == SNT_PICK) {
		pfs_queue_push(rq);
		pfs_hrtick_start(rq, p);
	}
}

static void task_tick_pfs(struct rq *rq, struct task_struct *curr, int queued)
{
	struct sched_pfs_entity *pe = &curr->pfs;

	update_curr_pfs(rq);
	update_pfs_rq_load_avg(rq_clock_pelt(rq), rq, 1);
	cpufreq_update_util(rq, 0);

	if (pe->state != PFS_QUEUED) {
		resched_curr(rq);
		return;
	}

	pfs_reweight_curr(rq, curr);
	/* on credit and over the limit, see pfs_effective_weight() */
	if (pe->unslotted && !pe->slot_mask && rq->pfs.nr_queued > 1 &&
	    pe->weight.weight <= scale_load(WEIGHT_IDLEPRIO))
		resched_curr_lazy(rq);

	/*
	 * A task of a limited group gives its slots back at the end of its
	 * run limit if somebody waits for them, see pfs_should_rotate().
	 */
	if (atomic_read(&pfs_nr_limited) && !pe->rotate &&
	    pfs_run_limit_reached(curr)) {
		if (pfs_should_rotate(curr)) {
			/*
			 * Only park a task preempted on its way to user space,
			 * see pfs_admit_or_park().
			 */
			if (pfs_must_run(curr)) {
				/* keeps running, see pfs_must_run() */
			} else if (pfs_tick_from_user()) {
				pe->rotate = 1;
				resched_curr(rq);
			} else {
				pfs_request_user_park(curr);
			}
		} else {
			pe->run_start = curr->se.sum_exec_runtime;
			pe->run_limit = pfs_run_limit(curr);
			pe->fresh = 0;
		}
	}

#ifdef CONFIG_SCHED_HRTICK
	if (queued)
		resched_curr(rq);
#endif
}

static void prio_changed_pfs(struct rq *rq, struct task_struct *p, u64 oldprio)
{
	if (!task_on_rq_queued(p) || p->prio == oldprio)
		return;

	/* boosted by a PI waiter: don't keep it waiting for a slot */
	if (p->pfs.state == PFS_WAITING) {
		if (pfs_must_run(p))
			pfs_unpark_credit(rq, p);
		return;
	}

	if (task_current_donor(rq, p)) {
		if (p->prio > oldprio)
			resched_curr(rq);
	} else {
		wakeup_preempt(rq, p, 0);
	}
}

static void switched_from_pfs(struct rq *rq, struct task_struct *p)
{
	if (p->pfs.slot_mask)
		pfs_release_slots(rq, p, false);
	p->pfs.granted = 0;
}

static void switched_to_pfs(struct rq *rq, struct task_struct *p)
{
	if (!task_on_rq_queued(p))
		return;

	if (task_current_donor(rq, p))
		resched_curr(rq);
	else
		wakeup_preempt(rq, p, 0);
}

/*
 * Called with the task dequeued (sched_change) or not runnable: its effective
 * weight is recomputed when it is admitted again.
 */
static void reweight_task_pfs(struct rq *rq, struct task_struct *p,
			      const struct load_weight *lw)
{
	p->se.load = *lw;
}

static unsigned int get_rr_interval_pfs(struct rq *rq, struct task_struct *p)
{
	return NS_TO_JIFFIES(pfs_task_slice(p));
}

/**************************************************************
 * Load balancing.
 */

static bool pfs_can_migrate(struct rq *src, struct task_struct *p, int dst_cpu)
{
	if (task_on_cpu(src, p))
		return false;
	if (is_migration_disabled(p))
		return false;
	if (!cpumask_test_cpu(dst_cpu, p->cpus_ptr))
		return false;
	return sched_task_cpu_allowed(p, dst_cpu);
}

/*
 * A queued, not running task of @src which may run on @dst_cpu, starting from
 * the end of the tree (the latest deadline, which would wait longest on @src)
 * whose effective weight is at most @max_weight.
 */
static struct task_struct *pfs_pick_migratable(struct rq *src, int dst_cpu,
					       unsigned long max_weight)
{
	struct rb_node *node = rb_last(&src->pfs.tasks_timeline.rb_root);
	int scanned = 0;

	for (; node && scanned < PFS_MIGRATE_SCAN; node = rb_prev(node), scanned++) {
		struct sched_pfs_entity *pe = __node_2_pe(node);
		struct task_struct *p = pfs_task_of(pe);

		if (pe->weight.weight > max_weight)
			continue;
		if (dst_cpu < 0) {
			/* any other CPU */
			if (!task_on_cpu(src, p) && !is_migration_disabled(p) &&
			    p->nr_cpus_allowed > 1)
				return p;
		} else if (pfs_can_migrate(src, p, dst_cpu)) {
			return p;
		}
	}
	return NULL;
}

/* A queued task holding slots which may move to another CPU. */
static struct task_struct *pfs_pick_holder(struct rq *rq)
{
	struct rb_node *node = rb_first_cached(&rq->pfs.tasks_timeline);
	int scanned = 0;

	for (; node && scanned < PFS_MIGRATE_SCAN; node = rb_next(node), scanned++) {
		struct task_struct *p = pfs_task_of(__node_2_pe(node));

		if (p->pfs.slot_mask && !task_on_cpu(rq, p) &&
		    !is_migration_disabled(p) && p->nr_cpus_allowed > 1)
			return p;
	}
	return NULL;
}

/* Queued tasks of @rq which aren't running, as seen without its lock. */
static inline unsigned int pfs_nr_waiting_on_tree(struct rq *rq)
{
	unsigned int nr = READ_ONCE(rq->pfs.nr_queued);
	struct task_struct *curr = READ_ONCE(rq->curr);

	if (nr && curr->sched_class == &pfs_sched_class)
		nr--;
	return nr;
}

static const struct cpumask *pfs_llc_span(int cpu)
{
	struct sched_domain *sd = rcu_dereference_all(per_cpu(sd_llc, cpu));

	return sd ? sched_domain_span(sd) : cpu_active_mask;
}

/* the newly idle pull looks beyond the LLC at most this often */
#define PFS_REMOTE_PULL_MS	4

/*
 * The CPU with the most queued, not running tasks, LLC first. The scan of
 * the whole machine, with this run queue locked and interrupts off, is
 * rate limited: idle CPUs elsewhere are also pushed to, see pfs_push().
 */
static struct rq *pfs_find_busiest(struct rq *this_rq)
{
	int this_cpu = cpu_of(this_rq), cpu, pass, passes = 1;
	struct rq *busiest = NULL;
	unsigned int max_nr = 0;

	if (time_after_eq(jiffies, this_rq->pfs.next_remote_pull)) {
		this_rq->pfs.next_remote_pull = jiffies +
			max(msecs_to_jiffies(PFS_REMOTE_PULL_MS), 1UL);
		passes = 2;
	}

	for (pass = 0; pass < passes && !busiest; pass++) {
		const struct cpumask *span = pass ? cpu_active_mask :
						    pfs_llc_span(this_cpu);

		for_each_cpu_wrap(cpu, span, this_cpu) {
			unsigned int nr;

			if (cpu == this_cpu)
				continue;
			nr = pfs_nr_waiting_on_tree(cpu_rq(cpu));
			if (nr > max_nr) {
				max_nr = nr;
				busiest = cpu_rq(cpu);
			}
		}
	}
	return busiest;
}

/*
 * This CPU is about to go idle: pull a queued task from the busiest CPU.
 * Returns 1 if there is work now, -1 if a higher class became runnable while
 * the run queue lock was dropped, 0 otherwise.
 */
static int pfs_newidle(struct rq *this_rq, struct rq_flags *rf)
{
	int this_cpu = cpu_of(this_rq);
	struct task_struct *p;
	struct rq *src;
	int pulled = 0;

	if (this_rq->ttwu_pending || !cpu_active(this_cpu))
		return 0;

	/*
	 * As the fair class does: the core measures how long this CPU stays
	 * idle from idle_stamp (rq->avg_idle). If it is usually woken again
	 * before a migration pays off, e.g. two tasks passing a token back and
	 * forth, don't pull.
	 */
	this_rq->idle_stamp = rq_clock(this_rq);
	if (this_rq->avg_idle < sysctl_sched_migration_cost)
		return 0;

	src = pfs_find_busiest(this_rq);
	if (!src)
		return 0;

	rq_unpin_lock(this_rq, rf);
	rq_modified_begin(this_rq, &pfs_sched_class);

	double_lock_balance(this_rq, src);
	p = pfs_pick_migratable(src, this_cpu, ULONG_MAX);
	if (p) {
		move_queued_task_locked(src, this_rq, p);
		pfs_stat(PFS_STAT_PULL);
		pulled = 1;
	}
	double_unlock_balance(this_rq, src);

	rq_repin_lock(this_rq, rf);

	if (rq_modified_above(this_rq, &pfs_sched_class))
		return -1;
	if (this_rq->pfs.nr_queued)
		pulled = 1;
	if (pulled)
		this_rq->idle_stamp = 0;
	return pulled;
}

static inline bool pfs_cpu_idle(int cpu)
{
	return available_idle_cpu(cpu) && !READ_ONCE(cpu_rq(cpu)->pfs.nr_queued);
}

/* An idle CPU in @p's affinity, close to @target first. */
static int pfs_find_idle_cpu(struct task_struct *p, int target)
{
	const struct cpumask *llc = pfs_llc_span(target);
	int cpu;

	/* overloaded: don't scan, as SIS_UTIL in the fair class */
	if (!atomic_read(&pfs_idle_cpus))
		return -1;

	for_each_cpu_wrap(cpu, llc, target) {
		if (cpumask_test_cpu(cpu, p->cpus_ptr) && pfs_cpu_idle(cpu))
			return cpu;
	}
	if (llc == cpu_active_mask)
		return -1;
	for_each_cpu_wrap(cpu, cpu_active_mask, target) {
		if (cpumask_test_cpu(cpu, llc))
			continue;
		if (cpumask_test_cpu(cpu, p->cpus_ptr) && pfs_cpu_idle(cpu))
			return cpu;
	}
	return -1;
}

/*
 * Wakeup placement, ported from select_task_rq_fair(): wake_wide() and
 * wake_affine() decide between the waking and the previous CPU,
 * select_idle_sibling() then looks for an idle CPU close to the choice.
 */

/* see record_wakee() in fair.c */
static void pfs_record_wakee(struct task_struct *p)
{
	if (time_after(jiffies, current->wakee_flip_decay_ts + HZ)) {
		current->wakee_flips >>= 1;
		current->wakee_flip_decay_ts = jiffies;
	}

	if (current->last_wakee != p) {
		current->last_wakee = p;
		current->wakee_flips++;
	}
}

/* M:N waker/wakee relations should spread, see wake_wide() in fair.c */
static int pfs_wake_wide(struct task_struct *p)
{
	unsigned int master = current->wakee_flips;
	unsigned int slave = p->wakee_flips;
	int factor = __this_cpu_read(sd_llc_size);

	if (master < slave)
		swap(master, slave);
	if (slave < factor || master < slave * factor)
		return 0;
	return 1;
}

static inline unsigned long pfs_task_load(struct task_struct *p)
{
	return p->pfs.weight.weight ?: p->se.load.weight;
}

/* wake_affine_idle() and wake_affine_weight() of fair.c on PFS loads */
static int pfs_wake_affine(struct task_struct *p, int this_cpu, int prev_cpu,
			   int sync)
{
	s64 this_load, prev_load, task_load;

	if (available_idle_cpu(this_cpu) && cpus_share_cache(this_cpu, prev_cpu))
		return available_idle_cpu(prev_cpu) ? prev_cpu : this_cpu;

	if (sync && cpu_rq(this_cpu)->nr_running == 1)
		return this_cpu;

	if (available_idle_cpu(prev_cpu))
		return prev_cpu;

	this_load = READ_ONCE(cpu_rq(this_cpu)->pfs.load);
	if (sync && current->sched_class == &pfs_sched_class) {
		s64 current_load = pfs_task_load(current);

		if (current_load > this_load)
			return this_cpu;
		this_load -= current_load;
	}

	task_load = pfs_task_load(p);
	this_load = (this_load + task_load) * 100;
	prev_load = max_t(s64, READ_ONCE(cpu_rq(prev_cpu)->pfs.load) - task_load, 0);
	/* imbalance_pct 117, half of it as in wake_affine_weight() */
	prev_load *= 100 + (117 - 100) / 2;
	if (sync)
		prev_load += 1;

	return this_load < prev_load ? this_cpu : prev_cpu;
}

/* Runqueue only has SCHED_IDLE tasks enqueued */
static inline bool pfs_sched_idle_rq(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned int nr = READ_ONCE(rq->nr_running);

	return nr && nr == READ_ONCE(rq->pfs.nr_idle_policy);
}

static inline bool pfs_choose_idle_cpu(int cpu, struct task_struct *p)
{
	return pfs_cpu_idle(cpu) ||
	       (pfs_sched_idle_rq(cpu) && !task_has_idle_policy(p));
}

/* see select_idle_sibling() in fair.c */
static int pfs_select_idle_sibling(struct task_struct *p, int prev, int target)
{
	int cpu, recent_used_cpu;
	struct sched_domain *sd;

	if (pfs_choose_idle_cpu(target, p))
		return target;

	/* if the previous CPU is cache affine and idle, don't be stupid */
	if (prev != target && cpus_share_cache(prev, target) &&
	    pfs_choose_idle_cpu(prev, p))
		return prev;

	/* a per-CPU kthread (e.g. I/O completion) stacks with its wakee */
	if (is_per_cpu_kthread(current) && in_task() &&
	    prev == smp_processor_id() && this_rq()->nr_running <= 1)
		return prev;

	recent_used_cpu = p->recent_used_cpu;
	p->recent_used_cpu = prev;
	if (recent_used_cpu != prev && recent_used_cpu != target &&
	    cpus_share_cache(recent_used_cpu, target) &&
	    pfs_choose_idle_cpu(recent_used_cpu, p) &&
	    cpumask_test_cpu(recent_used_cpu, p->cpus_ptr))
		return recent_used_cpu;

	sd = rcu_dereference_all(per_cpu(sd_llc, target));
	if (!sd || !atomic_read(&pfs_idle_cpus))
		return target;

	for_each_cpu_wrap(cpu, sched_domain_span(sd), target + 1) {
		if (!cpumask_test_cpu(cpu, p->cpus_ptr))
			continue;
		if (pfs_choose_idle_cpu(cpu, p))
			return cpu;
	}
	return target;
}

/*
 * For fork and exec: an idle CPU, else the one with the fewest tasks in the
 * LLC of the forking CPU (the whole machine only if @p may not run there):
 * on a busy machine every fork would otherwise read every run queue.
 */
static int pfs_find_idlest_cpu(struct task_struct *p, int this_cpu)
{
	const struct cpumask *span = pfs_llc_span(this_cpu);
	unsigned int nr, min_nr = UINT_MAX;
	int cpu, best = this_cpu;

	cpu = pfs_find_idle_cpu(p, this_cpu);
	if (cpu >= 0)
		return cpu;

	if (!cpumask_intersects(span, p->cpus_ptr))
		span = cpu_active_mask;
	for_each_cpu_wrap(cpu, span, this_cpu) {
		if (!cpumask_test_cpu(cpu, p->cpus_ptr) || !cpu_active(cpu))
			continue;
		nr = READ_ONCE(cpu_rq(cpu)->nr_running);
		if (nr < min_nr) {
			min_nr = nr;
			best = cpu;
		}
	}
	return best;
}

/*
 * Wake @p on the waking CPU instead of its busy previous CPU if no idle CPU
 * turns up: for a synchronous wakeup when the waking CPU runs only the waker,
 * else when it runs no more tasks than the previous CPU. A thread which wakes
 * others and then waits for them, e.g. a dispatcher waking its workers, gets
 * them on its CPU, where they run as soon as it blocks, instead of behind a
 * busy task on their previous CPU for the rest of its slice. The fair class
 * gets there through wake_affine() and its load averages.
 */
static bool pfs_wake_on_waker(struct task_struct *p, int this_cpu,
			      int prev_cpu, int sync)
{
	unsigned int nr = cpu_rq(this_cpu)->nr_running;

	if (!cpumask_test_cpu(this_cpu, p->cpus_ptr) ||
	    !cpus_share_cache(this_cpu, prev_cpu))
		return false;
	if (sync && nr == 1)
		return true;
	return pfs_feat(PFS_F_WAKE_WAKER) &&
	       nr <= READ_ONCE(cpu_rq(prev_cpu)->nr_running);
}

static int select_task_rq_pfs(struct task_struct *p, int prev_cpu, int wake_flags)
{
	int this_cpu = smp_processor_id();
	int sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	int target = prev_cpu;

	if (!(wake_flags & (WF_TTWU | WF_FORK | WF_EXEC)))
		return prev_cpu;

	/* it would wait for a slot, don't wake an idle CPU for it */
	if (atomic_read(&pfs_nr_limited) && pfs_path_full(task_pfs_group(p)) &&
	    cpumask_test_cpu(prev_cpu, p->cpus_ptr))
		return prev_cpu;

	guard(rcu)();

	if (wake_flags & (WF_FORK | WF_EXEC))
		return pfs_find_idlest_cpu(p, this_cpu);

	pfs_record_wakee(p);

	if ((wake_flags & WF_CURRENT_CPU) &&
	    cpumask_test_cpu(this_cpu, p->cpus_ptr))
		return this_cpu;

	if (!pfs_feat(PFS_F_WAKE_AFFINE)) {
		int cpu;

		if (cpumask_test_cpu(prev_cpu, p->cpus_ptr) && pfs_cpu_idle(prev_cpu))
			return prev_cpu;
		if (pfs_wake_on_waker(p, this_cpu, prev_cpu, sync))
			target = this_cpu;
		cpu = pfs_find_idle_cpu(p, target);
		return cpu >= 0 ? cpu : target;
	}

	if (this_cpu != prev_cpu && !pfs_wake_wide(p) &&
	    cpumask_test_cpu(this_cpu, p->cpus_ptr) &&
	    cpus_share_cache(this_cpu, prev_cpu))
		target = pfs_wake_affine(p, this_cpu, prev_cpu, sync);

	return pfs_select_idle_sibling(p, prev_cpu, target);
}

/*
 * Periodic balancing, a simplified sched_balance_rq() of fair.c: every
 * balance interval a CPU compares its PFS load (the effective weights of its
 * admitted tasks) with the busiest CPU of its LLC, then of the machine, and
 * pulls queued tasks which aren't cache hot while that reduces the
 * imbalance by more than imbalance_pct. Runs from SCHED_SOFTIRQ, raised by
 * the tick; idle CPUs pull in their newly idle pick and are pushed to.
 */
#define PFS_BALANCE_BUSY_MS	32
#define PFS_BALANCE_IDLE_MS	4
#define PFS_IMBALANCE_PCT	117
#define PFS_BALANCE_MAX_MOVE	8

void pfs_balance_trigger(struct rq *rq)
{
	if (pfs_feat(PFS_F_BALANCE) &&
	    time_after_eq(jiffies, READ_ONCE(rq->pfs.next_balance)) &&
	    cpu_active(cpu_of(rq)))
		raise_softirq(SCHED_SOFTIRQ);
}

/* @p ran on @rq less than sched_migration_cost ago */
static bool pfs_cache_hot(struct rq *rq, struct task_struct *p)
{
	s64 delta = rq_clock_task(rq) - p->se.exec_start;

	return p->se.exec_start && delta < (s64)sysctl_sched_migration_cost;
}

static struct rq *pfs_find_busiest_load(struct rq *this_rq,
					const struct cpumask *span,
					unsigned long *busiest_load)
{
	int this_cpu = cpu_of(this_rq), cpu;
	struct rq *busiest = NULL;
	unsigned long max = 0;

	for_each_cpu(cpu, span) {
		struct rq *rq = cpu_rq(cpu);
		unsigned long load;

		if (cpu == this_cpu || !pfs_nr_waiting_on_tree(rq))
			continue;
		load = READ_ONCE(rq->pfs.load);
		if (load > max) {
			max = load;
			busiest = rq;
		}
	}
	*busiest_load = max;
	return busiest;
}

/* Pull up to @imbalance of load from @src, with both run queues locked. */
static int pfs_detach_attach(struct rq *this_rq, struct rq *src, long imbalance)
{
	struct rb_node *node = rb_last(&src->pfs.tasks_timeline.rb_root);
	int this_cpu = cpu_of(this_rq), moved = 0, scanned = 0;

	while (node && imbalance > 0 && moved < PFS_BALANCE_MAX_MOVE &&
	       scanned++ < 2 * PFS_MIGRATE_SCAN) {
		struct sched_pfs_entity *pe = __node_2_pe(node);
		struct task_struct *p = pfs_task_of(pe);

		node = rb_prev(node);
		if (!pfs_can_migrate(src, p, this_cpu) || pfs_cache_hot(src, p))
			continue;
		/* moving it must not just mirror the imbalance */
		if ((long)pe->weight.weight >= imbalance * 2)
			continue;
		imbalance -= pe->weight.weight;
		move_queued_task_locked(src, this_rq, p);
		pfs_stat(PFS_STAT_BALANCE);
		moved++;
	}
	return moved;
}

static void pfs_balance_one(struct rq *this_rq, const struct cpumask *span)
{
	unsigned long this_load, busiest_load, this_nr, busiest_nr;
	struct rq *busiest;
	unsigned long flags;
	long imbalance;

	busiest = pfs_find_busiest_load(this_rq, span, &busiest_load);
	if (!busiest)
		return;
	this_load = READ_ONCE(this_rq->pfs.load);
	if (busiest_load * 100 <= this_load * PFS_IMBALANCE_PCT)
		return;
	/*
	 * The load is instantaneous, not averaged as in the fair class: only
	 * act on an imbalance in the number of tasks too, so that the
	 * moment-to-moment noise of wakeup-heavy loads doesn't migrate tasks.
	 * A load more than twice as high is enough on its own if the busiest
	 * CPU has two tasks; with equal weights that implies two tasks more,
	 * but it lets a heavy task (the slot holder of a limited group
	 * carries its group's weight) get a CPU of its own instead of sharing
	 * one with light tasks.
	 */
	this_nr = READ_ONCE(this_rq->pfs.nr_queued);
	busiest_nr = READ_ONCE(busiest->pfs.nr_queued);
	if (!(pfs_feat(PFS_F_BALANCE_COUNT) && busiest_nr >= this_nr + 2) &&
	    !(pfs_feat(PFS_F_BALANCE_WEIGHT) && busiest_nr >= 2 &&
	      busiest_load > 2 * this_load))
		return;
	imbalance = (busiest_load - this_load) / 2;

	local_irq_save(flags);
	double_rq_lock(this_rq, busiest);
	update_rq_clock(this_rq);
	update_rq_clock(busiest);
	pfs_detach_attach(this_rq, busiest, imbalance);
	double_rq_unlock(this_rq, busiest);
	local_irq_restore(flags);
}

void pfs_balance_softirq(void)
{
	struct rq *this_rq = this_rq();
	int this_cpu = cpu_of(this_rq);
	bool idle = !READ_ONCE(this_rq->pfs.nr_queued);
	unsigned long interval;

	interval = msecs_to_jiffies(idle ? PFS_BALANCE_IDLE_MS : PFS_BALANCE_BUSY_MS);
	WRITE_ONCE(this_rq->pfs.next_balance, jiffies + max(interval, 1UL));
	if (!cpu_active(this_cpu))
		return;

	rcu_read_lock();
	pfs_balance_one(this_rq, pfs_llc_span(this_cpu));
	/* across LLCs at a quarter of the rate */
	if (!(jiffies & 3) && pfs_llc_span(this_cpu) != cpu_active_mask)
		pfs_balance_one(this_rq, cpu_active_mask);
	rcu_read_unlock();
}

static DEFINE_PER_CPU(struct balance_callback, pfs_push_head);

/*
 * An idle CPU @p may move to from @rq. Idle CPUs don't run the periodic
 * balancer (NOHZ), so the busy ones push to them.
 */
static int pfs_find_push_target(struct rq *rq, struct task_struct *p)
{
	int this_cpu = cpu_of(rq), cpu;

	cpu = pfs_find_idle_cpu(p, this_cpu);
	if (cpu >= 0 && cpu != this_cpu)
		return cpu;
	return -1;
}

/*
 * Balance callback after a context switch: move one queued task to an idle
 * CPU.
 */
static void pfs_push(struct rq *rq)
{
	struct pfs_rq *prq = &rq->pfs;
	struct task_struct *p;
	struct rq *dst;
	int cpu;

	bool preempted = prq->push_preempted;

	prq->next_push = jiffies + PFS_PUSH_INTERVAL;
	prq->push_preempted = 0;

	if (prq->nr_queued < 2 || !cpu_active(cpu_of(rq)))
		return;

	/*
	 * A task holding slots which another task of this CPU preempted
	 * leaves its group's slot unused while it waits here: move it to an
	 * idle CPU first.
	 */
	rcu_read_lock();
	p = preempted ? pfs_pick_holder(rq) : NULL;
	cpu = p ? pfs_find_idle_cpu(p, cpu_of(rq)) : -1;
	if (cpu < 0 || cpu == cpu_of(rq)) {
		p = pfs_pick_migratable(rq, -1, ULONG_MAX);
		cpu = p ? pfs_find_push_target(rq, p) : -1;
	}
	rcu_read_unlock();
	if (cpu < 0)
		return;

	dst = cpu_rq(cpu);
	get_task_struct(p);
	double_lock_balance(rq, dst);
	if (task_rq(p) == rq && task_on_rq_queued(p) && p->pfs.on_tree &&
	    pfs_can_migrate(rq, p, cpu)) {
		move_queued_task_locked(rq, dst, p);
		pfs_stat(PFS_STAT_PUSH);
	}
	double_unlock_balance(rq, dst);
	put_task_struct(p);
}

static void pfs_queue_push(struct rq *rq)
{
	struct pfs_rq *prq = &rq->pfs;

	if (prq->nr_queued < 2)
		return;
	if (!prq->push_preempted && time_before(jiffies, prq->next_push))
		return;
	queue_balance_callback(rq, &per_cpu(pfs_push_head, cpu_of(rq)), pfs_push);
}

/*
 * The previous task's slice ended while others wait for its slots: it gives
 * them back now, before the pick, and is admitted again or parked in
 * put_prev_task_pfs().
 */
static void pfs_rotate_out(struct rq *rq, struct task_struct *prev)
{
	struct sched_pfs_entity *pe = &prev->pfs;

	lockdep_assert_rq_held(rq);
	pe->rotate = 0;
	if (pe->state != PFS_QUEUED || !task_on_rq_queued(prev))
		return;

	update_curr_pfs(rq);
	pfs_unqueue_task(rq, prev, 0);
	pfs_release_slots(rq, prev, true);
	pe->state = PFS_PARKING;
	pfs_stat(PFS_STAT_ROTATE);

	/*
	 * A task cut short in the burst it woke up for (e.g. by the budget of
	 * a fractional limit) keeps its wakeup priority for a couple of
	 * rounds, see pfs_next_waiter().
	 */
	if (pe->fresh && pe->fresh_rounds < PFS_FRESH_ROUNDS) {
		pe->woken = 1;
		pe->fresh_rounds++;
	}
}

/*
 * Same-CPU hand-off: the previous task of this CPU gave back slots of
 * rq->pfs.handoff's path while tasks wait for them. Admit the first waiting
 * task of the deepest level which has waiting tasks and pull it here, so that
 * it runs on the CPU which was just freed. A waiting task which can't run
 * here is admitted on its own CPU instead.
 *
 * Returns 1 if a task was queued here, 0 if not, -1 if a higher class became
 * runnable while the run queue lock was dropped.
 */
static int pfs_handoff(struct rq *rq, struct rq_flags *rf)
{
	struct pfs_group *leaf = rq->pfs.handoff, *lg;
	int this_cpu = cpu_of(rq), tries = 0, got = 0;
	bool unpinned = false;

	lockdep_assert_rq_held(rq);
	rq->pfs.handoff = NULL;

	for_each_pfs_level(lg, leaf) {
		while (!got && READ_ONCE(lg->nr_waiting) &&
		       tries++ < PFS_HANDOFF_TRIES) {
			struct task_struct *p;
			struct rq *src;
			int ret;

			/* waiting tasks are freed after a grace period */
			p = pfs_next_waiter(lg);
			if (!p)
				break;
			src = task_rq(p);

			if (src == rq) {
				if (!pfs_waiting_at(rq, p, lg))
					continue;
				if (!sched_task_cpu_allowed(p, this_cpu))
					break;
				ret = pfs_try_waiter(rq, p, lg);
				if (ret < 0)
					break;
				if (ret > 0) {
					pfs_unpark(rq, p);
					pfs_queue_task(rq, p, 0);
					pfs_stat(PFS_STAT_HANDOFF_LOCAL);
					got = 1;
				}
				continue;
			}

			if (!unpinned) {
				rq_unpin_lock(rq, rf);
				rq_modified_begin(rq, &pfs_sched_class);
				unpinned = true;
			}

			double_lock_balance(rq, src);
			if (!pfs_waiting_at(src, p, lg)) {
				double_unlock_balance(rq, src);
				continue;
			}

			update_rq_clock(src);
			if (pfs_can_migrate(src, p, this_cpu)) {
				ret = pfs_try_waiter(src, p, lg);
				if (ret > 0) {
					/* keeps its slots, enqueue_task_pfs() */
					pfs_unpark(src, p);
					p->pfs.granted = 1;
					deactivate_task(src, p, 0);
					set_task_cpu(p, this_cpu);
					activate_task(rq, p, 0);
					pfs_stat(PFS_STAT_HANDOFF);
					got = 1;
				}
			} else if (sched_task_cpu_allowed(p, cpu_of(src))) {
				ret = pfs_try_waiter(src, p, lg);
				if (ret > 0)
					pfs_admit_in_place(src, p);
			} else {
				/* hotplug moves it, see pfs_get_dying_waiter() */
				ret = -1;
			}
			double_unlock_balance(rq, src);

			if (ret < 0)
				break;
		}
	}

	/* slots left over, e.g. no waiting task could run here */
	pfs_retry_path(leaf);

	if (unpinned) {
		rq_repin_lock(rq, rf);
		if (rq_modified_above(rq, &pfs_sched_class))
			return -1;
	}
	return got;
}

static int balance_pfs(struct rq *rq, struct rq_flags *rf)
{
	struct task_struct *prev = rq->donor;

	if (prev->sched_class == &pfs_sched_class && prev->pfs.rotate)
		pfs_rotate_out(rq, prev);

	if (rq->pfs.handoff && pfs_handoff(rq, rf) < 0)
		return 1;

	return rq->pfs.nr_queued || sched_stop_runnable(rq) ||
	       sched_dl_runnable(rq) || sched_rt_runnable(rq);
}

/**************************************************************
 * Task groups.
 */

void pfs_init_group(struct task_group *tg, struct task_group *parent)
{
	struct pfs_group *g = &tg->pfs;

	raw_spin_lock_init(&g->lock);
	raw_spin_lock_init(&g->weight_lock);
	INIT_LIST_HEAD(&g->waiters);
	INIT_LIST_HEAD(&g->wake_waiters);
	g->depth = parent ? parent->pfs.depth + 1 : 0;
	g->weight = NICE_0_LOAD;
	hrtimer_setup(&g->window_timer, pfs_window_timer_fn, CLOCK_MONOTONIC,
		      HRTIMER_MODE_ABS_PINNED_HARD);
	g->retry_work = IRQ_WORK_INIT_HARD(pfs_retry_workfn);
}

/*
 * A cgroup's task group is going away (css_free, which may sleep): it has no
 * tasks left in its subtree and its files are gone, so nothing arms its
 * timer or queues its retry anymore. Wait for those still in flight.
 */
void pfs_offline_group(struct task_group *tg)
{
	struct pfs_group *g = &tg->pfs;

	if (!pfs_enabled())
		return;
	hrtimer_cancel(&g->window_timer);
	irq_work_sync(&g->retry_work);
}

/*
 * Called from an RCU callback. Cgroups went through pfs_offline_group();
 * autogroups have no cpu.max, so their timer and retry are never used.
 */
void pfs_free_group(struct task_group *tg)
{
	struct pfs_group *g = &tg->pfs;

	if (g->limit)
		atomic_dec(&pfs_nr_limited);
	if (WARN_ON_ONCE(hrtimer_active(&g->window_timer)))
		hrtimer_cancel(&g->window_timer);
	/* can't sleep here; irq_work isn't stranded, see pfs_queue_retry() */
	if (WARN_ON_ONCE(irq_work_is_busy(&g->retry_work))) {
		while (irq_work_is_busy(&g->retry_work))
			cpu_relax();
	}
	WARN_ON_ONCE(!list_empty(&g->waiters) || !list_empty(&g->wake_waiters));
}

/*
 * cpu.max "quota period" is a limit of quota / period CPUs, kept in
 * milli-CPUs. A limit as large as the machine is no limit.
 */
void pfs_group_set_bandwidth(struct task_group *tg, u64 period_us,
			     u64 quota_us, u64 burst_us)
{
	struct pfs_group *g = &tg->pfs;
	u64 limit = 0;
	unsigned int old;

	if (!pfs_enabled())
		return;

	if (quota_us != RUNTIME_INF && period_us) {
		limit = div64_u64(quota_us * 1000 + period_us - 1, period_us);
		if (limit >= (u64)num_possible_cpus() * 1000)
			limit = 0;
	}

	old = xchg(&g->limit, (unsigned int)limit);
	if (!old && limit)
		atomic_inc(&pfs_nr_limited);
	else if (old && !limit)
		atomic_dec(&pfs_nr_limited);

	/* the limit may have grown */
	if (READ_ONCE(g->nr_waiting))
		pfs_group_retry(g);
}

/* cpu.weight changed: tg->shares holds the new weight */
void pfs_group_set_weight(struct task_group *tg)
{
	struct pfs_group *g = &tg->pfs, *parent;
	unsigned long w, flags;

	if (!pfs_enabled() || !tg->parent)
		return;

	parent = pfs_parent(g);
	w = max(READ_ONCE(tg->shares), scale_load(MIN_SHARES));

	raw_spin_lock_irqsave(&parent->weight_lock, flags);
	if (g->nr_active > 0)
		parent->child_weight_sum += w - g->weight;
	g->weight = w;
	raw_spin_unlock_irqrestore(&parent->weight_lock, flags);
}

/**************************************************************
 * Setup.
 */

void pfs_init_entity(struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;

	RB_CLEAR_NODE(&pe->run_node);
	INIT_LIST_HEAD(&pe->wait_node);
	INIT_LIST_HEAD(&pe->rq_wait_node);
	pe->state = PFS_NONE;
	pe->on_tree = 0;
	pe->granted = 0;
	pe->rotate = 0;
	pe->rel_deadline = 0;
	pe->unslotted = 0;
	pe->woken = 0;
	pe->wake_list = 0;
	pe->fresh = 0;
	pe->fresh_rounds = 0;
	pe->charge_pending = 0;
	pe->sleep_stamp = 0;
	pe->slept = U64_MAX;
	init_task_work(&pe->user_work, pfs_user_workfn);
	/* not queued, see pfs_has_user_work() */
	pe->user_work.next = &pe->user_work;
	pe->slot_mask = 0;
	pe->grp = NULL;
	pe->wait_grp = NULL;
	pe->vlag = 0;
	pe->vruntime = 0;
	pe->deadline = 0;
	pe->weight.weight = 0;
	pe->weight.inv_weight = 0;
	pe->slice = sysctl_sched_base_slice;
}

static struct task_struct *
pfs_server_pick_task(struct sched_dl_entity *dl_se, struct rq_flags *rf)
{
	return pick_task_pfs(dl_se->rq, rf);
}

/* Returns true if the rq's fair_server now serves PFS. */
bool __init pfs_init_rq(struct rq *rq)
{
	struct pfs_rq *prq = &rq->pfs;

	prq->tasks_timeline = RB_ROOT_CACHED;
	prq->zero_vruntime = (u64)(-(1LL << 20));
	INIT_LIST_HEAD(&prq->waiters);

	if (!pfs_enabled())
		return false;

	pr_info_once("sched: Parallel Fair Scheduler enabled\n");

	/* the fair_server serves the normal class, which is PFS now */
	init_dl_entity(&rq->fair_server);
	dl_server_init(&rq->fair_server, rq, pfs_server_pick_task);
	return true;
}

/* A waiting task assigned to @rq, which went offline, with a reference. */
struct task_struct *pfs_get_dying_waiter(struct rq *rq)
{
	struct task_struct *p = NULL;
	struct sched_pfs_entity *pe;
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	pe = list_first_entry_or_null(&rq->pfs.waiters, struct sched_pfs_entity,
				      rq_wait_node);
	if (pe) {
		p = pfs_task_of(pe);
		get_task_struct(p);
	}
	rq_unlock_irqrestore(rq, &rf);
	return p;
}

/**************************************************************
 * Debugfs: /sys/kernel/debug/sched/pfs/
 */

static int pfs_stats_show(struct seq_file *m, void *v)
{
	unsigned long sum[PFS_NR_STATS] = { };
	unsigned int nr_waiting = 0;
	int cpu, i;

	for_each_possible_cpu(cpu) {
		for (i = 0; i < PFS_NR_STATS; i++)
			sum[i] += per_cpu(pfs_stats, cpu)[i];
		nr_waiting += READ_ONCE(cpu_rq(cpu)->pfs.nr_waiting);
	}
	seq_printf(m, "enabled %d\nlimited_groups %d\nwaiting %u\n",
		   pfs_enabled() ? 1 : 0, atomic_read(&pfs_nr_limited), nr_waiting);
	for (i = 0; i < PFS_NR_STATS; i++)
		seq_printf(m, "%s %lu\n", pfs_stat_names[i], sum[i]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pfs_stats);

static int pfs_groups_show(struct seq_file *m, void *v)
{
	struct task_group *tg;
	char *path;

	path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	seq_puts(m, "# limit_mcpu slots waiting window_used_us weight hweight active path\n");
	rcu_read_lock();
	list_for_each_entry_rcu(tg, &task_groups, list) {
		struct pfs_group *g = &tg->pfs;

		if (!READ_ONCE(g->limit) && !READ_ONCE(g->nr_waiting))
			continue;
		if (!autogroup_path(tg, path, PATH_MAX))
			cgroup_path(tg->css.cgroup, path, PATH_MAX);
		seq_printf(m, "%u %d %u %llu %lu %lu %d %s\n", READ_ONCE(g->limit),
			   atomic_read(&g->nr_slots), READ_ONCE(g->nr_waiting),
			   (u64)atomic64_read(&g->window_used) / NSEC_PER_USEC,
			   scale_load_down(READ_ONCE(g->weight)),
			   scale_load_down(READ_ONCE(g->hweight)),
			   READ_ONCE(g->nr_active), path);
	}
	rcu_read_unlock();
	kfree(path);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pfs_groups);

/* waiting tasks per CPU, for debugging */
static int pfs_waiters_show(struct seq_file *m, void *v)
{
	int cpu;

	seq_puts(m, "# cpu pid on_rq on_cpu state slots group_depth group_waiting group_slots group_limit comm\n");
	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);
		struct sched_pfs_entity *pe;
		struct rq_flags rf;

		rq_lock_irqsave(rq, &rf);
		list_for_each_entry(pe, &rq->pfs.waiters, rq_wait_node) {
			struct task_struct *p = pfs_task_of(pe);
			struct pfs_group *lg = pe->wait_grp;

			seq_printf(m, "%d %d %d %d %d %llx %d %u %d %u %s\n", cpu,
				   task_pid_nr(p), p->on_rq, p->on_cpu, pe->state,
				   pe->slot_mask, lg ? lg->depth : -1,
				   lg ? READ_ONCE(lg->nr_waiting) : 0,
				   lg ? atomic_read(&lg->nr_slots) : 0,
				   lg ? READ_ONCE(lg->limit) : 0, p->comm);
		}
		rq_unlock_irqrestore(rq, &rf);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pfs_waiters);

static int pfs_window_us_get(void *data, u64 *val)
{
	*val = div_u64(READ_ONCE(pfs_window_ns), NSEC_PER_USEC);
	return 0;
}

static int pfs_window_us_set(void *data, u64 val)
{
	if (val < 1000 || val > USEC_PER_SEC)
		return -EINVAL;
	WRITE_ONCE(pfs_window_ns, val * NSEC_PER_USEC);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(pfs_window_us_fops, pfs_window_us_get,
			 pfs_window_us_set, "%llu\n");

void pfs_debugfs_init(struct dentry *parent)
{
	struct dentry *d;

	if (!pfs_enabled())
		return;

	d = debugfs_create_dir("pfs", parent);
	debugfs_create_file("stats", 0400, d, NULL, &pfs_stats_fops);
	debugfs_create_file("groups", 0400, d, NULL, &pfs_groups_fops);
	debugfs_create_file("waiters", 0400, d, NULL, &pfs_waiters_fops);
	debugfs_create_file_unsafe("window_us", 0644, d, NULL, &pfs_window_us_fops);
	debugfs_create_u32("features", 0644, d, &pfs_features);
	debugfs_create_u32("wakeup_gran_us", 0644, d, &pfs_wakeup_gran_us);
}

#ifdef CONFIG_SCHED_CORE
/*
 * Core scheduling: order two PFS tasks of SMT siblings by their virtual
 * runtimes, each relative to the zero_vruntime of its CPU (the fair class
 * uses a snapshot taken at forced idle, see cfs_prio_less()). Returns true
 * if @a has less right to run than @b.
 */
bool pfs_prio_less(const struct task_struct *a, const struct task_struct *b,
		   bool in_fi)
{
	s64 va = (s64)(a->pfs.vruntime - task_rq(a)->pfs.zero_vruntime);
	s64 vb = (s64)(b->pfs.vruntime - task_rq(b)->pfs.zero_vruntime);

	return va > vb;
}

/* A task waiting for a slot is in the core tree, but must not be picked. */
static int task_is_throttled_pfs(struct task_struct *p, int cpu)
{
	return p->pfs.state == PFS_WAITING || p->pfs.state == PFS_PARKING;
}
#endif

bool pfs_can_stop_tick(struct rq *rq)
{
	struct task_struct *curr = rq->curr;

	if (rq->pfs.nr_queued > 1)
		return false;
	/* the tick rotates the slots of a limited group, see task_tick_pfs() */
	return curr->sched_class != &pfs_sched_class || !curr->pfs.slot_mask;
}

DEFINE_SCHED_CLASS(pfs) = {
	.enqueue_task		= enqueue_task_pfs,
	.dequeue_task		= dequeue_task_pfs,
	.yield_task		= yield_task_pfs,

	.wakeup_preempt		= wakeup_preempt_pfs,

	.balance		= balance_pfs,
	.pick_task		= pick_task_pfs,
	.put_prev_task		= put_prev_task_pfs,
	.set_next_task		= set_next_task_pfs,

	.select_task_rq		= select_task_rq_pfs,
	.set_cpus_allowed	= set_cpus_allowed_common,

	.task_tick		= task_tick_pfs,

	.reweight_task		= reweight_task_pfs,
	.prio_changed		= prio_changed_pfs,
	.switched_from		= switched_from_pfs,
	.switched_to		= switched_to_pfs,

	.get_rr_interval	= get_rr_interval_pfs,

	.update_curr		= update_curr_pfs,

#ifdef CONFIG_SCHED_CORE
	.task_is_throttled	= task_is_throttled_pfs,
#endif

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 1,
#endif
};
