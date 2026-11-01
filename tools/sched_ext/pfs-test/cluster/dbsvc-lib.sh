# shellcheck shell=bash
# Helpers for the db-services batches. Source from a batch script that runs under the node lock(s).
set -euo pipefail
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
KIT=$S/results2/scx-fix/kit
R=$S/results2/scx-fix
CTX="${CTX:?set CTX to the kubectl context}"
NS=sched-test
SSH_A="${SSH_A:?set SSH_A to user@host of node A}"
SSH_B="${SSH_B:?set SSH_B to user@host of node B}"
DS_FILE=$KIT/scx-pfs-daemonset.yaml
SCX_BIN=${SCX_BIN:-/opt/scx-fix/scx_pfs}

log() { echo "$(date -u +%T) $*" >&2; }
k() { kubectl --context "$CTX" -n "$NS" "$@"; }
nssh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$@"; }
host_of() { [[ $1 == a ]] && echo "$SSH_A" || echo "$SSH_B"; }
status() { echo "$(date -u +%FT%TZ) scx-fix $*" >>"$S/STATUS.md"; }

# mkpod NAME NODE IMAGE [pod.py options] [-- CMD...]: create, wait Running+Ready-ish.
mkpod() {
	local name=$1
	python3 -I "$KIT/pod.py" "$@" | k apply -f - >/dev/null
	k wait pod "$name" --for=condition=Ready --timeout=300s >/dev/null
}
rmpod() { k delete pod "$@" --ignore-not-found --wait=true >/dev/null 2>&1 || true; }
podip() { k get pod "$1" -o jsonpath='{.status.podIP}'; }
podspec() { k get pod "$1" -o json | python3 -I -c 'import json,sys; p=json.load(sys.stdin); print(json.dumps({"node":p["spec"]["nodeName"],"image":p["status"]["containerStatuses"][0]["imageID"],"resources":p["spec"]["containers"][0].get("resources",{}),"qos":p["status"].get("qosClass"),"cmd":p["spec"]["containers"][0].get("command")}))'; }

# cgsnap NODE POD: cpu.max + cpu.stat of the pod cgroup.
cgsnap() {
	local uid
	uid=$(k get pod "$2" -o jsonpath='{.metadata.uid}')
	nssh "$(host_of "$1")" "d=\$(find /sys/fs/cgroup/kubepods.slice -maxdepth 3 -type d -name '*pod${uid//-/_}.slice' | head -1)
		[ -n \"\$d\" ] || { echo cgroup=not-found; exit 0; }
		echo cgroup=\$d; echo cpu.max=\$(cat \$d/cpu.max); cat \$d/cpu.stat"
}

node_sched() {
	nssh "$SSH_B" 'st=$(cat /sys/kernel/sched_ext/state 2>/dev/null || echo missing)
		w=$(cat /proc/sys/kernel/sched_cfs_bandwidth_width 2>/dev/null || echo na)
		if [ "$st" = enabled ]; then echo "scx:$(cat /sys/kernel/sched_ext/root/ops)"; elif [ "$w" = 1 ]; then echo kwidth; else echo cfs; fi'
}

# Rewrite every cpu.max under kubepods so groups pick up the current width sysctl.
rewrite_cpumax() {
	nssh "$SSH_B" 'sudo sh -c "for f in \$(find /sys/fs/cgroup/kubepods.slice -name cpu.max); do cat \$f > \$f; done"'
}

scx_on() {
	[[ $(node_sched) == cfs ]] || { log "node B not on cfs before scx_on: $(node_sched)"; return 1; }
	sed "s|@SCX_BIN@|$SCX_BIN|; s|@SCX_ARGS@|${SCX_ARGS:-}|" "$DS_FILE" | k apply -f - >/dev/null
	k rollout status ds/scx-pfs --timeout=120s >/dev/null
	k wait pod -l app.kubernetes.io/name=scx-pfs --for=condition=Ready --timeout=90s >/dev/null
	[[ $(node_sched) == scx:pfs ]] || { log "scx not enabled: $(node_sched)"; return 1; }
}
scx_off() {
	k delete ds scx-pfs --ignore-not-found --wait=true >/dev/null
	k wait pod -l app.kubernetes.io/name=scx-pfs --for=delete --timeout=90s >/dev/null 2>&1 || true
	local _
	for _ in $(seq 30); do [[ $(node_sched) != scx:* ]] && return 0; sleep 2; done
	return 1
}
kwidth_set() { # 0|1
	nssh "$SSH_B" "sudo sysctl -q -w kernel.sched_cfs_bandwidth_width=$1"
	rewrite_cpumax
}
# setmode cfs|scx|kwidth
setmode() {
	case $1 in
	cfs) scx_off; [[ -e $S/kernel-on-nodeB.ok ]] && nssh "$SSH_B" 'test -e /proc/sys/kernel/sched_cfs_bandwidth_width' && kwidth_set 0; true ;;
	scx) [[ $(node_sched) == kwidth ]] && kwidth_set 0; scx_on ;;
	kwidth) scx_off; kwidth_set 1 ;;
	esac
	log "mode now $(node_sched)"
}
# Leave node B clean: scx off, sysctl 0 (with cpu.max rewritten).
cleanup_node_b() {
	scx_off || true
	if nssh "$SSH_B" 'test -e /proc/sys/kernel/sched_cfs_bandwidth_width'; then kwidth_set 0; fi
	log "node B left as $(node_sched)"
}

# meta OUTDIR NODE MODE: node/kernel/scheduler info.
meta() {
	{
		echo "node=$2 mode=$3 started=$(date -Is)"
		nssh "$(host_of "$2")" 'echo "host=$(hostname) kernel=$(uname -r) nproc=$(nproc)"
			echo "sched_ext_state=$(cat /sys/kernel/sched_ext/state 2>/dev/null)"
			echo "ops=$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)"
			echo "sched_cfs_bandwidth_width=$(cat /proc/sys/kernel/sched_cfs_bandwidth_width 2>/dev/null || echo absent)"'
		[[ $3 == scx ]] && echo "scx_width: /opt/scx/scx_width sha256 12ceee5b..52603a, args -i 10 (slice default 2500us, window 10000us)"
		true
	} >"$1/meta.txt"
}

# measure OUTDIR NODE MODE POD... -- CMD: snapshot cgroups of POD(s), run CMD (stdout -> run.out), snapshot again.
measure() {
	local out=$1 node=$2 mode=$3 pods=() p t0
	shift 3
	while [[ $1 != -- ]]; do pods+=("$1"); shift; done
	shift
	mkdir -p "$out"
	meta "$out" "$node" "$mode"
	for p in "${pods[@]}"; do podspec "$p" >"$out/spec-$p.json"; cgsnap "$node" "$p" >"$out/cg-$p.t0"; done
	t0=$(date +%s)
	"$@" >"$out/run.out" 2>"$out/run.err" || echo "rc=$?" >>"$out/run.err"
	for p in "${pods[@]}"; do cgsnap "$node" "$p" >"$out/cg-$p.t1"; done
	if [[ $mode == scx ]]; then k logs ds/scx-pfs --since="$(($(date +%s) - t0 + 5))s" >"$out/scx-width.log" 2>&1 || true; fi
	echo "ended=$(date -Is) secs=$(($(date +%s) - t0))" >>"$out/meta.txt"
}
