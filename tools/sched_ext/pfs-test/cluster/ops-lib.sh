#!/usr/bin/env bash
# Shared helpers for the ops-soak batches (failure/operations tests + soak on node B).
# Sourced by the batch scripts; every kubectl call carries --context and -n explicitly.
# shellcheck disable=SC2016  # remote snippets in single quotes expand on the node
set -euo pipefail
shopt -s inherit_errexit
# shellcheck disable=SC2034  # used by the batch scripts

S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
KIT="$S/results2/ops-soak/kit"
OUT_ROOT="$S/results2/ops-soak"
CTX="${CTX:?set CTX to the kubectl context}"
NS=sched-test
NODE_B="${NODE_B:-nodeB}"
SSH_B="${SSH_B:?set SSH_B to user@host of node B}"
DS_FILE="$S/k8s/scx-width-daemonset.hostpath.yaml"
WORKER=ops-soak

IMG_UBUNTU=ubuntu:24.04
IMG_PG=postgres:17.11-alpine3.23
IMG_NGINX=nginx:1.30.6-alpine
IMG_K6=grafana/k6:1.8.2

log() { echo ">> $(date -u +%H:%M:%S) $*" >&2; }
die() { echo "!! $*" >&2; exit 1; }
k() { kubectl --context "$CTX" -n "$NS" "$@"; }
kc() { kubectl --context "$CTX" "$@"; }
nssh() { ssh -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=5 "$SSH_B" "$@"; }
status() { echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $WORKER $*" >>"$S/STATUS.md"; }
now() { date +%s.%N; }

# "scx:<ops>" or "cfs"
node_sched() {
	nssh 'st=$(cat /sys/kernel/sched_ext/state 2>/dev/null || echo missing)
		if [ "$st" = enabled ]; then echo "scx:$(cat /sys/kernel/sched_ext/root/ops)"; else echo cfs; fi'
}

node_meta() {
	nssh 'echo "host=$(hostname) kernel=$(uname -r) nproc=$(nproc) date=$(date -Is)"
		echo "sched_ext_state=$(cat /sys/kernel/sched_ext/state 2>/dev/null)"
		echo "kwidth_sysctl=$(cat /proc/sys/kernel/sched_cfs_bandwidth_width 2>/dev/null || echo absent)"
		echo "scx_width_sha=$(sha256sum /opt/scx/scx_width | cut -c1-16)"
		for f in /sys/kernel/sched_ext/root/ops /sys/kernel/sched_ext/root/events \
			/sys/kernel/sched_ext/nr_rejected; do
			[ -r "$f" ] && { echo "--- $f"; cat "$f"; }
		done; true'
}

kwidth_present() { nssh 'test -e /proc/sys/kernel/sched_cfs_bandwidth_width'; }
kwidth_get() { nssh 'cat /proc/sys/kernel/sched_cfs_bandwidth_width 2>/dev/null || echo absent'; }
kwidth_set() { nssh "echo $1 | sudo tee /proc/sys/kernel/sched_cfs_bandwidth_width >/dev/null"; }

# Rewrite cpu.max of every limited cgroup of the sched-test pods (pod slice + container scopes),
# so that the current sysctl value applies to them.
kwidth_rewrite() {
	local uids
	uids=$(k get pod -o jsonpath='{range .items[*]}{.metadata.uid}{"\n"}{end}' | tr - _)
	[[ -n "$uids" ]] || return 0
	nssh "n=0; for u in $(echo "$uids" | tr '\n' ' '); do
		for d in \$(find /sys/fs/cgroup/kubepods.slice -type d -path \"*pod\${u}.slice*\"); do
			f=\$d/cpu.max; [ -w \$f ] || [ -e \$f ] || continue
			v=\$(cat \$f); case \$v in max*) continue ;; esac
			echo \"\$v\" | sudo tee \$f >/dev/null && n=\$((n+1))
		done; done; echo rewrote=\$n"
}

ds_present() { [[ -n "$(k get ds scx-width -o name --ignore-not-found)" ]]; }

scx_on() {
	k apply -f "$DS_FILE" >&2
	k rollout status ds/scx-width --timeout=180s >&2
	k wait pod -l app.kubernetes.io/name=scx-width --for=condition=Ready --timeout=90s >&2
	local st; st=$(node_sched)
	[[ "$st" == scx:width ]] || die "scx_on: node reports $st"
	log "scx_width on ($st)"
}

scx_off() {
	k delete ds scx-width --ignore-not-found --wait=true >&2
	k wait pod -l app.kubernetes.io/name=scx-width --for=delete --timeout=90s >/dev/null 2>&1 || true
	local _
	for _ in $(seq 1 45); do
		[[ "$(node_sched)" == cfs ]] && { log "node b on the fair scheduler"; return 0; }
		sleep 2
	done
	die "node b still reports $(node_sched) after DaemonSet deletion"
}

# Leave node B as the rules require: no ops-soak workloads, scx off, sysctl 0, schedulable.
restore_node_b() {
	log "restoring node b (workloads, scx off, sysctl 0, uncordon)"
	kc uncordon "$NODE_B" >&2 || true
	k delete deploy,job,pod,svc,cm,secret -l sched-test/owner=ops-soak --ignore-not-found --wait=false >&2 || true
	k wait pod -l sched-test/owner=ops-soak --for=delete --timeout=180s >/dev/null 2>&1 || true
	scx_off || true
	if kwidth_present; then kwidth_set 0 || true; fi
	nssh 'sudo rm -f /tmp/ops-soak-nodemon.py' || true
	log "node b: sched=$(node_sched) sysctl=$(kwidth_get)"
}

# Labels/annotation every object carries. $1 = component name.
meta_labels() {
	cat <<EOF
    app.kubernetes.io/name: $1
    sched-test/owner: ops-soak
    ${LABEL_DOMAIN:-pfs-test.example}/out-of-band: temporary
    ${LABEL_DOMAIN:-pfs-test.example}/owner: sched-ext-test
    ${LABEL_DOMAIN:-pfs-test.example}/expires: "2026-11-06"
EOF
}
meta_annot() {
	cat <<EOF
  annotations:
    ${LABEL_DOMAIN:-pfs-test.example}/reason: "sched_ext ops/soak test ($1)"
EOF
}

# Pod spec bits shared by every workload.
pod_common() {
	cat <<EOF
      nodeSelector:
        sched-test: "true"
        sched-test/role: scx
      tolerations:
        - {key: sched-test, operator: Equal, value: "true", effect: NoSchedule}
      enableServiceLinks: false
      automountServiceAccountToken: false
      terminationGracePeriodSeconds: 5
EOF
}

# resources block; $1 = cpu limit (or "-" for none), $2 = memory
res_block() {
	if [[ "$1" == "-" ]]; then
		echo "          resources: {}"
	else
		cat <<EOF
          resources:
            requests: {cpu: "$1", memory: $2}
            limits: {cpu: "$1", memory: $2}
EOF
	fi
}

# Deployment of spin-loop. $1 name, $2 cpu limit or "-", $3 threads, $4 interval s
render_spin() {
	local name=$1 cpu=$2 thr=$3 intv=$4
	cat <<EOF
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: $name
  namespace: $NS
  labels:
$(meta_labels spin-loop)
$(meta_annot "$name")
spec:
  replicas: 1
  selector: {matchLabels: {sched-test/ops: $name}}
  template:
    metadata:
      labels:
        sched-test/ops: $name
$(meta_labels spin-loop)
    spec:
$(pod_common)
      containers:
        - name: spin
          image: $IMG_UBUNTU
          imagePullPolicy: IfNotPresent
          command: ["/opt/scx/spin-loop"]
          args: ["$thr", "0", "$intv"]
          env:
            - {name: COMM, value: "$name"}
            - name: CPU_LIMIT
              valueFrom: {resourceFieldRef: {containerName: spin, resource: limits.cpu, divisor: 1m}}
          securityContext: {allowPrivilegeEscalation: false, capabilities: {drop: [ALL]}, runAsNonRoot: true, runAsUser: 65534}
$(res_block "$cpu" 64Mi)
          volumeMounts: [{name: scx-bin, mountPath: /opt/scx, readOnly: true}]
      volumes: [{name: scx-bin, hostPath: {path: /opt/scx, type: Directory}}]
EOF
}

# nginx (limit $1) + Service, and a k6 client (limit $2) at a fixed rate $3 rps, in chunks of $4 s.
# k6 prints one JSON line per chunk (p50/p99/p999/max ms, failures, dropped iterations).
render_http() {
	local scpu=$1 ccpu=$2 rate=$3 chunk=$4
	cat <<EOF
---
apiVersion: v1
kind: ConfigMap
metadata:
  name: ops-k6
  namespace: $NS
  labels:
$(meta_labels k6)
$(meta_annot k6-script)
data:
  run.js: |
    import http from 'k6/http';
    export const options = {
      discardResponseBodies: true,
      summaryTrendStats: ['avg', 'p(50)', 'p(99)', 'p(99.9)', 'max'],
      scenarios: { fixed: { executor: 'constant-arrival-rate', rate: __ENV.RATE * 1,
        timeUnit: '1s', duration: __ENV.CHUNK + 's', preAllocatedVUs: 64, maxVUs: 256 } },
    };
    export default function () { http.get('http://ops-nginx/', { timeout: '5s' }); }
    export function handleSummary(d) {
      const m = d.metrics, t = m.http_req_duration.values;
      const r = { t: new Date().toISOString(), rate: __ENV.RATE * 1, reqs: m.http_reqs.values.count,
        p50: t['p(50)'], p99: t['p(99)'], p999: t['p(99.9)'], max: t.max,
        failed: m.http_req_failed.values.passes,
        dropped: m.dropped_iterations ? m.dropped_iterations.values.count : 0 };
      return { stdout: 'K6 ' + JSON.stringify(r) + '\n' };
    }
---
apiVersion: v1
kind: Service
metadata:
  name: ops-nginx
  namespace: $NS
  labels:
$(meta_labels nginx)
$(meta_annot nginx-svc)
spec:
  selector: {sched-test/ops: ops-nginx}
  ports: [{port: 80, targetPort: 8080}]
---
apiVersion: v1
kind: ConfigMap
metadata:
  name: ops-nginx
  namespace: $NS
  labels:
$(meta_labels nginx)
$(meta_annot nginx-conf)
data:
  default.conf: |
    server { listen 8080; location / { root /usr/share/nginx/html; } }
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: ops-nginx
  namespace: $NS
  labels:
$(meta_labels nginx)
$(meta_annot nginx)
spec:
  replicas: 1
  selector: {matchLabels: {sched-test/ops: ops-nginx}}
  template:
    metadata:
      labels:
        sched-test/ops: ops-nginx
$(meta_labels nginx)
    spec:
$(pod_common)
      containers:
        - name: nginx
          image: $IMG_NGINX
          imagePullPolicy: IfNotPresent
          ports: [{containerPort: 8080}]
          readinessProbe: {httpGet: {path: /, port: 8080}, periodSeconds: 5}
$(res_block "$scpu" 256Mi)
          volumeMounts: [{name: conf, mountPath: /etc/nginx/conf.d}]
      volumes: [{name: conf, configMap: {name: ops-nginx}}]
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: ops-k6
  namespace: $NS
  labels:
$(meta_labels k6)
$(meta_annot k6)
spec:
  replicas: 1
  selector: {matchLabels: {sched-test/ops: ops-k6}}
  template:
    metadata:
      labels:
        sched-test/ops: ops-k6
$(meta_labels k6)
    spec:
$(pod_common)
      containers:
        - name: k6
          image: $IMG_K6
          imagePullPolicy: IfNotPresent
          command: ["/bin/sh", "-c"]
          args: ["until wget -q -T 2 -O /dev/null http://ops-nginx/; do sleep 1; done; while :; do k6 run -q --no-color --no-usage-report /k6/run.js 2>&1 | grep -E '^K6 |level=error' | tail -5; done"]
          env: [{name: RATE, value: "$rate"}, {name: CHUNK, value: "$chunk"}]
$(res_block "$ccpu" 512Mi)
          volumeMounts: [{name: script, mountPath: /k6}]
      volumes: [{name: script, configMap: {name: ops-k6}}]
EOF
}

# Postgres (limit $1) on an emptyDir + pgbench client (limit $2): scale $3, $4 clients,
# rate $5 tps (0 = unthrottled), chunks of $6 s; one JSON line per chunk with tps and
# latency percentiles (ms) from a sampled per-transaction log.
render_pg() {
	local dbcpu=$1 ccpu=$2 scale=$3 clients=$4 rate=$5 chunk=$6
	cat <<EOF
---
apiVersion: v1
kind: Service
metadata:
  name: ops-pg
  namespace: $NS
  labels:
$(meta_labels postgres)
$(meta_annot pg-svc)
spec:
  selector: {sched-test/ops: ops-pg}
  ports: [{port: 5432}]
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: ops-pg
  namespace: $NS
  labels:
$(meta_labels postgres)
$(meta_annot postgres)
spec:
  replicas: 1
  strategy: {type: Recreate}
  selector: {matchLabels: {sched-test/ops: ops-pg}}
  template:
    metadata:
      labels:
        sched-test/ops: ops-pg
$(meta_labels postgres)
    spec:
$(pod_common)
      containers:
        - name: pg
          image: $IMG_PG
          imagePullPolicy: IfNotPresent
          args: ["-c", "shared_buffers=1GB", "-c", "max_connections=200", "-c", "max_wal_size=4GB",
                 "-c", "checkpoint_timeout=5min", "-c", "log_min_messages=warning"]
          env:
            - {name: POSTGRES_PASSWORD, valueFrom: {secretKeyRef: {name: ops-pg, key: password}}}
            - {name: PGDATA, value: /var/lib/postgresql/data/pgdata}
          ports: [{containerPort: 5432}]
          readinessProbe: {exec: {command: ["pg_isready", "-U", "postgres"]}, periodSeconds: 5}
$(res_block "$dbcpu" 4Gi)
          volumeMounts: [{name: data, mountPath: /var/lib/postgresql/data}, {name: shm, mountPath: /dev/shm}]
      volumes:
        - {name: data, emptyDir: {sizeLimit: 8Gi}}
        - {name: shm, emptyDir: {medium: Memory, sizeLimit: 1200Mi}}
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: ops-pgbench
  namespace: $NS
  labels:
$(meta_labels pgbench)
$(meta_annot pgbench)
spec:
  replicas: 1
  selector: {matchLabels: {sched-test/ops: ops-pgbench}}
  template:
    metadata:
      labels:
        sched-test/ops: ops-pgbench
$(meta_labels pgbench)
    spec:
$(pod_common)
      containers:
        - name: pgbench
          image: $IMG_PG
          imagePullPolicy: IfNotPresent
          command: ["/bin/sh", "-c"]
          args:
            - |
              until pg_isready -h ops-pg -U postgres -q; do sleep 2; done
              psql -h ops-pg -U postgres -tAc "select 1 from pgbench_branches limit 1" >/dev/null 2>&1 ||
                pgbench -h ops-pg -U postgres -i -q -s $scale
              R=""; [ "$rate" != 0 ] && R="-R $rate"
              cd /tmp
              while :; do
                rm -f pgb.*
                t0=\$(date -u +%Y-%m-%dT%H:%M:%SZ)
                o=\$(pgbench -h ops-pg -U postgres -c $clients -j 4 -T $chunk \$R -n -l --sampling-rate=0.2 --log-prefix=pgb 2>&1)
                tps=\$(echo "\$o" | sed -n 's/^tps = \([0-9.]*\).*/\1/p' | head -1)
                fail=\$(echo "\$o" | sed -n 's/^number of failed transactions: \([0-9]*\).*/\1/p')
                lag=\$(echo "\$o" | sed -n 's/^rate limit schedule lag: avg \([0-9.]*\).*max \([0-9.]*\).*/\1 \2/p')
                cat pgb.* 2>/dev/null | awk '{print \$3}' | sort -n | awk -v t0="\$t0" -v tps="\$tps" -v fail="\${fail:-0}" -v lag="\$lag" \
                  '{a[NR]=\$1} END {n=NR; if(!n){print "PGB {\"t\":\""t0"\",\"n\":0,\"tps\":\""tps"\"}"; exit}
                   split(lag,l," ");
                   printf "PGB {\"t\":\"%s\",\"n\":%d,\"tps\":%s,\"failed\":%s,\"p50\":%.2f,\"p99\":%.2f,\"p999\":%.2f,\"max\":%.2f,\"lag_avg\":%s,\"lag_max\":%s}\n",
                     t0,n,(tps==""?0:tps),fail,a[int(n*.5)+1]/1000,a[int(n*.99)+1]/1000,a[int(n*.999)+1]/1000,a[n]/1000,(l[1]==""?0:l[1]),(l[2]==""?0:l[2])}'
                echo "\$o" | grep -iE 'error|abort|FATAL' | head -3
                [ -n "\$tps" ] || sleep 10
              done
          env:
            - {name: PGPASSWORD, valueFrom: {secretKeyRef: {name: ops-pg, key: password}}}
            - {name: PGDATABASE, value: postgres}
            - {name: PGUSER, value: postgres}
$(res_block "$ccpu" 1Gi)
EOF
}

