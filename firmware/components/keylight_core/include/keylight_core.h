#ifndef KEYLIGHT_CORE_H
#define KEYLIGHT_CORE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { KL_WHITE, KL_COLOR } kl_mode;
typedef enum { KL_EFFECT_NONE, KL_EFFECT_AURORA, KL_EFFECT_BREATHE } kl_effect;
typedef struct { uint8_t r, g, b; } kl_rgb;

typedef struct {
    bool power;
    kl_mode mode;
    uint8_t brightness;
    uint16_t temperature_k;
    kl_rgb rgb;
    uint16_t transition_ms;
    kl_effect effect;
    bool recording_lock;
} kl_state;

enum {
    KL_POWER = 1u << 0, KL_MODE = 1u << 1, KL_BRIGHTNESS = 1u << 2,
    KL_TEMPERATURE = 1u << 3, KL_RGB = 1u << 4, KL_TRANSITION = 1u << 5,
    KL_EFFECT = 1u << 6, KL_LOCK = 1u << 7,
    KL_ALL_FIELDS = 255u,
    KL_OUTPUT_FIELDS = KL_POWER | KL_MODE | KL_BRIGHTNESS | KL_TEMPERATURE | KL_RGB | KL_EFFECT
};

typedef struct { uint32_t fields; kl_state value; } kl_patch;
typedef enum { KL_OK, KL_INVALID, KL_LOCKED } kl_result;

/* Applies a fully validated patch atomically. An unlock is a separate command. */
kl_result kl_state_patch(const kl_state *current, const kl_patch *patch, kl_state *next);
bool kl_state_valid(const kl_state *state);
kl_state kl_state_default(void);

typedef struct { float r, g, b, white, temperature_k; } kl_frame;
typedef struct {
    kl_frame origin;
    kl_state target;
    uint64_t started_ms;
    uint32_t duration_ms;
} kl_transition;

/* Uses monotonic elapsed time; delayed calls sample now rather than queue old frames. */
void kl_transition_begin(kl_transition *transition, const kl_frame *current,
                         const kl_state *target, uint64_t now_ms);
kl_frame kl_transition_sample(const kl_transition *transition, uint64_t now_ms);
bool kl_transition_done(const kl_transition *transition, uint64_t now_ms);
kl_frame kl_state_frame(const kl_state *state, uint64_t effect_elapsed_ms);
uint8_t kl_byte(float value);

#endif
