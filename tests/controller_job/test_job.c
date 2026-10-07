#include "job_mocks.h"
#include "app.h"
#include "controller_job.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, frees, locked, nvs_calls, availability_calls;
static bool availability_ready;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); } } while (0)
static void job_free(void *pointer) { CHECK(pointer != NULL); ++frees; free(pointer); }
#define free job_free
#include "../../firmware/main/controller_job.c"
#undef free
#include "off_fixture.h"
#include "../controller_worker/low_fixture.h"

app_context app;
static uint8_t disk[256], pending[256];
static size_t disk_size, pending_size;
static bool disk_present, pending_present, dirty;
static int fail_open, fail_read, fail_set, fail_erase, fail_commit, fail_hash;
static bool commit_despite_error;
static int reset_reason;
static uint64_t elapsed_ms;

void app_lock(void) { CHECK(!locked); locked = 1; }
void app_unlock(void) { CHECK(locked == 1); locked = 0; }
void app_mqtt_availability(void) {
    CHECK(!locked); ++availability_calls;
    availability_ready=app.controller_ready && app.controller_connected && !app.updating && !app_controller_update_blocked();
}
uint64_t app_now_ms(void) { return elapsed_ms; }
int esp_reset_reason(void) { return reset_reason; }
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
    availability_calls=0;availability_ready=false;
    atomic_store(&blocked, true); ready(); frees = 0; nvs_calls = 0;
    fail_open = fail_read = fail_set = fail_erase = fail_commit = fail_hash = 0;
    dirty = false; commit_despite_error = false;
    reset_reason = ESP_RST_POWERON; elapsed_ms = 0;
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
static okl_loader_audit rejection_for(const app_controller_job *j) {
    okl_loader_audit a=audit_for(j);
    a.phase=OKL_LOADER_PRECOMMIT_FAILED;a.result=OKL_LOADER_INVALID;
    return a;
}
static void rejection_tests(void) {
    initialize();app_controller_job j=running();okl_loader_audit a=rejection_for(&j);
    app_controller_worker_outcome proof={.entry=APP_CONTROLLER_READ_ONLY_UNSUPPORTED,.synchronized=true};
    CHECK(!app_controller_update_persist(j.id,&a) && disk_present);
    CHECK(app_controller_update_reject(j.id,&a,OKL_LOADER_INVALID,&proof));
    CHECK(frees==1 && !app.updating && !disk_present && !app_controller_update_blocked());
    CHECK(!app.controller_ready && !app.reported_valid && !strcmp(app.controller_status,"starting"));
    check_json("failed","invalid",true,false,false);
    CHECK(!app_controller_update_reject(j.id,&a,OKL_LOADER_INVALID,&proof) && frees==1);
    CHECK(!app_controller_update_finish(j.id,&a,OKL_LOADER_INVALID,false,NULL) && frees==1);
    ready();uint32_t next=0;CHECK(app_controller_update_begin(&next)==200 && next>j.id);
    app_controller_update_cancel_upload(next);
    reset(true);CHECK(app_controller_update_init()==ESP_OK && !app_controller_update_blocked());

    /* Each disqualifying fact leaves the durable record in place. Do not
     * confuse a correlated unsupported getter with timeout/mutation proof. */
    for(unsigned fault=0;fault<22;++fault) {
        initialize();j=running();a=rejection_for(&j);proof=(app_controller_worker_outcome){.entry=APP_CONTROLLER_READ_ONLY_UNSUPPORTED,.synchronized=true};
        CHECK(!app_controller_update_persist(j.id,&a));
        switch(fault) {
        case 0:proof.entry=APP_CONTROLLER_ENTRY_UNPROVEN;break;
        case 1:proof.entry=APP_CONTROLLER_MUTATION_ATTEMPTED;break;
        case 2:proof.synchronized=false;break;
        case 3:a.loader_verified=1;break;
        case 4:a.erase_attempted=1;break;
        case 5:a.program_blocks_acked=1;break;
        case 6:a.readback_blocks_verified=1;break;
        case 7:a.complete_bank_verified=1;break;
        case 8:a.commit_attempted=1;break;
        case 9:a.abort_attempted=1;break;
        case 10:a.commit_delivery=OKL_LOADER_MAYBE_SENT;break;
        case 11:a.abort_delivery=OKL_LOADER_SENT_COMPLETE;break;
        case 12:a.quiet_completed=1;break;
        case 13:a.reset_boundary_established=1;break;
        case 14:a.persistence_failed=1;break;
        case 15:a.cancelled_after_commit=1;break;
        case 16:a.quiet_started_us=1;break;
        case 17:a.quiet_finished_us=1;break;
        case 18:a.observation.kind=OKL_LOADER_OBSERVATION_RESIDENT;break;
        case 19:a.phase=OKL_LOADER_ENTERING;break;
        case 20:job.audit.erase_attempted=1;break; /* Returned audit cannot erase historical evidence. */
        default:atomic_store(&blocked,true);break;
        }
        unsigned calls=nvs_calls;
        CHECK(!app_controller_update_reject(j.id,&a,OKL_LOADER_INVALID,&proof));
        CHECK(nvs_calls==calls && !frees && disk_present && app.updating);
        CHECK(!app_controller_update_finish(j.id,&a,OKL_LOADER_INVALID,false,NULL));
        CHECK(frees==1 && app_controller_update_blocked());
        reset(true);CHECK(app_controller_update_init()==ESP_OK && app_controller_update_blocked());
    }
    for(unsigned fault=0;fault<4;++fault) {
        initialize();j=running();a=rejection_for(&j);proof=(app_controller_worker_outcome){.entry=APP_CONTROLLER_READ_ONLY_UNSUPPORTED,.synchronized=true};
        CHECK(!app_controller_update_persist(j.id,&a));
        if(fault==0)fail_open=1;
        if(fault==1)fail_erase=1;
        if(fault>=2)fail_commit=1;
        if(fault==3)commit_despite_error=true;
        CHECK(!app_controller_update_reject(j.id,&a,OKL_LOADER_INVALID,&proof));
        CHECK(frees==1 && app_controller_update_blocked() && !app.updating && !app.controller_ready);
        CHECK(disk_present==(fault!=3));
        check_json("recovery_required","persist",true,false,true);
        CHECK(!app_controller_update_finish(j.id,&a,OKL_LOADER_INVALID,false,NULL) && frees==1);
    }
    /* In-RAM evidence is never enough to clear a reloaded pending journal. */
    initialize();j=running();a=rejection_for(&j);proof=(app_controller_worker_outcome){.entry=APP_CONTROLLER_READ_ONLY_UNSUPPORTED,.synchronized=true};
    CHECK(!app_controller_update_persist(j.id,&a));reset(true);CHECK(app_controller_update_init()==ESP_OK);
    unsigned calls=nvs_calls;
    CHECK(!app_controller_update_reject(j.id,&a,OKL_LOADER_INVALID,&proof) && nvs_calls==calls);
    CHECK(disk_present && app_controller_update_blocked());
}
static app_controller_job diagnostic_running(void) {
    uint32_t id; CHECK(app_controller_update_begin_role(&id,OKL_ROLE_SPI_DIAGNOSTIC)==200);
    uint8_t *p=package();p[22]=OKL_ROLE_SPI_DIAGNOSTIC;
    CHECK(app_controller_update_submit(id,p,OKL_LOADER_PACKAGE_BYTES)==202);
    app_controller_job out;CHECK(app_controller_update_take(&out));return out;
}
static void diagnostic_tests(void) {
    uint32_t id=99;
    initialize();CHECK(app_controller_update_begin_role(&id,0)==400 && id==99);
    CHECK(app_controller_update_begin_role(&id,3)==400 && id==99);
    CHECK(app_controller_update_begin_role(&id,1)==200);
    uint8_t *p=package();CHECK(app_controller_update_submit(id,p,OKL_LOADER_PACKAGE_BYTES)==400);
    free(p);app_controller_update_cancel_upload(id);
    CHECK(app_controller_update_begin(&id)==200);p=package();p[22]=1;
    CHECK(app_controller_update_submit(id,p,OKL_LOADER_PACKAGE_BYTES)==400);free(p);app_controller_update_cancel_upload(id);
    for(unsigned fault=0;fault<13;++fault) {
        initialize();app_controller_job j=diagnostic_running();okl_loader_audit a=complete_for(&j);
        a.observation.controller.role=1;a.observation.controller.capabilities=1;
        app_controller_worker_outcome proof={.entry=APP_CONTROLLER_MUTATION_ATTEMPTED,.synchronized=true,
            .diagnostic_trial_observed=true,.profile_verified=true,.command_attempted=true,
            .command_acknowledged=true,.registers_verified=true,.resident_proof_job_id=j.id,
            .diagnostic_profile=APP_CONTROLLER_PROFILE_OFF};
        off_fixture(proof.diagnostic_words);
        CHECK(app_diagnostic_registers(proof.diagnostic_words,17));
        CHECK(!app_controller_update_persist(j.id,&a));
        if(fault==1)proof.resident_proof_job_id++;
        if(fault==2)proof.synchronized=false;
        if(fault==3)proof.profile_verified=false;
        if(fault==4)proof.command_attempted=false;
        if(fault==5)proof.command_acknowledged=false;
        if(fault==6)proof.registers_verified=false;
        if(fault==7)proof.diagnostic_words[16+2*40+5]=0x81;
        if(fault==8)proof.diagnostic_words[14]=31000;
        if(fault==9)a.observation.controller.trial_confirmed=1;
        if(fault==10)a.commit_delivery=OKL_LOADER_MAYBE_SENT;
        if(fault==11)a.persistence_failed=1;
        if(fault==12)proof.diagnostic_trial_observed=false;
        unsigned calls=nvs_calls;
        app_controller_update_diagnostic_finish(j.id,&a,OKL_LOADER_OK,&proof);
        CHECK(nvs_calls==calls && disk_present && frees==1 && !app.updating && !app.controller_ready);
        CHECK(app_controller_update_blocked() && !app.controller_trial_confirmed);
        CHECK((job.resident_proof_job_id!=0)==(fault==0));
        if(!fault) {
            check_json("diagnostic_trial","ok",true,false,true);
            CHECK(app_controller_update_begin_role(&id,1)==200 && job.source==OKL_LOADER_FROM_FRESH_RESIDENT);
            p=package();CHECK(app_controller_update_submit(id,p,OKL_LOADER_PACKAGE_BYTES)==400);free(p);
            app_controller_update_cancel_upload(id);
            CHECK(job.resident_proof_job_id==j.id && disk_present);
            CHECK(app_controller_update_begin(&id)==200);
            p=package();CHECK(app_controller_update_submit(id,p,OKL_LOADER_PACKAGE_BYTES)==202);
            app_controller_job next;CHECK(app_controller_update_take(&next));
            CHECK(next.resident_proof_job_id==j.id && !job.resident_proof_job_id && disk_present);
            a=audit_for(&next);CHECK(!app_controller_update_finish(next.id,&a,OKL_LOADER_IO,false,NULL));
            CHECK(app_controller_update_begin(&id)==503); /* proof consumed, never retried */
        } else CHECK(app_controller_update_begin(&id)==503);
        reset(true);CHECK(app_controller_update_init()==ESP_OK && app_controller_update_blocked());
        CHECK(!job.resident_proof_job_id && app_controller_update_begin(&id)==503);
    }
}
static void low_diagnostic_tests(void) {
    uint32_t id=0;
    initialize();CHECK(app_controller_update_begin_mode(&id,3)==400);
    CHECK(app_controller_update_begin_mode(&id,255)==400 && !id && !app.updating);
    for(unsigned fault=0;fault<7;++fault) {
        initialize();CHECK(app_controller_update_begin_mode(&id,APP_CONTROLLER_PROFILE_LOW)==200);
        uint8_t *p=package();CHECK(app_controller_update_submit(id,p,OKL_LOADER_PACKAGE_BYTES)==400);
        p[22]=OKL_ROLE_SPI_DIAGNOSTIC;
        CHECK(app_controller_update_submit(id,p,OKL_LOADER_PACKAGE_BYTES)==202);
        app_controller_job j;CHECK(app_controller_update_take(&j));
        CHECK(j.diagnostic_profile==APP_CONTROLLER_PROFILE_LOW && j.image.role==1);
        okl_loader_audit a=complete_for(&j);a.observation.controller.role=1;a.observation.controller.capabilities=1;
        app_controller_worker_outcome proof={.entry=APP_CONTROLLER_MUTATION_ATTEMPTED,.synchronized=true,
            .diagnostic_trial_observed=true,.profile_verified=true,.command_attempted=true,
            .command_acknowledged=true,.registers_verified=true,.resident_proof_job_id=j.id,
            .diagnostic_profile=APP_CONTROLLER_PROFILE_LOW};
        low_fixture(proof.diagnostic_words);CHECK(app_low_diagnostic_registers(proof.diagnostic_words,77));
        CHECK(!app_controller_update_persist(j.id,&a));
        CHECK(disk[8]==3 && disk[64]==1 && disk[65]==2);
        if(fault==1)proof.diagnostic_profile=APP_CONTROLLER_PROFILE_OFF;
        if(fault==2)off_fixture(proof.diagnostic_words);
        if(fault==3)proof.diagnostic_words[12]=0;
        if(fault==4)proof.diagnostic_words[56+4*40+39]=0;
        if(fault==5)proof.command_acknowledged=false;
        if(fault==6)a.observation.controller.trial_confirmed=true;
        unsigned calls=nvs_calls;
        app_controller_update_diagnostic_finish(j.id,&a,OKL_LOADER_OK,&proof);
        CHECK(nvs_calls==calls && disk_present && frees==1 && app_controller_update_blocked() && !app.controller_ready);
        CHECK((job.resident_proof_job_id==id)==(fault==0) && !job.confirmed);
        cJSON *json=app_controller_update_json();cJSON *detail=cJSON_GetObjectItemCaseSensitive(json,"diagnostic");
        CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(detail,"profile")->valuestring,"LOW1"));
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(detail,"snapshot_words"))==256);
        if(!fault)CHECK(cJSON_GetObjectItemCaseSensitive(detail,"generation")->valueint==77);
        cJSON_Delete(json);
        reset(true);CHECK(app_controller_update_init()==ESP_OK && app_controller_update_blocked());
        CHECK(job.diagnostic_profile==APP_CONTROLLER_PROFILE_LOW && !job.resident_proof_job_id);
    }
    /* Decode prior journal versions, but reject role/profile contradictions
     * even with an otherwise valid integrity digest. */
    for(unsigned variant=0;variant<7;++variant) {
        initialize();app_controller_job j=diagnostic_running();okl_loader_audit a=audit_for(&j);
        CHECK(!app_controller_update_persist(j.id,&a));
        if(variant==0){disk[8]=1;disk[64]=0;disk[65]=0;}
        if(variant==1){disk[8]=2;disk[65]=0;}
        if(variant==2)disk[65]=2;
        if(variant==3)disk[65]=0;
        if(variant==4){disk[64]=2;disk[65]=1;}
        if(variant==5)disk[65]=3;
        if(variant==6)disk[66]=1;
        CHECK(!mbedtls_sha256(disk,JOURNAL_HASH_OFFSET,disk+JOURNAL_HASH_OFFSET,0));
        reset(true);CHECK(app_controller_update_init()==ESP_OK && app_controller_update_blocked());
        CHECK(job.target_known==(variant<3));
        if(variant<3)CHECK(job.diagnostic_profile==(variant==0?0:variant==1?1:2));
        else CHECK(!job.recovery_available);
    }
}
static uint32_t recovery_journal(unsigned changed) {
    initialize(); app_controller_job j=running();
    job.source=j.source=OKL_LOADER_FROM_LEGACY_1_3;
    okl_loader_audit a=audit_for(&j);a.phase=OKL_LOADER_PRECOMMIT_FAILED;a.result=OKL_LOADER_IO;
    if(changed==1)a.erase_attempted=1;
    if(changed==2)a.commit_attempted=1;
    if(changed==3)a.abort_attempted=1;
    if(changed==4)a.program_blocks_acked=1;
    if(changed==5)a.readback_blocks_verified=1;
    if(changed==6)a.complete_bank_verified=1;
    if(changed==7)a.phase=OKL_LOADER_ENTERING;
    if(changed==8)job.source=a.source=OKL_LOADER_FROM_ORIGINAL;
    CHECK(!app_controller_update_persist(j.id,&a));
    reset(true);CHECK(app_controller_update_init()==ESP_OK && app_controller_update_blocked());
    CHECK(job.recovery_available && job.allow_legacy_reconcile==(changed==0));
    return j.id;
}
static void recovery_tests(void) {
    initialize();app_controller_job traced=running();
    app_controller_worker_outcome captured={.transport_snapshot=true,.raw_reply_received=true,
        .transport_phase=3,.ready=1,.reply_kind=0,.reply_status=2,.reply_class=16,.reply_opcode=128,.reply_size=80};
    strcpy(captured.stage,"loader.information");memset(captured.reply_sha256,'a',64);captured.reply_sha256[64]=0;
    unsigned trace_calls=nvs_calls;
    app_controller_update_record_outcome(traced.id+1,&captured);CHECK(!job.diagnostic.stage[0]);
    app_controller_update_record_outcome(traced.id,&captured);CHECK(nvs_calls==trace_calls);
    cJSON *trace_json=app_controller_update_json();cJSON *wire=cJSON_GetObjectItemCaseSensitive(trace_json,"transport_diagnostic");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(wire,"parsed_reply_received")));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(wire,"raw_reply_received")));
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(wire,"routing_tag")->valuestring,"000000000000"));
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(wire,"report_sha256")->valuestring,captured.reply_sha256));
    CHECK(cJSON_GetObjectItemCaseSensitive(wire,"reply_opcode")->valueint==128);cJSON_Delete(trace_json);
    initialize();CHECK(app_controller_recovery_begin(1,true)==409);
    uint32_t id=recovery_journal(0);
    unsigned calls=nvs_calls;
    CHECK(app_controller_recovery_begin(id,false)==400);
    CHECK(app_controller_recovery_begin(0,true)==400);
    CHECK(app_controller_recovery_begin(id+1,true)==409);
    reset_reason=3;CHECK(app_controller_recovery_begin(id,true)==503);
    reset_reason=ESP_RST_POWERON;elapsed_ms=180000;CHECK(app_controller_recovery_begin(id,true)==503);
    elapsed_ms=179999;app.updating=true;CHECK(app_controller_recovery_begin(id,true)==409);app.updating=false;
    CHECK(nvs_calls==calls && disk_present && !job.recovery_only);
    unsigned availability_before=availability_calls;
    CHECK(app_controller_recovery_begin(id,true)==202);
    CHECK(availability_calls==availability_before+1 && !availability_ready);
    CHECK(app_controller_recovery_begin(id,true)==409);
    app_controller_job j;CHECK(app_controller_update_take(&j));
    CHECK(j.recovery_only && j.allow_legacy_reconcile && !j.package && j.id==id);
    CHECK(app_controller_update_blocked() && !job.recovery_available && !app.controller_ready);
    CHECK(!app_controller_update_take(&j));
    app_controller_worker_outcome proof={.synchronized=true,.resident_proof_job_id=id};
    CHECK(!app_controller_recovery_finish(id+1,&proof));
    CHECK(app.updating && job.state==JOB_RUNNING);
    availability_before=availability_calls;
    CHECK(!app_controller_recovery_finish(id,&proof));
    CHECK(availability_calls==availability_before+1 && !availability_ready);
    CHECK(!app.updating && disk_present && app_controller_update_blocked() && job.resident_proof_job_id==id);
    CHECK(nvs_calls==calls && !frees);
    CHECK(app_controller_recovery_begin(id,true)==503);
    CHECK(!app_controller_recovery_finish(id,&proof));
    cJSON *json=app_controller_update_json();CHECK(json);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"resident_recovery_ready")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json,"legacy_reconciled")));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"recovery_attempted")));
    cJSON_Delete(json);
    uint32_t next;CHECK(app_controller_update_begin_role(&next,1)==200 && next==id+1);
    CHECK(job.source==OKL_LOADER_FROM_FRESH_RESIDENT && job.resident_proof_job_id==id);
    app_controller_update_cancel_upload(next);

    for(unsigned invalid=0;invalid<5;++invalid) {
        id=recovery_journal(0);CHECK(app_controller_recovery_begin(id,true)==202);
        CHECK(app_controller_update_take(&j));calls=nvs_calls;
        proof=(app_controller_worker_outcome){.synchronized=true,.resident_proof_job_id=id};
        if(invalid==0)proof.synchronized=false;
        if(invalid==1)proof.resident_proof_job_id=id+1;
        if(invalid==2)proof.resident_proof_job_id=0;
        if(invalid==3)proof.legacy_reconciled=true; /* contradictory proof */
        CHECK(!app_controller_recovery_finish(id,invalid==4?NULL:&proof));
        CHECK(!job.resident_proof_job_id && disk_present && app_controller_update_blocked() && nvs_calls==calls);
        CHECK(!app.updating && !app.controller_ready && app_controller_recovery_begin(id,true)==503);
    }
    for(unsigned changed=0;changed<9;++changed) {
        id=recovery_journal(changed);CHECK(app_controller_recovery_begin(id,true)==202);
        CHECK(app_controller_update_take(&j));calls=nvs_calls;
        proof=(app_controller_worker_outcome){.synchronized=true,.legacy_reconciled=true};
        CHECK(app_controller_recovery_finish(id,&proof)==(changed==0));
        CHECK(app_controller_update_blocked()==(changed!=0) && disk_present==(changed!=0));
        CHECK(!app.controller_ready && !app.updating && !frees);
        CHECK((nvs_calls>calls)==(changed==0));
    }
    for(unsigned failure=0;failure<4;++failure) {
        id=recovery_journal(0);CHECK(app_controller_recovery_begin(id,true)==202);CHECK(app_controller_update_take(&j));
        if(failure==0)fail_open=1;
        if(failure==1)fail_erase=1;
        if(failure>=2)fail_commit=1;
        if(failure==3)commit_despite_error=true;
        proof=(app_controller_worker_outcome){.synchronized=true,.legacy_reconciled=true};
        CHECK(!app_controller_recovery_finish(id,&proof));
        CHECK(app_controller_update_blocked() && !app.controller_ready && !app.updating && !job.resident_proof_job_id);
    }
    /* A valid journal is required even after power-on. Warm OTA, corrupt or
     * unreadable storage never establishes a fresh reset through software. */
    for(unsigned failure=0;failure<3;++failure) {
        id=recovery_journal(0);reset(true);
        if(failure==0)reset_reason=3;
        if(failure==1)disk[30]^=1;
        if(failure==2)fail_read=1;
        CHECK(app_controller_update_init()==ESP_OK && app_controller_update_blocked());
        CHECK(!job.recovery_available);
        CHECK(app_controller_recovery_begin(id,true)!=202);
    }
}
static void availability_tests(void) {
    initialize(); app.controller_connected=true;
    uint32_t id;unsigned before=availability_calls;
    CHECK(app_controller_update_begin(&id)==200);
    CHECK(availability_calls==before+1 && !availability_ready);
    before=availability_calls;app_controller_update_cancel_upload(id);
    CHECK(availability_calls==before+1 && availability_ready);
    app_controller_job active=running();CHECK(!availability_ready);
    okl_loader_audit a=audit_for(&active);before=availability_calls;fail_set=1;
    CHECK(app_controller_update_persist(active.id,&a)==-1);
    CHECK(availability_calls==before+1 && !availability_ready);
    before=availability_calls;
    CHECK(!app_controller_update_finish(active.id,&a,OKL_LOADER_IO,false,"injected"));
    CHECK(availability_calls==before+1 && !availability_ready && !app.updating);
}
int main(void) {
    availability_tests();
    admission_tests(); package_tests(); persistence_tests(); reboot_tests(); finish_tests(); rejection_tests(); diagnostic_tests();low_diagnostic_tests();
    recovery_tests();reset(false); CHECK(!locked);
    printf("controller job: %u assertions passed\n", checks);
    return 0;
}
