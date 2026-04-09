/*
 * QEMU Crystal CS4231 audio chip emulation for sun4m (SPARCstation-4/5)
 *
 * Implements the CS4231 codec with APC DMA controller, providing working
 * audio playback for Solaris 7's audiocs driver.
 *
 * Single 0x40-byte MMIO region at cs_base (e.g. 0x6c000000):
 *   0x00-0x0f: CS4231 codec registers (IAR, IDR, Status, PIO)
 *   0x10-0x3f: APC DMA registers (CSR, capture/playback addr+count)
 *
 * Copyright (c) 2006 Fabrice Bellard (original stub)
 * Copyright (c) 2026 Julian Wolfe (audio + APC DMA implementation)
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
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sparc/sun4m_iommu.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "qemu/module.h"
#include "qemu/audio.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "trace.h"

/*
 * CS4231 codec constants
 */
#define CS_CODEC_MMIO_SIZE  0x40

/* Number of samples over which to fade in at playback start,
 * preventing the audible pop from an abrupt DC offset transition. */
#define FADEIN_SAMPLES      64
#define CS_REGS             16
#define CS_DREGS            32
#define CS_MAXDREG          (CS_DREGS - 1)

#define CS_RAP(s)           ((s)->codec_direct_regs[0] & CS_MAXDREG)
#define CS_VER              0xa0
#define CS_CDC_VER          0x8a

/* Index_Address register bits */
#define CS_IAR_MCE          (1 << 6)    /* Mode Change Enable */
#define CS_IAR_TRD          (1 << 5)    /* Transfer Request Disable */

/* Interface_Configuration (indirect reg 9) bits */
#define CS_IC_PEN           (1 << 0)    /* Playback Enable */
#define CS_IC_CEN           (1 << 1)    /* Capture Enable */

/* MODE_And_ID (indirect reg 12) bits */
#define CS_MODE2            (1 << 6)

/* Alternate_Feature_Status (indirect reg 24) bits */
#define CS_AFS_PI           (1 << 4)    /* Playback Interrupt */
#define CS_AFS_CI           (1 << 5)    /* Capture Interrupt */
#define CS_AFS_TI           (1 << 6)    /* Timer Interrupt */
#define CS_AFS_PMCE         (1 << 4)    /* Playback MCE (shared bit) */

/* Status register (direct reg 2) bits */
#define CS_STATUS_INT       (1 << 0)

/* Indirect register indices */
enum {
    CS_LEFT_ADC_INPUT           = 0,
    CS_RIGHT_ADC_INPUT          = 1,
    CS_LEFT_AUX1_INPUT          = 2,
    CS_RIGHT_AUX1_INPUT         = 3,
    CS_LEFT_AUX2_INPUT          = 4,
    CS_RIGHT_AUX2_INPUT         = 5,
    CS_LEFT_DAC_OUTPUT          = 6,
    CS_RIGHT_DAC_OUTPUT         = 7,
    CS_FS_AND_PLAYBACK_FORMAT   = 8,
    CS_INTERFACE_CONFIG         = 9,
    CS_PIN_CONTROL              = 10,
    CS_ERROR_STATUS_AND_INIT    = 11,
    CS_MODE_AND_ID              = 12,
    CS_LOOPBACK_CONTROL         = 13,
    CS_PLAYBACK_UPPER_BASE      = 14,
    CS_PLAYBACK_LOWER_BASE      = 15,
    CS_ALT_FEATURE_ENABLE_I     = 16,
    CS_ALT_FEATURE_ENABLE_II    = 17,
    CS_LEFT_LINE_INPUT          = 18,
    CS_RIGHT_LINE_INPUT         = 19,
    CS_TIMER_LOW_BASE           = 20,
    CS_TIMER_HIGH_BASE          = 21,
    CS_RESERVED                 = 22,
    CS_ALT_FEATURE_ENABLE_III   = 23,
    CS_ALT_FEATURE_STATUS       = 24,
    CS_VERSION_CHIP_ID          = 25,
    CS_MONO_IO_CONTROL          = 26,
    CS_RESERVED_2               = 27,
    CS_CAPTURE_DATA_FORMAT      = 28,
    CS_RESERVED_3               = 29,
    CS_CAPTURE_UPPER_BASE       = 30,
    CS_CAPTURE_LOWER_BASE       = 31,
};

/*
 * APC DMA register offsets — within the codec's 0x40 byte MMIO region.
 * On real SS-4 hardware, the first 0x10 bytes are CS4231 codec registers
 * and the remaining 0x30 bytes are APC DMA registers.
 *
 * Layout confirmed from Solaris 7 audiocs driver disassembly + ss4.bin FCode:
 *   - FCode: PNVA at +0x38, PNC at +0x3c, CNVA at +0x28, CNC at +0x2c
 *   - Driver: PBC read from +0x34, CBC read from +0x24
 *   - play_next writes +0x38 (PNVA) and +0x3c (PNC)
 *
 * NOTE: The illumos audio_4231_apcdma.c defines are for the EB2/EBUS variant
 * (PVA=0x24, PBC=0x28, PNVA=0x2c, PNC=0x30) — NOT the APC on SS4.
 * See docs/cs4231-audio/apc-register-layout.md for full analysis.
 */
#define APC_REG_CSR                 0x10
#define APC_REG_CAPTURE_ADDR        0x14    /* CVA — current capture VA */
#define APC_REG_CAPTURE_COUNT       0x24    /* CBC — current capture byte count */
#define APC_REG_CAPTURE_NEXT_ADDR   0x28    /* CNVA — capture next VA */
#define APC_REG_CAPTURE_NEXT_COUNT  0x2c    /* CNC — capture next byte count */
#define APC_REG_PLAYBACK_ADDR       0x30    /* PVA — current play VA */
#define APC_REG_PLAYBACK_COUNT      0x34    /* PBC — current play byte count */
#define APC_REG_PLAYBACK_NEXT_ADDR  0x38    /* PNVA — play next VA */
#define APC_REG_PLAYBACK_NEXT_COUNT 0x3c    /* PNC — play next byte count */

