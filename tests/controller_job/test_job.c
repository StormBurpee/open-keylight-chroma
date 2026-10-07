#include "job_mocks.h"
#include "app.h"
#include "controller_job.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, frees, locked, nvs_calls;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); } } while (0)
static void job_free(void *pointer) { CHECK(pointer != NULL); ++frees; free(pointer); }
#define free job_free
#include "../../firmware/main/controller_job.c"
#undef free

app_context app;
static uint8_t disk[256], pending[256];
static size_t disk_size, pending_size;
static bool disk_present, pending_present, dirty;
static int fail_open, fail_read, fail_set, fail_erase, fail_commit, fail_hash;
static bool commit_despite_error;

void app_lock(void) { CHECK(!locked); locked = 1; }
void app_unlock(void) { CHECK(locked == 1); locked = 0; }
uint64_t app_now_ms(void) { return 0; }
void app_event_locked(const char *actor, const char *event, const char *detail) {
    CHECK(locked == 1); CHECK(actor && event && detail);
}
/* Deliberately mocked crypto boundary. Production package/header/hash logic is
 * used; SHA implementation correctness belongs to mbedTLS, not this fixture. */
int mbedtls_sha256(const unsigned char *data, size_t size, unsigned char *out, int is224) {
    CHECK(is224 == 0);
    if (fail_hash) return -1;
    for (unsigned i = 0; i < 32; ++i) out[i] = (uint8_t)(i * 7 + 3);
    for (size_t i = 0; i < size; ++i) {
        unsigned index = (unsigned)(i % 32);
        out[index] = (uint8_t)((out[index] << 1) | (out[index] >> 7));
        out[index] ^= (uint8_t)(data[i] + i);
    }
    return 0;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    ++nvs_calls; CHECK(!strcmp(name, "openkeylight")); CHECK(locked);
    if (fail_open) return ESP_FAIL;
    *handle = mode + 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { CHECK(handle == 1 || handle == 2); dirty = false; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    ++nvs_calls; CHECK(handle == 1); CHECK(!strcmp(key, journal_key));
    if (fail_read) return ESP_FAIL;
    if (!disk_present) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < disk_size) { *size = disk_size; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(out, disk, disk_size); *size = disk_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) {
    ++nvs_calls; CHECK(handle == 2); CHECK(!strcmp(key, journal_key)); CHECK(size == JOURNAL_BYTES);
    if (fail_set) return ESP_FAIL;
    memcpy(pending, data, size); pending_size = size; pending_present = true; dirty = true;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    ++nvs_calls; CHECK(handle == 2); CHECK(!strcmp(key, journal_key));
    if (fail_erase) return ESP_FAIL;
    if (!disk_present) return ESP_ERR_NVS_NOT_FOUND;
    pending_present = false; pending_size = 0; dirty = true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    ++nvs_calls; CHECK(handle == 2);
    if ((!fail_commit || commit_despite_error) && dirty) {
        memcpy(disk, pending, pending_size); disk_size = pending_size; disk_present = pending_present;
    }
    return fail_commit ? ESP_FAIL : ESP_OK;
}
static void ready(void) {
    app.controller_ready = true; app.controller_part_id = OKL_LOADER_PART_ID;
    strcpy(app.controller_backend, "original");
}
static void reset(bool retain_disk) {
    CHECK(!locked);
    if (job.package) free(job.package); /* Simulated reset discards volatile ownership. */
    memset(&job, 0, sizeof(job)); memset(&app, 0, sizeof(app));
    atomic_store(&blocked, true); ready(); frees = 0; nvs_calls = 0;
    fail_open = fail_read = fail_set = fail_erase = fail_commit = fail_hash = 0;
    dirty = false; commit_despite_error = false;
    if (!retain_disk) { memset(disk, 0, sizeof(disk)); disk_present = false; disk_size = 0; }
}
static void initialize(void) {
    reset(false); CHECK(app_controller_update_init() == ESP_OK); CHECK(!app_controller_update_blocked());
}
static uint8_t *package(void) {
    uint8_t *p = calloc(1, OKL_LOADER_PACKAGE_BYTES); CHECK(p);
    memcpy(p, "OKLCNXP", 7); p[9] = 1; p[11] = 64;
    put32(p + 12, OKL_LOADER_BANK_BYTES); put32(p + 16, OKL_LOADER_PART_ID);
    p[20] = 1; p[22] = OKL_ROLE_LIGHTING; p[25] = 2;
    uint8_t *bank = p + 64; bank[1] = 0x10; bank[3] = 0x10;
    for (unsigned i = 4; i < 192; i += 4) { bank[i] = 0xc1; bank[i + 1] = 0x20; }
    CHECK(!mbedtls_sha256(bank, OKL_LOADER_BANK_BYTES, p + 28, 0));
    return p;
}
static app_controller_job running(void) {
    uint32_t id = 0; CHECK(app_controller_update_begin(&id) == 200); CHECK(id && app.updating);
    uint8_t *p = package(); CHECK(app_controller_update_submit(id, p, OKL_LOADER_PACKAGE_BYTES) == 202);
    app_controller_job out; CHECK(app_controller_update_take(&out)); CHECK(out.id == id && out.package == p);
    CHECK(out.image.package == p && out.source == OKL_LOADER_FROM_ORIGINAL);
    CHECK(!app.controller_ready && !app_controller_update_take(&out)); return out;
}
static okl_loader_audit audit_for(const app_controller_job *j) {
    okl_loader_audit a = {0}; a.source = j->source; a.phase = OKL_LOADER_VALIDATED;
    memcpy(a.bank_sha256, j->image.bank_sha256, 32); return a;
}
static okl_loader_audit complete_for(const app_controller_job *j) {
    okl_loader_audit a = audit_for(j);
    a.phase = OKL_LOADER_APPLICATION_SEEN; a.program_blocks_acked = a.readback_blocks_verified = 448;
    a.complete_bank_verified = a.commit_attempted = a.quiet_completed = a.reset_boundary_established = 1;
    a.loader_verified = a.erase_attempted = 1;
    a.quiet_started_us = 1; a.quiet_finished_us = 1 + OKL_LOADER_QUIET_US;
    a.commit_delivery = OKL_LOADER_SENT_COMPLETE;
    a.observation.kind = OKL_LOADER_OBSERVATION_APPLICATION; a.observation.dark_state_verified = 1;
    a.observation.version = j->image.version;
    a.observation.controller.abi_major = 1; a.observation.controller.role = OKL_ROLE_LIGHTING;
    a.observation.controller.capabilities = OKL_CAP_RECOVERY_READY | OKL_CAP_LIGHTING_READY;
    a.observation.controller.part_id = OKL_LOADER_PART_ID;
    return a;
}
static void check_json(const char *state, const char *result, bool target, bool confirmed, bool recovery) {
    unsigned before = nvs_calls;
    cJSON *json = app_controller_update_json(); CHECK(json);
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "state")->valuestring, state));
    cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "job_id");
    CHECK(job.id ? cJSON_IsNumber(id) && id->valuedouble == job.id : cJSON_IsNull(id));
    cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
    CHECK(job.error[0] ? cJSON_IsString(error) && !strcmp(error->valuestring, job.error) : cJSON_IsNull(error));
    cJSON *r = cJSON_GetObjectItemCaseSensitive(json, "result");
    CHECK(result ? cJSON_IsString(r) && !strcmp(r->valuestring, result) : cJSON_IsNull(r));
    CHECK(target ? cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "target_sha256")) :
        cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(json, "target_sha256")));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "controller_confirmed")) == confirmed);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "recovery_required")) == recovery);
    const char *delivery = job.audit.commit_delivery == OKL_LOADER_NOT_SENT ? "not_sent" :
        job.audit.commit_delivery == OKL_LOADER_SENT_COMPLETE ? "complete" : "maybe_sent";
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "commit_delivery")->valuestring, delivery));
    char *encoded = cJSON_PrintUnformatted(json); CHECK(encoded);
    cJSON *parsed = cJSON_Parse(encoded); CHECK(parsed); cJSON_Delete(parsed); cJSON_free(encoded); cJSON_Delete(json);
    CHECK(nvs_calls == before && !locked);
}
static void admission_tests(void) {
    reset(false); uint32_t id = 99; CHECK(app_controller_update_begin(&id) == 503 && id == 99);
    CHECK(app_controller_update_init() == ESP_OK); check_json("idle", NULL, false, false, false);
    CHECK(app_controller_update_begin(NULL) == 400);
    app.controller_ready = false; CHECK(app_controller_update_begin(&id) == 503); ready();
    strcpy(app.controller_backend, "unknown"); CHECK(app_controller_update_begin(&id) == 503); ready();
    app.controller_part_id = 0; CHECK(app_controller_update_begin(&id) == 503); ready();
    app.updating = true; CHECK(app_controller_update_begin(&id) == 409); app.updating = false;
    CHECK(app_controller_update_begin(&id) == 200); uint32_t first = id;
    CHECK(app_controller_update_begin(&id) == 409 && id == first);
    app_controller_update_cancel_upload(first + 1); CHECK(app.updating);
    check_json("receiving", NULL, false, false, false);
    app_controller_update_cancel_upload(first); CHECK(!app.updating && frees == 0);
    CHECK(app_controller_update_begin(&id) == 200 && id > first);
    uint8_t *p = package(); CHECK(app_controller_update_submit(first, p, OKL_LOADER_PACKAGE_BYTES) == 409);
    CHECK(app_controller_update_submit(id, p, OKL_LOADER_PACKAGE_BYTES) == 202);
    CHECK(app_controller_update_submit(id, p, OKL_LOADER_PACKAGE_BYTES) == 409);
    app_controller_update_cancel_upload(id); CHECK(app.updating && !frees);
    check_json("queued", NULL, true, false, false);
    CHECK(!app_controller_update_take(NULL)); app_controller_job out; CHECK(app_controller_update_take(&out));
    CHECK(!app_controller_update_take(&out)); CHECK(!frees);
    okl_loader_audit a = audit_for(&out);
    CHECK(!app_controller_update_finish(id + 1, &a, OKL_LOADER_IO, false, "wrong id") && !frees);
    CHECK(!app_controller_update_finish(id, &a, OKL_LOADER_IO, false, "exchange failed"));
    CHECK(frees == 1 && !app.updating && app_controller_update_blocked());
    CHECK(!app_controller_update_finish(id, &a, OKL_LOADER_IO, false, "again") && frees == 1);
    check_json("recovery_required", "io", true, false, true);
    initialize(); job.next_id = UINT32_MAX; CHECK(app_controller_update_begin(&id) == 503);
    initialize(); strcpy(app.controller_backend, "legacy"); CHECK(app_controller_update_begin(&id) == 200);
    CHECK(job.source == OKL_LOADER_FROM_LEGACY_1_3);
}
static void package_tests(void) {
    for (unsigned index = 0; index < 64; ++index) {
        initialize(); uint32_t id; CHECK(app_controller_update_begin(&id) == 200); uint8_t *p = package();
        p[index] ^= 0x80;
        int status = app_controller_update_submit(id, p, OKL_LOADER_PACKAGE_BYTES);
        /* Version is descriptive metadata; all other header bytes constrained. */
        if (index >= 24 && index < 28) CHECK(status == 202);
        else { CHECK(status == 400 && !frees && job.package == NULL); free(p); app_controller_update_cancel_upload(id); }
    }
    for (unsigned index = 0; index < 192; index += 4) {
        initialize(); uint32_t id; CHECK(app_controller_update_begin(&id) == 200); uint8_t *p = package();
        memset(p + 64 + index, 0, 4); CHECK(!mbedtls_sha256(p + 64, OKL_LOADER_BANK_BYTES, p + 28, 0));
        CHECK(app_controller_update_submit(id, p, OKL_LOADER_PACKAGE_BYTES) == 400); free(p);
    }
    initialize(); uint32_t id; CHECK(app_controller_update_begin(&id) == 200); uint8_t *p = package();
    CHECK(app_controller_update_submit(id, NULL, OKL_LOADER_PACKAGE_BYTES) == 400);
    CHECK(app_controller_update_submit(id, p, OKL_LOADER_PACKAGE_BYTES - 1) == 400);
    CHECK(app_controller_update_submit(id, p, OKL_LOADER_PACKAGE_BYTES + 1) == 400);
    p[200] ^= 1; CHECK(app_controller_update_submit(id, p, OKL_LOADER_PACKAGE_BYTES) == 400); p[200] ^= 1;
    fail_hash = 1; CHECK(app_controller_update_submit(id, p, OKL_LOADER_PACKAGE_BYTES) == 503);
    CHECK(job.state == JOB_RECEIVING && !frees && !disk_present); free(p);
}
static void persistence_tests(void) {
    for (unsigned failure = 0; failure < 5; ++failure) {
        initialize(); app_controller_job j = running(); okl_loader_audit a = audit_for(&j);
        CHECK(!app_controller_update_persist(j.id, &a)); uint8_t previous[128]; memcpy(previous, disk, 128);
        a.phase = OKL_LOADER_ENTERING;
        if (failure == 0) fail_open = 1;
        if (failure == 1) fail_set = 1;
        if (failure == 2) fail_commit = 1;
        if (failure == 3) fail_hash = 1;
        if (failure == 4) { fail_commit = 1; commit_despite_error = true; }
        CHECK(app_controller_update_persist(j.id, &a) == -1);
        CHECK(app_controller_update_blocked() && !app.controller_ready);
        if (failure != 4) CHECK(!memcmp(disk, previous, 128));
        CHECK(!app_controller_update_finish(j.id, &a, OKL_LOADER_PERSIST, false, "persist failed") && frees == 1);
        reset(true); CHECK(app_controller_update_init() == ESP_OK && app_controller_update_blocked());
        CHECK(job.state == JOB_RECOVERY_REQUIRED && job.id == j.id);
    }
    initialize(); app_controller_job j = running(); okl_loader_audit a = audit_for(&j);
    unsigned calls = nvs_calls;
    CHECK(app_controller_update_persist(j.id + 1, &a) == -1 && nvs_calls == calls);
    CHECK(app_controller_update_persist(j.id, NULL) == -1 && nvs_calls == calls);
    a.bank_sha256[0] ^= 1; CHECK(app_controller_update_persist(j.id, &a) == -1); a.bank_sha256[0] ^= 1;
    a.phase = (okl_loader_phase)99; CHECK(app_controller_update_persist(j.id, &a) == -1);
    app_controller_update_progress(j.id, &a); CHECK(job.audit.phase == OKL_LOADER_VALIDATED);
    a.phase = OKL_LOADER_PROGRAMMING; a.program_blocks_acked = 449;
    CHECK(app_controller_update_persist(j.id, &a) == -1);
    a.program_blocks_acked = 17; app_controller_update_progress(j.id, &a);
    CHECK(job.audit.program_blocks_acked == 17 && nvs_calls == calls);
}
static void reboot_tests(void) {
    uint8_t saved[128];
    initialize(); app_controller_job j = running(); okl_loader_audit a = audit_for(&j);
    a.phase = OKL_LOADER_COMMITTING; a.commit_attempted = 1;
    CHECK(!app_controller_update_persist(j.id, &a)); memcpy(saved, disk, sizeof(saved));
    for (unsigned byte = 0; byte < 128; ++byte) {
        reset(false); disk_present = true; disk_size = 128; memcpy(disk, saved, 128); disk[byte] ^= 1;
        CHECK(app_controller_update_init() == ESP_OK && app_controller_update_blocked());
        CHECK(!job.target_known && !app.controller_ready && !app.updating);
        uint32_t id; CHECK(app_controller_update_begin(&id) == 503); CHECK(!app_controller_update_take(&j));
    }
    for (size_t size = 0; size <= 129; ++size) {
        reset(false); disk_present = true; disk_size = size; memcpy(disk, saved, 128);
        CHECK(app_controller_update_init() == ESP_OK && app_controller_update_blocked());
        CHECK(job.target_known == (size == 128));
    }
    reset(false); disk_present = true; disk_size = 128; memcpy(disk, saved, 128);
    CHECK(app_controller_update_init() == ESP_OK && app_controller_update_blocked());
    CHECK(job.audit.phase == OKL_LOADER_COMMITTING && job.audit.commit_delivery == OKL_LOADER_MAYBE_SENT);
    check_json("recovery_required", "unresolved", true, false, true);
    unsigned calls = nvs_calls; CHECK(app_controller_update_init() == ESP_OK && nvs_calls == calls);
    reset(false); fail_open = 1; CHECK(app_controller_update_init() == ESP_OK && app_controller_update_blocked());
    check_json("recovery_required", "unresolved", false, false, true);
    reset(false); fail_read = 1; CHECK(app_controller_update_init() == ESP_OK && app_controller_update_blocked());
}
static void finish_tests(void) {
    initialize(); app_controller_job j = running(); okl_loader_audit a = complete_for(&j);
    CHECK(!app_controller_update_persist(j.id, &a) && disk_present);
    CHECK(app_controller_update_finish(j.id, &a, OKL_LOADER_OK, true, NULL));
    CHECK(frees == 1 && !app.updating && !disk_present && !app_controller_update_blocked());
    CHECK(job.image.package == NULL && !job.package);
    check_json("completed", "ok", true, true, false);
    CHECK(!app_controller_update_finish(j.id, &a, OKL_LOADER_OK, true, NULL) && frees == 1);
    reset(true); CHECK(app_controller_update_init() == ESP_OK && !app_controller_update_blocked());
    for (unsigned fault = 0; fault < 28; ++fault) {
        initialize(); j = running(); a = complete_for(&j); CHECK(!app_controller_update_persist(j.id, &a));
        bool confirmed = true;
        switch (fault) {
        case 0: confirmed = false; break;
        case 1: a.result = OKL_LOADER_VERIFY; break;
        case 2: a.phase = OKL_LOADER_OBSERVING; break;
        case 3: a.complete_bank_verified = 0; break;
        case 4: --a.program_blocks_acked; break;
        case 5: --a.readback_blocks_verified; break;
        case 6: a.commit_attempted = 0; break;
        case 7: a.commit_delivery = OKL_LOADER_MAYBE_SENT; break;
        case 8: a.quiet_completed = 0; break;
        case 9: a.reset_boundary_established = 0; break;
        case 10: a.persistence_failed = 1; break;
        case 11: a.cancelled_after_commit = 1; break;
        case 12: a.observation.kind = OKL_LOADER_OBSERVATION_RESIDENT; break;
        case 13: a.observation.dark_state_verified = 0; break;
        case 14: ++a.observation.version.component[0]; break;
        case 15: a.observation.controller.role = OKL_ROLE_SPI_DIAGNOSTIC; break;
        case 16: a.observation.controller.part_id = 0; break;
        case 17: a.observation.controller.capabilities = 0; break;
        case 18: a.observation.controller.boot_requested = 1; break;
        case 19: fail_open = 1; break;
        case 20: fail_erase = 1; break;
        case 21: fail_commit = 1; break;
        case 22: fail_commit = 1; commit_despite_error = true; break;
        case 23: a.loader_verified = 0; break;
        case 24: a.erase_attempted = 0; break;
        case 25: a.abort_attempted = 1; break;
        case 26: --a.quiet_finished_us; break;
        default: a.quiet_finished_us = 0; break;
        }
        CHECK(!app_controller_update_finish(j.id, &a, OKL_LOADER_OK, confirmed, NULL));
        CHECK(app_controller_update_blocked() && frees == 1 && !app.updating && !job.package);
        CHECK(!app.controller_ready && !strcmp(app.controller_status, "fault"));
        CHECK(disk_present == (fault != 22));
    }
}
int main(void) {
    admission_tests(); package_tests(); persistence_tests(); reboot_tests(); finish_tests();
    reset(false); CHECK(!locked);
    printf("controller job: %u assertions passed\n", checks);
    return 0;
}
