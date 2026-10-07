#include "app.h"
#include "nxp_transport.h"
#include "output_policy.h"
#include "controller_worker.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static okl_nxp nxp;
static uint8_t native_effect;
enum { CONTROLLER_UNKNOWN, CONTROLLER_LEGACY, CONTROLLER_ORIGINAL };
static struct {
    unsigned backend;
    bool confirmation_uncertain;
    okl_controller_status status;
} lifecycle;
#define CONTROLLER_PART_ID UINT32_C(0x0000bc40)
#define HEALTH_INTERVAL_MS UINT64_C(1000)

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
        app.controller_ready = false;
        snprintf(app.controller_status, sizeof(app.controller_status), "fault");
    }
    app_event_locked("controller", "command.failed", app.error);
    app_unlock();
}

static kl_frame observed_frame(const okl_light_state *state) {
    kl_frame value = {.white = state->white_brightness, .temperature_k = state->temperature_kelvin};
    /* Native custom/effect modes have no RGB framebuffer getter. Starting dark
     * is honest there; preference RGB and rounded public percentages are not
     * a measurement of the physical starting colour. */
    if (state->effect == 1 && state->color_count == 1) {
        float level = state->color_brightness / 255.0f;
        value.r = state->colors[0] * level;
        value.g = state->colors[1] * level;
        value.b = state->colors[2] * level;
    }
    return value;
}

static okl_result read_and_publish(bool adopt, const kl_state *expected, uint32_t revision,
                                   bool *mismatch, kl_frame *observed) {
    *mismatch = false;
    okl_light_state state;
    okl_result result = okl_nxp_read_state(&nxp, &state, nxp.transport.now_us(NULL) + 800000);
    if (result == OKL_OK) {
        native_effect = state.effect;
        if (observed) *observed = observed_frame(&state);
        publish_native(&state, adopt, revision);
        if (expected && !kl_native_matches(expected, &state)) { *mismatch = true; result = OKL_VERIFY; }
    }
    return result;
}

static okl_result prepare(const kl_state *target, bool animated, const kl_frame *initial) {
    okl_result result = okl_nxp_claim(&nxp, (const uint8_t *)"Open Keylight", 13, nxp.transport.now_us(NULL) + 600000);
    if (result != OKL_OK) return result;
    return kl_output_prepare(target, animated, initial, &native_effect, execute, NULL);
}

static okl_result release_if_synchronized(okl_result result) {
    if (nxp.needs_recovery) return result;
    /* release checks the current owner before writing; it cannot release a
     * different client. Preserve a prior failure instead of reporting success. */
    okl_result released = okl_nxp_release(&nxp, nxp.transport.now_us(NULL) + 600000);
    return result == OKL_OK ? released : result;
}

static okl_result getter(okl_command command, okl_reply *reply) {
    okl_request request;
    okl_result result = okl_request_get(&request, command);
    return result == OKL_OK ? okl_nxp_execute(&nxp, &request, reply, okl_nxp_default_deadline(&nxp)) : result;
}

static bool legacy_version(const okl_firmware_version *version) {
    const uint8_t supported[4] = {1, 3, 0, 0};
    return !memcmp(version->component, supported, sizeof(supported));
}

static bool unsupported_status(okl_result result, const okl_reply *reply) {
    return result == OKL_REMOTE && reply->received && reply->report.status == 5 &&
        reply->report.command_class == 0 && reply->report.opcode == 0xfc && reply->report.size == 0;
}

static bool owner_denied_status(okl_result result, const okl_reply *reply) {
    return result == OKL_OWNER_DENIED && reply->received && reply->report.status == 8 &&
        reply->report.command_class == 0 && reply->report.opcode == 0xfc && reply->report.size == 0;
}

static bool lighting_status(const okl_controller_status *status) {
    return status->role == OKL_ROLE_LIGHTING && !status->boot_requested &&
        status->part_id == CONTROLLER_PART_ID &&
        status->capabilities == (OKL_CAP_RECOVERY_READY | OKL_CAP_LIGHTING_READY);
}

static okl_result controller_status(okl_controller_status *status) {
    okl_reply reply;
    okl_result result = getter(OKL_GET_CONTROLLER_STATUS, &reply);
    return result == OKL_OK ? okl_reply_decode_controller_status(status, &reply) : result;
}

