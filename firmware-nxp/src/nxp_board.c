#include "nxp_board.h"

#define SYSCON UINT32_C(0x40048000)
#define IOCON UINT32_C(0x40044000)
#define GPIO UINT32_C(0x50000000)
#define SSP UINT32_C(0x40058000)
#define WHITE UINT32_C(0x40014000)
#define COLOR UINT32_C(0x40018000)
#define SELECT_BIT (UINT32_C(1) << 17)
#define READY_BIT (UINT32_C(1) << 9)
#define OUTPUT_PINS ((1u << 13) | (1u << 14) | (1u << 16) | (1u << 18) | (1u << 19))
#define TIMER_CLOCKS ((1u << 9) | (1u << 10))

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
    b->expected = b->link->phase == NXP_LINK_LENGTH ? (uint16_t)(2u + b->link->response_size) : NXP_SPI_SIZE;
    for (i = 0; i < sizeof(b->rx); ++i) b->rx[i] = b->tx[i] = 0;
    if (b->link->phase == NXP_LINK_LENGTH) {
        b->tx[1] = b->link->response_size;
        for (i = 0; i < b->link->response_size; ++i) b->tx[i + 2] = b->link->response[i];
    }
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
    /* GPIO owns the pads throughout configuration. Clear inherited capture,
     * counter and external-match modes before enabling PWM in held reset. */
    write_reg(b, base + 4, 2);
    write_reg(b, base + 0x14, 0); write_reg(b, base + 0x28, 0);
    write_reg(b, base + 0x70, 0); write_reg(b, base + 0x74, 0);
    write_reg(b, base + 0x3c, 0);
    write_reg(b, base, base == WHITE ? 0x5fu : 0x3fu);
    write_reg(b, base + 0xc, prescale);
    write_reg(b, base + 0x18, period + 1); write_reg(b, base + 0x1c, period + 1);
    write_reg(b, base + 0x20, period); write_reg(b, base + 0x24, period + 1);
    write_reg(b, base + 0x14, 0x80); /* MR2 reset only; no stop/interrupt bits. */
    write_reg(b, base + 0x74, channels);
    write_reg(b, base + 4, 2); /* Reset clears the PWM outputs, not just TC. */
    write_reg(b, base + 4, 1);
}
static void mux_output(nxp_board *b, unsigned pin, unsigned function) {
    uint32_t address = IOCON + pin * 4u;
    uint32_t value = (read_reg(b, address) & ~7u) | function;
    if (pin == 13 || pin == 14 || pin == 16) value |= 0x80u; /* Digital mode for ADC-capable pins. */
    write_reg(b, address, value);
}
static int gpio_valid(nxp_board *b) {
    return (read_reg(b, GPIO + 0x2000) & OUTPUT_PINS) == OUTPUT_PINS &&
        !(read_reg(b, GPIO + 0x2100) & OUTPUT_PINS) &&
        (read_reg(b, IOCON + 13u * 4u) & 0x87u) == 0x81u &&
        (read_reg(b, IOCON + 14u * 4u) & 0x87u) == 0x81u &&
        (read_reg(b, IOCON + 16u * 4u) & 0x87u) == 0x80u &&
        !(read_reg(b, IOCON + 18u * 4u) & 7u) && !(read_reg(b, IOCON + 19u * 4u) & 7u);
}
static int pwm_mux_valid(nxp_board *b) {
    return (read_reg(b, IOCON + 13u * 4u) & 0x87u) == 0x83u &&
        (read_reg(b, IOCON + 14u * 4u) & 0x87u) == 0x83u &&
        (read_reg(b, IOCON + 16u * 4u) & 0x87u) == 0x82u &&
        (read_reg(b, IOCON + 18u * 4u) & 7u) == 2u && (read_reg(b, IOCON + 19u * 4u) & 7u) == 2u;
}
static int timer_valid(nxp_board *b, uint32_t base, uint32_t prescale, uint32_t period, uint32_t channels) {
    return read_reg(b, base + 4) == 1 && read_reg(b, base + 0xc) == prescale &&
        read_reg(b, base + 0x14) == 0x80 && read_reg(b, base + 0x20) == period &&
        !read_reg(b, base + 0x28) && !read_reg(b, base + 0x70) &&
        !(read_reg(b, base + 0x3c) & 0xff0u) && read_reg(b, base + 0x74) == channels;
}
static int off_matches_valid(nxp_board *b, uint32_t base, uint32_t value) {
    return read_reg(b, base + 0x18) == value && read_reg(b, base + 0x1c) == value &&
        read_reg(b, base + 0x24) == value;
}
static int gpio_dark(nxp_board *b) {
    write_reg(b, SYSCON + 0x80, read_reg(b, SYSCON + 0x80) | (1u << 6) | (1u << 16));
    write_reg(b, GPIO + 0x2280, OUTPUT_PINS);
    write_reg(b, GPIO + 0x2000, read_reg(b, GPIO + 0x2000) | OUTPUT_PINS);
    mux_output(b, 13, 1); mux_output(b, 14, 1); mux_output(b, 16, 0);
    mux_output(b, 18, 0); mux_output(b, 19, 0);
    if (!gpio_valid(b)) { b->pwm_fault = 1; return 0; }
    b->pwm_started = 0; return 1;
}
static int stop_pwm(nxp_board *b) {
#if defined(NXP_PRODUCTION_LIGHTING) && NXP_PRODUCTION_LIGHTING
    if (!gpio_dark(b)) {
        /* Keep timers running: stopping a HIGH latch after a failed pad mux
         * would preserve its output. Best-effort all-off matches provide an
         * independent path to darkness at the next PWM reset boundary. */
        if ((read_reg(b, SYSCON + 0x80) & TIMER_CLOCKS) == TIMER_CLOCKS) {
            write_reg(b, COLOR + 0x24, 25500); write_reg(b, COLOR + 0x1c, 25500);
            write_reg(b, COLOR + 0x18, 25500); write_reg(b, WHITE + 0x18, 255);
            write_reg(b, WHITE + 0x1c, 255);
            /* Failure is sticky even if this fallback is readable. */
            (void)off_matches_valid(b, COLOR, 25500);
            (void)off_matches_valid(b, WHITE, 255);
        }
        return 0;
    }
#else
    if (!gpio_dark(b)) return 0;
#endif
    if ((read_reg(b, SYSCON + 0x80) & TIMER_CLOCKS) == TIMER_CLOCKS) {
        write_reg(b, WHITE + 4, 2); write_reg(b, COLOR + 4, 2);
        if (read_reg(b, WHITE + 4) != 2 || read_reg(b, COLOR + 4) != 2) {
            b->pwm_fault = 1; return 0;
        }
    }
    return 1;
}
int nxp_board_force_off(nxp_board *b) {
    return qualified(b, NXP_QUAL_PWM_REQUIRED) ? stop_pwm(b) : 0;
}
int nxp_board_trial_dark(nxp_board *b) {
    const uint32_t trial = NXP_QUAL_PART | NXP_QUAL_CLOCK | NXP_QUAL_RECOVERY | NXP_ALLOW_SPI_TRIAL;
    if (!qualified(b, trial)) return 0;
    return gpio_dark(b);
}
static int prepare_pwm(nxp_board *b) {
    if (b->pwm_fault || b->pwm_started || !stop_pwm(b)) return 0;
    write_reg(b, SYSCON + 0x80, read_reg(b, SYSCON + 0x80) | TIMER_CLOCKS | (1u << 16));
    configure_timer(b, WHITE, 47, 254, 3); configure_timer(b, COLOR, 0, 25499, 11);
    if ((read_reg(b, SYSCON + 0x80) & TIMER_CLOCKS) != TIMER_CLOCKS ||
        !timer_valid(b, WHITE, 47, 254, 3) || !timer_valid(b, COLOR, 0, 25499, 11) ||
        !off_matches_valid(b, WHITE, 255) || !off_matches_valid(b, COLOR, 25500)) {
        b->pwm_fault = 1; (void)stop_pwm(b); return 0;
    }
    return 1;
}
static int connect_pwm(nxp_board *b) {
    if (b->pwm_fault || b->pwm_started || !gpio_valid(b) ||
        !timer_valid(b, WHITE, 47, 254, 3) || !timer_valid(b, COLOR, 0, 25499, 11) ||
        !off_matches_valid(b, WHITE, 255) || !off_matches_valid(b, COLOR, 25500)) goto failed;
    /* Both timers are already running with verified dark matches. Mark a
     * possible connection before mux writes, so panic also covers setup. */
    b->pwm_started = 1;
    mux_output(b, 18, 2); mux_output(b, 19, 2);
    mux_output(b, 13, 3); mux_output(b, 14, 3); mux_output(b, 16, 2);
    if (!pwm_mux_valid(b)) goto failed;
    return 1;
failed:
    b->pwm_fault = 1; (void)stop_pwm(b); return 0;
}
int nxp_board_start_pwm(nxp_board *b) {
    return qualified(b, NXP_QUAL_PWM_REQUIRED) && prepare_pwm(b) && connect_pwm(b);
}
static int off_trial_permitted(nxp_board *b) {
    return qualified(b, NXP_QUAL_PART | NXP_QUAL_CLOCK | NXP_QUAL_RECOVERY |
        NXP_ALLOW_SPI_TRIAL | NXP_ALLOW_PWM_OFF_TRIAL) && b->config.observed_part == 0xbc40u &&
        !(b->config.qualifications & (NXP_QUAL_PWM_POLARITY | NXP_QUAL_POWER_LIMITS));
}
int nxp_board_prepare_pwm_off_trial(nxp_board *b) { return off_trial_permitted(b) && prepare_pwm(b); }
int nxp_board_connect_pwm_off_trial(nxp_board *b) { return off_trial_permitted(b) && connect_pwm(b); }
int nxp_board_stop_pwm_off_trial(nxp_board *b) { return off_trial_permitted(b) && stop_pwm(b); }
int nxp_board_apply_pwm(nxp_board *b, const nxp_pwm_frame *f) {
    if (!b || !b->pwm_started || b->pwm_fault || !qualified(b, NXP_QUAL_PWM_REQUIRED) || !f ||
        f->red_match > 25500 || f->green_match > 25500 || f->blue_match > 25500 ||
#if defined(NXP_PRODUCTION_LIGHTING) && NXP_PRODUCTION_LIGHTING
        f->cool_match > 255 || f->warm_match > 255 ||
        ((f->red_match < 25500 || f->green_match < 25500 || f->blue_match < 25500) &&
         (f->cool_match < 217 || f->warm_match < 217))) return 0;
#else
        f->cool_match > 255 || f->warm_match > 255) return 0;
