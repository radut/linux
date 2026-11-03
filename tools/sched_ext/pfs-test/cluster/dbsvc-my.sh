# shellcheck shell=bash
# MariaDB 11.4 + sysbench 1.0 (Ubuntu 24.04 package). Client pod tools on node B (limit 8 CPU).
MY_IMAGE=mariadb:11.4
MY_SECS=${MY_SECS:-90}
SB="sysbench --db-driver=mysql --mysql-user=root --mysql-password=bench --mysql-db=sbtest --tables=8 --table-size=200000"

tools_client() {
	k get pod tools >/dev/null 2>&1 && return 0
	mkpod tools b ubuntu:24.04 --cpu 8 --mem 4Gi --role client -- sh -c \
		'apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq sysbench netperf >/tmp/apt.log 2>&1 && touch /tmp/ready; sleep infinity'
	local _
	for _ in $(seq 90); do k exec tools -- test -e /tmp/ready 2>/dev/null && return 0; sleep 2; done
	log "tools client: apt install did not finish"; return 1
}

# my_server NODE LIMIT -> pod name; prepares sbtest tables.
my_server() {
	local node=$1 lim=$2 name="my-$1-${2/./p}" opts=()
	[[ $lim != none ]] && opts=(--cpu "$lim" --mem 3Gi)
	if ! k get pod "$name" >/dev/null 2>&1; then
		mkpod "$name" "$node" "$MY_IMAGE" "${opts[@]}" --data /var/lib/mysql \
			--env MARIADB_ROOT_PASSWORD=bench --env MARIADB_DATABASE=sbtest -- \
			docker-entrypoint.sh mariadbd --innodb-buffer-pool-size=1G --innodb-flush-log-at-trx-commit=2 \
			--innodb-log-file-size=512M --max-connections=300 --skip-name-resolve --skip-log-bin
		local ip _
		ip=$(podip "$name")
		for _ in $(seq 90); do k exec tools -- bash -c "exec 3<>/dev/tcp/$ip/3306" 2>/dev/null && break; sleep 2; done
		sleep 5
		k exec tools -- $SB --mysql-host="$ip" --threads=8 oltp_read_write prepare >&2
	fi
	echo "$name"
}

# my_bench IP TEST THREADS
my_bench() {
	k exec tools -- $SB --mysql-host="$1" --threads="$3" --time="$MY_SECS" --report-interval=1 \
		--percentile=99 --histogram=on "$2" run
}

# my_run NODE MODE LIMIT TEST THREADS REP SERVERPOD
my_run() {
	local node=$1 mode=$2 lim=$3 t=$4 th=$5 rep=$6 pod=$7 ip
	ip=$(podip "$pod")
	local out="$R/my/${node}-${mode}-l${lim}-${t}-t${th}-r${rep}-$(date +%H%M%S)"
	log "my $node $mode l$lim $t t$th r$rep"
	measure "$out" "$node" "$mode" "$pod" tools -- my_bench "$ip" "$t" "$th"
	grep -E 'transactions:|99th|avg:' "$out/run.out" | tr -s ' \n' ' ' >&2; echo >&2
}
