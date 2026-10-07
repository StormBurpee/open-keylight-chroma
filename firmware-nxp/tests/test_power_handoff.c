#define main baseline_pwm_tests
#include "test_pwm_production.c"
#undef main

static unsigned enforce_envelope, observed_writes, maximum_white;
static unsigned commanded(model *m, uint32_t address, unsigned full) {
    uint32_t match = get(m, address);
    return match >= full ? 0 : full - match;
}
static void envelope_write(void *user, uint32_t address, uint32_t value) {
    model *m = user;
    write_register(user, address, value);
    if (!enforce_envelope) return;
    ++observed_writes;
    unsigned white = commanded(m, WHITE + 0x18, 255) + commanded(m, WHITE + 0x1c, 255);
    unsigned color = commanded(m, COLOR + 0x18, 25500) |
        commanded(m, COLOR + 0x1c, 25500) | commanded(m, COLOR + 0x24, 25500);
    CHECK(white <= maximum_white);
    CHECK(!color || (commanded(m, WHITE + 0x18, 255) <= 38 && commanded(m, WHITE + 0x1c, 255) <= 38));
}
static void apply_state(nxp_board *b, unsigned effect, unsigned white,
                        unsigned kelvin, unsigned rgb, unsigned brightness) {
    nxp_state s; nxp_pwm_frame f;
    nxp_state_init(&s); s.effect = (uint8_t)effect;
    s.white_brightness = (uint8_t)white; s.temperature_k = (uint16_t)kelvin;
    s.rgb[0] = (uint8_t)rgb; s.rgb[1] = (uint8_t)(rgb >> 8); s.rgb[2] = (uint8_t)(rgb >> 16);
    s.rgb_brightness = (uint8_t)brightness;
    CHECK(nxp_state_valid(&s)); nxp_render(&s, 1, &f);
    model *m = b->io.user;
    unsigned previous_white = commanded(m, WHITE + 0x18, 255) + commanded(m, WHITE + 0x1c, 255);
    unsigned next_white = 510u - f.cool_match - f.warm_match;
    maximum_white = previous_white > next_white ? previous_white : next_white;
    CHECK(nxp_board_apply_pwm(b, &f));
}
int main(void) {
    model m; nxp_board b; unsigned i; uint32_t random = 0xabc123;
    init(&m, &b); b.io.write = envelope_write;
    CHECK(nxp_board_start_pwm(&b)); m.require_dark = 0; enforce_envelope = 1;
    /* Old fixed RGB/cool/warm write order fails the second endpoint here. */
    apply_state(&b, 0, 255, 3000, 0, 0);
    apply_state(&b, 0, 255, 7000, 0, 0);
    apply_state(&b, 0, 255, 5200, 0, 0);
    apply_state(&b, 1, 38, 5200, 0xffffff, 255);
    apply_state(&b, 0, 255, 3000, 0, 0);
    for (i = 0; i < 20000; ++i) {
        unsigned color, white, kelvin;
        random = random * 1664525u + 1013904223u;
        color = random & 1u; white = (random >> 1) % (color ? 39u : 256u);
        kelvin = 3000u + (random >> 9) % 4001u;
        apply_state(&b, color, white, kelvin, random, random >> 24);
    }
    CHECK(observed_writes == 5u * 20005u);
    /* A dropped reduction cannot be followed by an increasing RGB write. */
    apply_state(&b, 0, 255, 7000, 0, 0);
    nxp_pwm_frame target = {25400, 25500, 25500, 217, 255};
    m.drop_enabled = 1; m.drop_address = WHITE + 0x18; m.drop_value = 217;
    CHECK(!nxp_board_apply_pwm(&b, &target));
    CHECK(b.pwm_fault && !b.pwm_started);
    CHECK(get(&m, COLOR + 0x24) == 25500);
    init(&m, &b); b.io.write = envelope_write; enforce_envelope = 0;
    CHECK(nxp_board_start_pwm(&b)); unsigned before = m.writes;
    target.cool_match = 216;
    CHECK(!nxp_board_apply_pwm(&b, &target) && m.writes == before);
    printf("%u every-write power-envelope checks passed across 20005 frame handoffs plus dropped-reduction and invalid mixed-frame cases.\n", checks);
    return 0;
}
