#!/usr/bin/env bash
# (a) scx_width killed/restarted under load: kill -9, DaemonSet pod deleted, SIGTERM; REPS each.
# Measures time to fallback (sched_ext state change), what the limited pods see, and recovery.
# Run under the node B lock:  flock -w 7200 $S/nodeB.lock -c "$KIT/a-kill.sh"
source "$(dirname "$0")/lib.sh"; source "$KIT/ops.sh"
REPS=${REPS:-3} GAP=${GAP:-90}
ops_begin a-kill
scx_on
load_start
mon_start $((60 + 3 * REPS * GAP + 120))
sleep 30; event steady
for i in $(seq 1 "$REPS"); do
	t=$(nssh 'date +%s.%N; sudo pkill -KILL -x scx_width' | head -1); event "kill9-$i" "$t"
	sleep "$GAP"; health_snap -3min >>"$OUT/health.txt"
	[[ "$(node_sched)" == scx:width ]] || { log "not back after kill9-$i, waiting"; k wait pod -l app.kubernetes.io/name=scx-width --for=condition=Ready --timeout=120s || true; }

	p=$(k get pod -l app.kubernetes.io/name=scx-width -o name)
	event "delete-pod-$i"; k delete "$p" --wait=false
	sleep "$GAP"; health_snap -3min >>"$OUT/health.txt"
	k wait pod -l app.kubernetes.io/name=scx-width --for=condition=Ready --timeout=120s || true

	t=$(nssh 'date +%s.%N; sudo pkill -TERM -x scx_width' | head -1); event "sigterm-$i" "$t"
	sleep "$GAP"; health_snap -3min >>"$OUT/health.txt"
	k wait pod -l app.kubernetes.io/name=scx-width --for=condition=Ready --timeout=180s || true
done
k get pod -l app.kubernetes.io/name=scx-width -o jsonpath='{range .items[*]}{.metadata.name} restarts={.status.containerStatuses[0].restartCount}{"\n"}{end}' >>"$OUT/meta.txt"
ops_end
