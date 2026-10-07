#include "app.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "keylight_policy.h"
#include "scene_store.h"
#include "flash_guard.h"
#include <stdio.h>
#include <string.h>

static const char *namespace_name = "openkeylight";
_Static_assert(sizeof(app_config) == 407, "config_v1 layout must remain unchanged; use separate versioned keys");
typedef struct {
    uint32_t count;
    uint8_t hashes[4][32];
    char labels[4][33];
} clients_record;

static esp_err_t save_blob(const char *key, const void *value, size_t size) {
    nvs_handle_t handle;
    esp_err_t result = app_flash_guard_enter(app_flash_guard_deadline());
    if (result != ESP_OK) return result;
    result = nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, key, value, size);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    app_flash_guard_leave();
    return result;
}

static bool read_blob(nvs_handle_t handle, const char *key, void *value, size_t size) {
    size_t actual = 0;
    if (nvs_get_blob(handle, key, NULL, &actual) != ESP_OK || actual != size) return false;
    return nvs_get_blob(handle, key, value, &actual) == ESP_OK;
}

esp_err_t app_storage_init(void) {
    app.output_encoding = KL_OUTPUT_SRGB;
    snprintf(app.config.name, sizeof(app.config.name), "Open Keylight");
    snprintf(app.config.role, sizeof(app.config.role), "other");
    esp_err_t result = app_flash_guard_enter(app_flash_guard_deadline());
    if (result != ESP_OK) return result;
    result = nvs_flash_init();
    app_flash_guard_leave();
    if (result != ESP_OK) return result;
    nvs_handle_t handle;
    if (nvs_open(namespace_name, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t encoding;
        if (nvs_get_u8(handle, "out_encoding", &encoding) == ESP_OK && encoding <= KL_OUTPUT_LINEAR)
            app.output_encoding = (kl_output_encoding)encoding;
        app_config loaded;
        if (read_blob(handle, "config_v1", &loaded, sizeof(loaded))) {
            loaded.name[32] = 0; loaded.role[15] = 0; loaded.ssid[32] = 0; loaded.password[64] = 0;
            loaded.mqtt_uri[128] = 0; loaded.mqtt_username[64] = 0; loaded.mqtt_password[64] = 0;
            if (!kl_mqtt_uri_valid(loaded.mqtt_uri, loaded.mqtt_enabled)) {
                loaded.mqtt_enabled = false; loaded.mqtt_uri[0] = 0;
            }
            app.config = loaded;
        }
        clients_record clients;
        if (read_blob(handle, "clients_v1", &clients, sizeof(clients)) && clients.count <= 4) {
            bool valid = true;
            for (unsigned i = 0; i < clients.count; i++)
                valid &= memchr(clients.labels[i], 0, 33) && kl_client_label_valid(clients.labels[i]);
            if (valid) {
                app.token_count = (uint8_t)clients.count;
                memcpy(app.token_hashes, clients.hashes, sizeof(app.token_hashes));
                memcpy(app.client_labels, clients.labels, sizeof(app.client_labels));
            }
        }
        nvs_close(handle);
    }
    result = app_scene_store_init();
    if (!app.config.ssid[0] && nvs_open("nvskvinfo0", NVS_READONLY, &handle) == ESP_OK) {
        size_t ssid_size = sizeof(app.config.ssid), password_size = sizeof(app.config.password);
        esp_err_t ssid_result = nvs_get_str(handle, "w_ssid", app.config.ssid, &ssid_size);
        esp_err_t password_result = nvs_get_str(handle, "w_pwd", app.config.password, &password_size);
        nvs_close(handle);
        /* Import stays in RAM until the owner confirms the new application.
         * Successful import must not hide a scene persistence failure. */
        if (ssid_result != ESP_OK || password_result != ESP_OK || !app.config.ssid[0]) {
            memset(app.config.ssid, 0, sizeof(app.config.ssid));
            memset(app.config.password, 0, sizeof(app.config.password));
        }
    }
    return result;
}

esp_err_t app_config_save(const app_config *config) { return save_blob("config_v1", config, sizeof(*config)); }
esp_err_t app_output_encoding_save(kl_output_encoding encoding) {
    if (encoding != KL_OUTPUT_SRGB && encoding != KL_OUTPUT_LINEAR) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t result = app_flash_guard_enter(app_flash_guard_deadline());
    if (result != ESP_OK) return result;
    result = nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, "out_encoding", (uint8_t)encoding);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    app_flash_guard_leave();
    return result;
}

void app_pair_window(void) {
    app_lock();
    app.pair_until_ms = app_now_ms() + 180000;
    app_event_locked("button", "pairing.opened", "One pairing allowed for three minutes");
    app_unlock();
    app_network_recovery_request();
}

