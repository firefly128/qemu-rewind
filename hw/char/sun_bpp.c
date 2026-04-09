/*
 * QEMU Sun BPP (Bidirectional Parallel Port) with MIDI bit-bang decoder
 *
 * Emulates the SUNW,bpp parallel port found on SPARCstation 4/5/20.
 * Includes a MIDI bit-bang decoder that watches writes to the data
 * register and reconstructs MIDI bytes from the serial bit pattern
 * produced by the bppmidi Solaris driver.
 *
 * MIDI OUT bytes are forwarded to a platform-specific backend:
 *   - macOS: CoreMIDI virtual source ("QEMU SPARC MIDI Out")
 *   - Other: stderr logging (stub)
 *
 * Register layout (26 bytes at SBus MACIO offset):
 *   0x00  dma_csr      (32-bit) DMA control/status
 *   0x04  dma_addr     (32-bit) DMA address
 *   0x08  dma_bcnt     (32-bit) DMA byte count
 *   0x0c  (unused)     (32-bit)
 *   0x10  hw_config    (16-bit) hardware config
 *   0x12  op_config    (16-bit) operation config
 *   0x14  data         (8-bit)  parallel data pins
 *   0x15  trans_cntl   (8-bit)  transfer control
 *   0x16  out_pins     (8-bit)  output status pins
 *   0x17  in_pins      (8-bit)  input status pins (read-only)
 *   0x18  int_cntl     (16-bit) interrupt control
 *
 * Copyright (c) 2026 Julian Wolfe
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "sun_bpp_midi.h"

/*
 * BPP register offsets and sizes
 */
#define BPP_MMIO_SIZE       0x1a    /* 26 bytes total */

#define BPP_DMA_CSR         0x00    /* 32-bit */
#define BPP_DMA_ADDR        0x04    /* 32-bit */
#define BPP_DMA_BCNT        0x08    /* 32-bit */
#define BPP_DMA_UNUSED      0x0c    /* 32-bit */
#define BPP_HW_CONFIG       0x10    /* 16-bit */
#define BPP_OP_CONFIG       0x12    /* 16-bit */
#define BPP_DATA            0x14    /* 8-bit  */
#define BPP_TRANS_CNTL      0x15    /* 8-bit  */
#define BPP_OUT_PINS        0x16    /* 8-bit  */
#define BPP_IN_PINS         0x17    /* 8-bit  */
#define BPP_INT_CNTL        0x18    /* 16-bit */

/*
 * Data register pin assignments (matching bppmidi/sparc-midi-adapter)
 */
#define BPP_DATA_D0         0x01    /* MIDI OUT serial data */
#define BPP_DATA_D1         0x02    /* +5V reference (held high) */
#define BPP_DATA_D2         0x04    /* MIDI THRU serial data */
#define BPP_DATA_D7         0x80    /* MIDI IN opto VCC (held high) */
#define BPP_DATA_IDLE       0x87    /* All lines idle (D7|D2|D1|D0) */

/*
 * DMA CSR bits (minimal — enough for driver attach)
 */
#define BPP_CSR_INT_PEND    (1 << 0)
#define BPP_CSR_ERR_PEND    (1 << 1)
#define BPP_CSR_DRAIN       (1 << 2)
#define BPP_CSR_INT_EN      (1 << 4)
#define BPP_CSR_RESET       (1 << 7)

/*
 * MIDI bit-bang decoder states
 *
 * The bppmidi driver writes exactly 10 times per MIDI byte:
 *   1 start bit (D0=0) + 8 data bits (D0=0/1 LSB-first) + 1 stop bit (D0=1)
 * Each write is separated by drv_usecwait(32) in the guest.
 *
 * We reconstruct the byte by counting writes after the start bit.
 */
typedef enum {
    MIDI_DECODE_IDLE,       /* Waiting for start bit (D0 HIGH->LOW) */
    MIDI_DECODE_DATA,       /* Collecting 8 data bits */
    MIDI_DECODE_STOP,       /* Expecting stop bit */
} MidiDecodeState;

#define TYPE_SUN_BPP "sun-bpp"
OBJECT_DECLARE_SIMPLE_TYPE(SunBppState, SUN_BPP)

