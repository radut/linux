#!/usr/bin/env bash
# extra/drain.sh: drain then uncordon node B only, run inside a nodeB.lock hold. Skips when pods of other workers or
# other namespaces (outside kube-system and sched-test) run on node B. Records the evicted pods, uncordons at once,
# and confirms node B Ready, schedulable, cilium Ready and every node-B DaemonSet pod ready.
source "$(dirname "$0")/../lib.sh"
NODE="${NODE:-nodeB}"
OUT=$S/results4/remeasure5/extra/drain; mkdir -p "$OUT"
kc() { kubectl --context "$CTX" "$@"; }
trap 'kc uncordon $NODE >>"$OUT/drain.log" 2>&1 || true' EXIT
others=$(kc get pods -A --field-selector spec.nodeName=$NODE -o jsonpath='{range .items[*]}{.metadata.namespace}/{.metadata.name}{"\n"}{end}' |
	grep -vE '^(kube-system|sched-test)/' || true)
if [[ -n $others ]]; then
	status "drain SKIPPED: pods outside kube-system/sched-test on node B: $(echo $others)"
	exit 0
fi
kc get pods -A --field-selector spec.nodeName=$NODE -o wide >"$OUT/pods-before.txt"
status "drain start node B"
t0=$(date +%s)
kc cordon $NODE >>"$OUT/drain.log" 2>&1
timeout 420 kubectl --context "$CTX" drain $NODE --ignore-daemonsets --delete-emptydir-data --force --grace-period=10 \
	--timeout=360s >>"$OUT/drain.log" 2>&1
rc=$?
grep -iE "evict|deleting|delet" "$OUT/drain.log" >"$OUT/evicted.txt" || true
kc uncordon $NODE >>"$OUT/drain.log" 2>&1
t1=$(date +%s)
rd="" sc="x"
for i in $(seq 1 30); do
	rd=$(kc get node $NODE -o jsonpath='{.status.conditions[?(@.type=="Ready")].status}')
	sc=$(kc get node $NODE -o jsonpath='{.spec.unschedulable}')
	[[ $rd == True && -z $sc ]] && break
	sleep 10
done
sleep 20
cil=$(kc -n kube-system get pod -l k8s-app=cilium --field-selector spec.nodeName=$NODE -o jsonpath='{.items[0].status.containerStatuses[0].ready}')
kc get pods -A --field-selector spec.nodeName=$NODE -o wide >"$OUT/pods-after.txt"
notready=$(kc get pods -A --field-selector spec.nodeName=$NODE --no-headers | awk '{split($3,a,"/"); if (a[1]!=a[2] || $4!="Running") print $1"/"$2}')
echo "$notready" >"$OUT/notready-after.txt"
status "drain done node B: drain_rc=$rc cordon-to-uncordon=$((t1 - t0))s Ready=$rd unschedulable=${sc:-false} cilium_ready=$cil evicted=$(grep -c . "$OUT/evicted.txt") not_ready_pods_after=$(echo "$notready" | grep -c .)"