bool app_token_valid(const char *token) {
    if (!token || strlen(token) != 64) return false;
    uint8_t digest[32];
    if (mbedtls_sha256((const unsigned char *)token, 64, digest, 0)) return false;
    unsigned match = 0;
    app_lock();
    for (unsigned i = 0; i < app.token_count; i++) {
        unsigned difference = 0;
        for (unsigned j = 0; j < sizeof(digest); j++) difference |= digest[j] ^ app.token_hashes[i][j];
        match |= difference == 0;
    }
    app_unlock();
    return match != 0;
}

esp_err_t app_issue_token(const char *label, char token[65]) {
    static const char hex[] = "0123456789abcdef";
    if (!kl_client_label_valid(label)) return ESP_ERR_INVALID_ARG;
    app_lock();
    if (app_now_ms() >= app.pair_until_ms || app.token_count >= 4) { app_unlock(); return ESP_ERR_INVALID_STATE; }
    uint8_t random[32], digest[32];
    esp_fill_random(random, sizeof(random));
    for (unsigned i = 0; i < sizeof(random); i++) { token[i * 2] = hex[random[i] >> 4]; token[i * 2 + 1] = hex[random[i] & 15]; }
    token[64] = 0;
    if (mbedtls_sha256((const unsigned char *)token, 64, digest, 0)) {
        memset(token, 0, 65); memset(random, 0, sizeof(random));
        app_unlock(); return ESP_FAIL;
    }
    clients_record clients = {.count = app.token_count + 1};
    memcpy(clients.hashes, app.token_hashes, sizeof(clients.hashes));
    memcpy(clients.labels, app.client_labels, sizeof(clients.labels));
    memcpy(clients.hashes[app.token_count], digest, 32);
    snprintf(clients.labels[app.token_count], 33, "%s", label);
    esp_err_t result = save_blob("clients_v1", &clients, sizeof(clients));
    if (result == ESP_OK) {
        memcpy(app.token_hashes, clients.hashes, sizeof(clients.hashes));
        memcpy(app.client_labels, clients.labels, sizeof(clients.labels));
        app.token_count++;
        app.pair_until_ms = 0;
        app_event_locked("pairing", "client.paired", label);
    }
    if (result != ESP_OK) memset(token, 0, 65);
    app_unlock();
    return result;
}

static void client_id(const uint8_t hash[32], char id[17]) {
    for (unsigned i = 0; i < 8; i++) snprintf(id + i * 2, 3, "%02x", hash[i]);
}

cJSON *app_clients_json(void) {
    cJSON *json = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(json, "clients");
    app_lock();
    for (unsigned i = 0; i < app.token_count; i++) {
        char id[17]; client_id(app.token_hashes[i], id);
        cJSON *client = cJSON_CreateObject();
        cJSON_AddStringToObject(client, "id", id);
        cJSON_AddStringToObject(client, "label", app.client_labels[i]);
        cJSON_AddItemToArray(list, client);
    }
    app_unlock();
    return json;
}

int app_revoke_client(const char *id) {
    if (strlen(id) != 16) return 404;
    app_lock();
    unsigned index = app.token_count;
    for (unsigned i = 0; i < app.token_count; i++) {
        char candidate[17]; client_id(app.token_hashes[i], candidate);
        if (!strcmp(candidate, id)) { index = i; break; }
    }
    if (index == app.token_count) { app_unlock(); return 404; }
    clients_record clients = {.count = app.token_count - 1};
    for (unsigned i = 0, target = 0; i < app.token_count; i++) if (i != index) {
        memcpy(clients.hashes[target], app.token_hashes[i], 32);
        memcpy(clients.labels[target++], app.client_labels[i], 33);
    }
    esp_err_t result = save_blob("clients_v1", &clients, sizeof(clients));
    if (result == ESP_OK) {
        memcpy(app.token_hashes, clients.hashes, sizeof(clients.hashes));
        memcpy(app.client_labels, clients.labels, sizeof(clients.labels));
        app.token_count = (uint8_t)clients.count;
        app_event_locked("api", "client.revoked", id);
    }
    app_unlock();
    return result == ESP_OK ? 200 : 503;
}

esp_err_t app_clear_clients(void) {
    clients_record clients = {0};
    app_lock();
    esp_err_t result = save_blob("clients_v1", &clients, sizeof(clients));
    if (result == ESP_OK) {
        memset(app.token_hashes, 0, sizeof(app.token_hashes));
        memset(app.client_labels, 0, sizeof(app.client_labels));
        app.token_count = 0;
        app_event_locked("button", "clients.cleared", "Physical recovery revoked all clients; settings preserved");
    }
    app_unlock();
    if (result == ESP_OK) app_pair_window();
    return result;
}
