#ifndef KEYLIGHT_APP_H
#define KEYLIGHT_APP_H

#include "keylight_core.h"
#include "keylight_json.h"
#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stddef.h>

#define KL_SCENES 8
#define KL_HISTORY 32
#define KL_TOKEN_BYTES 32

typedef struct { bool used; char name[33]; kl_state state; } app_scene;
typedef struct {
    uint32_t sequence;
    uint64_t uptime_ms;
    char actor[17], event[25], detail[81];
} app_event;
typedef struct {
    char name[33], role[16], ssid[33], password[65];
    bool mqtt_enabled;
    char mqtt_uri[129], mqtt_username[65], mqtt_password[65];
} app_config;
typedef struct {
    SemaphoreHandle_t mutex;
    kl_state desired, reported;
    uint32_t revision, output_revision, completed_revision;
    uint32_t reported_revision, reported_fields;
    bool reported_valid, rgb_confirmed, controller_connected, network_connected;
    bool controller_ready, controller_trial_confirmed;
    uint32_t controller_part_id;
    uint64_t controller_last_health_ms;
    char controller_backend[12], controller_status[16];
    bool updating;
    char operation[12], error[81], actor[17], controller_version[20];
    char id[24], hostname[40], ip[16];
    uint8_t mac[6];
    int rssi;
    app_config config;
    kl_output_encoding output_encoding; /* Separate NVS key; never changes config_v1 layout. */
    app_scene scenes[KL_SCENES];
    app_event history[KL_HISTORY];
    uint32_t history_sequence;
    uint64_t pair_until_ms;
    uint8_t token_hashes[4][32];
    char client_labels[4][33];
    uint8_t token_count;
    bool mqtt_connected;
} app_context;

extern app_context app;
uint64_t app_now_ms(void);
void app_lock(void);
void app_unlock(void);
void app_event_locked(const char *actor, const char *event, const char *detail);
void app_pair_window(void);
bool app_token_valid(const char *token);
esp_err_t app_issue_token(const char *label, char token[65]);
cJSON *app_clients_json(void);
int app_revoke_client(const char *id);
esp_err_t app_clear_clients(void);
esp_err_t app_storage_init(void);
esp_err_t app_config_save(const app_config *config);
esp_err_t app_output_encoding_save(kl_output_encoding encoding);
esp_err_t app_scene_save(unsigned index, const app_scene *scene);
esp_err_t app_network_start(void);
void app_network_recovery_request(void);
esp_err_t app_http_start(void);
esp_err_t app_worker_start(void);
void app_mqtt_start(void);
void app_mqtt_publish(void);
void app_button_start(void);
/* Receiving reservations own app.updating. Submit transfers the immutable
 * package only on 202; the caller frees it on all other outcomes. */
int app_controller_update_begin(uint32_t *job_id);
int app_controller_update_begin_role(uint32_t *job_id, uint8_t role);
int app_controller_update_begin_mode(uint32_t *job_id, uint8_t diagnostic_profile);
int app_controller_recovery_begin(uint32_t expected_job_id, bool power_cycle_acknowledged);
int app_controller_update_submit(uint32_t job_id, uint8_t *package, size_t size);
void app_controller_update_cancel_upload(uint32_t job_id);
cJSON *app_controller_update_json(void);

/* Caller owns returned JSON. submit is atomic and returns an HTTP-style result. */
cJSON *app_state_json(void);
cJSON *kl_json_light(const kl_state *state);
bool kl_json_parse_patch(const cJSON *json, kl_patch *patch, uint32_t *expected, bool *has_expected);
int app_submit(const kl_patch *patch, const char *actor, uint32_t expected, bool has_expected);
int app_activate_scene(unsigned index, const char *actor, uint32_t expected, bool has_expected);

#endif
