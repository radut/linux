#!/bin/bash
# mech2.sh [OUTDIR] : phase 2 of the mechanism investigation, three boots per round.
#
#   p1-kpfs   sched_pfs=1   kpfs x1              2b.1 the fraction sweep
#   p1b-kpfs  sched_pfs=1   kpfs x3              2b.2 the burst_pct sweep at 2 vs 2.001
#   p2-f1     (plain)       cfs, scx3            2a.1 switches and 2a.2 wake-up latency at limit 4
#
# 2b.1 sweeps 1.5 2 2.1 2.5 2.9 2.999 3. pfs_width() ceils (pfs.c:562) while every budget decision
# truncates, so the gap between them is one slot at every non-integer limit and none at 2 and 3.
# pfs_depth() (pfs.c:614) grows with limit % 1000, so if the fractional-limit loss is the token
# bucket it should shrink across 2.001 -> 2.1 -> 2.5 -> 2.9 and vanish at 3. 1.5 and 2.5 are in the
# sweep because those are the limits k8s actually writes.
#
# 2b.2 holds the limit pair at 2 vs 2.001 and moves burst_pct, which scales pfs_depth() directly:
# a deeper bucket buying the loss back is the same hypothesis tested a second way.
#
# 2a runs limit 4 only, where both arms deliver 3.99 of 4 CPUs and scx_pfs still returns 0.63-0.75
# of CFS's work. Nothing here changes any scheduler code.
#
# Same gate as mech.sh: no other guest, then an unbounded wait for the 1-minute load below 6.0,
# and the boot is SKIPPED after GATE_MAX rather than measured. The 6.0 threshold is NOT tuned:
# loosening a gate after seeing it cost a run is the post-hoc move the gate exists to prevent.
#
# A skipped boot is not lost. boot() returns 2 on a skip and the driver puts the boot back at the
# END of the queue, so every other boot of the round is measured first and the retry meets a
# different slice of host load. Each boot gets ATTEMPTS tries before it is written off, which is
# recorded as SKIPPED-FINAL so a gap in the data set is always visible in the log.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
B=$S/fix3/hp/bin
O=${1:-$S/fix3/mech/res-mech2}; mkdir -p $O
MEM=${MEM:-4G}
GATE_MAX=${GATE_MAX:-3600}
RTAG=${RTAG:-}
ATTEMPTS=${ATTEMPTS:-3}
I1=$S/fix3/b-pkgkpfs8
echo "OUT=$O mem=$MEM gate_max=${GATE_MAX}s rtag=${RTAG:-none}"
[ -f "$I1/arch/x86/boot/bzImage" ] || { echo "ABORT: no bzImage at $I1"; echo MECH2-DONE; exit 1; }
[ -x $B/cand-charge ] || { echo "ABORT: no scx_pfs binary at $B/cand-charge"; echo MECH2-DONE; exit 1; }
echo "guard: image present; scx_pfs = $(sha256sum $B/cand-charge | cut -c1-16) (d2370ce7e33b)"

# boot NAME TIMEOUT KARGS SPEC...   where SPEC is  tag|arm|LIMITS|WLS|BURST
boot() {
	local name=$1 tmo=$2 kargs=$3; shift 3
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
	local sp tag arm lims wls burst
	for sp in "$@"; do
		IFS='|' read -r tag arm lims wls burst <<< "$sp"
		printf 'env BIN=%s LIMITS=%s WLS=%s %s bash %s/fix3/mech/mech2-guest.sh %s %s/%s-%s\n' \
			"$B/cand-charge" "'$lims'" "'$wls'" "${burst:+BURST=$burst}" \
			"$S" "$arm" "$O" "$name" "$tag" >> $O/$name.guest.sh
	done
	cat > $O/$name.run.sh <<EOG
cd $I1
exec vng --run $I1 --user root --cpus 8 --memory $MEM --rwdir=$S --append "$kargs" --exec "bash $O/$name.guest.sh"
EOG
	timeout -k 10 $tmo script -q -f -e -c "sg kvm -c 'bash $O/$name.run.sh'" $O/$name.console < /dev/null > /dev/null 2>&1
	local rc=$?
	cp $O/$name.console $O/$name.txt 2>/dev/null
	printf '%-16s rc=%-3s arms_done=%s/warm=%s  load_before="%s" load_after="%s"  %s\n' "$name" "$rc" \
		"$(grep -c 'CALIB-ARM-DONE' $O/$name.txt 2>/dev/null)" \
		"$(grep -c 'CALIB-WARMUP-DONE' $O/$name.txt 2>/dev/null)" "$l_before" \
		"$(cut -d' ' -f1-3 /proc/loadavg)" "$(date -u +%T)"
	echo "D boot=$name load_before=\"$l_before\" load_after=\"$(cut -d' ' -f1-3 /proc/loadavg)\"" >> $O/$name.txt
}

# The queue. One entry per boot: NAME <TAB> TIMEOUT <TAB> KARGS <TAB> SPEC [<TAB> SPEC...], where
# a SPEC is tag|arm|LIMITS|WLS|BURST. Tabs separate the fields because KARGS and the specs contain
# spaces; nothing in either contains a tab. KARGS of "-" means an empty command line: tab is
# IFS whitespace, so `read -a` collapses two adjacent tabs into one and an empty field would
# silently shift every field after it (verified: 5 fields read back as 4).
#
#   p1   14 cells in one arm: 7 limits x 2 workloads, ~40 s each plus one initdb.
#   p1b  3 arms x 4 cells. burst_pct is a global knob so each value needs its own arm, and 100 is
#        the default, carried here too so all three sit in one boot rather than across boots.
#   p2   2 arms x 2 cells, the 2a instrumentation.
queue=()
queue+=("p1-kpfs$RTAG	2400	sched_pfs=1	sweep|kpfs|1.5 2 2.1 2.5 2.9 2.999 3|tpcb select|")
queue+=("p1b-kpfs$RTAG	3000	sched_pfs=1	b10|kpfs|2 2.001|tpcb select|10	b100|kpfs|2 2.001|tpcb select|100	b400|kpfs|2 2.001|tpcb select|400")
queue+=("p2-f1$RTAG	2400	-	a|cfs|4|tpcb select|	a|scx3|4|tpcb select|")

declare -A tries=()
while [ ${#queue[@]} -gt 0 ]; do
	entry=${queue[0]}; queue=("${queue[@]:1}")
	IFS=$'\t' read -r -a f <<< "$entry"
	nm=${f[0]}; n=$(( ${tries[$nm]:-0} + 1 )); tries[$nm]=$n
	[ ${#f[@]} -ge 4 ] || { echo "ABORT: malformed queue entry: $nm"; break; }
	ka=${f[2]}; [ "$ka" = - ] && ka=
	[ $n -gt 1 ] && echo "=== retry $n/$ATTEMPTS of $nm  $(date -u +%T) ==="
	boot "${f[0]}" "${f[1]}" "$ka" "${f[@]:3}"
	if [ $? -eq 2 ]; then
		if [ $n -lt $ATTEMPTS ]; then
			# Back of the queue, not the front: every other boot gets measured first.
			queue+=("$entry")
			echo "$nm requeued, attempt $n/$ATTEMPTS used, ${#queue[@]} boot(s) still queued"
		else
			printf '%-16s SKIPPED-FINAL: host busy on all %s attempts  %s\n' \
				"$nm" "$ATTEMPTS" "$(date -u +%T)"
		fi
	fi
done
echo "=== rows collected ==="
grep -ch '^R ' $O/*.txt 2>/dev/null | paste -sd+ | bc 2>/dev/null
echo MECH2-DONE $(date -u +%T)
