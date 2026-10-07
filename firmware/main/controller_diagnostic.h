#ifndef KEYLIGHT_CONTROLLER_DIAGNOSTIC_H
#define KEYLIGHT_CONTROLLER_DIAGNOSTIC_H
#include <stdbool.h>
#include <stdint.h>
#include "okl_nxp.h"

enum { APP_DIAGNOSTIC_WORDS = 224, APP_DIAGNOSTIC_PAGES = 14 };
/* Fixed OFF1 ABI. These predicates verify captured registers and software
 * timing, never optical darkness or the safety of nonzero PWM. */
static inline bool app_diagnostic_profile(const uint32_t *w) {
    return w && w[0] == UINT32_C(0x4f464631) && w[1] == 1 &&
        w[2] <= 3 && w[7] <= 1 && w[8] <= 6 && !(w[10] & ~31u) &&
        w[13] == 0x193 && w[14] == 30000 && w[15] != 0;
}
static inline bool app_diagnostic_initial(const uint32_t *w) {
    if (!app_diagnostic_profile(w) || w[2] || w[7]) return false;
    for (unsigned i = 3; i < 13; ++i) if (w[i]) return false;
    return true;
}
static inline bool app_diagnostic_registers(const uint32_t *w, uint32_t generation) {
    if (!app_diagnostic_profile(w) || w[15] != generation || w[2] != 2 ||
        w[7] != 1 || w[8] != 1 || w[9] || w[10] != 31 || w[11] != 2 || w[12] != 2 ||
        w[3] >= 28000 || w[4] != w[3] || w[6] != w[4] + 400 ||
        w[5] < w[4] + 400 || w[5] > w[4] + 500) return false;
    for (unsigned i = 216; i < APP_DIAGNOSTIC_WORDS; ++i) if (w[i]) return false;
    const uint32_t pins = (1u << 13) | (1u << 14) | (1u << 16) | (1u << 18) | (1u << 19);
    const uint32_t gpio[5] = {0x81, 0x81, 0x80, 0, 0}, pwm[5] = {0x83, 0x83, 0x82, 2, 2};
    const uint32_t states[5] = {0, 0, 1, 2, 2};
    for (unsigned i = 0; i < 5; ++i) {
        const uint32_t *s = w + 16 + i * 40;
        if (s[0] != w[i < 3 ? 4 : 5] || s[1] != (i < 3 ? 0u : 1u) ||
            (s[2] & 0x600) != 0x600 || (s[3] & pins) != pins || (s[4] & pins) ||
            s[38] != states[i] || s[39]) return false;
        for (unsigned p = 0; p < 5; ++p)
            if ((s[5 + p] & (p < 3 ? 0x87u : 7u)) != (i == 2 ? pwm[p] : gpio[p])) return false;
        if (!i) continue; /* Record inherited timer state without assuming it. */
        for (unsigned c = 0; c < 2; ++c) {
            const uint32_t *t = s + (c ? 24 : 10);
            uint32_t period = c ? 25499 : 254;
            if (t[0] != (i == 4 ? 2u : 1u) || t[2] != (c ? 0u : 47u) ||
                t[4] != 0x80 || t[5] != period + 1 || t[6] != period + 1 ||
                t[7] != period || t[8] != period + 1 || t[9] || t[10] || t[11] ||
                t[12] != (c ? 11u : 3u) || (i == 4 && (t[1] || t[3]))) return false;
        }
    }
    return true;
}

enum { APP_LOW_DIAGNOSTIC_WORDS = 256, APP_LOW_DIAGNOSTIC_PAGES = 16 };
/* LOW1 is a bounded experimental profile, never a lighting-capable image.
 * These checks establish only its protocol/register record. They cannot
 * establish optical polarity, current, temperature or production readiness. */
