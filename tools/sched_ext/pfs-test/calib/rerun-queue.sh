#!/bin/bash
# rerun-queue.sh : the three boots the exclusion rule forced, one guest at a time.
#
#   s3  cfs cfs, warm-up OFF   the only boot that varies the warm-up alone, so without it the
#                              warm-up's own benefit is unmeasured. Lost to host load at 15:37.
#   s2  scx3 scx3, warm-up on  the only boot that tests the sequence effect on an arm with no
#                              headroom. Lost at 15:22; its p2 select@2 c16 read mid_cpus=0.112.
#   r12-f3                      even parity (kwidth first), restoring f3 to 4 odd / 4 even after
#                              r8-f3 and r10-f3 were dropped. FAMS=f3 so the f1 and f2 boots of
#                              the round are not paid for; f1 is already balanced 3/3 and f2 has
#                              8 boots.
#
# Both scripts now wait for the 1-minute load to fall below 6.0 with no cap, and SKIP the boot
# after GATE_MAX rather than measuring it. A skip is logged and the chain continues.
# s2 and s3 go to a fresh res-seq2 so the kept s1 boot in res-seq is left untouched.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
C=$S/fix3/calib
export GATE_MAX=3600
echo "=== rerun queue started $(date -u +%T) ==="
BOOTS="s2 s3" bash $C/seqtest.sh $C/res-seq2
FAMS=f3 bash $C/run-calib.sh 1 $C/res-r4b 12
echo "=== rerun queue finished $(date -u +%T) ==="
