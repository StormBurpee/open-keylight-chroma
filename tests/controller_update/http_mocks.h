#ifndef CONTROLLER_HTTP_MOCKS_H
#define CONTROLLER_HTTP_MOCKS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYLIGHT_HTTP_INTERNAL_H
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM, ESP_ERR_INVALID_CRC };
#define SOL_SOCKET 1
#define SO_RCVTIMEO 2
typedef unsigned socklen_t;
struct timeval { long tv_sec, tv_usec; };
typedef struct { size_t content_len; } httpd_req_t;
typedef struct { size_t bytes; } mbedtls_sha256_context;
typedef struct { bool accepted; double job_id; } cJSON;

uint64_t app_now_ms(void);
bool app_trial_pending(void);
int app_controller_update_begin(uint32_t *);
int app_controller_update_begin_role(uint32_t *, uint8_t);
int app_controller_update_submit(uint32_t, uint8_t *, size_t);
void app_controller_update_cancel_upload(uint32_t);
int httpd_req_to_sockfd(httpd_req_t *);
int getsockopt(int, int, int, void *, socklen_t *);
int setsockopt(int, int, int, const void *, socklen_t);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
int httpd_req_recv(httpd_req_t *, char *, size_t);
esp_err_t http_error(httpd_req_t *, int, const char *);
esp_err_t http_json(httpd_req_t *, int, cJSON *);
cJSON *cJSON_CreateObject(void);
void cJSON_AddBoolToObject(cJSON *, const char *, bool);
void cJSON_AddNumberToObject(cJSON *, const char *, double);
void mbedtls_sha256_init(mbedtls_sha256_context *);
int mbedtls_sha256_starts(mbedtls_sha256_context *, int);
int mbedtls_sha256_update(mbedtls_sha256_context *, const unsigned char *, size_t);
int mbedtls_sha256_finish(mbedtls_sha256_context *, unsigned char *);
void mbedtls_sha256_free(mbedtls_sha256_context *);
#endif
