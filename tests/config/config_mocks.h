#ifndef CONFIG_MOCKS_H
#define CONFIG_MOCKS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef struct { int method; const char *uri; size_t content_len; } httpd_req_t;
enum { HTTP_GET, HTTP_PATCH, HTTP_POST, HTTP_PUT };
#define pdMS_TO_TICKS(value) (value)
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *handle);
void vTaskDelay(unsigned ticks);
void esp_restart(void);
#endif
