#ifndef NXP_BOARD_H
#define NXP_BOARD_H

#include "nxp_app.h"

enum {
    NXP_QUAL_PART = 1u, NXP_QUAL_CLOCK = 2u, NXP_QUAL_SPI_WIRING = 4u,
    NXP_QUAL_SPI_MODE = 8u, NXP_QUAL_RECOVERY = 16u,
    NXP_QUAL_PWM_POLARITY = 32u, NXP_QUAL_POWER_LIMITS = 64u,
    NXP_QUAL_SPI_REQUIRED = 31u, NXP_QUAL_PWM_REQUIRED = 127u,
    NXP_ALLOW_SPI_TRIAL = 128u, NXP_ALLOW_PWM_OFF_TRIAL = 256u, NXP_ALLOW_PWM_LOW_TRIAL = 512u
};
typedef struct {
    void *user;
    uint32_t (*read)(void *user, uint32_t address);
    void (*write)(void *user, uint32_t address, uint32_t value);
} nxp_register_io;
typedef struct {
    uint32_t observed_part, approved_part, clock_hz, qualifications;
    uint8_t spi_mode;
} nxp_board_config;
typedef struct {
    nxp_register_io io;
    nxp_board_config config;
    nxp_link *link;
    /* A reply is one prequeued stream across length/body CS pulses. */
    uint8_t rx[NXP_SPI_SIZE + 2], tx[NXP_SPI_SIZE + 2];
    uint16_t received, queued, expected;
    uint8_t active, fault, spi_started, pwm_started, pwm_fault;
    uint32_t errors;
#if defined(NXP_PRODUCTION_LIGHTING) && NXP_PRODUCTION_LIGHTING
    /* Compare writes do not clear an already-HIGH PWM latch. Preserve pending
     * reductions across separate apply calls until a timer reset is observed. */
    uint32_t pwm_reduction_counter[2];
    uint8_t pwm_reduction_pending;
#endif
} nxp_board;

/* All writes pass through this mockable register interface. Initialization
 * performs no register access. No nonzero outputs without all seven gates. */
void nxp_board_init(nxp_board *board, const nxp_register_io *io,
                    const nxp_board_config *config, nxp_link *link);
int nxp_board_start_spi(nxp_board *board);
int nxp_board_start_pwm(nxp_board *board);
/* Production apply may wait one timer cycle for reduced output latches before
 * raising other channels. Call with interrupts enabled; polling is bounded and
 * a stalled/corrupt timer invokes the sticky fail-dark path. */
int nxp_board_apply_pwm(nxp_board *board, const nxp_pwm_frame *frame);
/* Qualified GPIO-low handoff, verified before holding timers in reset. A failed
 * mux/DIR/pad readback leaves the timers running and latches pwm_fault. A fault
 * cannot be cleared by another start/apply call; the platform must recover. */
int nxp_board_force_off(nxp_board *board);
/* Trial permits GPIO-low only; it does not qualify or enable PWM. */
int nxp_board_trial_dark(nxp_board *board);
/* Explicit experiment permission, with production polarity/power bits absent.
 * Prepare starts fixed all-low timers while GPIO owns every pad; connect only
 * exposes those verified low timers. No nonzero compare API is provided. */
int nxp_board_prepare_pwm_off_trial(nxp_board *board);
int nxp_board_connect_pwm_off_trial(nxp_board *board);
int nxp_board_stop_pwm_off_trial(nxp_board *board);
/* Caller preserves/masks interrupt state around this short operation. Feed
 * only an already-enabled WWDT inside its window; never alter TC/MOD/clocks. */
int nxp_board_service_watchdog(nxp_board *board);
/* IRQ service only drains/fills bounded FIFOs. Complete request processing is
 * in poll, called with SSP IRQ masked. READY separates request/reply exchanges;
 * length/body replies need neither a main-loop poll nor an IRQ in the CS gap.
 * Physical timing still requires on-board qualification. */
void nxp_board_spi_irq(nxp_board *board);
void nxp_board_poll(nxp_board *board, uint32_t now_ms);

#endif
