#!/bin/bash
# pg-guest.sh "LIMITS" "WLS" SECS: postgres in a two-level pod cgroup, pgbench (TCP, 32 clients) from the root
# cgroup; per cell: CPUs from cpu.stat, PFS stats delta, sched_switch concurrency trace over 5 s.
P5=${PFS_WORK}/p5 TD=$(cd "$(dirname "$0")" && pwd)
LIMITS=${1:-"4 1.5"} WLS=${2:-"tpcb select"} SECS=${3:-30} CL=${CL:-32}
PGB=$P5/root/usr/lib/postgresql/16/bin
export LD_LIBRARY_PATH=$P5/root/usr/lib/x86_64-linux-gnu
CG=/sys/fs/cgroup TR=/sys/kernel/tracing D=/sys/kernel/debug/sched/pfs
mountpoint -q $TR || mount -t tracefs nodev $TR
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
V=${PGV:-cfs}
echo "kernel $(uname -r) variant=$V cpus=$(nproc) limits=$LIMITS wls=$WLS secs=$SECS"
echo +cpu > $CG/cgroup.subtree_control
mkdir -p $CG/rt; echo +cpu > $CG/rt/cgroup.subtree_control
mkdir -p $CG/rt/pod; echo +cpu > $CG/rt/pod/cgroup.subtree_control; mkdir -p $CG/rt/pod/ctr
setlim() { local q; q=$(awk -v l=$1 'BEGIN { printf "%d", l * 100000 }')
	echo "$q 100000" > $CG/rt/pod/cpu.max; echo "$q 100000" > $CG/rt/pod/ctr/cpu.max; }
setlim 4
mkdir -p /tmp/pgd; mount -t tmpfs -o size=2G tmpfs /tmp/pgd; chown 1000:1000 /tmp/pgd
asu() { setpriv --reuid=1000 --regid=1000 --clear-groups "$@"; }
asu $PGB/initdb -D /tmp/pgd/data -U postgres > /dev/null 2>&1 || { echo "initdb failed"; exit 1; }
bash -c "echo \$\$ > $CG/rt/pod/ctr/cgroup.procs; exec setpriv --reuid=1000 --regid=1000 --clear-groups $PGB/postgres -D /tmp/pgd/data \
	-c shared_buffers=512MB -c max_connections=200 -c synchronous_commit=off -c log_min_messages=warning \
	-c listen_addresses=127.0.0.1 -k /tmp/pgd" > /tmp/pg.log 2>&1 &
for i in $(seq 60); do $PGB/pg_isready -q -h 127.0.0.1 -U postgres && break; sleep 0.5; done
$PGB/pgbench -i -q -s ${SCALE:-20} -h 127.0.0.1 -U postgres postgres > /tmp/pginit.log 2>&1 || { echo "pgbench -i failed"; tail /tmp/pginit.log; }
echo mono > $TR/trace_clock; echo 65536 > $TR/buffer_size_kb
pstat() { [ -d $D ] && cat $D/stats; }
# tsnap: pod cpu.stat throttle counters + scx_pfs -S counters of /rt/pod (SCXS set by guest.sh for scx)
tsnap() { local c s="- - -" d=${1:-pod}
	c=$(awk '/^nr_periods/ {p=$2} /^nr_throttled/ {n=$2} /^throttled_usec/ {t=$2} END {print p+0, n+0, t+0}' $CG/rt/$d/cpu.stat)
	[ -n "$SCXS" ] && s=$(awk -v p=/rt/$d '$1 == p {print $3, $5, $7; f=1} END {if (!f) print 0, 0, 0}' $SF 2>/dev/null)
	echo "$c ${s:-0 0 0}"; }
