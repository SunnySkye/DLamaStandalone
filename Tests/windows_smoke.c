#include "delay.h"
#include "synth.h"
#include "voice.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int close_enough(float actual, float expected, float tolerance) {
    return fabsf(actual - expected) <= tolerance;
}

static int test_pitch_math(void) {
    if (!close_enough(monk_midi_note_to_freq(69), 440.0f, 0.01f)) {
        fprintf(stderr, "pitch math: MIDI 69 is not 440 Hz\n");
        return 0;
    }
    if (!close_enough(monk_midi_note_to_freq(60), 261.625565f, 0.01f)) {
        fprintf(stderr, "pitch math: MIDI 60 frequency changed\n");
        return 0;
    }
    if (!close_enough(monk_hz_to_note(440.0f), 69.0f, 0.0001f)) {
        fprintf(stderr, "pitch math: 440 Hz does not round-trip to MIDI 69\n");
        return 0;
    }
    return 1;
}

static int test_delay_dry_path(void) {
    MonkDelay *delay = (MonkDelay *)calloc(1, sizeof(*delay));
    float input[64];
    float left[64];
    float right[64];

    if (!delay) {
        fprintf(stderr, "delay: allocation failed\n");
        return 0;
    }

    for (uint32_t i = 0; i < 64; ++i)
        input[i] = (float)i / 63.0f - 0.5f;

    monk_delay_init(delay, 48000.0f);
    monk_delay_set_mix(delay, 0.0f);
    monk_delay_process(delay, input, left, right, 64);

    int ok = 1;
    for (uint32_t i = 0; i < 64; ++i) {
        if (left[i] != input[i] || right[i] != input[i]) {
            fprintf(stderr, "delay: mix=0 did not preserve the dry signal at sample %u\n", i);
            ok = 0;
            break;
        }
    }

    free(delay);
    return ok;
}

static int test_synth_audio_and_state(void) {
    MonkSynthEngine *synth = monk_synth_new(48000.0f);
    float left[512];
    float right[512];
    double energy = 0.0;
    float peak = 0.0f;

    if (!synth) {
        fprintf(stderr, "synth: construction failed\n");
        return 0;
    }

    /* Keep this smoke test deterministic and make the direct voice signal
     * visible without depending on the long delay tap. */
    monk_synth_set_glide(synth, 0.0f);
    monk_synth_set_vibrato(synth, 0.0f);
    monk_synth_set_delay_mix(synth, 0.0f);
    monk_synth_set_volume(synth, 1.0f);
    monk_synth_set_level(synth, 1.0f);

    monk_synth_note_on(synth, 60, 1.0f);
    if (!monk_synth_is_active(synth)) {
        fprintf(stderr, "synth: note-on did not activate the engine\n");
        monk_synth_free(synth);
        return 0;
    }

    for (int block = 0; block < 8; ++block) {
        memset(left, 0, sizeof(left));
        memset(right, 0, sizeof(right));
        monk_synth_process(synth, left, right, 512);

        for (uint32_t i = 0; i < 512; ++i) {
            if (!isfinite(left[i]) || !isfinite(right[i])) {
                fprintf(stderr, "synth: non-finite output at block %d sample %u\n", block, i);
                monk_synth_free(synth);
                return 0;
            }
            float magnitude = fmaxf(fabsf(left[i]), fabsf(right[i]));
            if (magnitude > peak)
                peak = magnitude;
            energy += (double)left[i] * left[i] + (double)right[i] * right[i];
        }
    }

    if (peak <= 0.0001f || energy <= 0.0) {
        fprintf(stderr, "synth: note-on produced no measurable audio\n");
        monk_synth_free(synth);
        return 0;
    }

    /* Public parameter/state behavior: the vowel input is clamped and the
     * last-note priority stack returns to the older held note. */
    monk_synth_set_vowel(synth, -1.0f);
    if (!close_enough(monk_synth_get_vowel(synth), 0.0f, 0.0001f)) {
        fprintf(stderr, "synth: vowel lower bound was not clamped\n");
        monk_synth_free(synth);
        return 0;
    }
    monk_synth_set_vowel(synth, 2.0f);
    if (!close_enough(monk_synth_get_vowel(synth), 1.0f, 0.0001f)) {
        fprintf(stderr, "synth: vowel upper bound was not clamped\n");
        monk_synth_free(synth);
        return 0;
    }

    monk_synth_reset(synth);
    monk_synth_set_glide(synth, 0.0f);
    monk_synth_note_on(synth, 48, 1.0f);
    monk_synth_process(synth, left, right, 1);
    if (!close_enough(monk_synth_get_pitch_normalized(synth), 0.0f, 0.0001f)) {
        fprintf(stderr, "synth: C3 did not map to normalized pitch 0\n");
        monk_synth_free(synth);
        return 0;
    }

    monk_synth_note_on(synth, 60, 1.0f);
    monk_synth_process(synth, left, right, 1);
    if (!close_enough(monk_synth_get_pitch_normalized(synth), 1.0f, 0.0001f)) {
        fprintf(stderr, "synth: C4 did not map to normalized pitch 1\n");
        monk_synth_free(synth);
        return 0;
    }

    monk_synth_note_off(synth, 60);
    monk_synth_process(synth, left, right, 1);
    if (!close_enough(monk_synth_get_pitch_normalized(synth), 0.0f, 0.0001f)) {
        fprintf(stderr, "synth: releasing the top note did not restore the held note\n");
        monk_synth_free(synth);
        return 0;
    }

    monk_synth_note_off(synth, 48);
    if (monk_synth_is_active(synth)) {
        fprintf(stderr, "synth: note-off left the default-envelope voice active\n");
        monk_synth_free(synth);
        return 0;
    }

    monk_synth_free(synth);
    printf("audio peak=%.6f energy=%.6f\n", peak, energy);
    return 1;
}

static int run_test(const char *name, int (*test)(void)) {
    if (!test()) {
        fprintf(stderr, "[FAIL] %s\n", name);
        return 0;
    }
    printf("[PASS] %s\n", name);
    return 1;
}

int main(void) {
    int passed = 0;
    passed += run_test("pitch math", test_pitch_math);
    passed += run_test("delay dry path", test_delay_dry_path);
    passed += run_test("synth audio and state", test_synth_audio_and_state);

    printf("windows_smoke: %d/3 tests passed\n", passed);
    return passed == 3 ? 0 : 1;
}
