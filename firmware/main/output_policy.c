#include "output_policy.h"

uint8_t kl_native_level(uint8_t percent) { return (uint8_t)((percent * 255u + 50u) / 100u); }

static okl_result effect_off(uint8_t *effect, kl_output_exchange exchange, void *user) {
    uint8_t bytes[12] = {0}; okl_request request;
    okl_result result = okl_request_build(&request, OKL_SET_EFFECT, bytes, sizeof(bytes));
    if (result == OKL_OK) result = exchange(user, &request);
    if (result == OKL_OK) *effect = 0;
    return result;
}
static okl_result white(uint8_t level, uint8_t effect, kl_output_exchange exchange, void *user) {
    okl_request request;
    okl_result result = okl_request_white_brightness(&request, level, effect);
    return result == OKL_OK ? exchange(user, &request) : result;
}
static okl_result color_level(uint8_t level, kl_output_exchange exchange, void *user) {
    okl_request request;
    okl_result result = okl_request_color_brightness(&request, level);
    return result == OKL_OK ? exchange(user, &request) : result;
}
okl_result kl_output_park(const kl_state *target, uint8_t *effect, kl_output_exchange exchange, void *user) {
    if (!target || !effect || !exchange || !kl_state_valid(target) || target->mode != KL_COLOR || target->effect != KL_EFFECT_NONE)
        return OKL_INVALID;
    /* Lower the master before exposing unscaled RGB; never flash at the old 100% master. */
    okl_result result = color_level(kl_native_level(target->brightness), exchange, user);
    if (result != OKL_OK) return result;
    uint8_t rgb[] = {target->rgb.r, target->rgb.g, target->rgb.b}; okl_request request;
    result = okl_request_static(&request, rgb);
    if (result == OKL_OK) result = exchange(user, &request);
    if (result == OKL_OK) *effect = 1;
    return result;
}
okl_result kl_output_prepare(const kl_state *target, bool animated, const kl_frame *initial, uint8_t *effect,
                            kl_output_exchange exchange, void *user) {
    if (!target || !effect || !exchange || !kl_state_valid(target) || (animated && !initial)) return OKL_INVALID;
    okl_result result;
    if (!target->power) {
        /* Off never depends on temperature or a colour-setting transaction. */
        if ((result = white(0, *effect, exchange, user)) != OKL_OK) return result;
        return effect_off(effect, exchange, user);
    }
    if (target->mode == KL_WHITE) {
        if ((result = effect_off(effect, exchange, user)) != OKL_OK) return result;
        okl_request request;
        result = okl_request_temperature(&request, target->temperature_k);
        if (result == OKL_OK) result = exchange(user, &request);
        if (result != OKL_OK) return result;
        return white(kl_native_level(target->brightness), *effect, exchange, user);
    }
    if ((result = white(0, *effect, exchange, user)) != OKL_OK) return result;
    if (!animated) return kl_output_park(target, effect, exchange, user);
    /* Native static RGB and an old custom framebuffer may both be unscaled.
     * Hide them before mode setup, and expose only an ACKed scaled frame. A
     * setup failure leaves the master dark instead of replaying a command. */
    if ((result = color_level(0, exchange, user)) != OKL_OK) return result;
    okl_request request;
    result = okl_request_custom(&request);
    if (result == OKL_OK) result = exchange(user, &request);
    if (result != OKL_OK) return result;
    *effect = 8;
    uint8_t rgb[] = {kl_byte(initial->r), kl_byte(initial->g), kl_byte(initial->b)};
    result = okl_request_frame(&request, rgb);
    if (result == OKL_OK) result = exchange(user, &request);
    if (result != OKL_OK) return result;
    return color_level(255, exchange, user);
}

bool kl_native_matches(const kl_state *target, const okl_light_state *s) {
    if (!target || !s) return false;
    if (!target->power) return s->white_brightness == 0 && (!s->effect || s->color_brightness == 0);
    if (target->mode == KL_WHITE)
        return s->effect == 0 && s->white_brightness == kl_native_level(target->brightness)
            && s->temperature_kelvin == target->temperature_k;
    return target->effect == KL_EFFECT_NONE && s->white_brightness == 0 && s->effect == 1
        && s->color_brightness == kl_native_level(target->brightness) && s->color_count == 1
        && s->colors[0] == target->rgb.r && s->colors[1] == target->rgb.g && s->colors[2] == target->rgb.b;
}

