#!/bin/bash
# seqtest.sh [OUTDIR] : does the boot's second run gain on the first with the ARM HELD CONSTANT?
#
# The rotation in run-calib.sh changes the arm and the position together, so it cannot separate
# them. Here the arm is the same in both positions of a boot, so any gain is sequence alone.
#
#   b1  cfs  cfs    warm-up on   - the arm that showed 1.74-2.08x under rotation
#   b2  scx3 scx3   warm-up on   - the arm that read 1.01x, to check the effect is CFS-only
#   b3  cfs  cfs    warm-up OFF  - res-main (no warm-up) had a CLEAN position-1 CFS at limit 4
#                                  (3.82-3.98 CPUs, position ratio 0.98-1.07) while res-r4b (30 s
#                                  warm-up) has 2.80-2.90 CPUs and 1.89-2.62. That points at the
#                                  warm-up, but both broken boots are round 1 of res-r4b, the
#                                  highest-load round, so the two are confounded. This boot holds
#                                  the arm and the position fixed and varies only the warm-up.
#
# Same cells as the calibration: calib-guest.sh is called unchanged.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
O=${1:-$S/fix3/calib/res-seq}; mkdir -p $O
MEM=${MEM:-4G}
GATE_MAX=${GATE_MAX:-3600}
# Which of the three boots to run, so a lost boot can be re-run on its own.
BOOTS=${BOOTS:-"s1 s2 s3"}
# IDENTITY of what is being measured, parameterised and READ rather than asserted. The defaults
# are the paths this harness has always used, so an existing deployment is unaffected; set the
# variables to measure a different build without filing its rows under a directory named for
# another one. There is no hardcoded commit anywhere: asserting a commit that is not the one
# under test silently mislabels every row a run produces.
#   PFS_IMG          kernel build directory holding arch/x86/boot/bzImage
#   PFS_BINDIR       directory holding the scx_pfs binaries
#   PFS_SCXBIN       the scx_pfs binary every scx arm loads
#   PFS_SCXBIN_<arm> that one arm's binary instead, e.g. PFS_SCXBIN_scx3
IMG="${PFS_IMG:-$S/fix3/b-pkgkpfs8}"
BINDIR="${PFS_BINDIR:-$S/fix3/hp/bin}"
SCXBIN="${PFS_SCXBIN:-$BINDIR/cand-charge}"
# mech-guest.sh / calib-guest.sh take the binary from BIN in the environment and bake no path in,
# so an arm can be pointed at its own build.
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
echo "OUT=$O mem=$MEM boots=$BOOTS gate_max=${GATE_MAX}s"
[ -f "$IMG/arch/x86/boot/bzImage" ] || { echo "ABORT: no bzImage at $IMG"; echo SEQ-DONE; exit 1; }
[ -x "$SCXBIN" ] || { echo "ABORT: no scx_pfs binary at $SCXBIN"; echo SEQ-DONE; exit 1; }
echo "guard: $(provof "$IMG")"

boot() { # NAME IMAGE WARMUP ARM1 ARM2
	local name=$1 img=$2 warm=$3 a1=$4 a2=$5
	# Same gate as run-calib.sh: no other guest at all, then an unbounded wait for the 1-minute
	# load to fall below 6.0, and the boot is SKIPPED after GATE_MAX rather than measured.
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
	[ "$warm" = 1 ] && printf 'bash %s/fix3/calib/warmup-guest.sh %s/%s-warmup\n' \
		"$S" "$O" "$name" >> $O/$name.guest.sh
	# The two runs differ only in their output directory, so the same arm can run twice. The
	# 'D arm=' label is then identical in both, and run order in the file is what marks position.
	local i=1
	for a in "$a1" "$a2"; do
		printf 'env BIN=%s KB=%s KSRC=%s bash %s/fix3/calib/calib-guest.sh %s %s/%s-p%s-%s\n' \
			"$(armbin $a)" "$img" "$S/kbuild-kpfs6-src" "$S" "$a" "$O" "$name" "$i" "$a" >> $O/$name.guest.sh
		i=$((i + 1))
	done
	cat > $O/$name.run.sh <<EOG
cd $img
exec vng --run $img --user root --cpus 8 --memory $MEM --rwdir=$S --append "" --exec "bash $O/$name.guest.sh"
EOG
	timeout -k 10 1800 script -q -f -e -c "sg kvm -c 'bash $O/$name.run.sh'" $O/$name.console < /dev/null > /dev/null 2>&1
	local rc=$?
	cp $O/$name.console $O/$name.txt 2>/dev/null
	# The result file carries its own provenance, so a later reader never has to trust a
	# directory name for what kernel or binary produced these rows. $img, not a global:
	# this driver boots more than one image.
	local prov="D prov boot=$name $(provof "$img") arm_bins=\"$(for a in "$a1" "$a2"; do printf '%s=%s ' "$a" "$(armbin $a)"; done)\""
	echo "$prov"; echo "$prov" >> $O/$name.txt
	printf '%-16s rc=%-3s arms_done=%s warm=%s  load_before="%s" load_after="%s"  %s\n' "$name" "$rc" \
		"$(grep -c 'CALIB-ARM-DONE' $O/$name.txt 2>/dev/null)" \
		"$(grep -c 'CALIB-WARMUP-DONE' $O/$name.txt 2>/dev/null)" "$l_before" \
		"$(cut -d' ' -f1-3 /proc/loadavg)" "$(date -u +%T)"
	# Into the boot file as well, not only into run.log: exclude.py reads the .txt, and without
	# this line rule (B) had no load to test and silently passed s2 at loadavg 24.40.
	echo "D boot=$name load_before=\"$l_before\" load_after=\"$(cut -d' ' -f1-3 /proc/loadavg)\"" >> $O/$name.txt
}

case " $BOOTS " in *" s1 "*) boot "s1-cfs-warm"   $IMG 1 cfs  cfs  ;; esac
case " $BOOTS " in *" s2 "*) boot "s2-scx3-warm"  $IMG 1 scx3 scx3 ;; esac
case " $BOOTS " in *" s3 "*) boot "s3-cfs-nowarm" $IMG 0 cfs  cfs  ;; esac
echo "=== rows collected ==="
grep -ch '^R ' $O/*.txt 2>/dev/null | paste -sd+ | bc 2>/dev/null
echo SEQ-DONE $(date -u +%T)
