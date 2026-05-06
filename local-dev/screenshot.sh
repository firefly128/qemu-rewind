#!/bin/bash
# screenshot.sh — capture the QEMU framebuffer via QEMU's monitor
# `screendump` command. Solaris 7's xwd has a different option set
# than modern xwd (no -silent), and ssh-cat-piping the .xwd back
# turned out to silently reuse stale captures. The QEMU monitor
# bypasses both: it dumps the host-side framebuffer pixels straight
# to a file on the host filesystem.
#
# Usage: ./screenshot.sh [output.png]
#        defaults to /tmp/sparc-screen.png

set -euo pipefail
OUT="${1:-/tmp/sparc-screen.png}"
TMP_PPM=$(mktemp /tmp/qemu-fb.XXXXXX.ppm)
trap 'rm -f "$TMP_PPM"' EXIT

# QEMU monitor on tcp::5556 (per local-dev/start.sh).
# The screendump command writes the framebuffer as a PPM to a
# host-side path. We use a unique tempfile so concurrent runs
# don't race.
echo "screendump $TMP_PPM" | nc -w2 localhost 5556 > /dev/null 2>&1

# QEMU writes asynchronously; tiny wait for the file to appear.
for _ in 1 2 3 4 5; do
    [ -s "$TMP_PPM" ] && break
    sleep 0.5
done

if [ ! -s "$TMP_PPM" ]; then
    echo "screendump produced no file at $TMP_PPM" >&2
    exit 1
fi

pnmtopng "$TMP_PPM" > "$OUT" 2>/dev/null
echo "$OUT"
