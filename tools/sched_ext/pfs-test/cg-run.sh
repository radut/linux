#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# One test case on plain cgroup v2 (no container runtime).
set -euo pipefail
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$KIT/lib.sh"

usage() {
	cat <<EOF
usage: $0 [options] VARIANT LIMIT THREADS

  VARIANT   cfs | scx | kwidth
  LIMIT     CPU limit written to cpu.max as LIMIT*${PERIOD_US} ${PERIOD_US} (e.g. 0.5, 1, 2), or max
  THREADS   busy threads in the limited cgroup (with -w reqlat: in-process
            background burners next to the request threads)

options:
  -s SECS       run time (default 5)
  -w WORKLOAD   spin (throughput + stalls, default) or reqlat (request latency)
  -n NEIGHBOUR  none (default) or noisy: an unlimited cgroup with one busy
                thread per CPU competes for the machine
  -W WEIGHT     cpu.weight of the limited cgroup (default 100)
  -H            print the CSV header first
  -h            this text

reqlat runs REQ_THREADS (4) request threads, each doing REQ_WORK_US (1000) of
CPU work every REQ_PERIOD_US (20000), and reports latency percentiles.
Prints one CSV line ($CSV_HEADER).
Cleans up its cgroups, the scheduler and the sysctl on exit or Ctrl-C.
EOF
}

# run_cg_case VARIANT LIMIT THREADS SECS WORKLOAD NEIGHBOUR WEIGHT
# The variant must already be active (variant_start).
run_cg_case() {
	local variant=$1 limit=$2 threads=$3 secs=$4 wl=$5 neigh=$6 weight=$7
	local lim noisy out st conc wpid tmp
	tmp=$(mktemp -d)
	cg_setup
	lim=$(cg_new lim "$limit" "$weight")
	if [ "$neigh" = noisy ]; then
		noisy=$(cg_new noisy max)
		cg_exec "$noisy" "$KIT/bin/noisy" "$(nproc)" "$(awk -v s="$secs" 'BEGIN { print s + 1 }')" > /dev/null
		sleep 0.3
	fi
	trace_start
	if [ "$wl" = reqlat ]; then
		cg_exec "$lim" "$KIT/bin/reqlat" "${REQ_THREADS:-4}" "${REQ_WORK_US:-1000}" \
			"${REQ_PERIOD_US:-20000}" "$secs" "$threads" > "$tmp/out"
	else
		cg_exec "$lim" "$KIT/bin/spin" "$threads" "$secs" > "$tmp/out"
	fi
	wpid=$!
	wait "$wpid" || die "workload failed"
	trace_stop "$tmp/trace"
	kill_pids
	out=$(cat "$tmp/out")
	st=$(cg_stat "$lim")
	conc=$(conc_of "$wl" "$tmp/trace" "$limit")
	log "$variant limit=$limit threads=$threads neigh=$neigh: $out | cpu.stat nr_throttled/throttled_ms/usage_ms: $st | conc avg/max/%over: $conc"
	[ -z "${KEEP_TRACE:-}" ] || cp "$tmp/trace" "$KEEP_TRACE" 2>/dev/null || true
	rm -rf "$tmp"
	cg_rm "$lim"
	[ "$neigh" != noisy ] || cg_rm "$noisy"
	emit_row cgroup "$variant" "$limit" "$threads" "$neigh" "$secs" "$wl" "$out" "$st" "$conc"
}

# emit_row PATH VARIANT LIMIT THREADS NEIGH SECS WORKLOAD OUT "NR THR_MS USE_MS" "AVG MAX PCT"
emit_row() {
	local out=$8 iters miter stall stalls p50 p99 mx
	read -r nr thr _ <<< "$9"
	read -r cavg cmax cpct <<< "${10}"
	if [ "$7" = reqlat ]; then
		iters=$(kv burn_iters "$out"); stall=-; stalls=-
		p50=$(kv p50_ms "$out"); p99=$(kv p99_ms "$out"); mx=$(kv max_ms "$out")
	else
		iters=$(kv iters "$out"); stall=$(kv max_stall_ms "$out"); stalls=$(kv stalls_over_5ms "$out")
		p50=-; p99=-; mx=-
	fi
	miter=$(awk -v i="$iters" -v s="$6" 'BEGIN { printf "%.2f", i / s / 1e6 }')
	echo "$1,$2,$3,$4,$5,$6,$miter,$nr,$thr,$stall,$stalls,$cavg,$cmax,$cpct,$p50,$p99,$mx"
}

main() {
	local secs=5 wl=spin neigh=none weight=100 header=0 opt
	while getopts "s:w:n:W:Hh" opt; do
		case $opt in
		s) secs=$OPTARG ;;
		w) wl=$OPTARG ;;
		n) neigh=$OPTARG ;;
		W) weight=$OPTARG ;;
		H) header=1 ;;
		h) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
		esac
	done
	shift $((OPTIND - 1))
	[ $# -eq 3 ] || { usage >&2; exit 2; }
	check_case "$2" "$3" "$secs"
	check_int WEIGHT "$weight"
	case $wl in spin|reqlat) ;; *) die "workload must be spin or reqlat" ;; esac
	case $neigh in none|noisy) ;; *) die "neighbour must be none or noisy" ;; esac
	if [ ! -x "$KIT/bin/spin" ] || [ ! -x "$KIT/bin/reqlat" ]; then
		die "build first: make -C $KIT"
	fi
	need_root
	check_cgroup2
	trap cleanup EXIT INT TERM
	variant_start "$1"
	[ "$header" = 0 ] || echo "$CSV_HEADER"
	run_cg_case "$1" "$2" "$3" "$secs" "$wl" "$neigh" "$weight"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	main "$@"
fi
