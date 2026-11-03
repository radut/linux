#!/bin/bash
# seq-queue.sh : hold the sequence test until the rounds-3-8 run is finished, then run it.
# run.log already holds one CALIB-DONE (rounds 1-2), so the second one marks rounds 3-8 complete.
# Queued rather than run alongside: two guests at once would disturb the measurement.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
L=$S/fix3/calib/res-r4b/run.log
while [ "$(grep -c '^CALIB-DONE' $L)" -lt 2 ]; do sleep 120; done
exec bash $S/fix3/calib/seqtest.sh $S/fix3/calib/res-seq
