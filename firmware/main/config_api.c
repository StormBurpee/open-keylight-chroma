#include "http_internal.h"
#include "keylight_policy.h"
#include "esp_system.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static bool copy_string(char *destination, size_t capacity, const cJSON *value, bool empty) {
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= capacity || (!empty && !value->valuestring[0])) return false;
    for (const unsigned char *p = (unsigned char *)value->valuestring; *p; p++) if (*p < 32 || *p == 127) return false;
    snprintf(destination, capacity, "%s", value->valuestring); return true;
}
static bool unique(const cJSON *object) {
    for (const cJSON *a = object->child; a; a = a->next)
        for (const cJSON *b = a->next; b; b = b->next) if (!strcmp(a->string, b->string)) return false;
    return true;
}
static cJSON *settings_json(void) {
    cJSON *json = cJSON_CreateObject();
    app_lock();
    cJSON_AddStringToObject(json, "name", app.config.name); cJSON_AddStringToObject(json, "role", app.config.role);
    cJSON_AddStringToObject(json, "output_encoding", app.output_encoding == KL_OUTPUT_LINEAR ? "linear" : "srgb");
    cJSON *mqtt = cJSON_AddObjectToObject(json, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", app.config.mqtt_enabled); cJSON_AddStringToObject(mqtt, "uri", app.config.mqtt_uri);
    cJSON_AddStringToObject(mqtt, "username", app.config.mqtt_username); cJSON_AddBoolToObject(mqtt, "connected", app.mqtt_connected);
    app_unlock();
    cJSON *button = cJSON_AddObjectToObject(json, "button");
    cJSON_AddStringToObject(button, "single", "toggle"); cJSON_AddStringToObject(button, "double", "next_scene"); cJSON_AddStringToObject(button, "hold", "pair");
    return json;
}
static void restart_after_config(void *unused) { (void)unused; vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }

esp_err_t http_settings(httpd_req_t *request) {
    if (request->method == HTTP_GET) return http_json(request, 200, settings_json());
    if (request->method != HTTP_PATCH) return http_error(request, 405, "Use GET or PATCH");
    cJSON *json = http_read_json(request);
    if (!cJSON_IsObject(json) || !unique(json) || !json->child) { cJSON_Delete(json); return http_error(request, 400, "Invalid settings object"); }
    cJSON *encoding = cJSON_GetObjectItemCaseSensitive(json, "output_encoding");
    if (encoding) {
        bool valid = cJSON_GetArraySize(json) == 1 && cJSON_IsString(encoding) &&
            (!strcmp(encoding->valuestring, "srgb") || !strcmp(encoding->valuestring, "linear"));
        if (!valid) { cJSON_Delete(json); return http_error(request, 400, "Send only output_encoding: srgb or linear"); }
        kl_output_encoding next = !strcmp(encoding->valuestring, "linear") ? KL_OUTPUT_LINEAR : KL_OUTPUT_SRGB;
        cJSON_Delete(json);
        /* One separate NVS record, serialized with state changes and update
         * admission. No config_v1 migration or multi-record atomicity claim. */
        app_lock();
        int status = 200;
        bool changed = next != app.output_encoding;
        if (app.updating) status = 409;
        else if (changed && app.desired.recording_lock) status = 423;
        else if (changed && !app.controller_ready) status = 503;
        else if (changed && app_output_encoding_save(next) != ESP_OK) status = 503;
        if (status == 200 && changed) {
            app.output_encoding = next; ++app.revision; ++app.output_revision;
            app.reported_valid = false; app.rgb_confirmed = false; app.reported_fields = 0;
            snprintf(app.operation, sizeof(app.operation), "pending"); app.error[0] = 0;
            snprintf(app.actor, sizeof(app.actor), "dashboard");
            app_event_locked("dashboard", "encoding.saved", next == KL_OUTPUT_SRGB ? "sRGB output encoding" : "Linear output encoding");
        }
        app_unlock();
        if (status != 200) return http_error(request, status, status == 423 ?
            "Recording Lock is on; unlock before changing output encoding" : status == 409 ?
            "Update in progress" : "Output encoding could not be applied or persisted");
        if (changed) app_mqtt_publish();
        return http_json(request, 200, settings_json());
    }
    app_lock(); app_config config = app.config; bool updating = app.updating; app_unlock();
    if (updating) { cJSON_Delete(json); return http_error(request, 409, "Update in progress"); }
    bool valid = true, restart = false;
    const cJSON *item;
    cJSON_ArrayForEach(item, json) {
        if (!strcmp(item->string, "name")) valid &= copy_string(config.name, sizeof(config.name), item, false);
        else if (!strcmp(item->string, "role")) {
            valid &= copy_string(config.role, sizeof(config.role), item, false);
            valid &= !strcmp(config.role, "key") || !strcmp(config.role, "fill") || !strcmp(config.role, "background") || !strcmp(config.role, "other");
        } else if (!strcmp(item->string, "ssid")) { valid &= copy_string(config.ssid, sizeof(config.ssid), item, false); restart = true; }
        else if (!strcmp(item->string, "password")) { valid &= copy_string(config.password, sizeof(config.password), item, true); restart = true; }
        else if (!strcmp(item->string, "mqtt")) {
            if (!cJSON_IsObject(item) || !unique(item)) { valid = false; break; }
            const cJSON *field;
            cJSON_ArrayForEach(field, item) {
                if (!strcmp(field->string, "enabled") && cJSON_IsBool(field)) config.mqtt_enabled = cJSON_IsTrue(field);
                else if (!strcmp(field->string, "uri")) valid &= copy_string(config.mqtt_uri, sizeof(config.mqtt_uri), field, true);
                else if (!strcmp(field->string, "username")) valid &= copy_string(config.mqtt_username, sizeof(config.mqtt_username), field, true);
                else if (!strcmp(field->string, "password")) valid &= copy_string(config.mqtt_password, sizeof(config.mqtt_password), field, true);
                else valid = false;
            }
            restart = true;
        } else valid = false;
    }
    cJSON_Delete(json);
    valid &= kl_mqtt_uri_valid(config.mqtt_uri, config.mqtt_enabled);
    if (config.password[0] && strlen(config.password) < 8) valid = false;
    if (!valid) return http_error(request, 400, "Invalid settings; no changes saved");
    if (app_config_save(&config) != ESP_OK) return http_error(request, 503, "Settings could not be persisted");
    app_lock(); app.config = config; app_event_locked("dashboard", "settings.saved", restart ? "Network changes saved; restarting" : "Device settings saved"); app_unlock();
    esp_err_t result = http_json(request, 200, settings_json());
    if (restart) xTaskCreate(restart_after_config, "config_reboot", 2048, NULL, 4, NULL);
    return result;
}

esp_err_t http_scenes(httpd_req_t *request) {
    if (!strcmp(request->uri, "/api/v1/scenes") && request->method == HTTP_GET) {
        cJSON *json = cJSON_CreateObject(), *scenes = cJSON_AddArrayToObject(json, "scenes");
        app_lock();
        for (unsigned i = 0; i < KL_SCENES; i++) if (app.scenes[i].used) {
            cJSON *scene = cJSON_CreateObject(); cJSON_AddNumberToObject(scene, "id", i + 1);
            cJSON_AddStringToObject(scene, "name", app.scenes[i].name);
            cJSON_AddItemToObject(scene, "state", kl_json_light(&app.scenes[i].state)); cJSON_AddItemToArray(scenes, scene);
        }
        app_unlock(); return http_json(request, 200, json);
    }
    if (strncmp(request->uri, "/api/v1/scenes/", 15) || request->uri[15] < '1' || request->uri[15] > '8')
        return http_error(request, 404, "Unknown scene");
    unsigned index = request->uri[15] - '1';
    if (!strcmp(request->uri + 16, "/activate") && request->method == HTTP_POST) {
        uint32_t expected = 0; bool has_expected = false;
        if (request->content_len) {
            cJSON *json = http_read_json(request);
            bool valid = kl_json_parse_revision(json, &expected, &has_expected); cJSON_Delete(json);
            if (!valid) return http_error(request, 400, "Provide only an optional expected_revision");
        }
        int status = app_activate_scene(index, "scene", expected, has_expected);
        if (status == 202) return http_json(request, status, app_state_json());
        return http_error(request, status, status == 423 ? "Recording Lock is on" : "Scene cannot be activated");
    }
    if (request->uri[16]) return http_error(request, 404, "Unknown scene route");
    if (request->method != HTTP_PUT) return http_error(request, 405, "Use PUT to save a scene");
    cJSON *json = http_read_json(request); app_scene scene = {.used = true};
    cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name"), *state = cJSON_GetObjectItemCaseSensitive(json, "state");
    kl_patch patch; uint32_t ignored; bool has_expected;
    bool valid = cJSON_IsObject(json) && unique(json) && cJSON_GetArraySize(json) == 2
        && copy_string(scene.name, sizeof(scene.name), name, false)
        && kl_json_parse_patch(state, &patch, &ignored, &has_expected) && !has_expected;
    kl_state defaults = kl_state_default();
    if (valid) valid = kl_state_patch(&defaults, &patch, &scene.state) == KL_OK;
    cJSON_Delete(json);
    if (!valid) return http_error(request, 400, "Invalid scene name or state");
    scene.state.recording_lock = false;
    if (app_scene_save(index, &scene) != ESP_OK) return http_error(request, 503, "Scene could not be persisted");
    app_lock(); app.scenes[index] = scene; app_event_locked("dashboard", "scene.saved", scene.name); app_unlock();
    cJSON *response = cJSON_CreateObject(); cJSON_AddNumberToObject(response, "id", index + 1);
    cJSON_AddStringToObject(response, "name", scene.name); cJSON_AddItemToObject(response, "state", kl_json_light(&scene.state));
    return http_json(request, 200, response);
}
