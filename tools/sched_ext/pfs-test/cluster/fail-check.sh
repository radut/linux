#!/usr/bin/env bash
# fail-check.sh KIND: short failure checks under load (Postgres limit 4, TPC-B c32) on node B, under nodeB.lock.
#   kwflip    (kwidth kernel) flip kernel.sched_cfs_bandwidth_width 1/0 every 15 s (+ cpu.max rewrite) during the run
#   scxkill   (7.0 or 7.3) scx_pfs DaemonSet: SIGKILL the scheduler at ~30 s, delete its pod at ~75 s
#   kpfschurn (kpfs) pod churn during a 300 s run: 10 limited pods per cycle (spinners + exec loops), create, Ready, delete
source "$(dirname "$0")/lib.sh"; source "$KIT/pg.sh"
kind=$1
OUTB=$S/results3/retest/fail; mkdir -p "$OUTB"
preflight
trap 'k delete pod -l retest/role=churn --ignore-not-found --wait=true >/dev/null 2>&1 || true; cleanup_node_b' EXIT
case $kind in
kwflip) mode=kwidth ;;
scxkill) mode=${SCXMODE:-scx} ;;
kpfschurn) mode=kpfs; PG_SECS=300 ;;
churn2) mode=${CHMODE:-kpfs}; PG_SECS=600 ;;
scxroll) mode=${SCXMODE:-scx73}; PG_SECS=300; SCX_BIN_NEW=$SCX_BIN; SCX_BIN=/opt/scx-fix/scx_pfs2 ;;
kubelet) mode=${KLMODE:-kpfs}; PG_SECS=180 ;;
*) echo "bad kind"; exit 2 ;;
esac
kernel_ok "$mode" || exit 5
status "fail-check $kind start mode=$mode kernel=$(nssh "$SSH_B" uname -r)"
nssh "$SSH_B" 'sudo dmesg -T | tail -1' >"$OUTB/$kind-dmesg-before.txt" 2>&1
pg_client
pod=$(pg_server b 4)
setmode "$mode"
ev=$OUTB/$kind-events-$(date -u +%H%M%S).log
bg() {
	local t0; t0=$(date +%s)
	case $kind in
	kwflip)
		local v=1 i
		for i in $(seq 1 7); do
			sleep 15; v=$((1 - v))
			nssh "$SSH_B" "sudo sysctl -qw kernel.sched_cfs_bandwidth_width=$v" && rewrite_cpumax
			echo "$(($(date +%s) - t0)) sysctl=$v $(cgsnap "$pod" | grep -E 'nr_throttled|usage_usec' | tr '\n' ' ')" >>"$ev"
		done ;;
	scxkill)
		( for i in $(seq 1 130); do echo "$(($(date +%s) - t0)) state=$(nssh "$SSH_B" 'cat /sys/kernel/sched_ext/state') ds_ready=$(k get pod -l app.kubernetes.io/name=scx-pfs -o jsonpath='{.items[*].status.containerStatuses[0].ready}{" restarts="}{.items[*].status.containerStatuses[0].restartCount}')"; sleep 1; done >>"$ev.state" ) &
		sleep 30; echo "$(($(date +%s) - t0)) SIGKILL scx_pfs" >>"$ev"; nssh "$SSH_B" "sudo pkill -KILL -f '^$SCX_BIN' ; true"
		sleep 45; echo "$(($(date +%s) - t0)) delete scx-pfs pod" >>"$ev"; k delete pod -l app.kubernetes.io/name=scx-pfs --wait=false >/dev/null
		wait ;;
	kpfschurn)
		local c=0 i lim n
		while (($(date +%s) - t0 < PG_SECS - 20)); do
			c=$((c + 1))
			local ts; ts=$(date +%s%N)
			for i in $(seq 1 10); do
				lim=$(( (i % 4 + 1) * 250 ))m; n=churn-$c-$i
				if ((i % 2)); then
					python3 -I "$KIT/pod.py" "$n" b ubuntu:24.04 --cpu "$lim" --mem 64Mi --hostpath-scx --role churn --deadline 600 -- /opt/scx/spin-loop 2 0 30
				else
					python3 -I "$KIT/pod.py" "$n" b ubuntu:24.04 --cpu "$lim" --mem 64Mi --role churn --deadline 600 -- sh -c 'for i in $(seq 300); do /bin/true; done; sleep 30'
				fi | k apply -f - >/dev/null
			done
			k wait pod -l retest/role=churn --for=condition=Ready --timeout=120s >/dev/null 2>&1 || echo "cycle $c: not all Ready" >>"$ev"
			echo "$(($(date +%s) - t0)) cycle=$c ready_ms=$(( ($(date +%s%N) - ts) / 1000000 ))" >>"$ev"
			sleep 5
			k delete pod -l retest/role=churn --wait=true --grace-period=1 >/dev/null 2>&1
		done ;;
	churn2)
		# about 2 pods/s for PG_SECS-20 s: one limited pod every 0.5 s, each deleted about 10 s after creation
		local c=0 lim n
		while (($(date +%s) - t0 < PG_SECS - 20)); do
			c=$((c + 1)); lim=$(( (c % 4 + 1) * 250 ))m; n=ch2-$c
			python3 -I "$KIT/pod.py" "$n" b ubuntu:24.04 --cpu "$lim" --mem 64Mi --role churn --deadline 120 -- sh -c 'for i in $(seq 50); do /bin/true; done; sleep 8' | k apply -f - >/dev/null 2>>"$ev" || echo "$(($(date +%s) - t0)) create $n failed" >>"$ev"
			if ((c > 20)); then k delete pod "ch2-$((c - 20))" --wait=false --grace-period=1 >/dev/null 2>&1; fi
			((c % 20 == 0)) && echo "$(($(date +%s) - t0)) created=$c running=$(k get pods -l retest/role=churn --no-headers 2>/dev/null | grep -c Running) pending=$(k get pods -l retest/role=churn --no-headers 2>/dev/null | grep -c Pending)" >>"$ev"
			sleep 0.5
		done ;;
	scxroll)
		( for i in $(seq 1 $((PG_SECS + 10))); do echo "$(($(date +%s) - t0)) state=$(nssh "$SSH_B" 'cat /sys/kernel/sched_ext/state') pods=$(k get pod -l app.kubernetes.io/name=scx-pfs -o jsonpath='{.items[*].metadata.name}{" "}{.items[*].status.containerStatuses[0].ready}')"; sleep 1; done >>"$ev.state" ) &
		sleep 60; echo "$(($(date +%s) - t0)) rollout restart ds/scx-pfs" >>"$ev"; k rollout restart ds/scx-pfs >>"$ev" 2>&1
		k rollout status ds/scx-pfs --timeout=120s >>"$ev" 2>&1
		sleep 120; echo "$(($(date +%s) - t0)) upgrade ds/scx-pfs: scx_pfs2 -> $SCX_BIN_NEW" >>"$ev"
		sed "s|@SCX_BIN@|$SCX_BIN_NEW|; s|@SCX_ARGS@||" "$DS_FILE" | k apply -f - >>"$ev" 2>&1
		k rollout status ds/scx-pfs --timeout=120s >>"$ev" 2>&1
		echo "$(($(date +%s) - t0)) running binary: $(nssh "$SSH_B" 'pgrep -af scx_pfs | head -2' | tr '\n' ' ')" >>"$ev"
		wait ;;
	kubelet)
		sleep 60; echo "$(($(date +%s) - t0)) restart kubelet on node B" >>"$ev"
		nssh "$SSH_B" 'sudo systemctl restart kubelet' >>"$ev" 2>&1
		for i in $(seq 1 60); do
			echo "$(($(date +%s) - t0)) kubelet=$(nssh "$SSH_B" 'systemctl is-active kubelet') node_ready=$(kubectl --context "$CTX" get node ${NODE_B} -o jsonpath='{.status.conditions[?(@.type=="Ready")].status}') pg=$(k get pod "$pod" -o jsonpath='{.status.phase}/{.status.containerStatuses[0].restartCount}')" >>"$ev"; sleep 2
		done ;;
	esac
}
bg & BGP=$!
pg_run b "$mode-$kind" 4 tpcb 32 1 "$pod"
wait $BGP || true
nssh "$SSH_B" 'sudo dmesg -T | grep -iE "sched_ext|scx|pfs|WARNING|BUG:|stall|hung|RCU" | tail -30' >"$OUTB/$kind-dmesg-after.txt" 2>&1 || true
[[ $mode == kpfs ]] && nssh "$SSH_B" 'sudo cat /sys/kernel/debug/sched/pfs/stats; echo ---; sudo cat /sys/kernel/debug/sched/pfs/waiters 2>/dev/null | head -40' >"$OUTB/$kind-pfs.txt" 2>&1
[[ $mode == scx* ]] && k logs ds/scx-pfs --tail=60 >"$OUTB/$kind-scx-tail.log" 2>&1 || true
[[ $mode == scx* ]] && k get pod -l app.kubernetes.io/name=scx-pfs -o wide >>"$OUTB/$kind-scx-tail.log" 2>&1 || true
status "fail-check $kind done ($(tail -1 "$ev" 2>/dev/null))"