/*
 * APC CSR bits — confirmed from Solaris 7 audiocs driver disassembly
 * and illumos audio_4231.h source.
 *
 * Verified bits (from dis /usr/kernel/drv/audiocs):
 *   pollppipe: CSR & 0x4000 → PM (play pipe empty)
 *   pollpipe:  CSR & 0x800  → CX (capture pipe empty)
 *   playintr:  CSR & 0x400000 → PI (playback interrupt)
 *   start_output ORs: 0x1d1008 = PDMA_GO|PMI_EN|EIE|PIE|IE|0x100000
 *   stop_output ORs: 0x80 = P_ABORT, then polls PM
 *   FCode test: polls CSR & 0x2000000 for DMA completion
 */
#define APC_CSR_RESET               0x00000001  /* Reset DMA engine, R/W */
#define APC_CSR_CDMA_GO             0x00000004  /* Capture DMA go, R/W */
#define APC_CSR_PDMA_GO             0x00000008  /* Playback DMA go, R/W */
#define APC_CSR_LOOPBACK            0x00000010  /* Loopback mode, R/W */
#define APC_CSR_CODEC_PDN           0x00000020  /* Codec power down, R/W */
#define APC_CSR_C_ABORT             0x00000040  /* Capture abort, R/W */
#define APC_CSR_P_ABORT             0x00000080  /* Play abort, R/W */
#define APC_CSR_CXI_EN              0x00000100  /* Capture expired int enable, R/W */
#define APC_CSR_CXI                 0x00000200  /* Capture expired interrupt, W1C */
#define APC_CSR_CD                  0x00000400  /* Capture NVA dirty, R/O */
#define APC_CSR_CX                  0x00000800  /* Capture pipe empty, R/O */
#define APC_CSR_PMI_EN              0x00001000  /* Play pipe empty int enable, R/W */
#define APC_CSR_PD                  0x00002000  /* Play NVA dirty, R/O */
#define APC_CSR_PM                  0x00004000  /* Play pipe empty, R/O */
#define APC_CSR_PMI                 0x00008000  /* Play pipe empty interrupt, W1C */
#define APC_CSR_EIE                 0x00010000  /* Error interrupt enable, R/W */
#define APC_CSR_CIE                 0x00020000  /* Capture interrupt enable, R/W */
#define APC_CSR_PIE                 0x00040000  /* Playback interrupt enable, R/W */
#define APC_CSR_IE                  0x00080000  /* Global interrupt enable, R/W */
#define APC_CSR_EI                  0x00100000  /* Error interrupt, W1C */
#define APC_CSR_CI                  0x00200000  /* Capture interrupt, W1C */
#define APC_CSR_PI                  0x00400000  /* Playback interrupt, W1C */
#define APC_CSR_IP                  0x00800000  /* Interrupt pending, R/O */

/* Pure hardware status bits — driver writes are completely ignored */
#define APC_CSR_RO_STATUS_MASK      (APC_CSR_CD | APC_CSR_CX | \
                                     APC_CSR_PD | APC_CSR_PM | \
                                     APC_CSR_IP)

/*
 * Interrupt status bits — set by hardware, cleared by driver.
 * Write-1-to-clear (W1C): driver reads CSR and writes it back,
 * clearing the bits that were set.  This is confirmed by the
 * illumos cintr handler: "Clear all interrupt pending bits" by
 * writing the read value back.
 */
#define APC_CSR_W1C_MASK            (APC_CSR_CXI | APC_CSR_PMI | \
                                     APC_CSR_EI  | APC_CSR_CI  | \
                                     APC_CSR_PI)

/*
 * Frequency tables (from CS4231A datasheet)
 * Index: [crystal_select][divider]
 * Crystal 0 (xtal1 = 24.576 MHz), Crystal 1 (xtal2 = 16.9344 MHz)
 */
static const int codec_sample_frequencies[2][8] = {
    { 8000, 16000, 27420, 32000,    -1,    -1, 48000,  9600 },
    { 5510, 11025, 18900, 22050, 37800, 44100, 33075,  6620 }
};

/*
 * Mu-law decompression table
 * Maps 8-bit mu-law encoded samples to 16-bit linear PCM
 */
static const int16_t mulaw_decompress_table[256] = {
     -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
     -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
     -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
     -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
      -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
      -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
      -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
      -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
      -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
      -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
       -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
       -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
       -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
       -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
       -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
        -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
      32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
      23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
      15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
      11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
       7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
       5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
       3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
       2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
       1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
       1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
        876,   844,   812,   780,   748,   716,   684,   652,
        620,   588,   556,   524,   492,   460,   428,   396,
        372,   356,   340,   324,   308,   292,   276,   260,
        244,   228,   212,   196,   180,   164,   148,   132,
        120,   112,   104,    96,    88,    80,    72,    64,
         56,    48,    40,    32,    24,    16,     8,     0
};

/*
 * A-law decompression table
 * Maps 8-bit A-law encoded samples to 16-bit linear PCM
 */
static const int16_t alaw_decompress_table[256] = {
     -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736,
     -7552, -7296, -8064, -7808, -6528, -6272, -7040, -6784,
     -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368,
     -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392,
     -22016,-20992,-24064,-23040,-17920,-16896,-19968,-18944,
     -30208,-29184,-32256,-31232,-26112,-25088,-28160,-27136,
     -11008,-10496,-12032,-11520, -8960, -8448, -9984, -9472,
     -15104,-14592,-16128,-15616,-13056,-12544,-14080,-13568,
       -344,  -328,  -376,  -360,  -280,  -264,  -312,  -296,
       -472,  -456,  -504,  -488,  -408,  -392,  -440,  -424,
        -88,   -72,  -120,  -104,   -24,    -8,   -56,   -40,
       -216,  -200,  -248,  -232,  -152,  -136,  -184,  -168,
      -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184,
      -1888, -1824, -2016, -1952, -1632, -1568, -1760, -1696,
       -688,  -656,  -752,  -720,  -560,  -528,  -624,  -592,
       -944,  -912, -1008,  -976,  -816,  -784,  -880,  -848,
       5504,  5248,  6016,  5760,  4480,  4224,  4992,  4736,
       7552,  7296,  8064,  7808,  6528,  6272,  7040,  6784,
       2752,  2624,  3008,  2880,  2240,  2112,  2496,  2368,
       3776,  3648,  4032,  3904,  3264,  3136,  3520,  3392,
      22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944,
      30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136,
      11008, 10496, 12032, 11520,  8960,  8448,  9984,  9472,
      15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568,
        344,   328,   376,   360,   280,   264,   312,   296,
        472,   456,   504,   488,   408,   392,   440,   424,
         88,    72,   120,   104,    24,     8,    56,    40,
        216,   200,   248,   232,   152,   136,   184,   168,
       1376,  1312,  1504,  1440,  1120,  1056,  1248,  1184,
       1888,  1824,  2016,  1952,  1632,  1568,  1760,  1696,
        688,   656,   752,   720,   560,   528,   624,   592,
        944,   912,  1008,   976,   816,   784,   880,   848
};

