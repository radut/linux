#!/bin/bash
# foreign.sh: every 30 s, log qemu processes not started by rm2 (other sessions' VMs) with the current rm2 boot
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
while pgrep -f rm2/loop.sh > /dev/null || pgrep -f rm2/loop2.sh > /dev/null; do
	n=0; for p in $(pgrep -f qemu-system); do
		c=$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)
		case "$c" in *kpfs/build-perf*|*fixscx2/build-kw*|*rm2/b-*|*kpfs/build-r5*|*remeasure/sysvm*|*rm2/sysvm*) ;; *) n=$((n+1)); f=$(echo "$c" | grep -oE 'scratchpad/[a-z0-9_-]+/[a-z0-9_-]+' | head -1) ;; esac
	done
	cur=$(ls -t $S/rm2/res/*.console $S/rm2/resx/*.console 2>/dev/null | head -1)
	[ $n -gt 0 ] && echo "$(date -u +%FT%TZ) foreign=$n $f during=$(basename ${cur:-none} .console) load=$(cut -d' ' -f1 /proc/loadavg)"
	sleep 30
done >> $S/rm2/res/foreign.log