#endif
    if ((read_reg(b, SYSCON + 0x80) & TIMER_CLOCKS) != TIMER_CLOCKS || !pwm_mux_valid(b) ||
        !timer_valid(b, WHITE, 47, 254, 3) || !timer_valid(b, COLOR, 0, 25499, 11)) goto failed;
#if defined(NXP_PRODUCTION_LIGHTING) && NXP_PRODUCTION_LIGHTING
    /* Reduce commanded duty before increasing another channel. With inverse
     * PWM, a larger match is a lower duty. This preserves the bounded white
     * sum and mixed-mode envelope at every register write during handoff.
     * Timer latches still change at their hardware match/reset boundaries. */
    static const uint32_t address[5] = {COLOR + 0x24, COLOR + 0x1c, COLOR + 0x18, WHITE + 0x18, WHITE + 0x1c};
    const uint32_t target[5] = {f->red_match, f->green_match, f->blue_match, f->cool_match, f->warm_match};
    uint32_t previous[5];
    unsigned i;
    for (i = 0; i < 5; ++i) previous[i] = read_reg(b, address[i]);
    for (i = 0; i < 5; ++i) if (target[i] >= previous[i]) write_reg(b, address[i], target[i]);
    for (i = 0; i < 5; ++i)
        if (target[i] >= previous[i] && read_reg(b, address[i]) != target[i]) goto failed;
    for (i = 0; i < 5; ++i) if (target[i] < previous[i]) write_reg(b, address[i], target[i]);
