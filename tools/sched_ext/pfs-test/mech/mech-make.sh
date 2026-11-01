#!/bin/bash
# mech-make.sh BOOT [BOOT...] : re-run named res-mechrev boots that the load gate skipped.
#
# The reversed rounds run through mech.sh, which was already executing when the requeue rule
# arrived, so it could not be given one - editing a running bash script corrupts it, since bash
# reads the file incrementally. This is the make-up pass instead: the skipped boots by name, each
# retried up to ATTEMPTS times with the requeued boot going to the BACK of the queue.
#
# It calls mech-guest.sh and mech-pg.sh, NOT the mech2-* copies, so the made-up boots are measured
# by exactly the same code as the boots they are replacing. The phase 2 fixes (the v= label, the
# scx log flush, conc5.py) deliberately do not apply here: a data set measured two ways is worse
# than one measured one way with a known defect that is written down.
#
# A boot name carries its own family and round: m1-kpfs-rN is the sched_pfs=1 boot with the kpfs
# arm, m2-f1-rN the plain boot with cfs and scx3. Cell order is the reversed one.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
O=${OUTDIR:-$S/fix3/mech/res-mechrev}
MEM=${MEM:-4G}
GATE_MAX=${GATE_MAX:-3600}
ATTEMPTS=${ATTEMPTS:-3}
# Which arms each family runs. Defaulted to what the mechanism phase has always run, so an
# unset environment is unchanged. ARMS_M2="cfs scx3 scx3b" (or "scx3 scx3b") is the WITHIN-boot
# way to compare two scx_pfs builds; point PFS_SCXBIN_scx3 and PFS_SCXBIN_scx3b at them.
ARMS_M1=${ARMS_M1:-kpfs}
ARMS_M2=${ARMS_M2:-"cfs scx3"}
LIMITS=${LIMITS:-"2.001 2 4.001 4"}
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
[ $# -gt 0 ] || { echo "nothing to make up"; echo MAKEUP-DONE $(date -u +%T); exit 0; }
echo "OUT=$O mem=$MEM gate_max=${GATE_MAX}s attempts=$ATTEMPTS limits=$LIMITS boots=$*"
[ -f "$IMG/arch/x86/boot/bzImage" ] || { echo "ABORT: no bzImage at $IMG"; echo MAKEUP-DONE; exit 1; }
[ -x "$SCXBIN" ] || { echo "ABORT: no scx_pfs binary at $SCXBIN"; echo MAKEUP-DONE; exit 1; }
PROV="$(provof "$IMG")"
echo "guard: $PROV"

boot() { # NAME KARGS ARMS...
	local name=$1 kargs=$2; shift 2
	while [ "$(ps -eo comm | grep -c qemu-system)" -ne 0 ]; do sleep 30; done
	local waited=0
	until [ "$(awk '{print ($1 < 6.0)}' /proc/loadavg)" = 1 ]; do
		sleep 20; waited=$((waited + 20))
		if [ $waited -ge $GATE_MAX ]; then
			printf '%-16s SKIPPED: host busy, 1-min load %s held >= 6.0 for %ss  %s\n' \
				"$name" "$(cut -d' ' -f1 /proc/loadavg)" "$waited" "$(date -u +%T)"
			return 2
		fi
	done
	local l_before; l_before=$(cut -d' ' -f1-3 /proc/loadavg)
	: > $O/$name.guest.sh
	printf 'bash %s/fix3/calib/warmup-guest.sh %s/%s-warmup\n' "$S" "$O" "$name" >> $O/$name.guest.sh
	for a in "$@"; do
		printf 'env BIN=%s LIMITS=%s bash %s/fix3/mech/mech-guest.sh %s %s/%s-%s\n' \
			"$(armbin $a)" "'$LIMITS'" "$S" "$a" "$O" "$name" "$a" >> $O/$name.guest.sh
	done
	cat > $O/$name.run.sh <<EOG
cd $IMG
exec vng --run $IMG --user root --cpus 8 --memory $MEM --rwdir=$S --append "$kargs" --exec "bash $O/$name.guest.sh"
EOG
	timeout -k 10 2400 script -q -f -e -c "sg kvm -c 'bash $O/$name.run.sh'" $O/$name.console < /dev/null > /dev/null 2>&1
	local rc=$?
	cp $O/$name.console $O/$name.txt 2>/dev/null
	# The result file carries its own provenance, so a later reader never has to trust a
	# directory name for what kernel or binary produced these rows.
	local prov="D prov boot=$name $PROV arm_bins=\"$(for a in "$@"; do printf '%s=%s ' "$a" "$(armbin $a)"; done)\""
	echo "$prov"; echo "$prov" >> $O/$name.txt
	printf '%-16s rc=%-3s arms_done=%s/warm=%s  load_before="%s" load_after="%s"  %s\n' "$name" "$rc" \
		"$(grep -c 'CALIB-ARM-DONE' $O/$name.txt 2>/dev/null)" \
		"$(grep -c 'CALIB-WARMUP-DONE' $O/$name.txt 2>/dev/null)" "$l_before" \
		"$(cut -d' ' -f1-3 /proc/loadavg)" "$(date -u +%T)"
	echo "D boot=$name load_before=\"$l_before\" load_after=\"$(cut -d' ' -f1-3 /proc/loadavg)\"" >> $O/$name.txt
}

queue=("$@")
declare -A tries=()
while [ ${#queue[@]} -gt 0 ]; do
	nm=${queue[0]}; queue=("${queue[@]:1}")
	n=$(( ${tries[$nm]:-0} + 1 )); tries[$nm]=$n
	case $nm in
	m1-*)	kargs="sched_pfs=1"; arms="$ARMS_M1" ;;
	m2-*)	kargs="";            arms="$ARMS_M2" ;;
	*)	echo "SKIPPED-FINAL $nm: unknown boot name, no family to boot it as"; continue ;;
	esac
	[ $n -gt 1 ] && echo "=== retry $n/$ATTEMPTS of $nm  $(date -u +%T) ==="
	boot "$nm" "$kargs" $arms
	if [ $? -eq 2 ]; then
		if [ $n -lt $ATTEMPTS ]; then
			queue+=("$nm")
			echo "$nm requeued, attempt $n/$ATTEMPTS used, ${#queue[@]} boot(s) still queued"
		else
			printf '%-16s SKIPPED-FINAL: host busy on all %s attempts  %s\n' \
				"$nm" "$ATTEMPTS" "$(date -u +%T)"
		fi
	fi
done
echo "=== make-up rows now in $O ==="
grep -ch '^R ' $O/*.txt 2>/dev/null | paste -sd+ | bc 2>/dev/null
echo MAKEUP-DONE $(date -u +%T)
