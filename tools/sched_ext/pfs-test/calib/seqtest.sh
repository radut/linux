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
B=$S/fix3/hp/bin
O=${1:-$S/fix3/calib/res-seq}; mkdir -p $O
MEM=${MEM:-4G}
GATE_MAX=${GATE_MAX:-3600}
# Which of the three boots to run, so a lost boot can be re-run on its own.
BOOTS=${BOOTS:-"s1 s2 s3"}
I1=$S/fix3/b-pkgkpfs8
echo "OUT=$O mem=$MEM boots=$BOOTS gate_max=${GATE_MAX}s"
[ -f "$I1/arch/x86/boot/bzImage" ] || { echo "ABORT: no bzImage at $I1"; echo SEQ-DONE; exit 1; }
[ -x $B/cand-charge ] || { echo "ABORT: no scx_pfs binary at $B/cand-charge"; echo SEQ-DONE; exit 1; }
echo "guard: image present; scx_pfs = $(sha256sum $B/cand-charge | cut -c1-16) (d2370ce7e33b)"

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
			"$B/cand-charge" "$img" "$S/kbuild-kpfs6-src" "$S" "$a" "$O" "$name" "$i" "$a" >> $O/$name.guest.sh
		i=$((i + 1))
	done
	cat > $O/$name.run.sh <<EOG
cd $img
exec vng --run $img --user root --cpus 8 --memory $MEM --rwdir=$S --append "" --exec "bash $O/$name.guest.sh"
EOG
	timeout -k 10 1800 script -q -f -e -c "sg kvm -c 'bash $O/$name.run.sh'" $O/$name.console < /dev/null > /dev/null 2>&1
	local rc=$?
	cp $O/$name.console $O/$name.txt 2>/dev/null
	printf '%-16s rc=%-3s arms_done=%s warm=%s  load_before="%s" load_after="%s"  %s\n' "$name" "$rc" \
		"$(grep -c 'CALIB-ARM-DONE' $O/$name.txt 2>/dev/null)" \
		"$(grep -c 'CALIB-WARMUP-DONE' $O/$name.txt 2>/dev/null)" "$l_before" \
		"$(cut -d' ' -f1-3 /proc/loadavg)" "$(date -u +%T)"
	# Into the boot file as well, not only into run.log: exclude.py reads the .txt, and without
	# this line rule (B) had no load to test and silently passed s2 at loadavg 24.40.
	echo "D boot=$name load_before=\"$l_before\" load_after=\"$(cut -d' ' -f1-3 /proc/loadavg)\"" >> $O/$name.txt
}

case " $BOOTS " in *" s1 "*) boot "s1-cfs-warm"   $I1 1 cfs  cfs  ;; esac
case " $BOOTS " in *" s2 "*) boot "s2-scx3-warm"  $I1 1 scx3 scx3 ;; esac
case " $BOOTS " in *" s3 "*) boot "s3-cfs-nowarm" $I1 0 cfs  cfs  ;; esac
echo "=== rows collected ==="
grep -ch '^R ' $O/*.txt 2>/dev/null | paste -sd+ | bc 2>/dev/null
echo SEQ-DONE $(date -u +%T)
