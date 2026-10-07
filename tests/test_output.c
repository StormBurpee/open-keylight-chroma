#include "output_policy.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    okl_light_state state;
    unsigned calls, temperatures, fail_call;
    bool acknowledge_without_change;
    bool check_visible_limit;
    bool check_visible_floor;
    uint8_t frame[3], visible_limit[3], visible_floor[3];
    okl_request requests[8];
} fake_controller;

static okl_result exchange(void *user, const okl_request *request) {
    fake_controller *f = user;
    assert(f->calls < 8); f->requests[f->calls++] = *request;
    if (request->command == OKL_SET_TEMPERATURE) f->temperatures++;
    if (f->fail_call == f->calls) return OKL_TIMEOUT;
    if (f->acknowledge_without_change) return OKL_OK;
    const uint8_t *a = request->arguments;
    switch (request->command) {
    case OKL_SET_EFFECT:
        f->state.effect = a[2]; f->state.color_count = a[5]; memcpy(f->state.colors, a + 6, 6); break;
    case OKL_SET_COLOR_BRIGHTNESS: f->state.color_brightness = a[2]; break;
    case OKL_SET_WHITE_BRIGHTNESS: f->state.white_brightness = a[2]; break;
    case OKL_SET_TEMPERATURE: f->state.temperature_kelvin = (uint16_t)(a[2] * 256u + a[3]); break;
    case OKL_SET_FRAME: memcpy(f->frame, a + 5, 3); break;
    default: assert(false);
    }
    if (f->check_visible_limit || f->check_visible_floor) {
        const uint8_t *rgb = f->state.effect == 8 ? f->frame : f->state.colors;
        for (unsigned i = 0; i < 3; ++i) {
            unsigned output = f->state.effect ? (rgb[i] * f->state.color_brightness + 127u) / 255u : 0;
            if (f->check_visible_limit) assert(output <= f->visible_limit[i]);
            if (f->check_visible_floor) assert(output >= f->visible_floor[i]);
        }
    }
    return OKL_OK;
}

static void test_park_and_verification(void) {
    kl_state target = kl_state_default(); target.power = true; target.mode = KL_COLOR;
    target.rgb = (kl_rgb){255, 0, 32}; target.brightness = 40;
    fake_controller fake = {.state = {.effect = 8, .color_brightness = 255, .temperature_kelvin = 4500}};
    uint8_t effect = 8;
    assert(kl_output_park(&target, &effect, exchange, &fake) == OKL_OK);
    assert(fake.calls == 2 && fake.requests[0].command == OKL_SET_COLOR_BRIGHTNESS);
    assert(fake.state.color_brightness == 102 && fake.state.colors[0] == 255 && fake.state.colors[2] == 32);
    assert(effect == 1 && kl_native_matches(&target, &fake.state));
    fake = (fake_controller){.state = {.effect = 1, .color_count = 1, .colors = {4, 5, 6}, .color_brightness = 33}, .acknowledge_without_change = true};
    assert(kl_output_park(&target, &effect, exchange, &fake) == OKL_OK);
    assert(!kl_native_matches(&target, &fake.state)); /* ACK alone is never confirmation. */
    fake.state = (okl_light_state){.effect = 1, .color_count = 1, .colors = {255, 0, 32}, .color_brightness = 102};
    assert(kl_native_matches(&target, &fake.state));
    fake.state.white_brightness = 1; assert(!kl_native_matches(&target, &fake.state));
    fake.state.white_brightness = 0; fake.state.color_brightness = 103; assert(!kl_native_matches(&target, &fake.state));
    fake.state.color_brightness = 102; fake.state.color_count = 2; assert(!kl_native_matches(&target, &fake.state));
}

