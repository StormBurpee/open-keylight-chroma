#ifndef KEYLIGHT_UPDATE_INDICATOR_H
#define KEYLIGHT_UPDATE_INDICATOR_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    KL_UPDATE_IDLE, KL_UPDATE_RECEIVING, KL_UPDATE_VERIFIED, KL_UPDATE_FAILED
} kl_update_indicator_phase;

/* Volatile upload evidence, not a controller readback or an instruction to
 * restore a scene. Only the lighting worker may turn this into SPI requests. */
typedef struct {
    kl_update_indicator_phase phase;
    uint32_t generation, total_bytes, received_bytes;
    uint64_t started_ms, verified_ms;
} kl_update_indicator;

enum { KL_UPDATE_INDICATOR_MASTER = 12 }; /* Native 0..255 scale, about 4.7%. */
enum { KL_UPDATE_PROGRESS_RECEIVED = 990, KL_UPDATE_PROGRESS_VERIFIED = 1000 };
/* Blue at zero, cyan in between, green at verified completion.
 * RGB before byte rounding, with a required low native master. Full-range
 * channel modulation avoids an unnecessarily coarse 12-step byte stream.
 * The worker must keep white zero, mute before setup, install the first custom
 * frame, then enable exactly this master; never expose these RGB values with
 * an inherited high master. Final restoration is the worker's responsibility. */
typedef struct { float r, g, b; uint8_t master; } kl_update_indicator_color;

bool kl_update_indicator_begin(kl_update_indicator *state, uint32_t bytes, uint64_t now_ms);
bool kl_update_indicator_advance(kl_update_indicator *state, uint32_t received);
bool kl_update_indicator_verify(kl_update_indicator *state, uint64_t now_ms);
void kl_update_indicator_fail(kl_update_indicator *state);
/* Receiving, including all bytes received, is capped at 990/1000. Only the
 * caller's completed SHA/image validation and boot-slot acceptance may verify. */
uint16_t kl_update_indicator_progress(const kl_update_indicator *state);
bool kl_update_indicator_sample(const kl_update_indicator *state, uint64_t now_ms,
                                kl_update_indicator_color *color);
/* Two smooth red pulses over 1600ms. The worker timestamps observed failure,
 * then restores only while its lease/revision and transport remain valid. */
bool kl_update_indicator_failure_sample(uint64_t elapsed_ms, kl_update_indicator_color *color);

/* Locks app.mutex internally; never call while holding it. Returns a copy only.
 * Terminal generations remain observable until the next accepted ESP upload.
 * NXP controller updates do not create or advance an ESP indicator. */
void app_update_indicator_snapshot(kl_update_indicator *out);
/* Lock-free fixed controller-I/O cutoff, in esp_timer microseconds. Zero until
 * an accepted ESP image has selected its boot slot; then immutable until reboot.
 * Safe inside the transport/flash guard and independent of a stale UI snapshot. */
uint64_t app_update_reboot_deadline_us(void);

#endif