# Test-only password for the throwaway database, generated per run.
ensure_pg_secret() {
	k get secret ops-pg -o name --ignore-not-found | grep -q . && return 0
	k create secret generic ops-pg --from-literal=password="$(openssl rand -hex 16)" \
		--dry-run=client -o yaml |
		kc label --local -f - -o yaml sched-test/owner=ops-soak ${LABEL_DOMAIN:-pfs-test.example}/out-of-band=temporary \
			${LABEL_DOMAIN:-pfs-test.example}/owner=sched-ext-test ${LABEL_DOMAIN:-pfs-test.example}/expires=2026-11-06 |
		kc annotate --local -f - -o yaml ${LABEL_DOMAIN:-pfs-test.example}/reason="sched_ext ops/soak test (throwaway db password)" |
		k apply -f - >&2
}

wait_ready() { # label selector, timeout
	local _
	for _ in $(seq 1 60); do (($(k get pod -l "$1" -o name | wc -l) > 0)) && break; sleep 2; done
	k wait pod -l "$1" --for=condition=Ready --timeout="${2:-300s}" >&2
}

# ---- node-side monitor -------------------------------------------------------------------
# Python on node B: sched_ext state every 5 ms (changes only), and every second the cpu.stat
# of the given pods' cgroups, /proc/pressure/cpu and the sysctl. JSON lines on stdout.
NODEMON_PY='
import json, os, sys, time, glob
uids = [u.replace("-", "_") for u in sys.argv[2:]]
dur = float(sys.argv[1])
def rd(p):
    try:
        with open(p) as f: return f.read().strip()
    except OSError: return None
