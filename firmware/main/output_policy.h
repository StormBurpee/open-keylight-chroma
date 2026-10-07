#ifndef KEYLIGHT_OUTPUT_POLICY_H
#define KEYLIGHT_OUTPUT_POLICY_H

#include "keylight_core.h"
#include "okl_nxp.h"

/* Pure policy plus an injected exchange boundary, shared by worker and host tests. */
typedef okl_result (*kl_output_exchange)(void *user, const okl_request *request);
uint8_t kl_native_level(uint8_t percent);
/* Animated setup hides stale controller RGB while installing the first scaled
 * frame. initial is required only for animation; master enable follows its ACK. */
okl_result kl_output_prepare(const kl_state *target, bool animated, const kl_frame *initial, uint8_t *native_effect,
                            kl_output_exchange exchange, void *user);
okl_result kl_output_park(const kl_state *target, uint8_t *native_effect,
                         kl_output_exchange exchange, void *user);
bool kl_native_matches(const kl_state *target, const okl_light_state *native);
uint32_t kl_native_report(const kl_state *preferences, const okl_light_state *native, kl_state *out);
bool kl_report_publishable(bool connected, bool valid, bool idle,
                           uint32_t output_revision, uint32_t reported_revision, uint32_t fields);

/* Colour renderer uses linear channel bytes with the native RGB master fixed
 * at 255. Brightness is included exactly once in the frame. At low brightness
 * this has the controller's 8-bit channel quantization; it is not optical proof. */
uint8_t kl_srgb_channel(uint8_t value);
/* Nearest display-code estimate for boot adoption only. Quantization makes
 * this non-invertible; it cannot recover a user's original chosen colour. */
uint8_t kl_srgb_reconstruct(uint8_t linear);
kl_frame kl_color_frame(const kl_state *state, kl_output_encoding encoding, uint64_t elapsed_ms);
kl_frame kl_color_sample(const kl_transition *transition, kl_output_encoding encoding, uint64_t now_ms);
/* Caller owns the controller and supplies a fresh native snapshot. known_rgb
 * may be supplied only for its own last ACKed custom frame. A known static
 * frame is seeded before switching to custom, so an established master-255
 * colour never blanks or exposes stale buffer contents. Importing another
 * master level is conservative but not atomic; its brief dimming is bounded.
 * Unknown custom/effect output is safely muted before replacing its buffer. */
okl_result kl_color_enter(const okl_light_state *native, const uint8_t known_rgb[3],
                          uint8_t initial_rgb[3], uint8_t *native_effect,
                          kl_output_exchange exchange, void *user);
/* Park exactly the last ACKed canonical frame; no master change and no RGB
 * delta in the controller's custom-to-static handoff. */
okl_result kl_color_park(const uint8_t rgb[3], uint8_t *native_effect,
                         kl_output_exchange exchange, void *user);
bool kl_color_matches(const kl_state *target, kl_output_encoding encoding, const okl_light_state *native);

#endif