static inline bool app_low_diagnostic_identity(const okl_controller_status *s,
                                               const okl_firmware_version *v) {
    return s && v && !v->component[0] && v->component[1] == 1 && !v->component[2] && !v->component[3] &&
        s->abi_major == 1 && !s->abi_minor && s->role == OKL_ROLE_SPI_DIAGNOSTIC &&
        s->capabilities == OKL_CAP_RECOVERY_READY && s->part_id == 0xbc40 &&
        !s->trial_confirmed && !s->boot_requested && s->uptime_ms < 20000;
}
static inline bool app_low_diagnostic_profile(const uint32_t *w) {
    return w && w[0] == UINT32_C(0x4c4f5731) && w[1] == 1 && w[2] <= 3 &&
        w[6] <= 1 && w[7] <= 7 && !(w[9] & ~31u) && w[10] == 0x393 &&
        w[11] == 30000 && w[12] && w[13] == 100 && w[14] == 1500 && w[15] == 5;
}
static inline bool app_low_diagnostic_initial(const uint32_t *w) {
    if (!app_low_diagnostic_profile(w) || w[2]) return false;
    for (unsigned i = 3; i < 10; ++i) if (w[i]) return false;
    return true;
}
static inline bool app_low_diagnostic_registers(const uint32_t *w, uint32_t generation) {
    const uint32_t pins = (1u << 13) | (1u << 14) | (1u << 16) | (1u << 18) | (1u << 19);
    const uint32_t channel_pin[5] = {1u << 16, 1u << 14, 1u << 13, 1u << 19, 1u << 18};
    const uint32_t gpio[5] = {0x81, 0x81, 0x80, 0, 0}, pwm[5] = {0x83, 0x83, 0x82, 2, 2};
    if (!app_low_diagnostic_profile(w) || w[12] != generation || w[2] != 2 ||
        w[3] >= 22000 || w[4] != w[3] + 6500 || w[5] != w[4] ||
        w[6] != 1 || w[7] != 1 || w[8] || w[9] != 31) return false;
    const uint32_t *b = w + 16;
    if (b[0] != w[3] || b[1] || (b[2] & 0x600) != 0x600 ||
        (b[3] & pins) != pins || (b[4] & pins) || b[38] || b[39]) return false;
    for (unsigned p = 0; p < 5; ++p)
        if ((b[5 + p] & (p < 3 ? 0x87u : 7u)) != gpio[p]) return false;
    for (unsigned c = 0; c < 2; ++c) {
        const uint32_t *t = b + (c ? 24 : 10);
        uint32_t period = c ? 25499 : 254;
        if (t[0] != 1 || t[2] != (c ? 0u : 47u) || t[4] != 0x80 ||
            t[5] != period + 1 || t[6] != period + 1 || t[7] != period ||
            t[8] != period + 1 || t[9] || t[10] || t[11] || t[12] != (c ? 11u : 3u)) return false;
    }
    for (unsigned c = 0; c < 5; ++c) {
        const uint32_t *s = w + 56 + c * 40;
        uint32_t start = w[3] + c * 1600;
        if (s[0] != c || s[1] != start || s[2] != start + 100 || s[3] != s[2] ||
            s[4] != 1 || s[5] || (s[6] & 0x600) != 0x600 || (s[7] & pins) != pins ||
            (s[8] & (pins & ~channel_pin[c])) || s[14] != 1 || s[15] != 1 ||
            s[21] != 254 || s[22] != 25499 || s[23] != 3 || s[24] != 11 ||
            (s[25] & pins) != pins || (s[26] & pins) || s[32] != 2 || s[33] != 2 || s[39] != 1) return false;
        for (unsigned p = 0; p < 5; ++p) {
            uint32_t mask = p < 3 ? 0x87u : 7u;
            if ((s[9 + p] & mask) != pwm[p] || (s[27 + p] & mask) != gpio[p]) return false;
            if (s[16 + p] != (p == c ? (p < 3 ? 25245u : 253u) : (p < 3 ? 25500u : 255u)) ||
                s[34 + p] != (p < 3 ? 25500u : 255u)) return false;
        }
    }
    return true;
}
#endif
