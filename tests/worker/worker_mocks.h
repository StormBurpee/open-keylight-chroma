#ifndef WORKER_MOCKS_H
#define WORKER_MOCKS_H
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef struct cJSON cJSON;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define pdPASS 1
#define pdMS_TO_TICKS(value) (value)
enum { ESP_RST_POWERON = 1, ESP_RST_SW = 3, ESP_RST_BROWNOUT = 9 };
int esp_reset_reason(void);
void vTaskDelay(unsigned ticks);
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *argument,
                unsigned priority, TaskHandle_t *handle);
#endif
