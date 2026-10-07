#include "worker_mocks.h"
#include "app.h"
#include "okl_nxp.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(value) do { ++checks; if (!(value)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); exit(1); } } while (0)
static okl_result worker_execute(okl_nxp *, const okl_request *, okl_reply *, uint64_t);
static okl_result worker_claim(okl_nxp *, const uint8_t *, size_t, uint64_t);
static okl_result worker_release(okl_nxp *, uint64_t);
static okl_result worker_read(okl_nxp *, okl_light_state *, uint64_t);
static okl_result worker_recover(okl_nxp *, uint64_t);
#define okl_nxp_execute worker_execute
#define okl_nxp_claim worker_claim
#define okl_nxp_release worker_release
#define okl_nxp_read_state worker_read
#define okl_nxp_recover worker_recover
#include "../../firmware/main/worker.c"
#undef okl_nxp_execute
#undef okl_nxp_claim
#undef okl_nxp_release
#undef okl_nxp_read_state
#undef okl_nxp_recover

app_context app;
static jmp_buf finished;
static void (*task_entry)(void *);
static uint64_t now;
static unsigned locks, claims, releases, reads, versions, publications, faults, recoveries;
static unsigned writes, delays, stop_after, fail_write, delay_step;
static okl_request sent[32];
static okl_light_state controller;
static okl_result claim_result, read_result, release_result;
static bool poison_claim, poison_read, malformed_version, queued_on_read, retarget_after_failure;
static bool claimed, task_failure;
static bool poison_write;
static bool original, confirmation_missing, lost_confirmation_ack, malformed_status, status_unsupported;
static uint8_t legacy_minor;
static unsigned status_reads, part_reads, confirmations;
static uint32_t part_reply;
static okl_controller_status remote_status;
static uint64_t boot_at, reset_at;
static bool reset_done;
static bool journal_blocked, job_queued, finish_durable, finished_confirmed, off_on_read;
static unsigned transport_inits, job_runs, job_finishes;
static okl_loader_result job_result;

bool app_controller_update_blocked(void) { return journal_blocked; }
bool app_controller_update_take(app_controller_job *out) {
    if (!job_queued || journal_blocked) return false;
    CHECK(!locks); job_queued = false; app.updating = true; app.controller_ready = false;
    memset(out, 0, sizeof(*out)); out->id = 7; out->image.version.component[1] = 1;
    return true;
}
okl_loader_result app_controller_worker_run(okl_nxp *driver, const app_controller_job *job, okl_loader_audit *audit) {
    CHECK(driver == &nxp && job->id == 7 && !app.controller_ready && app.updating && !locks); ++job_runs;
    memset(audit, 0, sizeof(*audit));
    if (job_result == OKL_LOADER_OK) {
        original = true; boot_at = now; remote_status.trial_confirmed = 0;
        controller = (okl_light_state){.temperature_kelvin = 5000};
    }
    return job_result;
}
bool app_controller_update_finish(uint32_t id, const okl_loader_audit *audit, okl_loader_result result, bool confirmed, const char *error) {
    CHECK(id == 7 && audit && result == job_result && error && !app.controller_ready && !locks); ++job_finishes;
    finished_confirmed = confirmed; app.updating = false;
    bool success = finish_durable && confirmed && result == OKL_LOADER_OK;
    journal_blocked = !success;
    return success;
}