uint32_t kl_native_report(const kl_state *preferences, const okl_light_state *s, kl_state *out) {
    *out = *preferences;
    bool white_active = s->white_brightness != 0;
    bool color_active = s->effect != 0 && s->color_brightness != 0;
    out->power = white_active || color_active;
    out->temperature_k = s->temperature_kelvin;
    out->effect = KL_EFFECT_NONE;
    uint32_t fields = KL_POWER | KL_TEMPERATURE;
    if (s->effect == 0 || s->effect == 1) fields |= KL_EFFECT;
    if (white_active && !color_active) {
        out->mode = KL_WHITE;
        out->brightness = (s->white_brightness * 100u + 127u) / 255u;
        fields |= KL_MODE | KL_BRIGHTNESS;
    } else if (!white_active && s->effect) {
        out->mode = KL_COLOR;
        out->brightness = (s->color_brightness * 100u + 127u) / 255u;
        fields |= KL_MODE | KL_BRIGHTNESS;
    }
    if (s->effect == 1 && s->color_count == 1) {
        out->rgb = (kl_rgb){s->colors[0], s->colors[1], s->colors[2]};
        fields |= KL_RGB;
    }
    return fields;
}

bool kl_report_publishable(bool connected, bool valid, bool idle,
                           uint32_t output_revision, uint32_t reported_revision, uint32_t fields) {
    return connected && valid && idle && output_revision == reported_revision && (fields & KL_POWER);
}

/* IEC sRGB transfer to linear light, rounded to the available channel byte. */
static const uint8_t srgb_linear[256] = {
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7,
    8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 12, 12, 12, 13,
    13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 17, 18, 18, 19, 19, 20,
    20, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 27, 27, 28, 29, 29,
    30, 30, 31, 32, 32, 33, 34, 35, 35, 36, 37, 37, 38, 39, 40, 41,
    41, 42, 43, 44, 45, 45, 46, 47, 48, 49, 50, 51, 51, 52, 53, 54,
    55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70,
    71, 72, 73, 74, 76, 77, 78, 79, 80, 81, 82, 84, 85, 86, 87, 88,
    90, 91, 92, 93, 95, 96, 97, 99, 100, 101, 103, 104, 105, 107, 108, 109,
    111, 112, 114, 115, 116, 118, 119, 121, 122, 124, 125, 127, 128, 130, 131, 133,
    134, 136, 138, 139, 141, 142, 144, 146, 147, 149, 151, 152, 154, 156, 157, 159,
    161, 163, 164, 166, 168, 170, 171, 173, 175, 177, 179, 181, 183, 184, 186, 188,
    190, 192, 194, 196, 198, 200, 202, 204, 206, 208, 210, 212, 214, 216, 218, 220,
    222, 224, 226, 229, 231, 233, 235, 237, 239, 242, 244, 246, 248, 250, 253, 255,
};
uint8_t kl_srgb_channel(uint8_t value) { return srgb_linear[value]; }
uint8_t kl_srgb_reconstruct(uint8_t value) {
    unsigned lower = 0, upper = 255;
    while (lower < upper) {
        unsigned middle = lower + (upper - lower) / 2;
        if (srgb_linear[middle] < value) lower = middle + 1;
        else upper = middle;
    }
    if (lower && value - srgb_linear[lower - 1] <= srgb_linear[lower] - value) --lower;
    return (uint8_t)lower;
}

