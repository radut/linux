/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SCX_EXAMPLE_PFS_H
#define __SCX_EXAMPLE_PFS_H

#define HWEIGHT_ONE		(1LLU << 28)	/* the whole machine */
/* bound on how long a task of a small cgroup waits for its next slice */
#define WAIT_MAX_NS		(1000 * 1000 * 1000LLU)
/* default size of the per-cgroup queues, see -q */
#define NR_CGRP_Q		65536

/*
 * Throttling counters of a limited cgroup, the first member of its BPF
 * context. User space reads them with -S, see scx_pfs.c. A cgroup is
 * throttled while at least one of its runnable tasks waits in its DSQs, for
 * a slot or for budget. Windows are -w long.
 */
struct pfs_cgrp_stat {
	__u64 nr_periods;	/* windows in which the cgroup ran or waited */
	__u64 nr_throttled;	/* windows in which a task of it waited */
	__u64 throttled_ns;	/* time with a waiting task, ended waits */
	__u64 wait_since;	/* start of the current wait, if nr_waiting */
	__u64 period_id;	/* last window counted in nr_periods */
	__u64 throttled_id;	/* last window counted in nr_throttled */
	__s32 nr_waiting;	/* tasks waiting in the DSQs of the cgroup */
	__u32 pad;
	__s64 tokens_ns;	/* the token bucket at its last refill, a debt if < 0 */
};

enum pfs_stat_idx {
	PFS_STAT_DIRECT,		/* dispatched straight to an idle CPU */
	PFS_STAT_SHARED,		/* admitted to the shared DSQ on enqueue */
	PFS_STAT_PARK_FULL,		/* parked, all slots of a cgroup taken */
	PFS_STAT_PARK_BUDGET,		/* parked, fractional budget exhausted */
	PFS_STAT_ADMIT,		/* parked task admitted by a retry */
	PFS_STAT_REPARK,		/* parked task moved to another level */
	PFS_STAT_RETRY,		/* cgroup retries processed */
	PFS_STAT_RETRY_OVERFLOW,	/* cgroup queue full, rescued by the timer */
	PFS_STAT_MOVE_RACE,		/* admission raced, slots given back */
	PFS_STAT_TOO_DEEP,		/* task of a too deep cgroup not limited */
	PFS_STAT_ACTIVATE,		/* cgroup (de)activations changing weights */
	PFS_STAT_HWEIGHT,		/* hweight recomputations */
	PFS_STAT_KEEP,		/* expired task kept running by dispatch */
	PFS_STAT_PARK_WOKEN,		/* parks of tasks that woke up (of the above) */
	PFS_STAT_CUT,		/* rotated holders cut short for a woken waiter */
	PFS_STAT_YIELD_KEEP,		/* yields that kept the slots */
	PFS_STAT_YIELD_ROTATE,	/* yields that gave the slots to a waiter */
	PFS_STAT_RESCUE,		/* waiting cgroups retried by the timer sweep */
	PFS_STAT_LOST_PATH,		/* slots of a removed cgroup not given back */
	PFS_STAT_CREDIT,		/* ran without slots instead of waiting */
	PFS_STAT_DRAIN,		/* waiting tasks let run as their cgroup went */
	PFS_STAT_DRAIN_FAIL,		/* wait DSQ not empty after the drain, kept */
	PFS_STAT_NO_DSQ,		/* cgroups not limited, DSQ creation failed */
	PFS_STAT_PARK_RACE,		/* retries queued again for a landing park */
	PFS_STAT_PARK_DEBT,		/* slot refused: the cgroup ran beyond its limit */
	PFS_STAT_HOTPLUG,		/* CPUs going on or offline */
	PFS_STAT_YIELD_CREDIT,	/* yielded in the kernel, ran on credit instead of parking */

	PFS_NR_STATS,
};

/*
 * Slot index at which a refusal of take_slot() is counted; indices at or
 * above it share the last bucket. A limited cgroup hands out ceil(limit)
 * slots, so this covers limits below nine CPUs exactly.
 */
#define PFS_SLOT_IDX_MAX	8

/*
 * Why take_slot() refused a slot, and whether the refused slot was one of
 * the floor(limit) guaranteed slots or the fractional one above them.
 * park_debt alone cannot tell those apart, and the debt gate does refuse
 * guaranteed slots: it runs for taken + 1 >= full, i.e. also for the last
 * full slot, and for every index once the debt reached its cap. The budget
 * gate runs only for taken >= full, so a budget refusal is never of a
 * guaranteed slot and has no second class here.
 *
 * Kept apart from the kernel scheduler's enum pfs_k_refuse in
 * kernel/sched/pfs.c on purpose: both end up in vmlinux BTF on a kernel
 * built with CONFIG_DEBUG_INFO_BTF=y, and a shared enumerator name there is
 * a redefinition clang refuses.
 */
enum pfs_refuse_idx {
	PFS_REFUSE_BUDGET,	/* no tokens for the fractional slot */
	PFS_REFUSE_DEBT_GUAR,	/* over the limit, taken < full */
	PFS_REFUSE_DEBT_FRAC,	/* over the limit, taken >= full */

	PFS_NR_REFUSE,
};

/* buckets per reason: the indices 0..PFS_SLOT_IDX_MAX-1 and one for the rest */
#define PFS_REFUSE_BUCKETS	(PFS_SLOT_IDX_MAX + 1)
/* entries of the flat slot_refuse array: [reason * BUCKETS + slot index] */
#define PFS_NR_SLOT_REFUSE	(PFS_NR_REFUSE * PFS_REFUSE_BUCKETS)

#endif /* __SCX_EXAMPLE_PFS_H */
