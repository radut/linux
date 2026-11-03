#!/bin/bash
# guest.sh VARIANT ROUND PHASES : remeasure (post-review builds), runs as root inside the VM.
# VARIANT cfs|kwidth|scx|kpfs, plus the pre-fix references scxold (old scx_pfs binary)
# and kpfsold (phase-5 kPFS image). kpfs/kpfsold need a sched_pfs=1 boot.
# PHASES comma list: mx nn lat ulat wt st lk unl kit dock kh pg base
# Every result line starts with "R " and carries variant/round/test.
R=$(cd "$(dirname "$0")" && pwd)
V=$1 RND=$2 PH=$3
T=$R/t KIT=$R/kit TREE=${TREE:-$R/tree-kit}
# conc-pids.py is the in-tree name of the reducer this file used to call $T/conc5.py: same
# code, same argv (TRACE PIDFILE LIMIT), same output fields in the same order.
CONC=${CONC:-$R/conc-pids.py}
PERF=$(ls /usr/lib/linux-tools/*/perf | tail -1)
KV=$V; SCXN=scx_pfs.new
case $V in scxold) KV=scx; SCXN=scx_pfs.old ;; kpfs*) KV=kpfs ;; esac
SCXB=$R/kit/bin/$SCXN
CG=/sys/fs/cgroup KW=/proc/sys/kernel/sched_cfs_bandwidth_width
export SCX=$SCXB
pre="R v=$V r=$RND"
CW=; [ "${CORE:-0}" = 1 ] && CW=$T/corewrap
export WT_CW=$CW
echo "kernel $(uname -r) $(cat /proc/version | cut -d' ' -f4-) cmdline=$(cat /proc/cmdline | tr ' ' '\n' | grep -E 'sched_pfs' ) cpus=$(nproc) variant=$V kv=$KV scx=$SCXN round=$RND phases=$PH"
case $KV in kpfs) grep -qw sched_pfs=1 /proc/cmdline || { echo "ERROR kpfs without sched_pfs=1"; exit 1; } ;;
*) ! grep -qw sched_pfs=1 /proc/cmdline || { echo "ERROR $V on a sched_pfs=1 boot"; exit 1; } ;; esac
echo +cpu > $CG/cgroup.subtree_control
mountpoint -q /sys/kernel/tracing || mount -t tracefs nodev /sys/kernel/tracing
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
SP=
scx_on() { # load scx_pfs for phases that run their own workloads
	[ $KV = scx ] || return 0
	$SCXB -i 3600 > /tmp/scx-$1.log 2>&1 &
	SP=$!
	for i in $(seq 50); do [ "$(cat /sys/kernel/sched_ext/state)" = enabled ] && break; sleep 0.1; done
	echo "scx $1: $(cat /sys/kernel/sched_ext/state)"
}
scx_off() {
	[ -n "$SP" ] || return 0
	echo "scx $1 before unload: $(cat /sys/kernel/sched_ext/state)"
	kill -INT $SP; wait $SP
	grep -iE 'error|stall|exit' /tmp/scx-$1.log | grep -v libbpf | head -3
	SP=
}
kw() { [ -e $KW ] && echo $1 > $KW; }
[ $KV = kwidth ] && kw 1 || kw 0

# two-level pod/ctr cgroup limited to $1 CPUs, like a kubelet pod
pod() {
	rmdir $CG/rt/pod/ctr $CG/rt/pod 2>/dev/null
	mkdir -p $CG/rt; echo +cpu > $CG/rt/cgroup.subtree_control
	mkdir -p $CG/rt/pod; echo +cpu > $CG/rt/pod/cgroup.subtree_control; mkdir -p $CG/rt/pod/ctr
	local q; q=$(awk -v l=$1 'BEGIN { printf "%d", l * 100000 }')
	echo "$q 100000" > $CG/rt/pod/cpu.max; echo "$q 100000" > $CG/rt/pod/ctr/cpu.max
	if [ "${WT_PIN:-0}" = 1 ]; then
		echo +cpuset > $CG/cgroup.subtree_control; echo +cpuset > $CG/rt/cgroup.subtree_control
		echo "0-$(awk -v l=$1 'BEGIN { printf "%d", l - 1 }')" > $CG/rt/pod/cpuset.cpus
		echo "max 100000" > $CG/rt/pod/cpu.max; echo "max 100000" > $CG/rt/pod/ctr/cpu.max
	elif [ "${WT_BURST:-0}" = 1 ]; then
		echo $q > $CG/rt/pod/cpu.max.burst; echo $q > $CG/rt/pod/ctr/cpu.max.burst
	fi
}
# pfs stats delta (kPFS only)
PD=/sys/kernel/debug/sched/pfs/stats
pst() { [ -r $PD ] && cat $PD; }
pdelta() { [ -s $1 ] && [ -s $2 ] && awk 'NR==FNR {a[$1]=$2; next} $1 ~ /^(credit|credit_ns|slot_ns|park_debt|park_full|park_budget|user_park|rotate)$/ {printf "%s=%d ", $1, $2-a[$1]}' $1 $2; }
# kh NAME LIMIT CMD: CPUs from cpu.stat (10 s after 3 s warm-up) + trace concurrency (5 s)
kh() {
	local name=$1 lim=$2; shift 2
	pod $lim
	pst > /tmp/k0
	bash -c 'echo $$ > '$CG/rt/pod/ctr/cgroup.procs'; exec $0 "$@"' "${CW:-env}" "$@" > /dev/null 2>&1 &
	local bp=$! TR=/sys/kernel/tracing
	sleep 3
	local u0 u1 t0 t1 n0
	u0=$(awk '/usage_usec/ {print $2}' $CG/rt/pod/cpu.stat); n0=$(awk '/nr_throttled/ {print $2}' $CG/rt/pod/cpu.stat); t0=$(date +%s.%N)
	sleep 2
	echo 0 > $TR/tracing_on; echo mono > $TR/trace_clock; echo 32768 > $TR/buffer_size_kb; echo > $TR/trace
	echo 1 > $TR/events/sched/sched_switch/enable; echo 1 > $TR/tracing_on
	sleep 5
	echo 0 > $TR/tracing_on; echo 0 > $TR/events/sched/sched_switch/enable
	cat $CG/rt/pod/ctr/cgroup.threads > /tmp/khpids
	sleep 3
	t1=$(date +%s.%N); u1=$(awk '/usage_usec/ {print $2}' $CG/rt/pod/cpu.stat)
	local n1; n1=$(awk '/nr_throttled/ {print $2}' $CG/rt/pod/cpu.stat)
	cat $TR/trace > /tmp/kh.trace; echo > $TR/trace
	pst > /tmp/k1
	for p in $(cat $CG/rt/pod/ctr/cgroup.procs); do kill -9 $p 2>/dev/null; done
	kill -9 $bp 2>/dev/null; wait $bp 2>/dev/null
	echo "$pre test=kh-$name limit=$lim cpus=$(awk -v a=$u0 -v b=$u1 -v s=$t0 -v e=$t1 'BEGIN { printf "%.3f", (b - a) / 1e6 / (e - s) }') nr_thr=$((n1 - n0)) $(python3 -I $CONC /tmp/kh.trace /tmp/khpids $lim | cut -d' ' -f1-4) $(pdelta /tmp/k0 /tmp/k1)"
	sleep 1
}

# nest A B: parent rt/par cpu.max 4, children a (A) and b (B); MODE spin: 8 spinners each; MODE lat: a runs
# reqlat (4 request threads + 4 burners), b 8 spinners. CPUs per child (cpu.stat and trace), stalls, latency.
nest() {
	local A=$1 B=$2 mode=$3 TR=/sys/kernel/tracing P=$CG/rt/par
	rmdir $P/a $P/b $P 2>/dev/null; mkdir -p $CG/rt; echo +cpu > $CG/rt/cgroup.subtree_control
	mkdir -p $P; echo "400000 100000" > $P/cpu.max; echo +cpu > $P/cgroup.subtree_control
	mkdir -p $P/a $P/b
	echo "$(awk -v l=$A 'BEGIN { printf "%d", l * 100000 }') 100000" > $P/a/cpu.max
	echo "$(awk -v l=$B 'BEGIN { printf "%d", l * 100000 }') 100000" > $P/b/cpu.max
	local ua0 ub0 up0 t0
	ua0=$(awk '/usage_usec/ {print $2}' $P/a/cpu.stat); ub0=$(awk '/usage_usec/ {print $2}' $P/b/cpu.stat); up0=$(awk '/usage_usec/ {print $2}' $P/cpu.stat); t0=$(date +%s.%N)
	if [ $mode = lat ]; then
		bash -c 'echo $$ > '$P/a/cgroup.procs'; exec $0 "$@"' "${CW:-env}" $KIT/bin/reqlat 4 1000 20000 8 4 > /tmp/na.out 2>&1 &
	else
		bash -c 'echo $$ > '$P/a/cgroup.procs'; exec $0 "$@"' "${CW:-env}" $KIT/bin/spin 8 8 > /tmp/na.out 2>&1 &
	fi
	local pa=$!
	bash -c 'echo $$ > '$P/b/cgroup.procs'; exec $0 "$@"' "${CW:-env}" $KIT/bin/spin 8 8 > /tmp/nb.out 2>&1 &
	local pb=$!
	sleep 2
	echo 0 > $TR/tracing_on; echo mono > $TR/trace_clock; echo 32768 > $TR/buffer_size_kb; echo > $TR/trace
	echo 1 > $TR/events/sched/sched_switch/enable; echo 1 > $TR/tracing_on
	sleep 4
	echo 0 > $TR/tracing_on; echo 0 > $TR/events/sched/sched_switch/enable
	cat $P/a/cgroup.threads > /tmp/npa; cat $P/b/cgroup.threads > /tmp/npb; cat /tmp/npa /tmp/npb > /tmp/npp
	cat $TR/trace > /tmp/n.trace; echo > $TR/trace
	wait $pa $pb
	local t1 ua1 ub1 up1; t1=$(date +%s.%N)
	ua1=$(awk '/usage_usec/ {print $2}' $P/a/cpu.stat); ub1=$(awk '/usage_usec/ {print $2}' $P/b/cpu.stat); up1=$(awk '/usage_usec/ {print $2}' $P/cpu.stat)
	c() { awk -v a=$1 -v b=$2 -v s=$t0 -v e=$t1 'BEGIN { printf "%.3f", (b - a) / 1e6 / (e - s) }'; }
	tc() { python3 -I $CONC /tmp/n.trace $1 $2 | sed -n 's/.*mean_running=\([0-9.]*\).*/\1/p'; }
	echo "$pre test=nest-$mode cell=$A+$B par_cpus=$(c $up0 $up1) a_cpus=$(c $ua0 $ua1) b_cpus=$(c $ub0 $ub1) par_tr=$(tc /tmp/npp 4) a_tr=$(tc /tmp/npa $A) b_tr=$(tc /tmp/npb $B) a: $(tail -1 /tmp/na.out) | b: $(tail -1 /tmp/nb.out)"
	rmdir $P/a $P/b $P 2>/dev/null
	sleep 1
}
urand() { for i in $(seq 16); do dd if=/dev/urandom of=/dev/null bs=32M count=100000 2>/dev/null & done; wait; }
kpipe() { for i in $(seq 8); do (dd if=/dev/zero bs=16M count=100000 2>/dev/null | dd of=/dev/null bs=16M 2>/dev/null) & done; wait; }
export -f urand kpipe
# inpod NAME LIMIT CMD... : run CMD in the pod, report CPUs used (cpu.stat / wall)
inpod() {
	local name=$1 lim=$2; shift 2
	pod $lim
	local u0 u1 t0 t1 out
	u0=$(awk '/usage_usec/ {print $2}' $CG/rt/pod/cpu.stat); t0=$(date +%s.%N)
	out=$(bash -c 'echo $$ > '$CG/rt/pod/ctr/cgroup.procs'; exec timeout 60 $0 "$@"' "${CW:-env}" "$@" 2>&1 | tail -1)
	t1=$(date +%s.%N); u1=$(awk '/usage_usec/ {print $2}' $CG/rt/pod/cpu.stat)
	echo "$pre test=$name limit=$lim cpus=$(awk -v a=$u0 -v b=$u1 -v s=$t0 -v e=$t1 'BEGIN { printf "%.2f", (b - a) / 1e6 / (e - s) }') $out"
}

