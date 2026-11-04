/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_tickbug: keep a task runnable on a CPU that is in the idle loop.
 *
 * This scheduler exists to hold one state for a controllable length of time:
 * the idle task running on a CPU whose run queue is not empty.
 *
 * enqueue_task_scx() raises rq->nr_running before it calls ops.enqueue(), so a
 * BPF scheduler that stashes the task somewhere of its own - here a BPF queue
 * map - leaves it runnable and accounted on that CPU with nothing for the CPU
 * to pick. The CPU enters the idle loop and stays there. In that state
 * idle_cpu() is false, because idle_rq() tests rq->nr_running, while
 * is_idle_task(current) is true.
 *
 * tick_irq_exit() used to ask idle_cpu(), so every interrupt taken in that
 * idle loop skipped tick_nohz_irq_exit(). Each skip leaves TS_FLAG_IDLE_ACTIVE
 * clear, and the next exit from the idle loop then also skips
 * sched_clock_idle_wakeup_event(), so with an unstable sched_clock the CPU's
 * clock is never resynchronized: the task picked next starts with a stale
 * exec_start and the next tick charges it the whole idle stretch.
 *
 * The in-tree example schedulers can reach this state but not hold it.
 * scx_central stashes a task in a BPF queue exactly like this, then
 * immediately calls scx_bpf_kick_cpu(), so the window is microseconds wide and
 * nothing measurable accumulates. Here the only thing that releases a held
 * task is a periodic timer, so the window is hold_ns wide and can be swept.
 *
 * Only tasks named by @tag_comm are held; everything else runs through a
 * shared DSQ so the machine stays usable while the test runs.
 *
 * Copyright (c) 2026 Radu Toader
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

enum {
	SHARED_DSQ	= 0,
	MS_TO_NS	= 1000LLU * 1000,
};

/* CPU whose run queue holds the tagged task while the CPU idles. */
const volatile s32 target_cpu;
/* CPU the release timer is pinned to. Must not be @target_cpu. */
const volatile s32 timer_cpu = 1;
/* How long a tagged task is held before it is allowed to run. */
const volatile u64 hold_ns = 10 * MS_TO_NS;
const volatile u64 slice_ns;
const volatile u32 nr_cpu_ids = 1;	/* !0 for veristat, set during init */

bool timer_pinned = true;
bool timer_started;

/*
 * Set by the release timer, consumed by ops.dispatch() on @target_cpu. Without
 * it ops.dispatch() would hand the held task straight back out, because the
 * core calls ops.dispatch() whenever the CPU is about to go idle.
 */
bool release_now;

u64 nr_enq, nr_held, nr_released, nr_timers;
u64 nr_passthru, nr_locals, nr_lost, nr_other_cpu, nr_overflow;
/* Proof that the hold actually happened, and how wide it was. */
u64 held_at, total_held_ns, max_held_ns;

UEI_DEFINE(uei);

struct {
	__uint(type, BPF_MAP_TYPE_QUEUE);
	__uint(max_entries, 1024);
	__type(value, s32);
} hold_q SEC(".maps");

struct tickbug_timer {
	struct bpf_timer timer;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct tickbug_timer);
} release_timer SEC(".maps");

/*
 * The load generator is called "spin". Compare 5 bytes so the terminating NUL
 * has to match too and a longer name starting with "spin" is not held.
 */
static bool is_tagged(struct task_struct *p)
{
	return !bpf_strncmp(p->comm, 5, "spin");
}

s32 BPF_STRUCT_OPS(tickbug_select_cpu, struct task_struct *p,
		   s32 prev_cpu, u64 wake_flags)
{
	if (is_tagged(p))
		return target_cpu;
	return prev_cpu;
}

void BPF_STRUCT_OPS(tickbug_enqueue, struct task_struct *p, u64 enq_flags)
{
	s32 pid = p->pid;

	__sync_fetch_and_add(&nr_enq, 1);

	/*
	 * Per-CPU kthreads go to the head of their own CPU's local DSQ with
	 * preemption. The release timer runs from a softirq, so if a kthread
	 * such as ksoftirqd were starved behind a user task the held task
	 * would never be released.
	 */
	if ((p->flags & PF_KTHREAD) && p->nr_cpus_allowed == 1) {
		__sync_fetch_and_add(&nr_locals, 1);
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice_ns,
				   enq_flags | SCX_ENQ_PREEMPT);
		return;
	}

	if (is_tagged(p)) {
		if (scx_bpf_task_cpu(p) != target_cpu) {
			/*
			 * ops.select_cpu() is only a hint and is not called on
			 * every path. Counted rather than forced: a run with a
			 * non-zero count here did not test what it meant to.
			 */
			__sync_fetch_and_add(&nr_other_cpu, 1);
		} else if (bpf_map_push_elem(&hold_q, &pid, 0)) {
			__sync_fetch_and_add(&nr_overflow, 1);
		} else {
			/*
			 * This is the whole scheduler. The task is already in
			 * target_cpu's rq->nr_running; parking its pid here
			 * and deliberately not kicking the CPU leaves it in
			 * the idle loop with a runnable task queued.
			 */
			held_at = scx_bpf_now() ?: 1;
			__sync_fetch_and_add(&nr_held, 1);
			return;
		}
	}

	__sync_fetch_and_add(&nr_passthru, 1);
	scx_bpf_dsq_insert(p, SHARED_DSQ, slice_ns, enq_flags);
}

