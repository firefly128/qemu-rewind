/*
 * Sun BPP MIDI backend — MT-32 emulator (macOS)
 *
 * Uses libmt32emu to emulate a Roland MT-32 sound module.
 * Decoded MIDI bytes from the BPP bit-bang decoder are accumulated
 * into complete MIDI messages, fed to the MT-32 synth engine, and
 * the resulting PCM audio is output via CoreAudio's AudioQueue.
 *
 * Requires:
 *   - libmt32emu (Munt) headers and static library
 *   - MT-32 ROM files (MT32_CONTROL.ROM + MT32_PCM.ROM)
 *   - CoreAudio / AudioToolbox frameworks
 *
 * Copyright (c) 2026 Julian Wolfe
 * License: MIT (same as sun_bpp.c)
 */

#import <AudioToolbox/AudioToolbox.h>
#include <pthread.h>

#define MT32EMU_API_TYPE 1
#include <mt32emu/mt32emu.h>

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "sun_bpp_midi.h"

/*
 * Audio configuration.
 * MT-32 native sample rate is 32000 Hz.  We use the built-in sample rate
 * converter to output at 48000 Hz for better compatibility with audio
 * hardware.
 */
#define MT32_OUTPUT_SAMPLERATE  48000.0
#define MT32_NUM_AUDIO_BUFFERS  3
#define MT32_FRAMES_PER_BUFFER  1024    /* ~21 ms at 48 kHz */

/*
 * MIDI message accumulator — identical logic to the CoreMIDI backend.
 */
#define MIDI_MSG_MAX_SIZE   256

static const int midi_channel_message_length[8] = {
    3, 3, 3, 3, 2, 2, 3, 0  /* 0 = variable/system */
};

struct SunBppMidiBackend {
    /* MT-32 synth */
    mt32emu_context synth_context;

    /* CoreAudio output */
    AudioQueueRef audio_queue;
    AudioQueueBufferRef audio_buffers[MT32_NUM_AUDIO_BUFFERS];
    bool audio_running;

    /* Render lock — protects synth_context between MIDI input and
     * the audio render callback */
    pthread_mutex_t render_lock;

    /* MIDI message accumulator */
    uint8_t  message_buffer[MIDI_MSG_MAX_SIZE];
    int      message_position;
    int      message_expected_length;
    uint8_t  running_status;
    bool     in_sysex;
};

/*
 * Determine expected message length from a status byte.
 */
static int midi_message_length_for_status(uint8_t status_byte)
{
    if (status_byte < 0x80) {
        return 0;
    }
    if (status_byte < 0xF0) {
        int upper_nibble_index = (status_byte >> 4) - 8;
        return midi_channel_message_length[upper_nibble_index];
    }
    switch (status_byte) {
    case 0xF0: return 0;   /* SysEx */
    case 0xF1: return 2;
    case 0xF2: return 3;
    case 0xF3: return 2;
    case 0xF6: return 1;
    case 0xF7: return 1;
    case 0xF8: return 1;
    case 0xFA: return 1;
    case 0xFB: return 1;
    case 0xFC: return 1;
    case 0xFE: return 1;
    case 0xFF: return 1;
    default:   return 1;
    }
}

/*
 * Send a complete MIDI message to the MT-32 synth.
 */
static void mt32_flush_message(SunBppMidiBackend *backend)
{
    if (backend->message_position == 0) {
        return;
    }

    pthread_mutex_lock(&backend->render_lock);

    uint8_t status_byte = backend->message_buffer[0];

    if (status_byte == 0xF0) {
        /* SysEx message */
        mt32emu_play_sysex(backend->synth_context,
                           backend->message_buffer,
                           backend->message_position);
    } else if (status_byte >= 0x80) {
        /* Short message — pack into uint32: status | data1<<8 | data2<<16 */
        mt32emu_bit32u packed_message = status_byte;
        if (backend->message_position > 1) {
            packed_message |= (mt32emu_bit32u)backend->message_buffer[1] << 8;
        }
        if (backend->message_position > 2) {
            packed_message |= (mt32emu_bit32u)backend->message_buffer[2] << 16;
        }
        mt32emu_play_msg(backend->synth_context, packed_message);
    }

    pthread_mutex_unlock(&backend->render_lock);

    backend->message_position = 0;
}

/*
 * CoreAudio AudioQueue output callback.
 * Renders MT-32 audio into the buffer and re-enqueues it.
 */
