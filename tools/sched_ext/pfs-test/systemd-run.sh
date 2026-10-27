#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# One test case as a transient systemd unit (systemd-run), no containers:
# CPUQuota= becomes cpu.max and CPUWeight= cpu.weight of the unit's cgroup.
set -euo pipefail
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$KIT/lib.sh"
# shellcheck source=cg-run.sh
. "$KIT/cg-run.sh"

WT_SLICE=${WT_SLICE:-pfstest.slice}
WT_UNITS=()

usage() {
	cat <<EOF
usage: $0 [options] VARIANT LIMIT THREADS

  VARIANT   cfs | scx | kwidth
  LIMIT     CPUs, passed as CPUQuota=LIMIT*100%
  THREADS   busy threads (with -w reqlat: background burners)

options:
  -s SECS       run time (default 5)
  -w WORKLOAD   spin (default) or reqlat
  -n NEIGHBOUR  none (default) or noisy: an unlimited unit with one busy thread per CPU
  -m MODE       scope (default: systemd-run --scope, the command stays a child
                of this shell) or service (a transient service, detached)
  -P PERIOD_US  also set CPUQuotaPeriodSec= (default: systemd's 100 ms)
  -W WEIGHT     CPUWeight= (default 100)
  -H            print the CSV header first
  -h            this text

The units go into $WT_SLICE. Equivalent by hand:
  systemd-run --scope -p CPUQuota=150% -p CPUWeight=100 --slice=$WT_SLICE ./bin/spin 8 10
  systemd-run --unit=wt-demo -p CPUQuota=50% ./bin/spin 8 10
  cat /sys/fs/cgroup\$(systemctl show -P ControlGroup wt-demo.service)/cpu.stat
Prints one CSV line ($CSV_HEADER).
EOF
}

# unit_cgroup UNIT: cgroup directory of a running unit
unit_cgroup() {
	local p _
	for _ in $(seq 50); do
		p=$(systemctl show -P ControlGroup "$1" 2>/dev/null || true)
		if [ -n "$p" ] && [ -d "$CG_FS$p" ]; then
			echo "$CG_FS$p"
			return 0
		fi
		sleep 0.05
	done
	return 1
}

# start_unit MODE NAME OUTFILE PROPS... -- CMD...
start_unit() {
	local mode=$1 name=$2 outf=$3 props=()
	shift 3
	while [ "$1" != -- ]; do
		props+=(-p "$1")
		shift
	done
	shift
	WT_UNITS+=("$name.$mode")
	WT_SLICE_USED=1
	if [ "$mode" = scope ]; then
		systemd-run --quiet --scope --unit="$name" --slice="$WT_SLICE" "${props[@]}" "$@" > "$outf" 2>&1 &
		WT_PIDS+=($!)
	else
		systemd-run --quiet --unit="$name" --slice="$WT_SLICE" "${props[@]}" \
			-p StandardOutput="truncate:$outf" -p StandardError=journal "$@"
	fi
}

stop_units() {
	local u
	for u in "${WT_UNITS[@]}"; do
		systemctl stop "$u" > /dev/null 2>&1 || true
		systemctl reset-failed "$u" > /dev/null 2>&1 || true
	done
	WT_UNITS=()
}

# stop_slice: remove the (now empty) slice the units ran in
stop_slice() {
	[ -n "${WT_SLICE_USED:-}" ] || return 0
	systemctl stop "$WT_SLICE" > /dev/null 2>&1 || true
	WT_SLICE_USED=
}

# run_systemd_case VARIANT LIMIT THREADS SECS WORKLOAD NEIGHBOUR MODE PERIOD WEIGHT
run_systemd_case() {
	local variant=$1 limit=$2 threads=$3 secs=$4 wl=$5 neigh=$6 mode=$7 period=$8 weight=$9
	local name noisy props cg out st conc tmp
	WT_SEQ=$((${WT_SEQ:-0} + 1))
	name=wt-ctr-$$-$WT_SEQ noisy=wt-noisy-$$-$WT_SEQ
	tmp=$(mktemp -d)
	chmod 755 "$tmp"
	props=("CPUQuota=$(awk -v l="$limit" 'BEGIN { printf "%d%%", l * 100 }')" "CPUWeight=$weight")
	[ -z "$period" ] || props+=("CPUQuotaPeriodSec=$(awk -v p="$period" 'BEGIN { printf "%gms", p / 1000 }')")
	if [ "$neigh" = noisy ]; then
		start_unit "$mode" "$noisy" "$tmp/noisy.out" -- \
			"$KIT/bin/noisy" "$(nproc)" "$(awk -v s="$secs" 'BEGIN { print s + 2 }')"
		sleep 0.5
	fi
	trace_start
	if [ "$wl" = reqlat ]; then
		start_unit "$mode" "$name" "$tmp/out" "${props[@]}" -- "$KIT/bin/reqlat" \
			"${REQ_THREADS:-4}" "${REQ_WORK_US:-1000}" "${REQ_PERIOD_US:-20000}" "$secs" "$threads"
	else
		start_unit "$mode" "$name" "$tmp/out" "${props[@]}" -- "$KIT/bin/spin" "$threads" "$secs"
	fi
	cg=$(unit_cgroup "$name.$mode") || die "no cgroup for $name.$mode"
	log "unit cgroup: ${cg#"$CG_FS"} cpu.max=\"$(cat "$cg/cpu.max")\" cpu.weight=$(cat "$cg/cpu.weight")"
	# the cgroup goes away with the unit: poll cpu.stat while it runs
	st=$(cg_stat "$cg")
	while [ -d "$cg" ] && [ -n "$(cat "$cg/cgroup.procs" 2>/dev/null)" ]; do
		st=$(cg_stat "$cg" 2>/dev/null || echo "$st")
		sleep 0.2
	done
	trace_stop "$tmp/trace"
	sleep 0.2
	out=$(tail -1 "$tmp/out" 2>/dev/null || true)
	stop_units
	kill_pids
	conc=$(conc_of "$wl" "$tmp/trace" "$limit")
	rm -rf "$tmp"
	log "$variant limit=$limit threads=$threads neigh=$neigh: $out | cpu.stat nr_throttled/throttled_ms/usage_ms: $st | conc avg/max/%over: $conc"
	emit_row "systemd-$mode" "$variant" "$limit" "$threads" "$neigh" "$secs" "$wl" "$out" "$st" "$conc"
}

systemd_main() {
	local secs=5 wl=spin neigh=none mode=scope period='' weight=100 header=0 opt
	while getopts "s:w:n:m:P:W:Hh" opt; do
		case $opt in
		s) secs=$OPTARG ;;
		w) wl=$OPTARG ;;
		n) neigh=$OPTARG ;;
		m) mode=$OPTARG ;;
		P) period=$OPTARG ;;
		W) weight=$OPTARG ;;
		H) header=1 ;;
		h) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
		esac
	done
	shift $((OPTIND - 1))
	[ $# -eq 3 ] || { usage >&2; exit 2; }
	check_case "$2" "$3" "$secs"
	[ -z "$period" ] || check_int PERIOD_US "$period"
	check_int WEIGHT "$weight"
	case $mode in scope|service) ;; *) die "mode must be scope or service" ;; esac
	command -v systemd-run > /dev/null || die "systemd-run not found"
	[ -d /run/systemd/system ] || die "systemd is not the init system"
	need_root
	check_cgroup2
	trap cleanup EXIT INT TERM
	variant_start "$1"
	[ "$header" = 0 ] || echo "$CSV_HEADER"
	run_systemd_case "$1" "$2" "$3" "$secs" "$wl" "$neigh" "$mode" "$period" "$weight"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	systemd_main "$@"
fi
