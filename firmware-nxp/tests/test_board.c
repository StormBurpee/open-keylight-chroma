#include "nxp_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
typedef struct {
    uint32_t address[128], value[128], log_address[4096], log_value[4096];
    unsigned entries, writes, selected, tx_count, rx_count, rx_read;
    uint8_t tx[8], rx[8];
    uint32_t fault;
} bus;
static uint32_t stored(bus *b, uint32_t address) { unsigned i; for (i = 0; i < b->entries; ++i) if (b->address[i] == address) return b->value[i]; return 0; }
static void store(bus *b, uint32_t address, uint32_t value) {
    unsigned i; for (i = 0; i < b->entries; ++i) if (b->address[i] == address) break;
    CHECK(i < 128); if (i == b->entries) { b->address[i] = address; ++b->entries; } b->value[i] = value;
}
static uint32_t read_reg(void *user, uint32_t address) {
    bus *b = user;
    if (address == 0x50002100) return b->selected ? 0 : 1u << 17;
    /* BSY includes queued TX data, even after the master deselects the slave. */
    if (address == 0x4005800c) return (b->tx_count < 8 ? 2u : 0u) | (b->rx_count ? 4u : 0u) |
        ((b->selected || b->tx_count) ? 16u : 0u);
    if (address == 0x40058008) { uint8_t v; CHECK(b->rx_count); v = b->rx[b->rx_read++ & 7]; --b->rx_count; return v; }
    if (address == 0x40058018) return b->fault;
    return stored(b, address);
}
static void write_reg(void *user, uint32_t address, uint32_t value) {
    bus *b = user;
    CHECK(b->writes < 4096); b->log_address[b->writes] = address; b->log_value[b->writes++] = value;
    if (address == 0x40058008) { CHECK(b->tx_count < 8); b->tx[b->tx_count++] = (uint8_t)value; }
    if (address == 0x40048004 && !(value & 4)) b->tx_count = b->rx_count = b->rx_read = 0;
    if (address == 0x40058020) b->fault = 0;
    store(b, address, value);
}
static void initialize(bus *b, nxp_board *board, nxp_state *state, nxp_link *link, uint32_t flags) {
    nxp_register_io io; nxp_board_config config;
    memset(b, 0, sizeof(*b)); nxp_state_init(state); nxp_link_init(link, state);
    io.user = b; io.read = read_reg; io.write = write_reg;
    config.observed_part = config.approved_part = 0x1bc40; config.clock_hz = 48000000;
    config.qualifications = flags; config.spi_mode = 0;
    nxp_board_init(board, &io, &config, link);
    CHECK(!b->writes); /* Initialization itself has no hardware side effects. */
}
static void make_version(uint8_t *q) {
    memset(q, 0, 97); q[0] = 2; q[5] = 1; q[8] = 4; q[14] = q[95] = 0x87;
}
static void clocks(bus *b, nxp_board *board, const uint8_t *in, uint8_t *out, unsigned count, uint32_t now) {
    unsigned i, j; b->selected = 1;
    for (i = 0; i < count; ++i) {
        CHECK(b->tx_count); out[i] = b->tx[0];
        for (j = 1; j < b->tx_count; ++j) b->tx[j - 1] = b->tx[j];
        --b->tx_count; CHECK(b->rx_count < 8); b->rx[(b->rx_read + b->rx_count) & 7] = in[i]; ++b->rx_count;
        if ((i & 3) == 3 || i + 1 == count) nxp_board_spi_irq(board);
    }
    b->selected = 0; nxp_board_poll(board, now);
}
static void tests(void) {
    bus b; nxp_board board; nxp_state state; nxp_link link; nxp_pwm_frame frame;
    unsigned bit, before, i; uint8_t q[97], rx[97], dummy[97] = {0};
    for (bit = 0; bit < 7; ++bit) {
        initialize(&b, &board, &state, &link, NXP_QUAL_PWM_REQUIRED & ~(1u << bit));
        CHECK(!nxp_board_start_pwm(&board) && !nxp_board_force_off(&board) && !b.writes);
        if (bit < 5) CHECK(!nxp_board_start_spi(&board) && !b.writes);
    }
    initialize(&b, &board, &state, &link, NXP_QUAL_PWM_REQUIRED); board.config.approved_part = 0;
    CHECK(!nxp_board_start_spi(&board) && !nxp_board_start_pwm(&board) && !b.writes);
    board.config.approved_part = 1; CHECK(!nxp_board_start_spi(&board) && !nxp_board_start_pwm(&board) && !b.writes);
    initialize(&b, &board, &state, &link, NXP_QUAL_PWM_REQUIRED); board.config.clock_hz = 12000000;
    CHECK(!nxp_board_start_spi(&board) && !nxp_board_start_pwm(&board) && !b.writes);
    initialize(&b, &board, &state, &link, NXP_QUAL_PWM_REQUIRED);
    store(&b, 0x40044048, 0x80); store(&b, 0x4004404c, 0x80);
    CHECK(nxp_board_start_pwm(&board));
    CHECK(stored(&b, 0x4001400c) == 47 && stored(&b, 0x40014020) == 254 && stored(&b, 0x40014018) == 255);
    CHECK(stored(&b, 0x4001800c) == 0 && stored(&b, 0x40018020) == 25499 && stored(&b, 0x40018024) == 25500);
    CHECK(stored(&b, 0x40014074) == 3 && stored(&b, 0x40018074) == 11);
    CHECK(stored(&b, 0x40044034) == 0x83 && stored(&b, 0x40044038) == 0x83 && stored(&b, 0x40044040) == 0x82);
    CHECK(stored(&b, 0x40044048) == 0x82 && stored(&b, 0x4004404c) == 0x82);
    /* Every timer-enable write follows all five known dark comparisons. */
    for (i = 0; i < b.writes; ++i) if ((b.log_address[i] == 0x40014004 || b.log_address[i] == 0x40018004) && b.log_value[i] == 1) CHECK(i > 20);
    state.effect = 1; state.rgb[0] = 255; state.rgb_brightness = 255; nxp_render(&state, 1, &frame);
    CHECK(nxp_board_apply_pwm(&board, &frame) && stored(&b, 0x40018024) == 0);
    before = b.writes; frame.red_match = 25501; CHECK(!nxp_board_apply_pwm(&board, &frame) && b.writes == before);
    CHECK(nxp_board_force_off(&board) && !board.pwm_started);
    CHECK(stored(&b, 0x40044034) == 0x81 && stored(&b, 0x40044038) == 0x81 && stored(&b, 0x40044040) == 0x80);
    CHECK(stored(&b, 0x40044048) == 0x80 && stored(&b, 0x4004404c) == 0x80);
    initialize(&b, &board, &state, &link, NXP_QUAL_SPI_REQUIRED);
    CHECK(nxp_board_start_spi(&board) && !board.pwm_started && b.tx_count == 8);
    CHECK(stored(&b, 0x400440ac) == 0x12 && stored(&b, 0x4004409c) == 0x13); /* Port1 starts at0x60. */
    CHECK(stored(&b, 0x40058000) == 7 && stored(&b, 0x40058004) == 6);
    make_version(q); clocks(&b, &board, q, rx, 97, 1); CHECK(link.phase == NXP_LINK_LENGTH && b.tx_count == 2);
    clocks(&b, &board, dummy, rx, 2, 2); CHECK(rx[0] == 0 && rx[1] == 97 && link.phase == NXP_LINK_BODY);
    clocks(&b, &board, dummy, rx, 97, 3); CHECK(rx[7] == 2 && rx[12] == 4 && rx[16] == 1 && !nxp_link_ready(&link));
    clocks(&b, &board, q, rx, 97, 4); nxp_board_poll(&board, 104);
    CHECK(board.errors == 1 && !nxp_link_ready(&link));
    clocks(&b, &board, q, rx, 8, 105); CHECK(board.errors == 2 && !nxp_link_ready(&link));
    clocks(&b, &board, q, rx, 97, 106); b.fault = 1; nxp_board_spi_irq(&board);
    CHECK(board.fault); b.selected = 1; nxp_board_poll(&board, 107); CHECK(board.fault);
    b.selected = 0; nxp_board_poll(&board, 108); CHECK(board.errors == 3 && !board.fault);
    /* Stock bridge connection notifications are short completed-CS transfers. */
    initialize(&b, &board, &state, &link, NXP_QUAL_SPI_REQUIRED);
    CHECK(nxp_board_start_spi(&board));
    memset(q, 0, sizeof(q)); q[0] = 2; q[5] = 1; q[6] = 11; q[7] = q[8] = 1;
    clocks(&b, &board, q, rx, 9, 1); CHECK(state.claimed && link.phase == NXP_LINK_LENGTH && !board.errors);
    clocks(&b, &board, dummy, rx, 2, 2); CHECK(rx[1] == 97);
    clocks(&b, &board, dummy, rx, 97, 3); CHECK(rx[6] == 4 && rx[14] == 0x49 && rx[15] == 1);
    clocks(&b, &board, q, rx, 9, 4); CHECK(nxp_link_ready(&link) && !board.errors);
    clocks(&b, &board, dummy, rx, 2, 4); CHECK(!rx[0] && !rx[1] && !nxp_link_ready(&link));
    q[7] = 0; q[8] = 0; clocks(&b, &board, q, rx, 9, 5); CHECK(!state.claimed && nxp_link_ready(&link));
    clocks(&b, &board, dummy, rx, 2, 6); clocks(&b, &board, dummy, rx, 97, 7);
    CHECK(rx[15] == 0 && !board.errors);
    make_version(q); clocks(&b, &board, q, rx, 97, 8);
    clocks(&b, &board, dummy, rx, 2, 9); clocks(&b, &board, dummy, rx, 97, 10);
    CHECK(rx[7] == 2 && rx[14] == 0x87 && !board.errors);
    initialize(&b, &board, &state, &link, NXP_QUAL_PART);
    CHECK(!nxp_board_service_watchdog(&board) && !b.writes);
    store(&b, 0x40048080, 1u << 15); store(&b, 0x40004000, 1);
    store(&b, 0x40004018, 32); store(&b, 0x4000400c, 33);
    CHECK(!nxp_board_service_watchdog(&board) && !b.writes);
    store(&b, 0x4000400c, 32); CHECK(nxp_board_service_watchdog(&board) && b.writes == 2);
    CHECK(b.log_address[0] == 0x40004008 && b.log_value[0] == 0xaa);
    CHECK(b.log_address[1] == 0x40004008 && b.log_value[1] == 0x55);
    initialize(&b, &board, &state, &link, NXP_QUAL_PART | NXP_QUAL_CLOCK | NXP_QUAL_RECOVERY | NXP_ALLOW_SPI_TRIAL);
    CHECK(nxp_board_trial_dark(&board) && nxp_board_start_spi(&board));
    before = b.writes; CHECK(!nxp_board_start_pwm(&board) && b.writes == before);
    for (i = 0; i < b.writes; ++i)
        CHECK((b.log_address[i] < 0x40014000 || b.log_address[i] >= 0x40015000) &&
              (b.log_address[i] < 0x40018000 || b.log_address[i] >= 0x40019000));
}
int main(void) { tests(); printf("%u checks passed; gated register/FIFO adapter simulation; no hardware I/O.\n", checks); return 0; }
