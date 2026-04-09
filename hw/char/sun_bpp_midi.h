/*
 * Sun BPP MIDI backend interface
 *
 * Platform-specific backends implement these functions to route
 * decoded MIDI bytes from the BPP bit-bang decoder to the host
 * audio/MIDI subsystem.
 *
 * Copyright (c) 2026 Julian Wolfe
 * License: MIT (same as sun_bpp.c)
 */

#ifndef SUN_BPP_MIDI_H
#define SUN_BPP_MIDI_H

#include <stdint.h>

typedef struct SunBppMidiBackend SunBppMidiBackend;

/*
 * Initialize the MIDI backend.
 * Returns a backend handle, or NULL if initialization fails.
 */
SunBppMidiBackend *sun_bpp_midi_init(void);

/*
 * Send a single decoded MIDI byte to the backend.
 * The backend accumulates bytes into complete MIDI messages
 * and forwards them to the host MIDI subsystem.
 */
void sun_bpp_midi_send(SunBppMidiBackend *backend, uint8_t midi_byte);

/*
 * Tear down the MIDI backend and release resources.
 */
void sun_bpp_midi_cleanup(SunBppMidiBackend *backend);

#endif /* SUN_BPP_MIDI_H */
