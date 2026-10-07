#include "http_internal.h"
#include "update_indicator.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum trial_state { TRIAL_UNINITIALIZED, TRIAL_PENDING, TRIAL_CONFIRMING, TRIAL_CONFIRMED,
                   TRIAL_REVERTING, TRIAL_FAILED };
/* State/deadline transitions use app.mutex. Keep this separate atomic view:
 * device_json calls app_trial_pending while already holding that mutex. */
static enum trial_state trial_state;
static uint64_t trial_deadline_ms;
static atomic_bool trial_pending = true;
static kl_update_indicator update_indicator;

void app_update_indicator_snapshot(kl_update_indicator *out) {
    if (!out) return;
    app_lock();
    *out = update_indicator;
    app_unlock();
}

#define TRIAL_DURATION_MS UINT64_C(180000)
#define UPLOAD_DURATION_MS UINT64_C(120000)
#define UPLOAD_RECEIVE_SLICE_MS UINT64_C(5000)

bool app_trial_pending(void) { return atomic_load(&trial_pending); }

/* Called only by the winner of the PENDING -> REVERTING transition. The
 * installed older bootloader provides no automatic trial rollback. */
static void trial_revert(void) {
    const esp_partition_t *previous = esp_ota_get_next_update_partition(NULL);
    if (previous && esp_ota_set_boot_partition(previous) == ESP_OK) esp_restart();
    app_lock();
    trial_state = TRIAL_FAILED;
    app_event_locked("update", "rollback.failed", "Fallback could not restart; trial confirmation and OTA remain blocked");
    app_unlock();
}

esp_err_t app_trial_confirm(void) {
    app_lock();
    if (trial_state == TRIAL_CONFIRMED) { app_unlock(); return ESP_OK; }
    if (trial_state != TRIAL_PENDING) { app_unlock(); return ESP_ERR_INVALID_STATE; }
    if (app_now_ms() >= trial_deadline_ms) {
        trial_state = TRIAL_REVERTING;
        app_unlock();
        trial_revert();
        return ESP_ERR_TIMEOUT;
    }
    /* The owner wins before the deadline. Expiry cannot select another boot
     * slot while this bounded NVS operation completes, even if it crosses the
     * deadline. Failure releases the decision back to the expiry task. */
    trial_state = TRIAL_CONFIRMING;
    app_config config = app.config;
    app_unlock();
    nvs_handle_t handle;
    esp_err_t result = nvs_open("openkeylight", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, "config_v1", &config, sizeof(config));
        if (result == ESP_OK) result = nvs_set_blob(handle, "accepted_elf", esp_app_get_description()->app_elf_sha256, 32);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    app_lock();
    if (result == ESP_OK) {
        trial_state = TRIAL_CONFIRMED;
        atomic_store(&trial_pending, false);
        app_event_locked("owner", "update.confirmed", "Trial application accepted");
    } else trial_state = TRIAL_PENDING;
    app_unlock();
    return result;
}

static bool trial_poll(void) {
    app_lock();
    bool revert = trial_state == TRIAL_PENDING && app_now_ms() >= trial_deadline_ms;
    if (revert) trial_state = TRIAL_REVERTING;
    bool finished = trial_state != TRIAL_PENDING && trial_state != TRIAL_CONFIRMING;
    app_unlock();
    if (revert) trial_revert();
    return finished;
}

