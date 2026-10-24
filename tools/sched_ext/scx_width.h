#ifndef __SCX_EXAMPLE_WIDTH_H
#define __SCX_EXAMPLE_WIDTH_H

#define HWEIGHT_ONE		(1LLU << 28)	/* the whole machine */
/* bound on how long a task of a small cgroup waits for its next slice */
#define WAIT_MAX_NS		(1000 * 1000 * 1000LLU)

enum width_stat_idx {
	WIDTH_STAT_DIRECT,		/* dispatched straight to an idle CPU */
	WIDTH_STAT_SHARED,		/* admitted to the shared DSQ on enqueue */
	WIDTH_STAT_PARK_FULL,		/* parked, all slots of a cgroup taken */
	WIDTH_STAT_PARK_BUDGET,		/* parked, fractional budget exhausted */
	WIDTH_STAT_ADMIT,		/* parked task admitted by a retry */
	WIDTH_STAT_REPARK,		/* parked task moved to another level */
	WIDTH_STAT_RETRY,		/* cgroup retries processed */
	WIDTH_STAT_RETRY_OVERFLOW,	/* retry queue full, scheduler ejected */
	WIDTH_STAT_MOVE_RACE,		/* admission raced, slots given back */
	WIDTH_STAT_TOO_DEEP,		/* task of a too deep cgroup not limited */
	WIDTH_STAT_ACTIVATE,		/* cgroup (de)activations changing weights */
	WIDTH_STAT_HWEIGHT,		/* hweight recomputations */
	WIDTH_STAT_KEEP,		/* expired task kept running by dispatch */

	WIDTH_NR_STATS,
};

#endif /* __SCX_EXAMPLE_WIDTH_H */
