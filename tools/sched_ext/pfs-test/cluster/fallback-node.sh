#!/bin/bash
# Reboot node B into the GRUB saved default (7.0.0-31-generic); run under the node B lock.
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
OUT=$S/results2/kernel-node; NODE="${NODE_B:-nodeB}"; CTX="${CTX:?set CTX to the kubectl context}"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 ${SSH_B:?set SSH_B to user@host of node B}"
log() { echo "$(date -u +%FT%TZ) $*" | tee -a $OUT/fallback.log; }
log "lock acquired; $($SSH 'uname -r; sudo grub-editenv list' | tr '\n' ' ')"
$SSH "sudo pkill -INT scx_width; sudo sysctl -q kernel.sched_cfs_bandwidth_width=0; sudo systemctl reboot" || true
t0=$(date +%s)
until [ "$($SSH uname -r 2>/dev/null)" = 7.0.0-31-generic ]; do
	[ $(( $(date +%s) - t0 )) -gt 900 ] && { log "FAIL: not back on 7.0.0-31 in 15 min"; exit 2; }
	sleep 15
done
log "back on 7.0.0-31 after $(( $(date +%s) - t0 )) s"
for i in $(seq 60); do
	nr=$(kubectl --context $CTX get pods -A --field-selector spec.nodeName=$NODE \
		-o jsonpath='{range .items[*]}{.metadata.name} {.status.conditions[?(@.type=="Ready")].status}{"\n"}{end}' | grep -v ' True$')
	[ -z "$nr" ] && break; sleep 10
done
kubectl --context $CTX get pods -A -o wide --field-selector spec.nodeName=$NODE | tee -a $OUT/fallback.log
[ -n "$nr" ] && log "WARN not Ready: $nr" || log "all node B pods Ready"
