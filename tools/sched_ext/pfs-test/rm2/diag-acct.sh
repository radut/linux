#!/bin/bash
# diag-acct.sh: under scx_pfs (round 2), compare a limited cgroup's cpu.stat usage_usec with the tasks' own
# sum_exec_runtime (/proc/PID/task/TID/schedstat), the sched_switch trace and the work done (spin iterations).
CG=/sys/fs/cgroup G=$CG/acct K=$M/kit/bin
echo +cpu > $CG/cgroup.subtree_control
for mode in cfs scx; do
	SP=
	if [ $mode = scx ]; then $M/kit/bin/scx_pfs.new -i 1 > /tmp/scx-acct.log 2>&1 & SP=$!; sleep 2; echo "scx: $(cat /sys/kernel/sched_ext/state)"; fi
	for lim in 0.1 0.5; do for th in 1 4; do
		rmdir $G 2>/dev/null; mkdir $G; echo "$(awk -v l=$lim 'BEGIN{printf "%d", l*100000}') 100000" > $G/cpu.max
		timeout -s INT 100 bpftrace $M/su/conc2.bt $(stat -c %i $G) > /tmp/acct.conc 2>&1 & bp=$!
		for i in $(seq 100); do grep -q Attaching /tmp/acct.conc && break; sleep 0.1; done
		u0=$(awk '/^usage_usec/{print $2}' $G/cpu.stat); t0=$EPOCHREALTIME
		bash -c 'echo $$ > '$G'/cgroup.procs; exec '$K'/spin '$th' 10' > /tmp/acct.out &
		sp=$!; sleep 9.5
		se=0; for f in /proc/$sp/task/*/schedstat; do se=$((se + $(cut -d' ' -f1 $f))); done
		u1=$(awk '/^usage_usec/{print $2}' $G/cpu.stat); t1=$EPOCHREALTIME
		wait $sp; kill -INT $bp; wait $bp
		busy=$(awk '/^@busy:/{b=$2} END{print b+0}' /tmp/acct.conc)
		w=$(awk -v a=$t0 -v b=$t1 'BEGIN{print b-a}')
		echo "ACCT mode=$mode limit=$lim threads=$th cpustat_cpus=$(awk -v a=$u0 -v b=$u1 -v w=$w 'BEGIN{printf "%.3f", (b-a)/1e6/w}') schedstat_cpus=$(awk -v s=$se -v w=$w 'BEGIN{printf "%.3f", s/1e9/w}') trace_cpus=$(awk -v s=$busy 'BEGIN{printf "%.3f", s/1e9/10}') $(cat /tmp/acct.out)"
		grep -E '^(user|system)_usec' $G/cpu.stat | tr '\n' ' '; echo
	done; done
	[ -n "$SP" ] && { kill -INT $SP; wait $SP; }
done
rmdir $G
echo ACCT-DONE
