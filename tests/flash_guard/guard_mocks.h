#ifndef GUARD_MOCKS_H
#define GUARD_MOCKS_H
#include <stdint.h>
typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef void *SemaphoreHandle_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define pdTRUE 1
#ifndef MOCK_TICK_MS
#define MOCK_TICK_MS 1u
#endif
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / MOCK_TICK_MS))
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks);
void xSemaphoreGive(SemaphoreHandle_t handle);
int64_t esp_timer_get_time(void);
#endif
