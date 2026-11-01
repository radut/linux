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
scx3)	export SF=/tmp/scxcg SCXS=1 SCXL=$O/scx-stats.log
	${BIN:?BIN required for scx3} -i 1 -S $SF > $SCXL 2>&1 & SP=$!
	for i in $(seq 100); do [ "$(cat /sys/kernel/sched_ext/state 2>/dev/null)" = enabled ] && break; sleep 0.1; done
	st=$(cat /sys/kernel/sched_ext/state 2>/dev/null)
	echo "D arm=$ARM scx_state=$st stats_file=$SF log=$SCXL"
	[ "$st" = enabled ] || { echo "R ABORT arm=$ARM: sched_ext not enabled"; exit 1; } ;;
cfs)	[ -w $KW ] && echo 0 > $KW
	echo "D arm=$ARM width=$(cat $KW 2>/dev/null || echo na)" ;;
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
	echo "D arm=$ARM scx_unloaded state=$(cat /sys/kernel/sched_ext/state 2>/dev/null) scx_log_lines=$(wc -l < $SCXL 2>/dev/null)"
fi
echo "CALIB-ARM-DONE arm=$ARM"
