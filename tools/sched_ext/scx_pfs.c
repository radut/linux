// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
"Usage: %s [-s SLICE_US] [-c CUT_US] [-r ROT_US] [-k CREDIT_US] [-w WINDOW_US]\n"
"       [-q NR] [-i INTERVAL] [-v]\n"
"\n"
"  -s SLICE_US   Slice duration (default: 2500, 100 to 1000000)\n"
"  -c CUT_US     A task holding slots since a rotation gives them to a task\n"
"                which woke up after this, also the least time a yielding\n"
"                task keeps its slots (default: 500, at most 1000000)\n"
"  -r ROT_US     ... and to any waiting task after this, 0 for the end of the\n"
"                slice (default: 0, at most 1000000)\n"
"  -k CREDIT_US  A task which woke up and found no free slot runs without one\n"
"                for up to this long while its cgroup has unused budget, 0\n"
"                to make it wait (default: 1000, at most 1000000)\n"
"  -w WINDOW_US  Accounting window for fractional limits (default: 10000,\n"
"                1000 to 1000000)\n"
"  -q NR         Size of the queues of cgroups with waiting tasks; more\n"
"                such cgroups are found by a slower sweep (default: 65536,\n"
"                16 to 16777216)\n"
"  -i INTERVAL   Report interval in seconds (default: 1, at most 86400)\n"
"  -v            Print libbpf debug messages\n"
"  -h            Display this help and exit\n";

/* bounds the timer frequency, budgets are refilled every window */
#define MIN_WINDOW_US	1000
#define MAX_US		(1000 * 1000)	/* bounds for -s, -c, -r, -k and -w */
#define MIN_SLICE_US	100
#define MAX_INTERVAL	(24 * 3600)
#define MIN_CGRP_Q	16
#define MAX_CGRP_Q	(1 << 24)

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

/*
 * Parse @arg of option @opt as a number between @min and @max, or exit. The
 * microsecond options are multiplied by 1000, the bounds keep that in range.
 */
static __u64 parse_opt(int opt, const char *arg, __u64 min, __u64 max)
{
	unsigned long long val;
	char *end;

	errno = 0;
	val = strtoull(arg, &end, 0);
	if (errno || end == arg || *end || arg[0] == '-' || val < min ||
	    val > max) {
		fprintf(stderr, "-%c: '%s' is not a number from %llu to %llu\n",
			opt, arg, (unsigned long long)min,
			(unsigned long long)max);
		exit(1);
	}
	return val;
}

static void read_stats(struct scx_pfs *skel, __u64 *stats)
{
	static __u64 *cnts;
	static int nr_cpus;
	__u32 idx;

	/* one row of per-CPU counters, on the heap: there may be many CPUs */
	if (!cnts) {
		nr_cpus = libbpf_num_possible_cpus();
		assert(nr_cpus > 0);
		cnts = calloc(nr_cpus, sizeof(*cnts));
		assert(cnts);
	}

	memset(stats, 0, sizeof(stats[0]) * PFS_NR_STATS);

	for (idx = 0; idx < PFS_NR_STATS; idx++) {
		int ret, cpu;

		ret = bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats),
					  &idx, cnts);
		if (ret < 0)
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			stats[idx] += cnts[cpu];
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
	       "rescue=%llu lost_path=%llu credit=%llu drain=%llu/%llu no_dsq=%llu\n"
	       "park_race=%llu park_debt=%llu\n",
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
	       stats[PFS_STAT_CREDIT], stats[PFS_STAT_DRAIN],
	       stats[PFS_STAT_DRAIN_FAIL], stats[PFS_STAT_NO_DSQ],
	       stats[PFS_STAT_PARK_RACE], stats[PFS_STAT_PARK_DEBT]);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	struct scx_pfs *skel;
	struct bpf_link *link;
	__u64 intv = 1, nr_cgrp_q = NR_CGRP_Q;
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

	while ((opt = getopt(argc, argv, "s:c:r:k:w:q:i:vh")) != -1) {
		switch (opt) {
		case 's':
			skel->rodata->slice_ns = 1000 *
				parse_opt(opt, optarg, MIN_SLICE_US, MAX_US);
			break;
		case 'c':
			skel->rodata->cut_ns = 1000 *
				parse_opt(opt, optarg, 0, MAX_US);
			break;
		case 'r':
			skel->rodata->rot_ns = 1000 *
				parse_opt(opt, optarg, 0, MAX_US);
			break;
		case 'k':
			skel->rodata->credit_ns = 1000 *
				parse_opt(opt, optarg, 0, MAX_US);
			break;
		case 'w':
			skel->rodata->window_ns = 1000 *
				parse_opt(opt, optarg, MIN_WINDOW_US, MAX_US);
			break;
		case 'q':
			nr_cgrp_q = parse_opt(opt, optarg, MIN_CGRP_Q,
					      MAX_CGRP_Q);
			break;
		case 'i':
			intv = parse_opt(opt, optarg, 1, MAX_INTERVAL);
			break;
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
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

	printf("slice=%lluus cut=%lluus rot=%lluus credit=%lluus window=%lluus queues=%llu\n",
	       (unsigned long long)skel->rodata->slice_ns / 1000,
	       (unsigned long long)skel->rodata->cut_ns / 1000,
	       (unsigned long long)skel->rodata->rot_ns / 1000,
	       (unsigned long long)skel->rodata->credit_ns / 1000,
	       (unsigned long long)skel->rodata->window_ns / 1000,
	       (unsigned long long)nr_cgrp_q);

	bpf_map__set_max_entries(skel->maps.retry_q, nr_cgrp_q);
	bpf_map__set_max_entries(skel->maps.budget_q, nr_cgrp_q);
	bpf_map__set_max_entries(skel->maps.waitset, nr_cgrp_q);

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
