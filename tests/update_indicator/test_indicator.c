#include "update_indicator.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void lifecycle(void) {
    kl_update_indicator s = {0}, copy;
    kl_update_indicator_color color = {.r=1, .g=2, .b=3, .master=255};
    CHECK(!kl_update_indicator_sample(&s, 0, &color));
    CHECK(!color.r && !color.g && !color.b && !color.master);
    CHECK(!kl_update_indicator_begin(NULL, 1024, 0));
    CHECK(!kl_update_indicator_begin(&s, 0, 0));
    CHECK(kl_update_indicator_begin(&s, 1000, 100));
    CHECK(s.phase == KL_UPDATE_RECEIVING && s.generation == 1 && !s.received_bytes);
    CHECK(s.started_ms == 100 && !s.verified_ms && !kl_update_indicator_progress(&s));
    copy = s;
    CHECK(!kl_update_indicator_begin(&s, 1, 0) && !memcmp(&s, &copy, sizeof(s)));
    CHECK(!kl_update_indicator_verify(&s, 100));
    for (uint32_t n = 0; n <= 1000; ++n) {
        uint16_t before = kl_update_indicator_progress(&s);
        CHECK(kl_update_indicator_advance(&s, n));
        CHECK(kl_update_indicator_progress(&s) >= before && kl_update_indicator_progress(&s) <= 990);
    }
    CHECK(kl_update_indicator_progress(&s) == 990);
    copy = s;
    CHECK(!kl_update_indicator_advance(&s, 999) && !memcmp(&s, &copy, sizeof(s)));
    CHECK(!kl_update_indicator_advance(&s, 1001) && !memcmp(&s, &copy, sizeof(s)));
    CHECK(!kl_update_indicator_verify(&s, 99));
    CHECK(kl_update_indicator_verify(&s, 1200));
    CHECK(s.phase == KL_UPDATE_VERIFIED && s.verified_ms == 1200 && kl_update_indicator_progress(&s) == 1000);
    CHECK(!kl_update_indicator_verify(&s, 1300));
    CHECK(!kl_update_indicator_advance(&s, 0));
    CHECK(!kl_update_indicator_begin(&s, 1, 1300));
    kl_update_indicator_fail(&s);
    CHECK(s.phase == KL_UPDATE_VERIFIED); /* Late failure cannot erase accepted commit evidence. */

    memset(&s, 0, sizeof(s));
    CHECK(kl_update_indicator_begin(&s, 100, 0));
    CHECK(kl_update_indicator_advance(&s, 50));
    kl_update_indicator_fail(&s);
    CHECK(s.phase == KL_UPDATE_FAILED && kl_update_indicator_progress(&s) == 495);
    CHECK(!kl_update_indicator_sample(&s, 1500, &color));
    CHECK(!color.r && !color.g && !color.b && !color.master);
    CHECK(!kl_update_indicator_advance(&s, 100) && !kl_update_indicator_verify(&s, 2000));
    CHECK(kl_update_indicator_begin(&s, 100, 3000));
    CHECK(s.generation == 2 && !s.received_bytes && !s.verified_ms);
    kl_update_indicator_fail(&s); s.generation = UINT32_MAX;
    CHECK(kl_update_indicator_begin(&s, UINT32_MAX, UINT64_MAX-1));
    CHECK(s.generation == 1);
    CHECK(kl_update_indicator_advance(&s, UINT32_MAX));
    CHECK(kl_update_indicator_progress(&s) == 990);
    CHECK(kl_update_indicator_verify(&s, UINT64_MAX));
    CHECK(kl_update_indicator_sample(&s, UINT64_MAX, &color));
    CHECK(color.b >= 43 && color.b <= 255 && color.master == 12);
    CHECK(!kl_update_indicator_advance(NULL, 1) && !kl_update_indicator_verify(NULL, 1));
    kl_update_indicator_fail(NULL);
    CHECK(!kl_update_indicator_progress(NULL));
    CHECK(!kl_update_indicator_sample(&s, 0, NULL));
}