static void trial_task(void *unused) {
    (void)unused;
    while (!trial_poll()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelete(NULL);
}

void app_trial_start(void) {
    nvs_handle_t handle; uint8_t accepted[32]; size_t size = sizeof(accepted); bool confirmed = false;
    if (nvs_open("openkeylight", NVS_READONLY, &handle) == ESP_OK) {
        confirmed = nvs_get_blob(handle, "accepted_elf", accepted, &size) == ESP_OK && size == 32
            && !memcmp(accepted, esp_app_get_description()->app_elf_sha256, 32);
        nvs_close(handle);
    }
    app_lock();
    /* Initialization is one-shot: a second caller must not reset the deadline
     * or override an already elected confirmation/rollback. */
    if (trial_state != TRIAL_UNINITIALIZED) { app_unlock(); return; }
    trial_deadline_ms = app_now_ms() + TRIAL_DURATION_MS;
    trial_state = confirmed ? TRIAL_CONFIRMED : TRIAL_PENDING;
    atomic_store(&trial_pending, !confirmed);
    bool revert = false;
    /* Publish the timer or the fallback decision before another caller can
     * begin confirmation. The new task waits on this same mutex. */
    if (!confirmed && xTaskCreate(trial_task, "update_trial", 3072, NULL, 4, NULL) != pdPASS) {
        trial_state = TRIAL_REVERTING;
        revert = true;
        app_event_locked("update", "trial.task_failed", "Trial timer could not start; selecting fallback immediately");
    }
    app_unlock();
    if (revert) trial_revert();
}

static void reboot_task(void *unused) {
    (void)unused;
    /* Allocate before changing the boot slot; notify only after acceptance. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    vTaskDelete(NULL);
}

static bool upload_expired(uint64_t started_ms) {
    return app_now_ms() - started_ms >= UPLOAD_DURATION_MS;
}

esp_err_t http_update(httpd_req_t *request) {
    char digest_header[65], content_type[64]; uint8_t expected[32];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) != ESP_OK
        || strcmp(content_type, "application/octet-stream")) return http_error(request, 400, "Upload an application image as application/octet-stream");
    if (httpd_req_get_hdr_value_str(request, "X-SHA256", digest_header, sizeof(digest_header)) != ESP_OK || strlen(digest_header) != 64)
        return http_error(request, 400, "A 64-character X-SHA256 digest is required");
    for (unsigned i = 0; i < 32; i++) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; j++) {
            char c = digest_header[i * 2 + j];
            unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
            if (digit > 15) return http_error(request, 400, "Digest must be lowercase hexadecimal");
            value = value * 16 + digit;
        }
        expected[i] = value;
    }
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || request->content_len < 288 || request->content_len > partition->size || request->content_len > 1572864)
        return http_error(request, 413, "Image does not fit the existing application slot");
    if (app_trial_pending()) return http_error(request, 409, "Confirm the current trial before replacing its recovery slot");
    int socket = httpd_req_to_sockfd(request);
    struct timeval old_timeout;
    socklen_t timeout_size = sizeof(old_timeout);
    if (socket < 0 || getsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &old_timeout, &timeout_size) != 0)
        return http_error(request, 503, "Cannot bound the upload receive time");
    app_lock();
    if (app.updating) { app_unlock(); return http_error(request, 409, "An update is already active"); }
    if (!kl_update_indicator_begin(&update_indicator, (uint32_t)request->content_len, app_now_ms())) {
        app_unlock(); return http_error(request, 409, "An application upload is already active");
    }
    app.updating = true; app_event_locked("update", "upload.started", "Receiving application into inactive slot"); app_unlock();
    uint64_t started_ms = app_now_ms();
    esp_ota_handle_t ota = 0; bool began = false;
    esp_err_t result = esp_ota_begin(partition, request->content_len, &ota);
    if (result == ESP_OK) began = true;
    mbedtls_sha256_context sha; mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0 && result == ESP_OK) result = ESP_FAIL;
    uint8_t buffer[2048], digest[32]; size_t received = 0;
    while (result == ESP_OK && received < request->content_len) {
        uint64_t elapsed = app_now_ms() - started_ms;
        if (elapsed >= UPLOAD_DURATION_MS) { result = ESP_ERR_TIMEOUT; break; }
        uint64_t remaining = UPLOAD_DURATION_MS - elapsed;
        if (remaining > UPLOAD_RECEIVE_SLICE_MS) remaining = UPLOAD_RECEIVE_SLICE_MS;
        struct timeval timeout = {.tv_sec = (long)(remaining / 1000), .tv_usec = (long)(remaining % 1000) * 1000};
        if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) { result = ESP_FAIL; break; }
        size_t size = request->content_len - received; if (size > sizeof(buffer)) size = sizeof(buffer);
        int length = httpd_req_recv(request, (char *)buffer, size);
        if (length <= 0 || (size_t)length > size || upload_expired(started_ms)) { result = ESP_ERR_TIMEOUT; break; }
        if (mbedtls_sha256_update(&sha, buffer, (size_t)length) != 0) { result = ESP_FAIL; break; }
        result = esp_ota_write(ota, buffer, length); received += length;
        if (result == ESP_OK) {
            app_lock();
            (void)kl_update_indicator_advance(&update_indicator, (uint32_t)received);
            app_unlock();
        }
    }
    if (result == ESP_OK && mbedtls_sha256_finish(&sha, digest) != 0) result = ESP_FAIL;
    mbedtls_sha256_free(&sha);
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &old_timeout, sizeof(old_timeout)) != 0 && result == ESP_OK) result = ESP_FAIL;
    if (result == ESP_OK && upload_expired(started_ms)) result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK && memcmp(digest, expected, sizeof(digest))) result = ESP_ERR_INVALID_CRC;
    if (result == ESP_OK) { result = esp_ota_end(ota); began = false; }
    if (began) esp_ota_abort(ota);
    TaskHandle_t reboot = NULL;
    if (result == ESP_OK && upload_expired(started_ms)) result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK && xTaskCreate(reboot_task, "update_reboot", 2048, NULL, 4, &reboot) != pdPASS) result = ESP_ERR_NO_MEM;
    if (result == ESP_OK && upload_expired(started_ms)) result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK) result = esp_ota_set_boot_partition(partition);
    if (result != ESP_OK && reboot) vTaskDelete(reboot);
    app_lock();
    if (result != ESP_OK) {
        kl_update_indicator_fail(&update_indicator);
        app.updating = false; app_event_locked("update", "upload.failed", esp_err_to_name(result));
    } else {
        /* Full progress requires SHA/image validation and accepted boot-slot
         * selection. Receiving the last network chunk is not verification. */
        (void)kl_update_indicator_verify(&update_indicator, app_now_ms());
        app_event_locked("update", "upload.verified", "Application verified; restarting into trial");
    }
    app_unlock();
    if (result != ESP_OK) return http_error(request, result == ESP_ERR_TIMEOUT ? 408 : result == ESP_ERR_NO_MEM ? 503 : 400,
        "Update did not complete; no reboot was scheduled");
    cJSON *json = cJSON_CreateObject(); cJSON_AddBoolToObject(json, "accepted", true); cJSON_AddBoolToObject(json, "rebooting", true);
    esp_err_t response = http_json(request, 202, json);
    xTaskNotifyGive(reboot);
    return response;
}