static float color_channel(float value, kl_output_encoding encoding) {
    if (value <= 0.0f) return 0.0f;
    if (value >= 255.0f) return 255.0f;
    if (encoding == KL_OUTPUT_LINEAR) return value;
    unsigned lower = (unsigned)value;
    return srgb_linear[lower] + (srgb_linear[lower + 1] - srgb_linear[lower]) * (value - lower);
}
kl_frame kl_color_frame(const kl_state *state, kl_output_encoding encoding, uint64_t elapsed) {
    if (!state || !kl_state_valid(state) || (encoding != KL_OUTPUT_SRGB && encoding != KL_OUTPUT_LINEAR))
        return (kl_frame){0};
    if (state->mode != KL_COLOR || !state->power) return kl_state_frame(state, elapsed);
    kl_state full = *state; full.brightness = 100;
    kl_frame result = kl_state_frame(&full, elapsed);
    result.r = color_channel(result.r, encoding) * state->brightness / 100.0f;
    result.g = color_channel(result.g, encoding) * state->brightness / 100.0f;
    result.b = color_channel(result.b, encoding) * state->brightness / 100.0f;
    return result;
}
kl_frame kl_color_sample(const kl_transition *t, kl_output_encoding encoding, uint64_t now) {
    if (!t) return (kl_frame){0};
    uint64_t elapsed = now >= t->started_ms ? now - t->started_ms : 0;
    kl_frame target = kl_color_frame(&t->target, encoding, elapsed);
    if (!t->duration_ms || elapsed >= t->duration_ms) return target;
    float u = (float)elapsed / t->duration_ms;
    u = u * u * (3.0f - 2.0f * u);
    return (kl_frame){t->origin.r + (target.r - t->origin.r) * u,
        t->origin.g + (target.g - t->origin.g) * u,
        t->origin.b + (target.b - t->origin.b) * u,
        t->origin.white + (target.white - t->origin.white) * u,
        t->origin.temperature_k + (target.temperature_k - t->origin.temperature_k) * u};
}
okl_result kl_color_enter(const okl_light_state *native, const uint8_t known[3], uint8_t initial[3],
                          uint8_t *effect, kl_output_exchange exchange, void *user) {
    if (!native || !initial || !effect || !exchange) return OKL_INVALID;
    bool own_custom = native->effect == 8 && native->color_brightness == 255 && known;
    bool known_static = native->effect == 1 && native->color_count == 1;
    bool muted = false;
    for (unsigned i = 0; i < 3; ++i) {
        /* Match the legacy controller's integer master percentage during the
         * one-time import, rather than briefly increasing a dim source. */
        initial[i] = own_custom ? known[i] : known_static ?
            (uint8_t)((native->colors[i] * (native->color_brightness * 100u / 255u)) / 100u) : 0;
    }
    *effect = native->effect;
    okl_result result;
    if (native->white_brightness && (result = white(0, *effect, exchange, user)) != OKL_OK) return result;
    if (own_custom) return OKL_OK;
    if (!known_static && native->effect && native->color_brightness) {
        if ((result = color_level(0, exchange, user)) != OKL_OK) return result;
        muted = true;
    }
    okl_request request;
    result = okl_request_frame(&request, initial);
    if (result == OKL_OK) result = exchange(user, &request);
    if (result != OKL_OK) return result;
    if (*effect != 8) {
        result = okl_request_custom(&request);
        if (result == OKL_OK) result = exchange(user, &request);
        if (result != OKL_OK) return result;
        *effect = 8;
    }
    if (muted || native->color_brightness != 255) return color_level(255, exchange, user);
    return OKL_OK;
}
okl_result kl_color_park(const uint8_t rgb[3], uint8_t *effect, kl_output_exchange exchange, void *user) {
    if (!rgb || !effect || *effect != 8 || !exchange) return OKL_INVALID;
    okl_request request; okl_result result = okl_request_static(&request, rgb);
    if (result == OKL_OK) result = exchange(user, &request);
    if (result == OKL_OK) *effect = 1;
    return result;
}
bool kl_color_matches(const kl_state *target, kl_output_encoding encoding, const okl_light_state *native) {
    if (!target || !native || !target->power || target->mode != KL_COLOR || target->effect != KL_EFFECT_NONE ||
        !kl_state_valid(target) || (encoding != KL_OUTPUT_SRGB && encoding != KL_OUTPUT_LINEAR)) return false;
    kl_frame value = kl_color_frame(target, encoding, 0);
    return native->effect == 1 && native->color_count == 1 && native->white_brightness == 0 &&
        native->color_brightness == 255 && native->colors[0] == kl_byte(value.r) &&
        native->colors[1] == kl_byte(value.g) && native->colors[2] == kl_byte(value.b);
}