uint64_t app_now_ms(void) { return now; }
static uint64_t transport_now(void *unused) { (void)unused; return now * 1000; }
void app_lock(void) { CHECK(!locks); ++locks; }
void app_unlock(void) { CHECK(locks == 1); --locks; }
void app_event_locked(const char *actor, const char *event, const char *detail) {
    CHECK(locks == 1 && actor && event && detail);
    if (!strcmp(event, "command.failed")) ++faults;
}
void app_mqtt_publish(void) {
    CHECK(!locks); ++publications;
    /* A real API mutation is admitted only after the worker opens readiness. */
    if (queued_on_read && publications == 1 && app.controller_ready) {
        app.desired = kl_state_default(); app.desired.power = true; app.desired.mode = KL_COLOR;
        app.desired.rgb = (kl_rgb){10, 180, 90}; app.desired.brightness = 80;
        app.output_revision = 1; snprintf(app.operation, sizeof(app.operation), "queued");
    }
    if (off_on_read && publications == 1 && app.controller_ready) {
        CHECK(app.updating); app.desired.power = false; ++app.output_revision;
    }
}
esp_err_t app_nxp_transport_init(okl_nxp *driver, const uint8_t mac[6]) {
    ++transport_inits;
    CHECK(driver && mac); memset(driver, 0, sizeof(*driver)); driver->transport.now_us = transport_now;
    memcpy(driver->identity, mac, 6); return ESP_OK;
}
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *argument,
                unsigned priority, TaskHandle_t *handle) {
    CHECK(task && !strcmp(name, "lighting") && stack == 8192 && !argument && priority == 8 && !handle);
    task_entry = task; return task_failure ? 0 : pdPASS;
}
void vTaskDelay(unsigned ticks) {
    CHECK(!locks && (ticks == 5 || ticks == 10)); ++delays; now += delay_step;
    if (reset_at && !reset_done && now >= reset_at) {
        reset_done = true; boot_at = now; remote_status.trial_confirmed = 0; claimed = false;
        controller = (okl_light_state){.temperature_kelvin = 5200};
    }
    if (retarget_after_failure && delays == 2) {
        app.desired.rgb = (kl_rgb){5, 220, 35}; ++app.output_revision;
    }
    if (delays >= stop_after) longjmp(finished, 1);
}
static void bounded(uint64_t deadline, uint64_t maximum_us) {
    CHECK(!locks && deadline > now * 1000 && deadline <= now * 1000 + maximum_us);
}
static okl_result worker_claim(okl_nxp *driver, const uint8_t *name, size_t size, uint64_t deadline) {
    bounded(deadline, 600000); ++claims;
    CHECK(size == 13 && !memcmp(name, "Open Keylight", size));
    if (claim_result == OKL_OK) claimed = true;
    if (poison_claim) driver->needs_recovery = 1;
    return claim_result;
}
static okl_result worker_release(okl_nxp *driver, uint64_t deadline) {
    bounded(deadline, 600000); CHECK(!driver->needs_recovery); ++releases;
    if (release_result == OKL_OK) claimed = false;
    return release_result;
}
static okl_result worker_read(okl_nxp *driver, okl_light_state *state, uint64_t deadline) {
    bounded(deadline, 800000); ++reads;
    if (poison_read) driver->needs_recovery = 1;
    if (read_result == OKL_OK) *state = controller;
    return read_result;
}
static okl_result worker_recover(okl_nxp *driver, uint64_t deadline) {
    bounded(deadline, 250000); ++recoveries; driver->needs_recovery = 0; return OKL_OK;
}
static void put_word(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24); out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8); out[3] = (uint8_t)value;
}
static okl_result worker_execute(okl_nxp *driver, const okl_request *request, okl_reply *reply, uint64_t deadline) {
    bounded(deadline, OKL_DEFAULT_TIMEOUT_US); CHECK(!driver->needs_recovery);
    memset(reply, 0, sizeof(*reply));
    if (request->command == OKL_GET_FIRMWARE) {
        ++versions; reply->received = 1; reply->acknowledged = 1; reply->report.status = 2;
        reply->report.command_class = 0; reply->report.opcode = 0x87;
        reply->report.size = malformed_version ? 3 : 4;
        reply->report.arguments[0] = original ? 0 : 1; reply->report.arguments[1] = original ? 1 : legacy_minor;
        return OKL_OK;
    }
    if (request->command == OKL_GET_CONTROLLER_STATUS) {
        ++status_reads; reply->received = 1; reply->report.opcode = 0xfc;
        if (!original || status_unsupported) { reply->report.status = 5; return OKL_REMOTE; }
        reply->acknowledged = 1; reply->report.status = 2; reply->report.size = 24;
        uint8_t *a = reply->report.arguments;
        memcpy(a, "OKLC", 4); a[4] = 1; a[6] = remote_status.role;
        a[7] = remote_status.trial_confirmed | (remote_status.boot_requested << 1);
        put_word(a + 8, remote_status.capabilities); put_word(a + 12, remote_status.part_id);
        put_word(a + 16, (uint32_t)(now - boot_at)); put_word(a + 20, remote_status.reset_cause);
        if (malformed_status) a[5] = 1;
        return OKL_OK;
    }
    if (request->command == OKL_GET_PART_ID) {
        ++part_reads; reply->received = reply->acknowledged = 1;
        reply->report.status = 2; reply->report.opcode = 0xfe; reply->report.size = 4;
        put_word(reply->report.arguments, part_reply); return OKL_OK;
    }
    if (request->command == OKL_CONFIRM_CONTROLLER) {
        CHECK(claimed && original && remote_status.role == OKL_ROLE_LIGHTING);
        CHECK(request->size == 4 && !memcmp(request->arguments, "OKLC", 4)); ++confirmations;
        if (!confirmation_missing) remote_status.trial_confirmed = 1;
        if (lost_confirmation_ack) { driver->needs_recovery = 1; return OKL_TIMEOUT; }
        reply->received = reply->acknowledged = 1; reply->report.status = 2;
        reply->report.opcode = 0xfd; reply->report.size = 1; reply->report.arguments[0] = 1; return OKL_OK;
    }
    CHECK(claimed && writes < 32); sent[writes++] = *request;
    if (fail_write == writes) {
        if (poison_write) { driver->needs_recovery = 1; return OKL_TIMEOUT; }
        return OKL_REMOTE; /* Correlated rejection leaves the link synchronized. */
    }
    const uint8_t *a = request->arguments;
    switch (request->command) {
    case OKL_SET_EFFECT: controller.effect = a[2]; controller.color_count = a[5]; memcpy(controller.colors, a + 6, 6); break;
    case OKL_SET_COLOR_BRIGHTNESS: controller.color_brightness = a[2]; break;
    case OKL_SET_WHITE_BRIGHTNESS: controller.white_brightness = a[2]; break;
    case OKL_SET_TEMPERATURE: controller.temperature_kelvin = (uint16_t)(a[2] * 256u + a[3]); break;
    case OKL_SET_FRAME: break;
    default: CHECK(false);
    }
    return OKL_OK;
}
static void reset(void) {
    memset(&app, 0, sizeof(app)); app.desired = kl_state_default(); app.mac[0] = 2;
    memset(&nxp, 0, sizeof(nxp)); native_effect = 0;
    memset(sent, 0, sizeof(sent)); task_entry = NULL;
    now = locks = claims = releases = reads = versions = publications = faults = recoveries = 0;
    writes = delays = fail_write = 0; stop_after = 1; delay_step = 5;
    claim_result = read_result = release_result = OKL_OK;
    poison_claim = poison_read = malformed_version = queued_on_read = retarget_after_failure = claimed = task_failure = false;
    poison_write = false;
    original = confirmation_missing = lost_confirmation_ack = malformed_status = reset_done = status_unsupported = false;
    legacy_minor = 3;
    status_reads = part_reads = confirmations = 0; boot_at = reset_at = 0;
    part_reply = CONTROLLER_PART_ID;
    remote_status = (okl_controller_status){.role = OKL_ROLE_LIGHTING, .capabilities = 3,
        .part_id = CONTROLLER_PART_ID, .reset_cause = 0x13};
    journal_blocked = job_queued = finished_confirmed = off_on_read = false; finish_durable = true;
    transport_inits = job_runs = job_finishes = 0; job_result = OKL_LOADER_OK;
    controller = (okl_light_state){.effect = 1, .color_count = 1, .colors = {255, 0, 32},
                                  .color_brightness = 102, .temperature_kelvin = 4500};
}
static void run(void) {
    CHECK(app_worker_start() == ESP_OK && task_entry);
    if (!setjmp(finished)) task_entry(NULL);
    CHECK(!locks);
}
static void test_startup(void) {
    reset(); run();
    CHECK(claims == 1 && versions == 1 && reads == 1 && releases == 1 && !claimed);
    CHECK(!writes && !faults && publications == 1 && app.reported_valid && app.controller_connected);
    CHECK(app.desired.power && app.desired.mode == KL_COLOR && app.desired.brightness == 40);
    CHECK(app.desired.rgb.r == 255 && app.desired.rgb.b == 32 && !strcmp(app.operation, "idle"));
    for (unsigned failure = 0; failure < 5; ++failure) {
        reset(); stop_after = 3;
        if (failure == 0) { claim_result = OKL_OWNER_DENIED; }
        if (failure == 1) { claim_result = OKL_TIMEOUT; poison_claim = true; }
        if (failure == 2) { read_result = OKL_TIMEOUT; poison_read = true; }
        if (failure == 3) { release_result = OKL_IO; }
        if (failure == 4) { malformed_version = true; }
        run();
        CHECK(!writes && claims == (unsigned)(failure != 4) && !recoveries && faults == 1);
        CHECK(!app.reported_valid && !app.controller_connected && !strcmp(app.operation, "error"));
        CHECK(releases == (unsigned)(failure != 1 && failure != 2 && failure != 4));
    }
    reset(); task_failure = true;
    CHECK(app_worker_start() == ESP_ERR_NO_MEM && !claims && !writes);
}
static void test_startup_queued_transition(void) {
    reset(); queued_on_read = true; run();
    CHECK(claims == 2 && releases == 1 && writes == 6 && !faults);
    CHECK(app.desired.rgb.r == 10 && app.desired.brightness == 80);
    CHECK(sent[3].command == OKL_SET_FRAME);
    CHECK(sent[3].arguments[5] == 102 && sent[3].arguments[6] == 0 && sent[3].arguments[7] == 13);
    CHECK(sent[4].command == OKL_SET_COLOR_BRIGHTNESS && sent[4].arguments[2] == 255);
    CHECK(!app.reported_valid && !app.completed_revision);
    for (unsigned level = 0; level <= 255; ++level) {
        reset(); queued_on_read = true; controller.color_brightness = (uint8_t)level; run();
        CHECK(sent[3].command == OKL_SET_FRAME && sent[3].arguments[5] == level);
        CHECK(sent[3].arguments[7] == (32u * level + 127u) / 255u);
    }
    reset(); queued_on_read = true; controller.effect = 8; controller.color_brightness = 255; run();
    CHECK(sent[3].arguments[5] == 0 && sent[3].arguments[6] == 0 && sent[3].arguments[7] == 0);
    CHECK(!app.rgb_confirmed); /* Native effect metadata cannot confirm custom framebuffer RGB. */
}
static void test_setup_and_frame_failures(void) {
    for (unsigned failed = 1; failed <= 6; ++failed) {
        reset(); queued_on_read = true; fail_write = failed; stop_after = 3; run();
        CHECK(writes == failed && claims == 2 && releases == 2 && !claimed);
        CHECK(faults == 1 && !app.reported_valid && !app.completed_revision && !recoveries);
        if (failed == 4) CHECK(controller.color_brightness == 0); /* First frame rejection cannot enable master. */
    }
    reset(); queued_on_read = true; fail_write = 7; delay_step = 500;
    retarget_after_failure = true; stop_after = 3; run();
    CHECK(faults == 1 && claims == 3 && writes == 7);
    CHECK(sent[6].command == OKL_SET_FRAME && sent[6].arguments[6] != 0); /* Rejected mid-fade sample. */
    CHECK(app.controller_ready && app.completed_revision == 0); /* No queued replay through recovery. */
    for (unsigned failed = 1; failed <= 6; ++failed) {
        reset(); queued_on_read = true; fail_write = failed; poison_write = true; stop_after = 3; run();
        CHECK(writes == failed && claims == 2 && releases == 1 && nxp.needs_recovery);
        CHECK(faults == 1 && !recoveries && !app.completed_revision);
    }
}
static void test_transition_completion(void) {
    reset(); queued_on_read = true; delay_step = 2000; stop_after = 2; run();
    CHECK(writes == 9 && releases == 2 && !claimed && !faults);
    CHECK(sent[7].command == OKL_SET_COLOR_BRIGHTNESS && sent[7].arguments[2] == 204);
    CHECK(sent[8].command == OKL_SET_EFFECT && sent[8].arguments[2] == 1);
    CHECK(app.completed_revision == 1 && app.reported_revision == 1 && app.reported_valid && app.rgb_confirmed);
    CHECK(!strcmp(app.operation, "idle") && app.reported.rgb.g == 180 && app.reported.brightness == 80);
}
static void original_reset(void) {
    reset(); original = true;
    controller = (okl_light_state){.temperature_kelvin = 5200};
}
static void test_original_bootstrap(void) {
    original_reset(); run();
    CHECK(confirmations == 1 && status_reads == 3 && part_reads == 1 && claims == 1 && releases == 1);
    CHECK(!writes && !faults && app.controller_ready && app.controller_trial_confirmed);
    CHECK(!strcmp(app.controller_backend, "original") && app.controller_part_id == CONTROLLER_PART_ID);
    CHECK(!app.desired.power && !claimed && app.reported_valid);
    original_reset(); remote_status.trial_confirmed = 1;
    controller.effect = 1; controller.color_count = 1; controller.colors[1] = 255; controller.color_brightness = 127;
    run(); CHECK(confirmations == 0 && !writes && app.controller_ready && app.desired.power);
    CHECK(app.desired.rgb.g == 255 && app.desired.brightness == 50); /* ESP-only restart adopts a confirmed peer. */
}
static void test_original_rejections(void) {
    for (unsigned variant = 0; variant < 10; ++variant) {
        original_reset();
        if (variant == 0) remote_status.role = OKL_ROLE_SPI_DIAGNOSTIC;
        if (variant == 1) { remote_status.role = OKL_ROLE_SPI_DIAGNOSTIC; remote_status.trial_confirmed = 1; }
        if (variant == 2) remote_status.role = OKL_ROLE_UNQUALIFIED;
        if (variant == 3) remote_status.capabilities = OKL_CAP_RECOVERY_READY;
        if (variant == 4) remote_status.part_id ^= 1;
        if (variant == 5) part_reply ^= 1;
        if (variant == 6) malformed_status = true;
        if (variant == 7) status_unsupported = true; /* Old diagnostic has only version/FE. */
        if (variant == 8) remote_status.boot_requested = 1;
        if (variant == 9) { original = false; legacy_minor = 4; }
        run(); CHECK(!app.controller_ready && !writes && !confirmations && !claims && !reads);
        CHECK(app.controller_connected && !app.reported_valid);
        CHECK(!strcmp(app.controller_status, variant < 2 ? "diagnostic" : "unsupported"));
    }
    for (unsigned variant = 0; variant < 4; ++variant) {
        original_reset();
        if (variant == 0) controller.white_brightness = 1;
        if (variant == 1) { controller.effect = 8; controller.color_brightness = 255; } /* Unknown framebuffer is not dark proof. */
        if (variant == 2) now = 30000;
        if (variant == 3) claim_result = OKL_OWNER_DENIED;
        run(); CHECK(!app.controller_ready && !confirmations && !writes && faults == 1);
    }
}
static void test_confirmation_readback_and_ambiguity(void) {
    original_reset(); confirmation_missing = true; delay_step = 1000; stop_after = 3; run();
    CHECK(confirmations == 1 && !writes && !app.controller_ready && lifecycle.confirmation_uncertain);
    CHECK(faults == 3); /* An ACK without confirmed FC never opens readiness or retries FD. */
    original_reset(); lost_confirmation_ack = true; delay_step = 1000; stop_after = 3; run();
    CHECK(confirmations == 1 && recoveries == 1 && app.controller_ready && app.controller_trial_confirmed);
    CHECK(!writes && faults == 1 && !lifecycle.confirmation_uncertain); /* Fresh FC resolves the uncertain ACK. */
}
static void test_health_and_reset_no_replay(void) {
    original_reset(); queued_on_read = true; reset_at = 1000; delay_step = 1000; stop_after = 5; run();
    CHECK(reset_done && confirmations == 2 && faults == 1 && app.controller_ready);
    CHECK(writes == 6 && !controller.effect && !controller.white_brightness); /* No frames after reset detection. */
    CHECK(app.output_revision == 1 && app.completed_revision == 0 && app.reported_revision == 0);
    CHECK(app.desired.power && app.reported_valid && !app.reported.power); /* Desired scene remains visible, never replayed. */
    CHECK(app.controller_last_health_ms == 4000);
    original_reset(); delay_step = 1000; stop_after = 4; run();
    CHECK(status_reads == 6 && confirmations == 1 && !writes && app.controller_last_health_ms == 3000);
    /* Uptime wraps normally; backward movement within a healthy session does not. */
    lifecycle.status.uptime_ms = UINT32_MAX - 15u; now = (uint64_t)UINT32_MAX + 20;
    CHECK(health() == OKL_OK);
    boot_at = now - 1; CHECK(health() == OKL_VERIFY);
    reset(); delay_step = 1000; stop_after = 3; run();
    CHECK(versions == 3 && status_reads == 3 && !confirmations && app.controller_ready);
}
static void test_update_worker_gates(void) {
    reset(); journal_blocked = true; stop_after = 3; delay_step = 1000; run();
    CHECK(!transport_inits && !versions && !status_reads && !claims && !writes && !confirmations);
    reset(); app.updating = true; stop_after = 3; delay_step = 1000; run();
    CHECK(versions == 3 && status_reads == 3 && !writes); /* Health continues during HTTP reservation. */
    reset(); app.updating = true; off_on_read = true; run();
    CHECK(writes == 2 && !controller.effect && !controller.white_brightness && app.completed_revision == 1);
    for (unsigned failure = 0; failure < 3; ++failure) {
        reset(); job_queued = true; app.desired.power = true; app.desired.effect = KL_EFFECT_AURORA;
        app.output_revision = 9; stop_after = 3; delay_step = 1000;
        if (failure == 1) finish_durable = false;
        if (failure == 2) job_result = OKL_LOADER_UNRESOLVED;
        run(); CHECK(job_runs == 1 && job_finishes == 1 && !writes);
        CHECK(confirmations == (failure == 2 ? 0u : 1u));
        CHECK(app.controller_ready == (failure == 0));
        if (!failure) CHECK(!app.desired.power && app.desired.effect == KL_EFFECT_NONE && app.reported_valid && app.output_revision == 10);
        else CHECK(journal_blocked && app.output_revision == 9 && !app.controller_ready);
    }
}
int main(void) {
    test_startup(); test_startup_queued_transition(); test_setup_and_frame_failures();
    test_transition_completion();
    test_original_bootstrap(); test_original_rejections(); test_confirmation_readback_and_ambiguity();
    test_health_and_reset_no_replay();
    test_update_worker_gates();
    printf("%u worker assertions passed; actual source, no device I/O.\n", checks);
    return 0;
}
