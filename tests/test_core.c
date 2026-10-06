#include "keylight_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    kl_state s = kl_state_default(), next;
    assert(kl_state_valid(&s));
    kl_patch p = {.fields = KL_BRIGHTNESS, .value.brightness = 101};
    memset(&next, 0x55, sizeof(next));
    kl_state untouched = next;
    assert(kl_state_patch(&s, &p, &next) == KL_INVALID);
    assert(!memcmp(&next, &untouched, sizeof(next)));
    for (unsigned i = 0; i <= 100; i++) {
        p.value.brightness = (uint8_t)i;
        assert(kl_state_patch(&s, &p, &next) == KL_OK && next.brightness == i);
    }
    s.recording_lock = true;
    assert(kl_state_patch(&s, &p, &next) == KL_LOCKED);
    p.fields |= KL_LOCK; p.value.recording_lock = false;
    assert(kl_state_patch(&s, &p, &next) == KL_LOCKED);
    p.fields = KL_POWER; p.value.power = false;
    assert(kl_state_patch(&s, &p, &next) == KL_OK);
    p.fields = KL_LOCK;
    assert(kl_state_patch(&s, &p, &next) == KL_OK && !next.recording_lock);
    p.fields = 256;
    assert(kl_state_patch(&s, &p, &next) == KL_INVALID);
    s = kl_state_default(); s.power = true; s.mode = KL_COLOR; s.brightness = 100;
    s.rgb = (kl_rgb){255, 0, 128};
    kl_frame black = {.temperature_k = 4500}; kl_transition t;
    for (unsigned duration = 0; duration <= 10000; duration += 100) {
        s.transition_ms = (uint16_t)duration;
        kl_transition_begin(&t, &black, &s, 100);
        float previous = 0;
        for (unsigned now = 0; now <= duration; now++) {
            kl_frame f = kl_transition_sample(&t, 100 + now);
            assert(f.r >= previous && f.r <= 255 && f.g == 0 && f.b <= 128);
            previous = f.r;
        }
        kl_frame end = kl_transition_sample(&t, 100 + duration);
        assert(end.r == 255 && end.b == 128 && kl_transition_done(&t, 100 + duration));
    }
    s.effect = KL_EFFECT_AURORA;
    for (unsigned now = 0; now < 36000; now++) {
        kl_frame f = kl_state_frame(&s, now);
        assert(isfinite(f.r) && f.r >= 0 && f.r <= 255 && f.white == 0);
    }
    s.power = false; kl_transition_begin(&t, &black, &s, 0);
    assert(kl_transition_done(&t, 0));
    kl_frame off = kl_transition_sample(&t, 6000);
    assert(off.r == 0 && off.g == 0 && off.b == 0);
    assert(kl_byte(NAN) == 0 && kl_byte(256) == 255 && kl_byte(-1) == 0);
    puts("core: atomic validation, lock boundaries, time-based transitions and effects passed");
}
