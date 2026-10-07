#include "flash_guard.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t guard;

esp_err_t app_flash_guard_init(void) {
    /* Startup only, before concurrent callers exist. A second initialization
     * must not replace a mutex which may already protect a transaction. */
    if (guard) return ESP_OK;
    guard = xSemaphoreCreateMutex();
    return guard ? ESP_OK : ESP_ERR_NO_MEM;
}

uint64_t app_flash_guard_deadline(void) {
    uint64_t now = (uint64_t)esp_timer_get_time();
    return now > UINT64_MAX - APP_FLASH_GUARD_WAIT_US ? UINT64_MAX : now + APP_FLASH_GUARD_WAIT_US;
}

esp_err_t app_flash_guard_enter(uint64_t deadline_us) {
    if (!guard) return ESP_ERR_INVALID_STATE;
    uint64_t now = (uint64_t)esp_timer_get_time();
    if (now >= deadline_us) return ESP_ERR_TIMEOUT;
    uint64_t remaining = deadline_us - now;
    if (remaining > APP_FLASH_GUARD_WAIT_US) remaining = APP_FLASH_GUARD_WAIT_US;
    deadline_us = now + remaining;
    TickType_t ticks = pdMS_TO_TICKS((remaining + 999) / 1000);
    if (!ticks) ticks = 1;
    if (xSemaphoreTake(guard, ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    if ((uint64_t)esp_timer_get_time() >= deadline_us) {
        xSemaphoreGive(guard);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void app_flash_guard_leave(void) { xSemaphoreGive(guard); }