/*
 * Device type and state
 */
#define TYPE_SUN_CS4231 "sun-CS4231"
typedef struct SunCS4231State SunCS4231State;
DECLARE_INSTANCE_CHECKER(SunCS4231State, SUN_CS4231, TYPE_SUN_CS4231)

struct SunCS4231State {
    SysBusDevice parent_obj;

    /* MMIO regions */
    MemoryRegion codec_mmio;        /* CS4231 codec registers */
    MemoryRegion apc_dma_mmio;      /* APC DMA registers */

    qemu_irq codec_irq;            /* interrupt line (slavio_irq[5]) */

    /* CS4231 codec register state */
    uint32_t codec_direct_regs[CS_REGS];
    uint8_t  codec_indirect_regs[CS_DREGS];

    /* APC DMA register state */
    uint32_t apc_csr;
    uint32_t playback_dvma_address;
    uint32_t playback_byte_count;
    uint32_t playback_next_dvma_address;
    uint32_t playback_next_byte_count;
    uint32_t capture_dvma_address;
    uint32_t capture_byte_count;
    uint32_t capture_next_dvma_address;
    uint32_t capture_next_byte_count;

    /* IOMMU link for DVMA address translation */
    IOMMUState *iommu;

    /* Audio backend connection */
    AudioBackend *audio_backend;
    SWVoiceOut *playback_voice;

    /* DMA engine runtime state */
    bool playback_dma_active;
    int audio_bytes_available;

    /* DMA completion timer — fires at the correct time based on
     * buffer size and sample rate, delivering the interrupt to
     * the driver at the right moment regardless of when the audio
     * backend actually consumes the data. */
    QEMUTimer *completion_timer;
    int current_sample_rate;

    /* Auto-calibration simulation counter */
    int aci_counter;

    /* Current audio format state */
    int sample_shift;
    const int16_t *xlaw_decode_table;

    /* Fade-in counter to avoid pop at playback start.
     * Counts down from FADEIN_SAMPLES; while > 0, output is
     * scaled from 0 to full volume over that many samples. */
    int fadein_remaining;

    /* Intermediate buffer for DMA reads */
    uint8_t dma_read_buffer[4096];
};

/* ======================================================================
 * IRQ Management
 * ====================================================================== */

static void suncs4231_update_irq(SunCS4231State *state)
{
    bool should_assert = false;

    /*
     * The APC asserts an interrupt when IE (global enable) is set
     * and any enabled interrupt source is pending.
     *
     * Interrupt sources:
     *   PI  (playback interrupt)       gated by PIE
     *   CI  (capture interrupt)        gated by CIE
     *   EI  (error interrupt)          gated by EIE
     *   PMI (play pipe empty interrupt) gated by PMI_EN
     *   CXI (capture expired interrupt) gated by CXI_EN
     */
    if (state->apc_csr & APC_CSR_IE) {
        if ((state->apc_csr & APC_CSR_PI) && (state->apc_csr & APC_CSR_PIE)) {
            should_assert = true;
        }
        if ((state->apc_csr & APC_CSR_CI) && (state->apc_csr & APC_CSR_CIE)) {
            should_assert = true;
        }
        if ((state->apc_csr & APC_CSR_EI) && (state->apc_csr & APC_CSR_EIE)) {
            should_assert = true;
        }
        if ((state->apc_csr & APC_CSR_PMI) && (state->apc_csr & APC_CSR_PMI_EN)) {
            should_assert = true;
        }
        if ((state->apc_csr & APC_CSR_CXI) && (state->apc_csr & APC_CSR_CXI_EN)) {
            should_assert = true;
        }
    }

    if (should_assert) {
        state->apc_csr |= APC_CSR_IP;
        trace_cs4231_irq_raise(state->apc_csr);
        qemu_irq_raise(state->codec_irq);
    } else {
        state->apc_csr &= ~APC_CSR_IP;
        trace_cs4231_irq_lower(state->apc_csr);
        qemu_irq_lower(state->codec_irq);
    }
}

/* Forward declarations */
static void suncs4231_audio_callback(void *opaque, int free_bytes);
static uint64_t suncs4231_apc_dma_read(void *opaque, hwaddr address,
                                       unsigned size);
static void suncs4231_apc_dma_write(void *opaque, hwaddr address,
                                    uint64_t value, unsigned size);

/* ======================================================================
 * Audio Format Configuration
 * ====================================================================== */

static void suncs4231_reconfigure_voice(SunCS4231State *state,
                                        uint32_t format_register_value)
{
    struct audsettings audio_settings;
    int crystal_select;
    int frequency_index;
    int sample_frequency;

    crystal_select = format_register_value & 1;
    frequency_index = (format_register_value >> 1) & 7;
    sample_frequency = codec_sample_frequencies[crystal_select][frequency_index];

    if (sample_frequency == -1) {
        /* Unsupported frequency combination — leave voice unchanged */
        return;
    }

    state->current_sample_rate = sample_frequency *
        ((format_register_value & (1 << 4)) ? 2 : 1);
    audio_settings.freq = sample_frequency;
    audio_settings.nchannels = (format_register_value & (1 << 4)) ? 2 : 1;
    audio_settings.big_endian = false;
    state->xlaw_decode_table = NULL;

    switch ((format_register_value >> 5) &
            ((state->codec_indirect_regs[CS_MODE_AND_ID] & CS_MODE2) ? 7 : 3)) {
    case 0: /* 8-bit unsigned */
        audio_settings.fmt = AUDIO_FORMAT_U8;
        state->sample_shift = audio_settings.nchannels == 2 ? 1 : 0;
        break;

    case 1: /* mu-law compressed */
        state->xlaw_decode_table = mulaw_decompress_table;
        audio_settings.fmt = AUDIO_FORMAT_S16;
        audio_settings.big_endian = HOST_BIG_ENDIAN;
        state->sample_shift = audio_settings.nchannels == 2 ? 1 : 0;
        break;

    case 3: /* A-law compressed */
        state->xlaw_decode_table = alaw_decompress_table;
        audio_settings.fmt = AUDIO_FORMAT_S16;
        audio_settings.big_endian = HOST_BIG_ENDIAN;
        state->sample_shift = audio_settings.nchannels == 2 ? 1 : 0;
        break;

    case 6: /* 16-bit signed, big-endian (native SPARC byte order) */
        audio_settings.big_endian = true;
        /* fall through */
    case 2: /* 16-bit signed, little-endian */
        audio_settings.fmt = AUDIO_FORMAT_S16;
        state->sample_shift = audio_settings.nchannels;
        break;

    default:
        /* Reserved or ADPCM — not supported */
        return;
    }

    trace_cs4231_codec_reconfigure(sample_frequency,
                                   audio_settings.nchannels,
                                   audio_settings.fmt);

    state->playback_voice = audio_be_open_out(
        state->audio_backend,
        state->playback_voice,
        "sun-cs4231",
        state,
        suncs4231_audio_callback,
        &audio_settings
    );

    /* Voice stays inactive until DMA start activates it */
}

