#ifndef KEYLIGHT_HTTP_INTERNAL_H
#define KEYLIGHT_HTTP_INTERNAL_H
#include "app.h"
#include "esp_http_server.h"
cJSON *http_read_json(httpd_req_t *request);
esp_err_t http_json(httpd_req_t *request, int status, cJSON *json);
esp_err_t http_error(httpd_req_t *request, int status, const char *message);
esp_err_t http_settings(httpd_req_t *request);
esp_err_t http_scenes(httpd_req_t *request);
esp_err_t http_update(httpd_req_t *request);
esp_err_t app_trial_confirm(void);
void app_trial_start(void);
bool app_trial_pending(void);
#endif
