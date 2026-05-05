# local-dev/ — Local Solaris 7 QEMU run scripts (macOS)

This directory holds wrappers used **only** for running this fork of QEMU
locally on macOS to boot/install/test Solaris 7. It is not consumed by
the upstream qemu-rewind build itself (the patches in `hw/`, `include/`,
etc. are the real deliverable).

## What's here

- `setup.sh` — one-time setup: creates SCSI disk images, seeds NVRAM,
  patches the SS-4 ROM (`diag-device net → disk`, `auto-boot? → false`).
- `install.sh` — boots from a Solaris 7 install ISO with the Cocoa
  framebuffer display attached. Used once per fresh install.
- `start.sh` — normal boot / snapshot resume. Three modes:
    - default: cold boot to CDE on the framebuffer
    - `--local`: resume `local-ready` snapshot (CDE + SSH ready)
    - `--ci`: resume `ready` snapshot (serial console, CI mode)
- `dga_test.c` — direct framebuffer access test (open `/dev/fb`,
  mmap, write pixels, FBIOPUTCMAP). Cross-build with
  `sparc-sun-solaris2.7-gcc -O2 -o dga_test dga_test.c`, run as root
  on the guest.

## Dependencies on the sst-build-pipeline sibling

These scripts pull from the sst-build-pipeline (formerly sparc-build-host)
sibling repo for things that don't live here:
- `roms/ss4.bin` — original SS-4 OBP ROM
- `patches/patch-rom.py` — produces `ss4-patched.bin`
- `patches/nvram-seed.py` — produces `nvram.bin`
- `disc-images/sun-solaris-7-install-sparc.iso` — Solaris 7 install media

The scripts find that sibling via `$SST_BUILD_HOST` (env var). If unset,
they probe a few default locations relative to this repo:
  `../../../`  (for the current `<sst-build-pipeline>/libs/qemu-rewind/` layout)
  `../sst-build-pipeline/`
  `../sparc-build-host/`

If none of those are readable, the scripts error with a clear message —
set `SST_BUILD_HOST` explicitly to fix.

## Building qemu-system-sparc

These scripts assume a built `qemu-system-sparc` binary at `bin/qemu-system-sparc`
under this directory. Build it from the qemu-rewind source root:

```
mkdir -p build && cd build
../configure --target-list=sparc-softmmu --enable-sdl --enable-cocoa
make -j$(sysctl -n hw.ncpu) qemu-system-sparc
mkdir -p ../local-dev/bin
cp qemu-system-sparc ../local-dev/bin/
```

`bin/`, `disks/`, `*.bin`, and the binary itself are gitignored.