/* ======================================================================
 * DMA Playback Engine
 * ====================================================================== */

/*
 * Apply linear fade-in to 16-bit signed PCM samples, preventing
 * the pop caused by an abrupt transition from silence to audio.
 */
static void suncs4231_apply_fadein_s16(SunCS4231State *state,
                                       int16_t *samples, int sample_count)
{
    int fade_index;

    for (fade_index = 0; fade_index < sample_count; fade_index++) {
        if (state->fadein_remaining <= 0) {
            break;
        }
        int scale = FADEIN_SAMPLES - state->fadein_remaining;
        samples[fade_index] = (int16_t)((int32_t)samples[fade_index] *
                                         scale / FADEIN_SAMPLES);
        state->fadein_remaining--;
    }
}

/*
 * Apply linear fade-in to 8-bit unsigned PCM samples (center = 128).
 */
static void suncs4231_apply_fadein_u8(SunCS4231State *state,
                                      uint8_t *samples, int sample_count)
{
    int fade_index;

    for (fade_index = 0; fade_index < sample_count; fade_index++) {
        if (state->fadein_remaining <= 0) {
            break;
        }
        int scale = FADEIN_SAMPLES - state->fadein_remaining;
        int centered = (int)samples[fade_index] - 128;
        centered = centered * scale / FADEIN_SAMPLES;
        samples[fade_index] = (uint8_t)(centered + 128);
        state->fadein_remaining--;
    }
}

static void suncs4231_pump_playback(SunCS4231State *state)
{
    int bytes_to_transfer;
    int bytes_written;

    if (!state->playback_dma_active) {
        return;
    }
    if (state->playback_byte_count == 0) {
        return;
    }
    if (state->audio_bytes_available <= 0) {
        return;
    }
    if (!state->iommu) {
        return;
    }

    while (state->playback_byte_count > 0 && state->audio_bytes_available > 0) {
        /*
         * For xlaw formats, each input byte expands to 2 output bytes.
         * Limit the DMA read to half the available audio buffer space.
         */
        if (state->xlaw_decode_table) {
            bytes_to_transfer = MIN(state->audio_bytes_available / 2,
                                    (int)state->playback_byte_count);
        } else {
            bytes_to_transfer = MIN(state->audio_bytes_available,
                                    (int)state->playback_byte_count);
        }
        bytes_to_transfer = MIN(bytes_to_transfer,
                                (int)sizeof(state->dma_read_buffer));

        if (bytes_to_transfer <= 0) {
            break;
        }

        trace_cs4231_dma_playback_transfer(state->playback_dvma_address,
                                           bytes_to_transfer);

        /* Read audio data from guest memory through the IOMMU */
        {
            MemTxResult dma_result;
            dma_result = dma_memory_read(&state->iommu->iommu_as,
                            state->playback_dvma_address,
                            state->dma_read_buffer,
                            bytes_to_transfer,
                            MEMTXATTRS_UNSPECIFIED);
            if (dma_result != MEMTX_OK) {
                /* Output silence instead of garbage */
                memset(state->dma_read_buffer, 0xff, bytes_to_transfer);
            }
        }

        if (state->xlaw_decode_table) {
            /*
             * Expand xlaw-encoded bytes to 16-bit linear PCM.
             * Each input byte produces one int16_t output sample.
             */
            int16_t linear_buffer[sizeof(state->dma_read_buffer)];
            int sample_index;

            for (sample_index = 0; sample_index < bytes_to_transfer;
                 sample_index++) {
                linear_buffer[sample_index] =
                    state->xlaw_decode_table[state->dma_read_buffer[sample_index]];
            }

            if (state->fadein_remaining > 0) {
                suncs4231_apply_fadein_s16(state, linear_buffer,
                                           bytes_to_transfer);
            }

            bytes_written = audio_be_write(state->audio_backend,
                                           state->playback_voice,
                                           linear_buffer,
                                           bytes_to_transfer * 2);
            /* Convert output bytes back to input bytes consumed */
            bytes_written /= 2;
        } else {
            if (state->fadein_remaining > 0) {
                if (state->sample_shift >= 2) {
                    /* 16-bit stereo or mono — 2+ bytes per sample */
                    suncs4231_apply_fadein_s16(state,
                        (int16_t *)state->dma_read_buffer,
                        bytes_to_transfer / 2);
                } else {
                    /* 8-bit unsigned */
                    suncs4231_apply_fadein_u8(state,
                        state->dma_read_buffer,
                        bytes_to_transfer);
                }
            }
            bytes_written = audio_be_write(state->audio_backend,
                                           state->playback_voice,
                                           state->dma_read_buffer,
                                           bytes_to_transfer);
        }

        if (bytes_written <= 0) {
            break;
        }

        state->playback_dvma_address += bytes_written;
        state->playback_byte_count -= bytes_written;

        if (state->xlaw_decode_table) {
            state->audio_bytes_available -= bytes_written * 2;
        } else {
            state->audio_bytes_available -= bytes_written;
        }
    }

    /*
     * Check for buffer completion — swap to next buffer if available
     */
    /*
     * Buffer completion is handled by the completion_timer, not here.
     * The audio callback may exhaust the data instantly, but the
     * interrupt is delivered at the correct virtual time.
     */
}

static void suncs4231_audio_callback(void *opaque, int free_bytes)
{
    SunCS4231State *state = opaque;

    state->audio_bytes_available = free_bytes;
    suncs4231_pump_playback(state);
}

