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
static okl_result worker_owner(okl_nxp *, okl_owner *, uint64_t);
#define okl_nxp_execute worker_execute
#define okl_nxp_claim worker_claim
#define okl_nxp_release worker_release
#define okl_nxp_read_state worker_read
#define okl_nxp_recover worker_recover
#define okl_nxp_get_owner worker_owner
#include "../../firmware/main/update_indicator_output.c"
#include "../../firmware/main/worker.c"
#undef okl_nxp_execute
#undef okl_nxp_claim
#undef okl_nxp_release
#undef okl_nxp_read_state
#undef okl_nxp_recover
#undef okl_nxp_get_owner

app_context app;
static jmp_buf finished;
static void (*task_entry)(void *);
static uint64_t now;
static unsigned locks, claims, releases, reads, versions, publications, faults, recoveries;
static unsigned availability_calls;
static bool availability_ready;
static unsigned writes, delays, stop_after, fail_write, delay_step;
static okl_request sent[4096];
static okl_light_state controller;
static okl_result claim_result, read_result, release_result;
static bool poison_claim, poison_read, malformed_version, queued_on_read, retarget_after_failure;
static bool claimed, task_failure;
static bool poison_write;
static bool original, confirmation_missing, lost_confirmation_ack, malformed_status, status_unsupported;
static bool foreign_status_owner;
static unsigned denial_variant, claimed_status_variant;
static uint8_t legacy_minor;
static unsigned status_reads, part_reads, confirmations;
static uint32_t part_reply;
static okl_controller_status remote_status;
static uint64_t boot_at, reset_at;
static bool reset_done;
static bool journal_blocked, job_queued, finish_durable, finished_confirmed, off_on_read;
static unsigned transport_inits, job_runs, job_finishes, job_rejections;
static unsigned diagnostic_finishes;
static bool recovery_job, recovery_clear;
static unsigned recovery_jobs, recovery_finishes;
static uint8_t job_role;
static okl_loader_result job_result;
static bool job_read_only_rejection;
static kl_update_indicator upload;
static unsigned upload_at, upload_terminal_at, off_at;
static bool upload_success;
static uint64_t published_reboot_deadline;
static unsigned queued_duration, frame_latency, encoding_at, stolen_at, corrupt_read, owner_reads;
static okl_result owner_result;
static bool brightness_only_on_read;
static int boot_reason;
static unsigned brownout_started, brownout_verified, brownout_failed, brownout_bad_read;

int esp_reset_reason(void) { return boot_reason; }

void app_update_indicator_snapshot(kl_update_indicator *out) { CHECK(!locks); *out = upload; }
uint64_t app_update_reboot_deadline_us(void) { CHECK(!locks); return published_reboot_deadline; }

bool app_controller_update_blocked(void) { return journal_blocked; }
bool app_controller_update_take(app_controller_job *out) {
    if (!job_queued || (journal_blocked && !recovery_job)) return false;
    CHECK(!locks); job_queued = false; app.updating = true; app.controller_ready = false;
    memset(out, 0, sizeof(*out)); out->id = 7; out->image.version.component[1] = 1;
    out->image.role = job_role;
    out->recovery_only = recovery_job;
    return true;
}
void app_controller_worker_recover(okl_nxp *driver,const app_controller_job *job,
                                   const uint8_t identity[6],app_controller_worker_outcome *outcome) {
    CHECK(driver==&nxp && job->recovery_only && job->id==7 && identity==app.mac && !locks);
    CHECK(journal_blocked && !versions && !claims && !writes && !transport_inits);
    ++recovery_jobs;memset(outcome,0,sizeof(*outcome));
    CHECK(app_nxp_transport_init(driver,identity)==ESP_OK);
    controller=(okl_light_state){.temperature_kelvin=5000};
    outcome->legacy_reconciled=recovery_clear;
}
bool app_controller_recovery_finish(uint32_t id,const app_controller_worker_outcome *outcome) {
    CHECK(id==7 && outcome && !locks && recovery_jobs==1);++recovery_finishes;
    journal_blocked=!recovery_clear;app.updating=false;return recovery_clear;
}
okl_loader_result app_controller_worker_run(okl_nxp *driver, const app_controller_job *job,
                                          okl_loader_audit *audit, app_controller_worker_outcome *outcome) {
    CHECK(driver == &nxp && job->id == 7 && !app.controller_ready && app.updating && !locks); ++job_runs;
    memset(audit, 0, sizeof(*audit));
    *outcome=(app_controller_worker_outcome){.entry=job_read_only_rejection?
        APP_CONTROLLER_READ_ONLY_UNSUPPORTED:APP_CONTROLLER_MUTATION_ATTEMPTED,.synchronized=true};
    if (job_result == OKL_LOADER_OK) {
        original = true; boot_at = now; remote_status.trial_confirmed = 0;
        controller = (okl_light_state){.temperature_kelvin = 5000};
    }
    return job_result;
}
bool app_controller_update_reject(uint32_t id, const okl_loader_audit *audit,
                                  okl_loader_result result, const app_controller_worker_outcome *outcome) {
    CHECK(id==7 && audit && result==OKL_LOADER_INVALID && outcome && !locks);++job_rejections;
    if(!job_read_only_rejection || !finish_durable)return false;
    app.updating=false;journal_blocked=false;
    snprintf(app.operation,sizeof(app.operation),"idle");return true;
}
void app_controller_update_record_outcome(uint32_t id,const app_controller_worker_outcome *outcome) {
    CHECK(id==7 && outcome && !locks);
}
bool app_controller_update_finish(uint32_t id, const okl_loader_audit *audit, okl_loader_result result, bool confirmed, const char *error) {
    CHECK(id == 7 && audit && result == job_result && error && !app.controller_ready && !locks); ++job_finishes;
    finished_confirmed = confirmed; app.updating = false;
    bool success = finish_durable && confirmed && result == OKL_LOADER_OK;
    journal_blocked = !success;
    return success;
}
void app_controller_update_diagnostic_finish(uint32_t id, const okl_loader_audit *audit,
                                             okl_loader_result result, const app_controller_worker_outcome *outcome) {
    CHECK(id == 7 && audit && result == job_result && outcome && !locks);
    ++diagnostic_finishes; journal_blocked = true; app.updating = false;
    app.controller_ready = false; app.reported_valid = false; app.reported_fields = 0;
}

