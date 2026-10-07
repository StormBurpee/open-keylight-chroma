#ifndef KEYLIGHT_UPDATE_INDICATOR_OUTPUT_H
#define KEYLIGHT_UPDATE_INDICATOR_OUTPUT_H

#include "okl_nxp.h"
#include "update_indicator.h"

typedef bool (*kl_update_output_guard)(void *user, uint32_t revision);
typedef enum {
    KL_INDICATOR_NONE, KL_INDICATOR_ACTIVE, KL_INDICATOR_RESTORED,
    KL_INDICATOR_CANCELLED, KL_INDICATOR_ERROR
} kl_update_output_result;

/* Volatile, single-worker state. No task, storage, recovery or command retry.
 * A guard is checked before every output request, so a newer Off interrupts a
 * multi-command handoff. Aggregate driver reads/lease operations have one
 * guard and their own bounded deadline. The caller owns renderer suspension. */
typedef struct {
    uint32_t generation, revision;
    bool active, finished, resume_custom;
    bool failure_seen, release_attempted;
    uint64_t failure_ms, next_frame_ms, next_guard_ms, deadline_us;
    okl_light_state saved, restored;
    uint8_t saved_rgb[3];
    okl_result error;
    kl_update_output_guard guard;
    void *user;
} kl_update_output;

/* known_rgb is the caller's last ACKed custom frame, or NULL. An unknown
 * external custom framebuffer cannot be restored, so it is left untouched.
 * Successful upload freezes a formerly animated frame as native Static before
 * reboot. Failed precommit upload can resume that same in-memory renderer.
 * Transport failure terminates this generation permanently; no implicit
 * reclaim, recovery, uncertain-write retry or persistent scene replay occurs. */
kl_update_output_result kl_update_output_step(kl_update_output *out, okl_nxp *driver,
    const kl_update_indicator *evidence, uint32_t revision, const uint8_t known_rgb[3],
    kl_update_output_guard guard, void *user);

#endif
