#!/bin/bash
# phase2-queue.sh : wait for the reversed-order rounds, then run phase 2 twice.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
L=$S/fix3/mech/res-mechrev/queue.log
O=$S/fix3/mech/res-mech2; mkdir -p $O
export GATE_MAX=3600
echo "waiting for MECHREV-DONE in $L  $(date -u +%T)"
while ! grep -q '^MECHREV-DONE' $L 2>/dev/null; do sleep 60; done
echo "reversed rounds finished, starting phase 2  $(date -u +%T)"
for r in 1 2; do
	echo "=== phase 2 round $r ==="
	RTAG="-r$r" bash $S/fix3/mech/mech2.sh $O
done
echo "=== phase 2 finished ==="
echo PHASE2-DONE $(date -u +%T)
