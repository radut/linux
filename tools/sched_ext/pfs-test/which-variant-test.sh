#!/bin/bash
# Exercises every branch of which-variant.sh against fixtures, including the two PRECEDENCE cases
# that no single machine can show: a kernel where the width sysctl reads 1 while something else is
# actually running the tasks. Those are the cases where every file reads exactly as it should and
# the answer is still wrong if the checks are made in the wrong order, so they are the ones worth
# having a test for.
#
# which-variant.sh takes its four paths from the environment for exactly this reason, and for no
# other: nothing in normal use overrides them.
#
# Needs no root, no kernel features and no network. Exits non-zero on the first wrong answer.
set -uo pipefail
cd "$(dirname "$0")" || exit 1
WV=./which-variant.sh
[ -x $WV ] || { echo "FAIL: $WV not executable"; exit 1; }

T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/pfsd" "$T/scx/root" "$T/scx_other/root"
echo 1000 > "$T/pfsd/window_us"
printf 'enabled\n'  > "$T/scx/state";       printf 'scx_pfs\n' > "$T/scx/root/ops"
printf 'enabled\n'  > "$T/scx_other/state"; printf 'simple\n'  > "$T/scx_other/root/ops"
printf 'disabled\n' > "$T/scx_off_state"
mkdir -p "$T/scx_off"; printf 'disabled\n' > "$T/scx_off/state"
printf '1\n' > "$T/kw_on"; printf '0\n' > "$T/kw_off"
printf 'BOOT_IMAGE=/vmlinuz sched_pfs=1 ro quiet\n' > "$T/cmd_pfs"
printf 'BOOT_IMAGE=/vmlinuz ro quiet\n'             > "$T/cmd_plain"

pass=0; fail=0
# check NAME EXPECTED  PFSD SCX KW CMDLINE
check() {
	local name="$1" want="$2" pfsd="$3" scx="$4" kw="$5" cmd="$6" got
	got=$(WV_PFSD="$pfsd" WV_SCX="$scx" WV_KW="$kw" WV_CMDLINE="$cmd" $WV -q 2>/dev/null | tail -1)
	if [ "$got" = "$want" ]; then
		printf 'ok    %-34s -> %s\n' "$name" "$got"; pass=$((pass+1))
	else
		printf 'FAIL  %-34s -> got %-22s want %s\n' "$name" "${got:-<nothing>}" "$want"; fail=$((fail+1))
	fi
}

N=$T/none   # a path that does not exist

# --- the four ordinary answers ------------------------------------------------
check "kPFS, class registered"      kPFS             "$T/pfsd" "$N"     "$T/kw_off" "$T/cmd_pfs"
check "scx_pfs loaded"              scx_pfs          "$N"      "$T/scx" "$T/kw_off" "$T/cmd_plain"
check "kwidth sysctl on"            kwidth           "$N"      "$N"     "$T/kw_on"  "$T/cmd_plain"
check "stock cfs, nothing selected" cfs              "$N"      "$N"     "$T/kw_off" "$T/cmd_plain"

# --- the two answers it REFUSES to give --------------------------------------
# sched_pfs=1 on the boot line with debugfs absent is NOT kPFS: a kernel built without
# CONFIG_SCHED_PFS accepts the parameter and ignores it. Reporting kPFS here would be a
# confident wrong answer, which is worse than no answer.
#
# But WHICH non-answer is right depends on who is asking, and this is the whole point of the
# branch. A non-root caller cannot tell "the directory is not there" from "I am not allowed to
# look", and those mean opposite things, so it gets kPFS-unconfirmed. Only uid 0, for whom the
# absence is conclusive, gets the flat cfs. An earlier version of this test expected cfs at any
# uid; the script was right and the expectation was wrong.
if [ "$(id -u)" = 0 ]; then
	check "sched_pfs=1, no class (root)"  cfs                "$N" "$N" "$T/kw_off" "$T/cmd_pfs"
else
	check "sched_pfs=1, no class (non-root)" kPFS-unconfirmed "$N" "$N" "$T/kw_off" "$T/cmd_pfs"
	echo "note  the root-only 'cfs' form of that branch is not exercised at uid $(id -u)"
fi
# And a sched_ext scheduler that is not ours must not be reported as ours.
check "sched_ext, a different one"  sched_ext:simple "$N" "$T/scx_other" "$T/kw_off" "$T/cmd_plain"

# --- PRECEDENCE: the width sysctl reads 1 but governs nothing -----------------
# Under either kPFS or a BPF scheduler the fair class is not what runs the tasks, so the sysctl
# that steers the fair class is live-looking and irrelevant. A checker that tested the cheap
# sysctl first would answer "kwidth" in both of these, with every file reading correctly.
check "width=1 under kPFS"          kPFS             "$T/pfsd" "$N"     "$T/kw_on"  "$T/cmd_pfs"
check "width=1 under sched_ext"     scx_pfs          "$N"      "$T/scx" "$T/kw_on"  "$T/cmd_plain"
# sched_ext present but DISABLED must fall through, not match on the file existing.
check "sched_ext off, width=1"      kwidth           "$N" "$T/scx_off" "$T/kw_on"  "$T/cmd_plain"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
