#include "nxp_pwm_off_trial.h"

#define SYSCON UINT32_C(0x40048000)
#define GPIO UINT32_C(0x50000000)
#define IOCON UINT32_C(0x40044000)
#define WHITE UINT32_C(0x40014000)
#define COLOR UINT32_C(0x40018000)
#define COUNTER_ADDRESS UINT32_C(0x20004000)
#define COUNTER_MAGIC UINT32_C(0x31464f4e)
#define PROFILE_FLAGS (NXP_QUAL_PART | NXP_QUAL_CLOCK | NXP_QUAL_RECOVERY | NXP_ALLOW_SPI_TRIAL | NXP_ALLOW_PWM_OFF_TRIAL)
enum { M_MAGIC, M_ABI, M_PHASE, M_REQUEST, M_MUX, M_DARK, M_DEADLINE, M_ACCEPTED,
       M_REASON, M_ERRORS, M_SNAPSHOTS, M_WHITE_TCR, M_COLOR_TCR, M_FLAGS, M_RECOVERY, M_GENERATION };
static uint32_t rd(nxp_board *b, uint32_t a) { return b->io.read(b->io.user, a); }
static void wr(nxp_board *b, uint32_t a, uint32_t v) { b->io.write(b->io.user, a, v); }
static void word(nxp_pwm_off_trial *t, unsigned i, uint32_t v) {
    volatile uint8_t *p = t->record + i * 4u;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t value(const nxp_pwm_off_trial *t, unsigned i) {
    const volatile uint8_t *p = t->record + i * 4u;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static int permitted(const nxp_board *b) {
    return b && b->io.read && b->io.write && b->link && b->link->state &&
        b->config.observed_part == 0xbc40u && b->config.approved_part == 0xbc40u &&
        b->config.clock_hz == 48000000u && b->config.qualifications == PROFILE_FLAGS;
}
static int owner_matches(const nxp_state *s) {
    unsigned i; if (!s->claimed) return 0;
    for (i = 0; i < 6; ++i) if (s->owner[i] != s->off_owner[i]) return 0;
    return 1;
}
static void snapshot(nxp_pwm_off_trial *t, nxp_board *b, unsigned index, uint32_t reason, uint32_t now) {
    static const uint8_t pins[5] = {13, 14, 16, 18, 19};
    static const uint8_t offsets[14] = {4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 60, 112, 116, 0};
    unsigned i, start = 16u + index * 40u;
    if (index >= 5) return;
    word(t, start, now); word(t, start + 1, reason); word(t, start + 2, rd(b, SYSCON + 0x80));
    word(t, start + 3, rd(b, GPIO + 0x2000)); word(t, start + 4, rd(b, GPIO + 0x2100));
    for (i = 0; i < 5; ++i) word(t, start + 5 + i, rd(b, IOCON + pins[i] * 4u));
    for (i = 0; i < 14; ++i) {
        word(t, start + 10 + i, rd(b, WHITE + offsets[i]));
        word(t, start + 24 + i, rd(b, COLOR + offsets[i]));
    }
    word(t, start + 38, value(t, M_PHASE)); word(t, start + 39, b->errors);
    word(t, M_SNAPSHOTS, value(t, M_SNAPSHOTS) | (1u << index));
}
static void finish(nxp_pwm_off_trial *t, nxp_board *b, uint32_t reason, uint32_t now) {
    int handed_off = nxp_board_trial_dark(b);
    if (!handed_off) reason = NXP_OFF_REGISTER;
    word(t, M_DARK, now); word(t, M_REASON, reason); word(t, M_ERRORS, b->errors);
    word(t, M_PHASE, reason == NXP_OFF_DEADLINE ? NXP_OFF_COMPLETED : NXP_OFF_FAILED);
    snapshot(t, b, 3, reason, now);
    /* Readback of GPIO direction, mux and low pads precedes timer reset. */
    if (handed_off) { wr(b, WHITE + 4, 2); wr(b, COLOR + 4, 2); }
    word(t, M_WHITE_TCR, rd(b, WHITE + 4)); word(t, M_COLOR_TCR, rd(b, COLOR + 4));
    if (!handed_off || value(t, M_WHITE_TCR) != 2 || value(t, M_COLOR_TCR) != 2) {
        word(t, M_PHASE, NXP_OFF_FAILED); word(t, M_REASON, NXP_OFF_REGISTER);
    }
    t->active = 0;
    snapshot(t, b, 4, value(t, M_REASON), now);
}
int nxp_pwm_off_init(nxp_pwm_off_trial *t, nxp_board *b) {
    unsigned i; uint32_t generation;
    if (!t || !permitted(b)) return 0;
    t->initialized = t->active = t->started_ms = t->initial_errors = 0;
    for (i = 0; i < sizeof(t->record); ++i) t->record[i] = 0;
    if (!nxp_state_off_trial(b->link->state, t->record)) return 0;
    wr(b, SYSCON + 0x80, rd(b, SYSCON + 0x80) | (1u << 27));
    generation = rd(b, COUNTER_ADDRESS) == COUNTER_MAGIC ? rd(b, COUNTER_ADDRESS + 4) + 1u : 1u;
    if (!generation) generation = 1;
    wr(b, COUNTER_ADDRESS, COUNTER_MAGIC); wr(b, COUNTER_ADDRESS + 4, generation);
    if (rd(b, COUNTER_ADDRESS) != COUNTER_MAGIC || rd(b, COUNTER_ADDRESS + 4) != generation) return 0;
    word(t, M_MAGIC, UINT32_C(0x4f464631)); word(t, M_ABI, 1); word(t, M_FLAGS, PROFILE_FLAGS);
    word(t, M_RECOVERY, 30000); word(t, M_GENERATION, generation);
    t->initialized = 1; return 1;
}
void nxp_pwm_off_tick(nxp_pwm_off_trial *t, nxp_board *b, uint32_t now) {
    if (!t || !t->initialized || !t->active) return;
    if (!owner_matches(b->link->state)) finish(t, b, NXP_OFF_OWNER, now);
    else if (b->fault || b->pwm_fault || b->errors != t->initial_errors) finish(t, b, NXP_OFF_SPI, now);
    else if ((uint32_t)(now - t->started_ms) >= NXP_OFF_WINDOW_MS) finish(t, b, NXP_OFF_DEADLINE, now);
}
void nxp_pwm_off_service(nxp_pwm_off_trial *t, nxp_board *b, uint32_t now) {
    nxp_state *s;
    if (!t || !t->initialized || !permitted(b)) return;
    if (t->active) { nxp_pwm_off_tick(t, b, now); return; }
    s = b->link->state;
    if (s->off_requested != 2 || b->link->phase != NXP_LINK_REQUEST) return;
    s->off_requested = 3;
    word(t, M_REQUEST, now); word(t, M_ACCEPTED, 1);
    if (!owner_matches(s) || s->boot_requested || now >= 28500u) {
        word(t, M_PHASE, NXP_OFF_FAILED); word(t, M_REASON, NXP_OFF_CANCELLED); return;
    }
    if (!nxp_board_trial_dark(b)) {
        word(t, M_PHASE, NXP_OFF_FAILED); word(t, M_REASON, NXP_OFF_REGISTER); return;
    }
    wr(b, SYSCON + 0x80, rd(b, SYSCON + 0x80) | (1u << 9) | (1u << 10));
    snapshot(t, b, 0, NXP_OFF_NONE, now);
    if (!nxp_board_prepare_pwm_off_trial(b)) { finish(t, b, NXP_OFF_REGISTER, now); return; }
    snapshot(t, b, 1, NXP_OFF_NONE, now);
    t->started_ms = now; t->initial_errors = b->errors; t->active = 1;
    word(t, M_PHASE, NXP_OFF_RUNNING); word(t, M_MUX, now); word(t, M_DEADLINE, now + NXP_OFF_WINDOW_MS);
    if (!nxp_board_connect_pwm_off_trial(b)) { finish(t, b, NXP_OFF_REGISTER, now); return; }
    snapshot(t, b, 2, NXP_OFF_NONE, now);
}
void nxp_pwm_off_recovery(nxp_pwm_off_trial *t, nxp_board *b, uint32_t now) {
    if (!t || !t->initialized) return;
    if (t->active) finish(t, b, NXP_OFF_RECOVERY, now);
    else if (permitted(b)) (void)nxp_board_trial_dark(b);
}
