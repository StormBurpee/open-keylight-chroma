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

uint64_t app_now_ms(void) { return now; }
static uint64_t transport_now(void *unused) { (void)unused; return now * 1000; }
void app_lock(void) { CHECK(!locks); ++locks; }
void app_unlock(void) { CHECK(locks == 1); --locks; }
void app_event_locked(const char *actor, const char *event, const char *detail) {
    CHECK(locks == 1 && actor && event && detail);
    if (!strcmp(event, "command.failed")) ++faults;
}
void app_mqtt_publish(void) { CHECK(!locks); ++publications; }
esp_err_t app_nxp_transport_init(okl_nxp *driver, const uint8_t mac[6]) {
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
    if (queued_on_read && reads == 1) {
        app.desired = kl_state_default(); app.desired.power = true; app.desired.mode = KL_COLOR;
        app.desired.rgb = (kl_rgb){10, 180, 90}; app.desired.brightness = 80;
        app.output_revision = 1; strcpy(app.operation, "queued");
    }
    if (poison_read) driver->needs_recovery = 1;
    if (read_result == OKL_OK) *state = controller;
    return read_result;
}
static okl_result worker_recover(okl_nxp *driver, uint64_t deadline) {
    bounded(deadline, 250000); ++recoveries; driver->needs_recovery = 0; return OKL_OK;
}
static okl_result worker_execute(okl_nxp *driver, const okl_request *request, okl_reply *reply, uint64_t deadline) {
    bounded(deadline, OKL_DEFAULT_TIMEOUT_US); CHECK(!driver->needs_recovery);
    memset(reply, 0, sizeof(*reply));
    if (request->command == OKL_GET_FIRMWARE) {
        ++versions; reply->received = 1; reply->acknowledged = 1; reply->report.status = 2;
        reply->report.command_class = 0; reply->report.opcode = 0x87;
        reply->report.size = malformed_version ? 3 : 4;
        reply->report.arguments[0] = 1; reply->report.arguments[1] = 3;
        return OKL_OK;
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
        CHECK(!writes && claims == 1 && !recoveries && faults == 1);
        CHECK(!app.reported_valid && !app.controller_connected && !strcmp(app.operation, "error"));
        CHECK(releases == (unsigned)(failure != 1 && failure != 2));
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
    CHECK(faults == 1 && claims == 3 && writes == 13);
    CHECK(sent[6].command == OKL_SET_FRAME && sent[6].arguments[6] != 0); /* Rejected mid-fade sample. */
    CHECK(sent[10].command == OKL_SET_FRAME);
    CHECK(sent[10].arguments[5] == 102 && sent[10].arguments[6] == 0 && sent[10].arguments[7] == 13);
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
int main(void) {
    test_startup(); test_startup_queued_transition(); test_setup_and_frame_failures();
    test_transition_completion();
    printf("%u worker assertions passed; actual source, no device I/O.\n", checks);
    return 0;
}
