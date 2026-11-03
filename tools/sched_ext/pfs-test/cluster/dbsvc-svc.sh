# shellcheck shell=bash
# Redis (redis-benchmark), nginx and a JVM HTTP service (fortio at a fixed rate), netperf TCP_RR.
RD_IMAGE=redis:7.4
NGX_IMAGE=nginx:1.27
JVM_IMAGE=eclipse-temurin:21-jdk
FORTIO_IMAGE=fortio/fortio:latest
HTTP_SECS=${HTTP_SECS:-60}

rd_client() {
	alive rcli && return 0
	mkpod rcli b "$RD_IMAGE" --cpu 8 --mem 2Gi --role client --deadline 43200 -- sleep infinity
}
fortio_client() {
	alive fcli && return 0
	mkpod fcli b "$FORTIO_IMAGE" --cpu 8 --mem 2Gi --role client --deadline 43200 -- fortio server
}

# rd_server NODE LIMIT IOTHREADS -> pod name
rd_server() {
	local node=$1 lim=$2 io=$3 name="rd-$1-${2/./p}-io$3" opts=()
	[[ $lim != none ]] && opts=(--cpu "$lim" --mem 2Gi)
	alive "$name" || mkpod "$name" "$node" "$RD_IMAGE" "${opts[@]}" -- \
		redis-server --save "" --appendonly no --protected-mode no --io-threads "$io" --io-threads-do-reads yes
	echo "$name"
}
rd_bench() { # IP
	k exec rcli -- redis-benchmark -h "$1" -c 64 -n 2000000 -t set,get -d 256 -r 100000 --threads 4 --csv
}
rd_run() { # NODE MODE LIMIT IO REP POD
	local out="$R/redis/${1}-${2}-l${3}-io${4}-r${5}-$(date +%H%M%S)"
	log "redis $1 $2 l$3 io$4 r$5"
	measure "$out" "$1" "$2" "$6" rcli -- rd_bench "$(podip "$6")"
	tr '\n' ' ' <"$out/run.out" >&2; echo >&2
}

# nginx: 48 KiB text, gzip level 6 per request (CPU bound), worker_processes auto (= node CPUs).
ngx_server() {
	local node=$1 lim=$2 name="ngx-$1-${2/./p}" opts=()
	[[ $lim != none ]] && opts=(--cpu "$lim" --mem 1Gi)
	alive "$name" || mkpod "$name" "$node" "$NGX_IMAGE" "${opts[@]}" -- sh -c '
		head -c 36864 /dev/urandom | base64 > /usr/share/nginx/html/b.txt
		cat > /etc/nginx/conf.d/default.conf <<EOF
server { listen 80; access_log off;
  location / { root /usr/share/nginx/html; gzip on; gzip_comp_level 6; gzip_types text/plain; gzip_min_length 0; }
}
EOF
		exec nginx -g "daemon off;"'
	echo "$name"
}

# JVM: allocation-heavy handler + an old-gen cache (promotion -> full GCs), ParallelGC, 512 MiB heap.
JAVA_SRC='import com.sun.net.httpserver.*; import java.util.*; import java.util.concurrent.*; import java.net.*; import java.io.*;
public class Server {
  static final Object[] cache = new Object[60000];
  public static void main(String[] a) throws Exception {
    HttpServer s = HttpServer.create(new InetSocketAddress(8080), 1024);
    s.setExecutor(Executors.newFixedThreadPool(32));
    s.createContext("/", ex -> {
      ThreadLocalRandom r = ThreadLocalRandom.current();
      List<String> l = new ArrayList<>();
      for (int i = 0; i < 2000; i++) l.add(Long.toHexString(r.nextLong()) + i);
      Collections.sort(l);
      Map<String, Integer> m = new HashMap<>();
      for (String x : l) m.merge(x.substring(0, 2), 1, Integer::sum);
      cache[r.nextInt(cache.length)] = new byte[2048];
      cache[r.nextInt(cache.length)] = new byte[2048];
      byte[] body = m.toString().getBytes();
      ex.sendResponseHeaders(200, body.length);
      try (OutputStream o = ex.getResponseBody()) { o.write(body); }
    });
    s.start();
  }
}'
jvm_server() {
	local node=$1 lim=$2 name="jvm-$1-${2/./p}" opts=()
	[[ $lim != none ]] && opts=(--cpu "$lim" --mem 2Gi)
	if ! alive "$name"; then
		mkpod "$name" "$node" "$JVM_IMAGE" "${opts[@]}" --env "JAVA_SRC=$JAVA_SRC" -- sh -c '
			mkdir -p /app && printf "%s\n" "$JAVA_SRC" > /app/Server.java
			java -XX:+UseParallelGC -Xms512m -Xmx512m -Xlog:gc:file=/tmp/gc.log:uptime,level,tags \
				-Xlog:os+container=info:file=/tmp/container.log /app/Server.java'
		local ip _
		ip=$(podip "$name")
		for _ in $(seq 60); do k exec fcli -- fortio curl -quiet "http://$ip:8080/" >/dev/null 2>&1 && break; sleep 2; done
		# JIT warm-up
		k exec fcli -- fortio load -quiet -qps 0 -c 16 -t 30s "http://$ip:8080/" >/dev/null 2>&1
	fi
	echo "$name"
}

