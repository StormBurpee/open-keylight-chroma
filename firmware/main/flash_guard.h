#ifndef APP_FLASH_GUARD_H
#define APP_FLASH_GUARD_H

#include "esp_err.h"
#include <stdint.h>

/* Serializes complete controller request/reply operations with ESP flash
 * mutations, including NVS allocation/garbage collection. Initialize before
 * storage or the SPI worker. No guard holder on the SPI path may acquire
 * app.mutex: existing NVS writers may enter while holding app.mutex.
 *
 * The absolute deadline bounds admission, not cancellation of a flash write
 * already in progress. Release on every acquired path, after flash/driver
 * calls return and before callbacks which can acquire app.mutex. Nonrecursive. */
#define APP_FLASH_GUARD_WAIT_US UINT64_C(5000000)
esp_err_t app_flash_guard_init(void);
uint64_t app_flash_guard_deadline(void);
esp_err_t app_flash_guard_enter(uint64_t deadline_us);
void app_flash_guard_leave(void);

#endif
