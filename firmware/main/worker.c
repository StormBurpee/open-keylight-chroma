#include "app.h"
#include "nxp_transport.h"
#include "output_policy.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static okl_nxp nxp;
static uint8_t native_effect;

static okl_result execute(void *unused, const okl_request *request) {
    (void)unused;
    okl_reply reply;
    return okl_nxp_execute(&nxp, request, &reply, okl_nxp_default_deadline(&nxp));
}
static okl_result frame(const kl_frame *value) {
    uint8_t rgb[] = {kl_byte(value->r), kl_byte(value->g), kl_byte(value->b)};
    okl_request request; okl_request_frame(&request, rgb); return execute(NULL, &request);
}

static void publish_native(const okl_light_state *s, bool adopt, uint32_t revision) {
    app_lock();
    kl_state state;
    app.reported_fields = kl_native_report(&app.desired, s, &state);
    app.reported = state;
    app.reported_revision = revision;
    app.rgb_confirmed = (app.reported_fields & KL_RGB) != 0;
    app.reported_valid = true;
    app.controller_connected = true;
    if (adopt && app.output_revision == 0) app.desired = state;
    app_unlock();
}

static void fault(okl_result result, bool fresh_mismatch) {
    app_lock();
    snprintf(app.operation, sizeof(app.operation), "error");
    if (fresh_mismatch) {
        snprintf(app.error, sizeof(app.error), "Controller getter differs from requested output; command was not replayed");
    } else {
        snprintf(app.error, sizeof(app.error), "Controller exchange failed (%u); command was not replayed", (unsigned)result);
        app.controller_connected = false; app.reported_valid = false; app.reported_fields = 0;
    }
    app_event_locked("controller", "command.failed", app.error);
    app_unlock();
}

static okl_result read_and_publish(bool adopt, const kl_state *expected, uint32_t revision, bool *mismatch) {
    *mismatch = false;
    okl_light_state state;
    okl_result result = okl_nxp_read_state(&nxp, &state, nxp.transport.now_us(NULL) + 800000);
    if (result == OKL_OK) {
        native_effect = state.effect;
        publish_native(&state, adopt, revision);
        if (expected && !kl_native_matches(expected, &state)) { *mismatch = true; result = OKL_VERIFY; }
    }
    return result;
}

static okl_result prepare(const kl_state *target, bool animated) {
    okl_result result = okl_nxp_claim(&nxp, (const uint8_t *)"Open Keylight", 13, nxp.transport.now_us(NULL) + 600000);
    if (result != OKL_OK) return result;
    return kl_output_prepare(target, animated, &native_effect, execute, NULL);
}

static void worker_task(void *unused) {
    (void)unused;
    okl_request request; okl_reply reply; okl_firmware_version version;
    okl_request_get(&request, OKL_GET_FIRMWARE);
    okl_result result = okl_nxp_execute(&nxp, &request, &reply, okl_nxp_default_deadline(&nxp));
    bool mismatch = false;
    if (result == OKL_OK) result = okl_reply_decode_firmware(&version, &reply);
    if (result == OKL_OK) {
        app_lock(); snprintf(app.controller_version, sizeof(app.controller_version), "%u.%u.%u.%u",
            version.component[0], version.component[1], version.component[2], version.component[3]); app_unlock();
        result = read_and_publish(true, NULL, 0, &mismatch);
    }
    if (result != OKL_OK) fault(result, false);
    else {
        app_lock(); if (app.output_revision == 0) snprintf(app.operation, sizeof(app.operation), "idle"); app_unlock();
        app_mqtt_publish();
    }
    app_lock(); kl_state initial = app.desired; app_unlock();
    kl_frame current = kl_state_frame(&initial, 0);
    kl_transition transition = {0};
    uint32_t seen_revision = 0;
    bool rendering = false;
    for (;;) {
        app_lock(); kl_state target = app.desired; uint32_t revision = app.output_revision; app_unlock();
        if (revision != seen_revision) {
            seen_revision = revision;
            if (nxp.needs_recovery) {
                result = okl_nxp_recover(&nxp, nxp.transport.now_us(NULL) + 250000);
                if (result != OKL_OK) { fault(result, false); rendering = false; vTaskDelay(10); continue; }
            }
            bool animate = target.power && target.mode == KL_COLOR && (target.transition_ms || target.effect != KL_EFFECT_NONE);
            result = prepare(&target, animate);
            if (result != OKL_OK) { fault(result, false); rendering = false; vTaskDelay(10); continue; }
            kl_transition_begin(&transition, &current, &target, app_now_ms());
            rendering = animate;
            if (!animate) {
                current = kl_state_frame(&target, 0);
                result = read_and_publish(false, &target, revision, &mismatch);
                okl_result release = okl_nxp_release(&nxp, nxp.transport.now_us(NULL) + 600000);
                if (result == OKL_OK) result = release;
                if (result != OKL_OK) fault(result, mismatch);
                else {
                    app_lock(); if (revision == app.output_revision) snprintf(app.operation, sizeof(app.operation), "idle");
                    app.completed_revision = revision; app_event_locked("controller", "command.confirmed", "Native getters confirmed output settings"); app_unlock();
                    app_mqtt_publish();
                }
            } else { app_lock(); app.reported_valid = false; app.rgb_confirmed = false; app.reported_fields = 0; app_unlock(); }
        }
        if (rendering) {
            current = kl_transition_sample(&transition, app_now_ms());
            result = frame(&current);
            if (result != OKL_OK) { fault(result, false); rendering = false; }
            else if (kl_transition_done(&transition, app_now_ms()) && transition.target.effect == KL_EFFECT_NONE) {
                /* Restore the original RGB and separate master brightness after frame rendering. */
                mismatch = false;
                result = kl_output_park(&transition.target, &native_effect, execute, NULL);
                if (result == OKL_OK) result = read_and_publish(false, &transition.target, seen_revision, &mismatch);
                okl_result release = okl_nxp_release(&nxp, nxp.transport.now_us(NULL) + 600000);
                if (result == OKL_OK) result = release;
                rendering = false;
                if (result != OKL_OK) fault(result, mismatch);
                else {
                    app_lock();
                    if (seen_revision == app.output_revision) snprintf(app.operation, sizeof(app.operation), "idle");
                    app.completed_revision = seen_revision;
                    app_event_locked("controller", "transition.finished", "RGB and master brightness confirmed by getters"); app_unlock();
                    app_mqtt_publish();
                }
            } else if (kl_transition_done(&transition, app_now_ms())) {
                app_lock(); if (seen_revision == app.output_revision) snprintf(app.operation, sizeof(app.operation), "idle"); app_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(rendering ? 5 : 10));
    }
}

esp_err_t app_worker_start(void) {
    esp_err_t result = app_nxp_transport_init(&nxp, app.mac);
    if (result != ESP_OK) return result;
    return xTaskCreate(worker_task, "lighting", 8192, NULL, 8, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
