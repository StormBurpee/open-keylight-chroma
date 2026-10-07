/* Execute the actual update.c with mocked IDF boundaries, not a parallel model. */
#include "update_mocks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/update.c"

static unsigned assertions, availability_calls;
static bool availability_updating;
#define CHECK(x) do { ++assertions; if (!(x)) { fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
app_context app;
static uint64_t now_ms, begin_delay, recv_delay, write_delay, end_delay, create_delay;
static int locked, nvs_failure, create_failure, boot_failure, sha_failure, socket_failure;
static int ota_failure, recv_failure, bad_digest, header_failure, accepted_present, partition_missing;
static unsigned nvs_calls, commit_calls, boot_calls, restart_calls, task_calls, deleted_tasks;
static unsigned begin_calls, write_calls, end_calls, abort_calls, recv_calls, notify_calls, setsock_calls;
static unsigned hash_bytes, response_status, deadline_poll_count, confirm_during_rollback;
static size_t recv_chunk;
static bool race_confirm_commit, race_rollback_confirm;
static unsigned written_bytes, write_failure_at;
static bool response_failure;
static char events[32][32];
static unsigned event_count;
static mock_timeval last_timeout;
static esp_partition_t partition = {1572864};
static esp_app_desc_t descriptor = {{0x42}};
static cJSON response_json;
static bool flash_owned;
static unsigned flash_calls, flash_releases, flash_failure_at, excluded_spi;
static uint64_t flash_wait_ms;
static unsigned flash_wait_at;

uint64_t app_flash_guard_deadline(void) { return now_ms * 1000 + APP_FLASH_GUARD_WAIT_US; }
esp_err_t app_flash_guard_enter(uint64_t deadline) {
    CHECK(!flash_owned && !locked);
    CHECK(deadline <= now_ms * 1000 + APP_FLASH_GUARD_WAIT_US && deadline > now_ms * 1000);
    ++flash_calls;
    if (!flash_wait_at || flash_calls == flash_wait_at) now_ms += flash_wait_ms;
    if (flash_calls == flash_failure_at || now_ms * 1000 >= deadline) return ESP_ERR_TIMEOUT;
    flash_owned = true;
    return ESP_OK;
}
void app_flash_guard_leave(void) { CHECK(flash_owned && !locked); flash_owned = false; ++flash_releases; }
static void flash_mutation(void) {
    /* A competing complete SPI exchange is unable to enter while an actual
     * OTA/NVS boundary is executing. Admission timing is tested separately. */
    CHECK(flash_owned); ++excluded_spi;
}

static kl_update_indicator indicator_snapshot(void) {
    kl_update_indicator snapshot;
    CHECK(!locked);
    app_update_indicator_snapshot(&snapshot);
    CHECK(!locked);
    if (snapshot.phase != KL_UPDATE_VERIFIED) CHECK(app_update_reboot_deadline_us() == 0);
    return snapshot;
}
static void indicator_receiving(unsigned bytes) {
    kl_update_indicator snapshot = indicator_snapshot();
    CHECK(snapshot.phase == KL_UPDATE_RECEIVING && snapshot.generation != 0);
    CHECK(snapshot.received_bytes == bytes);
    CHECK(kl_update_indicator_progress(&snapshot) <= KL_UPDATE_PROGRESS_RECEIVED);
    CHECK(app_update_reboot_deadline_us() == 0);
}

