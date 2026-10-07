#include "app.h"
#include "output_policy.h"
#include "controller_job.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static esp_mqtt_client_handle_t client;
static char state_topic[80], command_topic[80], availability_topic[80];
static bool availability_known, availability_online;
enum { PUBLISH_AVAILABILITY = 1, PUBLISH_STATE = 2 };
static unsigned pending_publications;
static bool notification_queued;

static void notify_publication(unsigned flags) {
    app_lock();
    if (client && app.mqtt_connected) {
        pending_publications |= flags;
        if (!notification_queued) {
            esp_mqtt_event_t event = {0};
            /* IDF posts this event with zero wait and without its MQTT API lock.
             * All enqueue calls stay in the MQTT callback: the client invokes
             * callbacks while holding that lock, so a worker must not invert
             * it with app.mutex. If full, the next event consumes pending flags. */
            notification_queued = esp_mqtt_dispatch_custom_event(client, &event) == ESP_OK;
        }
    }
    app_unlock();
}

void app_mqtt_availability(void) { notify_publication(PUBLISH_AVAILABILITY); }
void app_mqtt_publish(void) { notify_publication(PUBLISH_AVAILABILITY | PUBLISH_STATE); }

/* Called with app.mutex held; journal blocking is an atomic cached read. */
static bool controller_available(void) {
    return app.controller_ready && app.controller_connected && !app.updating
        && !app_controller_update_blocked();
}

static void publish_availability(void) {
    app_lock();
    if (client && app.mqtt_connected) {
        bool online = controller_available();
        /* Serialize the readiness snapshot with enqueue so a late publisher cannot
         * replace a newer offline announcement with stale online state. */
        if ((!availability_known || online != availability_online)
            && esp_mqtt_client_enqueue(client, availability_topic, online ? "online" : "offline",
                0, 1, true, true) >= 0) {
            availability_online = online;
            availability_known = true;
        }
    }
    app_unlock();
}

static void publish_json(const char *topic, cJSON *json, bool retain) {
    char *text = cJSON_PrintUnformatted(json); cJSON_Delete(json);
    if (text) { esp_mqtt_client_enqueue(client, topic, text, 0, 1, retain, true); free(text); }
}

static void publish_state(void) {
    app_lock();
    kl_state state = app.reported;
    uint32_t fields = app.reported_fields, revision = app.reported_revision;
    bool ready = client && app.mqtt_connected && controller_available() && kl_report_publishable(app.controller_connected, app.reported_valid,
        !strcmp(app.operation, "idle"), app.output_revision, revision, fields);
    app_unlock();
    if (!ready) return;
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "state", state.power ? "ON" : "OFF");
    if (fields & KL_BRIGHTNESS) cJSON_AddNumberToObject(json, "brightness", state.brightness);
    if (fields & KL_MODE) cJSON_AddStringToObject(json, "color_mode", state.mode == KL_WHITE ? "color_temp" : "rgb");
    if (fields & KL_TEMPERATURE) cJSON_AddNumberToObject(json, "color_temp", state.temperature_k);
    if (fields & KL_RGB) {
        cJSON *rgb = cJSON_AddObjectToObject(json, "color");
        cJSON_AddNumberToObject(rgb, "r", state.rgb.r); cJSON_AddNumberToObject(rgb, "g", state.rgb.g); cJSON_AddNumberToObject(rgb, "b", state.rgb.b);
    }
    if (fields & KL_EFFECT) cJSON_AddStringToObject(json, "effect", "none");
    char *text = cJSON_PrintUnformatted(json); cJSON_Delete(json);
    if (!text) return;
    /* Close the snapshot/build race: a newer output intent must not be published as confirmed. */
    app_lock();
    ready = app.mqtt_connected && controller_available() && app.reported_revision == revision && app.reported_fields == fields
        && kl_report_publishable(app.controller_connected, app.reported_valid, !strcmp(app.operation, "idle"),
            app.output_revision, revision, fields);
    if (ready) esp_mqtt_client_enqueue(client, state_topic, text, 0, 1, true, true);
    app_unlock();
    free(text);
}