for lim in $LIMITS; do for wl in $WLS; do for rep in $(seq ${REPS:-1}); do
	setlim $lim
	sflag=; [ $wl = select ] && sflag=-S
	$PGB/psql -q -h 127.0.0.1 -U postgres -c 'vacuum analyze' -c checkpoint postgres
	[ -n "$SCXS" ] && sleep 1.1
	z0=$(tsnap); y0=$(tsnap pod/ctr); pstat > /tmp/s0; u0=$(awk '/usage_usec/ {print $2}' $CG/rt/pod/cpu.stat); t0=$(date +%s.%N)
	$PGB/pgbench -h 127.0.0.1 -U postgres -n $sflag -c $CL -j 4 -T $SECS postgres > /tmp/pgb.out 2>&1 &
	bp=$!
	sleep $((SECS / 2 - 3))
	echo 0 > $TR/tracing_on; echo > $TR/trace
	echo 1 > $TR/events/sched/sched_switch/enable
	m0=$(awk '/usage_usec/ {print $2}' $CG/rt/pod/cpu.stat); n0=$(date +%s.%N); echo 1 > $TR/tracing_on
	sleep 5
	echo 0 > $TR/tracing_on; m1=$(awk '/usage_usec/ {print $2}' $CG/rt/pod/cpu.stat); n1=$(date +%s.%N)
	echo 0 > $TR/events/sched/sched_switch/enable
	cat $CG/rt/pod/ctr/cgroup.threads > /tmp/pids
	cat $TR/trace > /tmp/sw.trace; echo > $TR/trace
	wait $bp
	t1=$(date +%s.%N); u1=$(awk '/usage_usec/ {print $2}' $CG/rt/pod/cpu.stat); pstat > /tmp/s1
	[ -n "$SCXS" ] && sleep 1.1
	z1=$(tsnap); y1=$(tsnap pod/ctr)
	cpus=$(awk -v a=$u0 -v b=$u1 -v s=$t0 -v e=$t1 'BEGIN { printf "%.3f", (b - a) / 1e6 / (e - s) }')
	tps=$(awk '/^tps/ {print int($3)}' /tmp/pgb.out); lat=$(awk '/latency average/ {print $4}' /tmp/pgb.out)
	st=""; [ -s /tmp/s1 ] && st=$(awk 'NR==FNR {a[$1]=$2; next} $1 ~ /^(admit|credit|park_full|park_budget|park_fifo|rotate|handoff|user_park|credit_ns|slot_ns|park_debt|wake_park)$/ {printf "%s=%d ", $1, $2-a[$1]}' /tmp/s0 /tmp/s1)
	echo "R v=$V lim=$lim wl=$wl rep=$rep tps=$tps lat_ms=$lat cpus=$cpus mid_cpus=$(awk -v a=$m0 -v b=$m1 -v s=$n0 -v e=$n1 'BEGIN { printf "%.3f", (b - a) / 1e6 / (e - s) }') $(python3 $TD/conc5.py /tmp/sw.trace /tmp/pids $lim) $(echo "$z0 $z1" | awk -v a=$t0 -v b=$t1 '{w=b-a; printf "thr_per=%d thr_n=%d thr_us=%d thr_frac=%.3f", $7-$1, $8-$2, $9-$3, ($9-$3)/1e6/w; if ($4 != "-") printf " pfs_per=%d pfs_n=%d pfs_us=%d pfs_frac=%.3f", $10-$4, $11-$5, $12-$6, ($12-$6)/1e6/w}') $(echo "$y0 $y1" | awk -v a=$t0 -v b=$t1 '{w=b-a; printf "c_thr_per=%d c_thr_n=%d c_thr_us=%d c_thr_frac=%.3f", $7-$1, $8-$2, $9-$3, ($9-$3)/1e6/w; if ($4 != "-") printf " c_pfs_per=%d c_pfs_n=%d c_pfs_us=%d c_pfs_frac=%.3f", $10-$4, $11-$5, $12-$6, ($12-$6)/1e6/w}') $st"
	rm -f /tmp/sw.trace; : $P5RES/${TAG:-x}-$V-l$lim-$wl-r$rep.trace 2>/dev/null; 
done; done; done
[ -d $D ] && cat $D/stats
asu $PGB/pg_ctl -D /tmp/pgd/data stop -m fast > /dev/null 2>&1
echo PG-DONE
