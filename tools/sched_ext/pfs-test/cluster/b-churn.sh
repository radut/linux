#!/usr/bin/env bash
# (b) pod churn under MODE (scx|kwidth|cfs): create limited pods at RATE/s for DUR s, each living
# LIFE s, next to the standard load; then counters (overflow/race), sched_ext events, dmesg,
# node Ready, leftover cgroups.
#   MODE=scx ./b-churn.sh     (run under the node B lock)
source "$(dirname "$0")/lib.sh"; source "$KIT/ops.sh"
MODE=${MODE:-scx} RATE=${RATE:-2} DUR=${DUR:-600} LIFE=${LIFE:-10}
LIMITS=(100m 250m 500m 750m 1 1500m 2 2500m)
ops_begin "b-churn-$MODE"
case $MODE in
scx) scx_on ;;
kwidth) kwidth_present || die "no kwidth sysctl on node b"; kwidth_set 1 ;;
cfs) ;;
*) die "MODE scx|kwidth|cfs" ;;
esac
echo "mode=$MODE rate=$RATE dur=$DUR life=$LIFE" >>"$OUT/meta.txt"
nssh 'find /sys/fs/cgroup/kubepods.slice -type d | wc -l' >"$OUT/cgroups.t0"
load_start
[[ $MODE == kwidth ]] && kwidth_rewrite
mon_start $((DUR + 240))
churn_pod() { # name cpu
	cat <<YAML
apiVersion: v1
kind: Pod
metadata:
  name: $1
  namespace: $NS
  labels:
$(meta_labels churn)
    sched-test/churn: "1"
$(meta_annot churn)
spec:
  restartPolicy: Never
$(pod_common | sed 's/^    //')
  containers:
    - name: spin
      image: $IMG_UBUNTU
      imagePullPolicy: IfNotPresent
      command: ["/opt/scx/spin-loop"]
      args: ["4", "$LIFE", "1"]
      securityContext: {allowPrivilegeEscalation: false, capabilities: {drop: [ALL]}, runAsNonRoot: true, runAsUser: 65534}
      resources: {requests: {cpu: "$2", memory: 32Mi}, limits: {cpu: "$2", memory: 32Mi}}
      volumeMounts: [{name: scx-bin, mountPath: /opt/scx, readOnly: true}]
  volumes: [{name: scx-bin, hostPath: {path: /opt/scx, type: Directory}}]
YAML
}
sleep 20; event churn-start
end=$(( $(date +%s) + DUR )) n=0
while (( $(date +%s) < end )); do
	n=$((n + 1))
	churn_pod "ops-churn-$n" "${LIMITS[n % ${#LIMITS[@]}]}" | k create -f - >/dev/null 2>>"$OUT/churn-errors.txt" &
	# delete pods past their life (+5 s slack) so creation and deletion overlap
	if (( n % (RATE * 5) == 0 )); then
		k delete pod -l sched-test/churn=1 --field-selector=status.phase!=Running --wait=false >/dev/null 2>&1 &
		k get pod -l sched-test/churn=1 -o json 2>/dev/null |
			jq -r --argjson l $((LIFE + 30)) '.items[] | select((now - (.metadata.creationTimestamp | fromdateiso8601)) > $l) | .metadata.name' |
			xargs -r kubectl --context "$CTX" -n "$NS" delete pod --wait=false >/dev/null 2>&1 &
		k get pod -l sched-test/churn=1 --no-headers 2>/dev/null | awk '{print $3}' | sort | uniq -c | tr '\n' ' ' >>"$OUT/churn-phases.txt"; echo >>"$OUT/churn-phases.txt"
	fi
	sleep "$(python3 -c "print(1/$RATE)")"
done
wait
event churn-stop
echo "created=$n create_errors=$(grep -c . "$OUT/churn-errors.txt" 2>/dev/null || echo 0)" >>"$OUT/meta.txt"
k delete pod -l sched-test/churn=1 --wait=true --timeout=300s || true
sleep 20
health_snap "-$((DUR / 60 + 5))min" >"$OUT/health.txt"
[[ $MODE == scx ]] && k logs ds/scx-width --tail=8 >"$OUT/scx-counters.txt"
nssh 'find /sys/fs/cgroup/kubepods.slice -type d | wc -l' >"$OUT/cgroups.t1"
kc get node "$NODE_B" -o jsonpath='{range .status.conditions[*]}{.type}={.status} {end}' >>"$OUT/meta.txt"
ops_end
