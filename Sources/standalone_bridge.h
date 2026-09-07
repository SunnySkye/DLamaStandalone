#ifndef DELAY_LAMA_STANDALONE_BRIDGE_H
#define DELAY_LAMA_STANDALONE_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DelayLamaHandle DelayLamaHandle;

DelayLamaHandle *dl_create(float sample_rate);
void dl_destroy(DelayLamaHandle *handle);
void dl_process(DelayLamaHandle *handle, float *left, float *right, uint32_t frames);

void dl_note_on(DelayLamaHandle *handle, uint8_t note, float velocity);
void dl_note_off(DelayLamaHandle *handle, uint8_t note);
void dl_all_notes_off(DelayLamaHandle *handle);
void dl_xy_begin(DelayLamaHandle *handle, float pitch, float vowel);
void dl_xy_move(DelayLamaHandle *handle, float pitch, float vowel);
void dl_xy_end(DelayLamaHandle *handle);

void dl_set_glide(DelayLamaHandle *handle, float value);
void dl_set_voice(DelayLamaHandle *handle, float value);
void dl_set_delay(DelayLamaHandle *handle, float value);
void dl_set_vibrato(DelayLamaHandle *handle, float value);
void dl_set_volume(DelayLamaHandle *handle, float value);

float dl_get_vowel(DelayLamaHandle *handle);
float dl_get_pitch(DelayLamaHandle *handle);
int32_t dl_is_active(DelayLamaHandle *handle);
int32_t dl_midi_source_count(DelayLamaHandle *handle);

#ifdef __cplusplus
}
#endif

#endif
