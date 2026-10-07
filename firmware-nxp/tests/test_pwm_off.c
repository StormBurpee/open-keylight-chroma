/* Reuse the independently observable register/pad model, not trial logic. */
#define main pwm_model_baseline
#include "test_pwm.c"
#undef main
#include "nxp_pwm_off_trial.h"

static uint32_t record_word(const nxp_pwm_off_trial *t, unsigned i) {
    const volatile uint8_t *p = t->record + i * 4u;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void off_init(model *m, nxp_board *b, nxp_state *s, nxp_link *l, nxp_pwm_off_trial *t) {
    unsigned i;
    init(m, b); b->config.qualifications = 0x193;
    nxp_state_init(s); s->part_id = 0xbc40;
    CHECK(nxp_state_platform(s, 1, 1, 0, 0x13));
    s->claimed = 1; for (i = 0; i < 6; ++i) s->owner[i] = (uint8_t)(i * 2 + 2);
    nxp_link_init(l, s); b->link = l;
    CHECK(nxp_pwm_off_init(t, b));
    CHECK(record_word(t, 0) == 0x4f464631 && record_word(t, 1) == 1);
    CHECK(record_word(t, 14) == 30000 && record_word(t, 15) == 1);
}
static void request(uint8_t out[97], nxp_state *s, uint8_t op, const uint8_t *args, uint8_t n) {
    unsigned i; memset(out, 0, 97); memcpy(out, s->owner, 6); out[12] = n; out[14] = op;
    if (n) memcpy(out + 15, args, n);
    for (i = 9; i < 95; ++i) out[95] ^= out[i];
}
static void request_off(nxp_state *s, nxp_link *l, uint32_t now, int body) {
    uint8_t q[97], out[97], zeros[97] = {0};
    request(q, s, 0x70, (const uint8_t *)"OFF1", 4);
    CHECK(nxp_link_transaction(l, q, 97, out, 97, now) == NXP_OK && s->off_requested == 1);
    CHECK(nxp_link_transaction(l, zeros, 2, out, 97, now + 1) == NXP_OK && out[1] == 97);
    if (body) CHECK(nxp_link_transaction(l, zeros, 97, out, 97, now + 2) == NXP_OK &&
        out[7] == 2 && out[12] == 4 && !memcmp(out + 15, "OFF1", 4) && s->off_requested == 2);
}
static void fixed_pages_and_deadline(void) {
    model m; nxp_board b; nxp_state s; nxp_link l; nxp_pwm_off_trial t;
    uint8_t q[97], out[97], page; unsigned before, i;
    off_init(&m, &b, &s, &l, &t);
    CHECK(!nxp_board_start_pwm(&b)); /* No production permission fabricated. */
    request_off(&s, &l, 100, 1);
    nxp_pwm_off_service(&t, &b, 103); CHECK(t.active && s.off_requested == 3);
    CHECK(record_word(&t, 2) == 1 && record_word(&t, 10) == 7 && record_word(&t, 6) == 503);
    nxp_pwm_off_tick(&t, &b, 502); CHECK(t.active && b.pwm_started);
    /* No main-loop/service call is needed for the 400ms handoff. */
    nxp_pwm_off_tick(&t, &b, 503); CHECK(!t.active && !b.pwm_started);
    CHECK(record_word(&t, 2) == 2 && record_word(&t, 8) == 1 && record_word(&t, 10) == 31);
    CHECK(record_word(&t, 5) - record_word(&t, 4) == 400);
    CHECK(record_word(&t, 11) == 2 && record_word(&t, 12) == 2 && record_word(&t, 13) == 0x193);
    before = m.writes;
    for (i = 504; i < 30001; i += 37) { nxp_pwm_off_tick(&t, &b, i); nxp_pwm_off_service(&t, &b, i); }
    CHECK(m.writes == before);
    for (page = 0; page < 14; ++page) {
        request(q, &s, 0xf1, &page, 1);
        CHECK(nxp_process_at(&s, q, 97, out, 97, 1000) == NXP_OK);
        CHECK(out[7] == 2 && out[12] == 72 && !memcmp(out + 15, "OFF1", 4));
        CHECK(out[19] == page && out[20] == 14 && out[21] == 64 && !out[22]);
        for (i = 0; i < 64; ++i) CHECK(out[23 + i] == t.record[page * 64u + i]);
    }
    for (i = 216; i < 224; ++i) CHECK(!record_word(&t, i));
    CHECK(nxp_trial_expired(&s, 30000));
}
static void cancellation_and_no_rearm(void) {
    unsigned kind;
    for (kind = 0; kind < 6; ++kind) {
        model m; nxp_board b; nxp_state s; nxp_link l; nxp_pwm_off_trial t;
        unsigned before;
        off_init(&m, &b, &s, &l, &t); request_off(&s, &l, 10, kind != 0);
        if (!kind) { CHECK(nxp_link_expire(&l, 110) == NXP_EXPIRED && s.off_requested == 3); }
        else if (kind == 1) s.claimed = 0;
        else if (kind == 2) s.boot_requested = 1;
        nxp_pwm_off_service(&t, &b, kind == 3 ? 28500 : 13);
        if (kind <= 3) CHECK(!t.active && !b.pwm_started);
        else {
            CHECK(t.active);
            if (kind == 4) s.owner[0] ^= 2; else ++b.errors;
            nxp_pwm_off_tick(&t, &b, 14); CHECK(!t.active && !b.pwm_started);
            CHECK(record_word(&t, 2) == 3 && record_word(&t, 8) == (kind == 4 ? 2u : 3u));
        }
        before = m.writes; nxp_pwm_off_service(&t, &b, 500); nxp_pwm_off_tick(&t, &b, 500);
        CHECK(m.writes == before);
    }
}
static void register_failures(void) {
    unsigned phase;
    for (phase = 0; phase < 3; ++phase) {
        model m; nxp_board b; nxp_state s; nxp_link l; nxp_pwm_off_trial t;
        off_init(&m, &b, &s, &l, &t); request_off(&s, &l, 10, 1);
        if (!phase) { m.drop_enabled = m.drop_any = 1; m.drop_address = WHITE + 0x28; }
        if (phase == 1) { m.drop_enabled = 1; m.drop_address = IOCON + 13 * 4; m.drop_value = 0x93; }
        nxp_pwm_off_service(&t, &b, 13);
        if (phase == 2) {
            unsigned before;
            CHECK(t.active); m.drop_enabled = m.drop_any = 1; m.drop_address = IOCON + 13 * 4;
            before = m.timer_writes; nxp_pwm_off_tick(&t, &b, 413);
            CHECK(m.timer_writes == before); /* GPIO failure must not stop PWM. */
        }
        CHECK(!t.active && record_word(&t, 2) == 3 && record_word(&t, 8) == 4);
    }
}
int main(void) {
    fixed_pages_and_deadline(); cancellation_and_no_rearm(); register_failures();
    printf("%u off-only trial lifecycle/register/snapshot checks passed; no hardware I/O.\n", checks);
    return 0;
}
