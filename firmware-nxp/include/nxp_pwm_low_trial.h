#ifndef NXP_PWM_LOW_TRIAL_H
#define NXP_PWM_LOW_TRIAL_H
#include "nxp_board.h"
enum { NXP_LOW_PULSE_MS = 100, NXP_LOW_GAP_MS = 1500, NXP_LOW_SLOT_MS = 1600,
       NXP_LOW_SEQUENCE_MS = 6500, NXP_LOW_RECORD_WORDS = 256 };
enum { NXP_LOW_IDLE, NXP_LOW_RUNNING, NXP_LOW_COMPLETED, NXP_LOW_FAILED };
enum { NXP_LOW_NONE, NXP_LOW_DEADLINE, NXP_LOW_OWNER, NXP_LOW_SPI,
       NXP_LOW_REGISTER, NXP_LOW_CANCELLED, NXP_LOW_RECOVERY, NXP_LOW_LATE };
typedef struct {
    volatile uint8_t record[NXP_LOW_RECORD_WORDS * 4];
    volatile uint32_t initialized, active, pulse_on, started_ms, initial_errors, channel;
} nxp_pwm_low_trial;
/* Explicitly experimental role1 profile. Fixed five channels, fixed
 * low duty and immutable30s escape. No production qualification or FD. */
int nxp_pwm_low_init(nxp_pwm_low_trial *trial, nxp_board *board);
/* Main service runs IRQ-masked. Subsequent pulse starts and every pulse end
 * are enforced by SysTick even if main stops; masked/halted CPU is not covered. */
void nxp_pwm_low_service(nxp_pwm_low_trial *trial, nxp_board *board, uint32_t now_ms);
void nxp_pwm_low_tick(nxp_pwm_low_trial *trial, nxp_board *board, uint32_t now_ms);
void nxp_pwm_low_recovery(nxp_pwm_low_trial *trial, nxp_board *board, uint32_t now_ms);
#endif
