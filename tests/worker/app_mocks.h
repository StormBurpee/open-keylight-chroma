#ifndef APP_MOCKS_H
#define APP_MOCKS_H
#include "worker_mocks.h"
#include <stdbool.h>
#include <stdlib.h>
#define KEYLIGHT_HTTP_INTERNAL_H
#define ESP_MAC_WIFI_STA 0
#define ESP_FAIL -1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define ESP_ERROR_CHECK(value) do { if ((value) != ESP_OK) abort(); } while (0)
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
void mock_log(const char *, const char *, ...);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
void xSemaphoreGive(SemaphoreHandle_t);
int64_t esp_timer_get_time(void);
void esp_read_mac(uint8_t *, unsigned);
const char *esp_err_to_name(esp_err_t);
void app_trial_start(void);
#endif