def cgdirs():
    r = {}
    for u in uids:
        g = glob.glob("/sys/fs/cgroup/kubepods.slice/**/*pod%s.slice" % u, recursive=True)
        if g: r[u] = g[0]
    return r
dirs = cgdirs()
def emit(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
emit({"t": time.time(), "ev": "start", "cgroups": dirs})
end = time.time() + dur
prev = None; nxt = 0
while time.time() < end:
    t = time.time()
    st = rd("/sys/kernel/sched_ext/state")
    if st != prev:
        emit({"t": t, "ev": "state", "state": st, "ops": rd("/sys/kernel/sched_ext/root/ops")})
        prev = st
    if t >= nxt:
        nxt = t + 1.0
        if len(dirs) < len(uids): dirs = cgdirs()
        cg = {}
        for u, d in dirs.items():
            s = rd(d + "/cpu.stat")
            if s is None: continue
            kv = dict(l.split() for l in s.splitlines())
            cg[u[:8]] = [int(kv.get("usage_usec", 0)), int(kv.get("nr_throttled", 0)),
                         int(kv.get("throttled_usec", 0)), rd(d + "/cpu.max")]
        emit({"t": t, "ev": "tick", "psi": rd("/proc/pressure/cpu").splitlines()[0],
              "load": rd("/proc/loadavg"), "kw": rd("/proc/sys/kernel/sched_cfs_bandwidth_width"), "cg": cg})
    time.sleep(0.005)
emit({"t": time.time(), "ev": "end"})
'

# nodemon_start OUTFILE DURATION_S : monitors every ops-soak pod present now; sets MON_PID
nodemon_start() {
	local out=$1 dur=$2 uids
	uids=$(k get pod -l sched-test/owner=ops-soak -o jsonpath='{range .items[*]}{.metadata.uid} {end}')
	nssh "cat > /tmp/ops-soak-nodemon.py" <<<"$NODEMON_PY"
	nssh "python3 /tmp/ops-soak-nodemon.py $dur $uids" >"$out" 2>&1 &
	MON_PID=$!
}

# Local-minus-node clock offset estimate (seconds), to put local events on the node clock.
clock_offset() {
	local a b n
	a=$(now); n=$(nssh 'date +%s.%N'); b=$(now)
	python3 -c "print(f'{($a + $b) / 2 - $n:.4f}')"
}

# Snapshot of pod restarts, scx log tail, sched_ext events and kernel warnings since $1 (node time, "-10min" etc.)
health_snap() {
	local since=${1:--15min}
	echo "== $(date -u +%FT%TZ) node: $(kc get node "$NODE_B" -o jsonpath='{range .status.conditions[?(@.type=="Ready")]}{.status} {.reason} {.lastTransitionTime}{end}')"
	k get pod -o wide 2>&1 | sed 's/^/   /'
	echo "-- scx_width log tail"; k logs ds/scx-width --tail=4 2>&1 | sed 's/^/   /' || true
	echo "-- node"
	nssh "echo sched=\$(cat /sys/kernel/sched_ext/state) sysctl=\$(cat /proc/sys/kernel/sched_cfs_bandwidth_width 2>/dev/null || echo absent)
		cat /sys/kernel/sched_ext/root/events 2>/dev/null | tr '\n' ' '; echo
		sudo journalctl -k --since '$since' --no-pager -o short-iso | grep -viE 'cilium|lxc|veth|eth0: |IPv6|entered (blocking|forwarding|disabled)' | tail -40" 2>&1 | sed 's/^/   /'
}
