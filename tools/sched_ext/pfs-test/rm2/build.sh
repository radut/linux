#!/bin/bash
# build.sh: kPFS images from cfe50389959b (rm2/src): perf (build-r5 config), noext, rt, kcsan (remeasure x-configs); -j4
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
M=$S/rm2 SRC=$M/src
for c in ${*:-perf noext rt kcsan}; do
	O=$M/b-$c; mkdir -p $O
	case $c in perf) cp $S/kpfs/build-r5/.config $O/.config ;; *) cp $S/kpfs/build-x$c/.config $O/.config ;; esac
	make -s -C $SRC O=$O olddefconfig > $M/build-$c.log 2>&1
	grep -E '^CONFIG_(SCHED_CLASS_EXT|PREEMPT_RT|KCSAN|SCHED_PFS|SCHED_CORE|DEBUG_INFO_BTF)=' $O/.config >> $M/build-$c.log
	t0=$(date +%s); echo "=== $c start $(date -u +%T)" >> $M/build.log
	nice -n 5 make -C $SRC O=$O -j4 bzImage modules >> $M/build-$c.log 2>&1; rc=$?
	if [ $rc != 0 ] && grep -q Killed $M/build-$c.log; then
		echo "=== $c OOM-killed, relink with swap" >> $M/build.log
		sudo -n swapon /swapfile-kpfs5; nice -n 5 make -C $SRC O=$O -j1 bzImage >> $M/build-$c.log 2>&1; rc=$?; sudo -n swapoff /swapfile-kpfs5
		[ $rc = 0 ] && { nice -n 5 make -C $SRC O=$O -j2 modules >> $M/build-$c.log 2>&1; rc=$?; }
	fi
	echo "=== $c rc=$rc $(( $(date +%s) - t0 ))s warnings=$(grep -c 'warning:' $M/build-$c.log) killed=$(grep -c Killed $M/build-$c.log) swap=$(swapon --show --noheadings | wc -l)" >> $M/build.log
done
echo BUILD-DONE >> $M/build.log
