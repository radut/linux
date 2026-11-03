#!/usr/bin/env bash
# Load + measurement scaffolding for the failure/operations batches (a-e).
# Sourced after lib.sh. A batch: ops_begin NAME; load_start; ...events...; ops_end.
# shellcheck disable=SC2034

# Two CPU-bound pods at 1 s resolution (stalls, CPUs used) and an HTTP service at a fixed rate.
LOAD_SPIN=("ops-spin-1500m 1500m 8 1" "ops-spin-2 2 8 1")
LOAD_HTTP=(1 1 500 30)   # nginx cpu, k6 cpu, rps, chunk s

ops_begin() { # NAME
	OPS_NAME=$1
	OUT="$OUT_ROOT/$(date -u +%Y%m%d-%H%M%S)-$1"
	mkdir -p "$OUT"
	EVENTS="$OUT/events.tsv"
	: >"$EVENTS"
	exec > >(tee -a "$OUT/batch.log") 2>&1
	trap 'ops_abort' EXIT
	status "batch $1 start (holding nodeB.lock) -> $OUT"
	log "batch $1 -> $OUT"
	node_meta >"$OUT/meta.txt"
	kc get node "$NODE_B" -o wide >>"$OUT/meta.txt"
	cp "$KIT"/*.sh "$OUT/" 2>/dev/null || true
}

ops_abort() {
	local rc=$?
	if [[ -z "${OPS_DONE:-}" ]]; then
		status "batch ${OPS_NAME:-?} aborted rc=$rc; restoring node b"
		collect || true
		restore_node_b || true
	fi
}

ops_end() {
	collect
	restore_node_b
	OPS_DONE=1
	status "batch $OPS_NAME done; node b sched=$(node_sched) sysctl=$(kwidth_get) -> $OUT"
}

# event NAME [node-time]: record an event on the node clock
event() {
	local t=${2:-}
	[[ -n "$t" ]] || t=$(python3 -c "print(f'{$(now) - $CLOCK_OFF:.4f}')")
	printf '%s\t%s\n' "$t" "$1" >>"$EVENTS"
	log "event $1 @ $t"
}

load_start() {
	local s
	{
		for s in "${LOAD_SPIN[@]}"; do
			# shellcheck disable=SC2086
			render_spin $s
		done
		render_http "${LOAD_HTTP[@]}"
	} >"$OUT/load.yaml"
	k apply -f "$OUT/load.yaml" >&2
	wait_ready sched-test/owner=ops-soak 300s
	CLOCK_OFF=$(clock_offset)
	echo "clock_offset_local_minus_node=$CLOCK_OFF" >>"$OUT/meta.txt"
	k get pod -l sched-test/owner=ops-soak -o yaml >"$OUT/pods.yaml"
}

# monitor for DUR seconds (node side), in the background
mon_start() {
	nodemon_start "$OUT/nodemon.jsonl" "$1"
	sleep 2
}

collect() {
	[[ -n "${OUT:-}" ]] || return 0
	local p
	for p in $(k get pod -l sched-test/owner=ops-soak -o name 2>/dev/null); do
		k logs "$p" --all-containers --timestamps >"$OUT/${p#pod/}.log" 2>&1 || true
		k logs "$p" --all-containers --timestamps --previous >"$OUT/${p#pod/}.prev.log" 2>/dev/null ||
			rm -f "$OUT/${p#pod/}.prev.log"
	done
	if ds_present; then
		for p in $(k get pod -l app.kubernetes.io/name=scx-width -o name); do
			k logs "$p" --timestamps >"$OUT/scx-${p#pod/}.log" 2>&1 || true
			k logs "$p" --timestamps --previous >"$OUT/scx-${p#pod/}.prev.log" 2>/dev/null || true
		done
		k get pod -l app.kubernetes.io/name=scx-width -o wide >"$OUT/scx-pods.txt" 2>&1 || true
	fi
	k get events --sort-by=.lastTimestamp >"$OUT/k8s-events.txt" 2>&1 || true
	health_snap -60min >"$OUT/health-end.txt" 2>&1 || true
	nssh "sudo journalctl -k --since '-60min' --no-pager -o short-iso-precise" >"$OUT/dmesg.txt" 2>&1 || true
	[[ -n "${MON_PID:-}" ]] && { wait "$MON_PID" 2>/dev/null || true; }
	python3 -I "$KIT/timeline.py" "$OUT" >"$OUT/timeline.txt" 2>&1 || true
}
