#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Print which of the four CPU-limit variants is enforcing cpu.max on this
# machine, and the evidence used. See VARIANTS.md.
#
#   cfs      stock CFS bandwidth control
#   kwidth   the width policy inside CFS bandwidth control  (sysctl)
#   scx_pfs  a sched_ext BPF scheduler                       (a running binary)
#   kPFS     a scheduling class of its own                   (boot parameter)
#
# The checks run in the order kPFS, scx_pfs, kwidth, cfs, because the lower ones
# cannot be active while a higher one is: sched_ext cannot load on a sched_pfs=1
# boot, and the width sysctl governs the fair class, which on either of those is
# not what runs the tasks. Checking the sysctl first - the obvious way to write
# this - reports kwidth on a kPFS machine.
#
# Two things this deliberately does NOT do:
#
#   * conclude "not kPFS" from a file it could not read. Without root, debugfs
#     is unreadable, and "absent" and "not permitted to look" are different
#     answers. It says which one it got.
#   * trust sched_pfs=1 on the command line by itself. A kernel built without
#     CONFIG_SCHED_PFS ignores the parameter, so the parameter alone proves
#     only that someone asked. The class having registered its debugfs
#     directory is what proves it answered.
#
# usage: which-variant.sh [-q]
#   -q   print only the variant name
set -u

QUIET=0
[ "${1:-}" = -q ] && QUIET=1

# The four interfaces this reads. Overridable only so that each branch can be
# exercised against a fixture tree on a machine that is not running that
# variant - a check whose own branches have never run is not a check.
KW=${WV_KW:-/proc/sys/kernel/sched_cfs_bandwidth_width}
SCX=${WV_SCX:-/sys/kernel/sched_ext}
PFSD=${WV_PFSD:-/sys/kernel/debug/sched/pfs}
CMDLINE=${WV_CMDLINE:-/proc/cmdline}

say() { [ "$QUIET" = 1 ] || printf '%s\n' "$*"; }
# The variant name is printed LAST and on a line of its own, so that
# "$(which-variant.sh -q)" is the name and nothing else. Each remaining
# argument is one evidence line: "say $@" would join them with spaces.
verdict() {
	local v=$1; shift
	local l; for l in "$@"; do say "$l"; done
	printf '%s\n' "$v"
	exit 0
}

# ---------------------------------------------------------------- 1. kPFS
# grep -q, not "grep -c ... -eq 1": grep -c PRINTS 0 and EXITS 1 on no-match,
# which makes a count of zero and a failed command the same thing.
if grep -qw sched_pfs=1 "$CMDLINE" 2>/dev/null; then
	if [ -d $PFSD ]; then
		say "  /proc/cmdline       sched_pfs=1"
		say "  $PFSD  exists, so the class registered"
		w=$(cat $PFSD/window_us 2>/dev/null) || w=
		[ -n "${w:-}" ] && say "  window_us           $w"
		verdict kPFS "  watch: cat $PFSD/stats, groups, waiters"
	fi
	# Asked for, not confirmed. Distinguish "no such directory" from "not
	# allowed to look", because only the first one means it is not running.
	if [ "$(id -u)" != 0 ]; then
		verdict kPFS-unconfirmed \
			"  /proc/cmdline       sched_pfs=1" \
			"  $PFSD  not readable as uid $(id -u); re-run as root to confirm"
	fi
	verdict cfs \
		"  /proc/cmdline       sched_pfs=1, but $PFSD does not exist" \
		"  so this kernel has no CONFIG_SCHED_PFS and ignored the parameter"
fi

# ------------------------------------------------------------- 2. scx_pfs
st=$(cat $SCX/state 2>/dev/null) || st=
if [ "${st:-}" = enabled ]; then
	ops=$(cat $SCX/root/ops 2>/dev/null) || ops=
	say "  $SCX/state  enabled"
	say "  scheduler           ${ops:-unknown}"
	rej=$(cat $SCX/root/nr_rejected 2>/dev/null) || rej=
	[ -n "${rej:-}" ] && say "  nr_rejected         $rej"
	say "  note: cpu.stat throttling fields stay ZERO under a BPF scheduler;"
	say "        read the loader's counters (-i, and -S FILE per cgroup) instead"
	case "${ops:-}" in
	pfs|scx_pfs) verdict scx_pfs ;;
	*) verdict "sched_ext:${ops:-unknown}" \
		"  this is a sched_ext scheduler, but not scx_pfs" ;;
	esac
fi

# -------------------------------------------------------------- 3. kwidth
if [ -r $KW ]; then
	w=$(cat $KW 2>/dev/null) || w=
	if [ "${w:-}" = 1 ]; then
		say "  $KW  1"
		verdict kwidth "  watch: cpu.stat in the cgroup, as for cfs"
	fi
	say "  $KW  ${w:-unreadable}"
else
	say "  $KW  absent, so this kernel has no width mode"
fi

# ----------------------------------------------------------------- 4. cfs
say "  no variant selected, so the kernel default is enforcing cpu.max"
if [ -r /sys/fs/cgroup/cgroup.controllers ] &&
   grep -qw cpu /sys/fs/cgroup/cgroup.controllers 2>/dev/null; then
	say "  cgroup v2 cpu controller present"
else
	say "  WARNING: no cgroup v2 cpu controller here, so cpu.max may not exist at all"
fi
verdict cfs "  watch: cpu.stat -> nr_throttled, throttled_usec"
