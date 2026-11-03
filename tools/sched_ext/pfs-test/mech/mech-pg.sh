#!/bin/bash
# mech-pg.sh "LIMITS" "WLS" SECS : the Postgres cell driver for the mechanism phase.
#
# One invocation = one arm's cells inside one boot: Postgres in a two-level pod cgroup, pgbench
# driven from the root cgroup, one cell per (limit, workload, rep) in the ORDER GIVEN.
#
# PROVENANCE OF THIS FILE. The measuring core - the cgroup layout, setlim, the Postgres and
# pgbench invocations, tsnap/toksnap/scxsnap, the cell loop, the 5 s mid window with the 100 Hz
# group sampler inside it, and every field of the R, D cell, D pgprog, D scx0/D scx1, D grpsample
# and G rows - is recovered, not redesigned: it is rm2/pg-guest.sh plus the eight replacements the
# original was built from, and the result reproduces the original's own line numbering. What is
# NOT from the original is marked `# RECONSTRUCTED:` at each site, and HARNESS.md lists those
# sites again. Read that list before treating any of them as a measured decision.
#
# What this driver measures, and why each piece is here:
#
#   1. THE WIDTH EXPERIMENT needs no code change at all: a limit of 4.001 CPUs is passed in LIMITS
#      beside 4. Both schedulers CEIL the limit to a concurrency cap:
#        kPFS      limit  = ceil(quota_us * 1000 / period_us)   pfs.c:3619
#                  width  = DIV_ROUND_UP(limit, 1000)           pfs.c:562, used at :877
#        scx_pfs   limit  = div_round_up(quota_us * FRAC_ONE, period_us)  scx_pfs.bpf.c:387
#                  width  = div_round_up(limit, FRAC_ONE)       scx_pfs.bpf.c:736, used at :758
#      so 4001 mcpu buys a FIFTH slot for 0.025% more quota. setlim's "%d", l * 100000 turns
#      4.001 into a quota of 400100 us per 100 ms period, i.e. exactly 4001 mcpu. Fractional
#      limits with an integer part of their own (1.5, 2.1, 2.5, 2.9, 2.999, 3.5) go through the
#      same path, and setlim now READS cpu.max BACK and aborts on a mismatch rather than trusting
#      the conversion.
#   2. THROTTLING UNDER A BPF SCHEDULER. cpu.stat's own throttling fields stay zero under
#      sched_ext (measured: c_thr_per=0 on every scx3 cell of the kept calibration set), so the
#      loader is run with -i 1 -S by the caller and its per-cgroup dump and 27 global counters are
#      read here. tokens_ns, which rm2/pg-guest.sh drops, is captured too.
#   3. PER-TRANSACTION counters. Deltas are taken over the WHOLE cell and divided by the
#      transactions pgbench completed in it, because tps is also a whole-cell number. The 5 s mid
#      window stays exactly what it was and is used for mid_cpus alone. Mixing the two would
#      divide a 30 s counter by a 5 s transaction count.
#   4. 100 Hz sampling of kPFS's per-cgroup slots/waiting/tokens across that same mid window, the
#      only thing that separates "the scheduler held work back" from "the benchmark offered none".
#
# MEASUREMENT RULES THIS FILE IS REQUIRED TO HONOUR, and where each one lives:
#   * cpu.stat usage_usec is CUMULATIVE. Every CPU figure here is a delta across a window whose
#     length is measured with date(1) and PRINTED BESIDE THE RATE (cell_secs, mid_secs, thr_win,
#     c_thr_win, trace_secs). No rate is ever a cumulative counter over a nominal duration.
#   * No counter is reset by recreating a cgroup - that is what the deltas are for. A cgroup left
#     behind by an earlier arm is a FATAL error, not something to mkdir -p over: see the stale
#     check below. A silently-failing rmdir in an older harness let a cumulative counter keep its
#     history and made one cell read 4.9 CPUs against a limit of 1.5.
#   * Each denominator names the interval AND the task set it covers: cpu_src says which cgroup
#     the cpu.stat deltas are taken from, conc_pids how many threads the trace filter matched.
#   * CELL ORDER IS A PARAMETER. LIMITS and WLS are consumed in the order given, so a caller can
#     reverse them (mech-rev.sh does); REV=1 reverses both in place. A fixed order confounds every
#     step comparison, so the order in force is printed in the D boot line.
#
# Parameters, all from the environment except the three positional ones:
#   $1 LIMITS   space-separated cpu.max limits in CPUs, IN CELL ORDER   default "4 1.5"
#   $2 WLS      space-separated workloads in cell order: tpcb | select  default "tpcb select"
#   $3 SECS     measured seconds per cell                               default 30
#   PFS_WORK    work directory holding the Postgres rootfs under $PFS_WORK/p5   required
#   MD          directory holding sample-groups.py                      default: this script's dir
#   CL          pgbench clients                                         default 32
#   REPS        reps per (limit, workload) cell                         default 1
#   SCALE       pgbench -i scale factor                                 default 20
#   GRPHZ       group-sampler rate, Hz                                  default 100
#   PGV         the v= label on every R row                             default cfs
#   REV         1 reverses LIMITS and WLS                               default 0
#   WARM        1 force / 0 off / auto = once per boot, via PG_WARM_STAMP   default auto
#   WARMSECS    seconds of the discarded warm-up cell                    default 10
#   PG_WARM_STAMP  file marking that this boot has paid its warm-up      default /tmp/pg-warmed
#   PG_GATE     if set, wait UNBOUNDEDLY for the guest 1-min load to fall below it before the
#               first cell. Unset means no gate, and the D pgload row says gate=off, so a missing
#               gate can never read as a passed one. There is no timeout and no cap: a gate that
#               proceeds after a cap is not a gate, and a 120 s cap once let a boot run at
#               loadavg 15.
#   SCXS/SF/SCXL  set by the caller for an scx_pfs arm: enable the dump, its path, loader log
# Rows out: see HARNESS.md. Callers filter with grep -E '^R |^D |^G |failed|PG-DONE'.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
P5=$S/p5 TD=$(cd "$(dirname "$0")" && pwd)
LIMITS=${1:-"4 1.5"} WLS=${2:-"tpcb select"} SECS=${3:-30} CL=${CL:-32}
PGB=$P5/root/usr/lib/postgresql/16/bin
export LD_LIBRARY_PATH=$P5/root/usr/lib/x86_64-linux-gnu
CG=/sys/fs/cgroup TR=/sys/kernel/tracing D=/sys/kernel/debug/sched/pfs
# RECONSTRUCTED: the original required MD; sample-groups.py sits beside this file, so it defaults.
MD=${MD:-$TD}
# RECONSTRUCTED: the order knob and the warm-up/gate knobs. LIMITS/WLS order itself is original.
REV=${REV:-0} WARM=${WARM:-auto} WARMSECS=${WARMSECS:-10}
WARM_STAMP=${PG_WARM_STAMP:-/tmp/pg-warmed}
PG_GATE=${PG_GATE:-}
# conc-pids.py is the in-tree name of the time-weighted concurrency reducer the original called
# conc5.py; its output fields (trace_secs, mean_running, frac_over_limit, max, hist) are unchanged.
CONC=${CONC:-$TD/../rm2/conc-pids.py}
[ -f $MD/sample-groups.py ] || echo "D warn no sample-groups.py at $MD: group sampling disabled"
[ -f $CONC ] || { echo "R ABORT: no concurrency reducer at $CONC (set CONC)"; exit 1; }
mountpoint -q $TR || mount -t tracefs nodev $TR
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
V=${PGV:-cfs}
if [ "$REV" = 1 ]; then
	LIMITS=$(for x in $LIMITS; do echo $x; done | tac | tr '\n' ' ')
	WLS=$(for x in $WLS; do echo $x; done | tac | tr '\n' ' ')
