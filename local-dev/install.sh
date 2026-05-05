#!/bin/bash
# install.sh — Boot Solaris 7 installer with Cocoa display
#
# This launches QEMU with the install ISO attached as a CD-ROM.
# The Cocoa window shows the framebuffer (OBP banner, then installer).
# A serial console is also available via: telnet localhost 5555
# QEMU monitor is available via: telnet localhost 5556
#
# At the OBP "ok" prompt, type: boot cdrom
# Then follow the Solaris 7 installer.
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
    if [ -f "$cand/disc-images/sun-solaris-7-install-sparc.iso" ]; then
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
ROM="$SCRIPT_DIR/ss4-patched.bin"
NVRAM="$DISKS_DIR/nvram.bin"
ISO="${1:-$REPO_DIR/disc-images/sun-solaris-7-install-sparc.iso}"

if [ ! -f "$ROM" ] || [ ! -f "$NVRAM" ]; then
  echo "Run ./setup.sh first!"
  exit 1
fi

if [ ! -f "$ISO" ]; then
  echo "Solaris 7 install ISO not found at: $ISO"
  echo "Usage: $0 [path-to-sun-solaris-7-install-sparc.iso]"
  exit 1
fi

export QEMU_NVRAM_FILE="$NVRAM"

echo "Starting Solaris 7 installer..."
echo "  Cocoa window: framebuffer display"
echo "  Serial console: telnet localhost 5555"
echo "  QEMU monitor: telnet localhost 5556"
echo ""
echo "At the OBP 'ok' prompt, type:  boot cdrom"
echo ""

exec "$SCRIPT_DIR/bin/qemu-system-sparc" \
  -M SS-4 \
  -m 160 \
  -bios "$ROM" \
  -drive file="$DISKS_DIR/root.qcow2",format=qcow2,if=none,id=hd0,cache=writethrough \
  -device scsi-hd,drive=hd0,bus=scsi.0,scsi-id=3,rotation_rate=7200 \
  -drive file="$DISKS_DIR/usr.qcow2",format=qcow2,if=none,id=hd1,cache=writethrough \
  -device scsi-hd,drive=hd1,bus=scsi.0,scsi-id=4,rotation_rate=7200 \
  -drive file="$DISKS_DIR/export.qcow2",format=qcow2,if=none,id=hd2,cache=writethrough \
  -device scsi-hd,drive=hd2,bus=scsi.0,scsi-id=1,rotation_rate=7200 \
  -drive file="$ISO",format=raw,if=none,id=cd0 \
  -device scsi-cd,drive=cd0,bus=scsi.0,scsi-id=6 \
  -serial tcp::5555,server,nowait,telnet \
  -monitor tcp::5556,server,nowait \
  -net nic,macaddr=08:00:20:12:34:56 \
  -net user,hostfwd=tcp::2222-:22 \
  -display cocoa