for ph in ${PH//,/ }; do
	echo "== phase $ph $(date +%T)"
	case $ph in
	mx) # limited matrix, throughput / CPUs (trace) / worst stall
		for l in 0.5 1 2 4; do for t in 1 8 64; do
			echo "$pre test=mx csv=$($KIT/cg-run.sh -s 5 $KV $l $t 2>/dev/null | tail -1)"
		done; done ;;
	nn) # noisy neighbour: limit 2 next to one unlimited burner per CPU
		for t in 1 8 64; do
			echo "$pre test=nn csv=$($KIT/cg-run.sh -s 5 -n noisy $KV 2 $t 2>/dev/null | tail -1)"
		done ;;
	lat) # limited request latency, 0 and 8 in-pod burners
		for l in 0.5 1 2; do for b in 0 8; do
			echo "$pre test=latlim b=$b csv=$($KIT/cg-run.sh -s 5 -w reqlat $KV $l $b 2>/dev/null | tail -1)"
		done; done ;;
	ulat) # unlimited request / wakeup latency
		scx_on ulat
		echo "$pre test=reqlat-b0 $($CW $KIT/bin/reqlat 4 1000 20000 3 0)"
		echo "$pre test=reqlat-b8 $($CW $KIT/bin/reqlat 4 1000 20000 3 8)"
		echo "$pre test=wlat-idle $($CW $T/wlat 8 10000 200 2 0)"
		echo "$pre test=wlat-b8 $($CW $T/wlat 8 10000 200 3 8)"
		echo "$pre test=wlat-busy16 $($CW $T/wlat 16 5000 500 3 4)"
		scx_off ulat ;;
	wt) # weights: requests 1:3; limits 4/6 with requests 1:3
		m=$KV; [ $KV = scx ] && m=width
		for k in req lim; do
			echo "$pre test=wt-$k $(SCX=$SCXN bash $T/weights.sh $m $k 8 2>&1 | grep WRES)"
		done ;;
	st) # starvation: weight-1 thread next to 16 weight-5000 spinners
		m=$KV; [ $KV = scx ] && m=width
		o=$(SCX=$SCXN bash $T/starve.sh $m 20 2>&1)
		echo "$pre test=starve $(echo "$o" | grep besteffort:) state=$(echo "$o" | sed -n 's/^state: //p')" ;;
	lk) # lock probes in a limited pod
		scx_on lk
		inpod ly16-4-1 4 $T/probe lockyield 16 4 1 5
		inpod ly16-4-100 4 $T/probe lockyield 16 4 100 5
		inpod ly16-4-100 2 $T/probe lockyield 16 4 100 5
		inpod flag16 4 $T/lockprobe flag 16 5
		inpod flag16 2 $T/lockprobe flag 16 5
		inpod gc 4 $T/probe gc 8 4 5 20 100
		inpod gc 2 $T/probe gc 8 2 5 10 100
		scx_off lk ;;
	burst) # short parallel bursts: a request fans out to 8 threads of 1-5 ms CPU, every 100 / 20 ms
		scx_on burst
		for l in 2 4; do for p in 100 20; do inpod fan-p$p $l $T/fanout $p 10; done; done
		scx_off burst ;;
	nest) # nested limits: parent 4, children 2+2 and 3+3, all busy; latency in child a
		scx_on nest
		for ab in "2 2" "3 3"; do nest $ab spin; nest $ab lat; done
		scx_off nest ;;
	lim) # limit checks at 1.5 / 2 / 4: 16 spinners, /dev/urandom readers, pipe writers
		scx_on lim
		for l in 1.5 2 4; do kh spin $l $KIT/bin/spin 16 30; kh urand $l bash -c urand; kh pipe $l bash -c kpipe; done
		scx_off lim ;;
	kh) # kernel-heavy limit checks: /dev/urandom readers and pipe writers at limits 1.5 and 4
		scx_on kh
		for l in 1.5 4; do kh urand $l bash -c urand; kh pipe $l bash -c kpipe; done
		scx_off kh ;;
	pg) # Postgres pod at limits 1.5 and 2 (phase-5 case), pgbench from the root cgroup
		scx_on pg
		mkdir -p $R/res/pg
		TAG=r$RND-$V P5RES=$R/res/pg PGV=$V bash $T/pg-guest.sh "1.5 2" "tpcb select" 30 2>&1 | grep -E '^R |failed|PG-DONE' | sed "s/^R v=[a-z]*/$pre test=pg/"
		scx_off pg ;;
	base) # CFS baselines: cpuset-pinned to LIMIT CPUs (no cpu.max) and cpu.max.burst = quota
		[ $V = cfs ] || continue
		for b in pin burst; do
			if [ $b = pin ]; then export WT_PIN=1 WT_BURST=0; else export WT_PIN=0 WT_BURST=1; fi
			for l in 1 2 4; do for t in 8 64; do
				echo "$pre test=mx base=$b csv=$($KIT/cg-run.sh -s 5 cfs $l $t 2>/dev/null | tail -1)"
			done; done
			for t in 1 8 64; do
				echo "$pre test=nn base=$b csv=$($KIT/cg-run.sh -s 5 -n noisy cfs 2 $t 2>/dev/null | tail -1)"
			done
			for l in 1 2; do for b2 in 0 8; do
				echo "$pre test=latlim base=$b b=$b2 csv=$($KIT/cg-run.sh -s 5 -w reqlat cfs $l $b2 2>/dev/null | tail -1)"
			done; done
			pre0=$pre; pre="$pre0 base=$b"
			inpod ly16-4-100 4 $T/probe lockyield 16 4 100 5
			inpod ly16-4-100 2 $T/probe lockyield 16 4 100 5
			inpod flag16 4 $T/lockprobe flag 16 5
			inpod flag16 2 $T/lockprobe flag 16 5
			inpod gc 2 $T/probe gc 8 2 5 10 100
			for l in 2 4; do for p in 100 20; do inpod fan-p$p $l $T/fanout $p 10; done; done
			[ $b = burst ] && { kh urand 1.5 bash -c urand; kh pipe 1.5 bash -c kpipe; }
			kh urand 4 bash -c urand; kh pipe 4 bash -c kpipe
			pre=$pre0
		done
		unset WT_PIN WT_BURST
		rmdir $CG/rt/pod/ctr $CG/rt/pod 2>/dev/null ;;
	unl) # tasks without limits
		scx_on unl
		for g in 4 8 16; do
			echo "$pre test=hb-threads-g$g time=$($CW $PERF bench sched messaging -g $g -l 400 2>&1 | awk '/Total time/ {print $3}')"
			echo "$pre test=hb-procs-g$g time=$($CW $PERF bench sched messaging -p -g $g -l 400 2>&1 | awk '/Total time/ {print $3}')"
		done
		echo "$pre test=pipe time=$($CW $PERF bench sched pipe -l 100000 2>&1 | awk '/Total time/ {print $3}')"
		echo "$pre test=barrier6 $($CW $T/lockprobe barrier 6 2)"
		for t in 4 8; do
			o=$($CW $T/schbench -m 2 -t $t -r 5 -i 100 2>&1)
			wl=$(echo "$o" | awk '/Wakeup Latencies/ {f=1} f && /50.0th/ {a=$2} f && /99.0th/ {b=$3; print a, b; exit}')
			rl=$(echo "$o" | awk '/Request Latencies/ {f=1} f && /50.0th/ {a=$2} f && /99.0th/ {b=$3; print a, b; exit}')
			echo "$pre test=schbench-t$t wake_p50_us=${wl% *} wake_p99_us=${wl#* } req_p50_us=${rl% *} req_p99_us=${rl#* } rps=$(echo "$o" | awk '/average rps/ {print $3}')"
		done
		scx_off unl ;;
	kit) # in-tree kit, cgroup path, key cells
		kv=$KV; [ $KV = kpfs ] && kv=cfs
		o=$R/res/kit-cg-$V-$RND; rm -rf $o
		$TREE/compare.sh -p cgroup -v $kv -C 1:8,1.5:8,2:64 -n none,noisy -L 0.5 -s 5 -r 1 -o $o > /dev/null 2>&1
		echo "$pre test=kit-cg rc=$? rows=$(($(wc -l < $o/results.csv) - 1))" ;;
	dock) # in-tree kit, Docker path (private dockerd on tmpfs)
		kv=$KV; [ $KV = kpfs ] && kv=cfs
		D=/tmp/wtd; mkdir -p $D; mountpoint -q $D || mount -t tmpfs tmpfs $D
		echo '{}' > $D/daemon.json
		dockerd --data-root $D/data --exec-root $D/exec --pidfile $D/pid -H unix://$D/docker.sock \
			--bridge=none --iptables=false --ip6tables=false --config-file $D/daemon.json > $D/dockerd.log 2>&1 &
		export DOCKER_HOST=unix://$D/docker.sock
		for i in $(seq 60); do docker info > /dev/null 2>&1 && break; sleep 0.5; done
		echo "docker: $(docker info --format '{{.ServerVersion}} {{.CgroupDriver}} {{.CgroupVersion}}' 2>&1)"
		$TREE/docker-image.sh > /dev/null 2>&1 || echo "docker-image failed"
		o=$R/res/kit-dock-$V-$RND; rm -rf $o
		$TREE/compare.sh -p docker -v $kv -C 1:8,1.5:8,2:64 -n none,noisy -L 0.5 -s 5 -r 1 -o $o > /dev/null 2>&1
		echo "$pre test=kit-dock rc=$? rows=$(($(wc -l < $o/results.csv) - 1)) leftover=$(docker ps -aq | wc -l)"
		kill $(cat $D/pid) 2>/dev/null; sleep 2; unset DOCKER_HOST ;;
	*) echo "unknown phase $ph" ;;
	esac
done
[ -e /sys/kernel/sched_ext/state ] && echo "scx final state $(cat /sys/kernel/sched_ext/state)"
dmesg | grep -E "WARNING|BUG|lockdep|INFO: task|rcu.*stall|sched_ext.*(error|stall)|runnable task stall" | head -10
echo GUEST-DONE