static void test_off_and_failures(void) {
    kl_state target = kl_state_default(); target.power = false;
    fake_controller fake = {.state = {.effect = 1, .white_brightness = 38, .color_brightness = 200}};
    uint8_t effect = 1;
    assert(kl_output_prepare(&target, false, NULL, &effect, exchange, &fake) == OKL_OK);
    assert(fake.calls == 2 && fake.temperatures == 0);
    assert(fake.requests[0].command == OKL_SET_WHITE_BRIGHTNESS && fake.requests[0].arguments[2] == 0);
    assert(fake.state.effect == 0 && fake.state.white_brightness == 0 && kl_native_matches(&target, &fake.state));
    /* Every failure boundary returns without replaying that command or sending later commands. */
    for (unsigned fail = 1; fail <= 2; fail++) {
        fake = (fake_controller){.fail_call = fail}; effect = 1;
        assert(kl_output_prepare(&target, false, NULL, &effect, exchange, &fake) == OKL_TIMEOUT);
        assert(fake.calls == fail && fake.temperatures == 0);
    }
    target.power = true; target.mode = KL_WHITE;
    for (unsigned fail = 1; fail <= 3; fail++) {
        fake = (fake_controller){.fail_call = fail}; effect = 0;
        assert(kl_output_prepare(&target, false, NULL, &effect, exchange, &fake) == OKL_TIMEOUT);
        assert(fake.calls == fail);
    }
}

static void test_all_brightness_levels(void) {
    kl_state target = kl_state_default(); target.power = true;
    for (unsigned percent = 0; percent <= 100; percent++) {
        target.brightness = (uint8_t)percent;
        fake_controller fake = {0}; uint8_t effect = 0;
        assert(kl_output_prepare(&target, false, NULL, &effect, exchange, &fake) == OKL_OK);
        assert(kl_native_matches(&target, &fake.state));
        assert((kl_native_level((uint8_t)percent) * 100u + 127u) / 255u == percent);
        fake.state.temperature_kelvin++; assert(!kl_native_matches(&target, &fake.state));
        fake.state.temperature_kelvin--; fake.state.effect = 1; assert(!kl_native_matches(&target, &fake.state));
    }
}

static void test_honest_report_fields(void) {
    kl_state preferences = kl_state_default(), reported;
    okl_light_state native = {.effect = 1, .color_count = 1, .colors = {255, 0, 32}, .color_brightness = 102, .temperature_kelvin = 4100};
    uint32_t fields = kl_native_report(&preferences, &native, &reported);
    assert(reported.mode == KL_COLOR && reported.brightness == 40 && reported.rgb.r == 255 && reported.rgb.b == 32);
    assert((fields & (KL_MODE | KL_BRIGHTNESS | KL_RGB)) == (KL_MODE | KL_BRIGHTNESS | KL_RGB));
    native.white_brightness = 30;
    fields = kl_native_report(&preferences, &native, &reported);
    assert(reported.power && !(fields & (KL_MODE | KL_BRIGHTNESS))); /* Mixed output cannot be described as white-only. */
    native = (okl_light_state){.color_brightness = 255, .temperature_kelvin = 4100};
    fields = kl_native_report(&preferences, &native, &reported);
    assert(!reported.power && !(fields & (KL_MODE | KL_BRIGHTNESS | KL_RGB)));
    native.effect = 8; fields = kl_native_report(&preferences, &native, &reported);
    assert(!(fields & (KL_RGB | KL_EFFECT))); /* Custom frame colour has no native getter. */
}

static void test_publication_revision(void) {
    assert(kl_report_publishable(true, true, true, 4, 4, KL_POWER));
    assert(!kl_report_publishable(true, true, false, 4, 4, KL_POWER));
    assert(!kl_report_publishable(true, true, true, 5, 4, KL_POWER));
    assert(!kl_report_publishable(true, false, true, 4, 4, KL_POWER));
    assert(!kl_report_publishable(false, true, true, 4, 4, KL_POWER));
    assert(!kl_report_publishable(true, true, true, 4, 4, 0));
}

