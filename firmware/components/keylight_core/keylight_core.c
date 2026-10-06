#include "keylight_core.h"
#include <math.h>

kl_state kl_state_default(void) {
    return (kl_state){ .power = false, .mode = KL_WHITE, .brightness = 50,
        .temperature_k = 4500, .rgb = {255, 96, 32}, .transition_ms = 600,
        .effect = KL_EFFECT_NONE, .recording_lock = false };
}

bool kl_state_valid(const kl_state *s) {
    return s && s->mode >= KL_WHITE && s->mode <= KL_COLOR && s->brightness <= 100
        && s->temperature_k >= 3000 && s->temperature_k <= 7000
        && s->transition_ms <= 10000 && s->effect >= KL_EFFECT_NONE
        && s->effect <= KL_EFFECT_BREATHE
        && (s->mode == KL_COLOR || s->effect == KL_EFFECT_NONE);
}

kl_result kl_state_patch(const kl_state *current, const kl_patch *p, kl_state *next) {
    if (!current || !p || !next || !p->fields || (p->fields & ~KL_ALL_FIELDS)) return KL_INVALID;
    bool only_off = (p->fields & KL_OUTPUT_FIELDS) == KL_POWER && !p->value.power;
    if (current->recording_lock && (p->fields & KL_OUTPUT_FIELDS) && !only_off) return KL_LOCKED;
    kl_state s = *current;
    if (p->fields & KL_POWER) s.power = p->value.power;
    if (p->fields & KL_MODE) s.mode = p->value.mode;
    if (p->fields & KL_BRIGHTNESS) s.brightness = p->value.brightness;
    if (p->fields & KL_TEMPERATURE) s.temperature_k = p->value.temperature_k;
    if (p->fields & KL_RGB) s.rgb = p->value.rgb;
    if (p->fields & KL_TRANSITION) s.transition_ms = p->value.transition_ms;
    if (p->fields & KL_EFFECT) s.effect = p->value.effect;
    if (p->fields & KL_LOCK) s.recording_lock = p->value.recording_lock;
    if ((p->fields & KL_MODE) && s.mode == KL_WHITE && !(p->fields & KL_EFFECT)) s.effect = KL_EFFECT_NONE;
    if (!kl_state_valid(&s)) return KL_INVALID;
    *next = s;
    return KL_OK;
}

uint8_t kl_byte(float value) {
    if (!(value > 0)) return 0;
    if (value >= 255) return 255;
    return (uint8_t)(value + 0.5f);
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

kl_frame kl_state_frame(const kl_state *s, uint64_t elapsed) {
    kl_frame f = { .temperature_k = s->temperature_k };
    if (!s->power) return f;
    float level = s->brightness / 100.0f;
    if (s->mode == KL_WHITE) { f.white = 255.0f * level; return f; }
    f.r = s->rgb.r * level; f.g = s->rgb.g * level; f.b = s->rgb.b * level;
    if (s->effect == KL_EFFECT_BREATHE) {
        float phase = (float)(elapsed % 6000u) / 6000.0f;
        float envelope = 0.18f + 0.82f * (0.5f - 0.5f * cosf(phase * 6.283185307f));
        f.r *= envelope; f.g *= envelope; f.b *= envelope;
    } else if (s->effect == KL_EFFECT_AURORA) {
        static const kl_rgb palette[] = {{18, 215, 165}, {25, 112, 255}, {172, 43, 223}, {18, 215, 165}};
        uint32_t position = (uint32_t)(elapsed % 18000u);
        unsigned segment = position / 6000u;
        float t = (position % 6000u) / 6000.0f;
        t = t * t * (3.0f - 2.0f * t);
        f.r = lerp(palette[segment].r, palette[segment + 1].r, t) * level;
        f.g = lerp(palette[segment].g, palette[segment + 1].g, t) * level;
        f.b = lerp(palette[segment].b, palette[segment + 1].b, t) * level;
    }
    return f;
}

void kl_transition_begin(kl_transition *t, const kl_frame *current, const kl_state *target, uint64_t now) {
    t->origin = *current; t->target = *target; t->started_ms = now;
    t->duration_ms = target->power ? target->transition_ms : 0;
}

bool kl_transition_done(const kl_transition *t, uint64_t now) {
    return now >= t->started_ms && now - t->started_ms >= t->duration_ms;
}

kl_frame kl_transition_sample(const kl_transition *t, uint64_t now) {
    uint64_t elapsed = now >= t->started_ms ? now - t->started_ms : 0;
    kl_frame target = kl_state_frame(&t->target, elapsed);
    if (elapsed >= t->duration_ms || !t->duration_ms) return target;
    float u = (float)elapsed / t->duration_ms;
    u = u * u * (3.0f - 2.0f * u);
    return (kl_frame){lerp(t->origin.r, target.r, u), lerp(t->origin.g, target.g, u),
        lerp(t->origin.b, target.b, u), lerp(t->origin.white, target.white, u),
        lerp(t->origin.temperature_k, target.temperature_k, u)};
}
