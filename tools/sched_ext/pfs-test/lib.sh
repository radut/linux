# SPDX-License-Identifier: GPL-2.0
# shellcheck shell=bash disable=SC2034
# Shared helpers for the PFS test kit. Sourced, not executed.
#
# Variants:
#   cfs     stock CFS bandwidth control (quota per period)
#   scx     the scx_pfs sched_ext scheduler loaded for the duration of a run
#   kwidth  the in-kernel width mode, sysctl kernel.sched_cfs_bandwidth_width=1
#
# Environment:
#   SCX        path to the scx_pfs binary (default: search, see find_scx)
#   SCX_ARGS   extra scx_pfs options, e.g. "-s 2500 -w 10000"
#   SCX_LOG    file for scx_pfs's output (default: a new mktemp file)
#   SCX_STATS  file scx_pfs writes its per-cgroup throttling counters to (-S, host
#              mode only; default: a new mktemp file)
#   WT_CG      cgroup directory name under /sys/fs/cgroup (default pfs-test)
#   WT_TRACE   0 disables the sched_switch trace (no concurrency columns)

KIT=${KIT:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
CG_FS=/sys/fs/cgroup
CG_ROOT=$CG_FS/${WT_CG:-pfs-test}
TRACEFS=/sys/kernel/tracing
SCX_STATE=/sys/kernel/sched_ext/state
KW_SYSCTL=/proc/sys/kernel/sched_cfs_bandwidth_width
PERIOD_US=${PERIOD_US:-100000}
CSV_HEADER="path,variant,limit,threads,neighbours,secs,miter_s,nr_throttled,throttled_ms,worst_stall_ms,stalls_5ms,avg_conc,max_conc,pct_over_cap,p50_ms,p99_ms,max_ms,pfs_nr_throttled,pfs_throttled_ms"

WT_PIDS=()
SCX_PID=
SCX_STARTED=
KW_SAVED=
TRACE_ON=0

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }
die() { echo "error: $*" >&2; exit 1; }

# check_num NAME VALUE: VALUE is a plain non-negative number (e.g. 1, 0.5)
check_num() {
	[[ $2 =~ ^[0-9]+(\.[0-9]+)?$ ]] || die "$1 must be a non-negative number, not '$2'"
}

# check_int NAME VALUE: VALUE is a positive integer
check_int() {
	[[ $2 =~ ^[1-9][0-9]{0,8}$ ]] || die "$1 must be a positive integer, not '$2'"
}

# check_case LIMIT THREADS SECS: the arguments of one test case
check_case() {
	[ "$1" = max ] || check_num LIMIT "$1"
	[[ $2 =~ ^[0-9]{1,4}$ ]] || die "THREADS must be a number of threads, not '$2'"
	check_int SECS "$3"
}

# scx_args: SCX_ARGS split into the array SCX_ARGV, options and numbers only,
# so that the variable can't smuggle other words onto a command line
scx_args() {
	local a
	read -r -a SCX_ARGV <<< "${SCX_ARGS:-}"
	for a in "${SCX_ARGV[@]}"; do
		[[ $a =~ ^-?[A-Za-z0-9.]+$ ]] || die "SCX_ARGS: unexpected word '$a'"
	done
}

# scx_log: the file scx_pfs writes to; a private new file unless SCX_LOG is set
scx_log() {
	if [ -z "${SCX_LOG:-}" ]; then
		SCX_LOG=$(mktemp "${TMPDIR:-/tmp}/scx_pfs.XXXXXXXX") || die "mktemp failed"
	fi
	echo "$SCX_LOG"
}

# scx_stats: the file scx_pfs writes its throttling counters to, see pfs_stat
scx_stats() {
	if [ -z "${SCX_STATS:-}" ]; then
		SCX_STATS=$(mktemp "${TMPDIR:-/tmp}/scx_pfs_stats.XXXXXXXX") || die "mktemp failed"
	fi
	echo "$SCX_STATS"
}

need_root() {
	[ "$(id -u)" = 0 ] || die "run as root (cgroup, tracefs and sched_ext need it)"
}

check_cgroup2() {
	[ "$(stat -fc %T $CG_FS 2>/dev/null)" = cgroup2fs ] ||
		die "$CG_FS is not cgroup v2 (boot with systemd.unified_cgroup_hierarchy=1)"
	grep -qw cpu $CG_FS/cgroup.controllers || die "cpu controller not available in $CG_FS"
}

# limit in CPUs (e.g. 0.5) or "max" -> "QUOTA PERIOD" for cpu.max
cpu_max_value() {
	if [ "$1" = max ]; then
		echo "max $PERIOD_US"
	else
		awk -v l="$1" -v p="$PERIOD_US" 'BEGIN { printf "%d %d\n", l * p, p }'
	fi
}

