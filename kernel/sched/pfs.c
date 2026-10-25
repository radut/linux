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

#include <uapi/linux/sched/types.h>

#include "sched.h"
#include "stats.h"
#include "autogroup.h"

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
	PFS_STAT_ROTATE,	/* gave its slots back at the end of a slice */
	PFS_STAT_HANDOFF,	/* waiting task pulled to the CPU that freed slots */
	PFS_STAT_HANDOFF_LOCAL,	/* ... which was already on that CPU */
	PFS_STAT_INPLACE,	/* waiting task admitted on its own CPU */
	PFS_STAT_RETRY,		/* retry runs */
	PFS_STAT_PULL,		/* newly idle pulls */
	PFS_STAT_PUSH,		/* pushes */
	PFS_STAT_KICK,		/* idle CPUs kicked */
	PFS_NR_STATS,
};

static const char * const pfs_stat_names[PFS_NR_STATS] = {
	"admit", "park_full", "park_budget", "park_fifo", "rotate", "handoff",
	"handoff_local", "inplace", "retry", "pull", "push", "kick",
};

static DEFINE_PER_CPU(unsigned long [PFS_NR_STATS], pfs_stats);

static inline void pfs_stat(enum pfs_stat idx)
{
	this_cpu_inc(pfs_stats[idx]);
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
static struct sched_pfs_entity *pfs_pick_eevdf(struct pfs_rq *prq)
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

static inline u64 pfs_budget(unsigned int limit)
{
	return div_u64((u64)limit * pfs_window_ns, 1000);
}

/*
 * The part of @lg's budget left in the current window. Windows are aligned
 * to multiples of the window length so that one timer per group finds the
 * budget refilled. Usage beyond the budget is carried into the next window,
 * up to one budget, so that the average stays at the limit.
 */
static u64 pfs_window_left(struct pfs_group *lg, u64 now)
{
	u64 id = div64_u64(now, pfs_window_ns);
	s64 old = atomic64_read(&lg->window_id);
	u64 budget = pfs_budget(READ_ONCE(lg->limit));
	u64 used;

	if ((u64)old != id && atomic64_try_cmpxchg(&lg->window_id, &old, id)) {
		used = atomic64_read(&lg->window_used);
		if (id == (u64)old + 1 && used > budget)
			used = min(used - budget, budget);
		else
			used = 0;
		atomic64_set(&lg->window_used, used);
	}

	used = atomic64_read(&lg->window_used);
	return used < budget ? budget - used : 0;
}

/*
 * Take one slot at the limited level @lg. The first floor(limit) slots are
 * free, the last slot of a fractional limit needs budget left in the window,
 * and clips *@run_limit to it.
 */
static enum pfs_reserve pfs_take_slot(struct pfs_group *lg, u64 now,
				      u64 *run_limit)
{
	unsigned int limit = READ_ONCE(lg->limit);
	unsigned int taken;
	u64 left, min_left;

	taken = atomic_inc_return(&lg->nr_slots) - 1;
	if (taken >= pfs_width(limit)) {
		atomic_dec(&lg->nr_slots);
		return PFS_RES_FULL;
	}

	if (taken >= limit / 1000) {
		left = pfs_window_left(lg, now);
		min_left = min_t(u64, pfs_budget(limit), PFS_MIN_BUDGET_NS);
		if (!left || left < min_left) {
			atomic_dec(&lg->nr_slots);
			return PFS_RES_BUDGET;
		}
		*run_limit = min(*run_limit, left);
	}
	return PFS_RES_OK;
}

static void pfs_queue_retry(struct pfs_group *lg);

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
 * slot is taken, or became limited after @p took its slots.
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
		if (taken > limit / 1000) {
			if (!now)
				now = pfs_now();
			if (!pfs_window_left(lg, now))
				return true;
		}
	}
	return false;
}

/* charge @delta of runtime to the budget windows of @g's limited levels */
static void pfs_charge(struct pfs_group *g, u64 delta)
{
	struct pfs_group *lg;
	u64 now = 0;

	for_each_pfs_level(lg, g) {
		if (!pfs_level_limited(lg))
			continue;
		if (!now)
			now = pfs_now();
		pfs_window_left(lg, now);
		atomic64_add(delta, &lg->window_used);
	}
}