void app_mqtt_availability(void) { CHECK(!locked); ++availability_calls; availability_updating=app.updating; }
uint64_t app_now_ms(void) { return now_ms; }
void app_lock(void) { CHECK(!locked); locked = 1; }
void app_unlock(void) { CHECK(locked); locked = 0; }
void app_event_locked(const char *actor, const char *event, const char *detail) {
    (void)actor; (void)detail; CHECK(locked); CHECK(event_count < 32);
    snprintf(events[event_count++], 32, "%s", event);
}
const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock failure"; }
const esp_app_desc_t *esp_app_get_description(void) { return &descriptor; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out) {
    if (mode == NVS_READWRITE) flash_mutation();
    CHECK(!strcmp(name, "openkeylight")); ++nvs_calls; *out = mode + 1;
    return nvs_failure == 1 ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    (void)handle; CHECK(!strcmp(key, "accepted_elf")); CHECK(*size == 32);
    if (!accepted_present) return ESP_FAIL;
    memcpy(out, descriptor.app_elf_sha256, 32);
    if (accepted_present == 2) ((uint8_t *)out)[1] = 1;
    if (accepted_present == 3) *size = 31;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size) {
    (void)handle; flash_mutation(); ++nvs_calls; CHECK(trial_state == TRIAL_CONFIRMING);
    if (!strcmp(key, "accepted_elf")) { CHECK(size == 32); CHECK(!memcmp(value, descriptor.app_elf_sha256, 32)); }
    else CHECK(!strcmp(key, "config_v1") && size == sizeof(app_config));
    return nvs_failure == (!strcmp(key, "config_v1") ? 2 : 3) ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle; flash_mutation(); ++commit_calls;
    if (race_confirm_commit) {
        now_ms = trial_deadline_ms + 1000;
        CHECK(!trial_poll()); ++deadline_poll_count;
        CHECK(boot_calls == 0 && app_trial_pending());
        CHECK(app_trial_confirm() == ESP_ERR_INVALID_STATE);
    }
    return nvs_failure == 4 ? ESP_FAIL : ESP_OK;
}
void nvs_close(nvs_handle_t handle) { if (handle == NVS_READWRITE + 1) CHECK(flash_owned); }
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *out) {
    (void)arg; CHECK(priority == 4); ++task_calls; now_ms += create_delay;
    if (!strcmp(name, "update_trial")) { CHECK(entry == trial_task && stack == 3072 && locked); }
    else CHECK(entry == reboot_task && stack == 2048 && !boot_calls);
    if (create_failure) return 0;
    if (out) *out = (void *)(uintptr_t)0x1234;
    return pdPASS;
}
void vTaskDelay(unsigned ticks) { now_ms += ticks; }
void vTaskDelete(TaskHandle_t handle) { (void)handle; ++deleted_tasks; }
unsigned ulTaskNotifyTake(int clear, unsigned timeout) { CHECK(clear && timeout == portMAX_DELAY); return 1; }
void xTaskNotifyGive(TaskHandle_t handle) {
    CHECK(handle == (void *)(uintptr_t)0x1234); CHECK(boot_calls == 1 && !boot_failure);
    kl_update_indicator snapshot = indicator_snapshot();
    CHECK(snapshot.phase == KL_UPDATE_VERIFIED && snapshot.received_bytes == written_bytes);
    CHECK(kl_update_indicator_progress(&snapshot) == KL_UPDATE_PROGRESS_VERIFIED);
    CHECK(app_update_reboot_deadline_us() == (snapshot.verified_ms + 1400) * 1000);
    app_lock(); CHECK(app_update_reboot_deadline_us() == reboot_io_deadline_us); app_unlock();
    ++notify_calls;
}
void esp_restart(void) { ++restart_calls; /* Return deliberately tests defensive failure handling. */ }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *previous) { CHECK(!previous); return partition_missing ? NULL : &partition; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *selected) {
    flash_mutation(); CHECK(selected == &partition); ++boot_calls;
    if (app.updating) indicator_receiving(written_bytes); /* Not verified before this call returns. */
    if (race_rollback_confirm) { CHECK(app_trial_confirm() == ESP_ERR_INVALID_STATE); ++confirm_during_rollback; }
    return boot_failure ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_begin(const esp_partition_t *selected, size_t size, esp_ota_handle_t *handle) {
    flash_mutation(); CHECK(selected == &partition && size == OTA_WITH_SEQUENTIAL_WRITES);
    ++begin_calls; now_ms += begin_delay; *handle = 1;
    written_bytes = 0;
    indicator_receiving(0);
    return ota_failure == 1 ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *value, size_t size) {
    flash_mutation(); CHECK(handle == 1 && value && size && size <= 2048); ++write_calls; now_ms += write_delay;
    indicator_receiving(written_bytes); /* Hashing/receiving does not publish a flash-write success. */
    if (ota_failure == 2 || write_calls == write_failure_at) return ESP_FAIL;
    written_bytes += (unsigned)size;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle) {
    flash_mutation(); CHECK(handle == 1); ++end_calls; now_ms += end_delay;
    indicator_receiving(written_bytes);
    CHECK(kl_update_indicator_progress(&update_indicator) == KL_UPDATE_PROGRESS_RECEIVED);
    return ota_failure == 3 ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle) { CHECK(handle == 1); ++abort_calls; return ESP_OK; }
int httpd_req_to_sockfd(httpd_req_t *request) { CHECK(request); return socket_failure == 1 ? -1 : 7; }
int getsockopt(int socket, int level, int option, void *value, socklen_t *size) {
    CHECK(socket == 7 && level == SOL_SOCKET && option == SO_RCVTIMEO && *size == sizeof(mock_timeval));
    mock_timeval *timeout = value; timeout->tv_sec = 5; timeout->tv_usec = 0;
    return socket_failure == 2 ? -1 : 0;
}
int setsockopt(int socket, int level, int option, const void *value, socklen_t size) {
    CHECK(socket == 7 && level == SOL_SOCKET && option == SO_RCVTIMEO && size == sizeof(mock_timeval));
    last_timeout = *(const mock_timeval *)value; ++setsock_calls;
    CHECK(last_timeout.tv_sec <= 5 && last_timeout.tv_usec < 1000000);
    if (socket_failure == 3 || (socket_failure == 4 && setsock_calls == 2)) return -1;
    return 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *name, char *value, size_t size) {
    (void)request;
    if (!strcmp(name, "Content-Type")) snprintf(value, size, "%s", header_failure == 1 ? "text/plain" : "application/octet-stream");
    else { CHECK(size == 65); for (unsigned i = 0; i < 64; ++i) value[i] = i % 2 ? 'b' : 'a'; value[64] = 0;
        if (bad_digest) value[0] = '0';
        if (header_failure == 2) value[2] = 'G';
        if (header_failure == 3) value[63] = 0;
    }
    return header_failure == 4 ? ESP_FAIL : ESP_OK;
}
int httpd_req_recv(httpd_req_t *request, char *buffer, size_t size) {
    CHECK(!flash_owned);
    (void)request; ++recv_calls; now_ms += recv_delay;
    if (recv_failure == 1) return -1;
    if (recv_failure == 2) return 0;
    if (recv_failure == 3) return (int)size + 1;
    if (size > recv_chunk) size = recv_chunk;
    memset(buffer, 0xe9, size); return (int)size;
}
esp_err_t http_error(httpd_req_t *request, int status, const char *message) { (void)request; CHECK(message); response_status = (unsigned)status; return ESP_FAIL; }
esp_err_t http_json(httpd_req_t *request, int status, cJSON *json) {
    (void)request; CHECK(json->accepted && json->rebooting); response_status = (unsigned)status;
    kl_update_indicator snapshot = indicator_snapshot();
    CHECK(snapshot.phase == KL_UPDATE_VERIFIED && end_calls == 1 && boot_calls == 1 && !boot_failure);
    return response_failure ? ESP_FAIL : ESP_OK;
}
cJSON *cJSON_CreateObject(void) { memset(&response_json, 0, sizeof(response_json)); return &response_json; }
void cJSON_AddBoolToObject(cJSON *json, const char *key, bool value) { if (!strcmp(key, "accepted")) json->accepted = value; else { CHECK(!strcmp(key, "rebooting")); json->rebooting = value; } }
void mbedtls_sha256_init(mbedtls_sha256_context *sha) { sha->bytes = 0; }
int mbedtls_sha256_starts(mbedtls_sha256_context *sha, int sha224) { (void)sha; CHECK(!sha224); return sha_failure == 1; }
int mbedtls_sha256_update(mbedtls_sha256_context *sha, const void *data, size_t size) { CHECK(data); sha->bytes += (unsigned)size; hash_bytes += (unsigned)size; return sha_failure == 2; }
int mbedtls_sha256_finish(mbedtls_sha256_context *sha, unsigned char *out) {
    CHECK(sha->bytes); memset(out, 0xab, 32);
    indicator_receiving(written_bytes);
    return sha_failure == 3;
}
void mbedtls_sha256_free(mbedtls_sha256_context *sha) { (void)sha; }

static void reset(void) {
    CHECK(!flash_owned);
    flash_calls = flash_releases = flash_failure_at = excluded_spi = flash_wait_at = 0; flash_wait_ms = 0;
    CHECK(!locked); memset(&app, 0, sizeof(app));
    availability_calls=0;availability_updating=false;
    now_ms = 100; begin_delay = recv_delay = write_delay = end_delay = create_delay = 0;
    nvs_failure = create_failure = boot_failure = sha_failure = socket_failure = ota_failure = 0;
    recv_failure = bad_digest = header_failure = accepted_present = partition_missing = 0;
    nvs_calls = commit_calls = boot_calls = restart_calls = task_calls = deleted_tasks = 0;
    begin_calls = write_calls = end_calls = abort_calls = recv_calls = notify_calls = setsock_calls = 0;
    hash_bytes = response_status = deadline_poll_count = confirm_during_rollback = event_count = 0;
    recv_chunk = 2048; race_confirm_commit = race_rollback_confirm = false;
    trial_state = TRIAL_UNINITIALIZED; trial_deadline_ms = 0; atomic_store(&trial_pending, true);
    memset(&update_indicator, 0, sizeof(update_indicator));
    atomic_store(&reboot_io_deadline_published, false); reboot_io_deadline_us = 0;
    written_bytes = write_failure_at = 0; response_failure = false;
}
static void accepted(void) { accepted_present = 1; app_trial_start(); CHECK(!app_trial_pending()); }
static void trial_tests(void) {
    reset(); CHECK(app_trial_pending()); CHECK(app_trial_confirm() == ESP_ERR_INVALID_STATE);
    app_trial_start(); CHECK(trial_state == TRIAL_PENDING && task_calls == 1);
    uint64_t deadline = trial_deadline_ms; now_ms += 10000; app_trial_start(); CHECK(trial_deadline_ms == deadline && task_calls == 1);
    app_lock(); CHECK(app_trial_pending()); app_unlock();
    now_ms = deadline - 1; CHECK(!trial_poll()); race_confirm_commit = true;
    CHECK(app_trial_confirm() == ESP_OK); CHECK(trial_state == TRIAL_CONFIRMED && !app_trial_pending());
    CHECK(deadline_poll_count == 1 && !boot_calls && trial_poll());
    unsigned calls = nvs_calls; CHECK(app_trial_confirm() == ESP_OK && nvs_calls == calls);
    for (unsigned ordering = 0; ordering < 2; ++ordering) {
        reset(); app_trial_start(); now_ms = trial_deadline_ms; race_rollback_confirm = true;
        if (ordering) CHECK(app_trial_confirm() == ESP_ERR_TIMEOUT); else CHECK(trial_poll());
        CHECK(boot_calls == 1 && restart_calls == 1 && confirm_during_rollback == 1);
        CHECK(app_trial_pending() && !commit_calls && trial_state == TRIAL_FAILED);
        CHECK(trial_poll() && boot_calls == 1); CHECK(app_trial_confirm() == ESP_ERR_INVALID_STATE);
    }
    for (int failure = 1; failure <= 4; ++failure) {
        reset(); app_trial_start(); nvs_failure = failure; CHECK(app_trial_confirm() != ESP_OK);
        CHECK(trial_state == TRIAL_PENDING && app_trial_pending() && !boot_calls);
        nvs_failure = 0; CHECK(app_trial_confirm() == ESP_OK && !app_trial_pending());
        reset(); app_trial_start(); race_confirm_commit = failure == 4; nvs_failure = failure;
        now_ms = trial_deadline_ms - 1; CHECK(app_trial_confirm() != ESP_OK);
        now_ms = trial_deadline_ms; CHECK(trial_poll() && boot_calls == 1);
    }
    for (int failure = 0; failure < 3; ++failure) {
        reset(); create_failure = 1; boot_failure = failure == 1; partition_missing = failure == 2;
        app_trial_start(); CHECK(task_calls == 1 && app_trial_pending() && trial_state == TRIAL_FAILED);
        CHECK(boot_calls == (failure == 2 ? 0u : 1u)); CHECK(!commit_calls);
        CHECK(app_trial_confirm() == ESP_ERR_INVALID_STATE);
    }
    for (int persisted = 1; persisted <= 3; ++persisted) {
        reset(); accepted_present = persisted; app_trial_start();
        CHECK(app_trial_pending() == (persisted != 1)); CHECK(task_calls == (persisted == 1 ? 0u : 1u));
    }
    reset(); app_trial_start(); trial_task(NULL); CHECK(boot_calls == 1 && deleted_tasks == 1 && now_ms == trial_deadline_ms);
}
static void upload_tests(void) {
    httpd_req_t request = {8193};
    reset(); accepted(); CHECK(http_update(&request) == ESP_OK);
    CHECK(availability_calls==2 && availability_updating);
    CHECK(response_status == 202 && hash_bytes == request.content_len && write_calls == 5);
    CHECK(end_calls == 1 && !abort_calls && boot_calls == 1 && notify_calls == 1 && app.updating);
    CHECK(last_timeout.tv_sec == 5 && !last_timeout.tv_usec);
    reboot_task(NULL); CHECK(restart_calls == 1 && deleted_tasks == 1);
    for (int fault = 0; fault < 18; ++fault) {
        reset(); accepted(); request.content_len = 288;
        if (fault < 4) header_failure = fault + 1;
        else if (fault == 4) request.content_len = 287;
        else if (fault == 5) request.content_len = 1572865;
        else if (fault == 6) atomic_store(&trial_pending, true);
        else if (fault == 7) app.updating = true;
        else if (fault == 8) partition_missing = 1;
        else if (fault < 13) socket_failure = fault - 8;
        else if (fault < 16) recv_failure = fault - 12;
        else if (fault == 16) bad_digest = 1;
        else create_failure = 1;
        CHECK(http_update(&request) != ESP_OK); CHECK(!boot_calls && !notify_calls);
        CHECK(!availability_calls || (availability_calls==2 && !availability_updating));
        CHECK(fault == 7 || !app.updating); CHECK(fault != 17 || (response_status == 503 && end_calls == 1));
        kl_update_indicator snapshot = indicator_snapshot();
        CHECK(snapshot.phase == (begin_calls ? KL_UPDATE_FAILED : KL_UPDATE_IDLE));
        CHECK(snapshot.received_bytes == written_bytes && kl_update_indicator_progress(&snapshot) < KL_UPDATE_PROGRESS_VERIFIED);
    }
    for (int failure = 1; failure <= 3; ++failure) {
        reset(); accepted(); ota_failure = failure; CHECK(http_update(&request) != ESP_OK);
        CHECK(!boot_calls && !notify_calls && !app.updating);
        CHECK(abort_calls == (failure == 2 ? 1u : 0u));
        CHECK(indicator_snapshot().phase == KL_UPDATE_FAILED);
        reset(); accepted(); sha_failure = failure; CHECK(http_update(&request) != ESP_OK);
        CHECK(abort_calls == 1 && !end_calls && !boot_calls);
        CHECK(indicator_snapshot().phase == KL_UPDATE_FAILED);
    }
    for (unsigned stage = 0; stage < 5; ++stage) {
        reset(); accepted(); request.content_len = 288;
        if (!stage) begin_delay = UPLOAD_DURATION_MS;
        if (stage == 1) recv_delay = UPLOAD_DURATION_MS;
        if (stage == 2) write_delay = UPLOAD_DURATION_MS;
        if (stage == 3) end_delay = UPLOAD_DURATION_MS;
        if (stage == 4) create_delay = UPLOAD_DURATION_MS;
        CHECK(http_update(&request) != ESP_OK && response_status == 408);
        CHECK(!boot_calls && !notify_calls && !app.updating);
        CHECK(deleted_tasks == (stage == 4 ? 1u : 0u));
        CHECK(indicator_snapshot().phase == KL_UPDATE_FAILED);
    }
    reset(); accepted(); recv_chunk = 1; recv_delay = 1000; request.content_len = 288;
    CHECK(http_update(&request) != ESP_OK && response_status == 408);
    CHECK(recv_calls == 120 && write_calls == 119 && !end_calls && abort_calls == 1);
    reset(); accepted(); begin_delay = UPLOAD_DURATION_MS - 80; recv_delay = 80;
    CHECK(http_update(&request) != ESP_OK && !write_calls && !boot_calls);
    reset(); accepted(); boot_failure = 1; CHECK(http_update(&request) != ESP_OK);
    CHECK(boot_calls == 1 && deleted_tasks == 1 && !notify_calls && !app.updating);
    CHECK(indicator_snapshot().phase == KL_UPDATE_FAILED);
    /* All short read boundaries must preserve exactly the declared byte count. */
    for (size_t chunk = 1; chunk <= 300; ++chunk) {
        reset(); accepted(); recv_chunk = chunk; request.content_len = 301;
        CHECK(http_update(&request) == ESP_OK && hash_bytes == 301 && notify_calls == 1);
    }
}

static void indicator_publication_tests(void) {
    httpd_req_t request = {8193};
    reset(); accepted();
    kl_update_indicator before = indicator_snapshot();
    CHECK(before.phase == KL_UPDATE_IDLE && !before.generation && !before.total_bytes);
    app_update_indicator_snapshot(NULL); CHECK(!locked);
    write_failure_at = 3;
    CHECK(http_update(&request) != ESP_OK && !notify_calls && !boot_calls);
    kl_update_indicator failed = indicator_snapshot();
    CHECK(failed.phase == KL_UPDATE_FAILED && failed.received_bytes == 4096 && failed.total_bytes == 8193);
    CHECK(kl_update_indicator_progress(&failed) == 494 && failed.generation == 1);
    failed.received_bytes = 0; /* The consumer receives a copy, never the live upload state. */
    CHECK(indicator_snapshot().received_bytes == 4096);
    header_failure = 1;
    CHECK(http_update(&request) != ESP_OK);
    CHECK(indicator_snapshot().generation == 1 && indicator_snapshot().received_bytes == 4096);
    header_failure = 0; write_failure_at = 0;
    CHECK(http_update(&request) == ESP_OK);
    CHECK(indicator_snapshot().generation == 2 && indicator_snapshot().phase == KL_UPDATE_VERIFIED);
    CHECK(indicator_snapshot().received_bytes == request.content_len);

    reset(); accepted(); response_failure = true;
    CHECK(http_update(&request) != ESP_OK); /* A lost HTTP response cannot undo accepted boot selection. */
    CHECK(indicator_snapshot().phase == KL_UPDATE_VERIFIED && app.updating && notify_calls == 1);
    uint64_t cutoff = app_update_reboot_deadline_us();
    unsigned received = written_bytes;
    now_ms += 20;
    CHECK(http_update(&request) != ESP_OK && response_status == 409);
    CHECK(indicator_snapshot().phase == KL_UPDATE_VERIFIED && written_bytes == received && notify_calls == 1);
    CHECK(app_update_reboot_deadline_us() == cutoff); /* Rejected work cannot move it. */
    uint64_t time_before_reboot = now_ms;
    reboot_task(NULL);
    CHECK(now_ms == time_before_reboot + 1500 && restart_calls == 1);
    reset(); accepted(); now_ms = UINT64_C(1) << 33;
    CHECK(http_update(&request) == ESP_OK);
    CHECK(app_update_reboot_deadline_us() == (now_ms + 1400) * 1000);
    CHECK(app_update_reboot_deadline_us() > UINT32_MAX);
}
static void flash_exclusion_tests(void) {
    httpd_req_t request = {288};
    /* Begin, the only write, finalize, and boot selection each need their own
     * admission. A rejected admission must never execute or retry that call. */
    for (unsigned fail = 1; fail <= 4; ++fail) {
        reset(); accepted(); flash_failure_at = fail;
        CHECK(http_update(&request) != ESP_OK);
        CHECK(response_status == 408 && !notify_calls && !flash_owned);
        CHECK(flash_calls == fail && flash_releases == fail - 1);
        CHECK(begin_calls == (fail > 1) && write_calls == (fail > 2));
        CHECK(end_calls == (fail > 3) && boot_calls == 0);
        CHECK(abort_calls == (fail == 2 || fail == 3));
        CHECK(deleted_tasks == (fail == 4));
        CHECK(indicator_snapshot().phase == KL_UPDATE_FAILED);
    }
    reset(); accepted(); flash_wait_ms = 600;
    CHECK(http_update(&request) == ESP_OK);
    CHECK(flash_calls == 4 && flash_releases == 4 && excluded_spi == 4 && !flash_owned);
    CHECK(recv_calls == 1 && written_bytes == 288);
    /* Admission delay still consumes the overall 120-second upload budget. */
    reset(); accepted(); flash_wait_ms = 4500; recv_chunk = 1;
    CHECK(http_update(&request) != ESP_OK && response_status == 408);
    CHECK(!notify_calls && !boot_calls && !flash_owned && abort_calls == 1);
    CHECK(now_ms >= UPLOAD_DURATION_MS && now_ms < UPLOAD_DURATION_MS + 5000);
    for (unsigned queued = 2; queued <= 4; ++queued) {
        reset(); accepted();
        begin_delay = UPLOAD_DURATION_MS - 200;
        flash_wait_at = queued; flash_wait_ms = 300;
        CHECK(http_update(&request) != ESP_OK && response_status == 408);
        CHECK(!boot_calls && !notify_calls && !flash_owned);
        CHECK(write_calls == (queued > 2) && end_calls == (queued > 3));
        CHECK(abort_calls == (queued < 4));
        CHECK(deleted_tasks == (queued == 4));
    }

    reset(); app_trial_start();
    unsigned reads = nvs_calls; flash_failure_at = 1;
    CHECK(app_trial_confirm() == ESP_ERR_TIMEOUT && app_trial_pending());
    CHECK(nvs_calls == reads && !commit_calls && !flash_owned && trial_state == TRIAL_PENDING);
    reset(); app_trial_start(); flash_failure_at = 1; now_ms = trial_deadline_ms;
    CHECK(trial_poll());
    CHECK(trial_state == TRIAL_FAILED && !boot_calls && !restart_calls && !flash_owned);
}
int main(void) {
    trial_tests(); upload_tests(); indicator_publication_tests(); flash_exclusion_tests();
    printf("PASS %u assertions against actual update.c\n", assertions); return 0;
}