# --- variants --------------------------------------------------------------

have_scx_kernel() { [ -r $SCX_STATE ]; }
have_kwidth() { [ -e $KW_SYSCTL ]; }

find_scx() {
	local c
	for c in "${SCX:-}" "$KIT/bin/scx_pfs" "$KIT/../build/bin/scx_pfs" \
		 "$(command -v scx_pfs 2>/dev/null || true)"; do
		if [ -n "$c" ] && [ -x "$c" ]; then
			echo "$c"
			return 0
		fi
	done
	return 1
}

available_variants() {
	local v="cfs"
	if have_scx_kernel && find_scx > /dev/null; then
		v="$v,scx"
	fi
	if have_kwidth; then
		v="$v,kwidth"
	fi
	echo "$v"
}

# wait_scx_enabled: up to 5 s for /sys/kernel/sched_ext/state to read enabled
wait_scx_enabled() {
	local i
	for i in $(seq 50); do
		[ "$(cat $SCX_STATE)" = enabled ] && return 0
		[ -z "$SCX_PID" ] || kill -0 "$SCX_PID" 2>/dev/null || return 1
		sleep 0.1
	done
	[ "$(cat $SCX_STATE)" = enabled ]
}

# scx_start: load scx_pfs. SCX_MODE picks how:
#   host       run the binary from this script (default)
#   service    systemctl start $SCX_UNIT (default scx_pfs.service)
#   container  container with the capabilities to load it, see scx_container_start
scx_start() {
	local bin
	have_scx_kernel || die "kernel has no sched_ext ($SCX_STATE missing; needs CONFIG_SCHED_CLASS_EXT)"
	[ "$(cat $SCX_STATE)" = disabled ] ||
		die "another sched_ext scheduler is loaded ($(cat /sys/kernel/sched_ext/root/ops 2>/dev/null))"
	case ${SCX_MODE:-host} in
	host)
		bin=$(find_scx) || die "scx_pfs binary not found (set SCX=/path/to/scx_pfs)"
		scx_args
		scx_log > /dev/null
		scx_stats > /dev/null
		"$bin" "${SCX_ARGV[@]}" -i 3600 -S "$SCX_STATS" > "$SCX_LOG" 2>&1 &
		SCX_PID=$!
		wait_scx_enabled || { cat "$SCX_LOG" >&2; die "scx_pfs did not enable"; }
		log "scx_pfs output in $SCX_LOG"
		;;
	service)
		systemctl start "${SCX_UNIT:-scx_pfs.service}"
		SCX_STARTED=service
		wait_scx_enabled || { journalctl -u "${SCX_UNIT:-scx_pfs.service}" -n 20 >&2; die "scx_pfs service did not enable"; }
		;;
	container)
		scx_container_start
		SCX_STARTED=container
		;;
	*)
		die "SCX_MODE must be host, service or container"
		;;
	esac
	log "scx_pfs enabled via ${SCX_MODE:-host} (ops=$(cat /sys/kernel/sched_ext/root/ops))"
}

scx_stop() {
	case ${SCX_STARTED:-} in
	service)
		systemctl stop "${SCX_UNIT:-scx_pfs.service}" || true
		;;
	container)
		scx_container_stop
		;;
	esac
	SCX_STARTED=
	[ -n "$SCX_PID" ] || return 0
	kill -INT "$SCX_PID" 2>/dev/null || true
	wait "$SCX_PID" 2>/dev/null || true
	SCX_PID=
	# the kernel ejects a scheduler that stalls tasks (watchdog); report it
	if [ -n "${SCX_LOG:-}" ] &&
	   grep -q 'SCX_EXIT_ERROR\|runnable task stall\|watchdog' "$SCX_LOG" 2>/dev/null; then
		log "scx_pfs log reports an error exit, see $SCX_LOG"
	fi
}

kwidth_set() {
	have_kwidth || die "kernel has no $KW_SYSCTL (kwidth patch not applied)"
	[ -n "$KW_SAVED" ] || KW_SAVED=$(cat $KW_SYSCTL)
	echo "$1" > $KW_SYSCTL
}

kwidth_restore() {
	if [ -n "$KW_SAVED" ]; then
		echo "$KW_SAVED" > $KW_SYSCTL
		KW_SAVED=
	fi
}

# variant_start VARIANT: cpu.max values written after this are enforced by it
variant_start() {
	case $1 in
	cfs)
		have_kwidth && kwidth_set 0
		if have_scx_kernel && [ "$(cat $SCX_STATE)" != disabled ]; then
			die "a sched_ext scheduler is loaded; unload it for the cfs variant"
		fi
		;;
	kwidth)
		kwidth_set 1
		;;
	scx)
		have_kwidth && kwidth_set 0
		scx_start
		;;
	*)
		die "unknown variant $1 (cfs|scx|kwidth)"
		;;
	esac
}

