// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <time.h>
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
"       [-b BURST_PCT] [-q NR] [-i INTERVAL] [-H SETTLE_MS] [-S FILE] [-v]\n"
"\n"
"  -s SLICE_US   Slice duration (default: 2500, 100 to 1000000)\n"
"  -c CUT_US     A task holding slots since a rotation gives them to a task\n"
"                which woke up after this, also the least time a yielding\n"
"                task keeps its slots (default: 500, at most 1000000)\n"
"  -r ROT_US     ... and to any waiting task after this, 0 for the end of the\n"
"                slice (default: 0, at most 1000000)\n"
"  -k CREDIT_US  A task which woke up and found no free slot runs without one\n"
"                for up to this long while its cgroup has saved up CPU time,\n"
"                0 to make it wait (default: 1000, at most 1000000)\n"
"  -w WINDOW_US  How often a cgroup out of CPU time for its fractional limit\n"
"                gets another try, also the window of the throttling counters\n"
"                (default: 10000, 1000 to 1000000)\n"
"  -b BURST_PCT  CPU time a fractional limit saves up while it runs below it,\n"
"                in percent of what its fraction allows per cpu.max period,\n"
"                at least a window's worth (default: 100, at most 1000)\n"
"  -q NR         Size of the queues of cgroups with waiting tasks; more\n"
"                such cgroups are found by a slower sweep (default: 65536,\n"
"                16 to 16777216)\n"
"  -i INTERVAL   Report interval in seconds, with a line of throttling counters\n"
"  -H SETTLE_MS  After a hotplug restart, wait for the hotplug sequence to\n"
"                stop moving before reloading, starting at this interval and\n"
"                doubling to 16x it, 0 to reload immediately as before\n"
"                (default: 20, at most 5000)\n"
"                per limited cgroup (default: 1, at most 86400)\n"
"  -S FILE       Write the throttling counters of the limited cgroups to FILE\n"
"                every second, see the README for the format\n"
"  -v            Print libbpf debug messages\n"
"  -h            Display this help and exit\n";

/* bounds the timer frequency, cgroups out of tokens are retried every window */
#define MIN_WINDOW_US	1000
#define MAX_US		(1000 * 1000)	/* bounds for -s, -c, -r, -k and -w */
#define MAX_BURST_PCT	1000
#define MIN_SLICE_US	100
#define MAX_INTERVAL	(24 * 3600)
#define MAX_SETTLE_MS	5000
#define MIN_CGRP_Q	16
#define MAX_CGRP_Q	(1 << 24)

#define CGROUP_FS	"/sys/fs/cgroup"

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

/* state of a dump_cgrp_stats() walk, nftw() callbacks take no argument */
static struct {
	FILE	*f;
	bool	kv;		/* key=value lines for the report */
	int	map_fd;
	void	*val;
	__u64	now;
} dump;

static int dump_one(const char *path, const struct stat *sb, int type,
		    struct FTW *ftw)
{
	const struct pfs_cgrp_stat *st = dump.val;
	__u64 throttled;
	int fd, ret;

	if (type != FTW_D)
		return 0;
	fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	ret = bpf_map_lookup_elem(dump.map_fd, &fd, dump.val);
	close(fd);
	if (ret || (!st->nr_periods && !st->nr_throttled))
		return 0;

	throttled = st->throttled_ns;
	if (st->nr_waiting > 0 && st->wait_since && dump.now > st->wait_since)
		throttled += dump.now - st->wait_since;
	path += strlen(CGROUP_FS);
	fprintf(dump.f, dump.kv ?
		"cgroup path=%s nr_periods=%llu nr_throttled=%llu throttled_usec=%llu\n" :
		"%s nr_periods %llu nr_throttled %llu throttled_usec %llu\n",
		*path ? path : "/", (unsigned long long)st->nr_periods,
		(unsigned long long)st->nr_throttled,
		(unsigned long long)throttled / 1000);
	return 0;
}

/*
 * Print the throttling counters of every cgroup that was limited and active
 * to @f, as key=value lines if @kv.
 */
static void print_cgrp_stats(struct scx_pfs *skel, FILE *f, bool kv)
{
	struct timespec ts;

	if (!dump.val) {
		dump.val = calloc(1, bpf_map__value_size(skel->maps.cgrp_ctx));
		assert(dump.val);
	}
	dump.f = f;
	dump.kv = kv;
	dump.map_fd = bpf_map__fd(skel->maps.cgrp_ctx);
	/* the BPF side's clock, bpf_ktime_get_ns() */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	dump.now = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	nftw(CGROUP_FS, dump_one, 32, FTW_PHYS | FTW_MOUNT);
}

/*
 * Write the counters to @file, through a temporary file renamed over it so
 * that readers never see a partial dump.
 */
static void dump_cgrp_stats(struct scx_pfs *skel, const char *file)
{
	static char tmp[4096];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", file);
	f = fopen(tmp, "we");
	if (!f) {
		fprintf(stderr, "-S: can't write %s: %s\n", tmp, strerror(errno));
		return;
	}
	print_cgrp_stats(skel, f, false);
	if (fclose(f) || rename(tmp, file))
		fprintf(stderr, "-S: can't write %s: %s\n", file, strerror(errno));
}

