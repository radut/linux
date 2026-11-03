#!/bin/bash
# Run mech rounds on one node, alternating cell order each round, and ABORT the queue the moment a
# round fails to produce rows -- so a broken configuration costs one round, not a night.
#
# Counting the rows is the thing that must not fail silently, so it is done without a glob:
# mech.sh writes "<boot><RTAG>.txt" (e.g. m1-kpfs-r2.txt), and the "-r2-<arm>" siblings are
# DIRECTORIES. An earlier "*-r$r-*.txt" glob matched neither, fed bc an empty string, and aborted
# three good rounds. "grep -c" also PRINTS 0 and EXITS 1 on no-match, so "|| echo 0" used to append
# a second 0 and make the integer test error out; every count below ends in "|| true" instead.
set -uo pipefail
IP="${NODE_IP:?}"; NODE="${NODE:?}"; K="${SSH_KEY:?}"; W=/home/ubuntu/pfs
OUT="${OUT:?}"; ROUNDS="${ROUNDS:-3}"; START="${START:-1}"
FWD="${FWD:?forward limit order}"; REV="${REV:?reversed limit order}"
ARMS_M2_FWD="${ARMS_M2_FWD:-cfs scx3}"; ARMS_M2_REV="${ARMS_M2_REV:-$ARMS_M2_FWD}"
EXTRA="${EXTRA:-}"
SSH="ssh -i $K -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o StrictHostKeyChecking=accept-new ubuntu@$IP"
log(){ echo "$(date -u +%FT%TZ) [$NODE] $*"; }

for r in $(seq "$START" "$ROUNDS"); do
	if [ $((r % 2)) -eq 1 ]; then lim="$FWD"; arms="$ARMS_M2_FWD"; ord=fwd
	else                          lim="$REV"; arms="$ARMS_M2_REV"; ord=rev; fi
	# Refuse to append to a round that already ran: contaminated .txt files cost a whole round once.
	pre=$($SSH "ls $W/fix3/mech/$OUT/*-r$r.txt 2>/dev/null | wc -l" || true)
	if [ "${pre:-0}" -gt 0 ]; then
		log "ABORTING QUEUE: $OUT already holds $pre *-r$r.txt files; archive or pick another START"
		exit 1
	fi
	log "round $r/$ROUNDS order=$ord limits='$lim' arms_m2='$arms'"
	$SSH "mkdir -p $W/fix3/mech/$OUT && cd $W/fix3/mech && \
	  env PFS_WORK=$W PFS_IMG=$W/fix3/b-kpfs10 PFS_BINDIR=$W/fix3/hp/bin \
	      PFS_SCXBIN=$W/fix3/hp/bin/base MEM=8G RTAG='-r$r' LIMITS='$lim' ARMS_M2='$arms' $EXTRA \
	      bash $W/fix3/mech/mech.sh $W/fix3/mech/$OUT >> $W/fix3/mech/$OUT/round-r$r.log 2>&1"
	rc=$?
	rows=$($SSH "cat $W/fix3/mech/$OUT/*-r$r.txt 2>/dev/null | grep -c '^R ' || true")
	bad=$($SSH "grep -c 'rc=[1-9]' $W/fix3/mech/$OUT/round-r$r.log 2>/dev/null || true")
	done_n=$($SSH "grep -c 'MECH-DONE' $W/fix3/mech/$OUT/round-r$r.log 2>/dev/null || true")
	log "round $r finished rc=$rc rows=${rows:-?} failed_boots=${bad:-?} mech_done=${done_n:-?}"
	if [ "${rows:-0}" -lt 1 ] || [ "${bad:-1}" -gt 0 ] || [ "${done_n:-0}" -lt 1 ] || [ "$rc" -ne 0 ]; then
		log "ABORTING QUEUE: round $r rows=${rows:-?} failed_boots=${bad:-?} mech_done=${done_n:-?} rc=$rc"
		$SSH "tail -20 $W/fix3/mech/$OUT/round-r$r.log" 2>&1 | sed 's/^/    /'
		exit 1
	fi
done
log "QUEUE DONE: rounds $START..$ROUNDS in $OUT"