variant_stop() {
	scx_stop
	kwidth_restore
}

# --- cgroups ---------------------------------------------------------------

cg_setup() {
	grep -qw cpu $CG_FS/cgroup.subtree_control || echo +cpu > $CG_FS/cgroup.subtree_control
	mkdir -p "$CG_ROOT"
	grep -qw cpu "$CG_ROOT/cgroup.subtree_control" || echo +cpu > "$CG_ROOT/cgroup.subtree_control"
}

# cg_new NAME LIMIT [WEIGHT]: fresh child cgroup, so cpu.stat starts at zero
cg_new() {
	local d=$CG_ROOT/$1
	cg_rm "$d"
	mkdir "$d"
	cpu_max_value "$2" > "$d/cpu.max"
	[ -z "${3:-}" ] || echo "$3" > "$d/cpu.weight"
	echo "$d"
}

cg_rm() {
	local d=$1 i
	[ -d "$d" ] || return 0
	for i in $(seq 50); do
		[ -s "$d/cgroup.procs" ] || break
		sleep 0.1
	done
	rmdir "$d" 2>/dev/null || log "could not remove $d (tasks still inside?)"
}

# cg_exec DIR CMD...: run CMD inside cgroup DIR in the background, pid in $!
cg_exec() {
	local d=$1
	shift
	bash -c 'echo $$ > "$0/cgroup.procs" && exec "$@"' "$d" "$@" &
	WT_PIDS+=($!)
}

# cpu.stat of DIR as "nr_throttled throttled_ms usage_ms"
cg_stat() {
	awk '/^nr_throttled/ {n = $2} /^throttled_usec/ {t = $2} /^usage_usec/ {u = $2}
	     END { printf "%d %.1f %.1f\n", n, t / 1000, u / 1000 }' "$1/cpu.stat"
}

# scx_pfs's throttling counters of DIR as "nr_throttled throttled_ms", "- -"
# without them (other variants, scx_pfs not run by this script). The file is
# rewritten every second.
pfs_stat() {
	local f=${SCX_STATS:-}
	if [ -z "${SCX_PID:-}" ] || [ ! -s "$f" ]; then
		echo "- -"
		return
	fi
	awk -v p="${1#"$CG_FS"}" '$1 == p { n = $5; t = $7 }
	     END { if (n == "") print "0 0.0"; else printf "%d %.1f\n", n, t / 1000 }' "$f"
}

# --- tracing ---------------------------------------------------------------

trace_start() {
	TRACE_ON=0
	[ "${WT_TRACE:-1}" = 1 ] || return 0
	mountpoint -q $TRACEFS || mount -t tracefs nodev $TRACEFS 2>/dev/null || return 0
	[ -w $TRACEFS/trace_clock ] || return 0
	echo 0 > $TRACEFS/tracing_on
	echo mono > $TRACEFS/trace_clock
	echo > $TRACEFS/trace
	echo 16384 > $TRACEFS/buffer_size_kb
	echo 1 > $TRACEFS/events/sched/sched_switch/enable
	echo 1 > $TRACEFS/tracing_on
	TRACE_ON=1
}

# trace_stop FILE
trace_stop() {
	[ "$TRACE_ON" = 1 ] || return 0
	echo 0 > $TRACEFS/tracing_on
	echo 0 > $TRACEFS/events/sched/sched_switch/enable
	cat $TRACEFS/trace > "$1"
	echo > $TRACEFS/trace
	TRACE_ON=0
}

# conc_of COMM TRACE LIMIT -> "avg max pct_over_cap" or "- - -"
conc_of() {
	local cap=$3 out
	[ "$cap" = max ] && cap=1024
	if [ -s "$2" ] && command -v python3 > /dev/null; then
		out=$(python3 "$KIT/conc.py" "$1" "$2" "$cap")
		echo "$out" | sed -E 's/.*max_conc=([0-9]+) avg_conc=([0-9.]+) pct_over_cap=([0-9.]+).*/\2 \1 \3/'
	else
		echo "- - -"
	fi
}

# kv KEY LINE: value of KEY=value in LINE, "-" if absent
kv() {
	echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1 | grep . || echo -
}

# --- cleanup ---------------------------------------------------------------

kill_pids() {
	local p
	for p in "${WT_PIDS[@]}"; do
		kill "$p" 2>/dev/null || true
	done
	for p in "${WT_PIDS[@]}"; do
		wait "$p" 2>/dev/null || true
	done
	WT_PIDS=()
}


# --- docker / podman -------------------------------------------------------

DOCKER=${DOCKER:-docker}
IMAGE=${IMAGE:-pfs-test:local}
SCX_CTR=${SCX_CTR:-scx-pfs}
WT_CTRS=()

