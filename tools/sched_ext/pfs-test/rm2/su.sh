#!/bin/bash
# su.sh KV PRE [WLS] : sub-unit limit suite inside the local VM (root), adapted from the cluster kit's
# subunit-guest.sh. Cgroup /sys/fs/cgroup/su (cpu.max LIMIT) / ctr (workload).
#   a: single Poisson requests of 1/5/20/50 ms CPU at ~50% of the limit (subreq), completion p50/p99/max
#   b: periodic bursts, 1 and 8 threads x 2-10 ms, every 100 ms and every 1 s (sufanout)
#   c: steady spin, 1 and 4 threads
# Per cell: CPUs over the run (cpu.stat and a sched_switch bpftrace), CPU rate over 100 ms and 1 s windows
# (cpu.stat sampled every 100 ms: max, p50, share of 100 ms windows above limit*1.05), cpu.pressure "some" delta
# ("held"), cpu.stat throttle deltas, and scx_pfs's -S counters (scx only).
# Local durations are shorter than the cluster kit's: requests clamp(150/rate, 20, 60) s, bursts 20 s / 40 s, spin 15 s.
KV=$1 PRE=$2 WLS=${3:-a,b,c}
D=$(cd "$(dirname "$0")" && pwd)
CG=/sys/fs/cgroup G=$CG/su SF=/tmp/pfs-stats-su
echo +cpu > $CG/cgroup.subtree_control
SP=
if [ "$KV" = scx ]; then
	$SCXB -i 1 -S $SF > /tmp/scx-su.log 2>&1 & SP=$!
	for i in $(seq 50); do [ "$(cat /sys/kernel/sched_ext/state)" = enabled ] && break; sleep 0.1; done
	echo "scx su: $(cat /sys/kernel/sched_ext/state)"
