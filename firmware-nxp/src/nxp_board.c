#include "nxp_board.h"

#define SYSCON UINT32_C(0x40048000)
#define IOCON UINT32_C(0x40044000)
#define GPIO UINT32_C(0x50000000)
#define SSP UINT32_C(0x40058000)
#define WHITE UINT32_C(0x40014000)
#define COLOR UINT32_C(0x40018000)
#define SELECT_BIT (UINT32_C(1) << 17)
#define READY_BIT (UINT32_C(1) << 9)

static uint32_t read_reg(nxp_board *b, uint32_t address) { return b->io.read(b->io.user, address); }
static void write_reg(nxp_board *b, uint32_t address, uint32_t value) { b->io.write(b->io.user, address, value); }
static int qualified(const nxp_board *b, uint32_t mask) {
    return b && b->io.read && b->io.write && b->config.approved_part &&
        b->config.observed_part == b->config.approved_part && b->config.clock_hz == 48000000 &&
        (b->config.qualifications & mask) == mask;
}
static void ready(nxp_board *b, int low) { write_reg(b, GPIO + (low ? 0x2280u : 0x2200u), READY_BIT); }
static int selected(nxp_board *b) { return !(read_reg(b, GPIO + 0x2100u) & SELECT_BIT); }
static void reset_ssp(nxp_board *b) {
    uint32_t reset = read_reg(b, SYSCON + 4);
    write_reg(b, SYSCON + 4, reset & ~4u);
    write_reg(b, SYSCON + 4, reset | 4u);
    write_reg(b, SSP, 7u | ((uint32_t)b->config.spi_mode << 6));
    write_reg(b, SSP + 0x10, 2); /* Valid even prescaler, although slave clocks externally. */
    write_reg(b, SSP + 0x20, 3); /* Clear overrun/receive-timeout conditions. */
    write_reg(b, SSP + 0x14, 7); /* RX, RX timeout, RX overrun; TX serviced with RX. */
    write_reg(b, SSP + 4, 6); /* Slave mode and serial enable. */
}
static void prepare(nxp_board *b) {
    unsigned i;
    b->received = b->queued = 0; b->active = b->fault = 0;
    b->expected = b->link->phase == NXP_LINK_LENGTH ? 2 : NXP_SPI_SIZE;
    for (i = 0; i < NXP_SPI_SIZE; ++i) b->rx[i] = b->tx[i] = 0;
    if (b->link->phase == NXP_LINK_LENGTH) b->tx[1] = NXP_SPI_SIZE;
    if (b->link->phase == NXP_LINK_BODY) for (i = 0; i < NXP_SPI_SIZE; ++i) b->tx[i] = b->link->response[i];
    reset_ssp(b);
    /* FIFO depth is eight words. Bounded loops never trust a stuck status bit. */
    for (i = 0; i < 8 && b->queued < b->expected && (read_reg(b, SSP + 0xc) & 2u); ++i)
        write_reg(b, SSP + 8, b->tx[b->queued++]);
    ready(b, nxp_link_ready(b->link));
}
void nxp_board_init(nxp_board *b, const nxp_register_io *io, const nxp_board_config *config, nxp_link *link) {
    size_t i; uint8_t *bytes;
    if (!b) return;
    bytes = (uint8_t *)b; for (i = 0; i < sizeof(*b); ++i) bytes[i] = 0;
    if (io) b->io = *io;
    if (config) b->config = *config;
    b->link = link;
}
int nxp_board_start_spi(nxp_board *b) {
    const uint32_t trial = NXP_QUAL_PART | NXP_QUAL_CLOCK | NXP_QUAL_RECOVERY | NXP_ALLOW_SPI_TRIAL;
    if ((!qualified(b, NXP_QUAL_SPI_REQUIRED) && !qualified(b, trial)) || !b->link || !b->link->state ||
        (b->config.spi_mode != 0 && b->config.spi_mode != 3)) return 0;
    /* Enable input observation before deciding whether CS is idle. */
    write_reg(b, SYSCON + 0x80, read_reg(b, SYSCON + 0x80) | (1u << 6) | (1u << 16));
    write_reg(b, IOCON + 17u * 4u, 0x10);
    write_reg(b, GPIO + 0x2000, read_reg(b, GPIO + 0x2000) & ~SELECT_BIT);
    if (selected(b)) return 0;
    write_reg(b, SYSCON + 0x80, read_reg(b, SYSCON + 0x80) | (1u << 18));
    write_reg(b, SYSCON + 0x9c, 1);
    write_reg(b, IOCON + 0x60u + 19u * 4u, 0x12);
    write_reg(b, IOCON + 0x60u + 15u * 4u, 0x13);
    write_reg(b, IOCON + 22u * 4u, 0x13); write_reg(b, IOCON + 21u * 4u, 0x12);
    write_reg(b, IOCON + 17u * 4u, 0x10); write_reg(b, IOCON + 9u * 4u, 0x10);
    ready(b, 0); /* Set latch before output direction to avoid a ready glitch. */
    write_reg(b, GPIO + 0x2000, (read_reg(b, GPIO + 0x2000) & ~SELECT_BIT) | READY_BIT);
    b->spi_started = 1; prepare(b); return 1;
}
static void configure_timer(nxp_board *b, uint32_t base, uint32_t prescale, uint32_t period, uint32_t channels) {
    write_reg(b, base + 4, 2); /* Stop/reset counter before changing matches. */
    write_reg(b, base + 0xc, prescale); write_reg(b, base + 0x14, 0x80);
    write_reg(b, base + 0x18, period + 1); write_reg(b, base + 0x1c, period + 1);
    write_reg(b, base + 0x20, period); write_reg(b, base + 0x24, period + 1);
    write_reg(b, base + 0x74, channels);
}
static void mux_output(nxp_board *b, unsigned pin, unsigned function) {
    uint32_t address = IOCON + pin * 4u;
    uint32_t value = (read_reg(b, address) & ~7u) | function;
    if (pin == 13 || pin == 14 || pin == 16) value |= 0x80u; /* Digital mode for ADC-capable pins. */
    write_reg(b, address, value);
}
static void gpio_dark(nxp_board *b) {
    const uint32_t pins = (1u << 13) | (1u << 14) | (1u << 16) | (1u << 18) | (1u << 19);
    write_reg(b, SYSCON + 0x80, read_reg(b, SYSCON + 0x80) | (1u << 6) | (1u << 16));
    write_reg(b, GPIO + 0x2280, pins);
    write_reg(b, GPIO + 0x2000, read_reg(b, GPIO + 0x2000) | pins);
    mux_output(b, 13, 1); mux_output(b, 14, 1); mux_output(b, 16, 0);
    mux_output(b, 18, 0); mux_output(b, 19, 0);
    b->pwm_started = 0;
}
int nxp_board_force_off(nxp_board *b) {
    if (!qualified(b, NXP_QUAL_PWM_REQUIRED)) return 0;
    gpio_dark(b); return 1;
}
int nxp_board_trial_dark(nxp_board *b) {
    const uint32_t trial = NXP_QUAL_PART | NXP_QUAL_CLOCK | NXP_QUAL_RECOVERY | NXP_ALLOW_SPI_TRIAL;
    if (!qualified(b, trial)) return 0;
    gpio_dark(b); return 1;
}
int nxp_board_start_pwm(nxp_board *b) {
    if (!qualified(b, NXP_QUAL_PWM_REQUIRED)) return 0;
    (void)nxp_board_force_off(b);
    write_reg(b, SYSCON + 0x80, read_reg(b, SYSCON + 0x80) | (1u << 9) | (1u << 10) | (1u << 16));
    configure_timer(b, WHITE, 47, 254, 3); configure_timer(b, COLOR, 0, 25499, 11);
    /* Matches are already dark before mux connects timers to the board. */
    mux_output(b, 18, 2); mux_output(b, 19, 2);
    mux_output(b, 13, 3); mux_output(b, 14, 3); mux_output(b, 16, 2);
    write_reg(b, WHITE + 4, 1); write_reg(b, COLOR + 4, 1); b->pwm_started = 1; return 1;
}
int nxp_board_apply_pwm(nxp_board *b, const nxp_pwm_frame *f) {
    if (!b || !b->pwm_started || !qualified(b, NXP_QUAL_PWM_REQUIRED) || !f ||
        f->red_match > 25500 || f->green_match > 25500 || f->blue_match > 25500 ||
        f->cool_match > 255 || f->warm_match > 255) return 0;
    write_reg(b, COLOR + 0x24, f->red_match); write_reg(b, COLOR + 0x1c, f->green_match);
    write_reg(b, COLOR + 0x18, f->blue_match); write_reg(b, WHITE + 0x18, f->cool_match);
    write_reg(b, WHITE + 0x1c, f->warm_match); return 1;
}
int nxp_board_service_watchdog(nxp_board *b) {
    uint32_t window, remaining;
    if (!qualified(b, NXP_QUAL_PART) || !(read_reg(b, SYSCON + 0x80) & (1u << 15)) ||
        !(read_reg(b, 0x40004000) & 1u)) return 0;
    window = read_reg(b, 0x40004018) & 0x00ffffffu;
    remaining = read_reg(b, 0x4000400c) & 0x00ffffffu;
    if (remaining > window) return 0;
    write_reg(b, 0x40004008, 0xaa); write_reg(b, 0x40004008, 0x55); return 1;
}
void nxp_board_spi_irq(nxp_board *b) {
    unsigned i;
    if (!b || !b->spi_started) return;
    if (selected(b)) b->active = 1;
    if (read_reg(b, SSP + 0x18) & 1u) b->fault = 1;
    for (i = 0; i < 8 && (read_reg(b, SSP + 0xc) & 4u); ++i) {
        uint8_t value = (uint8_t)read_reg(b, SSP + 8); b->active = 1;
        if (b->received < b->expected) b->rx[b->received++] = value;
        else b->fault = 1;
    }
    for (i = 0; i < 8 && b->queued < b->expected && (read_reg(b, SSP + 0xc) & 2u); ++i)
        write_reg(b, SSP + 8, b->tx[b->queued++]);
    write_reg(b, SSP + 0x20, 3);
}
void nxp_board_poll(nxp_board *b, uint32_t now_ms) {
    uint8_t ignored[NXP_SPI_SIZE]; nxp_result result;
    if (!b || !b->spi_started) return;
    nxp_board_spi_irq(b);
    if (selected(b) || (read_reg(b, SSP + 0xc) & 0x10u)) {
        if (nxp_link_expire(b->link, now_ms) == NXP_EXPIRED) { b->fault = 1; ready(b, 0); }
        return;
    }
    if (b->active || b->fault) {
        result = b->fault ? NXP_BAD_PACKET : nxp_link_transaction(b->link, b->rx, b->received, ignored, sizeof(ignored), now_ms);
        if (result != NXP_OK) { if (b->errors != UINT32_MAX) ++b->errors; nxp_link_cancel(b->link); }
        prepare(b);
    } else if (nxp_link_expire(b->link, now_ms) == NXP_EXPIRED) {
        if (b->errors != UINT32_MAX) ++b->errors;
        prepare(b);
    }
}
