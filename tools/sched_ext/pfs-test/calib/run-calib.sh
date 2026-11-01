#!/bin/bash
# run-calib.sh [ROUNDS] : the calibration round, to measure the paired ratio's sigma per cell.
#
# Corrects an error in the plan: one boot per family cannot yield a sigma across boots. Sigma needs
# repeats, so this runs ROUNDS boots of each family (default 5) and writes one file per boot, so a
# run stopped early still gives a usable, smaller n.
#
# Families, and which of them can honour "baseline and variants in the same boot":
#   F1  b-pkgkpfs8, no sched_pfs   cfs + scx3     IN-BOOT pair (scx loads and unloads at run time)
#   F2  b-pkgkpfs8, sched_pfs=1    kpfs           NO in-boot CFS possible: kPFS is early_param only
#                                                 (pfs.c:59, pfs_boot_enable before any run queue),
#                                                 so its baseline is F1's cfs run of the same round -
#                                                 boot-adjacent, same session, same image.
#   F3  b-pkgkwidth4, no sched_pfs cfs + kwidth   IN-BOOT pair (the width sysctl toggles at run time)
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
B=$S/fix3/hp/bin
ROUNDS=${1:-5}
O=${2:-$S/fix3/calib/res-$(date +%H%M%S)}; FIRST=${3:-1}; mkdir -p $O
# 4G, as round 2 gave its Postgres boots: the harness loads pgbench scale 20 into a tmpfs, which
# counts against guest RAM, and 2G OOM-killed postgres and then the next arm's scx loader.
MEM=${MEM:-4G}
# How long the load gate waits before skipping a boot (see the gate in boot()).
GATE_MAX=${GATE_MAX:-3600}
# Which families to run this round. Lets a single family be re-run to fix a parity
# deficit without paying for the other two boots.
FAMS=${FAMS:-"f1 f2 f3"}
echo "OUT=$O rounds=$ROUNDS first=$FIRST mem=$MEM fams=$FAMS gate_max=${GATE_MAX}s"
I1=$S/fix3/b-pkgkpfs8; I3=$S/fix3/b-pkgkwidth4
for i in $I1 $I3; do
	[ -f "$i/arch/x86/boot/bzImage" ] || { echo "ABORT: no bzImage at $i"; echo CALIB-DONE; exit 1; }
done
[ -x $B/cand-charge ] || { echo "ABORT: no scx_pfs binary at $B/cand-charge"; echo CALIB-DONE; exit 1; }
echo "guard: images present; scx_pfs = $(sha256sum $B/cand-charge | cut -c1-16) (d2370ce7e33b)"

boot() { # NAME IMAGE KARGS ARMS...
	local name=$1 img=$2 kargs=$3; shift 3
	# The gate. Two conditions, because only one of them is about contention between guests.
	#   (1) No other qemu. The hard requirement, waited for without limit.
	#   (2) 1-minute load below 6.0. Now also waited for without limit, up to GATE_MAX seconds,
	#       after which the boot is SKIPPED rather than measured.
	# This replaces a 120 s cap that fell through and ran the boot anyway. That is how r10-f1 came
	# to be measured at load_before="5.97 15.13 19.39", while another workspace's 8-thread test run
	# held this 8-CPU host at loadavg 20-24; all three round-10 boots and two of the three sequence
	# boots were lost that way. A gate that proceeds on timeout is not a gate - the exclusion rule
	# discards the boot afterwards, so it costs 14 minutes and yields nothing. Skipping is strictly
	# better: it costs nothing and says so in the log.
	# The threshold is 6.0, not 2.0, because this host's own idle baseline is 3-5 with no VM running
	# at all - eight long-lived sessions and the exporters - which is not contention for a guest's
	# 8 vCPUs. The load measured right after a guest exits still contains it (6.3-6.6 observed),
	# but the 1-minute average decays past 6.0 within a couple of minutes, so an unbounded wait
	# costs a short settle per boot rather than stalling the run.
	# The load is still RECORDED on both sides of every boot, into run.log AND into the boot's own
	# .txt: the exclusion rule reads the .txt, and a boot file with no load line silently cannot be
	# tested at all.
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
	# One discarded pg cell first, so the boot's first measured arm is not the one that pays the
	# first-run cost. See warmup-guest.sh for what that cost was worth: 1.66-2.00x on tpcb@2 c16.
	printf 'bash %s/fix3/calib/warmup-guest.sh %s/%s-warmup\n' "$S" "$O" "$name" >> $O/$name.guest.sh
	for a in "$@"; do
		printf 'env BIN=%s KB=%s KSRC=%s bash %s/fix3/calib/calib-guest.sh %s %s/%s-%s\n' \
			"$B/cand-charge" "$img" "$S/kbuild-kpfs6-src" "$S" "$a" "$O" "$name" "$a" >> $O/$name.guest.sh
	done
	cat > $O/$name.run.sh <<EOG
cd $img
exec vng --run $img --user root --cpus 8 --memory $MEM --rwdir=$S --append "$kargs" --exec "bash $O/$name.guest.sh"
EOG
	timeout -k 10 1800 script -q -f -e -c "sg kvm -c 'bash $O/$name.run.sh'" $O/$name.console < /dev/null > /dev/null 2>&1
	local rc=$?
	cp $O/$name.console $O/$name.txt 2>/dev/null
	printf '%-14s rc=%-3s arms_done=%s  load_before="%s" load_after="%s"  %s\n' "$name" "$rc" \
		"$(grep -c 'CALIB-ARM-DONE' $O/$name.txt 2>/dev/null)/warm=$(grep -c 'CALIB-WARMUP-DONE' $O/$name.txt 2>/dev/null)" "$l_before" \
		"$(cut -d' ' -f1-3 /proc/loadavg)" "$(date -u +%T)"
	echo "D boot=$name load_before=\"$l_before\" load_after=\"$(cut -d' ' -f1-3 /proc/loadavg)\"" >> $O/$name.txt
}

for r in $(seq $FIRST $((FIRST + ROUNDS - 1))); do
	# arm order rotated per round so neither a warm cache nor a drifting host favours one arm
	if [ $((r % 2)) -eq 1 ]; then f1="cfs scx3"; f3="cfs kwidth"; else f1="scx3 cfs"; f3="kwidth cfs"; fi
	case " $FAMS " in *" f1 "*) boot "r$r-f1" $I1 ""            $f1   ;; esac
	case " $FAMS " in *" f2 "*) boot "r$r-f2" $I1 "sched_pfs=1" kpfs  ;; esac
	case " $FAMS " in *" f3 "*) boot "r$r-f3" $I3 ""            $f3   ;; esac
done
echo "=== rows collected ==="
grep -ch '^R ' $O/*.txt 2>/dev/null | paste -sd+ | bc 2>/dev/null
echo CALIB-DONE $(date -u +%T)
