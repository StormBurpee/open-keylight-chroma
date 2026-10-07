#include "update_indicator_output.h"
#include <string.h>

enum { FRAME_MS = 20, GUARD_MS = 250, PURPLE_MS = 300, RESTORE_BEFORE_MS = 1400,
       FAILURE_MS = 1600, HANDOFF_MS = 1200 };

static uint64_t clock_us(const okl_nxp *d) { return d->transport.now_us(d->transport.user); }
static uint64_t clock_ms(const okl_nxp *d) { return clock_us(d) / 1000; }
static uint64_t plus(uint64_t a, uint64_t b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; }
static uint64_t bounded(kl_update_output *o, const okl_nxp *d, uint64_t amount) {
    uint64_t end = plus(clock_us(d), amount);
    return end < o->deadline_us ? end : o->deadline_us;
}
static okl_result allowed(kl_update_output *o, const okl_nxp *d) {
    if (!o->guard(o->user, o->revision)) return OKL_BUSY;
    if (d->needs_recovery) return OKL_NEEDS_RECOVERY;
    return clock_us(d) < o->deadline_us ? OKL_OK : OKL_TIMEOUT;
}
static okl_result send(kl_update_output *o, okl_nxp *d, const okl_request *request) {
    okl_result r = allowed(o, d); okl_reply reply;
    return r == OKL_OK ? okl_nxp_execute(d, request, &reply, bounded(o, d, OKL_DEFAULT_TIMEOUT_US)) : r;
}
static okl_result level(kl_update_output *o, okl_nxp *d, uint8_t value) {
    okl_request q; okl_result r = okl_request_color_brightness(&q, value);
    return r == OKL_OK ? send(o, d, &q) : r;
}
static okl_result white(kl_update_output *o, okl_nxp *d, uint8_t value, uint8_t effect) {
    okl_request q; okl_result r = okl_request_white_brightness(&q, value, effect);
    return r == OKL_OK ? send(o, d, &q) : r;
}
static okl_result effect(kl_update_output *o, okl_nxp *d, const okl_light_state *s) {
    uint8_t a[12] = {0, 0, s->effect, s->flags, s->speed, s->color_count};
    memcpy(a + 6, s->colors, 6);
    okl_request q; okl_result r = okl_request_build(&q, OKL_SET_EFFECT, a, sizeof(a));
    return r == OKL_OK ? send(o, d, &q) : r;
}
static okl_result rgb(kl_update_output *o, okl_nxp *d, const uint8_t value[3]) {
    okl_request q; okl_result r = okl_request_frame(&q, value);
    return r == OKL_OK ? send(o, d, &q) : r;
}
static okl_result read_state(kl_update_output *o, okl_nxp *d, okl_light_state *state) {
    okl_result r = allowed(o, d);
    return r == OKL_OK ? okl_nxp_read_state(d, state, bounded(o, d, 800000)) : r;
}
static okl_result release(kl_update_output *o, okl_nxp *d) {
    /* Guarded driver release cannot release a different identity. Even after
     * a newer Off, cleanup may relinquish our lease without changing output. */
    if (o->release_attempted) return OKL_BUSY;
    o->release_attempted = true;
    if (d->needs_recovery) return OKL_NEEDS_RECOVERY;
    if (clock_us(d) >= o->deadline_us) return OKL_TIMEOUT;
    return okl_nxp_release(d, bounded(o, d, 600000));
}
static okl_result coherent(kl_update_output *o, okl_nxp *d) {
    okl_owner owner; okl_light_state s;
    okl_result r = allowed(o, d);
    if (r == OKL_OK) r = okl_nxp_get_owner(d, &owner, bounded(o, d, OKL_DEFAULT_TIMEOUT_US));
    if (r != OKL_OK) return r;
    if (!owner.claimed || memcmp(owner.identity, d->identity, 6) || owner.name_size != 13 ||
        memcmp(owner.name, "Open Keylight", 13)) return OKL_NOT_OWNER;
    r = read_state(o, d, &s);
    if (r == OKL_OK && (s.effect != 8 || s.white_brightness ||
        s.color_brightness != KL_UPDATE_INDICATOR_MASTER)) r = OKL_VERIFY;
    return r;
}
static bool same(const okl_light_state *a, const okl_light_state *b) {
    return a->effect == b->effect && a->flags == b->flags && a->speed == b->speed &&
        a->color_count == b->color_count && !memcmp(a->colors, b->colors, 6) &&
        a->color_brightness == b->color_brightness && a->white_brightness == b->white_brightness &&
        a->temperature_kelvin == b->temperature_kelvin;
}
static uint8_t byte(float n) { return (uint8_t)(n < 0 ? 0 : n > 255 ? 255 : n + 0.5f); }
static okl_result sample(kl_update_output *o, okl_nxp *d, const kl_update_indicator *e) {
    kl_update_indicator_color c;
    if (e->phase == KL_UPDATE_FAILED) {
        kl_update_indicator_failure_sample(clock_ms(d) - o->failure_ms, &c);
    } else if (!kl_update_indicator_sample(e, clock_ms(d), &c)) return OKL_INVALID;
    uint8_t values[3] = {byte(c.r), byte(c.g), byte(c.b)};
    return rgb(o, d, values);
}
static kl_update_output_result terminate(kl_update_output *o, okl_nxp *d, okl_result error) {
    o->active = false; o->finished = true; o->resume_custom = false; o->error = error;
    /* A name/identity mismatch is positive evidence this lease is no longer
     * ours. The generic driver release checks identity only, so do not invoke
     * it on this path (including a same-MAC, different-name owner). */
    if (!o->release_attempted && error != OKL_NOT_OWNER && error != OKL_OWNER_DENIED) (void)release(o, d);
    return error == OKL_BUSY ? KL_INDICATOR_CANCELLED : KL_INDICATOR_ERROR;
}
static kl_update_output_result start(kl_update_output *o, okl_nxp *d,
    const kl_update_indicator *e, uint32_t revision, const uint8_t known_rgb[3],
    kl_update_output_guard guard, void *user) {
    *o = (kl_update_output){.generation = e->generation, .revision = revision,
        .guard = guard, .user = user, .deadline_us = plus(clock_us(d), HANDOFF_MS * 1000u)};
    /* An upload which completed before this worker observed it cannot safely
     * spend its reboot grace on setup followed by restoration. Leave it alone. */
    if (e->phase != KL_UPDATE_RECEIVING && e->phase != KL_UPDATE_FAILED) {
        o->finished = true; return KL_INDICATOR_NONE;
    }
    okl_result r = allowed(o, d);
    if (r != OKL_OK) { o->finished = true; return KL_INDICATOR_NONE; }
    r = okl_nxp_claim(d, (const uint8_t *)"Open Keylight", 13, bounded(o, d, 600000));
    if (r == OKL_OK) r = read_state(o, d, &o->saved);
    if (r != OKL_OK) return terminate(o, d, r);
    /* Validate the saved state's restoration requests before changing output.
     * A readable mixed state can still exceed this driver's supported white
     * limit while a colour effect is active. Leave such a state untouched. */
    uint8_t saved_args[12] = {0, 0, o->saved.effect, o->saved.flags, o->saved.speed, o->saved.color_count};
    memcpy(saved_args + 6, o->saved.colors, 6);
    okl_request q;
    if (okl_request_build(&q, OKL_SET_EFFECT, saved_args, sizeof(saved_args)) != OKL_OK ||
        okl_request_white_brightness(&q, o->saved.white_brightness, o->saved.effect) != OKL_OK) {
        o->finished = true; r = release(o, d);
        return r == OKL_OK ? KL_INDICATOR_NONE : terminate(o, d, r);
    }
    if (o->saved.effect == 8) {
        if (!known_rgb || o->saved.white_brightness || o->saved.color_brightness != 255) {
            o->finished = true;
            r = release(o, d);
            return r == OKL_OK ? KL_INDICATOR_NONE : terminate(o, d, r);
        }
        memcpy(o->saved_rgb, known_rgb, 3); o->resume_custom = true;
    }
    /* Muting precedes custom mode and every full-range frame. Master12 is
     * enabled only after the first frame has a positive transport ACK. */
    if ((r = level(o, d, 0)) == OKL_OK) r = white(o, d, 0, o->saved.effect);
    okl_request_custom(&q);
    if (r == OKL_OK) r = send(o, d, &q);
    if (e->phase == KL_UPDATE_FAILED) {
        o->failure_seen = true; o->failure_ms = clock_ms(d);
    }
    kl_update_indicator first = *e; first.received_bytes = 0;
    if (r == OKL_OK) r = sample(o, d, &first);
    if (r == OKL_OK) r = level(o, d, KL_UPDATE_INDICATOR_MASTER);
    if (r != OKL_OK) return terminate(o, d, r);
    o->active = true; o->next_frame_ms = clock_ms(d) + FRAME_MS;
    o->next_guard_ms = clock_ms(d) + GUARD_MS;
    return KL_INDICATOR_ACTIVE;
}
static kl_update_output_result restore(kl_update_output *o, okl_nxp *d, bool verified) {
    okl_result r = coherent(o, d);
    if (r != OKL_OK) return terminate(o, d, r);
    okl_light_state target = o->saved;
    bool resume = o->resume_custom && !verified;
    if (o->resume_custom && verified) {
        target.effect = 1; target.flags = target.speed = 0; target.color_count = 1;
        memset(target.colors, 0, 6); memcpy(target.colors, o->saved_rgb, 3);
    }
    if ((r = level(o, d, 0)) == OKL_OK) r = effect(o, d, &target);
    if (r == OKL_OK && resume) r = rgb(o, d, o->saved_rgb);
    /* Temperature was never changed by the indicator. Exact final getters
     * still detect an external change instead of concealing it. */
    if (r == OKL_OK) r = level(o, d, target.color_brightness);
    if (r == OKL_OK) r = white(o, d, target.white_brightness, target.effect);
    if (r == OKL_OK) r = read_state(o, d, &o->restored);
    if (r == OKL_OK) r = allowed(o, d);
    if (r == OKL_OK && !same(&target, &o->restored)) r = OKL_VERIFY;
    if (r == OKL_OK && !resume) r = release(o, d);
    if (r != OKL_OK) return terminate(o, d, r);
    o->resume_custom = resume; o->active = false; o->finished = true;
    return KL_INDICATOR_RESTORED;
}

