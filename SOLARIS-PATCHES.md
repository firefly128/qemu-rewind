# Solaris 7 SPARC Patches for QEMU

This fork of QEMU includes 12 patches that enable Solaris 7 to run on emulated
SPARCstation hardware using real Sun OBP firmware (ss4.bin). These patches are
required — stock QEMU cannot boot or install Solaris 7 on SS-4.

## Patched Files

| File | Patches Applied |
|------|------------------|
| `include/scsi/constants.h` | `MODE_PAGE_FORMAT_DEVICE` constant (0x03) |
| `hw/scsi/scsi-disk.c` | MODE SENSE page 3 handler, rotation rate in pages 4 & 5 |
| `hw/sparc/sun4m.c` | TCX VRAM 2MB, AFX base address, NMI support, NVRAM persistence |
| `hw/misc/slavio_misc.c` | diag-switch OFF by default |
| `hw/char/escc.c` | BREAK interrupt storm fix, NUL byte on BREAK |
| `hw/net/pcnet.c` | Lance packet data byte-swap fix (TX + RX) |
| `hw/dma/sparc32_dma.c` | LEDMA DVMA base address fix (read + write) |

## Patch Details

### 1–3. SCSI MODE SENSE Page 3 (Format Device)

**Critical.** Solaris 7's `sd` driver sends a MODE SENSE command for page 3
(Format Device) during device attach to read sectors-per-track and
bytes-per-sector. Stock QEMU doesn't implement page 3, so the driver gets
zeros, causing a **division-by-zero panic** in the kernel.

These three patches add:
- The `MODE_PAGE_FORMAT_DEVICE` (0x03) constant to `constants.h`
- Page 3 to the `mode_sense_valid[]` whitelist in `scsi-disk.c`
- A `case MODE_PAGE_FORMAT_DEVICE:` handler that returns geometry derived from
  the drive's block configuration (sectors per track, bytes per sector, etc.)

With this patch, `format(1M)` auto-detects the disk — no manual geometry entry
or post-install superblock patching is needed. Solaris reports:
```
Mode sense page(3) reports nsect value as 63, adjusting it to 65
```

### 4. TCX VRAM 2MB

A real SPARCstation 4 has 1MB of TCX VRAM. However, the OBP firmware probes
VRAM *size* by writing to `0x50900000` — 1MB past the start of the DFB8 (Dumb
Frame Buffer 8-bit) region. On real hardware, accessing past the end of
physical VRAM triggers an SBus bus error that the firmware's trap handler
catches gracefully to determine the VRAM boundary.

In stock QEMU, the DFB8 region is mapped as exactly 1MB (`0x100000`), ending
at `0x508FFFFF`. The firmware's probe at `0x50900000` hits completely unmapped
address space, which QEMU translates into an unrecoverable Data Access
Exception — crashing the firmware instead of letting the trap handler catch it.

This patch maps 2MB so the probe falls within a valid region and the firmware's
size-detection logic proceeds normally. The firmware still determines how much
VRAM is usable on its own; the extra mapped memory just prevents the fault.

A cleaner fix would be to make QEMU return a proper SBus bus error for accesses
past the mapped region boundary, but that would require invasive changes to
QEMU's memory subsystem. Mapping 2MB is the pragmatic workaround.

### 5–6. Rotation Rate (Pages 4 & 5)

Patches the HD Geometry (page 4) and Flexible Disk Geometry (page 5) MODE SENSE
responses to report a 7200 RPM rotation rate instead of 0. Some Solaris
utilities use this value.

### 7. AFX Base Address (SS-4)

The SS-4 firmware reads the AFX register at `0x6e000000` during the banner
display and boot sequence. Stock QEMU's SS-4 `hwdef` doesn't include this
address, causing a Data Access Exception. This patch adds `.afx_base` to the
SS-4 machine definition.

### 8. NMI Support + diag-switch OFF

Adds NMI (Non-Maskable Interrupt) support to the sun4m machine, allowing serial
BREAK to drop to the OBP `ok` prompt (equivalent to Stop-A on real hardware).
Also sets the `slavio_misc` diagnostic register to `0x01` at reset, which tells
the firmware that diag-switch is OFF — preventing it from running extended
diagnostics and trying to netboot via RARP on every start.

### 9. NVRAM Persistence

Replaces the stock `nvram_init()` (which writes CHRP partition headers
incompatible with real Sun firmware) with a load/save mechanism controlled by
the `QEMU_NVRAM_FILE` environment variable. On startup, if the file exists, its
contents are loaded into the M48T59 NVRAM config region (8176 bytes,
`0x0000–0x1FD7`). On exit, the region is saved back. This preserves OBP
variables (`boot-device`, `auto-boot?`, etc.) across QEMU restarts.

The IDPROM region (`0x1FD8–0x1FEF`) is always written by QEMU and is not
saved/loaded.

### 10. ESCC BREAK Fix

Fixes an infinite interrupt storm triggered by sending a serial BREAK to the
ESCC (Z8530) UART. Two issues in stock QEMU:

