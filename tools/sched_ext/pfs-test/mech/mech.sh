#!/bin/bash
# mech.sh [OUTDIR] : phases 0 and 1 of the mechanism investigation, two boots.
#
#   m1-kpfs   $PFS_IMG sched_pfs=1   $ARMS_M1   Q2: why only 3.23-3.37 of 4 CPUs at 16 clients
#   m2-f1     $PFS_IMG               $ARMS_M2   Q1: equal CPU, 0.58-0.73 of the work
#
# Both boots run limits 4, 4.001, 2, 2.001 x {tpcb, select} at 16 clients. The 4/4.001 pair is
# the width experiment: both schedulers ceil the limit to a concurrency cap, so 4001 mcpu buys a
# fifth slot for 0.025% more quota. The 2/2.001 pair is the control, in a cell where kPFS already
# delivers its limit (1.987-2.000), so the prediction there is "no change".
#
# Same gate as run-calib.sh: no other guest, then an unbounded wait for the 1-minute load below
# 6.0, and the boot is SKIPPED after GATE_MAX rather than measured.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
O=${1:-$S/fix3/mech/res-mech}; mkdir -p $O
MEM=${MEM:-4G}
GATE_MAX=${GATE_MAX:-3600}
# RTAG keeps the per-round boots apart in one outdir; LIMITS is baked into the generated guest
# script because the host environment does not reach the guest through vng --exec.
RTAG=${RTAG:-}
# Which arms each family runs. Defaulted to what the mechanism phase has always run, so an
# unset environment is unchanged. ARMS_M2="cfs scx3 scx3b" (or "scx3 scx3b") is the WITHIN-boot
# way to compare two scx_pfs builds; point PFS_SCXBIN_scx3 and PFS_SCXBIN_scx3b at them.
ARMS_M1=${ARMS_M1:-kpfs}
ARMS_M2=${ARMS_M2:-"cfs scx3"}
LIMITS=${LIMITS:-"4 4.001 2 2.001"}
# IDENTITY of what is being measured, parameterised and READ rather than asserted. The defaults
# are the paths this harness has always used, so an existing deployment is unaffected; set the
# variables to measure a different build without filing its rows under a directory named for
# another one.
#   PFS_IMG          kernel build directory holding arch/x86/boot/bzImage
#   PFS_BINDIR       directory holding the scx_pfs binaries
#   PFS_SCXBIN       the scx_pfs binary every scx arm uses
#   PFS_SCXBIN_<arm> that one arm's binary instead, e.g. PFS_SCXBIN_scx3
IMG="${PFS_IMG:-$S/fix3/b-pkgkpfs8}"
BINDIR="${PFS_BINDIR:-$S/fix3/hp/bin}"
SCXBIN="${PFS_SCXBIN:-$BINDIR/cand-charge}"
# mech-guest.sh takes the binary from BIN in the environment and bakes no path in, so an arm can
# be pointed at its own build. Two scx builds in ONE boot additionally need a second arm name,
# which mech-guest.sh does not accept yet - it aborts on anything but cfs, scx3 and kpfs.
armbin() { local v=PFS_SCXBIN_$1; echo "${!v:-$SCXBIN}"; }
# The kernel release is read, never claimed. include/config/kernel.release is what kbuild writes
# for the tree that produced the bzImage (Makefile:1410-1412, part of archprepare at :1429);
# file(1) on the image is the fallback. If neither can be read the field says unknown, because an
# omitted field reads as "not checked".
krelease() {
	local f=$1/include/config/kernel.release
	[ -r "$f" ] && { printf '%s kernel.release\n' "$(tr -d '\n' < "$f")"; return; }
	if command -v file > /dev/null 2>&1; then
		local v; v=$(file -L "$1/arch/x86/boot/bzImage" 2>/dev/null | sed -n 's/.*version \([^ ]*\).*/\1/p')
		[ -n "$v" ] && { printf '%s file\n' "$v"; return; }
	fi
	printf 'unknown none\n'; }
# provof IMAGE -> the identity of that image and of the scx_pfs binary, all measured.
provof() {
	local rel src; read -r rel src <<< "$(krelease "$1")"
	printf 'img=%s release=%s release_src=%s bzimage_sha=%s scxbin=%s scxbin_sha=%s' \
		"$1" "$rel" "$src" "$(sha256sum "$1/arch/x86/boot/bzImage" | cut -c1-16)" \
		"$SCXBIN" "$(sha256sum "$SCXBIN" | cut -c1-16)"; }
