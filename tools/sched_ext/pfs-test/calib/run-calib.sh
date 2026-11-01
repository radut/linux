#!/bin/bash
# run-calib.sh [ROUNDS] : the calibration round, to measure the paired ratio's sigma per cell.
#
# Corrects an error in the plan: one boot per family cannot yield a sigma across boots. Sigma needs
# repeats, so this runs ROUNDS boots of each family (default 5) and writes one file per boot, so a
# run stopped early still gives a usable, smaller n.
#
# Families, and which of them can honour "baseline and variants in the same boot":
#   F1  $PFS_IMG, no sched_pfs     cfs + scx3     IN-BOOT pair (scx loads and unloads at run time)
#   F2  $PFS_IMG, sched_pfs=1      kpfs           NO in-boot CFS possible: kPFS is early_param only
#                                                 (pfs.c:59, pfs_boot_enable before any run queue),
#                                                 so its baseline is F1's cfs run of the same round -
#                                                 boot-adjacent, same session, same image.
#   F3  $PFS_IMG_KWIDTH, no sched_pfs  cfs + kwidth  IN-BOOT pair (the width sysctl toggles at run time)
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
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
# IDENTITY of what is being measured, parameterised and READ rather than asserted. The defaults
# are the paths this harness has always used, so an existing deployment is unaffected; set the
# variables to measure a different build without filing its rows under a directory named for
# another one. There is no hardcoded commit anywhere: asserting a commit that is not the one
# under test silently mislabels every row a run produces.
#   PFS_IMG          kernel build directory holding arch/x86/boot/bzImage
#   PFS_BINDIR       directory holding the scx_pfs binaries
#   PFS_SCXBIN       the scx_pfs binary every scx arm loads
#   PFS_SCXBIN_<arm> that one arm's binary instead, e.g. PFS_SCXBIN_scx3
# Two images here: families f1/f2 boot the PFS build, f3 the kwidth build, so each has its own
# variable and its own guard line.
#   PFS_IMG_KWIDTH   the kwidth kernel build directory, for family f3
IMG="${PFS_IMG:-$S/fix3/b-pkgkpfs8}"
IMG_KW="${PFS_IMG_KWIDTH:-$S/fix3/b-pkgkwidth4}"
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
for i in $IMG $IMG_KW; do
	[ -f "$i/arch/x86/boot/bzImage" ] || { echo "ABORT: no bzImage at $i"; echo CALIB-DONE; exit 1; }
done
[ -x "$SCXBIN" ] || { echo "ABORT: no scx_pfs binary at $SCXBIN"; echo CALIB-DONE; exit 1; }
echo "guard: role=f1f2 $(provof "$IMG")"
echo "guard: role=f3   $(provof "$IMG_KW")"

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
			"$(armbin $a)" "$img" "$S/kbuild-kpfs6-src" "$S" "$a" "$O" "$name" "$a" >> $O/$name.guest.sh
	done
	cat > $O/$name.run.sh <<EOG
cd $img
exec vng --run $img --user root --cpus 8 --memory $MEM --rwdir=$S --append "$kargs" --exec "bash $O/$name.guest.sh"
EOG
	timeout -k 10 1800 script -q -f -e -c "sg kvm -c 'bash $O/$name.run.sh'" $O/$name.console < /dev/null > /dev/null 2>&1
	local rc=$?
	cp $O/$name.console $O/$name.txt 2>/dev/null
	# The result file carries its own provenance, so a later reader never has to trust a
	# directory name for what kernel or binary produced these rows. $img, not a global:
	# this driver boots more than one image.
	local prov="D prov boot=$name $(provof "$img") arm_bins=\"$(for a in "$@"; do printf '%s=%s ' "$a" "$(armbin $a)"; done)\""
	echo "$prov"; echo "$prov" >> $O/$name.txt
	printf '%-14s rc=%-3s arms_done=%s  load_before="%s" load_after="%s"  %s\n' "$name" "$rc" \
		"$(grep -c 'CALIB-ARM-DONE' $O/$name.txt 2>/dev/null)/warm=$(grep -c 'CALIB-WARMUP-DONE' $O/$name.txt 2>/dev/null)" "$l_before" \
		"$(cut -d' ' -f1-3 /proc/loadavg)" "$(date -u +%T)"
	echo "D boot=$name load_before=\"$l_before\" load_after=\"$(cut -d' ' -f1-3 /proc/loadavg)\"" >> $O/$name.txt
}

for r in $(seq $FIRST $((FIRST + ROUNDS - 1))); do
	# arm order rotated per round so neither a warm cache nor a drifting host favours one arm
	if [ $((r % 2)) -eq 1 ]; then f1="cfs scx3"; f3="cfs kwidth"; else f1="scx3 cfs"; f3="kwidth cfs"; fi
	case " $FAMS " in *" f1 "*) boot "r$r-f1" $IMG    ""            $f1   ;; esac
	case " $FAMS " in *" f2 "*) boot "r$r-f2" $IMG    "sched_pfs=1" kpfs  ;; esac
	case " $FAMS " in *" f3 "*) boot "r$r-f3" $IMG_KW ""            $f3   ;; esac
done
echo "=== rows collected ==="
grep -ch '^R ' $O/*.txt 2>/dev/null | paste -sd+ | bc 2>/dev/null
echo CALIB-DONE $(date -u +%T)
