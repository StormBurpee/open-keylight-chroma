#ifndef SERVICE_MOCKS_H
#define SERVICE_MOCKS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_NVS_NOT_FOUND 5
#define ESP_ERROR_CHECK(expression) do { if ((expression) != ESP_OK) abort(); } while (0)
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef void *httpd_handle_t;
typedef int httpd_method_t;
enum { HTTP_GET, HTTP_POST, HTTP_PATCH, HTTP_PUT, HTTP_DELETE };
#define HTTPD_RESP_USE_STRLEN -1
typedef struct { httpd_handle_t handle; const char *uri; httpd_method_t method; size_t content_len; } httpd_req_t;
typedef struct { const char *uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
typedef struct { unsigned stack_size, max_uri_handlers, recv_wait_timeout, send_wait_timeout; bool lru_purge_enable; void *uri_match_fn; } httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define httpd_uri_match_wildcard ((void *)1)
typedef struct { char version[32]; uint8_t app_elf_sha256[32]; } esp_app_desc_t;
#ifndef NETWORK_TEST
typedef struct { const char *path, *content_type; const unsigned char *data; unsigned size; } web_asset;
static const unsigned char mock_html[] = "html";
static const web_asset web_assets[] = {{"/index.html", "text/html", mock_html, 4}};
#define WEB_ASSET_COUNT 1
#endif
esp_err_t nvs_flash_init(void);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
void esp_fill_random(void *, size_t);
int mbedtls_sha256(const unsigned char *, size_t, unsigned char *, int);
const esp_app_desc_t *esp_app_get_description(void);
unsigned esp_get_free_heap_size(void);
unsigned esp_reset_reason(void);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
int httpd_req_recv(httpd_req_t *, char *, size_t);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, int);
int httpd_req_to_sockfd(httpd_req_t *);
esp_err_t httpd_sess_trigger_close(httpd_handle_t, int);
esp_err_t httpd_start(httpd_handle_t *, const httpd_config_t *);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
#endif
