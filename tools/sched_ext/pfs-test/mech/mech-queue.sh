#!/bin/bash
# mech-queue.sh : run phases 0 and 1 after the rerun queue, one guest at a time.
#
# Gated on rerun-queue.sh's own completion line rather than on "no qemu", because no-qemu is true
# in the gaps between its three boots and anything launched then would slip a second guest in
# alongside. mech.sh's gate handles the host load from there.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
L=$S/fix3/calib/res-seq2/queue.log
while ! grep -q '^=== rerun queue finished' $L 2>/dev/null; do sleep 60; done
exec bash $S/fix3/mech/mech.sh $S/fix3/mech/res-mech
