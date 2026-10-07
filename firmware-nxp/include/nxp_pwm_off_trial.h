#ifndef NXP_PWM_OFF_TRIAL_H
#define NXP_PWM_OFF_TRIAL_H

#include "nxp_board.h"

enum { NXP_OFF_WINDOW_MS = 400, NXP_OFF_RECORD_WORDS = 224 };
enum { NXP_OFF_IDLE, NXP_OFF_RUNNING, NXP_OFF_COMPLETED, NXP_OFF_FAILED };
enum { NXP_OFF_NONE, NXP_OFF_DEADLINE, NXP_OFF_OWNER, NXP_OFF_SPI,
       NXP_OFF_REGISTER, NXP_OFF_CANCELLED, NXP_OFF_RECOVERY };
typedef struct {
    volatile uint8_t record[NXP_OFF_RECORD_WORDS * 4];
    volatile uint32_t initialized, active, started_ms, initial_errors;
} nxp_pwm_off_trial;

/* Explicit off-only experiment: no production qualification flags, arbitrary
 * duty values, confirmation or re-arm. Initialization attaches fixed F1 pages
 * after the runtime platform has reported role1/recovery-only. Its retained
 * USB-RAM counter is correlation data, not authentication or a boot guarantee. */
int nxp_pwm_off_init(nxp_pwm_off_trial *trial, nxp_board *board);
/* Main calls service with interrupts masked. SysTick calls tick independently
 * of main; all operations are bounded. Only the completed OFF1 ACK can arm. */
void nxp_pwm_off_service(nxp_pwm_off_trial *trial, nxp_board *board, uint32_t now_ms);
void nxp_pwm_off_tick(nxp_pwm_off_trial *trial, nxp_board *board, uint32_t now_ms);
void nxp_pwm_off_recovery(nxp_pwm_off_trial *trial, nxp_board *board, uint32_t now_ms);

#endif
