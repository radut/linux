#!/bin/bash
# mech-rev.sh : the phase 0+1 boots again with the cell order REVERSED, three rounds.
#
# Why: res-mech ran one boot per family in a fixed cell order 4, 4.001, 2, 2.001, so every
# "4.001 beats 4" comparison also compares an earlier cell with a later one. The CFS arm measures
# that confound directly - it has no width at all, yet its own 4 -> 4.001 step gained 10.9% on
# tpcb and 2.1% on select, while its 2 -> 2.001 step was flat (-0.6%, -0.3%). So the order effect
# is real and concentrated in the first pair.
#
# Reversing the order puts 4.001 BEFORE 4. If the width effect survives with the same sign in
# this order, it is not the order; if it inverts, it was.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
O=$S/fix3/mech/res-mechrev; mkdir -p $O
export GATE_MAX=3600
for r in 1 2 3; do
	echo "=== reversed round $r ==="
	RTAG="-r$r" LIMITS="2.001 2 4.001 4" bash $S/fix3/mech/mech.sh $O
done
echo "=== reversed rounds finished ==="
grep -ch '^R ' $O/*.txt 2>/dev/null | paste -sd+ | bc 2>/dev/null
echo MECHREV-DONE $(date -u +%T)