/*
 * A CPU going offline or online exits the scheduler with SCX_ECODE_ACT_RESTART,
 * and main()'s restart below reopens, reloads and reattaches. The sequence
 * number of the last hotplug event is latched when the skeleton is opened and
 * the kernel refuses the enable if it has moved by the time the attach lands,
 * so a reattach that races the next event is rejected and the scheduler stays
 * off for the length of another open, load and attach. Under churn that keeps
 * repeating: measured over two runs of 20 offline/online cycles, 14 gaps of
 * median 1.8 s and worst 4.0 s in one, and 4 gaps of median 10.9 s and worst
 * 27.3 s in the other, with 19 and 26 rejected enables respectively.
 *
 * Reloading into the next event is what costs the time, so wait for the
 * sequence to stop moving before reopening. The wait doubles its interval up
 * to a cap and gives up after a bounded number of tries, after which the
 * attach is made anyway: a scheduler that keeps retrying is better than one
 * that stays off because the churn outlasted the loop.
 */
#define HOTPLUG_SETTLE_MS	20	/* -H: first interval, 0 disables the wait */
#define HOTPLUG_SETTLE_TRIES	10

static void wait_hotplug_settled(__u64 first_ms)
{
	__u64 ms = first_ms, cap = first_ms * 16;
	long seq, prev;
	int try;

	if (!first_ms)
		return;

	prev = scx_hotplug_seq();
	if (prev < 0)
		return;			/* no sysfs file: nothing to wait for */

	for (try = 1; try <= HOTPLUG_SETTLE_TRIES; try++) {
		usleep(ms * 1000);
		seq = scx_hotplug_seq();
		if (seq == prev) {
			if (try > 1 || verbose)
				fprintf(stderr, "hotplug_seq %ld settled after %d tr%s\n",
					seq, try, try == 1 ? "y" : "ies");
			return;
		}
		prev = seq;
		if (ms < cap)
			ms *= 2;
	}
	fprintf(stderr, "hotplug_seq still moving (%ld) after %d tries, attaching anyway\n",
		prev, HOTPLUG_SETTLE_TRIES);
}

int main(int argc, char **argv)
{
	struct scx_pfs *skel;
	struct bpf_link *link;
	__u64 intv = 1, nr_cgrp_q = NR_CGRP_Q, secs;
	__u64 settle_ms = HOTPLUG_SETTLE_MS;
	bool restarted = false;
	const char *stats_file = NULL;
	long nr_cpus;
	__s32 opt;
	__u64 ecode;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	if (restarted)
		wait_hotplug_settled(settle_ms);
	optind = 1;
	skel = SCX_OPS_OPEN(pfs_ops, scx_pfs);

	/*
	 * Slices end on a tick, 2.5ms run for about 3ms with HZ=1000, close to
	 * EEVDF's slice so that waiting tasks get their turn quickly.
	 */
	skel->rodata->slice_ns = 2500 * 1000;
	skel->rodata->window_ns = 10 * 1000 * 1000;

	while ((opt = getopt(argc, argv, "s:c:r:k:w:b:q:i:H:S:vh")) != -1) {
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
		case 'b':
			skel->rodata->burst_pct = parse_opt(opt, optarg, 0,
							    MAX_BURST_PCT);
			break;
		case 'q':
			nr_cgrp_q = parse_opt(opt, optarg, MIN_CGRP_Q,
					      MAX_CGRP_Q);
			break;
		case 'i':
			intv = parse_opt(opt, optarg, 1, MAX_INTERVAL);
			break;
		case 'H':
			settle_ms = parse_opt(opt, optarg, 0, MAX_SETTLE_MS);
			break;
		case 'S':
			stats_file = optarg;
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

	printf("slice=%lluus cut=%lluus rot=%lluus credit=%lluus ",
	       (unsigned long long)skel->rodata->slice_ns / 1000,
	       (unsigned long long)skel->rodata->cut_ns / 1000,
	       (unsigned long long)skel->rodata->rot_ns / 1000,
	       (unsigned long long)skel->rodata->credit_ns / 1000);
	printf("window=%lluus burst=%llu%% queues=%llu\n",
	       (unsigned long long)skel->rodata->window_ns / 1000,
	       (unsigned long long)skel->rodata->burst_pct,
	       (unsigned long long)nr_cgrp_q);

	bpf_map__set_max_entries(skel->maps.retry_q, nr_cgrp_q);
	bpf_map__set_max_entries(skel->maps.budget_q, nr_cgrp_q);
	bpf_map__set_max_entries(skel->maps.waitset, nr_cgrp_q);

	SCX_OPS_LOAD(skel, pfs_ops, scx_pfs, uei);
	link = SCX_OPS_ATTACH(skel, pfs_ops, scx_pfs);

	for (secs = 0; !exit_req && !UEI_EXITED(skel, uei); secs++) {
		if (!(secs % intv)) {
			print_stats(skel);
			print_cgrp_stats(skel, stdout, true);
			fflush(stdout);
		}
		if (stats_file)
			dump_cgrp_stats(skel, stats_file);
		sleep(1);
	}
	print_stats(skel);
	print_cgrp_stats(skel, stdout, true);
	fflush(stdout);
	if (stats_file)
		dump_cgrp_stats(skel, stats_file);

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_pfs__destroy(skel);

	if (!exit_req && UEI_ECODE_RESTART(ecode)) {
		restarted = true;
		goto restart;
	}
	return 0;
}