# docker_check: runtime answers and the image exists
docker_check() {
	$DOCKER version > /dev/null 2>&1 || die "'$DOCKER' does not answer (installed? daemon running?)"
	$DOCKER image inspect "$IMAGE" > /dev/null 2>&1 || die "image $IMAGE missing for '$DOCKER', run docker-image.sh with the same DOCKER"
}

# runtime_label: docker-systemd, podman-cgroupfs, podman-rootless-systemd, ...
# (WT_LABEL overrides); goes into the "path" column
runtime_label() {
	local name drv rl=""
	if [ -n "${WT_LABEL:-}" ]; then
		echo "$WT_LABEL"
		return
	fi
	if $DOCKER --version 2>/dev/null | grep -qi podman; then
		name=podman
		drv=$($DOCKER info --format '{{.Host.CgroupManager}}' 2>/dev/null || echo unknown)
		[ "$($DOCKER info --format '{{.Host.Security.Rootless}}' 2>/dev/null)" != true ] || rl=-rootless
	else
		name=docker
		drv=$($DOCKER info --format '{{.CgroupDriver}}' 2>/dev/null || echo unknown)
	fi
	echo "$name$rl-$drv"
}

# container_cgroup ID: host path of the container's cgroup (works for the
# systemd driver, .../system.slice/docker-<id>.scope, and for cgroupfs,
# /sys/fs/cgroup/docker/<id>)
container_cgroup() {
	local pid cg i
	pid=$($DOCKER inspect -f '{{.State.Pid}}' "$1")
	[ "$pid" -gt 0 ] 2>/dev/null || die "container $1 is not running"
	cg=$CG_FS$(sed -n 's/^0:://p' /proc/"$pid"/cgroup)
	limited_cgroup "$cg"
}

# limited_cgroup DIR: DIR, or the nearest ancestor (3 levels at most) that
# carries a cpu.max limit; some runtimes put the processes in a child
# ("container", "init.scope") of the cgroup they set the limit on
limited_cgroup() {
	local cg=$1 d=$1 i
	for i in 1 2 3; do
		if [ -r "$d/cpu.max" ] && ! grep -q '^max ' "$d/cpu.max"; then
			echo "$d"
			return
		fi
		d=$(dirname "$d")
		[ "$d" != "$CG_FS" ] || break
	done
	echo "$cg"
}

# Capabilities and options of the scheduler's container: loading a sched_ext
# scheduler is bpf() with CAP_BPF and CAP_PERFMON, plus CAP_SYS_ADMIN for
# struct_ops; libbpf reads the kernel's BTF from /sys/kernel/btf. No host PID
# namespace, devices, writable mounts or network are needed.
SCX_CTR_OPTS=(--cap-drop ALL --cap-add SYS_ADMIN --cap-add BPF --cap-add PERFMON
	      --security-opt no-new-privileges --read-only --network none)

# scx_container_start: scx_pfs in a container, the same shape as a
# Kubernetes DaemonSet
scx_container_start() {
	local i
	[ "$(cat $SCX_STATE 2>/dev/null)" = disabled ] || die "sched_ext missing or a scheduler is already loaded"
	scx_args
	$DOCKER run -d --name "$SCX_CTR" "${SCX_CTR_OPTS[@]}" \
		"$IMAGE" /scx_pfs "${SCX_ARGV[@]}" -i 3600 > /dev/null
	for i in $(seq 50); do
		[ "$(cat $SCX_STATE)" = enabled ] && break
		sleep 0.1
	done
	[ "$(cat $SCX_STATE)" = enabled ] || { $DOCKER logs "$SCX_CTR" >&2; die "scx_pfs container did not enable"; }
	log "scx_pfs enabled from container $SCX_CTR"
}

scx_container_stop() {
	$DOCKER inspect "$SCX_CTR" > /dev/null 2>&1 || return 0
	$DOCKER stop -t 10 "$SCX_CTR" > /dev/null 2>&1 || true
	$DOCKER rm -f "$SCX_CTR" > /dev/null 2>&1 || true
}


# --- cleanup (trap EXIT INT TERM) -------------------------------------------

cleanup() {
	local rc=$? c d
	trap - EXIT INT TERM
	! declare -F stop_units > /dev/null || stop_units
	! declare -F stop_slice > /dev/null || stop_slice
	kill_pids
	for c in "${WT_CTRS[@]}"; do
		$DOCKER rm -f "$c" > /dev/null 2>&1 || true
	done
	[ "$TRACE_ON" = 1 ] && trace_stop /dev/null
	variant_stop
	if [ -d "$CG_ROOT" ]; then
		for d in "$CG_ROOT"/*/; do
			[ -d "$d" ] && cg_rm "${d%/}"
		done
		rmdir "$CG_ROOT" 2>/dev/null || true
	fi
	exit $rc
}
