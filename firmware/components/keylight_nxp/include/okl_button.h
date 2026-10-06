#ifndef OKL_BUTTON_H
#define OKL_BUTTON_H

#include <stdint.h>

enum { OKL_BUTTON_NONE=0, OKL_BUTTON_SINGLE=1, OKL_BUTTON_DOUBLE=2, OKL_BUTTON_SETUP=4 };
typedef struct {
    uint64_t last_sample, candidate_since, pressed_since, released_since;
    uint8_t candidate, stable, pending_single, second_press, hold_sent, ignore_until_release;
} okl_button;

/* GPIO34 is active-low: adapter passes pressed=(gpio_get_level(34)==0).
 * Clock is monotonic milliseconds. Call every5..10ms; delayed calls do not
 * invent missed edges. Holding the button during boot is ignored until release.
 * Gestures:30ms debounce,350ms double-press window,3s setup hold. A hold
 * emits once and cancels its short/double gesture. No factory reset action. */
void okl_button_init(okl_button *button, int initially_pressed, uint64_t now_ms);
/* Returns0 for success,-1 for invalid input/backward clock; output unchanged
 * on error. Events are bits, allowing an expired single and new event together. */
int okl_button_sample(okl_button *button, int pressed, uint64_t now_ms, unsigned *events);

#endif
