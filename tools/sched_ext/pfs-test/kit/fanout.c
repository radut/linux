// fanout T PERIOD_MS SECS MIN_US MAX_US: every PERIOD_MS a request fans out to T threads; each burns a random
// MIN_US..MAX_US of its own CPU time; the request completes when the last thread is done. Prints completion-time
// percentiles (ms, from the release time) and how many requests were late (started after their slot).
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>

static int T, SECS, MINU, MAXU, PER;
static pthread_barrier_t start, done;
static volatile int stop;
static double *lat; static int nlat, cap;

static double now(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void burn(double us) { double e = now(CLOCK_THREAD_CPUTIME_ID) + us / 1e6; while (now(CLOCK_THREAD_CPUTIME_ID) < e) ; }

static void *worker(void *a) {
	unsigned s = (unsigned)(long)a * 2654435761u;
	for (;;) {
		pthread_barrier_wait(&start);
		if (stop) return NULL;
		burn(MINU + rand_r(&s) % (MAXU - MINU + 1));
		pthread_barrier_wait(&done);
	}
}
static int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
	if (argc < 6) { fprintf(stderr, "usage: fanout T PERIOD_MS SECS MIN_US MAX_US\n"); return 2; }
	T = atoi(argv[1]); PER = atoi(argv[2]); SECS = atoi(argv[3]); MINU = atoi(argv[4]); MAXU = atoi(argv[5]);
	pthread_barrier_init(&start, NULL, T + 1); pthread_barrier_init(&done, NULL, T + 1);
	pthread_t th[256];
	for (long i = 0; i < T; i++) pthread_create(&th[i], NULL, worker, (void *)(i + 1));
	cap = SECS * 1000 / PER + 16; lat = malloc(cap * sizeof(double));
	double t0 = now(CLOCK_MONOTONIC), next = t0; int late = 0;
	while (now(CLOCK_MONOTONIC) - t0 < SECS && nlat < cap) {
		double n = now(CLOCK_MONOTONIC);
		if (n < next) { struct timespec d = { 0, (long)((next - n) * 1e9) }; nanosleep(&d, NULL); }
		else if (n - next > PER / 1e3) late++;
		double rel = next > n ? next : n;
		pthread_barrier_wait(&start);
		pthread_barrier_wait(&done);
		lat[nlat++] = (now(CLOCK_MONOTONIC) - rel) * 1e3;
		next += PER / 1e3;
		if (now(CLOCK_MONOTONIC) > next + 1) next = now(CLOCK_MONOTONIC);
	}
	stop = 1; pthread_barrier_wait(&start);
	qsort(lat, nlat, sizeof(double), cmp);
	double sum = 0; for (int i = 0; i < nlat; i++) sum += lat[i];
	printf("fanout threads=%d period_ms=%d n=%d late=%d avg_ms=%.2f p50_ms=%.2f p90_ms=%.2f p99_ms=%.2f max_ms=%.2f\n",
	       T, PER, nlat, late, sum / nlat, lat[nlat / 2], lat[nlat * 9 / 10], lat[nlat * 99 / 100], lat[nlat - 1]);
	return 0;
}
