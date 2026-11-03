#!/bin/bash
# tail-queue.sh : what runs after rounds 3-8, in order, one guest at a time.
#   1. round 9  - odd parity (cfs first), the round the user's balanced order calls for.
#   2. s1-s3    - the sequence test, diagnostic, so it lands before the last data round.
#   3. round 10 - even parity (scx3 first). Needed because the exclusion rule drops r2-f1, which
#                 leaves f1 at 4 odd-parity boots against 3 even-parity ones. Only its f1 boot is
#                 required; its f2 and f3 boots simply add n and are kept if the rule passes them.
# Chained rather than parallel: the gate inside run-calib.sh only requires "no other qemu", which
# is true in the gaps between rounds, so anything launched now would slip a guest in alongside.
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
L=$S/fix3/calib/res-r4b/run.log
# run.log holds one CALIB-DONE from rounds 1-2; the second marks rounds 3-8 complete.
while [ "$(grep -c '^CALIB-DONE' $L)" -lt 2 ]; do sleep 120; done
bash $S/fix3/calib/run-calib.sh 1 $S/fix3/calib/res-r4b 9
bash $S/fix3/calib/seqtest.sh $S/fix3/calib/res-seq
exec bash $S/fix3/calib/run-calib.sh 1 $S/fix3/calib/res-r4b 10
