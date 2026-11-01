#!/bin/bash
# mech.sh [OUTDIR] : phases 0 and 1 of the mechanism investigation, two boots.
#
#   m1-kpfs   b-pkgkpfs8 sched_pfs=1   kpfs        Q2: why only 3.23-3.37 of 4 CPUs at 16 clients
#   m2-f1     b-pkgkpfs8               cfs scx3    Q1: equal CPU, 0.58-0.73 of the work
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
B=$S/fix3/hp/bin
O=${1:-$S/fix3/mech/res-mech}; mkdir -p $O
MEM=${MEM:-4G}
GATE_MAX=${GATE_MAX:-3600}
I1=$S/fix3/b-pkgkpfs8
echo "OUT=$O mem=$MEM gate_max=${GATE_MAX}s"
[ -f "$I1/arch/x86/boot/bzImage" ] || { echo "ABORT: no bzImage at $I1"; echo MECH-DONE; exit 1; }
[ -x $B/cand-charge ] || { echo "ABORT: no scx_pfs binary at $B/cand-charge"; echo MECH-DONE; exit 1; }
echo "guard: image present; scx_pfs = $(sha256sum $B/cand-charge | cut -c1-16) (d2370ce7e33b)"

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
	printf 'bash %s/fix3/calib/warmup-guest.sh %s/%s-warmup\n' "$S" "$O" "$name" >> $O/$name.guest.sh
	for a in "$@"; do
		printf 'env BIN=%s bash %s/fix3/mech/mech-guest.sh %s %s/%s-%s\n' \
			"$B/cand-charge" "$S" "$a" "$O" "$name" "$a" >> $O/$name.guest.sh
	done
	cat > $O/$name.run.sh <<EOG
cd $I1
exec vng --run $I1 --user root --cpus 8 --memory $MEM --rwdir=$S --append "$kargs" --exec "bash $O/$name.guest.sh"
EOG
	# 2400 s, not 1800: eight cells per arm against the calibration's four, and m2 runs two arms.
	timeout -k 10 2400 script -q -f -e -c "sg kvm -c 'bash $O/$name.run.sh'" $O/$name.console < /dev/null > /dev/null 2>&1
	local rc=$?
	cp $O/$name.console $O/$name.txt 2>/dev/null
	printf '%-14s rc=%-3s arms_done=%s/warm=%s  load_before="%s" load_after="%s"  %s\n' "$name" "$rc" \
		"$(grep -c 'CALIB-ARM-DONE' $O/$name.txt 2>/dev/null)" \
		"$(grep -c 'CALIB-WARMUP-DONE' $O/$name.txt 2>/dev/null)" "$l_before" \
		"$(cut -d' ' -f1-3 /proc/loadavg)" "$(date -u +%T)"
	echo "D boot=$name load_before=\"$l_before\" load_after=\"$(cut -d' ' -f1-3 /proc/loadavg)\"" >> $O/$name.txt
}

boot "m1-kpfs" "sched_pfs=1" kpfs
boot "m2-f1"   ""            cfs scx3
echo "=== rows collected ==="
grep -ch '^R ' $O/*.txt 2>/dev/null | paste -sd+ | bc 2>/dev/null
echo MECH-DONE $(date -u +%T)