static void flush_publications(void) {
    app_lock();
    unsigned pending = pending_publications;
    pending_publications = 0;
    notification_queued = false;
    app_unlock();
    publish_availability();
    if (pending & PUBLISH_STATE) publish_state();
}

static void discovery(void) {
    char topic[100], url[80]; snprintf(topic, sizeof(topic), "homeassistant/light/%s/config", app.id);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "schema", "json"); cJSON_AddStringToObject(json, "unique_id", app.id);
    cJSON_AddNullToObject(json, "name"); cJSON_AddStringToObject(json, "command_topic", command_topic);
    cJSON_AddStringToObject(json, "state_topic", state_topic); cJSON_AddStringToObject(json, "availability_topic", availability_topic);
    cJSON_AddBoolToObject(json, "brightness", true); cJSON_AddNumberToObject(json, "brightness_scale", 100);
    const char *modes[] = {"rgb", "color_temp"}; cJSON_AddItemToObject(json, "supported_color_modes", cJSON_CreateStringArray(modes, 2));
    cJSON_AddBoolToObject(json, "color_temp_kelvin", true);
    cJSON_AddNumberToObject(json, "min_kelvin", 3000); cJSON_AddNumberToObject(json, "max_kelvin", 7000);
    cJSON_AddBoolToObject(json, "flash", false);
    cJSON_AddBoolToObject(json, "effect", true); cJSON_AddBoolToObject(json, "transition", true);
    const char *effects[] = {"none", "aurora", "breathe"}; cJSON_AddItemToObject(json, "effect_list", cJSON_CreateStringArray(effects, 3));
    cJSON *device = cJSON_AddObjectToObject(json, "device");
    const char *identifiers[] = {app.id}; cJSON_AddItemToObject(device, "identifiers", cJSON_CreateStringArray(identifiers, 1));
    app_lock(); cJSON_AddStringToObject(device, "name", app.config.name);
    snprintf(url, sizeof(url), "http://%s/", app.ip); app_unlock();
    cJSON_AddStringToObject(device, "manufacturer", "Open Keylight"); cJSON_AddStringToObject(device, "model", "Key Light Chroma");
    cJSON_AddStringToObject(device, "sw_version", esp_app_get_description()->version); cJSON_AddStringToObject(device, "configuration_url", url);
    publish_json(topic, json, true);
}

