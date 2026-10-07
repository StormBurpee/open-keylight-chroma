#include "nxp_app.h"
#include "nxp_board.h"
#include "qualification.h"
#if NXP_PWM_LOW_TRIAL
#include "nxp_pwm_low_trial.h"
static nxp_pwm_low_trial low_trial;
#elif NXP_PWM_OFF_TRIAL
#include "nxp_pwm_off_trial.h"
static nxp_pwm_off_trial off_trial;
#endif
#include <stdint.h>

extern uint32_t _data_load, _data_start, _data_end, _bss_start, _bss_end;
static nxp_state state;
static nxp_link link;
static nxp_board board;
static volatile uint32_t milliseconds;
static volatile uint32_t recovery_allowed;
static void enter_recovery(void);

/* Inspectable qualification record. No device ID is invented. Probe and
 * peripheral service remain gated until the exact hardware is qualified. */
volatile struct {
    uint32_t marker, abi_version, part_id, qualification_flags, iap_status, clock_hz;
    uint32_t app_start, app_limit, vector_reservation, outputs_enabled, reset_status;
} nxp_diagnostic;

static uint32_t read_register(void *unused, uint32_t address) { (void)unused; return *(volatile uint32_t *)(uintptr_t)address; }
static void write_register(void *unused, uint32_t address, uint32_t value) { (void)unused; *(volatile uint32_t *)(uintptr_t)address = value; }
static uint32_t configured_clock(void) {
    /* 12 MHz oscillator remains a board qualification fact, not a measurement. */
    return read_register(0, 0x40048040) == 1 && read_register(0, 0x40048008) == 0x23 &&
        (read_register(0, 0x4004800c) & 1) && read_register(0, 0x40048070) == 3 &&
        read_register(0, 0x40048078) == 1 ? 48000000u : 0;
}
void SysTick_Handler(void) {
    uint32_t now = ++milliseconds;
#if NXP_PWM_LOW_TRIAL
    nxp_pwm_low_tick(&low_trial, &board, now);
#elif NXP_PWM_OFF_TRIAL
    nxp_pwm_off_tick(&off_trial, &board, now);
#endif
    /* Expiry must not depend on a healthy main/SPI polling loop. State
     * mutation runs with interrupts masked, so confirmation is indivisible
     * with respect to this check. This cannot cover a stuck masked CPU. */
    if (recovery_allowed == UINT32_C(0x5245434f) && nxp_trial_expired(&state, now)) enter_recovery();
}
void SSP1_Handler(void) { nxp_board_spi_irq(&board); }
static void enter_recovery(void) {
    __asm volatile ("cpsid i");
#if NXP_PWM_LOW_TRIAL
    nxp_pwm_low_recovery(&low_trial, &board, milliseconds);
#elif NXP_PWM_OFF_TRIAL
    nxp_pwm_off_recovery(&off_trial, &board, milliseconds);
#endif
#if !NXP_SPI_ONLY_TRIAL
    if (board.pwm_started) (void)nxp_board_force_off(&board);
#endif
    write_register(0, 0x40048080, read_register(0, 0x40048080) | (1u << 27));
    __asm volatile ("dsb");
    write_register(0, 0x200047fc, 0xaaaaaaaa);
    __asm volatile ("dsb");
    write_register(0, 0xe000ed0c, 0x05fa0004);
    __asm volatile ("dsb");
    for (;;) __asm volatile ("wfi");
}
void nxp_panic(void) {
    if (recovery_allowed == UINT32_C(0x5245434f)) enter_recovery();
#if !NXP_SPI_ONLY_TRIAL
    if (board.pwm_started) (void)nxp_board_force_off(&board);
#endif
    nxp_diagnostic.outputs_enabled = 0;
}
static void service_watchdog(void) {
    uint32_t previous;
    __asm volatile ("mrs %0, primask\ncpsid i" : "=r"(previous) :: "memory");
    (void)nxp_board_service_watchdog(&board);
    __asm volatile ("msr primask, %0" :: "r"(previous) : "memory");
}

