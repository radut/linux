#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Run the whole matrix for every available variant and print a CSV plus a
# summary table.
set -euo pipefail
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=docker-run.sh
. "$KIT/docker-run.sh"

usage() {
	cat <<EOF
usage: $0 [options]

  -p PATH       cgroup (default) or docker
  -v VARIANTS   comma list of cfs,scx,kwidth (default: what this machine has: $(available_variants 2>/dev/null || echo cfs))
  -l LIMITS     CPU limits (default 0.5,1,2,4)
  -t THREADS    busy thread counts (default 1,8,64)
  -n NEIGH      neighbour settings (default none,noisy)
  -L LIMITS     latency test limits, reqlat with REQ_BURN (2) burners (default 0.5,1; "none" skips)
  -s SECS       seconds per case (default 5)
  -o DIR        output directory (default ./results/<timestamp>)
  -h            this text

Writes DIR/results.csv and DIR/summary.txt; scx_width's log goes to DIR/scx-<variant>.log.
Limits above the CPU count are skipped. Takes about
  (#limits * #threads * #neigh + 2 * #latency limits) * #variants * (SECS + 2) seconds.
EOF
}

summary() {
	local csv=$1
	echo "Throughput (M iterations/s), worst stall (ms), throttled periods, average CPUs used:"
	awk -F, 'NR > 1 && $15 == "-" {
		k = $3 "," $4 "," $5
		if (!(k in seen)) { seen[k] = 1; keys[++n] = k }
		v[k, $2] = sprintf("%s/%s/%s/%s", $7, $10, $8, $12)
		if (!($2 in vs)) { vs[$2] = 1; var[++nv] = $2 }
	} END {
		printf "%-6s %-7s %-6s", "limit", "threads", "neigh"
		for (j = 1; j <= nv; j++) printf "  %-26s", var[j] " tput/stall/thr/cpus"
		printf "\n"
		for (i = 1; i <= n; i++) {
			split(keys[i], a, ",")
			printf "%-6s %-7s %-6s", a[1], a[2], a[3]
			for (j = 1; j <= nv; j++) printf "  %-26s", ((keys[i], var[j]) in v) ? v[keys[i], var[j]] : "-"
			printf "\n"
		}
	}' "$csv"
	echo
	echo "Request latency (reqlat, ms): p50/p99/max, background throughput (M it/s):"
	awk -F, 'NR > 1 && $15 != "-" {
		printf "%-7s limit=%-4s neigh=%-6s p50=%-7s p99=%-7s max=%-7s burn=%s thr=%s\n", $2, $3, $5, $15, $16, $17, $7, $8
	}' "$csv"
}

compare_main() {
	local path=cgroup variants="" limits=0.5,1,2,4 threads=1,8,64 neighs=none,noisy
	local lat=0.5,1 secs=5 out="" opt v l t n csv ncpu
	while getopts "p:v:l:t:n:L:s:o:h" opt; do
		case $opt in
		p) path=$OPTARG ;;
		v) variants=$OPTARG ;;
		l) limits=$OPTARG ;;
		t) threads=$OPTARG ;;
		n) neighs=$OPTARG ;;
		L) lat=$OPTARG ;;
		s) secs=$OPTARG ;;
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
	docker)
		command -v "$DOCKER" > /dev/null || die "$DOCKER not found"
		$DOCKER image inspect "$IMAGE" > /dev/null 2>&1 || die "image $IMAGE missing, run $KIT/docker-image.sh"
		;;
	*) die "path must be cgroup or docker" ;;
	esac
	[ -n "$variants" ] || variants=$(available_variants)
	[ -n "$out" ] || out=$KIT/results/$(date +%Y%m%d-%H%M%S)-$path
	mkdir -p "$out"
	csv=$out/results.csv
	ncpu=$(nproc)
	echo "$CSV_HEADER" > "$csv"
	{
		echo "kernel=$(uname -r) cpus=$ncpu path=$path variants=$variants secs=$secs"
		echo "sched_ext=$(cat $SCX_STATE 2>/dev/null || echo absent) kwidth_sysctl=$(cat $KW_SYSCTL 2>/dev/null || echo absent)"
	} | tee "$out/env.txt" >&2
	trap cleanup EXIT INT TERM
	for v in ${variants//,/ }; do
		# shellcheck disable=SC2034 # read by scx_start
		SCX_LOG=$out/scx-$v.log
		if [ "$path" = docker ]; then docker_variant_start "$v"; else variant_start "$v"; fi
		for l in ${limits//,/ }; do
			if awk -v l="$l" -v c="$ncpu" 'BEGIN { exit !(l >= c) }'; then
				log "skip limit $l: not below the $ncpu CPUs"
				continue
			fi
			for t in ${threads//,/ }; do
				for n in ${neighs//,/ }; do
					if [ "$path" = docker ]; then
						run_docker_case "$v" "$l" "$t" "$secs" spin "$n" "" "" >> "$csv"
					else
						run_cg_case "$v" "$l" "$t" "$secs" spin "$n" 100 >> "$csv"
					fi
				done
			done
		done
		if [ "$lat" != none ]; then
			for l in ${lat//,/ }; do
				for n in none noisy; do
					if [ "$path" = docker ]; then
						run_docker_case "$v" "$l" "${REQ_BURN:-2}" "$secs" reqlat "$n" "" "" >> "$csv"
					else
						run_cg_case "$v" "$l" "${REQ_BURN:-2}" "$secs" reqlat "$n" 100 >> "$csv"
					fi
				done
			done
		fi
		if [ "$path" = docker ] && [ "$v" = scx ] && [ "${SCX_MODE:-host}" = container ]; then
			scx_container_stop
		fi
		variant_stop
	done
	summary "$csv" | tee "$out/summary.txt"
	echo "CSV: $csv"
	[ "$path" = docker ] || { [ -d "$CG_ROOT" ] && rmdir "$CG_ROOT" 2>/dev/null || true; }
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	compare_main "$@"
fi