#else
    write_reg(b, COLOR + 0x24, f->red_match); write_reg(b, COLOR + 0x1c, f->green_match);
    write_reg(b, COLOR + 0x18, f->blue_match); write_reg(b, WHITE + 0x18, f->cool_match);
    write_reg(b, WHITE + 0x1c, f->warm_match);
#endif
    if (read_reg(b, COLOR + 0x24) != f->red_match || read_reg(b, COLOR + 0x1c) != f->green_match ||
        read_reg(b, COLOR + 0x18) != f->blue_match || read_reg(b, WHITE + 0x18) != f->cool_match ||
        read_reg(b, WHITE + 0x1c) != f->warm_match) goto failed;
    return 1;
failed:
    b->pwm_fault = 1; (void)nxp_board_force_off(b); return 0;
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
    /* In slave mode, queued TX words can keep SSP.BSY set after deselect.
     * Prepared response bytes therefore cannot be used as an end-of-request
     * gate. The separate select monitor defines completed transactions. */
    if (selected(b)) {
        if (nxp_link_expire(b->link, now_ms) == NXP_EXPIRED) { b->fault = 1; ready(b, 0); }
        return;
    }
    /* CS may rise just after the first drain sampled an empty RX FIFO. Its
     * final byte is then still pending even though this select sample is idle.
     * Drain again after observing deselect, before judging a packet's length.
     * A new transfer may start meanwhile: leave it for the next bounded poll. */
    nxp_board_spi_irq(b);
    if (selected(b) || (read_reg(b, SSP + 0xc) & 4u)) return;
    if (b->active || b->fault) {
        result = NXP_BAD_PACKET;
        if (!b->fault) {
            if (b->link->phase == NXP_LINK_REQUEST) {
                result = nxp_link_transaction(b->link, b->rx, b->received, ignored, sizeof(ignored), now_ms);
            } else if (b->received == b->expected ||
                       (b->link->phase == NXP_LINK_LENGTH && b->received == 2)) {
                if (b->link->phase == NXP_LINK_LENGTH)
                    result = nxp_link_transaction(b->link, b->rx, 2, ignored, sizeof(ignored), now_ms);
                else result = NXP_OK;
                if (result == NXP_OK && b->link->phase == NXP_LINK_BODY) {
                    if (b->received == 2) {
                        /* Keep the already queued body and cumulative RX.
                         * The master need not wait or let main run here. */
                        b->active = 0; return;
                    }
                    result = nxp_link_transaction(b->link, b->rx + 2, b->received - 2u,
                                                  ignored, sizeof(ignored), now_ms);
                }
            }
        }
        if (result != NXP_OK) { if (b->errors != UINT32_MAX) ++b->errors; nxp_link_cancel(b->link); }
        prepare(b);
    } else if (nxp_link_expire(b->link, now_ms) == NXP_EXPIRED) {
        if (b->errors != UINT32_MAX) ++b->errors;
        prepare(b);
    }
}