static void audio_queue_callback(void *user_data,
                                 AudioQueueRef audio_queue,
                                 AudioQueueBufferRef buffer)
{
    SunBppMidiBackend *backend = (SunBppMidiBackend *)user_data;

    int16_t *sample_data = (int16_t *)buffer->mAudioData;
    uint32_t frame_count = MT32_FRAMES_PER_BUFFER;

    pthread_mutex_lock(&backend->render_lock);
    mt32emu_render_bit16s(backend->synth_context, sample_data, frame_count);
    pthread_mutex_unlock(&backend->render_lock);

    buffer->mAudioDataByteSize = frame_count * 2 * sizeof(int16_t); /* stereo */

    AudioQueueEnqueueBuffer(audio_queue, buffer, 0, NULL);
}

/*
 * Find MT-32 ROM files.  Searches in order:
 *   1. ~/.local/share/munt/roms/
 *   2. ~/Library/Application Support/Munt/
 */
static bool find_rom_files(char *control_rom_path, char *pcm_rom_path,
                           size_t path_buffer_size)
{
    const char *home_directory = getenv("HOME");
    if (!home_directory) {
        return false;
    }

    static const char *search_directories[] = {
        "%s/.local/share/munt/roms",
        "%s/Library/Application Support/Munt",
        NULL
    };

    for (const char **directory_pattern = search_directories;
         *directory_pattern; directory_pattern++) {
        char directory_path[1024];
        snprintf(directory_path, sizeof(directory_path),
                 *directory_pattern, home_directory);

        snprintf(control_rom_path, path_buffer_size,
                 "%s/MT32_CONTROL.ROM", directory_path);
        snprintf(pcm_rom_path, path_buffer_size,
                 "%s/MT32_PCM.ROM", directory_path);

        if (access(control_rom_path, R_OK) == 0 &&
            access(pcm_rom_path, R_OK) == 0) {
            return true;
        }
    }

    return false;
}