static void lifecycle_observed(const char *status, bool connected) {
    app_lock();
    snprintf(app.controller_backend, sizeof(app.controller_backend), "%s",
        lifecycle.backend == CONTROLLER_LEGACY ? "legacy" : lifecycle.backend == CONTROLLER_ORIGINAL ? "original" : "unknown");
    snprintf(app.controller_status, sizeof(app.controller_status), "%s", status);
    app.controller_connected = connected;
    app.controller_ready = false;
    app.controller_part_id = lifecycle.backend == CONTROLLER_ORIGINAL ? lifecycle.status.part_id : 0;
    app.controller_trial_confirmed = lifecycle.backend == CONTROLLER_ORIGINAL && lifecycle.status.trial_confirmed;
    if (connected) app.controller_last_health_ms = app_now_ms();
    if (!strcmp(status, "diagnostic") || !strcmp(status, "unsupported")) {
        app.reported_valid = false; app.reported_fields = 0;
        snprintf(app.operation, sizeof(app.operation), "error");
        snprintf(app.error, sizeof(app.error), "%s", !strcmp(status, "diagnostic") ?
            "SPI diagnostic image; automatic confirmation and lighting are disabled" :
            "Unsupported controller identity or readiness; lighting is disabled");
    }
    app_unlock();
}

static okl_result bootstrap(kl_frame *current, uint32_t *seen_revision, bool publish_ready,
                            const okl_firmware_version *expected_version) {
    okl_reply reply; okl_firmware_version version;
    okl_controller_status status = {0}; uint32_t part;
    bool mismatch = false, admission_claimed = false;
    lifecycle_observed("starting", false);
    app_lock(); app.reported_valid = false; app.reported_fields = 0; app_unlock();
    if (nxp.needs_recovery) {
        okl_result recovered = okl_nxp_recover(&nxp, nxp.transport.now_us(NULL) + 250000);
        if (recovered != OKL_OK) return recovered;
    }
    okl_result result = getter(OKL_GET_FIRMWARE, &reply);
    if (result != OKL_OK || (result = okl_reply_decode_firmware(&version, &reply)) != OKL_OK) return result;
    if (expected_version && memcmp(version.component, expected_version->component, 4)) return OKL_VERIFY;
    app_lock(); snprintf(app.controller_version, sizeof(app.controller_version), "%u.%u.%u.%u",
        version.component[0], version.component[1], version.component[2], version.component[3]); app_unlock();
    result = getter(OKL_GET_CONTROLLER_STATUS, &reply);
    if (legacy_version(&version) && owner_denied_status(result, &reply)) {
        /* Legacy ownership exempts the version getter, but not unknown FC.
         * A stale owner from the previous ESP can therefore hide the expected
         * unsupported reply. Claim once and verify it, then probe FC again;
         * denial alone never establishes controller identity or readiness. */
        result = okl_nxp_claim(&nxp, (const uint8_t *)"Open Keylight", 13,
                               nxp.transport.now_us(NULL) + 600000);
        if (result != OKL_OK) return release_if_synchronized(result);
        admission_claimed = true;
        result = getter(OKL_GET_CONTROLLER_STATUS, &reply);
    }
    if (unsupported_status(result, &reply) && legacy_version(&version)) {
        lifecycle.backend = CONTROLLER_LEGACY;
    } else {
        lifecycle.backend = CONTROLLER_UNKNOWN;
        if (result != OKL_OK) {
            if (unsupported_status(result, &reply)) { lifecycle_observed("unsupported", true); result = OKL_VERIFY; }
            goto admission_failed;
        }
        result = okl_reply_decode_controller_status(&status, &reply);
        if (result != OKL_OK) { lifecycle_observed("unsupported", true); goto admission_failed; }
        lifecycle.backend = CONTROLLER_ORIGINAL; lifecycle.status = status;
        if (!lighting_status(&status)) {
            lifecycle_observed(status.role == OKL_ROLE_SPI_DIAGNOSTIC ? "diagnostic" : "unsupported", true);
            result = OKL_VERIFY; goto admission_failed;
        }
        result = getter(OKL_GET_PART_ID, &reply);
        if (result == OKL_OK) result = okl_reply_decode_part_id(&part, &reply);
        if (result != OKL_OK) goto admission_failed;
        if (part != status.part_id) { lifecycle_observed("unsupported", true); result = OKL_VERIFY; goto admission_failed; }
    }
    lifecycle_observed("starting", true);
    result = admission_claimed ? OKL_OK : okl_nxp_claim(&nxp, (const uint8_t *)"Open Keylight", 13,
                                                      nxp.transport.now_us(NULL) + 600000);
    if (result == OKL_OK) result = read_and_publish(true, NULL, 0, &mismatch, current);
    if (result == OKL_OK && lifecycle.backend == CONTROLLER_ORIGINAL) {
        result = controller_status(&status);
        if (result == OKL_OK && !lighting_status(&status)) result = OKL_VERIFY;
        if (result == OKL_OK && !status.trial_confirmed) {
            /* A fresh unconfirmed image must still be dark. Never replay an
             * old desired scene to qualify it, or repeat an uncertain FD. */
            if (lifecycle.confirmation_uncertain || native_effect != 0 || current->white || status.uptime_ms >= 30000u)
                result = OKL_VERIFY;
            else {
                okl_request request; okl_request_confirm_controller(&request);
                lifecycle.confirmation_uncertain = true;
                result = okl_nxp_execute(&nxp, &request, &reply, okl_nxp_default_deadline(&nxp));
                if (result == OKL_OK) result = okl_reply_check_controller_confirmation(&reply);
                if (result == OKL_OK) result = controller_status(&status);
                if (result == OKL_OK && (!lighting_status(&status) || !status.trial_confirmed)) result = OKL_VERIFY;
            }
        }
        if (result == OKL_OK) { lifecycle.confirmation_uncertain = false; lifecycle.status = status; }
    }
    result = release_if_synchronized(result);
    if (result != OKL_OK) return result;
    app_lock();
    /* Discard every pre-readiness output revision before opening the gate.
     * A later user mutation is observed normally; a reset cannot replay one. */
    *seen_revision = app.output_revision;
    app.controller_connected = true;
    app.controller_ready = publish_ready;
    snprintf(app.controller_status, sizeof(app.controller_status), "%s", publish_ready ? "ready" : "starting");
    app.controller_trial_confirmed = lifecycle.backend == CONTROLLER_ORIGINAL && lifecycle.status.trial_confirmed;
    app.controller_last_health_ms = app_now_ms();
    if (app.output_revision == 0) { snprintf(app.operation, sizeof(app.operation), "idle"); app.error[0] = 0; }
    if (publish_ready) app_event_locked("controller", "controller.ready", lifecycle.backend == CONTROLLER_LEGACY ?
        "Legacy 1.3 controller; no original trial confirmation" : "Original controller identity and trial verified");
    app_unlock(); if (publish_ready) app_mqtt_publish();
    return OKL_OK;
admission_failed:
    return admission_claimed ? release_if_synchronized(result) : result;
}

