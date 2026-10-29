#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Run scx_pfs in a container with only the capabilities it needs to load
# (CAP_SYS_ADMIN, CAP_BPF, CAP_PERFMON), the Docker equivalent of a
# Kubernetes DaemonSet. The scheduler then applies to every task on the host,
# not only to containers.
set -euo pipefail
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$KIT/lib.sh"

usage() {
	cat <<USAGE
usage: $0 start|stop|status
  env: DOCKER=docker|podman IMAGE=$IMAGE SCX_CTR=$SCX_CTR SCX_ARGS="-s 2500"
USAGE
}

case "${1:-}" in
start)
	need_root
	scx_container_start
	;;
stop)
	need_root
	scx_container_stop
	log "sched_ext state: $(cat $SCX_STATE 2>/dev/null || echo n/a)"
	;;
status)
	echo "sched_ext state: $(cat $SCX_STATE 2>/dev/null || echo n/a) ops: $(cat /sys/kernel/sched_ext/root/ops 2>/dev/null || echo -)"
	$DOCKER ps -a --filter "name=^${SCX_CTR}\$" --format '{{.Names}} {{.Status}}' 2>/dev/null || true
	;;
-h|--help)
	usage
	;;
*)
	usage >&2
	exit 2
	;;
esac
