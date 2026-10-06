#ifndef NXP_BOARD_H
#define NXP_BOARD_H

#include "nxp_app.h"

enum {
    NXP_QUAL_PART = 1u, NXP_QUAL_CLOCK = 2u, NXP_QUAL_SPI_WIRING = 4u,
    NXP_QUAL_SPI_MODE = 8u, NXP_QUAL_RECOVERY = 16u,
    NXP_QUAL_PWM_POLARITY = 32u, NXP_QUAL_POWER_LIMITS = 64u,
    NXP_QUAL_SPI_REQUIRED = 31u, NXP_QUAL_PWM_REQUIRED = 127u,
    NXP_ALLOW_SPI_TRIAL = 128u
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
    uint8_t rx[NXP_SPI_SIZE], tx[NXP_SPI_SIZE];
    uint16_t received, queued, expected;
    uint8_t active, fault, spi_started, pwm_started;
    uint32_t errors;
} nxp_board;

/* All writes pass through this mockable register interface. Initialization
 * performs no register access. No nonzero outputs without all seven gates. */
void nxp_board_init(nxp_board *board, const nxp_register_io *io,
                    const nxp_board_config *config, nxp_link *link);
int nxp_board_start_spi(nxp_board *board);
int nxp_board_start_pwm(nxp_board *board);
int nxp_board_apply_pwm(nxp_board *board, const nxp_pwm_frame *frame);
/* Qualified GPIO-low handoff. It does not stop a timer while its pin is high. */
int nxp_board_force_off(nxp_board *board);
/* Trial permits GPIO-low only; it does not qualify or enable PWM. */
int nxp_board_trial_dark(nxp_board *board);
/* Caller preserves/masks interrupt state around this short operation. Feed
 * only an already-enabled WWDT inside its window; never alter TC/MOD/clocks. */
int nxp_board_service_watchdog(nxp_board *board);
/* IRQ service only drains/fills bounded FIFOs. Complete request processing is
 * in poll, called with SSP IRQ masked. Poll must run between CS transactions.
 * This scheduling/timing contract still requires on-board qualification. */
void nxp_board_spi_irq(nxp_board *board);
void nxp_board_poll(nxp_board *board, uint32_t now_ms);

#endif
