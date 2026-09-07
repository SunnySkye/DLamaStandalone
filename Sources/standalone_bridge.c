#include "standalone_bridge.h"

#include "synth.h"
#include <CoreMIDI/CoreMIDI.h>
#include <math.h>
#include <os/lock.h>
#include <stdlib.h>
#include <string.h>

#define XY_SENTINEL_NOTE 127

struct DelayLamaHandle {
    MonkSynthEngine *synth;
    os_unfair_lock lock;
    MIDIClientRef midi_client;
    MIDIPortRef midi_port;
    int32_t midi_sources;
    uint8_t xy_active;
    uint8_t held[128];
};

static float clamp01(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

static float xy_pitch_hz(float normalized) {
    float midi_note = 48.0f + clamp01(normalized) * 24.0f;
    return 440.0f * powf(2.0f, (midi_note - 69.0f) / 12.0f);
}

static void locked_note_on(DelayLamaHandle *handle, uint8_t note, float velocity) {
    if (note == XY_SENTINEL_NOTE)
        return;
    handle->held[note] = 1;
    monk_synth_note_on(handle->synth, note, velocity);
}

static void locked_note_off(DelayLamaHandle *handle, uint8_t note) {
    if (note == XY_SENTINEL_NOTE)
        return;
    handle->held[note] = 0;
    monk_synth_note_off(handle->synth, note);
}

static void handle_midi_bytes(DelayLamaHandle *handle, const uint8_t *data, uint16_t length) {
    uint16_t i = 0;
    uint8_t running = 0;

    while (i < length) {
        uint8_t byte = data[i];
        if (byte & 0x80) {
            if (byte >= 0xF0) {
                running = 0;
                i++;
                continue;
            }
            running = byte;
            i++;
        }
        if (!running || i >= length)
            continue;

        uint8_t type = running & 0xF0;
        int needed = (type == 0xC0 || type == 0xD0) ? 1 : 2;
        if (i + needed > length)
            break;
        uint8_t d1 = data[i++];
        uint8_t d2 = needed == 2 ? data[i++] : 0;

        os_unfair_lock_lock(&handle->lock);
        switch (type) {
        case 0x80:
            locked_note_off(handle, d1);
            break;
        case 0x90:
            if (d2 == 0)
                locked_note_off(handle, d1);
            else
                locked_note_on(handle, d1, (float)d2 / 127.0f);
            break;
        case 0xB0:
            if (d1 == 123 || d1 == 120) {
                for (int note = 0; note < 127; ++note)
                    if (handle->held[note])
                        locked_note_off(handle, (uint8_t)note);
            } else {
                monk_synth_midi_cc(handle->synth, d1, (float)d2 / 127.0f);
            }
            break;
        case 0xE0: {
            int bend = ((int)d2 << 7) | d1;
            monk_synth_set_vowel(handle->synth, (float)bend / 16383.0f);
            break;
        }
        default:
            break;
        }
        os_unfair_lock_unlock(&handle->lock);
    }
}

static void midi_read(const MIDIPacketList *packets, void *refcon, void *source_refcon) {
    (void)source_refcon;
    DelayLamaHandle *handle = (DelayLamaHandle *)refcon;
    if (!handle || !packets)
        return;

    const MIDIPacket *packet = &packets->packet[0];
    for (UInt32 index = 0; index < packets->numPackets; ++index) {
        handle_midi_bytes(handle, packet->data, packet->length);
        packet = MIDIPacketNext(packet);
    }
}

static void connect_midi_sources(DelayLamaHandle *handle) {
    ItemCount count = MIDIGetNumberOfSources();
    int32_t connected = 0;
    for (ItemCount index = 0; index < count; ++index) {
        MIDIEndpointRef source = MIDIGetSource(index);
        if (source && MIDIPortConnectSource(handle->midi_port, source, NULL) == noErr)
            connected++;
    }
    handle->midi_sources = connected;
}

DelayLamaHandle *dl_create(float sample_rate) {
    DelayLamaHandle *handle = calloc(1, sizeof(*handle));
    if (!handle)
        return NULL;
    handle->lock = OS_UNFAIR_LOCK_INIT;
    handle->synth = monk_synth_new(sample_rate);
    if (!handle->synth) {
        free(handle);
        return NULL;
    }

    monk_synth_set_volume(handle->synth, 0.28f);
    monk_synth_set_level(handle->synth, 0.75f);
    monk_synth_set_vibrato(handle->synth, 0.15f);
    monk_synth_set_delay_mix(handle->synth, 0.42f);

    if (MIDIClientCreate(CFSTR("Delay Lama Standalone"), NULL, NULL, &handle->midi_client) == noErr &&
        MIDIInputPortCreate(handle->midi_client, CFSTR("Delay Lama MIDI Input"), midi_read, handle,
                            &handle->midi_port) == noErr) {
        connect_midi_sources(handle);
    }
    return handle;
}

void dl_destroy(DelayLamaHandle *handle) {
    if (!handle)
        return;
    if (handle->midi_port)
        MIDIPortDispose(handle->midi_port);
    if (handle->midi_client)
        MIDIClientDispose(handle->midi_client);
    monk_synth_free(handle->synth);
    free(handle);
}

void dl_process(DelayLamaHandle *handle, float *left, float *right, uint32_t frames) {
    if (!handle || !left || !right)
        return;
    os_unfair_lock_lock(&handle->lock);
    monk_synth_process(handle->synth, left, right, frames);
    os_unfair_lock_unlock(&handle->lock);
}

void dl_note_on(DelayLamaHandle *handle, uint8_t note, float velocity) {
    if (!handle)
        return;
    os_unfair_lock_lock(&handle->lock);
    locked_note_on(handle, note, velocity);
    os_unfair_lock_unlock(&handle->lock);
}

void dl_note_off(DelayLamaHandle *handle, uint8_t note) {
    if (!handle)
        return;
    os_unfair_lock_lock(&handle->lock);
    locked_note_off(handle, note);
    os_unfair_lock_unlock(&handle->lock);
}

void dl_all_notes_off(DelayLamaHandle *handle) {
    if (!handle)
        return;
    os_unfair_lock_lock(&handle->lock);
    for (int note = 0; note < 127; ++note)
        if (handle->held[note])
            locked_note_off(handle, (uint8_t)note);
    if (handle->xy_active) {
        monk_synth_note_off(handle->synth, XY_SENTINEL_NOTE);
        handle->xy_active = 0;
    }
    os_unfair_lock_unlock(&handle->lock);
}

void dl_xy_begin(DelayLamaHandle *handle, float pitch, float vowel) {
    if (!handle)
        return;
    os_unfair_lock_lock(&handle->lock);
    if (!handle->xy_active) {
        monk_synth_note_on(handle->synth, XY_SENTINEL_NOTE, 1.0f);
        handle->xy_active = 1;
    }
    monk_synth_set_vowel(handle->synth, clamp01(vowel));
    monk_synth_set_pitch_hz(handle->synth, xy_pitch_hz(pitch));
    os_unfair_lock_unlock(&handle->lock);
}

void dl_xy_move(DelayLamaHandle *handle, float pitch, float vowel) {
    if (!handle)
        return;
    os_unfair_lock_lock(&handle->lock);
    monk_synth_set_vowel(handle->synth, clamp01(vowel));
    monk_synth_set_pitch_hz(handle->synth, xy_pitch_hz(pitch));
    os_unfair_lock_unlock(&handle->lock);
}

void dl_xy_end(DelayLamaHandle *handle) {
    if (!handle)
        return;
    os_unfair_lock_lock(&handle->lock);
    if (handle->xy_active) {
        monk_synth_note_off(handle->synth, XY_SENTINEL_NOTE);
        handle->xy_active = 0;
    }
    os_unfair_lock_unlock(&handle->lock);
}

#define SETTER(name, call)                                                                         \
    void name(DelayLamaHandle *handle, float value) {                                               \
        if (!handle)                                                                                \
            return;                                                                                 \
        os_unfair_lock_lock(&handle->lock);                                                         \
        call(handle->synth, clamp01(value));                                                        \
        os_unfair_lock_unlock(&handle->lock);                                                       \
    }

SETTER(dl_set_glide, monk_synth_set_glide)
SETTER(dl_set_voice, monk_synth_set_voice)
SETTER(dl_set_delay, monk_synth_set_delay_mix)
SETTER(dl_set_vibrato, monk_synth_set_vibrato)
SETTER(dl_set_volume, monk_synth_set_volume)

float dl_get_vowel(DelayLamaHandle *handle) {
    if (!handle)
        return 0.5f;
    os_unfair_lock_lock(&handle->lock);
    float result = monk_synth_get_vowel(handle->synth);
    os_unfair_lock_unlock(&handle->lock);
    return result;
}

float dl_get_pitch(DelayLamaHandle *handle) {
    if (!handle)
        return 0.5f;
    os_unfair_lock_lock(&handle->lock);
    float result = monk_synth_get_pitch_normalized(handle->synth);
    os_unfair_lock_unlock(&handle->lock);
    return result;
}

int32_t dl_is_active(DelayLamaHandle *handle) {
    if (!handle)
        return 0;
    os_unfair_lock_lock(&handle->lock);
    int32_t result = monk_synth_is_active(handle->synth);
    os_unfair_lock_unlock(&handle->lock);
    return result;
}

int32_t dl_midi_source_count(DelayLamaHandle *handle) {
    return handle ? handle->midi_sources : 0;
}
