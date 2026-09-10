#include "standalone_bridge.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    DelayLamaHandle *handle = dl_create(48000.0f);
    float left[512];
    float right[512];
    double energy = 0.0;

    if (!handle) {
        fprintf(stderr, "bridge: dl_create failed\n");
        return 1;
    }

    /* Avoid relying on physical MIDI devices or the long delay tap. */
    dl_set_delay(handle, 0.0f);
    dl_set_volume(handle, 1.0f);
    dl_set_glide(handle, 0.0f);
    dl_note_on(handle, 60, 1.0f);

    if (!dl_is_active(handle)) {
        fprintf(stderr, "bridge: note-on did not activate the synth\n");
        dl_destroy(handle);
        return 1;
    }

    for (int block = 0; block < 4; ++block) {
        memset(left, 0, sizeof(left));
        memset(right, 0, sizeof(right));
        dl_process(handle, left, right, 512);
        for (uint32_t i = 0; i < 512; ++i) {
            if (!isfinite(left[i]) || !isfinite(right[i])) {
                fprintf(stderr, "bridge: non-finite output at block %d sample %u\n", block, i);
                dl_destroy(handle);
                return 1;
            }
            energy += (double)left[i] * left[i] + (double)right[i] * right[i];
        }
    }

    if (energy <= 0.0) {
        fprintf(stderr, "bridge: note-on produced no measurable audio\n");
        dl_destroy(handle);
        return 1;
    }

    /* Exercise the UI-facing XY path without requiring a window or MIDI
     * device, then verify that all-notes-off reaches the synth. */
    dl_xy_begin(handle, 0.5f, 0.5f);
    if (!isfinite(dl_get_pitch(handle)) || !isfinite(dl_get_vowel(handle))) {
        fprintf(stderr, "bridge: XY state readback was non-finite\n");
        dl_destroy(handle);
        return 1;
    }
    dl_xy_end(handle);
    dl_all_notes_off(handle);
    if (dl_is_active(handle)) {
        fprintf(stderr, "bridge: all-notes-off left the default-envelope voice active\n");
        dl_destroy(handle);
        return 1;
    }

    printf("windows_bridge_smoke: PASS energy=%.6f midi_sources=%d\n", energy,
           (int)dl_midi_source_count(handle));
    dl_destroy(handle);
    return 0;
}