/*
 * DMA completion timer callback.
 * Fires after the buffer duration has elapsed (buffer_bytes / sample_rate),
 * delivering PM/PMI/PI interrupts at the correct time.
 * The audio callback may have already consumed the data, but the driver
 * expects the interrupt at the real-time completion point.
 */
static void suncs4231_completion_timer_cb(void *opaque)
{
    SunCS4231State *state = opaque;

    if (!state->playback_dma_active) {
        return;
    }

    /*
     * If the audio callback hasn't finished consuming all data yet
     * (backend buffer was full or callback timing mismatch), reschedule
     * to fire when the remaining bytes are consumed. Only deliver the
     * interrupt when the buffer is truly complete (PBC == 0).
     */
    if (state->playback_byte_count > 0) {
        int64_t remaining_ns = (int64_t)state->playback_byte_count *
                               NANOSECONDS_PER_SECOND /
                               MAX(state->current_sample_rate, 1);
        if (remaining_ns < 1000000) {
            remaining_ns = 1000000; /* min 1ms to avoid busy-loop */
        }
        timer_mod(state->completion_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + remaining_ns);
        return;
    }

    /* Signal buffer completion — PBC is 0, buffer fully consumed */
    state->apc_csr |= APC_CSR_PM;    /* play pipe empty */
    state->apc_csr |= APC_CSR_PMI;   /* play pipe empty interrupt */
    state->apc_csr |= APC_CSR_PI;    /* playback interrupt */
    state->apc_csr |= APC_CSR_PD;    /* NVA dirty (ready for new addr) */

    /* Check if there's a next buffer queued */
    if (state->playback_next_byte_count > 0) {
        state->playback_dvma_address = state->playback_next_dvma_address;
        state->playback_byte_count = state->playback_next_byte_count;
        state->playback_next_dvma_address = 0;
        state->playback_next_byte_count = 0;
        state->apc_csr &= ~APC_CSR_PM;  /* pipe no longer empty */

        /* Schedule next completion */
        int64_t duration_ns = (int64_t)state->playback_byte_count *
                              NANOSECONDS_PER_SECOND /
                              MAX(state->current_sample_rate, 1);
        timer_mod(state->completion_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + duration_ns);
    } else {
        /*
         * No next buffer — signal completion but keep PDMA_GO set.
         * The driver's ISR will load the next buffer or stop DMA.
         * Don't clear PDMA_GO here — the driver expects to see it
         * still set when the ISR runs.
         */
        state->playback_dma_active = false;
    }

    suncs4231_update_irq(state);
}

static void suncs4231_start_playback(SunCS4231State *state)
{
    int64_t duration_ns;

    if (state->playback_dma_active) {
        return;
    }
    if (state->playback_byte_count == 0) {
        return;
    }

    state->playback_dma_active = true;
    state->fadein_remaining = FADEIN_SAMPLES;
    state->apc_csr &= ~APC_CSR_PM;  /* pipe no longer empty */

    /* Activate voice — data is ready so the synchronous callback
     * will immediately pump audio into the backend. */
    audio_be_set_active_out(state->audio_backend,
                            state->playback_voice, 1);

    /* Schedule the completion timer for the buffer duration */
    duration_ns = (int64_t)state->playback_byte_count *
                  NANOSECONDS_PER_SECOND /
                  MAX(state->current_sample_rate, 1);
    timer_mod(state->completion_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + duration_ns);
}

static void suncs4231_stop_playback(SunCS4231State *state)
{
    if (!state->playback_dma_active) {
        return;
    }

    state->playback_dma_active = false;
    timer_del(state->completion_timer);
}

/* ======================================================================
 * CS4231 Codec MMIO Handlers
 * ====================================================================== */

static uint64_t suncs4231_codec_read(void *opaque, hwaddr address,
                                     unsigned size)
{
    SunCS4231State *state = opaque;
    uint32_t register_index;
    uint32_t result;

    /* Offsets 0x10+ are APC DMA registers */
    if (address >= APC_REG_CSR) {
        return suncs4231_apc_dma_read(opaque, address, size);
    }

    register_index = address >> 2;

    switch (register_index) {
    case 0: /* Index Address Register */
        result = state->codec_direct_regs[0] & ~0x80;
        break;

    case 1: /* Index Data Register — read indirect register via RAP */
        {
            uint32_t indirect_index;
            if (!(state->codec_indirect_regs[CS_MODE_AND_ID] & CS_MODE2)) {
                indirect_index = state->codec_direct_regs[0] & 0x0f;
            } else {
                indirect_index = state->codec_direct_regs[0] & 0x1f;
            }

            result = state->codec_indirect_regs[indirect_index];

            /*
             * Error_Status_And_Init (reg 11): simulate auto-calibration.
             * After MCE is cleared, the ACI bit (bit 5) stays set for a
             * few reads, then clears to signal calibration complete.
             */
            if (indirect_index == CS_ERROR_STATUS_AND_INIT) {
                if (state->aci_counter > 0) {
                    result |= (1 << 5);  /* ACI — calibration in progress */
                    state->aci_counter--;
                }
            }

            trace_cs4231_codec_read_dreg(indirect_index, result);
            return result;
        }

    case 2: /* Status Register */
        result = state->codec_direct_regs[2];
        break;

    default:
        result = state->codec_direct_regs[register_index];
        trace_cs4231_codec_read_reg(register_index, result);
        break;
    }

    return result;
}

