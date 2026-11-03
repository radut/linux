#!/bin/bash
# mech-guest.sh ARM OUTDIR : one arm's phase 0 + phase 1 cells inside one boot.
#
# Like calib-guest.sh, but kheavy is dropped (neither question is about it) and the scx_pfs
# loader is run so that its counters are actually observable:
#   -i 1   print the 27 global counters every second instead of once an hour. The calibration
#          used -i 3600, so the only block it ever logged was the one at t=0, which is why no
#          scx_pfs counter appears anywhere in the 10-round data set.
#   -S F   dump the per-cgroup throttling counters to F every second. pg-guest.sh has always had
#          the SCXS/SF hooks for this; nothing ever set them.
# cpu.stat's own throttling fields stay zero under a BPF scheduler - measured, c_thr_per=0 on
# every scx3 cell of the kept set - so this dump is the only throttling signal scx_pfs has.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
ARM=${1:?arm} O=${2:?outdir}; mkdir -p $O
export MD=$S/fix3/mech
KW=/proc/sys/kernel/sched_cfs_bandwidth_width
SP=
# Phase 1: 4 against 4.001 and 2 against 2.001. Both schedulers ceil the limit to a width, so
# 4.001 CPUs buys a fifth slot for 0.025% more quota. Same clients throughout: 16, where kPFS's
# limit-4 shortfall is largest (3.285-3.349 of 4).
LIMITS=${LIMITS:-"4 4.001 2 2.001"}
export CL=${CL:-16}
echo "D boot kernel=$(uname -r) arm=$ARM cpus=$(nproc) cmdline_pfs=$(grep -c sched_pfs=1 /proc/cmdline) limits=$LIMITS cl=$CL"

case $ARM in
scx3|scx3b)
	# One branch, two names. scx3b exists so that TWO scx_pfs builds can be measured inside one
	# boot - the caller points BIN at a different binary per arm (PFS_SCXBIN_scx3,
	# PFS_SCXBIN_scx3b in mech.sh) - and it must therefore differ from scx3 in nothing but the
	# name, or the contrast would measure the harness as well as the two builds. SCXL lands in
	# the per-arm output directory, so the two loaders do not share a log.
	export SF=/tmp/scxcg SCXS=1 SCXL=$O/scx-stats.log
	${BIN:?BIN required for arm $ARM} -i 1 -S $SF > $SCXL 2>&1 & SP=$!
	for i in $(seq 100); do [ "$(cat /sys/kernel/sched_ext/state 2>/dev/null)" = enabled ] && break; sleep 0.1; done
	st=$(cat /sys/kernel/sched_ext/state 2>/dev/null)
	echo "D arm=$ARM scx_state=$st stats_file=$SF log=$SCXL"
	[ "$st" = enabled ] || { echo "R ABORT arm=$ARM: sched_ext not enabled"; exit 1; } ;;
cfs)	[ -w $KW ] && echo 0 > $KW
	# Read the sysctl BACK instead of trusting the write. On a kernel that carries width mode the
	# file can already hold 1 - the kwidth arm of this same boot puts it there - and a write that
	# did not take would measure kwidth while filing the rows under cfs. A wrong label is worse
	# than a missing one, so this aborts rather than reports.
	w=$(cat $KW 2>/dev/null || echo na)
	[ "$w" = na ] || [ "$w" = 0 ] || { echo "R ABORT arm=$ARM: width mode reads '$w' after writing 0"; exit 1; }
	echo "D arm=$ARM width=$w" ;;
kwidth)	# v1: the same concurrency-cap policy as scx_pfs and kPFS, but inside CFS bandwidth control,
	# switched on by the sysctl the width-mode patch adds. Three ways this arm could quietly
	# measure stock CFS and publish the rows as kwidth, each fatal here rather than reported:
	#   * the kernel does not carry the patch        -> the file does not exist
	#   * the file exists but the write did not take -> read it back and require 1
	#   * this is a sched_pfs=1 boot                 -> the fair class is not what runs the group,
	#                                                   so the sysctl governs nothing at all
	grep -qw sched_pfs=1 /proc/cmdline && {
		echo "R ABORT arm=$ARM: kwidth is a CFS bandwidth mode, not measurable on a sched_pfs=1 boot"; exit 1; }
	[ -w $KW ] || { echo "R ABORT arm=$ARM: $KW absent or read-only, this kernel has no width mode"; exit 1; }
	echo 1 > $KW 2>/dev/null
	w=$(cat $KW 2>/dev/null || echo na)
	[ "$w" = 1 ] || { echo "R ABORT arm=$ARM: width mode reads '$w' after writing 1"; exit 1; }
	echo "D arm=$ARM width=$w" ;;
kpfs)	grep -qw sched_pfs=1 /proc/cmdline || { echo "R ABORT arm=$ARM: not a sched_pfs=1 boot"; exit 1; }
	echo "D arm=$ARM pfs=on debugfs=$([ -d /sys/kernel/debug/sched/pfs ] && echo yes || echo no)"
	echo "D arm=$ARM features=$(cat /sys/kernel/debug/sched/pfs/features 2>/dev/null) window_us=$(cat /sys/kernel/debug/sched/pfs/window_us 2>/dev/null) burst_pct=$(cat /sys/kernel/debug/sched/pfs/burst_pct 2>/dev/null)" ;;
*)	echo "R ABORT: unknown arm $ARM"; exit 1 ;;
esac

echo "D arm=$ARM cell=pg clients=$CL"
bash $MD/mech-pg.sh "$LIMITS" "tpcb select" 30 2>&1 | grep -E '^R |^D |^G |failed|PG-DONE'

for i in $(seq 20); do pgrep -x postgres > /dev/null || break; pkill -x postgres; sleep 0.5; done
pgrep -x postgres > /dev/null && pkill -9 -x postgres
sleep 1
mountpoint -q /tmp/pgd && umount -l /tmp/pgd
rmdir /sys/fs/cgroup/rt/pod/ctr /sys/fs/cgroup/rt/pod /sys/fs/cgroup/rt 2>/dev/null
echo "D arm=$ARM cleanup postgres=$(pgrep -xc postgres 2>/dev/null || echo 0) mem_avail_mb=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)"

if [ -n "$SP" ]; then
	kill -INT $SP 2>/dev/null; wait $SP 2>/dev/null
	# Wait for sched_ext to actually leave "enabled" before this arm's process exits. With two scx
	# arms in one boot the next loader attaches straight after, and would fail if the previous one
	# were still attached. Bounded at 10 s, and the state reached is printed either way.
	for i in $(seq 100); do [ "$(cat /sys/kernel/sched_ext/state 2>/dev/null)" = enabled ] || break; sleep 0.1; done
	echo "D arm=$ARM scx_unloaded state=$(cat /sys/kernel/sched_ext/state 2>/dev/null) scx_log_lines=$(wc -l < $SCXL 2>/dev/null)"
fi
# Arms share a boot, so an arm that changed a global leaves it as it found it. Only kwidth does:
# without this a later cfs arm in the same boot would run with width mode still on, and the cfs
# read-back above would abort the boot rather than measure the wrong thing - correct, but it would
# make the arm ORDER decide whether a run completes, which is not a property a harness should have.
if [ "$ARM" = kwidth ] && [ -w $KW ]; then
	echo 0 > $KW 2>/dev/null
	echo "D arm=$ARM width_restored=$(cat $KW 2>/dev/null || echo na)"
fi
echo "CALIB-ARM-DONE arm=$ARM"