static void command(const char *data, size_t length) {
    if (!length || length > 512 || memchr(data, 0, length)) return;
    char body[513]; memcpy(body, data, length); body[length] = 0;
    if (strstr(body, "\\u0000")) return;
    cJSON *json = cJSON_ParseWithLengthOpts(body, length + 1, NULL, true);
    if (!cJSON_IsObject(json)) { cJSON_Delete(json); return; }
    cJSON *translated = cJSON_CreateObject(); bool valid = true, wants_color = false, wants_white = false;
    const cJSON *item;
    cJSON_ArrayForEach(item, json) {
        for (const cJSON *later = item->next; later; later = later->next)
            if (!strcmp(item->string, later->string)) valid = false;
        if (!strcmp(item->string, "state") && cJSON_IsString(item)) {
            if (!strcmp(item->valuestring, "ON")) cJSON_AddBoolToObject(translated, "power", true);
            else if (!strcmp(item->valuestring, "OFF")) cJSON_AddBoolToObject(translated, "power", false);
            else valid = false;
        } else if (!strcmp(item->string, "brightness")) cJSON_AddItemToObject(translated, "brightness", cJSON_Duplicate(item, true));
        else if (!strcmp(item->string, "color")) {
            wants_color = true; cJSON_AddItemToObject(translated, "rgb", cJSON_Duplicate(item, true));
        } else if (!strcmp(item->string, "color_temp") && cJSON_IsNumber(item) && item->valuedouble >= 3000 && item->valuedouble <= 7000) {
            wants_white = true;
            cJSON_AddNumberToObject(translated, "temperature_k", item->valuedouble);
        } else if (!strcmp(item->string, "effect")) {
            if (cJSON_IsString(item) && strcmp(item->valuestring, "none")) wants_color = true;
            cJSON_AddItemToObject(translated, "effect", cJSON_Duplicate(item, true));
        } else if (!strcmp(item->string, "transition") && cJSON_IsNumber(item) && isfinite(item->valuedouble)
            && item->valuedouble >= 0 && item->valuedouble <= 10) cJSON_AddNumberToObject(translated, "transition_ms", (int)(item->valuedouble * 1000 + 0.5));
        else valid = false;
    }
    if (wants_white && wants_color) valid = false;
    else if (wants_white || wants_color) cJSON_AddStringToObject(translated, "mode", wants_white ? "white" : "color");
    kl_patch patch; uint32_t expected; bool has_expected;
    valid = valid && kl_json_parse_patch(translated, &patch, &expected, &has_expected);
    cJSON_Delete(translated); cJSON_Delete(json);
    int result = valid ? app_submit(&patch, "homeassistant", 0, false) : 400;
    if (result != 202) { app_lock(); app_event_locked("homeassistant", "command.rejected", result == 423 ? "Recording Lock is on" : "Invalid MQTT command"); app_unlock(); }
}

static void mqtt_event(void *unused, esp_event_base_t base, int32_t id, void *data) {
    (void)unused; (void)base;
    esp_mqtt_event_handle_t event = data;
    if (id == MQTT_EVENT_CONNECTED) {
        app_lock(); app.mqtt_connected = true; availability_known = false;
        pending_publications |= PUBLISH_STATE;
        app_event_locked("mqtt", "broker.connected", "Home Assistant discovery published"); app_unlock();
        esp_mqtt_client_subscribe(client, command_topic, 1);
        esp_mqtt_client_subscribe(client, "homeassistant/status", 1);
        discovery();
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        app_lock(); app.mqtt_connected = false; availability_known = false; app_unlock();
    } else if (id == MQTT_EVENT_DATA && event->current_data_offset == 0 && event->total_data_len == event->data_len) {
        if (event->topic_len == strlen(command_topic) && !memcmp(event->topic, command_topic, event->topic_len)) {
            if (!event->retain) command(event->data, event->data_len);
        } else if (event->topic_len == 20 && !memcmp(event->topic, "homeassistant/status", 20)
            && event->data_len == 6 && !memcmp(event->data, "online", 6)) {
            app_lock(); availability_known = false; pending_publications |= PUBLISH_STATE; app_unlock();
            discovery();
        }
    }
    flush_publications();
}

void app_mqtt_start(void) {
    app_lock(); app_config config = app.config; app_unlock();
    if (!config.mqtt_enabled || !config.mqtt_uri[0]) return;
    snprintf(state_topic, sizeof(state_topic), "openkeylight/%s/state", app.id);
    snprintf(command_topic, sizeof(command_topic), "openkeylight/%s/set", app.id);
    snprintf(availability_topic, sizeof(availability_topic), "openkeylight/%s/availability", app.id);
    esp_mqtt_client_config_t mqtt = {
        .broker.address.uri = config.mqtt_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = app.id, .credentials.username = config.mqtt_username,
        .credentials.authentication.password = config.mqtt_password,
        .session.last_will.topic = availability_topic, .session.last_will.msg = "offline",
        .session.last_will.qos = 1, .session.last_will.retain = true,
        .buffer.size = 1024, .outbox.limit = 8192
    };
    esp_mqtt_client_handle_t created = esp_mqtt_client_init(&mqtt);
    app_lock(); client = created; availability_known = false; app_unlock();
    if (client) { esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event, NULL); esp_mqtt_client_start(client); }
}