fi
echo "kernel $(uname -r) variant=$V cpus=$(nproc) limits=$LIMITS wls=$WLS secs=$SECS"
echo "D pgdriver kernel=$(uname -r) variant=$V cpus=$(nproc) order=\"$LIMITS | $WLS\" rev=$REV cl=$CL secs=$SECS reps=${REPS:-1} scale=${SCALE:-20} grphz=${GRPHZ:-100} warm=$WARM warmsecs=$WARMSECS gate=${PG_GATE:-off}"

# RECONSTRUCTED: the stale-cgroup check. Nothing here resets a counter by recreating a cgroup -
# every rate below is a delta - but a leftover /rt tree means an earlier arm's teardown failed and
# its tasks may still be inside it. The removal is attempted and a FAILURE IS FATAL: an older
# harness let exactly this rmdir fail silently and measured a cumulative counter's whole history,
# reading 4.9 CPUs where the limit was 1.5.
for d in $CG/rt/pod/ctr $CG/rt/pod $CG/rt; do
	[ -d $d ] || continue
	echo "D cgstale path=$d procs=$(wc -l < $d/cgroup.procs 2>/dev/null || echo ?) threads=$(wc -l < $d/cgroup.threads 2>/dev/null || echo ?)"
	rmdir $d 2>/dev/null || { echo "R ABORT: stale cgroup $d could not be removed; refusing to measure into an inherited cumulative counter"; exit 1; }
