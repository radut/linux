#!/bin/bash
# Install the kwidth kernel on node B with a one-shot GRUB boot; run under the node B lock:
#   flock -w 7200 $S/nodeB.lock -c "$S/results2/kernel-node/install-nodeB.sh"
set -u
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
OUT=$S/results2/kernel-node
NODE="${NODE:-nodeB}"
IP="${NODE_IP:?set NODE_IP to the target node address}"
CTX="${CTX:?set CTX to the kubectl context}"
KREL=7.3.0-kwidth
OLD=7.0.0-31-generic
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=5 ubuntu@$IP"
log() { echo "$(date -u +%FT%TZ) $*" | tee -a $OUT/install.log; }

log "lock acquired"

# Preflight: no sched-test workloads on B, scx off.
pods=$(kubectl --context $CTX -n sched-test get pods --field-selector spec.nodeName=$NODE -o name)
if [ -n "$pods" ]; then log "ABORT: sched-test pods on $NODE: $pods"; exit 1; fi
$SSH "pgrep -a scx_width; cat /sys/kernel/sched_ext/state" | tee -a $OUT/install.log
if $SSH "pgrep scx_width >/dev/null"; then log "ABORT: scx_width running on $NODE"; exit 1; fi

# Copy packages.
$SSH "rm -rf /tmp/kwidth-debs && mkdir /tmp/kwidth-debs"
scp -q $S/linux-image-${KREL}_*.deb $S/linux-headers-${KREL}_*.deb ubuntu@$IP:/tmp/kwidth-debs/ || { log "ABORT: scp failed"; exit 1; }

# GRUB: saved default = old kernel, install, one-shot boot into the new one.
$SSH "sudo bash -s" <<EOF 2>&1 | tee -a $OUT/install.log
set -e
UUID=\$(sudo grub-probe --target=fs_uuid /)
ADV="gnulinux-advanced-\$UUID"
cat > /etc/default/grub.d/99-kwidth.cfg <<'CFG'
# kwidth test kernel: boot via saved entry; panic=10 reboots (to the saved fallback) after a panic
GRUB_DEFAULT=saved
GRUB_CMDLINE_LINUX="\$GRUB_CMDLINE_LINUX panic=10"
CFG
update-grub
grub-set-default "\$ADV>gnulinux-$OLD-advanced-\$UUID"
dpkg -i /tmp/kwidth-debs/linux-image-${KREL}_*.deb /tmp/kwidth-debs/linux-headers-${KREL}_*.deb
ls -l /boot/vmlinuz-$KREL /boot/initrd.img-$KREL
grep -q "gnulinux-$KREL-advanced-\$UUID" /boot/grub/grub.cfg
grub-set-default "\$ADV>gnulinux-$OLD-advanced-\$UUID"
grub-reboot "\$ADV>gnulinux-$KREL-advanced-\$UUID"
grub-editenv list
EOF
[ "${PIPESTATUS[0]}" -eq 0 ] || { log "ABORT: install/grub step failed"; exit 1; }

log "rebooting $NODE into $KREL (one-shot; fallback $OLD)"
t0=$(date +%s)
$SSH "sudo systemctl reboot" || true
sleep 30

# Wait up to 15 min for SSH + Ready.
up=0
while [ $(( $(date +%s) - t0 )) -lt 900 ]; do
	if $SSH true 2>/dev/null &&
	   [ "$(kubectl --context $CTX get node $NODE -o jsonpath='{.status.conditions[?(@.type=="Ready")].status}')" = True ]; then
		up=1; break
	fi
	sleep 15
done
if [ $up -ne 1 ]; then
	log "FAIL: $NODE not back within 15 min"
	openstack console log show --lines 200 $NODE > $OUT/console.log 2>&1 || true
	exit 2
fi
log "$NODE Ready after $(( $(date +%s) - t0 )) s"

# Wait for the node's DaemonSet pods (cilium etc.) to be Ready, up to 10 min.
for i in $(seq 60); do
	notready=$(kubectl --context $CTX get pods -A --field-selector spec.nodeName=$NODE \
		-o jsonpath='{range .items[*]}{.metadata.namespace}/{.metadata.name} {.status.conditions[?(@.type=="Ready")].status}{"\n"}{end}' | grep -v ' True$')
	[ -z "$notready" ] && break
	sleep 10
done
kubectl --context $CTX get pods -A -o wide --field-selector spec.nodeName=$NODE | tee -a $OUT/install.log
[ -n "$notready" ] && log "WARN: pods not Ready: $notready"

# Verify the running kernel.
$SSH "sudo bash -s" <<'EOF' 2>&1 | tee $OUT/verify.log
echo "uname: $(uname -r)"
echo "cmdline: $(cat /proc/cmdline)"
echo "sysctl: $(cat /proc/sys/kernel/sched_cfs_bandwidth_width 2>&1)"
echo "sched_ext state: $(cat /sys/kernel/sched_ext/state)"
echo "--- scx_width 20 s"
timeout -s INT 20 /opt/scx/scx_width -i 5 > /tmp/scx_width.out 2>&1 &
sleep 5
echo "state during run: $(cat /sys/kernel/sched_ext/state) ops: $(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)"
wait
echo "scx_width exit: $?"
cat /tmp/scx_width.out
echo "state after: $(cat /sys/kernel/sched_ext/state)"
dmesg | grep -iE 'sched_ext|panic|BUG|oops' | tail -20
grub-editenv list
EOF
log "verify done"