SunBppMidiBackend *sun_bpp_midi_init(void)
{
    char control_rom_path[1024];
    char pcm_rom_path[1024];

    if (!find_rom_files(control_rom_path, pcm_rom_path,
                        sizeof(control_rom_path))) {
        fprintf(stderr, "sun-bpp-midi-mt32: MT-32 ROMs not found "
                "(searched ~/.local/share/munt/roms/ and "
                "~/Library/Application Support/Munt/)\n");
        return NULL;
    }

    SunBppMidiBackend *backend = g_new0(SunBppMidiBackend, 1);
    pthread_mutex_init(&backend->render_lock, NULL);

    /* Create MT-32 synth context (no report handler) */
    mt32emu_report_handler_i null_handler;
    null_handler.v0 = NULL;
    backend->synth_context = mt32emu_create_context(null_handler, NULL);
    if (!backend->synth_context) {
        fprintf(stderr, "sun-bpp-midi-mt32: failed to create synth context\n");
        goto fail_context;
    }

    /* Load ROMs */
    mt32emu_return_code rom_result;

    rom_result = mt32emu_add_rom_file(backend->synth_context,
                                      control_rom_path);
    if (rom_result < MT32EMU_RC_OK) {
        fprintf(stderr, "sun-bpp-midi-mt32: failed to load control ROM "
                "'%s' (error %d)\n", control_rom_path, rom_result);
        goto fail_rom;
    }

    rom_result = mt32emu_add_rom_file(backend->synth_context, pcm_rom_path);
    if (rom_result < MT32EMU_RC_OK) {
        fprintf(stderr, "sun-bpp-midi-mt32: failed to load PCM ROM "
                "'%s' (error %d)\n", pcm_rom_path, rom_result);
        goto fail_rom;
    }

    /* Configure output sample rate and analog mode */
    mt32emu_set_stereo_output_samplerate(backend->synth_context,
                                         MT32_OUTPUT_SAMPLERATE);

    /* Open the synth */
    rom_result = mt32emu_open_synth(backend->synth_context);
    if (rom_result != MT32EMU_RC_OK) {
        fprintf(stderr, "sun-bpp-midi-mt32: failed to open synth "
                "(error %d)\n", rom_result);
        goto fail_rom;
    }

    /* Set up CoreAudio AudioQueue for PCM output */
    AudioStreamBasicDescription audio_format = {
        .mSampleRate       = MT32_OUTPUT_SAMPLERATE,
        .mFormatID         = kAudioFormatLinearPCM,
        .mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger |
                             kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket   = 4,     /* 2 channels * 2 bytes */
        .mFramesPerPacket  = 1,
        .mBytesPerFrame    = 4,
        .mChannelsPerFrame = 2,     /* stereo */
        .mBitsPerChannel   = 16,
    };

    OSStatus audio_status = AudioQueueNewOutput(
        &audio_format,
        audio_queue_callback,
        backend,
        NULL,       /* use internal run loop */
        NULL,
        0,
        &backend->audio_queue);

    if (audio_status != noErr) {
        fprintf(stderr, "sun-bpp-midi-mt32: AudioQueueNewOutput failed: %d\n",
                (int)audio_status);
        goto fail_audio;
    }

    /* Allocate and prime audio buffers */
    uint32_t buffer_byte_size = MT32_FRAMES_PER_BUFFER * 2 * sizeof(int16_t);
    for (int buffer_index = 0; buffer_index < MT32_NUM_AUDIO_BUFFERS;
         buffer_index++) {
        audio_status = AudioQueueAllocateBuffer(
            backend->audio_queue,
            buffer_byte_size,
            &backend->audio_buffers[buffer_index]);

        if (audio_status != noErr) {
            fprintf(stderr,
                    "sun-bpp-midi-mt32: AudioQueueAllocateBuffer failed\n");
            goto fail_buffers;
        }

        /* Pre-fill with silence so the queue starts immediately */
        memset(backend->audio_buffers[buffer_index]->mAudioData,
               0, buffer_byte_size);
        backend->audio_buffers[buffer_index]->mAudioDataByteSize =
            buffer_byte_size;

        AudioQueueEnqueueBuffer(backend->audio_queue,
                                backend->audio_buffers[buffer_index],
                                0, NULL);
    }

    /* Start playback */
    audio_status = AudioQueueStart(backend->audio_queue, NULL);
    if (audio_status != noErr) {
        fprintf(stderr, "sun-bpp-midi-mt32: AudioQueueStart failed: %d\n",
                (int)audio_status);
        goto fail_buffers;
    }

    backend->audio_running = true;
    backend->message_position = 0;
    backend->message_expected_length = 0;
    backend->running_status = 0;
    backend->in_sysex = false;

    /* Print ROM info */
    mt32emu_rom_info rom_info;
    mt32emu_get_rom_info(backend->synth_context, &rom_info);
    fprintf(stderr, "sun-bpp-midi-mt32: MT-32 synth ready\n"
            "  Control ROM: %s\n"
            "  PCM ROM:     %s\n"
            "  Output:      %.0f Hz stereo, %d-frame buffers\n",
            rom_info.control_rom_description ?
                rom_info.control_rom_description : "(unknown)",
            rom_info.pcm_rom_description ?
                rom_info.pcm_rom_description : "(unknown)",
            MT32_OUTPUT_SAMPLERATE,
            MT32_FRAMES_PER_BUFFER);

    return backend;

fail_buffers:
    AudioQueueDispose(backend->audio_queue, true);
fail_audio:
    mt32emu_close_synth(backend->synth_context);
fail_rom:
    mt32emu_free_context(backend->synth_context);
fail_context:
    pthread_mutex_destroy(&backend->render_lock);
    g_free(backend);
    return NULL;
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
            mt32_flush_message(backend);
            backend->in_sysex = false;
        }
        return;
    }

    /* Status byte? */
    if (midi_byte & 0x80) {
        if (backend->message_position > 0) {
            mt32_flush_message(backend);
        }

        if (midi_byte == 0xF0) {
            backend->in_sysex = true;
            backend->message_buffer[0] = midi_byte;
            backend->message_position = 1;
            backend->running_status = 0;
            return;
        }

        if (midi_byte >= 0xF8) {
            backend->message_buffer[0] = midi_byte;
            backend->message_position = 1;
            mt32_flush_message(backend);
            return;
        }

        backend->message_buffer[0] = midi_byte;
        backend->message_position = 1;
        backend->message_expected_length =
            midi_message_length_for_status(midi_byte);

        if (midi_byte < 0xF0) {
            backend->running_status = midi_byte;
        } else {
            backend->running_status = 0;
        }

        if (backend->message_expected_length == 1) {
            mt32_flush_message(backend);
        }
        return;
    }

    /* Data byte */
    if (backend->message_position == 0 && backend->running_status) {
        backend->message_buffer[0] = backend->running_status;
        backend->message_position = 1;
        backend->message_expected_length =
            midi_message_length_for_status(backend->running_status);
    }

    if (backend->message_position > 0 &&
        backend->message_position < MIDI_MSG_MAX_SIZE) {
        backend->message_buffer[backend->message_position++] = midi_byte;
    }

    if (backend->message_expected_length > 0 &&
        backend->message_position >= backend->message_expected_length) {
        mt32_flush_message(backend);
    }
}

void sun_bpp_midi_cleanup(SunBppMidiBackend *backend)
{
    if (!backend) {
        return;
    }

    if (backend->audio_running) {
        AudioQueueStop(backend->audio_queue, true);
        AudioQueueDispose(backend->audio_queue, true);
    }

    mt32emu_close_synth(backend->synth_context);
    mt32emu_free_context(backend->synth_context);
    pthread_mutex_destroy(&backend->render_lock);

    fprintf(stderr, "sun-bpp-midi-mt32: MT-32 synth shut down\n");

    g_free(backend);
}
