#!/bin/bash
# makeup-queue.sh : after phase 2, make up any reversed-order boot the gate skipped.
#
# res-mechrev's driver predates the requeue rule and could not be changed while running, so its
# skips are collected here instead and re-run at the very end of the chain, exactly as asked.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
P2=$S/fix3/mech/res-mech2/queue.log
RV=$S/fix3/mech/res-mechrev/queue.log
export GATE_MAX=3600 ATTEMPTS=3
echo "waiting for PHASE2-DONE in $P2  $(date -u +%T)"
while ! grep -q '^PHASE2-DONE' $P2 2>/dev/null; do sleep 60; done
# A boot that skipped and never later reported rc= is one with no data.
skipped=$(awk '/SKIPPED/ {s[$1]=1} /rc=/ {delete s[$1]} END {for (k in s) print k}' $RV | sort | tr '\n' ' ')
echo "reversed-order boots with no data: ${skipped:-none}  $(date -u +%T)"
bash $S/fix3/mech/mech-make.sh $skipped