done

echo +cpu > $CG/cgroup.subtree_control
mkdir -p $CG/rt; echo +cpu > $CG/rt/cgroup.subtree_control
mkdir -p $CG/rt/pod; echo +cpu > $CG/rt/pod/cgroup.subtree_control; mkdir -p $CG/rt/pod/ctr
# RECONSTRUCTED: the read-back and the D limset row. The conversion itself is the original's.
setlim() { local q r
	q=$(awk -v l=$1 'BEGIN { printf "%d", l * 100000 }')
	echo "$q 100000" > $CG/rt/pod/cpu.max; echo "$q 100000" > $CG/rt/pod/ctr/cpu.max
	r=$(cat $CG/rt/pod/cpu.max)
	[ "$r" = "$q 100000" ] || { echo "R ABORT: cpu.max for limit $1 wanted \"$q 100000\", reads \"$r\""; exit 1; }
	echo "D limset lim=$1 quota_us=$q period_us=100000 mcpu=$((q / 100)) pod_cpu_max=\"$r\" ctr_cpu_max=\"$(cat $CG/rt/pod/ctr/cpu.max)\""; }
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
usage() { awk '/usage_usec/ {print $2}' $CG/rt/pod/cpu.stat; }
# tsnap: pod cpu.stat throttle counters + scx_pfs -S counters of /rt/pod (SCXS set by the caller)
tsnap() { local c s="- - -" d=${1:-pod}
	c=$(awk '/^nr_periods/ {p=$2} /^nr_throttled/ {n=$2} /^throttled_usec/ {t=$2} END {print p+0, n+0, t+0}' $CG/rt/$d/cpu.stat)
	[ -n "${SCXS:-}" ] && s=$(awk -v p=/rt/$d '$1 == p {print $3, $5, $7; f=1} END {if (!f) print 0, 0, 0}' ${SF:-/dev/null} 2>/dev/null)
	echo "$c ${s:-0 0 0}"; }
# tokens_ns of the pod from the scx_pfs dump, field 9 of its PLAINFMT line; "-" with no dump
toksnap() { [ -n "${SCXS:-}" ] || { echo -; return; }
	awk -v p=/rt/pod '$1 == p {print $9; f=1} END {if (!f) print "-"}' ${SF:-/dev/null} 2>/dev/null; }