# fortio_cap URL [HDR]: max qps with 64 connections, 20 s.
fortio_cap() {
	k exec fcli -- fortio load -quiet -qps 0 -c 64 -t 20s ${2:+-H "$2"} -json - "$1" 2>/dev/null |
		python3 -I -c 'import json,sys; print(int(json.load(sys.stdin)["ActualQPS"]))'
}
fortio_fixed() { # URL QPS [HDR]
	k exec fcli -- fortio load -quiet -uniform -qps "$2" -c 32 -t "${HTTP_SECS}s" -p "50,90,99,99.9" \
		${3:+-H "$3"} -json - "$1"
}
http_run() { # KIND(ngx|jvm) NODE MODE LIMIT QPS REP POD
	local kind=$1 node=$2 mode=$3 lim=$4 qps=$5 rep=$6 pod=$7 ip url hdr=""
	ip=$(podip "$pod")
	if [[ $kind == ngx ]]; then url="http://$ip/b.txt"; hdr="Accept-Encoding: gzip"; else url="http://$ip:8080/"; fi
	local out="$R/$kind/${node}-${mode}-l${lim}-q${qps}-r${rep}-$(date +%H%M%S)"
	log "$kind $node $mode l$lim qps$qps r$rep"
	local gc0=0
	[[ $kind == jvm ]] && gc0=$(k exec "$pod" -- sh -c 'wc -l < /tmp/gc.log')
	measure "$out" "$node" "$mode" "$pod" fcli -- fortio_fixed "$url" "$qps" "$hdr"
	if [[ $kind == jvm ]]; then
		k exec "$pod" -- sh -c "tail -n +$((gc0 + 1)) /tmp/gc.log" >"$out/gc.log"
		k exec "$pod" -- cat /tmp/container.log >"$out/container.log" 2>/dev/null || true
	fi
	python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); h=d["DurationHistogram"]
print("qps=%.0f"%d["ActualQPS"], " ".join("p%s=%.2fms"%(p["Percentile"],p["Value"]*1e3) for p in h["Percentiles"]), "max=%.1fms"%(h["Max"]*1e3), "codes=%s"%d.get("RetCodes"))' "$out/run.out" >&2 || true
}

# netperf: netserver pod (limit 1) on node NODE, client = tools pod on B.
np_server() {
	local node=$1 name="np-$1"
	if ! alive "$name"; then
		mkpod "$name" "$node" ubuntu:24.04 --cpu 1 --mem 512Mi -- sh -c \
			'apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq netperf >/dev/null 2>&1; exec netserver -D'
		local ip _
		ip=$(podip "$name")
		for _ in $(seq 90); do k exec tools -- netperf -H "$ip" -l 1 -t TCP_RR >/dev/null 2>&1 && break; sleep 2; done
	fi
	echo "$name"
}
np_run() { # NODE MODE REP POD
	local out="$R/netperf/${1}-${2}-r${3}-$(date +%H%M%S)"
	log "netperf server-on-$1 $2 r$3"
	measure "$out" "$1" "$2" "$4" tools -- k exec tools -- netperf -H "$(podip "$4")" -l 30 -t TCP_RR -- \
		-o min_latency,mean_latency,p50_latency,p90_latency,p99_latency,max_latency,transaction_rate
	tail -1 "$out/run.out" >&2
}