static void suncs4231_codec_write(void *opaque, hwaddr address,
                                  uint64_t value, unsigned size)
{
    SunCS4231State *state = opaque;
    uint32_t register_index;

    /* Offsets 0x10+ are APC DMA registers */
    if (address >= APC_REG_CSR) {
        suncs4231_apc_dma_write(opaque, address, value, size);
        return;
    }

    register_index = address >> 2;

    trace_cs4231_codec_write_reg(register_index,
                                 state->codec_direct_regs[register_index],
                                 (uint32_t)value);

    switch (register_index) {
    case 0: /* Index Address Register */
        /*
         * Auto-calibration starts when MCE transitions 1→0 (cleared).
         * The CS4231 performs internal calibration after a mode change.
         * Set aci_counter so reads of Error_Status (reg 11) will report
         * ACI (bit 5) as set, then clear after enough reads.
         */
        if ((state->codec_direct_regs[0] & CS_IAR_MCE) && !(value & CS_IAR_MCE)) {
            state->aci_counter = 2;
        }
        state->codec_direct_regs[0] = value & ~(1 << 7);
        break;

    case 1: /* Index Data Register — write indirect register via RAP */
        {
            uint32_t indirect_index;
            if (!(state->codec_indirect_regs[CS_MODE_AND_ID] & CS_MODE2)) {
                indirect_index = state->codec_direct_regs[0] & 0x0f;
            } else {
                indirect_index = state->codec_direct_regs[0] & 0x1f;
            }

            trace_cs4231_codec_write_dreg(indirect_index,
                                          state->codec_indirect_regs[indirect_index],
                                          (uint32_t)value);

            switch (indirect_index) {
            case CS_ERROR_STATUS_AND_INIT:
            case CS_VERSION_CHIP_ID:
                /* Read-only registers — ignore writes */
                break;

            case CS_FS_AND_PLAYBACK_FORMAT:
                /*
                 * Format/rate change only takes effect when MCE is set,
                 * or when PMCE (Playback MCE) is set in Alternate Feature Status.
                 */
                if ((state->codec_direct_regs[0] & CS_IAR_MCE) ||
                    (state->codec_indirect_regs[CS_ALT_FEATURE_STATUS] &
                     CS_AFS_PMCE)) {
                    state->codec_indirect_regs[indirect_index] = value;
                    suncs4231_reconfigure_voice(state, value);
                }
                break;

            case CS_INTERFACE_CONFIG:
                value &= ~(1 << 5);  /* Bit 5 is reserved */
                state->codec_indirect_regs[indirect_index] = value;

                /*
                 * PEN (Playback Enable) controls whether audio output is active.
                 * Combined with PDMA_GO from the APC CSR.
                 */
                if ((value & CS_IC_PEN) &&
                    (state->apc_csr & APC_CSR_PDMA_GO)) {
                    suncs4231_start_playback(state);
                } else if (!(value & CS_IC_PEN)) {
                    suncs4231_stop_playback(state);
                }
                break;

            case CS_MODE_AND_ID:
                if (value & CS_MODE2) {
                    state->codec_indirect_regs[indirect_index] |= CS_MODE2;
                } else {
                    state->codec_indirect_regs[indirect_index] &= ~CS_MODE2;
                }
                break;

            case CS_ALT_FEATURE_STATUS:
                /*
                 * Writing 0 to PI/CI/TI bits clears them and may lower IRQ.
                 * These are write-1-to-clear in some interpretations, but
                 * the cs4231a.c reference treats them as direct write.
                 */
                if ((state->codec_indirect_regs[indirect_index] & CS_AFS_PI)
                    && !(value & CS_AFS_PI)) {
                    qemu_irq_lower(state->codec_irq);
                    state->codec_direct_regs[2] &= ~CS_STATUS_INT;
                }
                state->codec_indirect_regs[indirect_index] = value;
                break;

            default:
                state->codec_indirect_regs[indirect_index] = value;
                break;
            }
        }
        break;

    case 2: /* Status Register — write to clear INT */
        if (state->codec_direct_regs[2] & CS_STATUS_INT) {
            qemu_irq_lower(state->codec_irq);
        }
        state->codec_direct_regs[2] &= ~CS_STATUS_INT;
        state->codec_indirect_regs[CS_ALT_FEATURE_STATUS] &=
            ~(CS_AFS_PI | CS_AFS_CI | CS_AFS_TI);
        break;

    case 4: /* Reset register */
        if (value & 1) {
            /* Software reset */
            memset(state->codec_direct_regs, 0,
                   CS_REGS * sizeof(uint32_t));
            memset(state->codec_indirect_regs, 0, CS_DREGS);
            state->codec_indirect_regs[CS_MODE_AND_ID] = CS_CDC_VER;
            state->codec_indirect_regs[CS_VERSION_CHIP_ID] = CS_VER;
        }
        value &= 0x7f;
        state->codec_direct_regs[register_index] = value;
        break;

    default:
        state->codec_direct_regs[register_index] = value;
        break;
    }
}

