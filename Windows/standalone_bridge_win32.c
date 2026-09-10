#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include "../DSP/synth.h"
#include "../Sources/standalone_bridge.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define XY_SENTINEL_NOTE 127
#define MAX_MIDI_INPUTS 32

struct DelayLamaHandle {
    MonkSynthEngine *synth;
    CRITICAL_SECTION lock;
    HMIDIIN midi_inputs[MAX_MIDI_INPUTS];
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

static void locked_all_notes_off(DelayLamaHandle *handle) {
    for (int note = 0; note < 127; ++note) {
        if (handle->held[note])
            locked_note_off(handle, (uint8_t)note);
    }
    if (handle->xy_active) {
        monk_synth_note_off(handle->synth, XY_SENTINEL_NOTE);
        handle->xy_active = 0;
    }
}

/* WinMM delivers short MIDI messages packed as status, data1, data2 in the
 * low 24 bits of dwParam1. The original bridge handles the same channel voice
 * messages, so SysEx and system realtime messages are intentionally ignored. */
static void handle_midi_message(DelayLamaHandle *handle, DWORD packed_message) {
    uint8_t status = (uint8_t)(packed_message & 0xffu);
    uint8_t data1 = (uint8_t)((packed_message >> 8) & 0xffu);
    uint8_t data2 = (uint8_t)((packed_message >> 16) & 0xffu);

    if ((status & 0x80u) == 0)
        return;

    EnterCriticalSection(&handle->lock);
    switch (status & 0xf0u) {
    case 0x80:
        locked_note_off(handle, data1 & 0x7fu);
        break;
    case 0x90:
        if (data2 == 0)
            locked_note_off(handle, data1 & 0x7fu);
        else
            locked_note_on(handle, data1 & 0x7fu, (float)data2 / 127.0f);
        break;
    case 0xb0:
        if (data1 == 123 || data1 == 120) {
            locked_all_notes_off(handle);
        } else {
            monk_synth_midi_cc(handle->synth, data1 & 0x7fu, (float)data2 / 127.0f);
        }
        break;
    case 0xe0: {
        int bend = ((int)(data2 & 0x7fu) << 7) | (data1 & 0x7fu);
        monk_synth_set_vowel(handle->synth, (float)bend / 16383.0f);
        break;
    }
    default:
        break;
    }
    LeaveCriticalSection(&handle->lock);
}

static void CALLBACK midi_read(HMIDIIN midi_input, UINT message, DWORD_PTR instance,
                               DWORD_PTR param1, DWORD_PTR param2) {
    (void)midi_input;
    (void)param2;
    DelayLamaHandle *handle = (DelayLamaHandle *)instance;
    if (!handle || message != MIM_DATA)
        return;
    handle_midi_message(handle, (DWORD)param1);
}

static void connect_midi_inputs(DelayLamaHandle *handle) {
    UINT device_count = midiInGetNumDevs();
    for (UINT device = 0; device < device_count && handle->midi_sources < MAX_MIDI_INPUTS;
         ++device) {
        HMIDIIN input = NULL;
        MMRESULT result = midiInOpen(&input, device, (DWORD_PTR)midi_read,
                                     (DWORD_PTR)handle, CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR || !input)
            continue;
        result = midiInStart(input);
        if (result != MMSYSERR_NOERROR) {
            midiInClose(input);
            continue;
        }
        handle->midi_inputs[handle->midi_sources++] = input;
    }
}

static void disconnect_midi_inputs(DelayLamaHandle *handle) {
    for (int32_t index = 0; index < handle->midi_sources; ++index) {
        HMIDIIN input = handle->midi_inputs[index];
        if (!input)
            continue;
        midiInStop(input);
        midiInReset(input);
        midiInClose(input);
        handle->midi_inputs[index] = NULL;
    }
    handle->midi_sources = 0;
}

DelayLamaHandle *dl_create(float sample_rate) {
    DelayLamaHandle *handle = (DelayLamaHandle *)calloc(1, sizeof(*handle));
    if (!handle)
        return NULL;

    InitializeCriticalSection(&handle->lock);
    handle->synth = monk_synth_new(sample_rate);
    if (!handle->synth) {
        DeleteCriticalSection(&handle->lock);
        free(handle);
        return NULL;
    }

    monk_synth_set_volume(handle->synth, 0.28f);
    monk_synth_set_level(handle->synth, 0.75f);
    monk_synth_set_vibrato(handle->synth, 0.15f);
    monk_synth_set_delay_mix(handle->synth, 0.42f);

    connect_midi_inputs(handle);
    return handle;
}

void dl_destroy(DelayLamaHandle *handle) {
    if (!handle)
        return;
    disconnect_midi_inputs(handle);
    EnterCriticalSection(&handle->lock);
    locked_all_notes_off(handle);
    LeaveCriticalSection(&handle->lock);
    monk_synth_free(handle->synth);
    DeleteCriticalSection(&handle->lock);
    free(handle);
}

void dl_process(DelayLamaHandle *handle, float *left, float *right, uint32_t frames) {
    if (!handle || !left || !right)
        return;
    EnterCriticalSection(&handle->lock);
    monk_synth_process(handle->synth, left, right, frames);
    LeaveCriticalSection(&handle->lock);
}

void dl_note_on(DelayLamaHandle *handle, uint8_t note, float velocity) {
    if (!handle)
        return;
    EnterCriticalSection(&handle->lock);
    locked_note_on(handle, note, velocity);
    LeaveCriticalSection(&handle->lock);
}

void dl_note_off(DelayLamaHandle *handle, uint8_t note) {
    if (!handle)
        return;
    EnterCriticalSection(&handle->lock);
    locked_note_off(handle, note);
    LeaveCriticalSection(&handle->lock);
}

void dl_all_notes_off(DelayLamaHandle *handle) {
    if (!handle)
        return;
    EnterCriticalSection(&handle->lock);
    locked_all_notes_off(handle);
    LeaveCriticalSection(&handle->lock);
}

void dl_xy_begin(DelayLamaHandle *handle, float pitch, float vowel) {
    if (!handle)
        return;
    EnterCriticalSection(&handle->lock);
    if (!handle->xy_active) {
        monk_synth_note_on(handle->synth, XY_SENTINEL_NOTE, 1.0f);
        handle->xy_active = 1;
    }
    monk_synth_set_vowel(handle->synth, clamp01(vowel));
    monk_synth_set_pitch_hz(handle->synth, xy_pitch_hz(pitch));
    LeaveCriticalSection(&handle->lock);
}

void dl_xy_move(DelayLamaHandle *handle, float pitch, float vowel) {
    if (!handle)
        return;
    EnterCriticalSection(&handle->lock);
    monk_synth_set_vowel(handle->synth, clamp01(vowel));
    monk_synth_set_pitch_hz(handle->synth, xy_pitch_hz(pitch));
    LeaveCriticalSection(&handle->lock);
}

void dl_xy_end(DelayLamaHandle *handle) {
    if (!handle)
        return;
    EnterCriticalSection(&handle->lock);
    if (handle->xy_active) {
        monk_synth_note_off(handle->synth, XY_SENTINEL_NOTE);
        handle->xy_active = 0;
        monk_synth_restore_note_stack(handle->synth);
    }
    LeaveCriticalSection(&handle->lock);
}

#define SETTER(name, call)                                                        \
    void name(DelayLamaHandle *handle, float value) {                              \
        if (!handle)                                                               \
            return;                                                               \
        EnterCriticalSection(&handle->lock);                                       \
        call(handle->synth, clamp01(value));                                       \
        LeaveCriticalSection(&handle->lock);                                       \
    }

SETTER(dl_set_glide, monk_synth_set_glide)
SETTER(dl_set_voice, monk_synth_set_voice)
SETTER(dl_set_delay, monk_synth_set_delay_mix)
SETTER(dl_set_vibrato, monk_synth_set_vibrato)
SETTER(dl_set_volume, monk_synth_set_volume)

float dl_get_vowel(DelayLamaHandle *handle) {
    if (!handle)
        return 0.5f;
    EnterCriticalSection(&handle->lock);
    float result = monk_synth_get_vowel(handle->synth);
    LeaveCriticalSection(&handle->lock);
    return result;
}

float dl_get_pitch(DelayLamaHandle *handle) {
    if (!handle)
        return 0.5f;
    EnterCriticalSection(&handle->lock);
    float result = monk_synth_get_pitch_normalized(handle->synth);
    LeaveCriticalSection(&handle->lock);
    return result;
}

int32_t dl_is_active(DelayLamaHandle *handle) {
    if (!handle)
        return 0;
    EnterCriticalSection(&handle->lock);
    int32_t result = monk_synth_is_active(handle->synth);
    LeaveCriticalSection(&handle->lock);
    return result;
}

int32_t dl_midi_source_count(DelayLamaHandle *handle) {
    return handle ? handle->midi_sources : 0;
}