static void test_animation_handoff(void) {
    kl_state target = kl_state_default(); target.power = true; target.mode = KL_COLOR;
    target.rgb = (kl_rgb){12, 240, 130}; target.brightness = 70;
    for (unsigned level = 0; level <= 255; ++level) {
        fake_controller fake = {.state = {.effect = 1, .color_count = 1,
            .colors = {255, 127, 64}, .color_brightness = (uint8_t)level},
            .frame = {255, 255, 255}, .check_visible_limit = true};
        kl_frame initial = {.r = (float)level, .g = 127.0f * level / 255.0f, .b = 64.0f * level / 255.0f};
        fake.visible_limit[0] = kl_byte(initial.r); fake.visible_limit[1] = kl_byte(initial.g);
        fake.visible_limit[2] = kl_byte(initial.b);
        uint8_t effect = 1;
        assert(kl_output_prepare(&target, true, &initial, &effect, exchange, &fake) == OKL_OK);
        assert(fake.calls == 5 && effect == 8);
        assert(fake.requests[0].command == OKL_SET_WHITE_BRIGHTNESS);
        assert(fake.requests[1].command == OKL_SET_COLOR_BRIGHTNESS && fake.requests[1].arguments[2] == 0);
        assert(fake.requests[2].command == OKL_SET_EFFECT && fake.requests[2].arguments[2] == 8);
        assert(fake.requests[3].command == OKL_SET_FRAME);
        assert(fake.requests[4].command == OKL_SET_COLOR_BRIGHTNESS && fake.requests[4].arguments[2] == 255);
        assert(!memcmp(fake.frame, fake.visible_limit, 3));
    }
    kl_frame initial = {.r = 70, .g = 30, .b = 12};
    for (unsigned fail = 1; fail <= 5; ++fail) {
        fake_controller fake = {.state = {.effect = 1, .color_count = 1, .colors = {255, 0, 0}, .color_brightness = 70}, .fail_call = fail};
        uint8_t effect = 1;
        assert(kl_output_prepare(&target, true, &initial, &effect, exchange, &fake) == OKL_TIMEOUT);
        assert(fake.calls == fail); /* No replay and no later master enable. */
        if (fail >= 3) assert(fake.state.color_brightness == 0);
        assert(effect == (fail >= 4 ? 8 : 1));
    }
    fake_controller fake = {0}; uint8_t effect = 1;
    assert(kl_output_prepare(&target, true, NULL, &effect, exchange, &fake) == OKL_INVALID);
    assert(fake.calls == 0);
}

static void test_srgb_forward_and_canonical_readback(void) {
    kl_state target = kl_state_default(); target.power = true; target.mode = KL_COLOR; target.effect = KL_EFFECT_NONE;
    assert(kl_srgb_channel(0) == 0 && kl_srgb_channel(255) == 255 && kl_srgb_channel(32) == 4);
    for (unsigned code = 0; code <= 255; ++code) {
        double value = code / 255.0;
        double linear = value <= .04045 ? value / 12.92 : pow((value + .055) / 1.055, 2.4);
        assert(kl_srgb_channel((uint8_t)code) == (unsigned)floor(linear * 255 + .5));
        if (code) assert(kl_srgb_channel((uint8_t)code) >= kl_srgb_channel((uint8_t)(code - 1)));
        unsigned reconstructed = kl_srgb_reconstruct((uint8_t)code);
        int distance = abs((int)kl_srgb_channel((uint8_t)reconstructed) - (int)code);
        assert(distance <= 1);
        for (unsigned candidate = 0; candidate <= 255; ++candidate)
            assert(distance <= abs((int)kl_srgb_channel((uint8_t)candidate) - (int)code));
        for (unsigned brightness = 0; brightness <= 100; ++brightness) {
            target.rgb = (kl_rgb){(uint8_t)code, 32, 255}; target.brightness = (uint8_t)brightness;
            for (unsigned encoding = 0; encoding < 2; ++encoding) {
                kl_frame frame = kl_color_frame(&target, (kl_output_encoding)encoding, 0);
                unsigned channel = encoding == KL_OUTPUT_SRGB ? kl_srgb_channel((uint8_t)code) : code;
                assert(kl_byte(frame.r) == (channel * brightness + 50u) / 100u);
                okl_light_state native = {.effect = 1, .color_count = 1, .color_brightness = 255,
                    .colors = {kl_byte(frame.r), kl_byte(frame.g), kl_byte(frame.b)}};
                assert(kl_color_matches(&target, (kl_output_encoding)encoding, &native));
                native.colors[0] ^= 1; assert(!kl_color_matches(&target, (kl_output_encoding)encoding, &native));
                native.colors[0] ^= 1; native.color_brightness = 254;
                assert(!kl_color_matches(&target, (kl_output_encoding)encoding, &native));
            }
        }
    }
    target.mode = KL_WHITE;
    kl_frame white = kl_color_frame(&target, KL_OUTPUT_SRGB, 0), raw = kl_state_frame(&target, 0);
    assert(!memcmp(&white, &raw, sizeof(white)));
    target.mode = KL_COLOR; target.brightness = 100; target.rgb = (kl_rgb){255, 0, 32}; target.transition_ms = 100;
    kl_frame origin = {.r = 4, .g = 100, .b = 55}; kl_transition transition;
    kl_transition_begin(&transition, &origin, &target, 500);
    kl_frame start = kl_color_sample(&transition, KL_OUTPUT_SRGB, 500);
    kl_frame middle = kl_color_sample(&transition, KL_OUTPUT_SRGB, 550);
    kl_frame end = kl_color_sample(&transition, KL_OUTPUT_SRGB, 600);
    assert(start.r == 4 && start.g == 100 && start.b == 55);
    assert(middle.r == 129.5f && middle.g == 50 && middle.b == 29.5f);
    assert(end.r == 255 && end.g == 0 && end.b == 4);
    kl_frame late = kl_color_sample(&transition, KL_OUTPUT_SRGB, 20000);
    assert(end.r == late.r && end.g == late.g && end.b == late.b);
}

