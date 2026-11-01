#!/bin/bash
# warmup-guest.sh OUTDIR : one DISCARDED pg cell per boot, before any measured arm runs.
#
# Why this exists. In the 5-round calibration the boot's SECOND arm read 1.66-2.00x the first arm's
# tps on tpcb@2 c16 - the first pgbench run of the boot - while every later pg cell read only
# 1.01-1.19. Rotating arm order per round turned that into 37-56% of apparent per-boot sigma, and
# any arm paired against the alternating baseline inherited it. So something primed by the first pg
# run survives the inter-arm cleanup, and only the boot's first measured cell pays for it.
#
# This pays it instead, unmeasured. Its output is dropped, and it ends with the same teardown
# calib-guest.sh runs between arms, so the first measured arm starts from the state the second one
# would otherwise have found. It runs before any arm setup, on plain CFS, in every family.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
O=${1:?outdir}; mkdir -p $O
t0=$(date +%s)
# One cell, the same shape as the first measured one (limit 2, tpcb, clients 16), shortened: this is
# about priming, not measuring. Full setup is deliberate - initdb and pgbench -i are part of what a
# first run pays for.
CL=16 bash $S/rm2/t/pg-guest.sh "2" "tpcb" 10 > $O/warmup.log 2>&1
rc=$?
for i in $(seq 20); do pgrep -x postgres > /dev/null || break; pkill -x postgres; sleep 0.5; done
pgrep -x postgres > /dev/null && pkill -9 -x postgres
sleep 1
mountpoint -q /tmp/pgd && umount -l /tmp/pgd
rmdir /sys/fs/cgroup/rt/pod/ctr /sys/fs/cgroup/rt/pod /sys/fs/cgroup/rt 2>/dev/null
echo "D warmup rc=$rc secs=$(( $(date +%s) - t0 )) tps=$(awk '/^R /{for(i=1;i<=NF;i++) if ($i ~ /^tps=/) print substr($i,5)}' $O/warmup.log | head -1) postgres=$(pgrep -xc postgres 2>/dev/null || echo 0) pgd_mounted=$(mountpoint -q /tmp/pgd && echo yes || echo no) mem_avail_mb=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)"
echo "CALIB-WARMUP-DONE"
