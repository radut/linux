#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Run the whole matrix for every available variant and print a CSV plus a
# summary table.
set -euo pipefail
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=docker-run.sh
. "$KIT/docker-run.sh"
# shellcheck source=systemd-run.sh
. "$KIT/systemd-run.sh"

usage() {
	cat <<EOF
usage: $0 [options]

  -p PATH       cgroup (default), docker (also Podman via DOCKER=podman) or systemd (systemd-run)
  -v VARIANTS   comma list of cfs,scx,kwidth (default: what this machine has: $(available_variants 2>/dev/null || echo cfs))
  -l LIMITS     CPU limits (default 0.5,1,2,4)
  -t THREADS    busy thread counts (default 1,8,64)
  -C CELLS      explicit LIMIT:THREADS list instead of -l x -t, e.g. 1:8,1.5:8,2:64
  -n NEIGH      neighbour settings (default none,noisy)
  -L LIMITS     latency test limits, reqlat with REQ_BURN (2) burners (default 0.5,1; "none" skips)
  -r REPS       repeat every case REPS times (default 1); the summary shows median (min-max)
  -s SECS       seconds per case (default 5)
  -m MODE       systemd path: scope (default) or service
  -o DIR        output directory (default ./results/<timestamp>-<path>)
  -h            this text

Writes DIR/results.csv and DIR/summary.txt; scx_width's log goes to DIR/scx-<variant>.log.
Limits not below the CPU count are skipped. Takes about
  (#cells * #neigh + 2 * #latency limits) * #variants * REPS * (SECS + 2) seconds.
EOF
}

# run_case PATH VARIANT LIMIT THREADS SECS WORKLOAD NEIGHBOUR
run_case() {
	case $1 in
	cgroup) run_cg_case "$2" "$3" "$4" "$5" "$6" "$7" 100 ;;
	docker) run_docker_case "$2" "$3" "$4" "$5" "$6" "$7" "" "" ;;
	systemd) run_systemd_case "$2" "$3" "$4" "$5" "$6" "$7" "$SYSTEMD_MODE" "" 100 ;;
	esac
}

compare_main() {
	local path=cgroup variants="" limits=0.5,1,2,4 threads=1,8,64 cells="" neighs=none,noisy
	local lat=0.5,1 reps=1 secs=5 out="" opt v c l t n r csv ncpu
	SYSTEMD_MODE=scope
	while getopts "p:v:l:t:C:n:L:r:s:m:o:h" opt; do
		case $opt in
		p) path=$OPTARG ;;
		v) variants=$OPTARG ;;
		l) limits=$OPTARG ;;
		t) threads=$OPTARG ;;
		C) cells=$OPTARG ;;
		n) neighs=$OPTARG ;;
		L) lat=$OPTARG ;;
		r) reps=$OPTARG ;;
		s) secs=$OPTARG ;;
		m) SYSTEMD_MODE=$OPTARG ;;
		o) out=$OPTARG ;;
		h) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
		esac
	done
	need_root
	check_cgroup2
	[ -x "$KIT/bin/spin" ] && [ -x "$KIT/bin/reqlat" ] || die "build first: make -C $KIT"
	case $path in
	cgroup) ;;
	docker) docker_check ;;
	systemd) [ -d /run/systemd/system ] || die "systemd is not the init system" ;;
	*) die "path must be cgroup, docker or systemd" ;;
	esac
	if [ -z "$cells" ]; then
		for l in ${limits//,/ }; do
			for t in ${threads//,/ }; do
				cells="$cells${cells:+,}$l:$t"
			done
		done
	fi
	[ -n "$variants" ] || variants=$(available_variants)
	[ -n "$out" ] || out=$KIT/results/$(date +%Y%m%d-%H%M%S)-$path
	mkdir -p "$out"
	csv=$out/results.csv
	ncpu=$(nproc)
	echo "$CSV_HEADER" > "$csv"
	{
		echo "kernel=$(uname -r) cpus=$ncpu path=$path variants=$variants cells=$cells neigh=$neighs latency=$lat reps=$reps secs=$secs"
		echo "sched_ext=$(cat $SCX_STATE 2>/dev/null || echo absent) kwidth_sysctl=$(cat $KW_SYSCTL 2>/dev/null || echo absent) scx_mode=${SCX_MODE:-host}"
		[ "$path" != docker ] || echo "runtime=$(runtime_label) docker='$DOCKER'"
	} | tee "$out/env.txt" >&2
	trap cleanup EXIT INT TERM
	for v in ${variants//,/ }; do
		# shellcheck disable=SC2034 # read by scx_start
		SCX_LOG=$out/scx-$v.log
		variant_start "$v"
		for r in $(seq "$reps"); do
			for c in ${cells//,/ }; do
				l=${c%%:*} t=${c##*:}
				if awk -v l="$l" -v c="$ncpu" 'BEGIN { exit !(l >= c) }'; then
					[ "$r" != 1 ] || log "skip limit $l: not below the $ncpu CPUs"
					continue
				fi
				for n in ${neighs//,/ }; do
					run_case "$path" "$v" "$l" "$t" "$secs" spin "$n" >> "$csv"
				done
			done
			[ "$lat" != none ] || continue
			for l in ${lat//,/ }; do
				for n in ${neighs//,/ }; do
					run_case "$path" "$v" "$l" "${REQ_BURN:-2}" "$secs" reqlat "$n" >> "$csv"
				done
			done
		done
		variant_stop
	done
	python3 "$KIT/summary.py" "$csv" | tee "$out/summary.txt"
	echo "CSV: $csv"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	compare_main "$@"
fi