static void test_canonical_handoff_never_blanks_or_reveals_stale_frame(void) {
    for (unsigned channel = 0; channel <= 255; ++channel) {
        uint8_t rgb[3] = {(uint8_t)channel, 91, 14}, initial[3];
        fake_controller fake = {.state = {.effect = 1, .color_count = 1,
            .color_brightness = 255, .colors = {(uint8_t)channel, 91, 14}}, .frame = {255, 255, 255},
            .check_visible_limit = true, .check_visible_floor = true};
        memcpy(fake.visible_limit, rgb, 3); memcpy(fake.visible_floor, rgb, 3);
        uint8_t effect = 1;
        assert(kl_color_enter(&fake.state, NULL, initial, &effect, exchange, &fake) == OKL_OK);
        assert(fake.calls == 2 && effect == 8 && !memcmp(initial, rgb, 3));
        assert(fake.requests[0].command == OKL_SET_FRAME);
        assert(fake.requests[1].command == OKL_SET_EFFECT && fake.requests[1].arguments[2] == 8);
        assert(kl_color_park(rgb, &effect, exchange, &fake) == OKL_OK);
        assert(effect == 1 && fake.calls == 3 && fake.state.color_brightness == 255);
        assert(!memcmp(fake.state.colors, rgb, 3));
    }
    fake_controller fake = {.state = {.effect = 8, .color_brightness = 255}, .frame = {19, 80, 220}};
    uint8_t initial[3], effect = 8;
    assert(kl_color_enter(&fake.state, fake.frame, initial, &effect, exchange, &fake) == OKL_OK);
    assert(fake.calls == 0 && !memcmp(initial, fake.frame, 3)); /* Retarget preserves the owned stream. */
    for (unsigned fail = 1; fail <= 2; ++fail) {
        fake = (fake_controller){.state = {.effect = 1, .color_count = 1, .color_brightness = 255,
            .colors = {19, 80, 220}}, .fail_call = fail}; effect = 1;
        assert(kl_color_enter(&fake.state, NULL, initial, &effect, exchange, &fake) == OKL_TIMEOUT);
        assert(fake.calls == fail && fake.state.color_brightness == 255);
    }
    fake = (fake_controller){.state = {.effect = 8, .color_brightness = 255}, .frame = {255, 255, 255}};
    assert(kl_color_enter(&fake.state, NULL, initial, &effect, exchange, &fake) == OKL_OK);
    assert(fake.calls == 3 && fake.requests[0].command == OKL_SET_COLOR_BRIGHTNESS && !fake.requests[0].arguments[2]);
    assert(fake.frame[0] == 0 && fake.frame[1] == 0 && fake.frame[2] == 0 && fake.state.color_brightness == 255);
}

int main(void) {
    test_park_and_verification(); test_off_and_failures(); test_all_brightness_levels();
    test_honest_report_fields(); test_publication_revision();
    test_animation_handoff();
    test_srgb_forward_and_canonical_readback();
    test_canonical_handoff_never_blanks_or_reveals_stale_frame();
    puts("output: exact native readback, normalized parking, Off ordering, failures, mixed fields and publication revision passed");
    return 0;
}