struct SunBppState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    /* BPP registers */
    uint32_t dma_csr;
    uint32_t dma_addr;
    uint32_t dma_bcnt;
    uint16_t hw_config;
    uint16_t op_config;
    uint8_t  data;
    uint8_t  trans_cntl;
    uint8_t  out_pins;
    uint8_t  in_pins;
    uint16_t int_cntl;

    /* MIDI bit-bang decoder */
    uint8_t  decode_state;      /* MidiDecodeState value */
    uint8_t  decode_byte;       /* Byte being assembled */
    uint8_t  decode_bit_count;  /* Bits received so far (0-7) */
    uint8_t  prev_d0;           /* Previous D0 value for edge detection */

    /* MIDI backend */
    SunBppMidiBackend *midi_backend;
};

/*
 * MIDI bit-bang decoder — called on every write to the data register.
 *
 * Watches D0 transitions and reconstructs MIDI bytes using a
 * write-counting state machine.
 */
static void sun_bpp_midi_decode(SunBppState *state, uint8_t new_data)
{
    uint8_t current_d0 = new_data & BPP_DATA_D0;
    uint8_t previous_d0 = state->prev_d0;

    state->prev_d0 = current_d0;

    switch (state->decode_state) {
    case MIDI_DECODE_IDLE:
        /* Start bit: D0 transitions from HIGH (idle) to LOW */
        if (previous_d0 && !current_d0) {
            state->decode_state = MIDI_DECODE_DATA;
            state->decode_byte = 0;
            state->decode_bit_count = 0;
        }
        break;

    case MIDI_DECODE_DATA:
        /* Sample D0 into the byte, LSB first */
        if (current_d0) {
            state->decode_byte |= (1 << state->decode_bit_count);
        }
        state->decode_bit_count++;

        if (state->decode_bit_count >= 8) {
            state->decode_state = MIDI_DECODE_STOP;
        }
        break;

    case MIDI_DECODE_STOP:
        if (current_d0) {
            /* Valid stop bit — emit the decoded byte */
            if (state->midi_backend) {
                sun_bpp_midi_send(state->midi_backend,
                                  state->decode_byte);
            }
            qemu_log_mask(LOG_GUEST_ERROR & 0, /* silent unless debug */
                          "sun-bpp: MIDI OUT byte 0x%02x\n",
                          state->decode_byte);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sun-bpp: MIDI framing error "
                          "(expected stop bit, D0=0)\n");
        }
        state->decode_state = MIDI_DECODE_IDLE;
        break;
    }
}

/*
 * MMIO read handler
 */
static uint64_t sun_bpp_read(void *opaque, hwaddr addr, unsigned size)
{
    SunBppState *state = SUN_BPP(opaque);

    switch (addr) {
    case BPP_DMA_CSR:
        return state->dma_csr;
    case BPP_DMA_ADDR:
        return state->dma_addr;
    case BPP_DMA_BCNT:
        return state->dma_bcnt;
    case BPP_DMA_UNUSED:
        return 0;
    case BPP_HW_CONFIG:
        return state->hw_config;
    case BPP_OP_CONFIG:
        return state->op_config;
    case BPP_DATA:
        return state->data;
    case BPP_TRANS_CNTL:
        return state->trans_cntl;
    case BPP_OUT_PINS:
        return state->out_pins;
    case BPP_IN_PINS:
        return state->in_pins;
    case BPP_INT_CNTL:
        return state->int_cntl;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sun-bpp: read at unhandled offset 0x%02" HWADDR_PRIx
                      " size %u\n", addr, size);
        return 0;
    }
}

/*
 * MMIO write handler
 */