/* Let one held task run on the CPU we are dispatching for. */
static bool release_one(void)
{
	struct task_struct *p;
	s32 pid;

	bpf_repeat(BPF_MAX_LOOPS) {
		u64 now, at;

		if (bpf_map_pop_elem(&hold_q, &pid))
			return false;

		p = bpf_task_from_pid(pid);
		if (!p) {
			__sync_fetch_and_add(&nr_lost, 1);
			continue;
		}

		/*
		 * Affinity may have changed while the task sat in the queue.
		 * Such a task goes to the shared DSQ instead of being pinned
		 * to a CPU it is no longer allowed on.
		 */
		if (!bpf_cpumask_test_cpu(target_cpu, p->cpus_ptr) ||
		    is_migration_disabled(p)) {
			scx_bpf_dsq_insert(p, SHARED_DSQ, slice_ns, 0);
			bpf_task_release(p);
			if (!scx_bpf_dispatch_nr_slots())
				return false;
			continue;
		}

		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice_ns, 0);
		bpf_task_release(p);

		at = held_at;
		now = scx_bpf_now();
		if (at && now > at) {
			u64 d = now - at;

			__sync_fetch_and_add(&total_held_ns, d);
			if (d > max_held_ns)
				max_held_ns = d;
		}
		held_at = 0;

		__sync_fetch_and_add(&nr_released, 1);
		return true;
	}

	return false;
}

static void start_release_timer(void)
{
	struct bpf_timer *timer;
	u32 key = 0;
	int ret;

	if (likely(timer_started))
		return;

	timer = bpf_map_lookup_elem(&release_timer, &key);
	if (!timer) {
		scx_bpf_error("failed to look up the release timer");
		return;
	}

	ret = bpf_timer_start(timer, hold_ns, BPF_F_TIMER_CPU_PIN);
	if (ret == -EINVAL) {
		/* BPF_F_TIMER_CPU_PIN needs >= 6.7 */
		timer_pinned = false;
		ret = bpf_timer_start(timer, hold_ns, 0);
	}
	if (ret) {
		scx_bpf_error("bpf_timer_start failed (%d)", ret);
		return;
	}

	timer_started = true;
}

void BPF_STRUCT_OPS(tickbug_dispatch, s32 cpu, struct task_struct *prev)
{
	/*
	 * Arm the timer from the CPU it must be pinned to. ops.init() runs in
	 * whatever context loaded the scheduler, which is the wrong CPU.
	 */
	if (cpu == timer_cpu)
		start_release_timer();

	if (cpu == target_cpu && release_now) {
		release_now = false;
		if (release_one())
			return;
	}

	scx_bpf_dsq_move_to_local(SHARED_DSQ, 0);
}

static int release_timerfn(void *map, int *key, struct bpf_timer *timer)
{
	s32 curr_cpu = bpf_get_smp_processor_id();
	int ret;

	if (timer_pinned && curr_cpu == target_cpu) {
		scx_bpf_error("release timer ran on the target CPU %d", curr_cpu);
		return 0;
	}

	/*
	 * Allow one held task through and make @target_cpu notice.
	 * SCX_KICK_PREEMPT sends a reschedule IPI, which does not go through
	 * irq_enter() and so does not run tick_irq_exit() itself: it only ends
	 * the idle stretch whose bookkeeping the mis-gated interrupts dropped.
	 */
	release_now = true;
	scx_bpf_kick_cpu(target_cpu, SCX_KICK_PREEMPT);

	__sync_fetch_and_add(&nr_timers, 1);

	ret = bpf_timer_start(timer, hold_ns,
			      timer_pinned ? BPF_F_TIMER_CPU_PIN : 0);
	if (ret)
		scx_bpf_error("bpf_timer_start failed (%d)", ret);
	return 0;
}

int BPF_STRUCT_OPS_SLEEPABLE(tickbug_init)
{
	struct bpf_timer *timer;
	u32 key = 0;
	int ret;

	if (target_cpu == timer_cpu) {
		scx_bpf_error("target and timer CPU must differ (both %d)",
			      target_cpu);
		return -EINVAL;
	}

	ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (ret) {
		scx_bpf_error("scx_bpf_create_dsq failed (%d)", ret);
		return ret;
	}

	timer = bpf_map_lookup_elem(&release_timer, &key);
	if (!timer)
		return -ESRCH;

	bpf_timer_init(timer, &release_timer, CLOCK_MONOTONIC);
	bpf_timer_set_callback(timer, release_timerfn);

	/* Get ops.dispatch() to run on @timer_cpu so it arms the timer. */
	scx_bpf_kick_cpu(timer_cpu, 0);

	return 0;
}

void BPF_STRUCT_OPS(tickbug_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(tickbug_ops,
	       /*
		* The last runnable task on a CPU has to come through
		* ops.enqueue() too, otherwise the tagged task would simply
		* keep running on @target_cpu and never be held.
		*/
	       .flags			= SCX_OPS_ENQ_LAST,

	       .select_cpu		= (void *)tickbug_select_cpu,
	       .enqueue			= (void *)tickbug_enqueue,
	       .dispatch		= (void *)tickbug_dispatch,
	       .init			= (void *)tickbug_init,
	       .exit			= (void *)tickbug_exit,
	       /*
		* A held task is runnable and not running, which is what the
		* watchdog looks for. Keep the timeout well above any hold.
		*/
	       .timeout_ms		= 5000U,
	       .name			= "tickbug");
