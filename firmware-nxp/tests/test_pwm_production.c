#include "nxp_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define GPIO UINT32_C(0x50000000)
#define IOCON UINT32_C(0x40044000)
#define WHITE UINT32_C(0x40014000)
#define COLOR UINT32_C(0x40018000)
static const unsigned pins[5] = {13, 14, 16, 18, 19};
static const unsigned functions[5] = {3, 3, 2, 2, 2};
static const unsigned gpio_functions[5] = {1, 1, 0, 0, 0};
typedef struct {
    uint32_t address[96], value[96], latch, timer_latch[5], drop_address, drop_value;
    unsigned count, writes, drop_enabled, drop_any, require_dark, timer_writes, pin_writes;
} model;
static uint32_t get(model *m, uint32_t a) {
    unsigned i; for (i = 0; i < m->count; ++i) if (m->address[i] == a) return m->value[i]; return 0;
}
static void set(model *m, uint32_t a, uint32_t v) {
    unsigned i; for (i = 0; i < m->count && m->address[i] != a; ++i) {}
    CHECK(i < 96); if (i == m->count) { m->address[i] = a; ++m->count; } m->value[i] = v;
}
static unsigned pad(model *m, unsigned i) {
    uint32_t mux = get(m, IOCON + pins[i] * 4u), bit = 1u << pins[i];
    if ((mux & 7u) == gpio_functions[i]) return (get(m, GPIO + 0x2000) & m->latch & bit) != 0;
    if ((mux & 7u) == functions[i]) return m->timer_latch[i];
    return 0;
}
static uint32_t read_register(void *user, uint32_t a) {
    model *m = user; unsigned i; uint32_t v = 0;
    if (a == GPIO + 0x2100) { for (i = 0; i < 5; ++i) v |= pad(m, i) << pins[i]; return v; }
    /* This basic register model advances counters for the bounded boundary
     * wait. The separate temporal suite models actual per-cycle PWM latches. */
    if (a == WHITE + 8 || a == COLOR + 8) {
        uint32_t period = a == WHITE + 8 ? 255u : 25500u;
        v = (get(m, a) + (a == WHITE + 8 ? 3u : 256u)) % period;
        set(m, a, v); return v;
    }
    return get(m, a);
}
static void write_register(void *user, uint32_t a, uint32_t v) {
    model *m = user; unsigned i;
    ++m->writes;
    if ((a >= WHITE && a < WHITE + 0x80) || (a >= COLOR && a < COLOR + 0x80)) ++m->timer_writes;
    if (a >= IOCON && a < IOCON + 0x80) ++m->pin_writes;
    if (!(m->drop_enabled && a == m->drop_address && (m->drop_any || v == m->drop_value))) {
        set(m, a, v);
        if (a == GPIO + 0x2280) m->latch &= ~v;
        if ((a == WHITE + 4 || a == COLOR + 4) && (v & 2u))
            for (i = (a == COLOR + 4 ? 0u : 3u); i < (a == COLOR + 4 ? 3u : 5u); ++i) m->timer_latch[i] = 0;
    }
    if (m->require_dark) for (i = 0; i < 5; ++i) CHECK(!pad(m, i));
}
static void init(model *m, nxp_board *b) {
    nxp_board_config config = {0xbc40, 0xbc40, 48000000, NXP_QUAL_PWM_REQUIRED, 3};
    nxp_register_io io = {m, read_register, write_register}; unsigned i;
    memset(m, 0, sizeof(*m));
    /* Timers inherited with stale high output latches and arbitrary modes,
     * while bootloader GPIO currently keeps every external pad low. */
    for (i = 0; i < 5; ++i) { set(m, IOCON + pins[i] * 4u, 0x90u | gpio_functions[i]); m->timer_latch[i] = 1; }
    set(m, WHITE + 0x28, 0x3f); set(m, COLOR + 0x28, 0x3f);
    set(m, WHITE + 0x70, 3); set(m, COLOR + 0x70, 3);
    set(m, WHITE + 0x3c, 0xfff); set(m, COLOR + 0x3c, 0xfff);
    set(m, WHITE + 0x14, 0xfff); set(m, COLOR + 0x14, 0xfff);
    m->require_dark = 1; nxp_board_init(b, &io, &config, NULL);
}
static void startup_and_mapping(void) {
    model m; nxp_board b; unsigned i;
    nxp_pwm_frame f = {24444, 23333, 22222, 225, 230};
    init(&m, &b); CHECK(nxp_board_start_pwm(&b));
    CHECK(b.pwm_started && !b.pwm_fault);
    CHECK(get(&m, WHITE + 0x28) == 0 && get(&m, COLOR + 0x28) == 0);
    CHECK(get(&m, WHITE + 0x70) == 0 && get(&m, COLOR + 0x70) == 0);
    CHECK(get(&m, WHITE) == 0x5f && get(&m, COLOR) == 0x3f);
    CHECK(get(&m, WHITE + 4) == 1 && get(&m, COLOR + 4) == 1);
    CHECK(nxp_board_apply_pwm(&b, &f));
    CHECK(get(&m, COLOR + 0x24) == 24444 && get(&m, COLOR + 0x1c) == 23333 && get(&m, COLOR + 0x18) == 22222);
    CHECK(get(&m, WHITE + 0x18) == 225 && get(&m, WHITE + 0x1c) == 230);
    m.require_dark = 0;
    for (i = 0; i < 5; ++i) m.timer_latch[i] = 1;
    CHECK(nxp_board_force_off(&b) && !b.pwm_started && !b.pwm_fault);
    for (i = 0; i < 5; ++i) CHECK(!pad(&m, i));
    CHECK(get(&m, WHITE + 4) == 2 && get(&m, COLOR + 4) == 2);
}
static void corrupted_configuration(void) {
    static const uint32_t addresses[] = {
        WHITE + 0x28, COLOR + 0x28, WHITE + 0x70, COLOR + 0x70,
        WHITE + 0x14, COLOR + 0x14, WHITE + 0x3c, COLOR + 0x3c,
        WHITE + 0x0c, COLOR + 0x0c, WHITE + 0x20, COLOR + 0x20,
        WHITE + 0x74, COLOR + 0x74, WHITE + 4, COLOR + 4,
        IOCON + 13 * 4, IOCON + 14 * 4, IOCON + 16 * 4, IOCON + 18 * 4, IOCON + 19 * 4
    };
    unsigned i;
    for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        model m; nxp_board b; unsigned before;
        nxp_pwm_frame f = {25400, 25400, 25400, 254, 254};
        init(&m, &b); CHECK(nxp_board_start_pwm(&b));
        set(&m, addresses[i], get(&m, addresses[i]) ^
            ((addresses[i] == WHITE + 0x3c || addresses[i] == COLOR + 0x3c) ? 16u : 1u));
        CHECK(!nxp_board_apply_pwm(&b, &f)); CHECK(b.pwm_fault && !b.pwm_started);
        before = m.writes; CHECK(!nxp_board_start_pwm(&b)); CHECK(!nxp_board_apply_pwm(&b, &f)); CHECK(m.writes == before);
    }
}
static void dropped_setup_and_frame(void) {
    static const uint32_t setup[] = {WHITE + 0x28, COLOR + 0x28, WHITE + 0x70, COLOR + 0x70,
        WHITE + 0x3c, COLOR + 0x3c, WHITE + 0x18, COLOR + 0x24, WHITE + 0x0c,
        WHITE + 0x20, COLOR + 0x20, WHITE + 0x74, COLOR + 0x74};
    static const uint32_t matches[] = {COLOR + 0x24, COLOR + 0x1c, COLOR + 0x18, WHITE + 0x18, WHITE + 0x1c};
    unsigned i;
    for (i = 0; i < sizeof(setup) / sizeof(setup[0]); ++i) {
        model m; nxp_board b;
        init(&m, &b); m.drop_address = setup[i]; m.drop_enabled = m.drop_any = 1;
        CHECK(!nxp_board_start_pwm(&b) && b.pwm_fault && !b.pwm_started);
    }
    for (i = 0; i < 5; ++i) {
        model m; nxp_board b; nxp_pwm_frame f = {24444, 23333, 22222, 225, 230};
        init(&m, &b); CHECK(nxp_board_start_pwm(&b));
        m.drop_address = matches[i]; m.drop_enabled = m.drop_any = 1;
        CHECK(!nxp_board_apply_pwm(&b, &f) && b.pwm_fault && !b.pwm_started);
    }
}
static void failed_gpio_handoff_never_stops_high_timer(void) {
    unsigned i;
    for (i = 0; i < 5; ++i) {
        model m; nxp_board b; unsigned before;
        init(&m, &b); CHECK(nxp_board_start_pwm(&b));
        m.require_dark = 0; m.timer_latch[i] = 1;
        m.drop_address = IOCON + pins[i] * 4u; m.drop_enabled = m.drop_any = 1;
        before = m.timer_writes;
        CHECK(!nxp_board_force_off(&b) && b.pwm_fault && b.pwm_started);
        CHECK(m.timer_writes == before + 5 && pad(&m, i));
        CHECK(get(&m, COLOR + 0x24) == 25500 && get(&m, COLOR + 0x1c) == 25500 && get(&m, COLOR + 0x18) == 25500);
        CHECK(get(&m, WHITE + 0x18) == 255 && get(&m, WHITE + 0x1c) == 255);
        CHECK(get(&m, WHITE + 4) == 1 && get(&m, COLOR + 4) == 1);
    }
}
int main(void) {
    startup_and_mapping(); corrupted_configuration(); dropped_setup_and_frame();
    failed_gpio_handoff_never_stops_high_timer();
    printf("%u PWM register/pad-model checks passed; no hardware I/O.\n", checks); return 0;
}