static void sun_bpp_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    SunBppState *state = SUN_BPP(opaque);

    switch (addr) {
    case BPP_DMA_CSR:
        if (value & BPP_CSR_RESET) {
            state->dma_csr = 0;
        } else {
            state->dma_csr = value & 0xffffffff;
        }
        break;
    case BPP_DMA_ADDR:
        state->dma_addr = value & 0xffffffff;
        break;
    case BPP_DMA_BCNT:
        state->dma_bcnt = value & 0xffffffff;
        break;
    case BPP_DMA_UNUSED:
        break;
    case BPP_HW_CONFIG:
        state->hw_config = value & 0xffff;
        break;
    case BPP_OP_CONFIG:
        state->op_config = value & 0xffff;
        break;
    case BPP_DATA:
        state->data = value & 0xff;
        sun_bpp_midi_decode(state, state->data);
        break;
    case BPP_TRANS_CNTL:
        state->trans_cntl = value & 0xff;
        break;
    case BPP_OUT_PINS:
        state->out_pins = value & 0xff;
        break;
    case BPP_IN_PINS:
        /* Read-only register — writes are ignored */
        break;
    case BPP_INT_CNTL:
        state->int_cntl = value & 0xffff;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sun-bpp: write at unhandled offset 0x%02" HWADDR_PRIx
                      " value 0x%02" PRIx64 " size %u\n",
                      addr, value, size);
        break;
    }
}

static const MemoryRegionOps sun_bpp_ops = {
    .read = sun_bpp_read,
    .write = sun_bpp_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * Device lifecycle
 */

static void sun_bpp_init(Object *obj)
{
    SunBppState *state = SUN_BPP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&state->mmio, obj, &sun_bpp_ops, state,
                          "sun-bpp", BPP_MMIO_SIZE);
    sysbus_init_mmio(sbd, &state->mmio);
    sysbus_init_irq(sbd, &state->irq);
}

static void sun_bpp_realize(DeviceState *dev, Error **errp)
{
    SunBppState *state = SUN_BPP(dev);

    /* Initialize registers to idle state */
    state->dma_csr = 0;
    state->dma_addr = 0;
    state->dma_bcnt = 0;
    state->hw_config = 0;
    state->op_config = 0;
    state->data = BPP_DATA_IDLE;
    state->trans_cntl = 0;
    state->out_pins = 0;
    state->in_pins = 0;          /* nError low = MIDI idle (mark) */
    state->int_cntl = 0;

    /* Initialize MIDI decoder */
    state->decode_state = MIDI_DECODE_IDLE;
    state->decode_byte = 0;
    state->decode_bit_count = 0;
    state->prev_d0 = 1;          /* D0 starts high (idle) */

    /* Initialize MIDI backend */
    state->midi_backend = sun_bpp_midi_init();
    if (!state->midi_backend) {
        qemu_log_mask(LOG_UNIMP,
                      "sun-bpp: MIDI backend unavailable, "
                      "decoded bytes will be discarded\n");
    }
}

static void sun_bpp_unrealize(DeviceState *dev)
{
    SunBppState *state = SUN_BPP(dev);

    if (state->midi_backend) {
        sun_bpp_midi_cleanup(state->midi_backend);
        state->midi_backend = NULL;
    }
}

/*
 * VM state for snapshots/migration
 */
static const VMStateDescription vmstate_sun_bpp = {
    .name = "sun-bpp",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dma_csr, SunBppState),
        VMSTATE_UINT32(dma_addr, SunBppState),
        VMSTATE_UINT32(dma_bcnt, SunBppState),
        VMSTATE_UINT16(hw_config, SunBppState),
        VMSTATE_UINT16(op_config, SunBppState),
        VMSTATE_UINT8(data, SunBppState),
        VMSTATE_UINT8(trans_cntl, SunBppState),
        VMSTATE_UINT8(out_pins, SunBppState),
        VMSTATE_UINT8(in_pins, SunBppState),
        VMSTATE_UINT16(int_cntl, SunBppState),
        VMSTATE_UINT8(decode_state, SunBppState),
        VMSTATE_UINT8(decode_byte, SunBppState),
        VMSTATE_UINT8(decode_bit_count, SunBppState),
        VMSTATE_UINT8(prev_d0, SunBppState),
        VMSTATE_END_OF_LIST()
    }
};

static void sun_bpp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = sun_bpp_realize;
    dc->unrealize = sun_bpp_unrealize;
    dc->vmsd = &vmstate_sun_bpp;
}

static const TypeInfo sun_bpp_info = {
    .name          = TYPE_SUN_BPP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SunBppState),
    .instance_init = sun_bpp_init,
    .class_init    = sun_bpp_class_init,
};

static void sun_bpp_register_types(void)
{
    type_register_static(&sun_bpp_info);
}

type_init(sun_bpp_register_types)
