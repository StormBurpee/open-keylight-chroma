#include "update_indicator.h"
#include <string.h>

enum { BREATH_HALF_MS = 1600, CHANNEL_MIN = 43, CHANNEL_MAX = 255, CHANNEL_SCALE = 256 };

bool kl_update_indicator_begin(kl_update_indicator *state, uint32_t bytes, uint64_t now_ms) {
    if (!state || !bytes || state->phase == KL_UPDATE_RECEIVING || state->phase == KL_UPDATE_VERIFIED)
        return false;
    uint32_t generation = state->generation + 1u;
    if (!generation) generation = 1;
    *state = (kl_update_indicator){.phase = KL_UPDATE_RECEIVING, .generation = generation,
        .total_bytes = bytes, .started_ms = now_ms};
    return true;
}

bool kl_update_indicator_advance(kl_update_indicator *state, uint32_t received) {
    if (!state || state->phase != KL_UPDATE_RECEIVING || !state->total_bytes ||
        received < state->received_bytes || received > state->total_bytes) return false;
    state->received_bytes = received;
    return true;
}

bool kl_update_indicator_verify(kl_update_indicator *state, uint64_t now_ms) {
    if (!state || state->phase != KL_UPDATE_RECEIVING || !state->total_bytes ||
        state->received_bytes != state->total_bytes || now_ms < state->started_ms) return false;
    state->phase = KL_UPDATE_VERIFIED;
    state->verified_ms = now_ms;
    return true;
}

void kl_update_indicator_fail(kl_update_indicator *state) {
    if (state && state->phase == KL_UPDATE_RECEIVING) state->phase = KL_UPDATE_FAILED;
}

uint16_t kl_update_indicator_progress(const kl_update_indicator *state) {
    if (!state || !state->total_bytes || state->received_bytes > state->total_bytes) return 0;
    if (state->phase == KL_UPDATE_VERIFIED)
        return state->received_bytes == state->total_bytes ? KL_UPDATE_PROGRESS_VERIFIED : 0;
    if (state->phase != KL_UPDATE_RECEIVING && state->phase != KL_UPDATE_FAILED) return 0;
    return (uint16_t)((uint64_t)state->received_bytes * KL_UPDATE_PROGRESS_RECEIVED / state->total_bytes);
}

bool kl_update_indicator_sample(const kl_update_indicator *state, uint64_t now_ms,
                                kl_update_indicator_color *color) {
    if (!color) return false;
    memset(color, 0, sizeof(*color));
    if (!state || (state->phase != KL_UPDATE_RECEIVING && state->phase != KL_UPDATE_VERIFIED) ||
        !state->total_bytes || state->received_bytes > state->total_bytes) return false;
    /* A 3.2-second cubic-eased breath, with zero slope at both endpoints.
     * Integer fixed-point intermediates avoid a math-library dependency.
     * A delayed call samples now; it never queues missed animation frames. */
    uint64_t elapsed = now_ms >= state->started_ms ? now_ms - state->started_ms : 0;
    uint32_t position = (uint32_t)(elapsed % (2u * BREATH_HALF_MS));
    uint32_t x = position <= BREATH_HALF_MS ? position : 2u * BREATH_HALF_MS - position;
    uint64_t eased = (uint64_t)x * x * (3u * BREATH_HALF_MS - 2u * x);
    uint64_t denominator = (uint64_t)BREATH_HALF_MS * BREATH_HALF_MS * BREATH_HALF_MS;
    uint32_t level_q8 = CHANNEL_MIN * CHANNEL_SCALE +
        (uint32_t)((CHANNEL_MAX-CHANNEL_MIN) * CHANNEL_SCALE * eased / denominator);
    uint32_t green_q8 = (uint32_t)((uint64_t)level_q8 * kl_update_indicator_progress(state) /
                                  KL_UPDATE_PROGRESS_VERIFIED);
    color->g = (float)green_q8 / CHANNEL_SCALE;
    color->b = (float)(level_q8 - green_q8) / CHANNEL_SCALE;
    color->master = KL_UPDATE_INDICATOR_MASTER;
    return true;
}

bool kl_update_indicator_failure_sample(uint64_t elapsed_ms, kl_update_indicator_color *color) {
    if (!color) return false;
    memset(color, 0, sizeof(*color));
    if (elapsed_ms >= 1600) return false;
    uint32_t position = (uint32_t)(elapsed_ms % 800);
    uint32_t x = position <= 400 ? position : 800 - position;
    uint64_t eased = (uint64_t)x * x * (1200 - 2 * x);
    color->r = (float)(CHANNEL_MIN * CHANNEL_SCALE +
        (uint32_t)((CHANNEL_MAX - CHANNEL_MIN) * CHANNEL_SCALE * eased / UINT64_C(64000000))) / CHANNEL_SCALE;
    color->master = KL_UPDATE_INDICATOR_MASTER;
    return true;
}
