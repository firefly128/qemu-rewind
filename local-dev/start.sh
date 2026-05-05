#!/bin/bash
# start.sh — Boot Solaris 7 on local macOS with Cocoa display
#
# Uses the patched QEMU (BT458 DAC fix + all Solaris patches) with
# the original SS-4 ROM for proper auto-boot and framebuffer console.
#
# Usage:
#   ./start.sh                Full cold boot (CDE desktop on framebuffer)
#   ./start.sh --local        Resume 'local-ready' snapshot (CDE, SSH ready)
#   ./start.sh --ci           Resume 'ready' snapshot (serial console, CI mode)
#
# Access:
#   Cocoa window: framebuffer (CDE desktop, graphical apps)
#   Serial console: telnet localhost 5555
#   QEMU monitor: telnet localhost 5556
#   SSH: ssh -p 2222 -i disks/.ssh/id_rsa root@localhost
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

find_sst_build_host() {
  if [ -n "${SST_BUILD_HOST:-}" ]; then
    echo "$SST_BUILD_HOST"
    return
  fi
  for cand in \
    "$SCRIPT_DIR/../../.." \
    "$SCRIPT_DIR/../../../sst-build-pipeline" \
    "$SCRIPT_DIR/../../../sparc-build-host" ; do
    if [ -f "$cand/roms/ss4.bin" ]; then
      (cd "$cand" && pwd)
      return
    fi
  done
  echo "ERROR: could not locate sst-build-pipeline sibling repo." >&2
  echo "Set SST_BUILD_HOST=/path/to/sst-build-pipeline and re-run." >&2
  exit 1
}

REPO_DIR="$(find_sst_build_host)"
DISKS_DIR="$SCRIPT_DIR/disks"
ROM="$REPO_DIR/roms/ss4.bin"
NVRAM="$DISKS_DIR/nvram.bin"

if [ ! -f "$ROM" ] || [ ! -f "$NVRAM" ]; then
  echo "Run ./setup.sh first!"
  exit 1
fi

export QEMU_NVRAM_FILE="$NVRAM"

EXTRA_ARGS=()
case "${1:-}" in
  --local)
    echo "Resuming from 'local-ready' snapshot (CDE desktop)..."
    EXTRA_ARGS+=(-loadvm local-ready)
    ;;
  --ci)
    echo "Resuming from 'ready' snapshot (serial console, CI mode)..."
    EXTRA_ARGS+=(-loadvm ready)
    ;;
  *)
    echo "Full cold boot (CDE desktop on framebuffer)..."
    ;;
esac

echo "  Cocoa window: framebuffer (CDE desktop)"
echo "  Serial console: telnet localhost 5555"
echo "  QEMU monitor: telnet localhost 5556"
echo "  SSH: ssh -p 2222 -i disks/.ssh/id_rsa root@localhost"
echo ""

exec "$SCRIPT_DIR/bin/qemu-system-sparc" \
  -M SS-4 \
  -m 160 \
  -g 1024x768x8 \
  -bios "$ROM" \
  -audio driver=sdl \
  -drive file="$DISKS_DIR/root.qcow2",format=qcow2,if=none,id=hd0,cache=writethrough \
  -device scsi-hd,drive=hd0,bus=scsi.0,scsi-id=3,rotation_rate=7200 \
  -drive file="$DISKS_DIR/usr.qcow2",format=qcow2,if=none,id=hd1,cache=writethrough \
  -device scsi-hd,drive=hd1,bus=scsi.0,scsi-id=4,rotation_rate=7200 \
  -drive file="$DISKS_DIR/export.qcow2",format=qcow2,if=none,id=hd2,cache=writethrough \
  -device scsi-hd,drive=hd2,bus=scsi.0,scsi-id=1,rotation_rate=7200 \
  -serial tcp::5555,server,nowait,telnet \
  -monitor tcp::5556,server,nowait \
  -net nic,macaddr=08:00:20:12:34:56 \
  -net user,hostfwd=tcp::2222-:22 \
  -display cocoa \
  ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
