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

#endif