kl_update_output_result kl_update_output_step(kl_update_output *o, okl_nxp *d,
    const kl_update_indicator *e, uint32_t revision, const uint8_t known_rgb[3],
    kl_update_output_guard guard, void *user) {
    if (!o || !d || !d->transport.now_us || !e || !guard || !e->generation || e->phase == KL_UPDATE_IDLE)
        return KL_INDICATOR_NONE;
    if (o->generation != e->generation) {
        if (!o->active) return start(o, d, e, revision, known_rgb, guard, user);
        /* A new authorized upload during the red failure pulse keeps the
         * original saved output, rather than treating red as the prior scene. */
        o->generation = e->generation; o->failure_seen = false; o->next_frame_ms = 0;
    }
    if (!o->active) return KL_INDICATOR_NONE;
    uint64_t now = clock_ms(d);
    o->deadline_us = plus(clock_us(d), HANDOFF_MS * 1000u);
    if (e->phase == KL_UPDATE_VERIFIED) {
        uint64_t end_ms = plus(e->verified_ms, RESTORE_BEFORE_MS);
        o->deadline_us = end_ms > UINT64_MAX / 1000 ? UINT64_MAX : end_ms * 1000;
    }
    okl_result r = allowed(o, d);
    if (r != OKL_OK) return terminate(o, d, r);
    if (e->phase == KL_UPDATE_FAILED && !o->failure_seen) {
        o->failure_seen = true; o->failure_ms = now; o->next_frame_ms = 0;
    }
    bool verified = e->phase == KL_UPDATE_VERIFIED;
    if ((verified && now >= plus(e->verified_ms, PURPLE_MS)) ||
        (o->failure_seen && now >= plus(o->failure_ms, FAILURE_MS))) return restore(o, d, verified);
    if (now >= o->next_guard_ms) {
        r = coherent(o, d);
        if (r != OKL_OK) return terminate(o, d, r);
        o->next_guard_ms = clock_ms(d) + GUARD_MS;
    }
    if (clock_ms(d) >= o->next_frame_ms) {
        r = sample(o, d, e);
        if (r != OKL_OK) return terminate(o, d, r);
        o->next_frame_ms = clock_ms(d) + FRAME_MS; /* Never replay a backlog. */
    }
    return KL_INDICATOR_ACTIVE;
}
