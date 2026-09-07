#include "synth.h"
#include <math.h>
#include <stdio.h>

int main(void) {
    float left[512], right[512];
    double energy = 0.0;
    float peak = 0.0f;
    MonkSynthEngine *synth = monk_synth_new(48000.0f);
    if (!synth)
        return 2;

    monk_synth_set_volume(synth, 0.25f);
    monk_synth_note_on(synth, 48, 1.0f);
    for (int block = 0; block < 40; ++block) {
        monk_synth_process(synth, left, right, 512);
        for (int i = 0; i < 512; ++i) {
            if (!isfinite(left[i]) || !isfinite(right[i]))
                return 3;
            float magnitude = fmaxf(fabsf(left[i]), fabsf(right[i]));
            if (magnitude > peak)
                peak = magnitude;
            energy += (double)left[i] * left[i] + (double)right[i] * right[i];
        }
    }
    monk_synth_note_off(synth, 48);
    monk_synth_free(synth);

    printf("peak=%.6f rms=%.6f\n", peak, sqrt(energy / (40.0 * 512.0 * 2.0)));
    return peak > 0.0001f && energy > 0.0 ? 0 : 4;
}
