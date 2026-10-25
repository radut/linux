#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Build a tiny local image (no registry, no network, no Dockerfile builder):
# the static test binaries in a FROM-scratch filesystem, via "docker import".
# Adds /scx_width when a statically linked scx_width is found (for
# scx-container.sh); see README.md for the static link line.
set -euo pipefail
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$KIT/lib.sh"

usage() {
	echo "usage: $0 [-h]   (env: DOCKER=docker|podman IMAGE=$IMAGE SCX=/path/to/static/scx_width)"
}
[ "${1:-}" != -h ] || { usage; exit 0; }
[ $# -eq 0 ] || { usage >&2; exit 2; }

is_static() { ! ldd "$1" > /dev/null 2>&1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
for b in spin noisy reqlat; do
	[ -x "$KIT/bin/$b" ] || die "missing $KIT/bin/$b, run: make -C $KIT"
	is_static "$KIT/bin/$b" || die "$KIT/bin/$b is dynamically linked; rebuild with STATIC=-static"
	cp "$KIT/bin/$b" "$stage/"
done
if scx=$(find_scx) && is_static "$scx"; then
	cp "$scx" "$stage/scx_width"
	log "including static scx_width from $scx"
else
	log "no static scx_width found: the image cannot run scx-container.sh (host mode still works)"
fi
mkdir -p "$stage/tmp"
tar -C "$stage" -c . | $DOCKER import -c 'CMD ["/spin", "1", "5"]' - "$IMAGE" > /dev/null
log "built $IMAGE: $(cd "$stage" && echo *)"