uint64_t app_now_ms(void) { return now; }
static uint64_t transport_now(void *unused) { (void)unused; return now * 1000; }
void app_lock(void) { CHECK(!locks); ++locks; }
void app_unlock(void) { CHECK(locks == 1); --locks; }
void app_event_locked(const char *actor, const char *event, const char *detail) {
    CHECK(locks == 1 && actor && event && detail);
    if (!strcmp(event, "command.failed")) ++faults;
    if (!strcmp(event, "brownout.off_started")) ++brownout_started;
    if (!strcmp(event, "brownout.off_verified")) ++brownout_verified;
    if (!strcmp(event, "brownout.recovery_failed")) ++brownout_failed;
}
void app_mqtt_availability(void) {
    CHECK(!locks); ++availability_calls;
    availability_ready=app.controller_ready && app.controller_connected && !app.updating && !journal_blocked;
}
void app_mqtt_publish(void) {
    app_mqtt_availability();
    CHECK(!locks); ++publications;
    /* A real API mutation is admitted only after the worker opens readiness. */
    if (queued_on_read && publications == 1 && app.controller_ready) {
        app.desired = kl_state_default(); app.desired.power = true; app.desired.mode = KL_COLOR;
        app.desired.rgb = (kl_rgb){10, 180, 90}; app.desired.brightness = 80;
        app.desired.transition_ms = queued_duration;
        app.output_revision = 1; snprintf(app.operation, sizeof(app.operation), "queued");
    }
    if (off_on_read && publications == 1 && app.controller_ready) {
        CHECK(app.updating); app.desired.power = false; ++app.output_revision;
    }
    if (brightness_only_on_read && publications == 1 && app.controller_ready) {
        CHECK(!app.rgb_confirmed && !(app.reported_fields & (KL_RGB | KL_BRIGHTNESS)));
        app.desired.brightness = 50; app.desired.transition_ms = 0; ++app.output_revision;
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
    if (delays == encoding_at) { app.output_encoding = KL_OUTPUT_LINEAR; ++app.output_revision; }
    if (delays == stolen_at) claimed = false;
    if (delays == upload_at) { CHECK(kl_update_indicator_begin(&upload,1000,now)); app.updating=true; }
    if (delays == upload_terminal_at) {
        if (upload_success) {
            CHECK(kl_update_indicator_advance(&upload,1000));
            published_reboot_deadline = (now + 1400) * 1000;
            CHECK(kl_update_indicator_verify(&upload,now));
        }
        else { kl_update_indicator_fail(&upload); app.updating=false; }
    }
    if (delays == off_at) { app.desired.power=false; ++app.output_revision; }
    if (delays >= stop_after) longjmp(finished, 1);
}
static void assert_deadline(uint64_t deadline, uint64_t maximum_us) {
    CHECK(!locks && deadline > now * 1000 && deadline <= now * 1000 + maximum_us);
}
static okl_result worker_claim(okl_nxp *driver, const uint8_t *name, size_t size, uint64_t deadline) {
    assert_deadline(deadline, 600000); ++claims;
    CHECK(size == 13 && !memcmp(name, "Open Keylight", size));
    if (claim_result == OKL_OK) claimed = true;
    if (poison_claim) driver->needs_recovery = 1;
    return claim_result;
}
static okl_result worker_release(okl_nxp *driver, uint64_t deadline) {
    assert_deadline(deadline, 600000); CHECK(!driver->needs_recovery); ++releases;
    if (release_result == OKL_OK) {
        if (claimed) foreign_status_owner = false;
        claimed = false;
    }
    return release_result;
}
static okl_result worker_read(okl_nxp *driver, okl_light_state *state, uint64_t deadline) {
    assert_deadline(deadline, 800000); ++reads;
    if (poison_read) driver->needs_recovery = 1;
    if (read_result == OKL_OK) {
        *state = controller;
        if (reads == corrupt_read) state->colors[0] ^= 1;
        if (brownout_bad_read == 1) state->white_brightness = 1;
        if (brownout_bad_read == 2) state->effect = 8;
    }
    return read_result;
}
static okl_result worker_recover(okl_nxp *driver, uint64_t deadline) {
    assert_deadline(deadline, 250000); ++recoveries; driver->needs_recovery = 0; return OKL_OK;
}
static okl_result worker_owner(okl_nxp *driver, okl_owner *owner, uint64_t deadline) {
    ++owner_reads;
    assert_deadline(deadline,150000);memset(owner,0,sizeof(*owner));owner->claimed=claimed;
    memcpy(owner->identity,driver->identity,6);owner->name_size=13;memcpy(owner->name,"Open Keylight",13);
    return owner_result;
}
static void put_word(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24); out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8); out[3] = (uint8_t)value;
}
static okl_result worker_execute(okl_nxp *driver, const okl_request *request, okl_reply *reply, uint64_t deadline) {
    assert_deadline(deadline, OKL_DEFAULT_TIMEOUT_US); CHECK(!driver->needs_recovery);
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
        if (foreign_status_owner && !claimed) {
            reply->report.status = 8;
            if (denial_variant == 1) reply->received = 0;
            if (denial_variant == 2) reply->report.status = 4;
            if (denial_variant == 3) reply->report.command_class = 3;
            if (denial_variant == 4) reply->report.opcode = 0xfd;
            if (denial_variant == 5) reply->report.size = 1;
            return OKL_OWNER_DENIED;
        }
        if (foreign_status_owner && claimed && claimed_status_variant) {
            if (claimed_status_variant == 1) { reply->report.status = 8; return OKL_OWNER_DENIED; }
            if (claimed_status_variant == 2) { reply->report.status = 4; return OKL_REMOTE; }
            if (claimed_status_variant == 3) { driver->needs_recovery = 1; return OKL_TIMEOUT; }
            if (claimed_status_variant == 4) { reply->report.status = 5; reply->report.size = 1; return OKL_REMOTE; }
            reply->acknowledged = 1; reply->report.status = 2; reply->report.size = 24;
            uint8_t *a = reply->report.arguments;
            memcpy(a, "OKLC", 4); a[4] = 1; a[6] = OKL_ROLE_SPI_DIAGNOSTIC;
            put_word(a + 8, OKL_CAP_RECOVERY_READY); put_word(a + 12, CONTROLLER_PART_ID);
            if (claimed_status_variant == 6) a[0] = 'X';
            if (claimed_status_variant == 7) { a[6] = OKL_ROLE_LIGHTING; put_word(a + 8, 3); put_word(a + 12, CONTROLLER_PART_ID ^ 1u); }
            return OKL_OK;
        }
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
    CHECK(claimed && writes < 4096); sent[writes++] = *request;
    if (fail_write == writes) {
        if (poison_write) { driver->needs_recovery = 1; return OKL_TIMEOUT; }
        return OKL_REMOTE; /* Correlated rejection leaves the link synchronized. */
    }
    const uint8_t *a = request->arguments;
    switch (request->command) {
    case OKL_SET_EFFECT: controller.effect = a[2]; controller.flags=a[3];controller.speed=a[4];controller.color_count = a[5]; memcpy(controller.colors, a + 6, 6); break;
    case OKL_SET_COLOR_BRIGHTNESS: controller.color_brightness = a[2]; break;
    case OKL_SET_WHITE_BRIGHTNESS: controller.white_brightness = a[2]; break;
    case OKL_SET_TEMPERATURE: controller.temperature_kelvin = (uint16_t)(a[2] * 256u + a[3]); break;
    case OKL_SET_FRAME: now += frame_latency; break;
    default: CHECK(false);
    }
    return OKL_OK;
}
static void reset(void) {
    memset(&app, 0, sizeof(app)); app.desired = kl_state_default(); app.mac[0] = 2;
    memset(&nxp, 0, sizeof(nxp)); native_effect = 0;
    memset(sent, 0, sizeof(sent)); task_entry = NULL;
    availability_calls=0; availability_ready=false;
    now = locks = claims = releases = reads = versions = publications = faults = recoveries = 0;
    writes = delays = fail_write = 0; stop_after = 1; delay_step = 5;
    claim_result = read_result = release_result = OKL_OK;
    poison_claim = poison_read = malformed_version = queued_on_read = retarget_after_failure = claimed = task_failure = false;
    poison_write = false;
    original = confirmation_missing = lost_confirmation_ack = malformed_status = reset_done = status_unsupported = false;
    foreign_status_owner = false; denial_variant = claimed_status_variant = 0;
    legacy_minor = 3;
    status_reads = part_reads = confirmations = 0; boot_at = reset_at = 0;
    part_reply = CONTROLLER_PART_ID;
    remote_status = (okl_controller_status){.role = OKL_ROLE_LIGHTING, .capabilities = 3,
        .part_id = CONTROLLER_PART_ID, .reset_cause = 0x13};
    journal_blocked = job_queued = finished_confirmed = off_on_read = false; finish_durable = true;
    transport_inits = job_runs = job_finishes = job_rejections = 0; job_result = OKL_LOADER_OK;
    diagnostic_finishes = 0; job_role = OKL_ROLE_LIGHTING;
    recovery_job=recovery_clear=false;recovery_jobs=recovery_finishes=0;
    job_read_only_rejection=false;
    memset(&upload,0,sizeof(upload));upload_at=upload_terminal_at=off_at=0;upload_success=false;
    published_reboot_deadline=0;
    queued_duration=600;frame_latency=encoding_at=stolen_at=corrupt_read=owner_reads=0;owner_result=OKL_OK;
    brightness_only_on_read=false;
    boot_reason=ESP_RST_SW;brownout_started=brownout_verified=brownout_failed=brownout_bad_read=0;
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
    CHECK(app.desired.rgb.r == 255 && app.desired.rgb.b == kl_srgb_reconstruct(32) && !strcmp(app.operation, "idle"));
    CHECK(!app.rgb_confirmed && !(app.reported_fields & (KL_RGB | KL_BRIGHTNESS)));
    reset(); controller.white_brightness = 10; run();
    CHECK(app.reported_valid && !app.rgb_confirmed && !(app.reported_fields & (KL_RGB | KL_BRIGHTNESS | KL_MODE)));
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
    CHECK(claims == 2 && releases == 1 && writes == 4 && !faults);
    CHECK(app.desired.rgb.r == 10 && app.desired.brightness == 80);
    CHECK(sent[0].command == OKL_SET_FRAME);
    CHECK(sent[0].arguments[5] == 102 && sent[0].arguments[6] == 0 && sent[0].arguments[7] == 12);
    CHECK(sent[2].command == OKL_SET_COLOR_BRIGHTNESS && sent[2].arguments[2] == 255);
    CHECK(!app.reported_valid && !app.completed_revision);
    for (unsigned level = 0; level <= 255; ++level) {
        reset(); queued_on_read = true; controller.color_brightness = (uint8_t)level; run();
        CHECK(sent[0].command == OKL_SET_FRAME && sent[0].arguments[5] == 255u * (level * 100u / 255u) / 100u);
        CHECK(sent[0].arguments[7] == 32u * (level * 100u / 255u) / 100u);
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
        if (failed == 1) CHECK(controller.effect == 1 && controller.color_brightness == 102);
    }
    reset(); queued_on_read = true; fail_write = 5; delay_step = 100; stop_after = 3; run();
    CHECK(faults == 1 && claims == 2 && writes == 5);
    CHECK(sent[4].command == OKL_SET_FRAME && sent[4].arguments[6] != 0); /* Rejected mid-fade sample. */
    CHECK(!app.controller_ready && app.completed_revision == 0); /* No command replay after failure. */
    for (unsigned failed = 1; failed <= 6; ++failed) {
        reset(); queued_on_read = true; fail_write = failed; poison_write = true; stop_after = 3; run();
        CHECK(writes == failed && claims == 2 && releases == 1 && nxp.needs_recovery);
        CHECK(faults == 1 && !recoveries && !app.completed_revision);
    }
}
static void test_transition_completion(void) {
    reset(); queued_on_read = true; delay_step = 2000; stop_after = 2; run();
    CHECK(writes == 6 && releases == 2 && !claimed && !faults);
    CHECK(sent[4].command == OKL_SET_FRAME && sent[5].command == OKL_SET_EFFECT && sent[5].arguments[2] == 1);
    CHECK(controller.color_brightness == 255);
    CHECK(controller.colors[1] == (kl_srgb_channel(180) * 80u + 50u) / 100u);
    CHECK(app.completed_revision == 1 && app.reported_revision == 1 && app.reported_valid && app.rgb_confirmed);
    CHECK(!strcmp(app.operation, "idle") && app.reported.rgb.g == 180 && app.reported.brightness == 80);
}
static void test_canonical_color_worker(void) {
    for (unsigned encoding = 0; encoding < 2; ++encoding) {
        reset(); queued_on_read = true; queued_duration = 0;
        app.output_encoding = (kl_output_encoding)encoding; run();
        CHECK(writes == 5 && !faults && app.completed_revision == 1 && app.rgb_confirmed);
        CHECK(sent[0].command == OKL_SET_FRAME); /* Seed while native Static is still selected. */
        CHECK(sent[1].command == OKL_SET_EFFECT && sent[1].arguments[2] == 8);
        CHECK(sent[3].command == OKL_SET_FRAME);
        CHECK(sent[4].command == OKL_SET_EFFECT && sent[4].arguments[2] == 1);
        CHECK(!memcmp(sent[3].arguments + 5, sent[4].arguments + 6, 3));
        CHECK(controller.color_brightness == 255 && !controller.white_brightness);
        CHECK(controller.colors[1] == (encoding ? 144 : (kl_srgb_channel(180) * 80u + 50u) / 100u));
        CHECK(app.desired.rgb.g == 180 && app.reported.rgb.g == 180 && app.reported.brightness == 80);
        /* Matching indicator restoration retains this logical tuple. Unknown
         * or externally changed raw bytes invalidate its provenance. */
        publish_native(&controller, false, 1);
        CHECK(app.reported.rgb.g == 180 && app.reported.brightness == 80 && color_intent.valid);
        controller.colors[0] ^= 1; publish_native(&controller, false, 1);
        CHECK(!color_intent.valid && app.reported.rgb.r == controller.colors[0]);
    }
    reset(); queued_on_read = true; controller.color_brightness = 255;
    delay_step = 100; stop_after = 2; run();
    CHECK(writes == 4 && sent[0].command == OKL_SET_FRAME && sent[0].arguments[5] == 255);
    for (unsigned i = 0; i < writes; ++i) CHECK(sent[i].command != OKL_SET_COLOR_BRIGHTNESS);
    /* Retarget uses the last ACKed raw frame without toggling mode/master. */
    reset(); queued_on_read = true; controller.color_brightness = 255;
    retarget_after_failure = true; delay_step = 100; stop_after = 3; run();
    CHECK(writes == 5 && owner_reads == 1 && !faults);
    CHECK(sent[3].command == OKL_SET_FRAME && sent[4].command == OKL_SET_FRAME);
    CHECK(!memcmp(sent[3].arguments + 5, sent[4].arguments + 5, 3));
    for (unsigned i = 2; i < writes; ++i) CHECK(sent[i].command == OKL_SET_FRAME);
    /* A foreign owner breaks framebuffer provenance even if effect8 remains. */
    reset(); queued_on_read = true; controller.color_brightness = 255;
    retarget_after_failure = true; stolen_at = 2; delay_step = 100; stop_after = 3; run();
    CHECK(owner_reads == 1 && writes == 8 && !faults);
    CHECK(sent[4].command == OKL_SET_COLOR_BRIGHTNESS && !sent[4].arguments[2]);
    CHECK(sent[5].command == OKL_SET_FRAME && !sent[5].arguments[5] && !sent[5].arguments[6] && !sent[5].arguments[7]);
    reset(); queued_on_read = true; controller.color_brightness = 255;
    retarget_after_failure = true; owner_result = OKL_IO; delay_step = 100; stop_after = 3; run();
    CHECK(owner_reads == 1 && writes == 4 && faults == 1 && !app.completed_revision);
    /* A slow ACK crossing the duration must not park a pre-deadline sample. */
    reset(); queued_on_read = true; controller.color_brightness = 255;
    frame_latency = 650; stop_after = 1; run();
    CHECK(writes == 3 && controller.effect == 8 && !app.completed_revision);
    reset(); queued_on_read = true; controller.color_brightness = 255;
    frame_latency = 650; stop_after = 2; run();
    CHECK(writes == 5 && controller.effect == 1 && app.completed_revision == 1);
    CHECK(controller.colors[1] == (kl_srgb_channel(180) * 80u + 50u) / 100u);
    /* Encoding revision applies forward conversion once, leaving chosen hex
     * and brightness unchanged and requiring a new exact raw readback. */
    reset(); queued_on_read = true; queued_duration = 0; encoding_at = 1; stop_after = 2; run();
    CHECK(!faults && app.completed_revision == 2 && app.reported_revision == 2);
    CHECK(controller.colors[1] == 144 && app.reported.rgb.g == 180 && app.reported.brightness == 80);
    CHECK(writes == 9); /* Second canonical handoff has no master write. */
    reset(); queued_on_read = true; queued_duration = 0; fail_write = 5; run();
    CHECK(faults == 1 && !app.completed_revision && !app.reported_valid);
    reset(); queued_on_read = true; queued_duration = 0; corrupt_read = 3; run();
    CHECK(faults == 1 && !app.completed_revision && !color_intent.valid);
    CHECK(app.reported_valid && strcmp(app.operation, "idle")); /* Fresh mismatch is not confirmation. */
}
static void test_reboot_color_adoption(void) {
    for (unsigned encoding = 0; encoding < 2; ++encoding) {
        for (unsigned raw = 0; raw <= 255; ++raw) {
            reset(); app.output_encoding = (kl_output_encoding)encoding;
            controller.color_brightness = 255; controller.colors[0] = (uint8_t)raw;
            brightness_only_on_read = true; run();
            unsigned logical = encoding == KL_OUTPUT_SRGB ? kl_srgb_reconstruct((uint8_t)raw) : raw;
            unsigned decoded = encoding == KL_OUTPUT_SRGB ? kl_srgb_channel((uint8_t)logical) : raw;
            CHECK(app.desired.rgb.r == logical && app.reported.rgb.r == logical);
            CHECK(controller.colors[0] == (decoded * 50u + 50u) / 100u);
            CHECK(abs((int)controller.colors[0] - (int)((raw * 50u + 50u) / 100u)) <= 1);
            CHECK(sent[0].command == OKL_SET_FRAME && sent[0].arguments[5] == raw);
            CHECK(app.completed_revision == 1 && app.reported_valid && app.rgb_confirmed);
            CHECK(app.reported.brightness == 50 && controller.color_brightness == 255 && !faults);
        }
    }
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
static void test_legacy_owner_admission(void) {
    /* Actual stock semantics: version87 is exempt, FC is not. No output or FD
     * may follow the denial until one verified claim exposes exact unsupported5. */
    reset(); foreign_status_owner = true; run();
    CHECK(claims == 1 && releases == 1 && versions == 1 && status_reads == 2 && reads == 1);
    CHECK(app.controller_ready && !strcmp(app.controller_backend, "legacy") && !claimed);
    CHECK(!writes && !confirmations && !faults && app.reported_valid);
    CHECK(app.reported.rgb.r == 255 && app.reported.rgb.b == kl_srgb_reconstruct(32) && app.desired.power);
    CHECK(health() == OKL_OK && claims == 1 && releases == 1); /* Released/unclaimed stock FC dispatches. */
    foreign_status_owner = true;
    CHECK(health() == OKL_OWNER_DENIED && claims == 1); /* Health never steals another owner. */
    for (unsigned variant = 1; variant <= 5; ++variant) {
        reset(); foreign_status_owner = true; denial_variant = variant; run();
        CHECK(!claims && !reads && !writes && !confirmations && !app.controller_ready);
    }
    reset(); foreign_status_owner = true; legacy_minor = 4; run();
    CHECK(!claims && !reads && !writes && !confirmations && !app.controller_ready);
    original_reset(); foreign_status_owner = true; run();
    CHECK(!claims && !reads && !writes && !confirmations && !app.controller_ready);
    reset(); foreign_status_owner = true; malformed_version = true; run();
    CHECK(!status_reads && !claims && !reads && !writes && !app.controller_ready);
    for (unsigned variant = 1; variant <= 7; ++variant) {
        reset(); foreign_status_owner = true; claimed_status_variant = variant; run();
        CHECK(claims == 1 && status_reads == 2 && !reads && !writes && !confirmations && !app.controller_ready);
        CHECK(releases == (variant == 3 ? 0u : 1u)); /* Never touch a poisoned transport. */
        if (variant == 5) CHECK(!strcmp(app.controller_status, "diagnostic"));
    }
    reset(); foreign_status_owner = true; claim_result = OKL_TIMEOUT; poison_claim = true; run();
    CHECK(claims == 1 && !releases && status_reads == 1 && !reads && !writes && !confirmations);
    reset(); foreign_status_owner = true; claim_result = OKL_OWNER_DENIED; run();
    CHECK(claims == 1 && releases == 1 && status_reads == 1 && !reads && !app.controller_ready);
    reset(); foreign_status_owner = true; release_result = OKL_NOT_OWNER; run();
    CHECK(claims == 1 && releases == 1 && reads == 1 && !writes && !confirmations && !app.controller_ready);
    reset(); foreign_status_owner = true; app.output_revision = 42; app.desired.effect = KL_EFFECT_AURORA; run();
    CHECK(app.controller_ready && !writes && !confirmations && app.completed_revision == 0);
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
    CHECK(faults == 1); /* Repeated health checks neither retry FD nor flood history. */
    original_reset(); lost_confirmation_ack = true; delay_step = 1000; stop_after = 3; run();
    CHECK(confirmations == 1 && recoveries == 1 && app.controller_ready && app.controller_trial_confirmed);
    CHECK(!writes && faults == 1 && !lifecycle.confirmation_uncertain); /* Fresh FC resolves the uncertain ACK. */
}
static void test_health_and_reset_no_replay(void) {
    original_reset(); queued_on_read = true; reset_at = 1000; delay_step = 1000; stop_after = 5; run();
    CHECK(reset_done && confirmations == 2 && faults == 1 && app.controller_ready);
    CHECK(writes == 4 && !controller.effect && !controller.white_brightness); /* No frames after reset detection. */
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
    for(unsigned cleared=0;cleared<2;++cleared) {
        reset();journal_blocked=true;job_queued=true;recovery_job=true;recovery_clear=cleared!=0;
        app.desired.power=true;app.desired.effect=KL_EFFECT_AURORA;app.output_revision=9;
        stop_after=4;delay_step=1000;run();
        CHECK(recovery_jobs==1 && recovery_finishes==1 && !job_runs && !job_finishes && !diagnostic_finishes);
        CHECK(!writes && !confirmations && app.output_revision==9 && app.controller_ready==(cleared!=0));
        CHECK(journal_blocked==(cleared==0) && transport_inits==1);
    }
    reset(); journal_blocked = true; stop_after = 3; delay_step = 1000; run();
    CHECK(!transport_inits && !versions && !status_reads && !claims && !writes && !confirmations);
    reset(); app.updating = true; stop_after = 3; delay_step = 1000; run();
    CHECK(versions == 3 && status_reads == 3 && !writes); /* Health continues during HTTP reservation. */
    reset(); app.updating = true; off_on_read = true; run();
    CHECK(writes == 2 && !controller.effect && !controller.white_brightness && app.completed_revision == 1);
    for (unsigned failure = 0; failure < 2; ++failure) {
        reset(); job_queued = true; job_role = OKL_ROLE_SPI_DIAGNOSTIC;
        stop_after = 4; delay_step = 1000; app.desired.power = true; app.output_revision = 9;
        if (failure) job_result = OKL_LOADER_UNRESOLVED;
        run();
        CHECK(job_runs == 1 && diagnostic_finishes == 1 && !job_finishes && !job_rejections);
        CHECK(!confirmations && !writes && journal_blocked && !app.controller_ready);
        CHECK(!app.reported_valid && app.output_revision == 9 && versions == 1);
    }
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
    for(unsigned failure=0;failure<2;++failure) {
        reset();job_queued=true;job_result=OKL_LOADER_INVALID;job_read_only_rejection=true;
        app.desired.power=true;app.desired.effect=KL_EFFECT_AURORA;app.output_revision=9;
        stop_after=4;delay_step=1000;if(failure)finish_durable=false;
        run();CHECK(job_runs==1 && job_rejections==1 && job_finishes==failure);
        CHECK(!writes && !confirmations && app.output_revision==9); /* Old effect never resumes. */
        CHECK(app.controller_ready==!failure && journal_blocked==(failure!=0));
        if(!failure)CHECK(versions>1 && reads>1 && !app.updating); /* Fresh ordinary bootstrap. */
    }
    for(unsigned failure=0;failure<3;++failure) {
        reset();app.updating=true;off_on_read=true;job_queued=true;
        job_result=OKL_LOADER_INVALID;job_read_only_rejection=true;stop_after=5;delay_step=1000;
        if(failure==1)fail_write=1;
        if(failure==2)finish_durable=false;
        run();CHECK(job_rejections==1);
        if(!failure)CHECK(writes==2 && !controller.effect && !controller.white_brightness && app.completed_revision==1);
        if(failure==1)CHECK(writes==1 && !app.completed_revision); /* Failed Off is not replayed after health recovery. */
        if(failure==2)CHECK(!writes && journal_blocked && !app.controller_ready);
    }
    reset();job_queued=true;job_result=OKL_LOADER_INVALID;job_read_only_rejection=true;
    app.desired.power=false;app.output_revision=9;stop_after=4;delay_step=1000;
    run();CHECK(!writes && app.controller_ready); /* Already consumed Off is not a new request. */
}
static void test_update_indicator_worker(void) {
    reset();
    kl_update_output stale={.deadline_us=9000000};
    app.controller_ready=true;
    CHECK(indicator_guard(&stale,0) && !stale.fixed_deadline);
    published_reboot_deadline=2400000;
    CHECK(indicator_guard(&stale,0) && stale.fixed_deadline && stale.deadline_us==2400000);
    stale.deadline_us=2300000;app.desired.recording_lock=true;
    CHECK(!indicator_guard(&stale,0) && stale.deadline_us==2300000);
    for(unsigned success=0;success<2;++success) {
        reset();okl_light_state saved=controller;
        upload_at=1;upload_terminal_at=8;upload_success=success!=0;stop_after=110;delay_step=20;
        run();CHECK(upload.generation==1 && upload.phase==(success?KL_UPDATE_VERIFIED:KL_UPDATE_FAILED));
        CHECK(same(&saved,&controller) && app.reported_valid && app.controller_ready && !claimed && !faults);
        CHECK(claims==2 && releases==2 && writes>10 && !app.output_revision && !app.completed_revision);
        bool blue=false,purple=false,red=false;
        for(unsigned n=0;n<writes;++n)if(sent[n].command==OKL_SET_FRAME){
            const uint8_t *a=sent[n].arguments;
            CHECK(!a[6]);blue|=!a[5] && a[7]!=0;purple|=a[5]!=0 && a[7]!=0;red|=a[5]!=0 && !a[7];
        }
        CHECK(blue && (success?purple && !red:red));
    }
    /* A new Off cancels the generation, runs once, and no later progress or
     * verified notification can restart the cosmetic animation. */
    reset();upload_at=1;upload_terminal_at=8;upload_success=true;off_at=4;stop_after=40;delay_step=20;
    run();CHECK(!controller.effect && !controller.white_brightness && !claimed && !faults);
    CHECK(app.completed_revision==1 && app.output_revision==1);
    unsigned last_off=0;
    for(unsigned n=0;n<writes;++n)if(sent[n].command==OKL_SET_EFFECT && !sent[n].arguments[2])last_off=n;
    CHECK(last_off==writes-1);
    /* Off already pending when the upload appears suppresses setup entirely. */
    reset();upload_at=off_at=1;stop_after=6;delay_step=20;run();
    CHECK(writes==2 && !controller.effect && app.completed_revision==1);
    /* Unknown external custom framebuffer cannot be captured by getters. */
    reset();controller.effect=8;controller.color_brightness=255;upload_at=1;stop_after=6;run();
    CHECK(!writes && claims==2 && releases==2 && app.controller_ready);
    reset();app.desired.recording_lock=true;upload_at=1;stop_after=6;run();
    CHECK(!writes && claims==1 && releases==1 && app.controller_ready);
    reset();upload_at=upload_terminal_at=1;stop_after=100;delay_step=20;run();
    CHECK(!faults && claims==2 && releases==2 && app.reported_valid);
    for(unsigned n=0;n<writes;++n)if(sent[n].command==OKL_SET_FRAME)
        CHECK(sent[n].arguments[5]>=43 && !sent[n].arguments[6] && !sent[n].arguments[7]);
    reset();upload_at=upload_terminal_at=2;retarget_after_failure=true;stop_after=110;delay_step=20;run();
    CHECK(claims==2 && releases==2 && !faults && app.completed_revision==1);
    CHECK(controller.colors[0]==kl_srgb_channel(5)*40u/100u &&
        controller.colors[1]==(kl_srgb_channel(220)*40u+50u)/100u &&
        controller.colors[2]==(kl_srgb_channel(35)*40u+50u)/100u);
    /* A poisoned first frame gets no master-enable, retry, or restoration. */
    reset();upload_at=1;fail_write=4;poison_write=true;stop_after=10;run();
    CHECK(writes==4 && !controller.color_brightness && nxp.needs_recovery && faults==1 && !recoveries);
    /* In-process custom animation resumes only after failed precommit upload;
     * a successful update parks its captured ACKed frame for boot adoption. */
    for(unsigned success=0;success<2;++success) {
        reset();queued_on_read=true;upload_at=2;upload_terminal_at=6;upload_success=success!=0;
        stop_after=110;delay_step=20;run();
        CHECK(!faults && app.controller_ready && !claimed);
        CHECK(controller.effect==1 && app.reported_valid && app.rgb_confirmed==!success);
        if(success)CHECK(controller.color_brightness==255 && app.completed_revision==0);
        else CHECK(controller.color_brightness==255 && app.completed_revision==1 &&
            controller.colors[1]==(kl_srgb_channel(180)*80u+50u)/100u);
    }
    /* Controller journal/job wins before the cosmetic worker and must not
     * emit an ESP progress frame while the other MCU is being rewritten. */
    reset();CHECK(kl_update_indicator_begin(&upload,1000,0));journal_blocked=true;
    stop_after=5;run();CHECK(!writes && !transport_inits && !claims);
}
static void test_availability_lifecycle(void) {
    reset();run();CHECK(availability_calls>=3 && availability_ready);
    unsigned before=availability_calls;fault(OKL_TIMEOUT,false);
    CHECK(availability_calls==before+1 && !availability_ready);
    app.controller_ready=app.controller_connected=true;before=availability_calls;
    lifecycle_observed("diagnostic",true);CHECK(availability_calls==before+1 && !availability_ready);
    reset();app.updating=true;run();CHECK(availability_calls>=3 && !availability_ready);
}
static void test_brownout_recovery(void) {
    for (unsigned backend=0;backend<2;++backend) {
        reset();original=backend!=0;remote_status.trial_confirmed=1;
        boot_reason=ESP_RST_BROWNOUT;controller.white_brightness=255;controller.color_brightness=255;
        app.desired.power=true;app.desired.effect=KL_EFFECT_AURORA;app.output_revision=12;
        stop_after=8;delay_step=1000;run();
        CHECK(writes==2 && sent[0].command==OKL_SET_WHITE_BRIGHTNESS && sent[0].arguments[2]==0);
        CHECK(sent[1].command==OKL_SET_EFFECT && sent[1].arguments[2]==0);
        CHECK(reads==1 && !confirmations && brownout_started==1 && brownout_verified==1 && !brownout_failed);
        CHECK(!app.desired.power && !app.reported.power && app.reported_valid && app.controller_ready);
        CHECK(app.reported_revision==12 && app.completed_revision==12 && !app.error[0]);
        CHECK(!brownout_recovery.required && brownout_recovery.attempted && !brownout_recovery.failed);
    }
    for (unsigned failure=0;failure<10;++failure) {
        reset();original=true;remote_status.trial_confirmed=1;boot_reason=ESP_RST_BROWNOUT;
        stop_after=8;delay_step=1000;
        if(failure==0) claim_result=OKL_OWNER_DENIED;
        if(failure==1) { claim_result=OKL_TIMEOUT;poison_claim=true; }
        if(failure>=2 && failure<=5) { fail_write=1+(failure&1);poison_write=failure>=4; }
        if(failure==6) { read_result=OKL_TIMEOUT;poison_read=true; }
        if(failure==7) brownout_bad_read=1;
        if(failure==8) brownout_bad_read=2;
        if(failure==9) release_result=OKL_IO;
        run();
        CHECK(!app.controller_ready && !app.reported_valid && !app.controller_connected);
        CHECK(brownout_recovery.failed && brownout_failed==1 && !brownout_verified && !confirmations);
        CHECK(claims==1 && versions==1 && !recoveries && writes<=2);
        CHECK(strstr(app.error,"Brownout recovery Off") && strstr(app.error,"no retry"));
        unsigned before=writes;kl_frame frame={0};uint32_t revision=0;
        CHECK(bootstrap(&frame,&revision,true,NULL)!=OKL_OK && writes==before && claims==1);
    }
    for(unsigned invalid=0;invalid<4;++invalid) {
        reset();original=true;boot_reason=ESP_RST_BROWNOUT;stop_after=5;delay_step=1000;
        if(invalid==0) remote_status.role=OKL_ROLE_SPI_DIAGNOSTIC;
        if(invalid==1) remote_status.capabilities=OKL_CAP_RECOVERY_READY;
        if(invalid==2) part_reply^=1;
        if(invalid==3) malformed_version=true;
        run();CHECK(!writes && !claims && !brownout_started && !app.controller_ready && brownout_recovery.failed);
    }
    const int ordinary[]={0,ESP_RST_POWERON,ESP_RST_SW,7};
    for(unsigned i=0;i<sizeof(ordinary)/sizeof(ordinary[0]);++i) {
        reset();boot_reason=ordinary[i];controller.color_brightness=255;run();
        CHECK(!writes && !brownout_started && !brownout_recovery.required && app.desired.power && app.controller_ready);
    }
    reset();original=true;controller=(okl_light_state){.temperature_kelvin=4500};boot_reason=ESP_RST_BROWNOUT;run();
    CHECK(writes==2 && confirmations==1 && brownout_verified==1 && app.controller_ready);
}
int main(void) {
    reset();
    fault(OKL_TIMEOUT, false);
    for (unsigned i=0;i<100;++i) fault(OKL_TIMEOUT, false);
    CHECK(faults==1 && !app.controller_ready && !app.reported_valid);
    fault(OKL_VERIFY, true); CHECK(faults==2);
    fault(OKL_VERIFY, true); CHECK(faults==2);
    snprintf(app.operation,sizeof(app.operation),"pending");
    fault(OKL_VERIFY, true); CHECK(faults==3);
    test_brownout_recovery();
    test_availability_lifecycle();
    test_startup(); test_startup_queued_transition(); test_setup_and_frame_failures();
    test_transition_completion();
    test_canonical_color_worker();
    test_reboot_color_adoption();
    test_original_bootstrap(); test_original_rejections(); test_confirmation_readback_and_ambiguity();
    test_legacy_owner_admission();
    test_health_and_reset_no_replay();
    test_update_worker_gates();
    test_update_indicator_worker();
    printf("%u worker assertions passed; actual source, no device I/O.\n", checks);
    return 0;
}