static const MemoryRegionOps suncs4231_codec_ops = {
    .read = suncs4231_codec_read,
    .write = suncs4231_codec_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* ======================================================================
 * APC DMA MMIO Handlers
 * ====================================================================== */

static uint64_t suncs4231_apc_dma_read(void *opaque, hwaddr address,
                                       unsigned size)
{
    SunCS4231State *state = opaque;
    uint32_t result = 0;

    switch (address) {
    case APC_REG_CSR:
        result = state->apc_csr;
        break;
    case APC_REG_CAPTURE_ADDR:
        result = state->capture_dvma_address;
        break;
    case APC_REG_CAPTURE_COUNT:
        result = state->capture_byte_count;
        break;
    case APC_REG_CAPTURE_NEXT_ADDR:
        result = state->capture_next_dvma_address;
        break;
    case APC_REG_CAPTURE_NEXT_COUNT:
        result = state->capture_next_byte_count;
        break;
    case APC_REG_PLAYBACK_ADDR:
        result = state->playback_dvma_address;
        break;
    case APC_REG_PLAYBACK_COUNT:
        result = state->playback_byte_count;
        break;
    case APC_REG_PLAYBACK_NEXT_ADDR:
        result = state->playback_next_dvma_address;
        break;
    case APC_REG_PLAYBACK_NEXT_COUNT:
        result = state->playback_next_byte_count;
        break;
    default:
        result = 0;
        break;
    }

    trace_cs4231_apc_dma_read_reg((uint32_t)address, result);

    return result;
}

static void suncs4231_apc_dma_write(void *opaque, hwaddr address,
                                    uint64_t value, unsigned size)
{
    SunCS4231State *state = opaque;
    uint32_t old_csr;

    switch (address) {
    case APC_REG_CSR:
        old_csr = state->apc_csr;
        trace_cs4231_apc_dma_write_reg((uint32_t)address, old_csr,
                                       (uint32_t)value);

        /* Handle reset */
        if (value & APC_CSR_RESET) {
            suncs4231_stop_playback(state);
            state->apc_csr = APC_CSR_PM | APC_CSR_CX;  /* pipes empty */
            state->playback_dvma_address = 0;
            state->playback_byte_count = 0;
            state->playback_next_dvma_address = 0;
            state->playback_next_byte_count = 0;
            state->capture_dvma_address = 0;
            state->capture_byte_count = 0;
            state->capture_next_dvma_address = 0;
            state->capture_next_byte_count = 0;
            suncs4231_update_irq(state);
            return;
        }

        /*
         * CSR write semantics:
         *   R/O bits (CD, CX, PD, PM, IP): preserved from hardware
         *   W1C bits (CXI, PMI, EI, CI, PI): write-1-to-clear
         *     - writing 1 clears the bit (driver reads and writes back)
         *     - writing 0 preserves the bit
         *   All other bits: plain R/W
         */
        {
            /* Preserve pure R/O hardware status */
            uint32_t new_csr = state->apc_csr & APC_CSR_RO_STATUS_MASK;

            /* W1C interrupt bits: clear where driver wrote 1 */
            uint32_t w1c_clear = value & APC_CSR_W1C_MASK;
            new_csr |= state->apc_csr & ~w1c_clear & APC_CSR_W1C_MASK;

            /* Plain R/W bits from the written value */
            uint32_t rw_mask = ~(APC_CSR_RO_STATUS_MASK | APC_CSR_W1C_MASK);
            new_csr |= value & rw_mask;

            state->apc_csr = new_csr;
        }

        /* P_ABORT: stop playback, set PM when pipe drains */
        if ((value & APC_CSR_P_ABORT) && !(old_csr & APC_CSR_P_ABORT)) {
            suncs4231_stop_playback(state);
            if (state->playback_byte_count == 0) {
                state->apc_csr |= APC_CSR_PM;  /* pipe is already empty */
            }
        }

        /* PDMA_GO transition: start or stop playback DMA */
        if ((value & APC_CSR_PDMA_GO) && !(old_csr & APC_CSR_PDMA_GO)) {
            /* Rising edge: start DMA if codec PEN is also set */
            state->apc_csr &= ~APC_CSR_PM;  /* pipe no longer empty */
            state->apc_csr |= APC_CSR_PD;   /* NVA dirty (ready for addr) */
            if (state->codec_indirect_regs[CS_INTERFACE_CONFIG] & CS_IC_PEN) {
                suncs4231_start_playback(state);
            }
        } else if (!(value & APC_CSR_PDMA_GO) && (old_csr & APC_CSR_PDMA_GO)) {
            /* Falling edge: stop DMA */
            suncs4231_stop_playback(state);
        }

        suncs4231_update_irq(state);
        break;

    case APC_REG_CAPTURE_ADDR:
        trace_cs4231_apc_dma_write_reg((uint32_t)address,
                                       state->capture_dvma_address,
                                       (uint32_t)value);
        state->capture_dvma_address = value;
        break;

    case APC_REG_CAPTURE_COUNT:
        trace_cs4231_apc_dma_write_reg((uint32_t)address,
                                       state->capture_byte_count,
                                       (uint32_t)value);
        state->capture_byte_count = value;
        break;

    case APC_REG_CAPTURE_NEXT_ADDR:
        trace_cs4231_apc_dma_write_reg((uint32_t)address,
                                       state->capture_next_dvma_address,
                                       (uint32_t)value);
        state->capture_next_dvma_address = value;
        break;

    case APC_REG_CAPTURE_NEXT_COUNT:
        trace_cs4231_apc_dma_write_reg((uint32_t)address,
                                       state->capture_next_byte_count,
                                       (uint32_t)value);
        state->capture_next_byte_count = value;
        break;

    case APC_REG_PLAYBACK_ADDR:
        trace_cs4231_apc_dma_write_reg((uint32_t)address,
                                       state->playback_dvma_address,
                                       (uint32_t)value);
        state->playback_dvma_address = value;
        break;

    case APC_REG_PLAYBACK_COUNT:
        trace_cs4231_apc_dma_write_reg((uint32_t)address,
                                       state->playback_byte_count,
                                       (uint32_t)value);
        state->playback_byte_count = value;
        break;

    case APC_REG_PLAYBACK_NEXT_ADDR:
        trace_cs4231_apc_dma_write_reg((uint32_t)address,
                                       state->playback_next_dvma_address,
                                       (uint32_t)value);
        state->playback_next_dvma_address = value;
        break;

    case APC_REG_PLAYBACK_NEXT_COUNT:
        trace_cs4231_apc_dma_write_reg((uint32_t)address,
                                       state->playback_next_byte_count,
                                       (uint32_t)value);
        state->playback_next_byte_count = value;
        /*
         * Writing NC "enables the state machine" (per illumos).
         * Clear PD (NVA consumed), clear PM (pipe has data).
         * If no current buffer, promote next to current immediately.
         */
        state->apc_csr &= ~APC_CSR_PD;
        if (value > 0 && state->playback_byte_count == 0 &&
            state->playback_next_dvma_address != 0) {
            state->playback_dvma_address = state->playback_next_dvma_address;
            state->playback_byte_count = state->playback_next_byte_count;
            state->playback_next_dvma_address = 0;
            state->playback_next_byte_count = 0;
            state->apc_csr &= ~APC_CSR_PM;
            state->apc_csr |= APC_CSR_PD;  /* ready for more */
            /* Start DMA if PDMA_GO and PEN are set */
            if ((state->apc_csr & APC_CSR_PDMA_GO) &&
                (state->codec_indirect_regs[CS_INTERFACE_CONFIG] &
                 CS_IC_PEN)) {
                suncs4231_start_playback(state);
            }
        }
        break;

    default:
        break;
    }
}

/* APC DMA reads/writes are dispatched from suncs4231_codec_ops handlers
 * (offsets >= 0x10 within the unified 0x40-byte MMIO region). */

/* ======================================================================
 * Device Lifecycle: Reset / Realize / Init
 * ====================================================================== */

static void suncs4231_reset(DeviceState *device)
{
    SunCS4231State *state = SUN_CS4231(device);

    /* Stop any active DMA */
    state->playback_dma_active = false;
    state->audio_bytes_available = 0;

    /* Reset codec registers */
    memset(state->codec_direct_regs, 0, CS_REGS * sizeof(uint32_t));
    memset(state->codec_indirect_regs, 0, CS_DREGS);
    state->codec_indirect_regs[CS_MODE_AND_ID] = CS_CDC_VER;
    state->codec_indirect_regs[CS_VERSION_CHIP_ID] = CS_VER;

    /* Reset APC DMA registers — pipes are empty after reset */
    state->apc_csr = APC_CSR_PM | APC_CSR_CX;
    state->playback_dvma_address = 0;
    state->playback_byte_count = 0;
    state->playback_next_dvma_address = 0;
    state->playback_next_byte_count = 0;
    state->capture_dvma_address = 0;
    state->capture_byte_count = 0;
    state->capture_next_dvma_address = 0;
    state->capture_next_byte_count = 0;

    /* Reset format state */
    state->sample_shift = 0;
    state->xlaw_decode_table = NULL;
    state->aci_counter = 0;
    state->current_sample_rate = 8000;
    timer_del(state->completion_timer);
}

static void suncs4231_realize(DeviceState *device, Error **errp)
{
    SunCS4231State *state = SUN_CS4231(device);
    struct audsettings default_audio_settings;

    if (!audio_be_check(&state->audio_backend, errp)) {
        return;
    }

    /*
     * Open an initial voice with default settings.
     * The Solaris driver will reconfigure via FS_And_Playback_Data_Format.
     */
    default_audio_settings.freq = 8000;
    default_audio_settings.nchannels = 1;
    default_audio_settings.fmt = AUDIO_FORMAT_S16;
    default_audio_settings.big_endian = false;

    state->playback_voice = audio_be_open_out(
        state->audio_backend,
        NULL,
        "sun-cs4231",
        state,
        suncs4231_audio_callback,
        &default_audio_settings
    );

    if (!state->playback_voice) {
        error_setg(errp, "sun-CS4231: failed to open audio output");
        return;
    }

    /*
     * Voice starts inactive — activated at DMA start when data is ready.
     * Fade-in (FADEIN_SAMPLES) prevents the pop from abrupt activation.
     * Voice is never deactivated so CoreAudio keeps draining the buffer.
     */
}

static void suncs4231_init(Object *object)
{
    SunCS4231State *state = SUN_CS4231(object);
    SysBusDevice *sysbus_device = SYS_BUS_DEVICE(object);

    /* Single MMIO region: codec (0x00-0x0f) + APC DMA (0x10-0x3f) */
    memory_region_init_io(&state->codec_mmio, object,
                          &suncs4231_codec_ops, state,
                          "sun-cs4231", CS_CODEC_MMIO_SIZE);
    sysbus_init_mmio(sysbus_device, &state->codec_mmio);

    /* IRQ */
    sysbus_init_irq(sysbus_device, &state->codec_irq);

    /* DMA completion timer */
    state->completion_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                            suncs4231_completion_timer_cb, state);

    /* IOMMU link — set from sun4m.c before realize */
    object_property_add_link(object, "iommu", TYPE_SUN4M_IOMMU,
                             (Object **)&state->iommu,
                             qdev_prop_allow_set_link_before_realize,
                             0);
}

