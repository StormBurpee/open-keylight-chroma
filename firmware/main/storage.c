#include "app.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "keylight_policy.h"
#include "scene_store.h"
#include "flash_guard.h"
#include "esp_partition.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *namespace_name = "openkeylight";
_Static_assert(sizeof(app_config) == 407, "config_v1 layout must remain unchanged; use separate versioned keys");
typedef struct {
    uint32_t count;
    uint8_t hashes[4][32];
    char labels[4][33];
} clients_record;
_Static_assert(sizeof(clients_record) == 264, "clients_v1 must retain its original layout");

/* v1 remains untouched for old firmware. A present v2 is authoritative, even
 * when unreadable: falling back could revive a revoked legacy credential.
 * The first successful mutation migrates a valid v1 collection in one blob. */
enum { CLIENT_HEADER_BYTES = 16, CLIENT_ENTRY_BYTES = 65 };
static bool clients_writable;
static bool clients_auth_uncertain;

static size_t clients_storage_bytes(void) {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    return partition ? partition->size : 0;
}
static uint32_t clients_get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void clients_put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}
static size_t clients_record_bytes(size_t count) {
    size_t available = clients_storage_bytes();
    if (available < CLIENT_HEADER_BYTES || count > UINT32_MAX
        || count > (available - CLIENT_HEADER_BYTES) / CLIENT_ENTRY_BYTES) return 0;
    return CLIENT_HEADER_BYTES + count * CLIENT_ENTRY_BYTES;
}
static bool clients_valid(const app_client *clients, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const char *end = memchr(clients[i].label, 0, sizeof(clients[i].label));
        if (!end || !kl_client_label_valid(clients[i].label)) return false;
        for (const char *p = end + 1; p < clients[i].label + sizeof(clients[i].label); ++p)
            if (*p) return false;
        /* IDs use the first eight hash bytes; disallow ambiguous revocation. */
        for (size_t j = 0; j < i; ++j) if (!memcmp(clients[i].hash, clients[j].hash, 8)) return false;
    }
    return true;
}
static esp_err_t clients_load(nvs_handle_t handle) {
    size_t size = 0;
    esp_err_t result = nvs_get_blob(handle, "clients_v2", NULL, &size);
    app_client *loaded = NULL; size_t count = 0;
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        clients_record legacy;
        result = nvs_get_blob(handle, "clients_v1", NULL, &size);
        if (result == ESP_ERR_NVS_NOT_FOUND) { clients_writable = true; return ESP_OK; }
        if (result != ESP_OK) return result;
        if (size != sizeof(legacy)) return ESP_ERR_INVALID_STATE;
        result = nvs_get_blob(handle, "clients_v1", &legacy, &size);
        if (result != ESP_OK) return result;
        if (size != sizeof(legacy) || legacy.count > 4) return ESP_ERR_INVALID_STATE;
        count = legacy.count;
        loaded = count ? calloc(count, sizeof(*loaded)) : NULL;
        if (count && !loaded) return ESP_ERR_NO_MEM;
        for (size_t i = 0; i < count; ++i) {
            /* v1 allowed arbitrary bytes after the first NUL. Preserve the
             * accepted label, then canonicalize its unused padding for v2. */
            if (!memchr(legacy.labels[i], 0, 33) || !kl_client_label_valid(legacy.labels[i])) {
                free(loaded); return ESP_ERR_INVALID_STATE;
            }
            memcpy(loaded[i].hash, legacy.hashes[i], 32);
            snprintf(loaded[i].label, 33, "%s", legacy.labels[i]);
        }
    } else {
        if (result != ESP_OK) return result;
        /* Bound the persisted length before allocation, independently of its
         * claimed count. No artificial client-count limit replaces four. */
        if (size < CLIENT_HEADER_BYTES || size > clients_storage_bytes()
            || (size - CLIENT_HEADER_BYTES) % CLIENT_ENTRY_BYTES) return ESP_ERR_INVALID_STATE;
        uint8_t *raw = malloc(size);
        if (!raw) return ESP_ERR_NO_MEM;
        size_t actual = size;
        result = nvs_get_blob(handle, "clients_v2", raw, &actual);
        static const uint8_t prefix[8] = {'O','K','C','L',2,0,CLIENT_ENTRY_BYTES,0};
        count = (size - CLIENT_HEADER_BYTES) / CLIENT_ENTRY_BYTES;
        if (result == ESP_OK && (actual != size || memcmp(raw, prefix, sizeof(prefix))
            || clients_get32(raw + 8) != count || clients_get32(raw + 12))) result = ESP_ERR_INVALID_STATE;
        if (result == ESP_OK && count) {
            loaded = calloc(count, sizeof(*loaded));
            if (!loaded) result = ESP_ERR_NO_MEM;
            else for (size_t i = 0; i < count; ++i) {
                memcpy(loaded[i].hash, raw + CLIENT_HEADER_BYTES + i * CLIENT_ENTRY_BYTES, 32);
                memcpy(loaded[i].label, raw + CLIENT_HEADER_BYTES + i * CLIENT_ENTRY_BYTES + 32, 33);
            }
        }
        free(raw);
        if (result != ESP_OK) { free(loaded); return result; }
    }
    if (!clients_valid(loaded, count)) { free(loaded); return ESP_ERR_INVALID_STATE; }
    app.clients = loaded; app.token_count = count; clients_writable = true;
    return ESP_OK;
}

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