static okl_result health(void) {
    okl_reply reply; okl_firmware_version version; okl_controller_status status;
    okl_result result;
    if (lifecycle.backend == CONTROLLER_LEGACY) {
        result = getter(OKL_GET_FIRMWARE, &reply);
        if (result == OKL_OK) result = okl_reply_decode_firmware(&version, &reply);
        if (result != OKL_OK) return result;
        if (!legacy_version(&version)) return OKL_VERIFY;
        result = getter(OKL_GET_CONTROLLER_STATUS, &reply);
        if (!unsupported_status(result, &reply)) return result == OKL_OK ? OKL_VERIFY : result;
    } else {
        result = controller_status(&status);
        if (result != OKL_OK) return result;
        if (!lighting_status(&status) || !status.trial_confirmed ||
            (uint32_t)(status.uptime_ms - lifecycle.status.uptime_ms) >= UINT32_C(0x80000000)) return OKL_VERIFY;
        lifecycle.status = status;
    }
    app_lock(); app.controller_last_health_ms = app_now_ms(); app_unlock();
    return OKL_OK;
}

static void worker_task(void *unused) {
    (void)unused;
    bool mismatch = false;
    kl_frame current = {0};
    uint32_t seen_revision = 0;
    okl_result result = app_controller_update_blocked() ? OKL_OK : bootstrap(&current, &seen_revision, true, NULL);
    if (result != OKL_OK) {
        app_lock(); bool classified = !strcmp(app.controller_status, "diagnostic") || !strcmp(app.controller_status, "unsupported"); app_unlock();
        if (!classified) fault(result, false);
    }
    uint64_t next_health = app_now_ms() + HEALTH_INTERVAL_MS;
    kl_transition transition = {0};
    bool rendering = false;
    bool pending_off = false;
    uint32_t pending_off_revision = 0;
    for (;;) {
        app_controller_job job;
        if (app_controller_update_take(&job)) {
            app_lock();
            bool accepted_off = !app.desired.power && app.output_revision != seen_revision;
            uint32_t accepted_off_revision = app.output_revision;
            app_unlock();
            rendering = false;
            okl_loader_audit audit;
            app_controller_worker_outcome outcome;
            okl_loader_result updated = app_controller_worker_run(&nxp, &job, &audit, &outcome);
            bool confirmed = false;
            if (updated == OKL_LOADER_OK) {
                lifecycle.confirmation_uncertain = false;
                result = bootstrap(&current, &seen_revision, false, &job.image.version);
                confirmed = result == OKL_OK && lifecycle.backend == CONTROLLER_ORIGINAL && lifecycle.status.trial_confirmed;
            }
            bool rejected = updated == OKL_LOADER_INVALID &&
                app_controller_update_reject(job.id, &audit, updated, &outcome);
            bool durable = !rejected && app_controller_update_finish(job.id, &audit, updated, confirmed,
                "Controller update unresolved; explicit recovery required");
            /* A successful update executes Off itself. Read-only rejection
             * must preserve a newly accepted Off from the receive window,
             * without reviving an older scene or retrying an attempted Off. */
            pending_off = rejected && accepted_off;
            pending_off_revision = accepted_off_revision;
            app_lock();
            seen_revision = app.output_revision;
            app.controller_ready = durable;
            if (durable) {
                /* The authorized update leaves dark readback as the desired
                 * state. Old scenes cannot resume after journal completion. */
                app.desired = app.reported; ++app.revision; ++app.output_revision;
                seen_revision = app.output_revision;
                app.reported_revision = app.completed_revision = app.output_revision;
                snprintf(app.controller_status, sizeof(app.controller_status), "ready");
            }
            app_unlock(); app_mqtt_publish();
            next_health = app_now_ms() + HEALTH_INTERVAL_MS;
            continue;
        }
        /* A receiving reservation (including ESP OTA) still permits the
         * already accepted Off command and ordinary health. Only the taken
         * synchronous controller job excludes all other SPI work. */
        if (app_controller_update_blocked()) {
            rendering = false; vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }
        app_lock(); bool ready = app.controller_ready; app_unlock();
        if (app_now_ms() >= next_health) {
            result = ready ? health() : bootstrap(&current, &seen_revision, true, NULL);
            next_health = app_now_ms() + HEALTH_INTERVAL_MS;
            if (result != OKL_OK) {
                rendering = false;
                app_lock(); bool classified = !strcmp(app.controller_status, "diagnostic") || !strcmp(app.controller_status, "unsupported"); app_unlock();
                if (!classified) fault(result, false);
            }
            app_lock(); ready = app.controller_ready; app_unlock();
        }
        if (!ready) { rendering = false; vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        app_lock(); kl_state target = app.desired; uint32_t revision = app.output_revision; app_unlock();
        bool apply_pending_off = pending_off && revision == pending_off_revision && !target.power;
        if (revision != seen_revision || apply_pending_off) {
            pending_off = false;
            seen_revision = revision;
            if (nxp.needs_recovery) {
                result = okl_nxp_recover(&nxp, nxp.transport.now_us(NULL) + 250000);
                if (result != OKL_OK) { fault(result, false); rendering = false; vTaskDelay(10); continue; }
            }
            bool animate = target.power && target.mode == KL_COLOR && (target.transition_ms || target.effect != KL_EFFECT_NONE);
            result = prepare(&target, animate, &current);
            if (result != OKL_OK) {
                result = release_if_synchronized(result);
                fault(result, false); rendering = false; vTaskDelay(10); continue;
            }
            kl_transition_begin(&transition, &current, &target, app_now_ms());
            rendering = animate;
            if (!animate) {
                result = read_and_publish(false, &target, revision, &mismatch, &current);
                result = release_if_synchronized(result);
                if (result != OKL_OK) fault(result, mismatch);
                else {
                    app_lock(); if (revision == app.output_revision) snprintf(app.operation, sizeof(app.operation), "idle");
                    app.completed_revision = revision; app_event_locked("controller", "command.confirmed", "Native getters confirmed output settings"); app_unlock();
                    app_mqtt_publish();
                }
            } else { app_lock(); app.reported_valid = false; app.rgb_confirmed = false; app.reported_fields = 0; app_unlock(); }
        }
        if (rendering) {
            kl_frame sample = kl_transition_sample(&transition, app_now_ms());
            result = frame(&sample);
            if (result != OKL_OK) {
                result = release_if_synchronized(result);
                fault(result, false); rendering = false;
            }
            else {
                current = sample; /* Keep the last ACKed frame if a later exchange fails. */
                if (kl_transition_done(&transition, app_now_ms()) && transition.target.effect == KL_EFFECT_NONE) {
                    /* Restore original RGB and separate master brightness after rendering. */
                    mismatch = false;
                    result = kl_output_park(&transition.target, &native_effect, execute, NULL);
                    if (result == OKL_OK) result = read_and_publish(false, &transition.target, seen_revision, &mismatch, &current);
                    result = release_if_synchronized(result);
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
        }
        vTaskDelay(pdMS_TO_TICKS(rendering ? 5 : 10));
    }
}

esp_err_t app_worker_start(void) {
    memset(&lifecycle, 0, sizeof(lifecycle));
    if (!app_controller_update_blocked()) {
        lifecycle_observed("starting", false);
        esp_err_t result = app_nxp_transport_init(&nxp, app.mac);
        if (result != ESP_OK) return result;
    }
    return xTaskCreate(worker_task, "lighting", 8192, NULL, 8, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
