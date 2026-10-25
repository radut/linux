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

#include <uapi/linux/sched/types.h>

#include "sched.h"
#include "stats.h"

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

/**************************************************************
 * Admission. In this version every task is admitted at once.
 */

static unsigned long pfs_effective_weight(struct task_struct *p)
{
	return p->se.load.weight;
}

/* Put an admitted task on @rq: the tree, or as the running task. */
static void pfs_queue_task(struct rq *rq, struct task_struct *p, int flags)
{
	struct pfs_rq *prq = &rq->pfs;
	struct sched_pfs_entity *pe = &p->pfs;
	unsigned long w = pfs_effective_weight(p);

	if (pe->weight.weight && pe->weight.weight != w && pe->vlag)
		pe->vlag = div64_long(pe->vlag * (long)scale_load_down(pe->weight.weight),
				      (long)max(scale_load_down(w), 1UL));
	pe->weight.weight = w;
	pe->weight.inv_weight = 0;

	pfs_place_entity(prq, pe, flags);
	pe->state = PFS_QUEUED;
	if (prq->curr != p)
		__pfs_enqueue_entity(prq, pe);

	pe->run_start = p->se.sum_exec_runtime;
	pe->run_limit = pfs_task_slice(p);
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

	if (WARN_ON_ONCE(pe->state != PFS_NONE))
		return;

	update_curr_pfs(rq);

	/* tasks migrated here aren't cache hot */
	if (flags & ENQUEUE_MIGRATED)
		p->se.exec_start = 0;

	pfs_queue_task(rq, p, flags);
}

static bool dequeue_task_pfs(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_pfs_entity *pe = &p->pfs;

	switch (pe->state) {
	case PFS_QUEUED:
		update_curr_pfs(rq);
		pfs_unqueue_task(rq, p, flags);
		break;
	default:
		WARN_ON_ONCE(1);
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

static struct task_struct *pick_task_pfs(struct rq *rq, struct rq_flags *rf)
{
	struct pfs_rq *prq = &rq->pfs;
	struct sched_pfs_entity *pe;
	int pulled;

again:
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

	if (pe->state == PFS_QUEUED) {
		update_curr_pfs(rq);
		if (!pe->on_tree)
			__pfs_enqueue_entity(prq, pe);
	}

	WARN_ON_ONCE(prq->curr != prev);
	prq->curr = NULL;
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
	update_curr_pfs(rq);

	if (curr->pfs.state != PFS_QUEUED) {
		resched_curr(rq);
		return;
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
	    pfs_can_migrate(rq, p, cpu))
		move_queued_task_locked(rq, dst, p);
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

static int balance_pfs(struct rq *rq, struct rq_flags *rf)
{
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
}

void pfs_free_group(struct task_group *tg)
{
}

void pfs_group_set_bandwidth(struct task_group *tg, u64 period_us,
			     u64 quota_us, u64 burst_us)
{
}

void pfs_group_set_weight(struct task_group *tg)
{
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

struct task_struct *pfs_get_dying_waiter(struct rq *rq)
{
	return NULL;
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
	.switched_to		= switched_to_pfs,

	.get_rr_interval	= get_rr_interval_pfs,

	.update_curr		= update_curr_pfs,

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 1,
#endif
};