fi
mkg() {
	rmdir $G/ctr $G 2>/dev/null; mkdir -p $G; echo +cpu > $G/cgroup.subtree_control; mkdir $G/ctr
	echo "$(awk -v l=$1 'BEGIN{printf "%d", l*100000}') 100000" > $G/cpu.max
}
sampler() {
	local k v
	while :; do
		while read -r k v; do [ "$k" = usage_usec ] && { echo "${EPOCHREALTIME/./} $v"; break; }; done < $G/cpu.stat
		sleep 0.1
	done > "$1"
}
sst() { awk -v p="/su" '$1 == p {print $3, $5, $7; f=1} END {if (!f) print 0, 0, 0}' $SF 2>/dev/null || echo 0 0 0; }
cell() {
	local label=$1 lim=$2; shift 2
	mkg $lim
	[ -n "$SP" ] && sleep 1.1
	cp $G/cpu.stat /tmp/su.s0; cp $G/cpu.pressure /tmp/su.p0; local s0; s0=$(sst)
	sampler /tmp/su.samp & local smp=$!
	timeout -s INT 400 bpftrace $D/conc.bt $(stat -c %i $G) > /tmp/su.conc 2>&1 & local bp=$!
	local i; for i in $(seq 100); do grep -q Attaching /tmp/su.conc 2>/dev/null && break; sleep 0.1; done
	local tr0=$EPOCHREALTIME
	sleep 0.5
	local t0=$EPOCHREALTIME u0; u0=$(awk '/^usage_usec/{print $2}' $G/cpu.stat)
	local r; r=$(bash -c 'echo $$ > '$G/ctr/cgroup.procs'; exec "$@"' x "$@" 2>&1 | tail -1)
	local t1=$EPOCHREALTIME u1; u1=$(awk '/^usage_usec/{print $2}' $G/cpu.stat)
	kill $smp 2>/dev/null; wait $smp 2>/dev/null
	kill -INT $bp 2>/dev/null; wait $bp 2>/dev/null
	[ -n "$SP" ] && sleep 1.1
	cp $G/cpu.stat /tmp/su.s1; cp $G/cpu.pressure /tmp/su.p1; local s1; s1=$(sst)
	for p in $(cat $G/ctr/cgroup.procs); do kill -9 $p 2>/dev/null; done
	local secs; secs=$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.3f", b-a}')
	local cpus; cpus=$(awk -v a=$u0 -v b=$u1 -v s=$secs 'BEGIN{printf "%.4f", (b-a)/1e6/s}')
	local sw; sw=$(awk '/conc_traced_ns=/{split($0,x,"="); tr=x[2]; getline; if ($1=="@busy:") printf "%.4f", $2/1e9/s}' s=$secs /tmp/su.conc | head -1)
	awk -v a=$t0 -v b=$t1 '$1 >= a*1e6 && $1 <= b*1e6' /tmp/su.samp > /tmp/su.sampw; mv /tmp/su.sampw /tmp/su.samp
	awk '{t[NR]=$1; u[NR]=$2} END{for(i=2;i<=NR;i++){dt=(t[i]-t[i-1])/1e6; if(dt>0) printf "%.4f\n", (u[i]-u[i-1])/1e6/dt}}' /tmp/su.samp | sort -n > /tmp/su.w100
	awk '{t[NR]=$1; u[NR]=$2} END{for(i=11;i<=NR;i+=10){dt=(t[i]-t[i-10])/1e6; if(dt>0) printf "%.4f\n", (u[i]-u[i-10])/1e6/dt}}' /tmp/su.samp | sort -n > /tmp/su.w1s
	local win; win=$(awk -v L=$lim 'NR==FNR{a[++n]=$1; if($1>L*1.05)o++; next} {b[++k]=$1} END{
		printf "w100_max=%.3f w100_p50=%.3f w100_over=%.3f w1s_max=%.3f w1s_p50=%.3f", a[n], a[int(n/2)+1], n?o/n:0, b[k], b[int(k/2)+1]}' /tmp/su.w100 /tmp/su.w1s)
	# the same windows from the trace (cpu.stat over-reports under scx_pfs): T lines every 100 ms with cumulative busy ns
	local tw; tw=$(awk -v t0r=$tr0 -v a=$t0 -v b=$t1 '/^T /{t=$2; next} /^@busy:/ && t!="" {print t, $2; t=""}' /tmp/su.conc | awk -v off=$(awk -v x=$t0 -v y=$tr0 'BEGIN{printf "%.0f", (x-y)*1e9}') -v dur=$(awk -v x=$t0 -v y=$t1 'BEGIN{printf "%.0f", (y-x)*1e9}') '$1 >= off && $1 <= off+dur {t[++n]=$1; u[n]=$2} END{for(i=2;i<=n;i++) if(t[i]>t[i-1]) print "a", (u[i]-u[i-1])/(t[i]-t[i-1]); for(i=11;i<=n;i+=10) if(t[i]>t[i-10]) print "b", (u[i]-u[i-10])/(t[i]-t[i-10])}' | awk -v L=$lim '$1=="a"{n++; if($2>a)a=$2; if($2>L*1.05)o++} $1=="b"{if($2>b)b=$2} END{printf "tw100_max=%.3f tw100_over=%.3f tw1s_max=%.3f", a, n?o/n:0, b}')
	local thr; thr=$(awk 'NR==FNR{a[$1]=$2; next} ($1 in a) && $1 ~ /^(nr_periods|nr_throttled|throttled_usec|nr_bursts|burst_usec)$/ {printf "d_%s=%d ", $1, $2-a[$1]}' /tmp/su.s0 /tmp/su.s1)
	local held; held=$(awk 'NR==FNR{if($1=="some"){split($5,x,"=");a=x[2]} next} $1=="some"{split($5,x,"="); printf "%.3f", (x[2]-a)/1e6}' /tmp/su.p0 /tmp/su.p1)
	local pfs=""; [ -n "$SP" ] && pfs=$(echo "$s0 $s1" | awk '{printf "pfs_nr_periods=%d pfs_nr_throttled=%d pfs_throttled_usec=%d", $4-$1, $5-$2, $6-$3}')
	echo "$PRE test=su wl=$label limit=$lim secs=$secs cpus=$cpus sw_cpus=${sw:-na} $win $tw held_s=$held held_frac=$(awk -v h=$held -v s=$secs 'BEGIN{printf "%.3f", h/s}') $thr$pfs | $r"
	rmdir $G/ctr $G 2>/dev/null; sleep 0.5
}
LIMS=${LIMS:-"0.05 0.1 0.2 0.25 0.5 0.75 1.25 1.5"}
for lim in $LIMS; do
	case ",$WLS," in *,a,*)
	for w in 1 5 20 50; do
		rate=$(awk -v l=$lim -v w=$w 'BEGIN{printf "%.4f", 0.5*l*1000/w}')
		secs=$(awk -v r=$rate 'BEGIN{s=150/r; if(s<20)s=20; if(s>60)s=60; printf "%d", s}')
		cell "req-${w}ms" $lim $D/subreq $w $rate $secs 4
	done ;; esac
	case ",$WLS," in *,b,*)
	for th in 1 8; do for per in 100 1000; do
		s=20; [ $per = 1000 ] && s=40
		cell "burst-t${th}-p${per}" $lim $D/sufanout $th $per $s 2000 10000
	done; done ;; esac
	case ",$WLS," in *,c,*)
	for th in 1 4; do cell "spin-t$th" $lim $KIT/bin/spin $th 15; done ;; esac
done
if [ -n "$SP" ]; then
	kill -INT $SP; wait $SP
	grep -iE 'error|stall|exit' /tmp/scx-su.log | grep -v libbpf | head -3
	cp /tmp/scx-su.log $R/res/scxlog-su-$V-r$RND.log 2>/dev/null
fi
rmdir $G/ctr $G 2>/dev/null
echo SU-DONE