static void breathing(void) {
    kl_update_indicator s = {0};
    kl_update_indicator_color c, previous, repeat, symmetric;
    bool blue_levels[256] = {0};
    CHECK(kl_update_indicator_begin(&s, 1000, 10000));
    CHECK(kl_update_indicator_sample(&s, 10000, &c) && c.r == 0 && c.g == 0 && c.b == 43);
    CHECK(kl_update_indicator_sample(&s, 11600, &c) && c.r == 0 && c.g == 0 && c.b == 255);
    CHECK(kl_update_indicator_sample(&s, 9999, &c) && c.b == 43);
    for (unsigned progress = 0; progress <= 1000; progress += 125) {
        CHECK(kl_update_indicator_advance(&s, progress));
        CHECK(kl_update_indicator_sample(&s, 10000, &previous));
        for (uint32_t ms = 0; ms <= 3200; ++ms) {
            CHECK(kl_update_indicator_sample(&s, 10000+ms, &c));
            CHECK(c.r >= 0 && c.r <= 170 && c.g == 0 && c.b >= 43 && c.b <= 255);
            CHECK(c.master == 12 && c.r*c.master/255.0f <= 8 && c.b*c.master/255.0f <= 12);
            blue_levels[(unsigned)(c.b+.5f)] = true;
            CHECK(isfinite(c.r) && isfinite(c.b));
            CHECK(fabsf(c.b-previous.b) < .21f); /* No discontinuity at turnaround/wrap. */
            if (ms && ms <= 1600) CHECK(c.b >= previous.b);
            if (ms > 1600) CHECK(c.b <= previous.b);
            CHECK(kl_update_indicator_sample(&s, 10000+ms+3200, &repeat));
            CHECK(c.r == repeat.r && c.g == repeat.g && c.b == repeat.b);
            CHECK(kl_update_indicator_sample(&s, 10000+3200-ms, &symmetric));
            CHECK(c.r == symmetric.r && c.b == symmetric.b);
            /* Fixed-point rounding may change the ratio by less than one
             * sub-byte quantum, never amplify the bounded blue channel. */
            float ideal = c.b * kl_update_indicator_progress(&s) / 1500.0f;
            CHECK(c.r <= ideal+.00001f && c.r > ideal-1.0f/256.0f-.00001f);
            previous = c;
        }
    }
    unsigned distinct = 0;
    for (unsigned n=0; n<256; ++n) if (blue_levels[n]) ++distinct;
    CHECK(distinct == 213); /* Hardware appearance still depends on controller PWM. */
    CHECK(kl_update_indicator_verify(&s, 15000));
    CHECK(kl_update_indicator_sample(&s, 11600, &c) && c.r == 170 && c.b == 255 && c.g == 0);
    CHECK(kl_update_indicator_sample(&s, UINT64_MAX, &c));
    CHECK(c.r <= 170 && c.b <= 255 && c.g == 0 && c.master == 12);
    /* Progress affects hue only. A fixed breath phase never moves toward blue
     * as accepted bytes increase, including a single-chunk upload. */
    memset(&s, 0, sizeof(s)); CHECK(kl_update_indicator_begin(&s, 65535, 0));
    float red = 0;
    for (uint32_t n = 0; n <= 65535; ++n) {
        CHECK(kl_update_indicator_advance(&s, n));
        CHECK(kl_update_indicator_sample(&s, 1600, &c));
        CHECK(c.r >= red && c.r < 170 && c.b == 255);
        red = c.r;
    }
}

int main(void) {
    lifecycle(); breathing();
    kl_update_indicator_color failure, repeated;
    CHECK(!kl_update_indicator_failure_sample(0, NULL));
    for (unsigned ms=0; ms<1600; ++ms) {
        CHECK(kl_update_indicator_failure_sample(ms, &failure));
        CHECK(failure.r>=43 && failure.r<=255 && !failure.g && !failure.b && failure.master==12);
        CHECK(kl_update_indicator_failure_sample(ms%800, &repeated));
        CHECK(failure.r==repeated.r);
    }
    CHECK(!kl_update_indicator_failure_sample(1600, &failure) && !failure.r && !failure.master);
    CHECK(!kl_update_indicator_failure_sample(UINT64_MAX, &failure));
    printf("update indicator: %u assertions passed; pure state/math, no device I/O\n", checks);
    return 0;
}
