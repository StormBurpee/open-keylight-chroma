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
void vTaskDelay(unsigned ticks);
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *argument,
                unsigned priority, TaskHandle_t *handle);
#endif