void nxp_reset_c(void) {
    typedef void (*iap_entry)(uint32_t *, uint32_t *);
    uint32_t command[5] = {54, 0, 0, 0, 0}, result[5] = {UINT32_MAX, 0, 0, 0, 0};
    nxp_register_io io = {0, read_register, write_register};
    nxp_board_config config;
#if !NXP_SPI_ONLY_TRIAL
    uint32_t last_render = 0;
#endif
    uint32_t *to = &_data_start, *from = &_data_load;
    while (to < &_data_end) *to++ = *from++;
    for (to = &_bss_start; to < &_bss_end; ++to) *to = 0;
    nxp_state_init(&state); nxp_link_init(&link, &state);
    nxp_diagnostic.marker = UINT32_C(0x4f4b4c43);
    nxp_diagnostic.abi_version = 1;
    nxp_diagnostic.app_start = UINT32_C(0x2000);
    nxp_diagnostic.app_limit = UINT32_C(0x9000);
    nxp_diagnostic.vector_reservation = 192;
    /* These macros may only be set after the installed working application
     * observed this exact part and its resident recovery was qualified. Arm
     * the escape before ROM/peripheral work so an early fault can return. */
    if (NXP_APPROVED_PART_ID &&
        (NXP_BOARD_QUALIFICATIONS & (NXP_QUAL_PART | NXP_QUAL_RECOVERY)) == (NXP_QUAL_PART | NXP_QUAL_RECOVERY))
        recovery_allowed = UINT32_C(0x5245434f);
    /* Startup still has interrupts masked. IAP54 is a read-only ROM operation. */
    ((iap_entry)(uintptr_t)UINT32_C(0x1fff1ff1))(command, result);
    nxp_diagnostic.iap_status = result[0];
    if (!result[0]) { nxp_diagnostic.part_id = result[1]; state.part_id = result[1]; }
    if (result[0] || result[1] != NXP_APPROVED_PART_ID) {
        /* Keep the proven board's escape when an in-app probe fails. */
        if (recovery_allowed) enter_recovery();
    }
    nxp_diagnostic.clock_hz = configured_clock();
    config.observed_part = nxp_diagnostic.part_id; config.approved_part = NXP_APPROVED_PART_ID;
    config.clock_hz = nxp_diagnostic.clock_hz; config.qualifications = NXP_BOARD_QUALIFICATIONS;
    config.spi_mode = NXP_QUALIFIED_SPI_MODE;
    nxp_board_init(&board, &io, &config, &link);
#if NXP_SPI_ONLY_TRIAL
    if (!nxp_board_trial_dark(&board) && recovery_allowed) enter_recovery();
#endif
    nxp_diagnostic.qualification_flags = config.qualifications;
    nxp_diagnostic.reset_status = read_register(0, 0x40048030);
    if (config.approved_part && config.observed_part == config.approved_part &&
        (config.qualifications & (NXP_QUAL_PART | NXP_QUAL_RECOVERY)) == (NXP_QUAL_PART | NXP_QUAL_RECOVERY))
        recovery_allowed = UINT32_C(0x5245434f);
    if (!config.clock_hz && recovery_allowed) enter_recovery();
    if ((nxp_diagnostic.reset_status & (1u << 2)) && recovery_allowed) enter_recovery();
    if (recovery_allowed && config.clock_hz) {
        /* Trial clock is established before any SPI peripheral work. */
        write_register(0, 0xe000e014, 47999); write_register(0, 0xe000e018, 0);
        write_register(0, 0xe000e010, 7);
    }
    service_watchdog();
    if (nxp_board_start_spi(&board)) {
        write_register(0, 0xe000e280, 1u << 14);
        write_register(0, 0xe000e100, 1u << 14);
#if !NXP_SPI_ONLY_TRIAL
        nxp_diagnostic.outputs_enabled = (uint32_t)nxp_board_start_pwm(&board);
        if (!nxp_diagnostic.outputs_enabled && recovery_allowed) enter_recovery();
#endif
        if (!nxp_state_platform(&state, NXP_SPI_ONLY_TRIAL,
            recovery_allowed == UINT32_C(0x5245434f) && config.clock_hz == 48000000u,
            board.pwm_started != 0, nxp_diagnostic.reset_status)) nxp_panic();
#if NXP_PWM_LOW_TRIAL
        if (!nxp_pwm_low_init(&low_trial, &board)) enter_recovery();
#elif NXP_PWM_OFF_TRIAL
        if (!nxp_pwm_off_init(&off_trial, &board)) enter_recovery();
#endif
        __asm volatile ("cpsie i");
        for (;;) {
            uint32_t now = milliseconds;
            if (nxp_trial_expired(&state, now)) enter_recovery();
            service_watchdog();
            __asm volatile ("cpsid i"); nxp_board_poll(&board, now);
#if NXP_PWM_LOW_TRIAL
            nxp_pwm_low_service(&low_trial, &board, now);
#elif NXP_PWM_OFF_TRIAL
            nxp_pwm_off_service(&off_trial, &board, now);
#endif
            __asm volatile ("cpsie i");
            if (link.recovery_ready || nxp_trial_expired(&state, milliseconds)) enter_recovery();
#if !NXP_SPI_ONLY_TRIAL
            if ((uint32_t)(now - last_render) >= 10) {
                nxp_pwm_frame frame; last_render = now;
                nxp_render(&state, board.pwm_started && state.trial_confirmed, &frame);
                if (!nxp_board_apply_pwm(&board, &frame)) {
                    nxp_diagnostic.outputs_enabled = 0;
                    nxp_panic();
                }
            }
#endif
        }
    }
    if (recovery_allowed) enter_recovery();
    /* Default build has no qualification gates and reaches this inert path.
     * It must not be uploaded: SPI/recovery would be unavailable. */
    for (;;) __asm volatile ("wfi");
}