echo "OUT=$O mem=$MEM gate_max=${GATE_MAX}s rtag=${RTAG:-none} limits=$LIMITS arms_m1=\"$ARMS_M1\" arms_m2=\"$ARMS_M2\""
[ -f "$IMG/arch/x86/boot/bzImage" ] || { echo "ABORT: no bzImage at $IMG"; echo MECH-DONE; exit 1; }
[ -x "$SCXBIN" ] || { echo "ABORT: no scx_pfs binary at $SCXBIN"; echo MECH-DONE; exit 1; }
PROV="$(provof "$IMG")"
echo "guard: $PROV"

boot() { # NAME KARGS ARMS...
	local name=$1 kargs=$2; shift 2
	while [ "$(ps -eo comm | grep -c qemu-system)" -ne 0 ]; do sleep 30; done
	local waited=0
	until [ "$(awk '{print ($1 < 6.0)}' /proc/loadavg)" = 1 ]; do
		sleep 20; waited=$((waited + 20))
		if [ $waited -ge $GATE_MAX ]; then
			printf '%-14s SKIPPED: host busy, 1-min load %s held >= 6.0 for %ss  %s\n' \
				"$name" "$(cut -d' ' -f1 /proc/loadavg)" "$waited" "$(date -u +%T)"
			return 0
		fi
	done
	local l_before; l_before=$(cut -d' ' -f1-3 /proc/loadavg)
	: > $O/$name.guest.sh
	# The warm-up stays: it is the same 30 s discarded cell the calibration used, and position-1
	# cells are compared against position-2 cells here as everywhere else.
	# PFS_WORK is baked in for the same reason LIMITS is: the host environment does not reach the
	# guest through vng --exec, and every guest-side script requires it.
	printf 'env PFS_WORK=%s bash %s/fix3/calib/warmup-guest.sh %s/%s-warmup\n' \
		"$S" "$S" "$O" "$name" >> $O/$name.guest.sh
	for a in "$@"; do
		printf 'env PFS_WORK=%s BIN=%s LIMITS=%s bash %s/fix3/mech/mech-guest.sh %s %s/%s-%s\n' \
			"$S" "$(armbin $a)" "'$LIMITS'" "$S" "$a" "$O" "$name" "$a" >> $O/$name.guest.sh
	done
	cat > $O/$name.run.sh <<EOG
cd $IMG
exec vng --run $IMG --user root --cpus 8 --memory $MEM --rwdir=$S --append "$kargs" --exec "bash $O/$name.guest.sh"
EOG
	# 2400 s, not 1800: eight cells per arm against the calibration's four, and m2 runs two arms.
	timeout -k 10 2400 script -q -f -e -c "sg kvm -c 'bash $O/$name.run.sh'" $O/$name.console < /dev/null > /dev/null 2>&1
	local rc=$?
	cp $O/$name.console $O/$name.txt 2>/dev/null
	# The result file carries its own provenance, so a later reader never has to trust a
	# directory name for what kernel or binary produced these rows.
	local prov="D prov boot=$name $PROV arm_bins=\"$(for a in "$@"; do printf '%s=%s ' "$a" "$(armbin $a)"; done)\""
	echo "$prov"; echo "$prov" >> $O/$name.txt
	printf '%-14s rc=%-3s arms_done=%s/warm=%s  load_before="%s" load_after="%s"  %s\n' "$name" "$rc" \
		"$(grep -c 'CALIB-ARM-DONE' $O/$name.txt 2>/dev/null)" \
		"$(grep -c 'CALIB-WARMUP-DONE' $O/$name.txt 2>/dev/null)" "$l_before" \
		"$(cut -d' ' -f1-3 /proc/loadavg)" "$(date -u +%T)"
	echo "D boot=$name load_before=\"$l_before\" load_after=\"$(cut -d' ' -f1-3 /proc/loadavg)\"" >> $O/$name.txt
}

boot "m1-kpfs$RTAG" "sched_pfs=1" $ARMS_M1
boot "m2-f1$RTAG"   ""            $ARMS_M2
echo "=== rows collected ==="
grep -ch '^R ' $O/*.txt 2>/dev/null | paste -sd+ | bc 2>/dev/null
echo MECH-DONE $(date -u +%T)
