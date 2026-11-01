#!/bin/bash
# guest.sh VARIANT ROUND PHASES : four-way retest, runs as root inside the VM.
# VARIANT cfs|kwidth|scx|kpfs (kpfs needs a sched_pfs=1 boot).
# PHASES comma list: mx nn lat ulat wt st lk unl kit dock
# Every result line starts with "R " and carries variant/round/test.
R=$(cd "$(dirname "$0")" && pwd)
V=$1 RND=$2 PH=$3
T=$R/t KIT=$R/kit TREE=${TREE:-$R/tree-kit}
PERF=$(ls /usr/lib/linux-tools/*/perf | tail -1)
SCXB=$R/kit/bin/scx_pfs
CG=/sys/fs/cgroup KW=/proc/sys/kernel/sched_cfs_bandwidth_width
export SCX=$SCXB
pre="R v=$V r=$RND"
echo "kernel $(uname -r) cmdline=$(cat /proc/cmdline | tr ' ' '\n' | grep -E 'sched_pfs' ) cpus=$(nproc) variant=$V round=$RND phases=$PH"
case $V in kpfs) grep -qw sched_pfs=1 /proc/cmdline || { echo "ERROR kpfs without sched_pfs=1"; exit 1; } ;;
*) ! grep -qw sched_pfs=1 /proc/cmdline || { echo "ERROR $V on a sched_pfs=1 boot"; exit 1; } ;; esac
echo +cpu > $CG/cgroup.subtree_control
mountpoint -q /sys/kernel/tracing || mount -t tracefs nodev /sys/kernel/tracing

SP=
scx_on() { # load scx_pfs for phases that run their own workloads
	[ $V = scx ] || return 0
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
[ $V = kwidth ] && kw 1 || kw 0

# two-level pod/ctr cgroup limited to $1 CPUs, like a kubelet pod
pod() {
	rmdir $CG/rt/pod/ctr $CG/rt/pod 2>/dev/null
	mkdir -p $CG/rt; echo +cpu > $CG/rt/cgroup.subtree_control
	mkdir -p $CG/rt/pod; echo +cpu > $CG/rt/pod/cgroup.subtree_control; mkdir -p $CG/rt/pod/ctr
	local q; q=$(awk -v l=$1 'BEGIN { printf "%d", l * 100000 }')
	echo "$q 100000" > $CG/rt/pod/cpu.max; echo "$q 100000" > $CG/rt/pod/ctr/cpu.max
}
# inpod NAME LIMIT CMD... : run CMD in the pod, report CPUs used (cpu.stat / wall)
inpod() {
	local name=$1 lim=$2; shift 2
	pod $lim
	local u0 u1 t0 t1 out
	u0=$(awk '/usage_usec/ {print $2}' $CG/rt/pod/cpu.stat); t0=$(date +%s.%N)
	out=$(bash -c 'echo $$ > '$CG/rt/pod/ctr/cgroup.procs'; exec timeout 60 "$@"' x "$@" 2>&1 | tail -1)
	t1=$(date +%s.%N); u1=$(awk '/usage_usec/ {print $2}' $CG/rt/pod/cpu.stat)
	echo "$pre test=$name limit=$lim cpus=$(awk -v a=$u0 -v b=$u1 -v s=$t0 -v e=$t1 'BEGIN { printf "%.2f", (b - a) / 1e6 / (e - s) }') $out"
}

for ph in ${PH//,/ }; do
	echo "== phase $ph $(date +%T)"
	case $ph in
	mx) # limited matrix, throughput / CPUs (trace) / worst stall
		for l in 0.5 1 2 4; do for t in 1 8 64; do
			echo "$pre test=mx csv=$($KIT/cg-run.sh -s 5 $V $l $t 2>/dev/null | tail -1)"
		done; done ;;
	nn) # noisy neighbour: limit 2 next to one unlimited burner per CPU
		for t in 1 8 64; do
			echo "$pre test=nn csv=$($KIT/cg-run.sh -s 5 -n noisy $V 2 $t 2>/dev/null | tail -1)"
		done ;;
	lat) # limited request latency, 0 and 8 in-pod burners
		for l in 1 2; do for b in 0 8; do
			echo "$pre test=latlim b=$b csv=$($KIT/cg-run.sh -s 5 -w reqlat $V $l $b 2>/dev/null | tail -1)"
		done; done ;;
	ulat) # unlimited request / wakeup latency
		scx_on ulat
		echo "$pre test=reqlat-b0 $($KIT/bin/reqlat 4 1000 20000 3 0)"
		echo "$pre test=reqlat-b8 $($KIT/bin/reqlat 4 1000 20000 3 8)"
		echo "$pre test=wlat-idle $($T/wlat 8 10000 200 2 0)"
		echo "$pre test=wlat-b8 $($T/wlat 8 10000 200 3 8)"
		echo "$pre test=wlat-busy16 $($T/wlat 16 5000 500 3 4)"
		scx_off ulat ;;
	wt) # weights: requests 1:3; limits 4/6 with requests 1:3
		m=$V; [ $V = scx ] && m=width
		for k in req lim; do
			echo "$pre test=wt-$k $(SCX=scx_pfs bash $T/weights.sh $m $k 8 2>&1 | grep WRES)"
		done ;;
	st) # starvation: weight-1 thread next to 16 weight-5000 spinners
		m=$V; [ $V = scx ] && m=width
		o=$(SCX=scx_pfs bash $T/starve.sh $m 20 2>&1)
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
	unl) # tasks without limits
		scx_on unl
		for g in 4 8 16; do
			echo "$pre test=hb-threads-g$g time=$($PERF bench sched messaging -g $g -l 400 2>&1 | awk '/Total time/ {print $3}')"
			echo "$pre test=hb-procs-g$g time=$($PERF bench sched messaging -p -g $g -l 400 2>&1 | awk '/Total time/ {print $3}')"
		done
		echo "$pre test=pipe time=$($PERF bench sched pipe -l 100000 2>&1 | awk '/Total time/ {print $3}')"
		echo "$pre test=barrier6 $($T/lockprobe barrier 6 2)"
		for t in 4 8; do
			o=$($T/schbench -m 2 -t $t -r 5 -i 100 2>&1)
			wl=$(echo "$o" | awk '/Wakeup Latencies/ {f=1} f && /50.0th/ {a=$2} f && /99.0th/ {b=$3; print a, b; exit}')
			rl=$(echo "$o" | awk '/Request Latencies/ {f=1} f && /50.0th/ {a=$2} f && /99.0th/ {b=$3; print a, b; exit}')
			echo "$pre test=schbench-t$t wake_p50_us=${wl% *} wake_p99_us=${wl#* } req_p50_us=${rl% *} req_p99_us=${rl#* } rps=$(echo "$o" | awk '/average rps/ {print $3}')"
		done
		scx_off unl ;;
	kit) # in-tree kit, cgroup path, key cells
		kv=$V; [ $V = kpfs ] && kv=cfs
		o=$R/res/kit-cg-$V-$RND; rm -rf $o
		$TREE/compare.sh -p cgroup -v $kv -C 1:8,1.5:8,2:64 -n none,noisy -L 0.5 -s 5 -r 1 -o $o > /dev/null 2>&1
		echo "$pre test=kit-cg rc=$? rows=$(($(wc -l < $o/results.csv) - 1))" ;;
	dock) # in-tree kit, Docker path (private dockerd on tmpfs)
		kv=$V; [ $V = kpfs ] && kv=cfs
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