# The scx_pfs loader's global counters: the latest value of each, which is the newest block by
# construction. Taking the last N key=value tokens does not work - print_stats() emits
# "drain=%llu/%llu" as a single token and the per-cgroup block after it has key=value fields of
# its own - so each counter is matched by name and the slash-joined pair is split first.
scxsnap() { [ -n "${SCXL:-}" ] && [ -s "${SCXL:-}" ] || { echo; return; }
	tr ' ' '\n' < $SCXL | sed 's|^drain=\([0-9]*\)/\([0-9]*\)$|drain=\1\ndrain_fail=\2|' | awk -F= '
		$1 ~ /^(direct|shared|park_full|park_budget|admit|repark|retry|overflow|race|too_deep|activate|hweight|keep|park_woken|cut|yield_keep|yield_rotate|rescue|lost_path|credit|drain|drain_fail|no_dsq|park_race|park_debt|hotplug|yield_credit)$/ &&
		$2 ~ /^[0-9]+$/ { v[$1] = $2 }
		END { n = split("direct shared admit park_full park_budget park_debt park_woken park_race cut keep repark retry overflow race credit drain drain_fail yield_keep yield_rotate yield_credit rescue lost_path too_deep activate hweight no_dsq hotplug", o, " ")
		      for (i = 1; i <= n; i++) if (o[i] in v) printf "%s=%s ", o[i], v[o[i]] }'; }

# RECONSTRUCTED: the load record and the gate. The loadavg of the machine the cells run on goes
# into THIS stream, which is the per-boot output file the exclusion rule reads - not into a
# separate log, because a boot file with no load line cannot be tested at all and must never read
# as a pass. The wait is unbounded by construction: there is no counter and no exit from the loop
# other than the condition itself.
pgload() { echo "D pgload when=$1 loadavg=\"$(cut -d' ' -f1-3 /proc/loadavg)\" gate=${PG_GATE:-off} waited_s=${2:-0} $(date -u +%T)"; }
gate() { local w=0
	[ -n "$PG_GATE" ] || { pgload before 0; return; }
	until [ "$(awk -v g=$PG_GATE '{print ($1 < g)}' /proc/loadavg)" = 1 ]; do sleep 5; w=$((w + 5)); done
	pgload before $w; }

# RECONSTRUCTED: the warm-up cell. A boot's first measured cell runs against a lower ceiling (in
# the calibration the boot's SECOND arm read 1.66-2.00x the first arm's tps on the same cell), so
# one cell is run and DISCARDED first, in the shape of the cell it protects - the FIRST limit and
# workload of the order in force, not a fixed shape, because the order is a parameter and a fixed
# warm-up shape stops matching as soon as the caller reverses it. WARM=auto pays it once per boot
# via a stamp file in the guest's own /tmp; WARM=0 prints warm=off rather than omitting it in
# silence. Its tps is printed, so what the first run was worth is a measured number here too.
warmcell() {
	local wl0 lim0 sf w0 w1 u0 u1 tps
	case $WARM in
	0)	echo "D pgwarm discarded=0 warm=off"; return ;;
	auto)	[ -e "$WARM_STAMP" ] && { echo "D pgwarm discarded=0 warm=auto already_paid_this_boot stamp=$WARM_STAMP"; return; } ;;
	esac
	lim0=$(set -- $LIMITS; echo $1); wl0=$(set -- $WLS; echo $1)
	setlim $lim0
	sf=; [ "$wl0" = select ] && sf=-S
	$PGB/psql -q -h 127.0.0.1 -U postgres -c 'vacuum analyze' -c checkpoint postgres > /dev/null 2>&1
	u0=$(usage); w0=$(date +%s.%N)
	$PGB/pgbench -h 127.0.0.1 -U postgres -n $sf -c $CL -j 4 -T $WARMSECS postgres > /tmp/pgwarm.out 2>&1
	w1=$(date +%s.%N); u1=$(usage)
	tps=$(awk '/^tps/ {print int($3)}' /tmp/pgwarm.out)
	echo "D pgwarm discarded=1 warm=$WARM lim=$lim0 wl=$wl0 cl=$CL secs=$(awk -v a=$w0 -v b=$w1 'BEGIN{printf "%.3f", b-a}') tps=${tps:-na} cpus=$(awk -v a=$u0 -v b=$u1 -v s=$w0 -v e=$w1 'BEGIN { printf "%.3f", (b - a) / 1e6 / (e - s) }') cpu_src=/rt/pod"
	: > "$WARM_STAMP" 2>/dev/null || true; }

