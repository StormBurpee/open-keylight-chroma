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
okl_result kl_output_prepare(const kl_state *target, bool animated, uint8_t *effect,
                            kl_output_exchange exchange, void *user) {
    if (!target || !effect || !exchange || !kl_state_valid(target)) return OKL_INVALID;
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
    if ((result = color_level(255, exchange, user)) != OKL_OK) return result;
    okl_request request;
    result = okl_request_custom(&request);
    if (result == OKL_OK) result = exchange(user, &request);
    if (result == OKL_OK) *effect = 8;
    return result;
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
