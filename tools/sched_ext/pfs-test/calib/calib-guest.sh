#!/bin/bash
# calib-guest.sh ARM OUTDIR : one arm's cells inside one boot. ARM = cfs | scx3 | kpfs | kwidth.
#
# This wrapper owns the arm setup, so the sub-harnesses are always called with a variant string
# that makes them do no setup of their own (they would otherwise fight over it, and kheavy3 tears
# scx down at its end while pg still needs it up). Both sub-harnesses label their own lines with a
# variant they infer themselves, which is wrong for an scx arm on a sched_pfs=0 boot, so the arm is
# marked by the "D arm=" line printed before each sub-harness and the post-processing keys on that.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
ARM=${1:?arm} O=${2:?outdir}; mkdir -p $O
TD=$(cd "$(dirname "$0")" && pwd)
# The Postgres cell driver, beside this file in rm2/. Resolved relative to this script: the
# old absolute $S/rm2/t/ path no longer exists anywhere.
PG=${PG:-$TD/../rm2/pg-guest.sh}
[ -f "$PG" ] || { echo "R ABORT: no Postgres cell driver at $PG (set PG)"; exit 1; }
KW=/proc/sys/kernel/sched_cfs_bandwidth_width
SP=
echo "D boot kernel=$(uname -r) arm=$ARM cpus=$(nproc) cmdline_pfs=$(grep -c sched_pfs=1 /proc/cmdline)"

case $ARM in
scx3)	${BIN:?BIN required for scx3} -i 3600 > $O/loader-$ARM.log 2>&1 & SP=$!
	for i in $(seq 100); do [ "$(cat /sys/kernel/sched_ext/state 2>/dev/null)" = enabled ] && break; sleep 0.1; done
	st=$(cat /sys/kernel/sched_ext/state 2>/dev/null)
	echo "D arm=$ARM scx_state=$st"
	[ "$st" = enabled ] || { echo "R ABORT arm=$ARM: sched_ext not enabled"; exit 1; } ;;
kwidth)	[ -w $KW ] || { echo "R ABORT arm=$ARM: no $KW"; exit 1; }
	echo 1 > $KW; echo "D arm=$ARM width=$(cat $KW)" ;;
cfs)	[ -w $KW ] && echo 0 > $KW
	echo "D arm=$ARM width=$(cat $KW 2>/dev/null || echo na)" ;;
kpfs)	grep -qw sched_pfs=1 /proc/cmdline || { echo "R ABORT arm=$ARM: not a sched_pfs=1 boot"; exit 1; }
	echo "D arm=$ARM pfs=on" ;;
*)	echo "R ABORT: unknown arm $ARM"; exit 1 ;;
esac

echo "D arm=$ARM cell=kheavy"
env KB=${KB:?KB required} KSRC=${KSRC:?KSRC required} \
	bash $S/fix3/calib/kheavy3.sh cfs $O/kh-$ARM 1.5 $ARM

for cl in 16 32; do
	echo "D arm=$ARM cell=pg clients=$cl"
	CL=$cl bash "$PG" "2 4" "tpcb select" 30 2>&1 | grep -E '^R |failed|PG-DONE'
done

# Postgres and its tmpfs must go before the next arm runs in this same boot. Without this the
# first arm's failed pg left a 2 GB tmpfs holding scale-20 data and a live postgres behind, and the
# next arm's scx_pfs loader was OOM-killed - one arm's failure became the other's.
for i in $(seq 20); do pgrep -x postgres > /dev/null || break; pkill -x postgres; sleep 0.5; done
pgrep -x postgres > /dev/null && pkill -9 -x postgres
sleep 1
mountpoint -q /tmp/pgd && umount -l /tmp/pgd
rmdir /sys/fs/cgroup/rt/pod/ctr /sys/fs/cgroup/rt/pod /sys/fs/cgroup/rt 2>/dev/null
echo "D arm=$ARM cleanup postgres=$(pgrep -xc postgres 2>/dev/null || echo 0) pgd_mounted=$(mountpoint -q /tmp/pgd && echo yes || echo no) mem_avail_mb=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)"

if [ -n "$SP" ]; then
	kill -INT $SP 2>/dev/null; wait $SP 2>/dev/null
	echo "D arm=$ARM scx_unloaded state=$(cat /sys/kernel/sched_ext/state 2>/dev/null)"
fi
[ "$ARM" = kwidth ] && echo 0 > $KW
echo "CALIB-ARM-DONE arm=$ARM"
