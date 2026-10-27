// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_pfs.h"
#include "scx_pfs.bpf.skel.h"

const char help_fmt[] =
"Parallel Fair Scheduler: enforces cgroup CPU limits as a concurrency cap.\n"
"\n"
"See the top-level comment in .bpf.c for more details.\n"
"\n"
"Usage: %s [-s SLICE_US] [-c CUT_US] [-r ROT_US] [-w WINDOW_US] [-i INTERVAL]\n"
"       [-v]\n"
"\n"
"  -s SLICE_US   Slice duration (default: 2500)\n"
"  -c CUT_US     A task holding slots since a rotation gives them to a task\n"
"                which woke up after this, also the least time a yielding\n"
"                task keeps its slots (default: 500)\n"
"  -r ROT_US     ... and to any waiting task after this, 0 for the end of the\n"
"                slice (default: 0)\n"
"  -w WINDOW_US  Accounting window for fractional limits (default: 10000,\n"
"                min: 1000)\n"
"  -i INTERVAL   Report interval in seconds\n"
"  -v            Print libbpf debug messages\n"
"  -h            Display this help and exit\n";

/* bounds the timer frequency, budgets are refilled every window */
#define MIN_WINDOW_US	1000

static bool verbose;
static volatile int exit_req;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sigint_handler(int dummy)
{
	exit_req = 1;
}

static void read_stats(struct scx_pfs *skel, __u64 *stats)
{
	int nr_cpus = libbpf_num_possible_cpus();
	assert(nr_cpus > 0);
	__u64 cnts[PFS_NR_STATS][nr_cpus];
	__u32 idx;

	memset(stats, 0, sizeof(stats[0]) * PFS_NR_STATS);

	for (idx = 0; idx < PFS_NR_STATS; idx++) {
		int ret, cpu;

		ret = bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats),
					  &idx, cnts[idx]);
		if (ret < 0)
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			stats[idx] += cnts[idx][cpu];
	}
}

static void print_stats(struct scx_pfs *skel)
{
	__u64 stats[PFS_NR_STATS];

	read_stats(skel, stats);
	printf("direct=%llu shared=%llu park_full=%llu park_budget=%llu\n"
	       "admit=%llu repark=%llu retry=%llu overflow=%llu race=%llu\n"
	       "too_deep=%llu activate=%llu hweight=%llu keep=%llu\n"
	       "park_woken=%llu cut=%llu yield_keep=%llu yield_rotate=%llu "
	       "rescue=%llu lost_path=%llu credit=%llu\n",
	       stats[PFS_STAT_DIRECT], stats[PFS_STAT_SHARED],
	       stats[PFS_STAT_PARK_FULL], stats[PFS_STAT_PARK_BUDGET],
	       stats[PFS_STAT_ADMIT], stats[PFS_STAT_REPARK],
	       stats[PFS_STAT_RETRY], stats[PFS_STAT_RETRY_OVERFLOW],
	       stats[PFS_STAT_MOVE_RACE], stats[PFS_STAT_TOO_DEEP],
	       stats[PFS_STAT_ACTIVATE], stats[PFS_STAT_HWEIGHT],
	       stats[PFS_STAT_KEEP], stats[PFS_STAT_PARK_WOKEN],
	       stats[PFS_STAT_CUT], stats[PFS_STAT_YIELD_KEEP],
	       stats[PFS_STAT_YIELD_ROTATE],
	       stats[PFS_STAT_RESCUE], stats[PFS_STAT_LOST_PATH],
	       stats[PFS_STAT_CREDIT]);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	struct scx_pfs *skel;
	struct bpf_link *link;
	__u64 intv = 1;
	long nr_cpus;
	__s32 opt;
	__u64 ecode;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	optind = 1;
	skel = SCX_OPS_OPEN(pfs_ops, scx_pfs);

	/*
	 * Slices end on a tick, 2.5ms run for about 3ms with HZ=1000, close to
	 * EEVDF's slice so that waiting tasks get their turn quickly.
	 */
	skel->rodata->slice_ns = 2500 * 1000;
	skel->rodata->window_ns = 10 * 1000 * 1000;

	while ((opt = getopt(argc, argv, "s:c:r:w:i:vh")) != -1) {
		switch (opt) {
		case 's':
			skel->rodata->slice_ns = strtoull(optarg, NULL, 0) * 1000;
			break;
		case 'c':
			skel->rodata->cut_ns = strtoull(optarg, NULL, 0) * 1000;
			break;
		case 'r':
			skel->rodata->rot_ns = strtoull(optarg, NULL, 0) * 1000;
			break;
		case 'w':
			skel->rodata->window_ns = strtoull(optarg, NULL, 0) * 1000;
			break;
		case 'i':
			intv = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	if (!skel->rodata->slice_ns || !skel->rodata->window_ns || !intv) {
		fprintf(stderr, "slice, window and interval must be positive\n");
		return 1;
	}

	if (skel->rodata->window_ns < MIN_WINDOW_US * 1000) {
		fprintf(stderr, "window must be at least %dus\n", MIN_WINDOW_US);
		return 1;
	}

	if (skel->rodata->rot_ns && skel->rodata->cut_ns > skel->rodata->rot_ns)
		fprintf(stderr, "WARNING: cut is longer than rot\n");

	if (skel->rodata->slice_ns > skel->rodata->window_ns)
		fprintf(stderr, "WARNING: slice is longer than the window\n");

	/* see scale_by_cgrp_share() */
	nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (nr_cpus < 1)
		nr_cpus = 1;
	skel->rodata->min_share = HWEIGHT_ONE * skel->rodata->slice_ns /
				  (WAIT_MAX_NS * nr_cpus) ?: 1;

	printf("slice=%lluus cut=%lluus rot=%lluus window=%lluus\n",
	       (unsigned long long)skel->rodata->slice_ns / 1000,
	       (unsigned long long)skel->rodata->cut_ns / 1000,
	       (unsigned long long)skel->rodata->rot_ns / 1000,
	       (unsigned long long)skel->rodata->window_ns / 1000);

	SCX_OPS_LOAD(skel, pfs_ops, scx_pfs, uei);
	link = SCX_OPS_ATTACH(skel, pfs_ops, scx_pfs);

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		print_stats(skel);
		sleep(intv);
	}
	print_stats(skel);

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_pfs__destroy(skel);

	if (!exit_req && UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