/**************************************************************
 * Waiting.
 */

static enum hrtimer_restart pfs_window_timer_fn(struct hrtimer *timer);
static void pfs_group_retry(struct pfs_group *g);

/* Start @lg's timer for the next window, for tasks waiting for budget. */
static void pfs_arm_window_timer(struct pfs_group *lg)
{
	u64 now, next;

	if (atomic_cmpxchg(&lg->timer_armed, 0, 1))
		return;

	now = pfs_now();
	next = (div64_u64(now, pfs_window_ns) + 1) * pfs_window_ns;
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

/* Admit @lg's waiting tasks soon, from a context without run queue locks. */
static void pfs_queue_retry(struct pfs_group *lg)
{
	irq_work_queue(&lg->retry_work);
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
	pfs_stat(PFS_STAT_KICK);
	cpumask_set_cpu(cpu, this_cpu_ptr(&pfs_kick_mask));
	irq_work_queue(this_cpu_ptr(&pfs_kick_work));
}

static void __pfs_link_waiter(struct sched_pfs_entity *pe, struct pfs_group *lg)
{
	raw_spin_lock(&lg->lock);
	list_add_tail(&pe->wait_node, &lg->waiters);
	WRITE_ONCE(lg->nr_waiting, lg->nr_waiting + 1);
	raw_spin_unlock(&lg->lock);
	pe->wait_grp = lg;
}

static void __pfs_unlink_waiter(struct sched_pfs_entity *pe)
{
	struct pfs_group *lg = pe->wait_grp;

	raw_spin_lock(&lg->lock);
	list_del_init(&pe->wait_node);
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
		pfs_arm_window_timer(lg);
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

	pe->state = PFS_WAITING;
	__pfs_link_waiter(pe, lg);
	list_add_tail(&pe->rq_wait_node, &rq->pfs.waiters);
	rq->pfs.nr_waiting++;
	pfs_waiter_linked(lg, why);
}

static void pfs_unpark(struct rq *rq, struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;

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
	enum pfs_reserve ret;

	lockdep_assert_rq_held(rq);

	ret = pfs_reserve(p, g, false, &fail);
	if (ret == PFS_RES_OK)
		return 1;

	if (fail == lg) {
		if (ret == PFS_RES_BUDGET)
			pfs_arm_window_timer(lg);
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

static struct task_struct *pfs_first_waiter(struct pfs_group *lg)
{
	struct sched_pfs_entity *pe;
	unsigned long flags;

	raw_spin_lock_irqsave(&lg->lock, flags);
	pe = list_first_entry_or_null(&lg->waiters, struct sched_pfs_entity,
				      wait_node);
	raw_spin_unlock_irqrestore(&lg->lock, flags);
	return pe ? pfs_task_of(pe) : NULL;
}

static void pfs_queue_task(struct rq *rq, struct task_struct *p, int flags);

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
		p = pfs_first_waiter(lg);
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

/* protects pfs_group::weight, ::nr_active and ::child_weight_sum */
static DEFINE_RAW_SPINLOCK(pfs_weight_lock);
/* bumped when the active groups or a weight change, expires hweights */
static atomic_long_t pfs_hweight_gen = ATOMIC_LONG_INIT(1);

#define PFS_HWEIGHT_REFRESH_NS	NSEC_PER_MSEC

/*
 * Count @g in or out of its parent's active weight sum and propagate while
 * the parents' states change. Increments and decrements commute, so the
 * races of a group's tasks going in and out from several CPUs leave the sums
 * consistent: a group's weight is in its parent's sum exactly while its
 * nr_active is positive.
 */
static void pfs_update_active(struct pfs_group *g, bool active)
{
	struct pfs_group *parent;
	bool propagate;

	for (; g->depth > 0; g = parent) {
		parent = pfs_parent(g);
		propagate = false;

		raw_spin_lock(&pfs_weight_lock);
		if (active) {
			if (!g->nr_active++) {
				parent->child_weight_sum += g->weight;
				propagate = true;
			}
		} else {
			if (!--g->nr_active) {
				parent->child_weight_sum -= g->weight;
				propagate = true;
			}
		}
		raw_spin_unlock(&pfs_weight_lock);

		if (!propagate)
			break;
	}
	atomic_long_inc(&pfs_hweight_gen);
}

static inline unsigned long pfs_group_load(struct pfs_group *g)
{
	return READ_ONCE(g->child_weight_sum) + atomic_long_read(&g->own_weight);
}

/*
 * @g's hierarchical weight, recomputed when the generation changed but at
 * most once per PFS_HWEIGHT_REFRESH_NS: on a busy host groups go idle and
 * busy all the time. The inputs are read without the lock, the result is
 * approximate by nature.
 */
static unsigned long pfs_hweight(struct pfs_group *g, unsigned long gen, u64 now)
{
	struct pfs_group *parent = pfs_parent(g);
	unsigned long h, d;

	if (READ_ONCE(g->hweight_gen) == gen)
		return READ_ONCE(g->hweight);
	if (READ_ONCE(g->hweight) &&
	    now - READ_ONCE(g->hweight_at) < PFS_HWEIGHT_REFRESH_NS)
		return READ_ONCE(g->hweight);

	h = READ_ONCE(g->weight);
	if (parent->depth > 0 && g->depth <= PFS_MAX_DEPTH) {
		d = pfs_group_load(parent);
		if (d)
			h = mul_u64_u64_div_u64(pfs_hweight(parent, gen, now), h, d);
	}
	h = max(h, scale_load(MIN_SHARES));

	WRITE_ONCE(g->hweight, h);
	WRITE_ONCE(g->hweight_at, now);
	WRITE_ONCE(g->hweight_gen, gen);
	return h;
}

/* The weight @p is scheduled with, @p is counted in its group. */
static unsigned long pfs_effective_weight(struct task_struct *p)
{
	struct sched_pfs_entity *pe = &p->pfs;
	struct pfs_group *g = pe->grp;
	unsigned long w = p->se.load.weight, d;

	if (!g || !g->depth)
		return w;

	d = pfs_group_load(g);
	if (d)
		w = mul_u64_u64_div_u64(pfs_hweight(g, atomic_long_read(&pfs_hweight_gen),
						    pfs_now()), w, d);
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

	w = pfs_effective_weight(curr);
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

/* Put an admitted task on @rq: the tree, or as the running task. */
static void pfs_queue_task(struct rq *rq, struct task_struct *p, int flags)
{
	struct pfs_rq *prq = &rq->pfs;
	struct sched_pfs_entity *pe = &p->pfs;
	unsigned long w;

	pfs_account(p);
	w = pfs_effective_weight(p);
	pfs_set_weight(pe, w);

	pfs_place_entity(prq, pe, flags);
	pe->state = PFS_QUEUED;
	if (prq->curr != p)
		__pfs_enqueue_entity(prq, pe);

	pe->rotate = 0;

	if (!prq->nr_queued++)
		dl_server_start(&rq->fair_server);
	prq->load += w;
	add_nr_running(rq, 1);
}

/* Take an admitted task off @rq, keeping its lag. */
static void pfs_unqueue_task(struct rq *rq, struct task_struct *p, int flags)
{
	struct pfs_rq *prq = &rq->pfs;
	struct sched_pfs_entity *pe = &p->pfs;

	pfs_update_entity_lag(prq, pe);
	if (!(flags & DEQUEUE_SLEEP)) {
		pe->deadline -= pe->vruntime;
		pe->rel_deadline = 1;
	}
	if (pe->on_tree)
		__pfs_dequeue_entity(prq, pe);

	pe->state = PFS_NONE;
	prq->nr_queued--;
	prq->load -= pe->weight.weight;
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

	pe->slot_mask = 0;
	pe->granted = 0;
	if (!mask || !pfs_put_slots(pe->grp, mask, !handoff) || !handoff)
		return;

	if (rq->pfs.handoff && rq->pfs.handoff != pe->grp)
		pfs_retry_path(rq->pfs.handoff);
	rq->pfs.handoff = pe->grp;
}

/* Admit @p on @rq if it gets its slots, otherwise make it wait. */
static void pfs_admit_or_park(struct rq *rq, struct task_struct *p, int flags)
{
	struct pfs_group *g = task_pfs_group(p), *fail = NULL;
	enum pfs_reserve ret;

	ret = pfs_reserve(p, g, true, &fail);
	if (ret == PFS_RES_OK)
		pfs_queue_task(rq, p, flags);
	else
		pfs_park(rq, p, fail, ret);
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

static void update_curr_pfs(struct rq *rq)
{
	struct pfs_rq *prq = &rq->pfs;
	struct task_struct *curr = prq->curr;
	struct sched_pfs_entity *pe;
	s64 delta;

	if (!curr)
		return;

	delta = update_curr_common(rq);
	if (delta <= 0)
		return;

	dl_server_update(&rq->fair_server, delta);

	pe = &curr->pfs;
	if (pe->slot_mask)
		pfs_charge(pe->grp, delta);
	if (pe->state != PFS_QUEUED)
		return;

	pe->vruntime += pfs_calc_delta(delta, pe);
	if (pfs_update_deadline(prq, pe) && prq->nr_queued > 1)
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

	/* tasks migrated here aren't cache hot */
	if (flags & ENQUEUE_MIGRATED)
		p->se.exec_start = 0;

	/* kept its slots across a migration or an attribute change */
	if (pe->granted && pe->grp == task_pfs_group(p)) {
		pe->granted = 0;
		pfs_queue_task(rq, p, flags);
		return;
	}
	if (pe->slot_mask)
		pfs_release_slots(rq, p, false);

	pfs_admit_or_park(rq, p, flags);
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

	switch (pe->state) {
	case PFS_QUEUED:
		update_curr_pfs(rq);
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

static void yield_task_pfs(struct rq *rq)
{
	struct task_struct *curr = rq->donor;
	struct sched_pfs_entity *pe = &curr->pfs;
	struct pfs_rq *prq = &rq->pfs;

	if (unlikely(rq->nr_running == 1) || pe->state != PFS_QUEUED)
		return;

	update_rq_clock(rq);
	update_curr_pfs(rq);
	rq_clock_skip_update(rq);

	/* forfeit the rest of the request, if it is owed service at all */
	if (pfs_entity_eligible(prq, pe)) {
		pe->vruntime = pe->deadline;
		pfs_update_deadline(prq, pe);
	}
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

	if (wake_flags & WF_FORK)
		return;

	update_curr_pfs(rq);
	if (pfs_pick_eevdf(prq) == &p->pfs)
		goto preempt;
	return;

preempt:
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

		pe = pfs_pick_eevdf(prq);
		if (pe)
			return pfs_task_of(pe);
	}

	if (sched_core_enabled(rq) || !rf)
		return NULL;

	pulled = pfs_newidle(rq, rf);
	if (pulled < 0)
		return RETRY_TASK;
	if (pulled > 0)
		goto again;
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

	WARN_ON_ONCE(prq->curr != prev);
	prq->curr = NULL;

	switch (pe->state) {
	case PFS_QUEUED:
		if (!pe->on_tree)
			__pfs_enqueue_entity(prq, pe);
		break;
	case PFS_PARKING:
		/*
		 * Its slice ended while others wait: admit it again only if
		 * it doesn't overtake anybody, else it waits at the tail. The
		 * run queue lock is held until it is off the CPU, see
		 * finish_task_switch(), so nobody sees it waiting before.
		 */
		pe->state = PFS_NONE;
		pfs_admit_or_park(rq, prev, 0);
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

static void set_next_task_pfs(struct rq *rq, struct task_struct *p,
			      enum snt_e type)
{
	struct pfs_rq *prq = &rq->pfs;
	struct sched_pfs_entity *pe = &p->pfs;

	if (type == SNT_REPICK)
		return;

	if (pe->on_tree)
		__pfs_dequeue_entity(prq, pe);

	p->se.exec_start = rq_clock_task(rq);
	p->se.prev_sum_exec_runtime = p->se.sum_exec_runtime;
	prq->curr = p;

	if (type == SNT_PICK)
		pfs_queue_push(rq);
}

static void task_tick_pfs(struct rq *rq, struct task_struct *curr, int queued)
{
	struct sched_pfs_entity *pe = &curr->pfs;

	update_curr_pfs(rq);

	if (pe->state != PFS_QUEUED) {
		resched_curr(rq);
		return;
	}

	pfs_reweight_curr(rq, curr);

	/*
	 * A task of a limited group gives its slots back at the end of its
	 * run limit if somebody waits for them, see pfs_should_rotate().
	 */
	if (atomic_read(&pfs_nr_limited) && !pe->rotate &&
	    curr->se.sum_exec_runtime - pe->run_start >= pe->run_limit) {
		if (pfs_should_rotate(curr)) {
			pe->rotate = 1;
			resched_curr(rq);
		} else {
			pe->run_start = curr->se.sum_exec_runtime;
			pe->run_limit = pfs_run_limit(curr);
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

/* The CPU with the most queued, not running tasks, LLC first. */
static struct rq *pfs_find_busiest(struct rq *this_rq)
{
	int this_cpu = cpu_of(this_rq), cpu, pass;
	struct rq *busiest = NULL;
	unsigned int max_nr = 0;

	for (pass = 0; pass < 2 && !busiest; pass++) {
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
	return pulled;
}

static inline bool pfs_cpu_idle(int cpu)
{
	return available_idle_cpu(cpu) && !READ_ONCE(cpu_rq(cpu)->pfs.nr_queued);
}

/* An idle CPU in @p's affinity, close to @target first. */
static int pfs_find_idle_cpu(struct task_struct *p, int target)
{
	int cpu, pass;

	for (pass = 0; pass < 2; pass++) {
		const struct cpumask *span = pass ? cpu_active_mask :
						    pfs_llc_span(target);

		for_each_cpu_wrap(cpu, span, target) {
			if (!cpumask_test_cpu(cpu, p->cpus_ptr))
				continue;
			if (pfs_cpu_idle(cpu))
				return cpu;
		}
	}
	return -1;
}

static int select_task_rq_pfs(struct task_struct *p, int prev_cpu, int wake_flags)
{
	int this_cpu = smp_processor_id();
	int target = prev_cpu;
	int cpu;

	if (!(wake_flags & (WF_TTWU | WF_FORK | WF_EXEC)))
		return prev_cpu;

	/* it would wait for a slot, don't wake an idle CPU for it */
	if (atomic_read(&pfs_nr_limited) && pfs_path_full(task_pfs_group(p)) &&
	    cpumask_test_cpu(prev_cpu, p->cpus_ptr))
		return prev_cpu;

	if (wake_flags & (WF_FORK | WF_EXEC))
		target = this_cpu;

	if (cpumask_test_cpu(target, p->cpus_ptr) && pfs_cpu_idle(target))
		return target;

	/* a synchronous waker is about to sleep, its CPU frees up */
	if ((wake_flags & WF_SYNC) && cpu_rq(this_cpu)->nr_running == 1 &&
	    cpumask_test_cpu(this_cpu, p->cpus_ptr) &&
	    cpus_share_cache(this_cpu, prev_cpu))
		target = this_cpu;

	rcu_read_lock();
	cpu = pfs_find_idle_cpu(p, target);
	rcu_read_unlock();
	if (cpu >= 0)
		return cpu;

	return target;
}

static DEFINE_PER_CPU(struct balance_callback, pfs_push_head);

/*
 * The least loaded CPU @p may move to from @rq, an idle one if possible. Only
 * returned if moving @p makes the load of the two CPUs more even.
 */
static int pfs_find_push_target(struct rq *rq, struct task_struct *p)
{
	int this_cpu = cpu_of(rq), cpu, best = -1, pass;
	unsigned long w = p->pfs.weight.weight;
	unsigned long min_load = ULONG_MAX;

	cpu = pfs_find_idle_cpu(p, this_cpu);
	if (cpu >= 0 && cpu != this_cpu)
		return cpu;

	for (pass = 0; pass < 2 && best < 0; pass++) {
		const struct cpumask *span = pass ? cpu_active_mask :
						    pfs_llc_span(this_cpu);

		for_each_cpu_wrap(cpu, span, this_cpu) {
			unsigned long load;

			if (cpu == this_cpu || !cpumask_test_cpu(cpu, p->cpus_ptr))
				continue;
			load = READ_ONCE(cpu_rq(cpu)->pfs.load);
			if (load < min_load) {
				min_load = load;
				best = cpu;
			}
		}
	}

	/* moving p must lower the larger of the two loads */
	if (best >= 0 && min_load + w + (w >> 3) < READ_ONCE(rq->pfs.load))
		return best;
	return -1;
}

/*
 * Balance callback after a context switch: move one queued task to an idle
 * CPU, or to a CPU with less load.
 */
static void pfs_push(struct rq *rq)
{
	struct pfs_rq *prq = &rq->pfs;
	struct task_struct *p;
	struct rq *dst;
	int cpu;

	prq->next_push = jiffies + PFS_PUSH_INTERVAL;
	prq->push_preempted = 0;

	if (prq->nr_queued < 2 || !cpu_active(cpu_of(rq)))
		return;

	p = pfs_pick_migratable(rq, -1, ULONG_MAX);
	if (!p)
		return;

	rcu_read_lock();
	cpu = pfs_find_push_target(rq, p);
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

	pe->rotate = 0;
	if (pe->state != PFS_QUEUED || !task_on_rq_queued(prev))
		return;

	update_curr_pfs(rq);
	pfs_unqueue_task(rq, prev, 0);
	pfs_release_slots(rq, prev, true);
	pe->state = PFS_PARKING;
	pfs_stat(PFS_STAT_ROTATE);
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

	rq->pfs.handoff = NULL;

	for_each_pfs_level(lg, leaf) {
		while (!got && READ_ONCE(lg->nr_waiting) &&
		       tries++ < PFS_HANDOFF_TRIES) {
			struct task_struct *p;
			struct rq *src;
			int ret;

			/* waiting tasks are freed after a grace period */
			p = pfs_first_waiter(lg);
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
	INIT_LIST_HEAD(&g->waiters);
	g->depth = parent ? parent->pfs.depth + 1 : 0;
	g->weight = NICE_0_LOAD;
	hrtimer_setup(&g->window_timer, pfs_window_timer_fn, CLOCK_MONOTONIC,
		      HRTIMER_MODE_ABS_PINNED_HARD);
	g->retry_work = IRQ_WORK_INIT_HARD(pfs_retry_workfn);
}

void pfs_free_group(struct task_group *tg)
{
	struct pfs_group *g = &tg->pfs;

	if (g->limit)
		atomic_dec(&pfs_nr_limited);
	hrtimer_cancel(&g->window_timer);
	/*
	 * Called from an RCU callback, irq_work_sync() may sleep. The group
	 * has no tasks left, so no retry can be queued anymore; wait for one
	 * that may still run, it is a hard irq_work.
	 */
	while (irq_work_is_busy(&g->retry_work))
		cpu_relax();
	WARN_ON_ONCE(!list_empty(&g->waiters));
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

	raw_spin_lock_irqsave(&pfs_weight_lock, flags);
	if (g->nr_active > 0)
		parent->child_weight_sum += w - g->weight;
	g->weight = w;
	raw_spin_unlock_irqrestore(&pfs_weight_lock, flags);

	atomic_long_inc(&pfs_hweight_gen);
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
	debugfs_create_file("stats", 0444, d, NULL, &pfs_stats_fops);
	debugfs_create_file("groups", 0444, d, NULL, &pfs_groups_fops);
	debugfs_create_file_unsafe("window_us", 0644, d, NULL, &pfs_window_us_fops);
}

bool pfs_can_stop_tick(struct rq *rq)
{
	return rq->pfs.nr_queued <= 1;
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

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 1,
#endif
};
