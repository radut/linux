// SPDX-License-Identifier: GPL-2.0
// reqlat: open-loop request latency under a CPU limit.
//
//   reqlat THREADS WORK_US PERIOD_US SECS [BURN_THREADS]
//
// Each of THREADS request threads wakes every PERIOD_US (absolute timer) and
// burns WORK_US of its own CPU time (a "request"). Latency is measured from
// the scheduled start to completion, so time spent runnable but not running
// (throttled, waiting for a slot, preempted) counts. The timer keeps its
// schedule when a request is late: a backlog shows up as latency, it is not
// hidden (no coordinated omission).
//
// BURN_THREADS busy threads run in the same process (a garbage collector,
// background writer or batch job sharing the container's CPU limit).
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_THREADS	256

static uint64_t work_ns, period_ns, end_ns;
static volatile int stop_burn;

struct req {
	uint64_t *lat;
	size_t n, cap;
};

static uint64_t ts_ns(clockid_t clk)
{
	struct timespec ts;

	clock_gettime(clk, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void burn_cpu(uint64_t ns)
{
	uint64_t until = ts_ns(CLOCK_THREAD_CPUTIME_ID) + ns;
	volatile uint64_t x = 0;

	while (ts_ns(CLOCK_THREAD_CPUTIME_ID) < until)
		for (int i = 0; i < 200; i++)
			x += i;
}

static void *req_thread(void *arg)
{
	struct req *r = arg;
	uint64_t next = ts_ns(CLOCK_MONOTONIC);

	for (;;) {
		struct timespec ts;

		next += period_ns;
		if (next >= end_ns)
			break;
		ts.tv_sec = next / 1000000000ull;
		ts.tv_nsec = next % 1000000000ull;
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
			;
		burn_cpu(work_ns);
		if (r->n < r->cap)
			r->lat[r->n++] = ts_ns(CLOCK_MONOTONIC) - next;
	}
	return NULL;
}

static void *burn_thread(void *arg)
{
	uint64_t *iters = arg;
	volatile uint64_t x = 0;

	while (!stop_burn) {
		for (int i = 0; i < 2000; i++)
			x += i;
		(*iters)++;
	}
	return NULL;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static double pct(const uint64_t *v, size_t n, double p)
{
	size_t i;

	if (!n)
		return 0;
	i = (size_t)(p * (n - 1) + 0.5);
	return v[i] / 1e6;
}

int main(int argc, char **argv)
{
	pthread_t rt[MAX_THREADS], bt[MAX_THREADS];
	struct req req[MAX_THREADS] = {};
	uint64_t burn_iters[MAX_THREADS] = {}, burn_total = 0;
	int nreq, nburn;
	double secs;
	uint64_t *all;
	size_t total = 0;

	if (argc < 5) {
		fprintf(stderr,
			"usage: %s THREADS WORK_US PERIOD_US SECS [BURN_THREADS]\n",
			argv[0]);
		return 2;
	}
	nreq = atoi(argv[1]);
	work_ns = strtoull(argv[2], NULL, 0) * 1000;
	period_ns = strtoull(argv[3], NULL, 0) * 1000;
	secs = atof(argv[4]);
	nburn = argc > 5 ? atoi(argv[5]) : 0;
	if (nreq < 1 || nreq > MAX_THREADS || nburn < 0 || nburn > MAX_THREADS ||
	    !work_ns || period_ns < work_ns || secs <= 0) {
		fprintf(stderr, "invalid arguments\n");
		return 2;
	}
	end_ns = ts_ns(CLOCK_MONOTONIC) + (uint64_t)(secs * 1e9);

	for (int i = 0; i < nreq; i++) {
		req[i].cap = (size_t)(secs * 1e9 / period_ns) + 2;
		req[i].lat = calloc(req[i].cap, sizeof(uint64_t));
		if (!req[i].lat) {
			perror("calloc");
			return 1;
		}
		pthread_create(&rt[i], NULL, req_thread, &req[i]);
	}
	for (int i = 0; i < nburn; i++)
		pthread_create(&bt[i], NULL, burn_thread, &burn_iters[i]);

	for (int i = 0; i < nreq; i++) {
		pthread_join(rt[i], NULL);
		total += req[i].n;
	}
	stop_burn = 1;
	for (int i = 0; i < nburn; i++) {
		pthread_join(bt[i], NULL);
		burn_total += burn_iters[i];
	}

	all = malloc((total ? total : 1) * sizeof(uint64_t));
	if (!all) {
		perror("malloc");
		return 1;
	}
	total = 0;
	for (int i = 0; i < nreq; i++)
		for (size_t j = 0; j < req[i].n; j++)
			all[total++] = req[i].lat[j];
	qsort(all, total, sizeof(uint64_t), cmp_u64);

	printf("requests=%zu p50_ms=%.2f p90_ms=%.2f p99_ms=%.2f p999_ms=%.2f max_ms=%.2f burn_iters=%llu\n",
	       total, pct(all, total, 0.50), pct(all, total, 0.90), pct(all, total, 0.99),
	       pct(all, total, 0.999), total ? all[total - 1] / 1e6 : 0.0,
	       (unsigned long long)burn_total);
	return 0;
}
