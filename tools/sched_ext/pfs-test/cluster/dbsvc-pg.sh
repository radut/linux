# shellcheck shell=bash
# PostgreSQL 17 + pgbench. Client pod pgcli on node B (limit 8 CPU), server pg-<node>-<limit>.
PG_IMAGE=postgres:17
PG_SCALE=${PG_SCALE:-50}
PG_SECS=${PG_SECS:-120}

pg_client() {
	k get pod pgcli >/dev/null 2>&1 && return 0
	mkpod pgcli b "$PG_IMAGE" --cpu 8 --mem 4Gi --role client --env POSTGRES_HOST_AUTH_METHOD=trust -- sleep infinity
}

# pg_server NODE LIMIT(none|cpus) -> pod name on stdout; initialises pgbench data.
pg_server() {
	local node=$1 lim=$2 name="pg-$1-${2/./p}" opts=()
	[[ $lim != none ]] && opts=(--cpu "$lim" --mem 3Gi)
	if ! k get pod "$name" >/dev/null 2>&1; then
		mkpod "$name" "$node" "$PG_IMAGE" "${opts[@]}" --shm 512Mi --data /var/lib/postgresql/data \
			--env POSTGRES_HOST_AUTH_METHOD=trust -- \
			docker-entrypoint.sh postgres -c shared_buffers=512MB -c max_connections=200 \
			-c synchronous_commit=off -c log_min_messages=warning
		local ip
		ip=$(podip "$name")
		local _
		for _ in $(seq 60); do k exec pgcli -- pg_isready -q -h "$ip" -U postgres && break; sleep 2; done
		k exec pgcli -- pgbench -i -q -s "$PG_SCALE" -h "$ip" -U postgres postgres >&2
	fi
	echo "$name"
}

# pg_bench IP WL(select|tpcb) CLIENTS: runs in pgcli, prints pgbench summary, percentiles and per-second rows.
pg_bench() {
	local ip=$1 wl=$2 c=$3 sflag="" rate=1
	[[ $wl == select ]] && { sflag=-S; rate=0.2; }
	k exec pgcli -- sh -c "
		psql -q -h $ip -U postgres -c 'vacuum analyze' -c checkpoint postgres
		rm -rf /tmp/r && mkdir /tmp/r && cd /tmp/r
		pgbench -h $ip -U postgres -n $sflag -c $c -j 4 -T $PG_SECS -P 10 --log --log-prefix=/tmp/r/l --sampling-rate=$rate postgres 2>&1
		echo '### percentiles_us'
		cat l.* | awk '{print \$3}' | sort -n > lat
		awk 'BEGIN{n=split(\"0.5 0.9 0.99 0.999 0.9999 1\",q,\" \")} {a[NR]=\$1; s+=\$1}
		     END{printf \"n=%d avg=%.0f\", NR, s/NR; for(i=1;i<=n;i++){j=int(NR*q[i]); if(j<1)j=1; printf \" p%s=%d\", q[i]*100, a[j]} print \"\"}' lat
		echo '### per_second epoch sampled_txn max_us'
		cat l.* | awk '{c[\$5]++; if(\$3>m[\$5])m[\$5]=\$3} END{for(s in c) print s, c[s], m[s]}' | sort -n
		rm -rf /tmp/r"
}

# pg_run NODE MODE LIMIT WL CLIENTS REP SERVERPOD
pg_run() {
	local node=$1 mode=$2 lim=$3 wl=$4 c=$5 rep=$6 pod=$7 ip
	ip=$(podip "$pod")
	local out="$R/pg/${node}-${mode}-l${lim}-${wl}-c${c}-r${rep}-$(date +%H%M%S)"
	log "pg $node $mode l$lim $wl c$c r$rep"
	measure "$out" "$node" "$mode" "$pod" pgcli -- pg_bench "$ip" "$wl" "$c"
	grep -E '^tps|^latency average|^p50|^n=' "$out/run.out" | tr '\n' ' ' >&2; echo >&2
}
