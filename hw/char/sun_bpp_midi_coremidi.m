/*
 * Sun BPP MIDI backend — CoreMIDI (macOS)
 *
 * Creates a virtual MIDI source named "QEMU SPARC MIDI Out" that
 * appears in Audio MIDI Setup and any CoreMIDI-aware application.
 * Decoded MIDI bytes from the BPP bit-bang decoder are accumulated
 * into complete MIDI messages and sent via MIDIReceived().
 *
 * Copyright (c) 2026 Julian Wolfe
 * License: MIT (same as sun_bpp.c)
 */

#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "sun_bpp_midi.h"

/*
 * Maximum MIDI message size.  Standard messages are 1-3 bytes,
 * but SysEx can be arbitrarily long.  We cap at 256 bytes which
 * is generous for any real-world MIDI stream from a game or app.
 */
#define MIDI_MSG_MAX_SIZE   256

/*
 * MIDI message length table indexed by status byte upper nibble.
 * Includes the status byte itself.
 *   0x8n Note Off:        3 bytes
 *   0x9n Note On:         3 bytes
 *   0xAn Poly Aftertouch: 3 bytes
 *   0xBn Control Change:  3 bytes
 *   0xCn Program Change:  2 bytes
 *   0xDn Channel Pressure: 2 bytes
 *   0xEn Pitch Bend:      3 bytes
 *   0xFn System:          variable (handled separately)
 */
static const int midi_channel_message_length[8] = {
    3, 3, 3, 3, 2, 2, 3, 0  /* 0 = variable/system */
};

struct SunBppMidiBackend {
    MIDIClientRef   client;
    MIDIEndpointRef source;

    /* Message accumulator */
    uint8_t  message_buffer[MIDI_MSG_MAX_SIZE];
    int      message_position;
    int      message_expected_length;
    uint8_t  running_status;
    bool     in_sysex;
};

/*
 * Send the accumulated message buffer as a MIDIPacket.
 */
static void coremidi_flush_message(SunBppMidiBackend *backend)
{
    if (backend->message_position == 0) {
        return;
    }

    MIDIPacketList packet_list;
    MIDIPacket *packet = MIDIPacketListInit(&packet_list);
    packet = MIDIPacketListAdd(&packet_list, sizeof(packet_list),
                               packet, mach_absolute_time(),
                               backend->message_position,
                               backend->message_buffer);

    if (packet != NULL) {
        OSStatus send_result = MIDIReceived(backend->source, &packet_list);
        if (send_result != noErr) {
            qemu_log_mask(LOG_UNIMP,
                          "sun-bpp-midi: MIDIReceived failed: %d\n",
                          (int)send_result);
        }
    }

    backend->message_position = 0;
}

/*
 * Determine expected message length from a status byte.
 * Returns 0 for variable-length (SysEx) or unknown system messages.
 */
static int midi_message_length_for_status(uint8_t status_byte)
{
    if (status_byte < 0x80) {
        return 0;  /* Not a status byte */
    }

    if (status_byte < 0xF0) {
        /* Channel message */
        int upper_nibble_index = (status_byte >> 4) - 8;
        return midi_channel_message_length[upper_nibble_index];
    }

    /* System messages */
    switch (status_byte) {
    case 0xF0: return 0;   /* SysEx — variable length, terminated by 0xF7 */
    case 0xF1: return 2;   /* MTC Quarter Frame */
    case 0xF2: return 3;   /* Song Position */
    case 0xF3: return 2;   /* Song Select */
    case 0xF6: return 1;   /* Tune Request */
    case 0xF7: return 1;   /* End of SysEx */
    case 0xF8: return 1;   /* Timing Clock */
    case 0xFA: return 1;   /* Start */
    case 0xFB: return 1;   /* Continue */
    case 0xFC: return 1;   /* Stop */
    case 0xFE: return 1;   /* Active Sensing */
    case 0xFF: return 1;   /* System Reset */
    default:   return 1;   /* Unknown system common — treat as 1 byte */
    }
}

