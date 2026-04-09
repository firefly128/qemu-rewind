/*
 * Sun BPP MIDI backend — stub (non-macOS platforms)
 *
 * Logs decoded MIDI bytes to stderr when QEMU is started with
 * -d unimp.  No actual MIDI output is produced.
 *
 * Copyright (c) 2026 Julian Wolfe
 * License: MIT (same as sun_bpp.c)
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "sun_bpp_midi.h"

struct SunBppMidiBackend {
    uint8_t placeholder;    /* Struct cannot be empty */
};

SunBppMidiBackend *sun_bpp_midi_init(void)
{
    SunBppMidiBackend *backend = g_new0(SunBppMidiBackend, 1);

    fprintf(stderr, "sun-bpp-midi: stub backend (no CoreMIDI), "
            "MIDI bytes will be logged with -d unimp\n");

    return backend;
}

void sun_bpp_midi_send(SunBppMidiBackend *backend, uint8_t midi_byte)
{
    if (!backend) {
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "sun-bpp-midi: MIDI byte 0x%02x\n", midi_byte);
}

void sun_bpp_midi_cleanup(SunBppMidiBackend *backend)
{
    g_free(backend);
}
