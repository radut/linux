// SPDX-License-Identifier: GPL-2.0
// spin: N busy threads for D seconds; reports per-thread work and the longest
// stall (gap between two consecutive loop iterations), which is where CFS
// bandwidth throttling shows up.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

static double duration;

struct res {
	uint64_t iters;
	uint64_t max_gap_ns;
	uint64_t gaps_over_5ms;
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void *worker(void *arg)
{
	struct res *r = arg;
	uint64_t start = now_ns(), end = start + (uint64_t)(duration * 1e9);
	uint64_t prev = start, t;
	volatile uint64_t x = 0;

	while ((t = now_ns()) < end) {
		uint64_t gap = t - prev;

		if (gap > r->max_gap_ns)
			r->max_gap_ns = gap;
		if (gap > 5000000)
			r->gaps_over_5ms++;
		prev = t;
		for (int i = 0; i < 2000; i++)
			x += i;
		r->iters++;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 4;
	pthread_t th[256];
	struct res res[256] = {};
	uint64_t total = 0, worst = 0, stalls = 0;

	duration = argc > 2 ? atof(argv[2]) : 5;
	if (n < 1 || n > 256)
		return 1;

	for (int i = 0; i < n; i++)
		pthread_create(&th[i], NULL, worker, &res[i]);
	for (int i = 0; i < n; i++) {
		pthread_join(th[i], NULL);
		total += res[i].iters;
		stalls += res[i].gaps_over_5ms;
		if (res[i].max_gap_ns > worst)
			worst = res[i].max_gap_ns;
	}
	printf("threads=%d iters=%llu max_stall_ms=%.2f stalls_over_5ms=%llu\n", n,
	       (unsigned long long)total, worst / 1e6, (unsigned long long)stalls);
	return 0;
}