static esp_err_t clients_persist(const app_client *clients, size_t count, bool removes_clients) {
    size_t size = clients_record_bytes(count);
    if (!size) return ESP_ERR_NVS_VALUE_TOO_LONG;
    uint8_t *raw = calloc(1, size);
    if (!raw) return ESP_ERR_NO_MEM;
    memcpy(raw, "OKCL", 4); raw[4] = 2; raw[6] = CLIENT_ENTRY_BYTES;
    clients_put32(raw + 8, (uint32_t)count);
    for (size_t i = 0; i < count; ++i) {
        memcpy(raw + CLIENT_HEADER_BYTES + i * CLIENT_ENTRY_BYTES, clients[i].hash, 32);
        memcpy(raw + CLIENT_HEADER_BYTES + i * CLIENT_ENTRY_BYTES + 32, clients[i].label, 33);
    }
    nvs_handle_t handle;
    esp_err_t result = app_flash_guard_enter(app_flash_guard_deadline());
    if (result == ESP_OK) {
        result = nvs_open(namespace_name, NVS_READWRITE, &handle);
        if (result == ESP_OK) {
            result = nvs_set_blob(handle, "clients_v2", raw, size);
            if (result == ESP_OK) result = nvs_commit(handle);
            /* NVS writes during set_blob, not only commit. An error can follow
             * durable publication: retain RAM credentials but quarantine later
             * edits until reload or explicit physical clear reconciles them. */
            if (result != ESP_OK) {
                clients_writable = false;
                clients_auth_uncertain |= removes_clients;
            }
            nvs_close(handle);
        }
        app_flash_guard_leave();
    }
    free(raw);
    return result;
}

