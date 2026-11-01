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
B=$S/fix3/hp/bin
O=${OUTDIR:-$S/fix3/mech/res-mechrev}
MEM=${MEM:-4G}
GATE_MAX=${GATE_MAX:-3600}
ATTEMPTS=${ATTEMPTS:-3}
LIMITS=${LIMITS:-"2.001 2 4.001 4"}
I1=$S/fix3/b-pkgkpfs8
[ $# -gt 0 ] || { echo "nothing to make up"; echo MAKEUP-DONE $(date -u +%T); exit 0; }
echo "OUT=$O mem=$MEM gate_max=${GATE_MAX}s attempts=$ATTEMPTS limits=$LIMITS boots=$*"
[ -f "$I1/arch/x86/boot/bzImage" ] || { echo "ABORT: no bzImage at $I1"; echo MAKEUP-DONE; exit 1; }
[ -x $B/cand-charge ] || { echo "ABORT: no scx_pfs binary"; echo MAKEUP-DONE; exit 1; }
echo "guard: image present; scx_pfs = $(sha256sum $B/cand-charge | cut -c1-16) (d2370ce7e33b)"

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
			"$B/cand-charge" "'$LIMITS'" "$S" "$a" "$O" "$name" "$a" >> $O/$name.guest.sh
	done
	cat > $O/$name.run.sh <<EOG
cd $I1
exec vng --run $I1 --user root --cpus 8 --memory $MEM --rwdir=$S --append "$kargs" --exec "bash $O/$name.guest.sh"
EOG
	timeout -k 10 2400 script -q -f -e -c "sg kvm -c 'bash $O/$name.run.sh'" $O/$name.console < /dev/null > /dev/null 2>&1
	local rc=$?
	cp $O/$name.console $O/$name.txt 2>/dev/null
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
	m1-*)	kargs="sched_pfs=1"; arms="kpfs" ;;
	m2-*)	kargs="";            arms="cfs scx3" ;;
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
