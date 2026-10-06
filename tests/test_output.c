#include "output_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    okl_light_state state;
    unsigned calls, temperatures, fail_call;
    bool acknowledge_without_change;
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
    default: assert(false);
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
    assert(kl_output_prepare(&target, false, &effect, exchange, &fake) == OKL_OK);
    assert(fake.calls == 2 && fake.temperatures == 0);
    assert(fake.requests[0].command == OKL_SET_WHITE_BRIGHTNESS && fake.requests[0].arguments[2] == 0);
    assert(fake.state.effect == 0 && fake.state.white_brightness == 0 && kl_native_matches(&target, &fake.state));
    /* Every failure boundary returns without replaying that command or sending later commands. */
    for (unsigned fail = 1; fail <= 2; fail++) {
        fake = (fake_controller){.fail_call = fail}; effect = 1;
        assert(kl_output_prepare(&target, false, &effect, exchange, &fake) == OKL_TIMEOUT);
        assert(fake.calls == fail && fake.temperatures == 0);
    }
    target.power = true; target.mode = KL_WHITE;
    for (unsigned fail = 1; fail <= 3; fail++) {
        fake = (fake_controller){.fail_call = fail}; effect = 0;
        assert(kl_output_prepare(&target, false, &effect, exchange, &fake) == OKL_TIMEOUT);
        assert(fake.calls == fail);
    }
}

static void test_all_brightness_levels(void) {
    kl_state target = kl_state_default(); target.power = true;
    for (unsigned percent = 0; percent <= 100; percent++) {
        target.brightness = (uint8_t)percent;
        fake_controller fake = {0}; uint8_t effect = 0;
        assert(kl_output_prepare(&target, false, &effect, exchange, &fake) == OKL_OK);
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

int main(void) {
    test_park_and_verification(); test_off_and_failures(); test_all_brightness_levels();
    test_honest_report_fields(); test_publication_revision();
    puts("output: exact native readback, normalized parking, Off ordering, failures, mixed fields and publication revision passed");
    return 0;
}
