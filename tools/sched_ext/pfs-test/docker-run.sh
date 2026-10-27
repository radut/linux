#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# One test case with Docker (or Podman: DOCKER=podman). The CPU limit is set
# the usual way (--cpus, or --cpu-period/--cpu-quota); the runtime writes it
# to the container's cpu.max, which is all the schedulers look at.
set -euo pipefail
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$KIT/lib.sh"
# shellcheck source=cg-run.sh
. "$KIT/cg-run.sh"

usage() {
	cat <<EOF
usage: $0 [options] VARIANT LIMIT THREADS

  VARIANT   cfs | scx | kwidth
  LIMIT     CPUs, passed as --cpus=LIMIT (or as quota with -P)
  THREADS   busy threads in the container (with -w reqlat: background burners)

options:
  -s SECS       run time (default 5)
  -w WORKLOAD   spin (default) or reqlat
  -n NEIGHBOUR  none (default) or noisy: an unlimited container with one busy
                thread per CPU
  -P PERIOD_US  use --cpu-period=PERIOD_US --cpu-quota=LIMIT*PERIOD_US instead of --cpus
  -S SHARES     --cpu-shares=SHARES (cgroup v2: converted to cpu.weight)
  -H            print the CSV header first
  -h            this text

environment:
  DOCKER=podman   use podman (same flags)
  IMAGE=...       image with /spin /noisy /reqlat (default $IMAGE, see docker-image.sh)
  DOCKER="sudo -u USER XDG_RUNTIME_DIR=/run/user/UID podman"   rootless Podman
  DOCKER="podman --cgroup-manager=cgroupfs"   Podman with the cgroupfs manager
  SCX_MODE=container|service   run scx_pfs as a container or a
                  systemd unit instead of from this script (default host)

Prints one CSV line ($CSV_HEADER).
EOF
}

# run_docker_case VARIANT LIMIT THREADS SECS WORKLOAD NEIGHBOUR PERIOD SHARES
run_docker_case() {
	local variant=$1 limit=$2 threads=$3 secs=$4 wl=$5 neigh=$6 period=$7 shares=$8
	local name noisy cg out st conc tmp c cpuopt=()
	# fresh names per case: Podman frees a name only once removal completes
	WT_SEQ=$((${WT_SEQ:-0} + 1))
	name=wt-ctr-$$-$WT_SEQ noisy=wt-noisy-$$-$WT_SEQ
	tmp=$(mktemp -d)
	if [ -n "$period" ]; then
		cpuopt=(--cpu-period="$period" --cpu-quota="$(awk -v l="$limit" -v p="$period" 'BEGIN { printf "%d", l * p }')")
	else
		cpuopt=(--cpus="$limit")
	fi
	[ -z "$shares" ] || cpuopt+=(--cpu-shares="$shares")
	if [ "$neigh" = noisy ]; then
		WT_CTRS+=("$noisy")
		$DOCKER run -d --name "$noisy" --network none "$IMAGE" \
			/noisy "$(nproc)" "$(awk -v s="$secs" 'BEGIN { print s + 2 }')" > /dev/null
		sleep 0.5
	fi
	WT_CTRS+=("$name")
	trace_start
	if [ "$wl" = reqlat ]; then
		$DOCKER run -d --name "$name" --network none "${cpuopt[@]}" "$IMAGE" /reqlat \
			"${REQ_THREADS:-4}" "${REQ_WORK_US:-1000}" "${REQ_PERIOD_US:-20000}" "$secs" "$threads" > /dev/null
	else
		$DOCKER run -d --name "$name" --network none "${cpuopt[@]}" "$IMAGE" /spin "$threads" "$secs" > /dev/null
	fi
	cg=$(container_cgroup "$name")
	log "container cgroup: ${cg#"$CG_FS"} cpu.max=\"$(cat "$cg/cpu.max")\" cpu.weight=$(cat "$cg/cpu.weight")"
	# poll cpu.stat while running: the cgroup goes away with the container
	st=$(cg_stat "$cg")
	while [ "$($DOCKER inspect -f '{{.State.Running}}' "$name")" = true ]; do
		st=$(cg_stat "$cg" 2>/dev/null || echo "$st")
		sleep 0.2
	done
	trace_stop "$tmp/trace"
	out=$($DOCKER logs "$name" 2>&1 | tail -1)
	for c in "$name" "$noisy"; do
		$DOCKER rm -f "$c" > /dev/null 2>&1 || true
	done
	conc=$(conc_of "$wl" "$tmp/trace" "$limit")
	rm -rf "$tmp"
	log "$variant limit=$limit threads=$threads neigh=$neigh: $out | cpu.stat nr_throttled/throttled_ms/usage_ms: $st | conc avg/max/%over: $conc"
	emit_row "$(runtime_label)" "$variant" "$limit" "$threads" "$neigh" "$secs" "$wl" "$out" "$st" "$conc"
}

docker_main() {
	local secs=5 wl=spin neigh=none period='' shares='' header=0 opt
	while getopts "s:w:n:P:S:Hh" opt; do
		case $opt in
		s) secs=$OPTARG ;;
		w) wl=$OPTARG ;;
		n) neigh=$OPTARG ;;
		P) period=$OPTARG ;;
		S) shares=$OPTARG ;;
		H) header=1 ;;
		h) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
		esac
	done
	shift $((OPTIND - 1))
	[ $# -eq 3 ] || { usage >&2; exit 2; }
	check_case "$2" "$3" "$secs"
	[ -z "$period" ] || check_int PERIOD_US "$period"
	[ -z "$shares" ] || check_int SHARES "$shares"
	docker_check
	need_root
	check_cgroup2
	trap cleanup EXIT INT TERM
	variant_start "$1"
	[ "$header" = 0 ] || echo "$CSV_HEADER"
	run_docker_case "$1" "$2" "$3" "$secs" "$wl" "$neigh" "$period" "$shares"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	docker_main "$@"
fi