SunBppMidiBackend *sun_bpp_midi_init(void)
{
    SunBppMidiBackend *backend = g_new0(SunBppMidiBackend, 1);

    OSStatus create_result = MIDIClientCreate(
        CFSTR("QEMU Sun BPP"),
        NULL,   /* notifyProc — we don't need setup change notifications */
        NULL,   /* notifyRefCon */
        &backend->client);

    if (create_result != noErr) {
        qemu_log_mask(LOG_UNIMP,
                      "sun-bpp-midi: MIDIClientCreate failed: %d\n",
                      (int)create_result);
        g_free(backend);
        return NULL;
    }

    create_result = MIDISourceCreate(
        backend->client,
        CFSTR("QEMU SPARC MIDI Out"),
        &backend->source);

    if (create_result != noErr) {
        qemu_log_mask(LOG_UNIMP,
                      "sun-bpp-midi: MIDISourceCreate failed: %d\n",
                      (int)create_result);
        MIDIClientDispose(backend->client);
        g_free(backend);
        return NULL;
    }

    backend->message_position = 0;
    backend->message_expected_length = 0;
    backend->running_status = 0;
    backend->in_sysex = false;

    fprintf(stderr, "sun-bpp-midi: CoreMIDI source "
            "\"QEMU SPARC MIDI Out\" created\n");

    return backend;
}

void sun_bpp_midi_send(SunBppMidiBackend *backend, uint8_t midi_byte)
{
    if (!backend) {
        return;
    }

    /* Handle SysEx accumulation */
    if (backend->in_sysex) {
        if (backend->message_position < MIDI_MSG_MAX_SIZE) {
            backend->message_buffer[backend->message_position++] = midi_byte;
        }
        if (midi_byte == 0xF7) {
            /* End of SysEx — flush */
            coremidi_flush_message(backend);
            backend->in_sysex = false;
        }
        return;
    }

    /* Status byte? */
    if (midi_byte & 0x80) {
        /* Flush any pending incomplete message */
        if (backend->message_position > 0) {
            coremidi_flush_message(backend);
        }

        if (midi_byte == 0xF0) {
            /* Start SysEx */
            backend->in_sysex = true;
            backend->message_buffer[0] = midi_byte;
            backend->message_position = 1;
            backend->running_status = 0;
            return;
        }

        /* Real-time messages (0xF8-0xFF) are single-byte, send immediately */
        if (midi_byte >= 0xF8) {
            backend->message_buffer[0] = midi_byte;
            backend->message_position = 1;
            coremidi_flush_message(backend);
            return;
        }

        /* Start new message */
        backend->message_buffer[0] = midi_byte;
        backend->message_position = 1;
        backend->message_expected_length =
            midi_message_length_for_status(midi_byte);

        /* Update running status for channel messages */
        if (midi_byte < 0xF0) {
            backend->running_status = midi_byte;
        } else {
            backend->running_status = 0;
        }

        /* Single-byte messages — flush immediately */
        if (backend->message_expected_length == 1) {
            coremidi_flush_message(backend);
        }
        return;
    }

    /* Data byte (0x00-0x7F) */

    /* If no message in progress, try running status */
    if (backend->message_position == 0 && backend->running_status) {
        backend->message_buffer[0] = backend->running_status;
        backend->message_position = 1;
        backend->message_expected_length =
            midi_message_length_for_status(backend->running_status);
    }

    /* Append data byte */
    if (backend->message_position > 0 &&
        backend->message_position < MIDI_MSG_MAX_SIZE) {
        backend->message_buffer[backend->message_position++] = midi_byte;
    }

    /* Check if message is complete */
    if (backend->message_expected_length > 0 &&
        backend->message_position >= backend->message_expected_length) {
        coremidi_flush_message(backend);
    }
}

void sun_bpp_midi_cleanup(SunBppMidiBackend *backend)
{
    if (!backend) {
        return;
    }

    if (backend->source) {
        MIDIEndpointDispose(backend->source);
    }
    if (backend->client) {
        MIDIClientDispose(backend->client);
    }

    fprintf(stderr, "sun-bpp-midi: CoreMIDI source disposed\n");

    g_free(backend);
}