esp_err_t app_storage_init(void) {
    free(app.clients); app.clients = NULL; app.token_count = 0;
    clients_writable = false; clients_auth_uncertain = false;
    app.output_encoding = KL_OUTPUT_SRGB;
    snprintf(app.config.name, sizeof(app.config.name), "Open Keylight");
    snprintf(app.config.role, sizeof(app.config.role), "other");
    esp_err_t result = app_flash_guard_enter(app_flash_guard_deadline());
    if (result != ESP_OK) return result;
    result = nvs_flash_init();
    app_flash_guard_leave();
    if (result != ESP_OK) return result;
    nvs_handle_t handle;
    esp_err_t clients_result = nvs_open(namespace_name, NVS_READONLY, &handle);
    if (clients_result == ESP_ERR_NVS_NOT_FOUND) { clients_writable = true; clients_result = ESP_OK; }
    else if (clients_result == ESP_OK) {
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
        clients_result = clients_load(handle);
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
    return clients_result == ESP_OK ? result : clients_result;
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
    for (size_t i = 0; !clients_auth_uncertain && i < app.token_count; i++) {
        unsigned difference = 0;
        for (unsigned j = 0; j < sizeof(digest); j++) difference |= digest[j] ^ app.clients[i].hash[j];
        match |= difference == 0;
    }
    app_unlock();
    return match != 0;
}

esp_err_t app_issue_token(const char *label, char token[65]) {
    static const char hex[] = "0123456789abcdef";
    if (!token) return ESP_ERR_INVALID_ARG;
    memset(token, 0, 65);
    if (!kl_client_label_valid(label)) return ESP_ERR_INVALID_ARG;
    app_lock();
    if (app_now_ms() >= app.pair_until_ms) { app_unlock(); return ESP_ERR_INVALID_STATE; }
    if (!clients_writable) { app_unlock(); return ESP_FAIL; }
    if (app.token_count == SIZE_MAX || !clients_record_bytes(app.token_count + 1)) {
        app_unlock(); return ESP_ERR_NVS_VALUE_TOO_LONG;
    }
    app_client *clients = calloc(app.token_count + 1, sizeof(*clients));
    if (!clients) { app_unlock(); return ESP_ERR_NO_MEM; }
    if (app.token_count) memcpy(clients, app.clients, app.token_count * sizeof(*clients));
    uint8_t random[32], digest[32];
    esp_fill_random(random, sizeof(random));
    for (unsigned i = 0; i < sizeof(random); i++) { token[i * 2] = hex[random[i] >> 4]; token[i * 2 + 1] = hex[random[i] & 15]; }
    token[64] = 0;
    if (mbedtls_sha256((const unsigned char *)token, 64, digest, 0)) {
        memset(token, 0, 65); memset(random, 0, sizeof(random));
        free(clients); app_unlock(); return ESP_FAIL;
    }
    memcpy(clients[app.token_count].hash, digest, 32);
    snprintf(clients[app.token_count].label, 33, "%s", label);
    esp_err_t result = clients_valid(clients, app.token_count + 1)
        ? clients_persist(clients, app.token_count + 1, false) : ESP_FAIL;
    if (result == ESP_OK) {
        free(app.clients); app.clients = clients; clients = NULL;
        app.token_count++;
        app.pair_until_ms = 0;
        app_event_locked("pairing", "client.paired", label);
    }
    free(clients); memset(random, 0, sizeof(random)); memset(digest, 0, sizeof(digest));
    if (result != ESP_OK) memset(token, 0, 65);
    app_unlock();
    return result;
}

static void client_id(const uint8_t hash[32], char id[17]) {
    for (unsigned i = 0; i < 8; i++) snprintf(id + i * 2, 3, "%02x", hash[i]);
}

cJSON *app_clients_json(void) {
    cJSON *json = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(json, "clients");
    if (!json || !list) { cJSON_Delete(json); return NULL; }
    app_lock();
    if (!clients_writable) { app_unlock(); cJSON_Delete(json); return NULL; }
    for (size_t i = 0; i < app.token_count; i++) {
        char id[17]; client_id(app.clients[i].hash, id);
        cJSON *client = cJSON_CreateObject();
        if (!client || !cJSON_AddStringToObject(client, "id", id)
            || !cJSON_AddStringToObject(client, "label", app.clients[i].label)
            || !cJSON_AddItemToArray(list, client)) {
            cJSON_Delete(client); cJSON_Delete(json); json = NULL; break;
        }
    }
    app_unlock();
    return json;
}

int app_revoke_client(const char *id) {
    if (!id || strlen(id) != 16) return 404;
    app_lock();
    if (!clients_writable) { app_unlock(); return 503; }
    size_t index = app.token_count;
    for (size_t i = 0; i < app.token_count; i++) {
        char candidate[17]; client_id(app.clients[i].hash, candidate);
        if (!strcmp(candidate, id)) { index = i; break; }
    }
    if (index == app.token_count) { app_unlock(); return 404; }
    size_t count = app.token_count - 1;
    app_client *clients = count ? calloc(count, sizeof(*clients)) : NULL;
    if (count && !clients) { app_unlock(); return 503; }
    for (size_t i = 0, target = 0; i < app.token_count; i++) if (i != index) {
        clients[target++] = app.clients[i];
    }
    esp_err_t result = clients_persist(clients, count, true);
    if (result == ESP_OK) {
        free(app.clients); app.clients = clients; clients = NULL;
        app.token_count = count;
        app_event_locked("api", "client.revoked", id);
    }
    free(clients);
    app_unlock();
    return result == ESP_OK ? 200 : 503;
}

esp_err_t app_clear_clients(void) {
    app_lock();
    esp_err_t result = clients_persist(NULL, 0, true);
    if (result == ESP_OK) {
        free(app.clients); app.clients = NULL; app.token_count = 0;
        clients_writable = true; clients_auth_uncertain = false;
        app_event_locked("button", "clients.cleared", "Physical recovery revoked all clients; settings preserved");
    }
    app_unlock();
    if (result == ESP_OK) app_pair_window();
    return result;
}