gate
warmcell
for lim in $LIMITS; do for wl in $WLS; do for rep in $(seq ${REPS:-1}); do
	setlim $lim
	sflag=; [ $wl = select ] && sflag=-S
	$PGB/psql -q -h 127.0.0.1 -U postgres -c 'vacuum analyze' -c checkpoint postgres
	[ -n "${SCXS:-}" ] && sleep 1.1
	# RECONSTRUCTED: zt0/zt1 bracket the throttle snapshots, so thr_frac and pfs_frac are divided
	# by the interval those snapshots actually span and not by the pgbench interval. The original
	# used t1-t0, which under an scx arm is short of the snapshot span by the two 1.1 s settles.
	zt0=$(date +%s.%N); z0=$(tsnap); y0=$(tsnap pod/ctr); pstat > /tmp/s0; k0=$(toksnap); x0=$(scxsnap)
	u0=$(usage); t0=$(date +%s.%N)
	$PGB/pgbench -h 127.0.0.1 -U postgres -n $sflag -c $CL -j 4 -T $SECS -P 1 postgres > /tmp/pgb.out 2>&1 &
	bp=$!
	sleep $((SECS / 2 - 3))
	echo 0 > $TR/tracing_on; echo > $TR/trace
	echo 1 > $TR/events/sched/sched_switch/enable
	# The sampler starts with the window and is waited for inside it, so it covers exactly the
	# interval mid_cpus is measured over and the window stays 5 s. kPFS only: no such file exists
	# under a BPF scheduler, and the -d test then simply skips it.
	rm -f /tmp/grp.sample; gs=
	m0=$(usage); n0=$(date +%s.%N); echo 1 > $TR/tracing_on
	[ -d $D ] && [ -f $MD/sample-groups.py ] && { python3 -I $MD/sample-groups.py /tmp/grp.sample ${GRPHZ:-100} 5 /rt/pod & gs=$!; }
	sleep 5
	[ -n "$gs" ] && wait $gs 2>/dev/null
	echo 0 > $TR/tracing_on; m1=$(usage); n1=$(date +%s.%N)
	echo 0 > $TR/events/sched/sched_switch/enable
	cat $CG/rt/pod/ctr/cgroup.threads > /tmp/pids
	cat $TR/trace > /tmp/sw.trace; echo > $TR/trace
	wait $bp
	t1=$(date +%s.%N); u1=$(usage); pstat > /tmp/s1
	k1=$(toksnap); x1=$(scxsnap)
	[ -n "${SCXS:-}" ] && sleep 1.1
	z1=$(tsnap); y1=$(tsnap pod/ctr); zt1=$(date +%s.%N)
	# Delivered CPU: a DELTA of the cumulative usage_usec over a MEASURED window, never the
	# counter over a nominal duration. Both windows are printed beside their rate below.
	cpus=$(awk -v a=$u0 -v b=$u1 -v s=$t0 -v e=$t1 'BEGIN { printf "%.3f", (b - a) / 1e6 / (e - s) }')
	cell_secs=$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.3f", b-a}')
	mid_secs=$(awk -v a=$n0 -v b=$n1 'BEGIN{printf "%.3f", b-a}')
	thr_win=$(awk -v a=$zt0 -v b=$zt1 'BEGIN{printf "%.3f", b-a}')
	tps=$(awk '/^tps/ {print int($3)}' /tmp/pgb.out); lat=$(awk '/latency average/ {print $4}' /tmp/pgb.out)
	# The kPFS counter deltas for this cell. The aggregate names are the original set; the
	# per-slot refusal counters (pfs.c: refuse_budget_frac / refuse_debt_guar / refuse_debt_frac,
	# buckets _0.._7 and _hi) are taken HERE, per cell, because that is the only thing that
	# attributes a refusal to a LIMIT: the one dump mech.sh takes at warm-up is a single limit's
	# and is not comparable with the scx per-cell numbers.
	# refuse_keys and refuse_nobase are published beside them so an UNINSTRUMENTED kernel, which
	# prints no refuse_* at all, can never read as a measured zero - and so a counter that appears
	# only in the second snapshot, whose "delta" would be its whole history, is named rather than
	# quietly believed.
	st=""; [ -s /tmp/s1 ] && st=$(awk '
		NR == FNR { a[$1] = $2; next }
		$1 ~ /^(admit|credit|park_full|park_budget|park_fifo|rotate|handoff|user_park|credit_ns|slot_ns|park_debt|wake_park)$/ { printf "%s=%d ", $1, $2 - a[$1] }
		$1 ~ /^refuse_[a-z_]+_([0-9]+|hi)$/ { if (!($1 in a)) miss++; printf "%s=%d ", $1, $2 - a[$1]; nr++ }
		END { printf "refuse_keys=%d refuse_nobase=%d ", nr + 0, miss + 0 }' /tmp/s0 /tmp/s1)
	echo "R v=$V lim=$lim wl=$wl rep=$rep tps=$tps lat_ms=$lat cpus=$cpus mid_cpus=$(awk -v a=$m0 -v b=$m1 -v s=$n0 -v e=$n1 'BEGIN { printf "%.3f", (b - a) / 1e6 / (e - s) }') cell_secs=$cell_secs mid_secs=$mid_secs cpu_src=/rt/pod conc_pids=$(wc -l < /tmp/pids) $(python3 -I $CONC /tmp/sw.trace /tmp/pids $lim) $(echo "$z0 $z1" | awk -v w=$thr_win '{printf "thr_win=%.3f thr_per=%d thr_n=%d thr_us=%d thr_frac=%.3f", w, $7-$1, $8-$2, $9-$3, ($9-$3)/1e6/w; if ($4 != "-") printf " pfs_per=%d pfs_n=%d pfs_us=%d pfs_frac=%.3f", $10-$4, $11-$5, $12-$6, ($12-$6)/1e6/w}') $(echo "$y0 $y1" | awk -v w=$thr_win '{printf "c_thr_win=%.3f c_thr_per=%d c_thr_n=%d c_thr_us=%d c_thr_frac=%.3f", w, $7-$1, $8-$2, $9-$3, ($9-$3)/1e6/w; if ($4 != "-") printf " c_pfs_per=%d c_pfs_n=%d c_pfs_us=%d c_pfs_frac=%.3f", $10-$4, $11-$5, $12-$6, ($12-$6)/1e6/w}') $st"
	# Transactions over the whole cell, to normalise the whole-cell counter deltas. pgbench -T
	# prints a bare count, -t prints "done/total"; the first field is what completed.
	txns=$(awk '/number of transactions actually processed/ {n=$NF; sub(/\/.*/, "", n); print n}' /tmp/pgb.out)
	[ -n "$txns" ] || txns=$(awk -v t=$tps -v a=$t0 -v b=$t1 'BEGIN {printf "%d", t * (b - a)}')
	echo "D cell lim=$lim wl=$wl cl=$CL secs=$cell_secs txns=$txns tokens_ns_before=$k0 tokens_ns_after=$k1"
	echo "D pgprog lim=$lim wl=$wl $(awk '/^progress:/ {printf "%s:%s ", $2, $4}' /tmp/pgb.out)"
	[ -n "$x0" ] && echo "D scx0 lim=$lim wl=$wl $x0"
	[ -n "$x1" ] && echo "D scx1 lim=$lim wl=$wl $x1"
	if [ -s /tmp/grp.sample ]; then
		echo "D grpsample lim=$lim wl=$wl $(tail -1 /tmp/grp.sample)"
		awk -v l=$lim -v w=$wl '/^S /{print "G lim=" l " wl=" w " " $2 " " $3 " " $4 " " $5 " " $6 " " $7 " " $8}' /tmp/grp.sample
	fi
	rm -f /tmp/sw.trace; : ${P5RES:-}/${TAG:-x}-$V-l$lim-$wl-r$rep.trace 2>/dev/null
done; done; done
pgload after 0
[ -d $D ] && cat $D/stats
asu $PGB/pg_ctl -D /tmp/pgd/data stop -m fast > /dev/null 2>&1
echo PG-DONE
