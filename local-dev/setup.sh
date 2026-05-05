#!/bin/bash
# setup.sh — One-time setup for local Solaris 7 QEMU on macOS
#
# Creates disk images, seeds NVRAM, and patches the ROM.
# Run this once before the first boot.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Find the sst-build-pipeline (formerly sparc-build-host) sibling repo,
# which holds roms/, patches/, and disc-images/.
find_sst_build_host() {
  if [ -n "${SST_BUILD_HOST:-}" ]; then
    echo "$SST_BUILD_HOST"
    return
  fi
  for cand in \
    "$SCRIPT_DIR/../../.." \
    "$SCRIPT_DIR/../../../sst-build-pipeline" \
    "$SCRIPT_DIR/../../../sparc-build-host" ; do
    if [ -f "$cand/roms/ss4.bin" ] && [ -f "$cand/patches/patch-rom.py" ]; then
      (cd "$cand" && pwd)
      return
    fi
  done
  echo "ERROR: could not locate sst-build-pipeline sibling repo." >&2
  echo "Set SST_BUILD_HOST=/path/to/sst-build-pipeline and re-run." >&2
  exit 1
}

REPO_DIR="$(find_sst_build_host)"
LOCAL_DIR="$SCRIPT_DIR"
DISKS_DIR="$LOCAL_DIR/disks"
ROM_SRC="$REPO_DIR/roms/ss4.bin"
ROM_PATCHED="$LOCAL_DIR/ss4-patched.bin"
NVRAM_FILE="$DISKS_DIR/nvram.bin"

mkdir -p "$DISKS_DIR"

# --- Create disk images (same layout as CI: root, usr, export) ---
if [ ! -f "$DISKS_DIR/root.qcow2" ]; then
  echo "Creating root.qcow2 (2G)..."
  "$LOCAL_DIR/bin/qemu-system-sparc" --version >/dev/null 2>&1  # sanity check
  qemu-img create -f qcow2 "$DISKS_DIR/root.qcow2" 2G
else
  echo "root.qcow2 already exists, skipping."
fi

if [ ! -f "$DISKS_DIR/usr.qcow2" ]; then
  echo "Creating usr.qcow2 (4G)..."
  qemu-img create -f qcow2 "$DISKS_DIR/usr.qcow2" 4G
else
  echo "usr.qcow2 already exists, skipping."
fi

if [ ! -f "$DISKS_DIR/export.qcow2" ]; then
  echo "Creating export.qcow2 (4G)..."
  qemu-img create -f qcow2 "$DISKS_DIR/export.qcow2" 4G
else
  echo "export.qcow2 already exists, skipping."
fi

# --- Patch ROM ---
if [ ! -f "$ROM_PATCHED" ]; then
  echo "Patching ROM (diag-device net -> disk, auto-boot? -> false)..."
  python3 "$REPO_DIR/patches/patch-rom.py" "$ROM_SRC" "$ROM_PATCHED"
else
  echo "Patched ROM already exists, skipping."
fi

# --- Seed NVRAM ---
if [ ! -f "$NVRAM_FILE" ]; then
  echo "Generating seed NVRAM..."
  python3 "$REPO_DIR/patches/nvram-seed.py" "$NVRAM_FILE"
else
  echo "NVRAM already exists, skipping."
fi

echo ""
echo "Setup complete! Next steps:"
echo "  1. Run ./install.sh to install Solaris 7 from ISO"
echo "  2. After install, run ./start.sh to boot normally"
