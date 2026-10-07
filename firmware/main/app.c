#include "app.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "http_internal.h"
#include "controller_job.h"
#include <stdio.h>
#include <string.h>

app_context app;

uint64_t app_now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }
void app_lock(void) { xSemaphoreTake(app.mutex, portMAX_DELAY); }
void app_unlock(void) { xSemaphoreGive(app.mutex); }

void app_event_locked(const char *actor, const char *event, const char *detail) {
    app_event *e = &app.history[app.history_sequence % KL_HISTORY];
    e->sequence = ++app.history_sequence;
    e->uptime_ms = app_now_ms();
    snprintf(e->actor, sizeof(e->actor), "%s", actor);
    snprintf(e->event, sizeof(e->event), "%s", event);
    snprintf(e->detail, sizeof(e->detail), "%s", detail);
}

int app_submit(const kl_patch *patch, const char *actor, uint32_t expected, bool has_expected) {
    app_lock();
    if ((patch->fields & KL_OUTPUT_FIELDS) && !app.controller_ready) { app_unlock(); return 503; }
    if (app.updating && !(patch->fields == KL_POWER && !patch->value.power)) { app_unlock(); return 503; }
    if (has_expected && expected != app.revision) { app_unlock(); return 409; }
    kl_state next;
    kl_result result = kl_state_patch(&app.desired, patch, &next);
    if (result != KL_OK) { app_unlock(); return result == KL_LOCKED ? 423 : 400; }
    app.desired = next;
    app.revision++;
    snprintf(app.actor, sizeof(app.actor), "%s", actor);
    if (patch->fields & KL_OUTPUT_FIELDS) {
        app.output_revision++;
        snprintf(app.operation, sizeof(app.operation), "pending");
        app.error[0] = 0;
    }
    app_event_locked(actor, "command.accepted", "Desired state changed");
    app_unlock();
    return 202;
}

int app_activate_scene(unsigned index, const char *actor, uint32_t expected, bool has_expected) {
    if (index >= KL_SCENES) return 404;
    app_lock(); app_scene scene = app.scenes[index]; app_unlock();
    if (!scene.used) return 404;
    kl_patch patch = {.fields = KL_ALL_FIELDS & ~KL_LOCK, .value = scene.state};
    return app_submit(&patch, actor, expected, has_expected);
}

void app_main(void) {
    app.mutex = xSemaphoreCreateMutex();
    if (!app.mutex) abort();
    app.desired = app.reported = kl_state_default();
    snprintf(app.operation, sizeof(app.operation), "pending");
    snprintf(app.actor, sizeof(app.actor), "boot");
    esp_read_mac(app.mac, ESP_MAC_WIFI_STA);
    snprintf(app.id, sizeof(app.id), "keylight-%02x%02x%02x", app.mac[3], app.mac[4], app.mac[5]);
    snprintf(app.hostname, sizeof(app.hostname), "%s", app.id);
    esp_err_t storage = app_storage_init();
    if (storage != ESP_OK) {
        ESP_LOGE("keylight", "Storage unavailable (%s); shared NVS was not erased", esp_err_to_name(storage));
        snprintf(app.error, sizeof(app.error), "Storage unavailable; shared NVS preserved");
    }
    /* Journal errors retain diagnostics/HTTP but gate automatic controller I/O. */
    (void)app_controller_update_init();
    app_trial_start();
    if (!app.token_count) app_pair_window();
    esp_err_t worker = app_worker_start();
    if (worker != ESP_OK) {
        ESP_LOGE("keylight", "Lighting unavailable: %s", esp_err_to_name(worker));
        snprintf(app.controller_status, sizeof(app.controller_status), "fault");
    }
    ESP_ERROR_CHECK(app_network_start());
    ESP_ERROR_CHECK(app_http_start());
    esp_err_t button = app_button_start();
    if (button != ESP_OK) {
        ESP_LOGE("keylight", "Physical button unavailable: %s", esp_err_to_name(button));
        app_lock();
        app_event_locked("button", "startup.failed", "Physical button unavailable; dashboard remains available");
        app_unlock();
    }
    app_mqtt_start();
}