1. **Missing Reset Ext/Status command (WR0 = 0x10):** The Z8530's "Reset
   Ext/Status Interrupts" command was not implemented. When the Solaris kernel
   handles a BREAK interrupt, it writes 0x10 to WR0 to clear the latched
   status. Without this, `STATUS_BRK` is never cleared and the interrupt fires
   endlessly, hanging the system.

2. **Missing NUL byte on BREAK:** A real Z8530 delivers a NUL (0x00) byte into
   the receive buffer when BREAK is detected. The Solaris `zs` driver expects
   this — it reads the data register as part of BREAK handling. Without it,
   the driver's state machine gets confused.

With this patch, serial BREAK cleanly drops to the OBP `ok` prompt (Stop-A
equivalent) and the system can resume with `go`.

### 11. Lance Packet Data Byte-Swap Fix

**Critical for networking.** The SPARC LEDMA applies `bswap16` to all DMA data
when `do_bswap=0` (which is `CSR_BSWP(s)` for the Am7990 lance chip). This is
correct for descriptor structures and init blocks — `pcnet.c`'s
`le32/16_to_cpu` macros compensate for the swap. But for **packet data**, the
bswap16 garbles Ethernet frames:

- TX: guest sends ethertype `0x0806` (ARP) → SLIRP receives `0x0608`
- RX: SLIRP sends ethertype `0x0800` (IP) → guest receives `0x0008`

SLIRP can't parse the garbled frames, so ARP never resolves and no traffic
flows.

**Fix:** In `pcnet.c`, pass `do_bswap=1` (skip swap) for the two DMA calls
that handle packet data: the `phys_mem_read` in `pcnet_transmit` and the
`phys_mem_write` in the `PCNET_RECV_STORE` macro. Descriptor DMA retains
`CSR_BSWP(s)=0`. This is safe for `pcnet-pci` which doesn't use LEDMA.

### 12. LEDMA DVMA Base Address Fix

**Critical for networking.** On Sun4m, the LEDMA's `dmaregs[3]` register
provides the high address bits (`0xFF000000`) that extend the Am7990's 24-bit
DMA addresses into the IOMMU's mapped range (`0xFF000000–0xFFFFFFFF`). OpenBIOS
sets this correctly, but the **Solaris `le` driver clears it to 0** during
device initialization or reset.

With `dmaregs[3]=0`, lance DMA addresses like `0x00E916` go directly to the
IOMMU without the required high-byte extension. The IOMMU translates these
through the wrong page table entries, causing `pcnet_transmit` to read garbage
data (typically OpenBIOS residue or unrelated guest memory) instead of the
actual packet buffers.

**Diagnosis:** QEMU monitor `xp` commands confirmed:
- Physical address `0x009CE916` (correct IOPTE mapping via `0xFF00E916`)
  contains a valid ARP broadcast frame
- Physical address `0x03221916` (wrong IOPTE mapping via `0x0000E916`)
  contains zeros or unrelated data
- `dmaregs[3]` at MMIO address `0x7840001C` reads as `0x00000000`

**Fix:** Replace `addr |= s->dmaregs[3]` with `addr |= 0xff000000` in both
`ledma_memory_read` and `ledma_memory_write` in `sparc32_dma.c`. This ensures
the DVMA base is always correct regardless of guest register state.

**Result:** With both patches 11 and 12 applied, SLIRP networking works
end-to-end: ARP resolves, gateway ping succeeds, DNS resolution works via the
system resolver, and external connectivity is functional.

## Applying Patches

The patches are applied via `sed` by the script in
[sparc-build-host/patches/apply-qemu-patches.sh](../sparc-build-host/patches/apply-qemu-patches.sh):

```sh
./apply-qemu-patches.sh /path/to/qemu-source
```

**macOS:** The script uses GNU `sed -i` syntax. Install GNU sed first:
```sh
brew install gnu-sed
PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH" bash apply-qemu-patches.sh /path/to/qemu
```

## Building

After applying patches, build QEMU with SPARC target support:

```sh
mkdir build && cd build
../configure --target-list=sparc-softmmu
make -j$(nproc)
```

## Usage

```sh
qemu-system-sparc \
  -M SS-4 \
  -m 160 \
  -bios ss4.bin \
  -drive file=disk.qcow2,format=qcow2,if=none,id=hd0 \
  -device scsi-hd,drive=hd0,bus=scsi.0,scsi-id=3,cyls=4000,heads=32,secs=63,rotation_rate=7200 \
  -drive file=sol7install.iso,format=raw,if=none,id=cd0 \
  -device scsi-cd,drive=cd0,bus=scsi.0,scsi-id=6 \
  -serial tcp::5555,server,nowait,telnet \
  -monitor tcp::5556,server,nowait \
  -net nic,macaddr=08:00:20:12:34:56 -net user \
  -nographic
```

The MAC address `08:00:20:12:34:56` uses the Sun OUI (`08:00:20`) so the
Solaris `le` driver accepts it. SLIRP provides a gateway at `10.0.2.2`, DNS at
`10.0.2.3`, and assigns the guest `10.0.2.15/24`.

## Related Projects

- **[sparc-build-host](../sparc-build-host/)** — Automated Solaris 7 installation using Docker + this patched QEMU
- **[solaris-sparc-mcp-server](../solaris-sparc-mcp-server/)** — MCP server providing AI-accessible Solaris SPARC knowledge