/* ======================================================================
 * VMState for Migration / Snapshots
 * ====================================================================== */

static int suncs4231_pre_load(void *opaque)
{
    SunCS4231State *state = opaque;

    /* Stop DMA before loading state */
    state->playback_dma_active = false;
    return 0;
}

static int suncs4231_post_load(void *opaque, int version_id)
{
    SunCS4231State *state = opaque;

    if (version_id < 2) {
        /* Loading v1 state — initialize new fields */
        state->apc_csr = 0;
        state->playback_dvma_address = 0;
        state->playback_byte_count = 0;
        state->playback_next_dvma_address = 0;
        state->playback_next_byte_count = 0;
        state->capture_dvma_address = 0;
        state->capture_byte_count = 0;
        state->capture_next_dvma_address = 0;
        state->capture_next_byte_count = 0;
        state->playback_dma_active = false;
    }

    /* Reconfigure voice from saved codec format register */
    suncs4231_reconfigure_voice(state,
        state->codec_indirect_regs[CS_FS_AND_PLAYBACK_FORMAT]);

    /* If DMA was active, re-activate voice */
    if (state->playback_dma_active) {
        audio_be_set_active_out(state->audio_backend,
                                state->playback_voice, 1);
    }

    return 0;
}

static const VMStateDescription vmstate_suncs4231 = {
    .name = "cs4231",
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_load = suncs4231_pre_load,
    .post_load = suncs4231_post_load,
    .fields = (const VMStateField[]) {
        /* V1 fields (codec registers — backward compatible) */
        VMSTATE_UINT32_ARRAY(codec_direct_regs, SunCS4231State, CS_REGS),
        VMSTATE_UINT8_ARRAY(codec_indirect_regs, SunCS4231State, CS_DREGS),

        /* V2 fields (APC DMA state) */
        VMSTATE_UINT32_V(apc_csr, SunCS4231State, 2),
        VMSTATE_UINT32_V(playback_dvma_address, SunCS4231State, 2),
        VMSTATE_UINT32_V(playback_byte_count, SunCS4231State, 2),
        VMSTATE_UINT32_V(playback_next_dvma_address, SunCS4231State, 2),
        VMSTATE_UINT32_V(playback_next_byte_count, SunCS4231State, 2),
        VMSTATE_UINT32_V(capture_dvma_address, SunCS4231State, 2),
        VMSTATE_UINT32_V(capture_byte_count, SunCS4231State, 2),
        VMSTATE_UINT32_V(capture_next_dvma_address, SunCS4231State, 2),
        VMSTATE_UINT32_V(capture_next_byte_count, SunCS4231State, 2),
        VMSTATE_BOOL_V(playback_dma_active, SunCS4231State, 2),
        VMSTATE_INT32_V(aci_counter, SunCS4231State, 2),

        VMSTATE_END_OF_LIST()
    }
};

/* ======================================================================
 * Device Properties and Type Registration
 * ====================================================================== */

static const Property suncs4231_properties[] = {
    DEFINE_AUDIO_PROPERTIES(SunCS4231State, audio_backend),
};

static void suncs4231_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(device_class, suncs4231_reset);
    device_class->realize = suncs4231_realize;
    device_class->vmsd = &vmstate_suncs4231;
    device_class_set_props(device_class, suncs4231_properties);
    set_bit(DEVICE_CATEGORY_SOUND, device_class->categories);
    device_class->desc = "Crystal CS4231 with APC DMA (sun4m)";
}

static const TypeInfo suncs4231_info = {
    .name          = TYPE_SUN_CS4231,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SunCS4231State),
    .instance_init = suncs4231_init,
    .class_init    = suncs4231_class_init,
};

static void suncs4231_register_types(void)
{
    type_register_static(&suncs4231_info);
}

type_init(suncs4231_register_types)
