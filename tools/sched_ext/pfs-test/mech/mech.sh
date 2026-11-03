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
# Which families to boot. "m1 m2" is what the mechanism phase has always run, so an unset
# environment is unchanged; a targeted rerun of ONE question boots only its family instead of
# paying for the other's 8 minutes. An unknown name is fatal rather than a silent no-op: a
# typo here would otherwise produce a run with no boots and a result file with no rows, which
# reads like a clean result instead of like nothing having happened.
FAMS=${FAMS:-"m1 m2"}
for f in $FAMS; do case $f in m1|m2) ;; *) echo "ABORT: FAMS names unknown family '$f' (m1, m2)"; echo MECH-DONE; exit 1 ;; esac; done
ARMS_M2=${ARMS_M2:-"cfs scx3"}
# Arm names are checked HERE, before a boot is paid for, and against the same list mech-guest.sh
# accepts. mech-guest.sh does abort on an unknown arm, but only after the guest has booted, so a
# typo in ARMS_M2 costs a boot and lands a result file holding a warm-up and nothing else.
for a in $ARMS_M1 $ARMS_M2; do
	case $a in cfs|kwidth|kpfs|scx3|scx3b) ;;
	*) echo "ABORT: unknown arm '$a' (cfs, kwidth, kpfs, scx3, scx3b)"; echo MECH-DONE; exit 1 ;; esac
done
# kwidth and kpfs are the two arms that are only valid in one family: kwidth needs the fair class
# to be what runs the group, kpfs needs sched_pfs=1. Caught here for the same reason.
case " $ARMS_M1 " in *" kwidth "*|*" cfs "*|*" scx3 "*|*" scx3b "*)
	echo "ABORT: ARMS_M1 is the sched_pfs=1 boot and takes only kpfs, not '$ARMS_M1'"; echo MECH-DONE; exit 1 ;; esac
case " $ARMS_M2 " in *" kpfs "*)
	echo "ABORT: kpfs needs a sched_pfs=1 boot, so it belongs in ARMS_M1, not ARMS_M2"; echo MECH-DONE; exit 1 ;; esac
# AT MOST TWO ARMS PER FAMILY, because the analysis keys POSITION on the arm's ordinal within the
# boot (calib/final.py read(), "pos + 1" per "D boot ... arm=" row) and the pre-registered
# two-factor fit compares exactly positions 1 and 2. With three arms and the usual forward/reverse
# rotation the outer two arms occupy positions {1,3} and the MIDDLE ONE IS ALWAYS POSITION 2, so it
# is perfectly confounded with its position and every cell is dropped for want of both positions.
# That run completes, reports its rows, and yields empty contrast tables - which read as "measured,
# no difference" rather than as "not analysable". Measured the hard way: a three-arm
# "cfs kwidth scx3" campaign produced 56 rows a round and not one contrast.
#
# Three arms are still the right thing for DELIVERY and THROTTLING figures, which are absolute per
# arm and need no position balance. ALLOW_UNBALANCED=1 permits it and says what the run is good
# for, so the choice is deliberate and recorded in the log rather than discovered afterwards.
nm2=$(set -- $ARMS_M2; echo $#)
if [ "$nm2" -gt 2 ] && [ "${ALLOW_UNBALANCED:-0}" != 1 ]; then
	echo "ABORT: ARMS_M2 has $nm2 arms ('$ARMS_M2'); the position-balanced contrast takes 2."
	echo "       With 3+ arms the middle arm is always position 2, so it cannot be separated from"
	echo "       the position effect and final.py drops every cell. Run the pairs instead:"
	echo "         ARMS_M2=\"cfs kwidth\"   then   ARMS_M2=\"kwidth scx3\"   then   ARMS_M2=\"cfs scx3\""
	echo "       Set ALLOW_UNBALANCED=1 if you want delivery and throttling figures only."
	echo MECH-DONE; exit 1
fi
[ "$nm2" -gt 2 ] && echo "NOTE: ALLOW_UNBALANCED=1 with $nm2 arms: delivery and throttling only, no arm contrast"
LIMITS=${LIMITS:-"4 4.001 2 2.001"}
# Clients per cell. 16 is what mech-guest.sh has always defaulted to, so an unset CL is unchanged.
CL=${CL:-16}
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
echo "OUT=$O mem=$MEM gate_max=${GATE_MAX}s rtag=${RTAG:-none} limits=$LIMITS cl=$CL arms_m1=\"$ARMS_M1\" arms_m2=\"$ARMS_M2\""
[ -f "$IMG/arch/x86/boot/bzImage" ] || { echo "ABORT: no bzImage at $IMG"; echo MECH-DONE; exit 1; }
[ -x "$SCXBIN" ] || { echo "ABORT: no scx_pfs binary at $SCXBIN"; echo MECH-DONE; exit 1; }
PROV="$(provof "$IMG")"
echo "guard: $PROV"

boot() { # NAME KARGS ARMS...
	local name=$1 kargs=$2; shift 2
	local nargs=$#
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
	# CL is baked in for exactly the reason LIMITS is: the host environment does not reach the guest
	# through "vng --exec", so a CL set on the host was silently dropped and every cell ran at
	# mech-guest.sh's default of 16 clients. That made the client count unmeasurable from here -
	# the "scale" dimension of the test plan - while looking like it was being set.
	for a in "$@"; do
		printf 'env PFS_WORK=%s BIN=%s LIMITS=%s CL=%s bash %s/fix3/mech/mech-guest.sh %s %s/%s-%s\n' \
			"$S" "$(armbin $a)" "'$LIMITS'" "$CL" "$S" "$a" "$O" "$name" "$a" >> $O/$name.guest.sh
	done
	cat > $O/$name.run.sh <<EOG
cd $IMG
exec vng --run $IMG --user root --cpus 8 --memory $MEM --rwdir=$S --append "$kargs" --exec "bash $O/$name.guest.sh"
EOG
	# The timeout SCALES with the arm count instead of being the constant 2400 it was. That constant
	# was sized for "eight cells per arm, m2 runs two arms", so it was correct only at two arms: a
	# third arm in one boot (cfs, kwidth and scx3 together) would be killed part-way through the
	# last one, and a killed boot loses every row it had already produced. 600 + 900 per arm
	# reproduces 2400 exactly at two arms, so every existing run is timed as before.
	local btmo=${BOOT_TIMEOUT:-$((600 + 900 * nargs))}
	timeout -k 10 "$btmo" script -q -f -e -c "sg kvm -c 'bash $O/$name.run.sh'" $O/$name.console < /dev/null > /dev/null 2>&1
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

case " $FAMS " in *" m1 "*) boot "m1-kpfs$RTAG" "sched_pfs=1" $ARMS_M1 ;; esac
case " $FAMS " in *" m2 "*) boot "m2-f1$RTAG"   ""            $ARMS_M2 ;; esac
echo "=== rows collected ==="
# Counted without a glob that can match nothing and without "grep -c", which PRINTS 0 and EXITS 1
# on no-match: the earlier "grep -ch ... | paste -sd+ | bc" fed bc an empty string and printed a
# blank line where a 0 belongs, and a blank count is indistinguishable from a count of zero.
cat $O/*$RTAG.txt 2>/dev/null | grep -c '^R ' || true
echo MECH-DONE $(date -u +%T)
