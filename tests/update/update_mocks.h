#ifndef UPDATE_MOCKS_H
#define UPDATE_MOCKS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define KEYLIGHT_HTTP_INTERNAL_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_CRC 4
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(value) (value)
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define OTA_WITH_SEQUENTIAL_WRITES ((size_t)-2)
#define SOL_SOCKET 1
#define SO_RCVTIMEO 2

typedef int nvs_handle_t;
typedef int esp_ota_handle_t;
typedef unsigned socklen_t;
typedef struct mock_timeval { long tv_sec, tv_usec; } mock_timeval;
#define timeval mock_timeval
typedef void *TaskHandle_t;
typedef struct { uint8_t app_elf_sha256[32]; } esp_app_desc_t;
typedef struct { size_t size; } esp_partition_t;
typedef struct { size_t content_len; } httpd_req_t;
typedef struct { unsigned bytes; } mbedtls_sha256_context;
typedef struct { char name[33]; } app_config;
typedef struct { app_config config; bool updating; } app_context;
typedef struct { bool accepted, rebooting; } cJSON;

extern app_context app;
uint64_t app_now_ms(void);
void app_lock(void);
void app_unlock(void);
void app_mqtt_availability(void);
void app_event_locked(const char *, const char *, const char *);
const char *esp_err_to_name(esp_err_t);
const esp_app_desc_t *esp_app_get_description(void);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskDelay(unsigned);
void vTaskDelete(TaskHandle_t);
unsigned ulTaskNotifyTake(int, unsigned);
void xTaskNotifyGive(TaskHandle_t);
void esp_restart(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
esp_err_t esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
int httpd_req_to_sockfd(httpd_req_t *);
int getsockopt(int, int, int, void *, socklen_t *);
int setsockopt(int, int, int, const void *, socklen_t);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
int httpd_req_recv(httpd_req_t *, char *, size_t);
esp_err_t http_error(httpd_req_t *, int, const char *);
esp_err_t http_json(httpd_req_t *, int, cJSON *);
cJSON *cJSON_CreateObject(void);
void cJSON_AddBoolToObject(cJSON *, const char *, bool);
void mbedtls_sha256_init(mbedtls_sha256_context *);
int mbedtls_sha256_starts(mbedtls_sha256_context *, int);
int mbedtls_sha256_update(mbedtls_sha256_context *, const void *, size_t);
int mbedtls_sha256_finish(mbedtls_sha256_context *, unsigned char *);
void mbedtls_sha256_free(mbedtls_sha256_context *);
#endif
