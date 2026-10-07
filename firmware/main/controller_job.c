#include "app.h"
#include "controller_job.h"
#include "controller_diagnostic.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum job_state { JOB_IDLE, JOB_RECEIVING, JOB_QUEUED, JOB_RUNNING,
                 JOB_SUCCEEDED, JOB_RECOVERY_REQUIRED, JOB_FAILED, JOB_DIAGNOSTIC_TRIAL };
static struct {
    bool initialized;
    enum job_state state;
    uint32_t id, next_id;
    uint8_t *package;
    okl_loader_image image;
    okl_loader_source source;
    okl_loader_audit audit;
    bool confirmed, target_known;
    uint8_t requested_role;
    uint32_t resident_proof_job_id;
    app_controller_worker_outcome diagnostic;
    char error[81];
} job;
static atomic_bool blocked = true;
static const char journal_key[] = "nxp_job_v1";
enum { JOURNAL_BYTES = 128, JOURNAL_HASH_OFFSET = 96 };
static const uint8_t journal_magic[8] = {'O','K','L','J','O','B','1',0};

static int sha256(void *unused, const uint8_t *data, size_t size, uint8_t digest[32]) {
    (void)unused;
    return mbedtls_sha256(data, size, digest, 0);
}
static void put32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static bool audit_valid(const okl_loader_audit *a) {
    return a && (unsigned)a->phase <= OKL_LOADER_COMMIT_UNRESOLVED &&
        (unsigned)a->result <= OKL_LOADER_UNRESOLVED && a->source == job.source &&
        a->program_blocks_acked <= OKL_LOADER_BLOCKS &&
        a->readback_blocks_verified <= OKL_LOADER_BLOCKS &&
        !memcmp(a->bank_sha256, job.image.bank_sha256, 32);
}
static void disable_locked(const char *error) {
    atomic_store(&blocked, true);
    app.controller_ready = false;
    app.reported_valid = false;
    app.reported_fields = 0;
    snprintf(app.controller_status, sizeof(app.controller_status), "fault");
    snprintf(app.operation, sizeof(app.operation), "error");
    snprintf(job.error, sizeof(job.error), "%s", error);
    snprintf(app.error, sizeof(app.error), "%s", error);
}
/* Fixed byte layout, independent of compiler padding and enum widths. A
 * digest catches truncation/corruption; it is not an authentication signature. */
static bool encode_journal(uint8_t record[JOURNAL_BYTES], const okl_loader_audit *a) {
    memset(record, 0, JOURNAL_BYTES);
    memcpy(record, journal_magic, sizeof(journal_magic));
    record[8] = 2;
    record[9] = (uint8_t)a->phase; record[10] = (uint8_t)a->source;
    record[11] = (uint8_t)a->result; put32(record + 12, job.id);
    put32(record + 16, a->program_blocks_acked); put32(record + 20, a->readback_blocks_verified);
    memcpy(record + 24, job.image.version.component, 4);
    memcpy(record + 28, a->bank_sha256, 32);
    record[60] = a->erase_attempted != 0; record[61] = a->commit_attempted != 0;
    record[62] = a->abort_attempted != 0; record[63] = a->complete_bank_verified != 0;
    record[64] = job.image.role;
    return !sha256(NULL, record, JOURNAL_HASH_OFFSET, record + JOURNAL_HASH_OFFSET);
}
static bool decode_journal(const uint8_t record[JOURNAL_BYTES]) {
    uint8_t digest[32];
    if (memcmp(record, journal_magic, sizeof(journal_magic)) || (record[8] != 1 && record[8] != 2) ||
        record[9] > OKL_LOADER_COMMIT_UNRESOLVED || record[10] > OKL_LOADER_FROM_FRESH_RESIDENT ||
        record[11] > OKL_LOADER_UNRESOLVED || !get32(record + 12) ||
        get32(record + 16) > OKL_LOADER_BLOCKS || get32(record + 20) > OKL_LOADER_BLOCKS)
        return false;
    for (unsigned i = 60; i < 64; ++i) if (record[i] > 1) return false;
    if (record[8] == 2 && record[64] != OKL_ROLE_LIGHTING && record[64] != OKL_ROLE_SPI_DIAGNOSTIC) return false;
    for (unsigned i = record[8] == 1 ? 64 : 65; i < JOURNAL_HASH_OFFSET; ++i) if (record[i]) return false;
    if (sha256(NULL, record, JOURNAL_HASH_OFFSET, digest) ||
        memcmp(digest, record + JOURNAL_HASH_OFFSET, 32)) return false;
    job.id = job.next_id = get32(record + 12);
    job.source = (okl_loader_source)record[10];
    job.audit.phase = (okl_loader_phase)record[9]; job.audit.source = job.source;
    job.audit.result = (okl_loader_result)record[11];
    job.audit.program_blocks_acked = (uint16_t)get32(record + 16);
    job.audit.readback_blocks_verified = (uint16_t)get32(record + 20);
    memcpy(job.image.version.component, record + 24, 4);
    job.image.role = record[8] == 1 ? OKL_ROLE_LIGHTING : record[64];
    job.requested_role = job.image.role;
    memcpy(job.image.bank_sha256, record + 28, 32);
    job.target_known = true;
    memcpy(job.audit.bank_sha256, record + 28, 32);
    job.audit.erase_attempted = record[60]; job.audit.commit_attempted = record[61];
    job.audit.abort_attempted = record[62]; job.audit.complete_bank_verified = record[63];
    /* The last durable pre-send intent cannot establish whether End ran. */
    if (job.audit.commit_attempted) job.audit.commit_delivery = OKL_LOADER_MAYBE_SENT;
    return true;
}
static esp_err_t write_journal(const uint8_t *record) {
    nvs_handle_t handle;
    esp_err_t result = nvs_open("openkeylight", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = record ? nvs_set_blob(handle, journal_key, record, JOURNAL_BYTES) : nvs_erase_key(handle, journal_key);
    if (!record && result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

bool app_controller_update_blocked(void) { return atomic_load(&blocked); }

esp_err_t app_controller_update_init(void) {
    app_lock();
    if (job.initialized) { app_unlock(); return ESP_OK; }
    job.initialized = true;
    uint8_t record[JOURNAL_BYTES]; size_t size = sizeof(record);
    nvs_handle_t handle;
    esp_err_t result = nvs_open("openkeylight", NVS_READONLY, &handle);
    if (result == ESP_OK) {
        result = nvs_get_blob(handle, journal_key, record, &size);
        nvs_close(handle);
    }
    if (result == ESP_ERR_NVS_NOT_FOUND) atomic_store(&blocked, false);
    else {
        job.state = JOB_RECOVERY_REQUIRED;
        bool valid = result == ESP_OK && size == sizeof(record) && decode_journal(record);
        job.audit.result = OKL_LOADER_UNRESOLVED;
        disable_locked(valid ? "Interrupted controller update; explicit recovery required" :
            "Controller update journal unavailable or invalid; explicit recovery required");
    }
    app_unlock();
    return ESP_OK;
}

int app_controller_update_begin(uint32_t *id) {
    return app_controller_update_begin_role(id, OKL_ROLE_LIGHTING);
}

int app_controller_update_begin_role(uint32_t *id, uint8_t role) {
    if (!id || (role != OKL_ROLE_LIGHTING && role != OKL_ROLE_SPI_DIAGNOSTIC)) return 400;
    app_lock();
    bool resident = atomic_load(&blocked) && job.resident_proof_job_id != 0;
    if (!job.initialized || (atomic_load(&blocked) && !resident) || job.next_id == UINT32_MAX) {
        app_unlock(); return 503;
    }
    if (app.updating || job.state == JOB_RECEIVING || job.state == JOB_QUEUED || job.state == JOB_RUNNING) {
        app_unlock(); return 409;
    }
    if (!resident && !app.controller_ready) { app_unlock(); return 503; }
    okl_loader_source source;
    if (resident) source = OKL_LOADER_FROM_FRESH_RESIDENT;
    else if (!strcmp(app.controller_backend, "legacy")) source = OKL_LOADER_FROM_LEGACY_1_3;
    else if (!strcmp(app.controller_backend, "original") && app.controller_part_id == OKL_LOADER_PART_ID)
        source = OKL_LOADER_FROM_ORIGINAL;
    else { app_unlock(); return 503; }
    job.id = ++job.next_id; job.state = JOB_RECEIVING; job.source = source;
    job.confirmed = false; job.target_known = false; job.error[0] = 0;
    job.requested_role = role;
    memset(&job.diagnostic, 0, sizeof(job.diagnostic));
    memset(&job.image, 0, sizeof(job.image)); memset(&job.audit, 0, sizeof(job.audit));
    job.audit.source = source;
    app.updating = true;
    app_event_locked("update", "controller.receiving", "Receiving controller package; no SPI mutation started");
    *id = job.id;
    app_unlock(); return 200;
}

int app_controller_update_submit(uint32_t id, uint8_t *package, size_t size) {
    app_lock();
    if (job.state != JOB_RECEIVING || job.id != id) { app_unlock(); return 409; }
    okl_loader_image image;
    const okl_loader_ops ops = {.sha256 = sha256};
    okl_loader_result result = okl_loader_prepare(&image, package, size, &ops);
    if (result != OKL_LOADER_OK) { app_unlock(); return result == OKL_LOADER_IO ? 503 : 400; }
    if (image.role != job.requested_role) { app_unlock(); return 400; }
    job.image = image; job.package = package; job.state = JOB_QUEUED; job.target_known = true;
    job.audit.phase = OKL_LOADER_VALIDATED;
    memcpy(job.audit.bank_sha256, image.bank_sha256, 32);
    app_event_locked("update", "controller.queued", "Complete controller package validated; awaiting worker");
    app_unlock(); return 202;
}

void app_controller_update_cancel_upload(uint32_t id) {
    app_lock();
    if (job.state == JOB_RECEIVING && job.id == id) {
        job.state = atomic_load(&blocked) ? JOB_RECOVERY_REQUIRED : JOB_IDLE; app.updating = false;
        app_event_locked("update", "controller.cancelled", "Controller upload cancelled before admission");
    }
    app_unlock();
}

bool app_controller_update_take(app_controller_job *out) {
    if (!out) return false;
    app_lock();
    bool available = job.state == JOB_QUEUED && (!atomic_load(&blocked) ||
        (job.source == OKL_LOADER_FROM_FRESH_RESIDENT && job.resident_proof_job_id));
    if (available) {
        *out = (app_controller_job){job.id, job.package, job.image, job.source, job.resident_proof_job_id};
        job.resident_proof_job_id = 0;
        /* A new explicit job owns recovery. Durable errors reassert this gate;
         * the journal remains until normal success, never on admission. */
        atomic_store(&blocked, false);
        job.state = JOB_RUNNING;
        app.controller_ready = false; app.reported_valid = false; app.reported_fields = 0;
        snprintf(app.operation, sizeof(app.operation), "pending");
    }
    app_unlock(); return available;
}

void app_controller_update_progress(uint32_t id, const okl_loader_audit *audit) {
    app_lock();
    if (job.state == JOB_RUNNING && job.id == id && audit_valid(audit)) job.audit = *audit;
    app_unlock();
}

int app_controller_update_persist(uint32_t id, const okl_loader_audit *audit) {
    app_lock();
    if (job.state != JOB_RUNNING || job.id != id || !audit_valid(audit)) { app_unlock(); return -1; }
    uint8_t record[JOURNAL_BYTES];
    bool saved = encode_journal(record, audit) && write_journal(record) == ESP_OK;
    job.audit = *audit;
    if (!saved) disable_locked("Controller update journal write failed; explicit recovery required");
    app_unlock(); return saved ? 0 : -1;
}

bool app_controller_update_finish(uint32_t id, const okl_loader_audit *audit,
                                  okl_loader_result result, bool confirmed, const char *error) {
    app_lock();
    if (job.state != JOB_RUNNING || job.id != id) { app_unlock(); return false; }
    bool valid = audit_valid(audit);
    if (valid) job.audit = *audit;
    bool complete = valid && job.image.role == OKL_ROLE_LIGHTING &&
        result == OKL_LOADER_OK && audit->result == OKL_LOADER_OK && confirmed &&
        audit->phase == OKL_LOADER_APPLICATION_SEEN && audit->loader_verified && audit->erase_attempted &&
        audit->complete_bank_verified && !audit->abort_attempted &&
        audit->program_blocks_acked == OKL_LOADER_BLOCKS && audit->readback_blocks_verified == OKL_LOADER_BLOCKS &&
        audit->commit_attempted && audit->commit_delivery == OKL_LOADER_SENT_COMPLETE &&
        audit->quiet_completed && audit->quiet_finished_us >= audit->quiet_started_us &&
        audit->quiet_finished_us - audit->quiet_started_us >= OKL_LOADER_QUIET_US &&
        audit->reset_boundary_established && !audit->persistence_failed &&
        !audit->cancelled_after_commit && !atomic_load(&blocked) &&
        audit->observation.kind == OKL_LOADER_OBSERVATION_APPLICATION &&
        audit->observation.dark_state_verified == 1 &&
        !memcmp(audit->observation.version.component, job.image.version.component, 4) &&
        audit->observation.controller.abi_major == 1 && !audit->observation.controller.abi_minor &&
        audit->observation.controller.role == OKL_ROLE_LIGHTING &&
        audit->observation.controller.capabilities == (OKL_CAP_RECOVERY_READY | OKL_CAP_LIGHTING_READY) &&
        audit->observation.controller.part_id == OKL_LOADER_PART_ID && !audit->observation.controller.boot_requested;
    if ((unsigned)result <= OKL_LOADER_UNRESOLVED) job.audit.result = result;
    bool clear_failed = complete && write_journal(NULL) != ESP_OK;
    if (clear_failed) { complete = false; job.audit.result = OKL_LOADER_PERSIST; }
    else if (!complete && job.audit.result == OKL_LOADER_OK) job.audit.result = OKL_LOADER_UNRESOLVED;
    job.confirmed = complete;
    job.state = complete ? JOB_SUCCEEDED : JOB_RECOVERY_REQUIRED;
    if (!complete) disable_locked(clear_failed ? "Controller update journal clear failed; explicit recovery required" :
        error && error[0] ? error : "Controller update unresolved; explicit recovery required");
    else {
        job.error[0] = 0;
        snprintf(app.operation, sizeof(app.operation), "idle"); app.error[0] = 0;
    }
    uint8_t *package = job.package;
    job.package = NULL; job.image.package = NULL; job.image.size = 0;
    app.updating = false;
    app_event_locked("update", complete ? "controller.updated" : "controller.unresolved",
        complete ? "Controller image verified and typed trial confirmed" : job.error);
    app_unlock();
    free(package);
    return complete;
}

void app_controller_update_diagnostic_finish(uint32_t id, const okl_loader_audit *audit,
                                           okl_loader_result result,
                                           const app_controller_worker_outcome *outcome) {
    app_lock();
    if (job.state != JOB_RUNNING || job.id != id) { app_unlock(); return; }
    bool observed = audit_valid(audit) && job.image.role == OKL_ROLE_SPI_DIAGNOSTIC &&
        result == OKL_LOADER_OK && audit->result == OKL_LOADER_OK &&
        audit->phase == OKL_LOADER_APPLICATION_SEEN && audit->loader_verified && audit->erase_attempted &&
        audit->complete_bank_verified && audit->program_blocks_acked == OKL_LOADER_BLOCKS &&
        audit->readback_blocks_verified == OKL_LOADER_BLOCKS && audit->commit_attempted &&
        audit->commit_delivery == OKL_LOADER_SENT_COMPLETE && !audit->abort_attempted &&
        audit->quiet_completed && audit->quiet_finished_us >= audit->quiet_started_us &&
        audit->quiet_finished_us - audit->quiet_started_us >= OKL_LOADER_QUIET_US &&
        audit->reset_boundary_established && !audit->persistence_failed && !audit->cancelled_after_commit &&
        !atomic_load(&blocked) && audit->observation.kind == OKL_LOADER_OBSERVATION_APPLICATION &&
        audit->observation.dark_state_verified == 1 &&
        !memcmp(audit->observation.version.component, job.image.version.component, 4) &&
        audit->observation.controller.abi_major == 1 && !audit->observation.controller.abi_minor &&
        audit->observation.controller.role == OKL_ROLE_SPI_DIAGNOSTIC &&
        audit->observation.controller.capabilities == OKL_CAP_RECOVERY_READY &&
        audit->observation.controller.part_id == OKL_LOADER_PART_ID &&
        !audit->observation.controller.boot_requested && !audit->observation.controller.trial_confirmed &&
        outcome && outcome->diagnostic_trial_observed;
    if (audit_valid(audit)) job.audit = *audit;
    if ((unsigned)result <= OKL_LOADER_UNRESOLVED) job.audit.result = result;
    if (outcome) job.diagnostic = *outcome;
    job.diagnostic.diagnostic_trial_observed = observed;
    bool proof = observed && outcome->synchronized && outcome->profile_verified &&
        outcome->command_attempted && outcome->command_acknowledged && outcome->registers_verified &&
        app_diagnostic_registers(outcome->diagnostic_words, outcome->diagnostic_words[15]) &&
        outcome->resident_proof_job_id == id;
    job.resident_proof_job_id = proof ? id : 0;
    job.diagnostic.resident_proof_job_id = job.resident_proof_job_id;
    job.confirmed = false;
    job.state = observed ? JOB_DIAGNOSTIC_TRIAL : JOB_RECOVERY_REQUIRED;
    atomic_store(&blocked, true);
    app.controller_ready = false; app.reported_valid = false; app.reported_fields = 0;
    app.controller_trial_confirmed = false;
    app.controller_connected = proof;
    if (observed) {
        snprintf(app.controller_backend, sizeof(app.controller_backend), "original");
        snprintf(app.controller_version, sizeof(app.controller_version), "%u.%u.%u.%u",
            job.image.version.component[0], job.image.version.component[1],
            job.image.version.component[2], job.image.version.component[3]);
        app.controller_part_id = OKL_LOADER_PART_ID;
    }
    snprintf(app.controller_status, sizeof(app.controller_status), "%s", proof ? "loader" : observed ? "diagnostic" : "fault");
    snprintf(app.operation, sizeof(app.operation), "pending");
    snprintf(job.error, sizeof(job.error), "%s", proof ? "" :
        outcome && outcome->diagnostic_error[0] ? outcome->diagnostic_error : "Diagnostic unresolved; explicit recovery required");
    snprintf(app.error, sizeof(app.error), "%s", job.error);
    uint8_t *package = job.package;
    job.package = NULL; job.image.package = NULL; job.image.size = 0; app.updating = false;
    app_event_locked("update", proof ? "controller.diagnostic" : "controller.unresolved", proof ?
        "Off diagnostic and resident return verified; explicit next package required" : job.error);
    app_unlock(); free(package);
}

static bool read_only_failure(const okl_loader_audit *a) {
    return a && a->phase == OKL_LOADER_PRECOMMIT_FAILED &&
        (a->result == OKL_LOADER_IO || a->result == OKL_LOADER_INVALID) &&
        !a->loader_verified && !a->erase_attempted && !a->complete_bank_verified &&
        !a->program_blocks_acked && !a->readback_blocks_verified &&
        !a->commit_attempted && !a->abort_attempted && !a->quiet_completed &&
        !a->reset_boundary_established && !a->persistence_failed && !a->cancelled_after_commit &&
        a->commit_delivery == OKL_LOADER_NOT_SENT && a->abort_delivery == OKL_LOADER_NOT_SENT &&
        !a->quiet_started_us && !a->quiet_finished_us &&
        a->observation.kind == OKL_LOADER_OBSERVATION_UNKNOWN;
}

bool app_controller_update_reject(uint32_t id, const okl_loader_audit *audit,
                                  okl_loader_result result,
                                  const app_controller_worker_outcome *outcome) {
    app_lock();
    /* The live worker proof cannot be reconstructed from a journal after a
     * reboot. Check both the manager's last persisted audit and the returned
     * one; neither may contain a loader operation or storage failure. */
    bool proven = job.state == JOB_RUNNING && job.id == id && !atomic_load(&blocked) &&
        outcome && outcome->entry == APP_CONTROLLER_READ_ONLY_UNSUPPORTED && outcome->synchronized &&
        result == OKL_LOADER_INVALID && audit_valid(audit) && audit->result == result &&
        (job.source == OKL_LOADER_FROM_ORIGINAL || job.source == OKL_LOADER_FROM_LEGACY_1_3) &&
        read_only_failure(audit) && read_only_failure(&job.audit);
    if (!proven) { app_unlock(); return false; }
    job.audit = *audit;
    bool cleared = write_journal(NULL) == ESP_OK;
    job.confirmed = false;
    job.state = cleared ? JOB_FAILED : JOB_RECOVERY_REQUIRED;
    app.controller_ready = false; app.reported_valid = false; app.reported_fields = 0;
    if (cleared) {
        snprintf(job.error, sizeof(job.error), "Unsupported controller target; update rejected before any mutation");
        snprintf(app.controller_status, sizeof(app.controller_status), "starting");
        snprintf(app.operation, sizeof(app.operation), "idle"); app.error[0] = 0;
    } else {
        job.audit.result = OKL_LOADER_PERSIST;
        disable_locked("Controller rejection journal clear failed; explicit recovery required");
    }
    uint8_t *package = job.package;
    job.package = NULL; job.image.package = NULL; job.image.size = 0;
    app.updating = false;
    app_event_locked("update", cleared ? "controller.rejected" : "controller.unresolved", job.error);
    app_unlock();
    free(package);
    return cleared;
}

cJSON *app_controller_update_json(void) {
    static const char *states[] = {"idle", "receiving", "queued", "running", "completed", "recovery_required", "failed", "diagnostic_trial"};
    static const char *phases[] = {"idle", "validated", "entering", "ready", "erasing", "programming",
        "verifying", "committing", "quiet", "observing", "application_seen", "precommit_failed", "commit_unresolved"};
    static const char *results[] = {"ok", "invalid", "busy", "cancelled", "timeout", "io", "protocol", "verify", "persist", "unresolved"};
    static const char *deliveries[] = {"not_sent", "complete", "maybe_sent"};
    app_lock();
    enum job_state state = job.state; uint32_t id = job.id; bool confirmed = job.confirmed;
    bool target_known = job.target_known;
    uint8_t role = job.image.role;
    bool resident_ready = job.resident_proof_job_id != 0;
    app_controller_worker_outcome diagnostic = job.diagnostic;
    okl_loader_audit audit = job.audit; bool recovery = atomic_load(&blocked);
    uint8_t digest[32]; memcpy(digest, job.image.bank_sha256, 32);
    char error[81]; memcpy(error, job.error, sizeof(error));
    app_unlock();
    char hex[65]; for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    cJSON_AddStringToObject(json, "state", states[state]);
    if (id) cJSON_AddNumberToObject(json, "job_id", id);
    else cJSON_AddNullToObject(json, "job_id");
    cJSON_AddStringToObject(json, "phase", phases[audit.phase]);
    if (state == JOB_SUCCEEDED || state == JOB_RECOVERY_REQUIRED || state == JOB_FAILED || state == JOB_DIAGNOSTIC_TRIAL)
        cJSON_AddStringToObject(json, "result", results[audit.result]);
    else cJSON_AddNullToObject(json, "result");
    cJSON_AddNumberToObject(json, "program_blocks_acked", audit.program_blocks_acked);
    cJSON_AddNumberToObject(json, "readback_blocks_verified", audit.readback_blocks_verified);
    cJSON_AddNumberToObject(json, "total_blocks", OKL_LOADER_BLOCKS);
    if (!target_known) cJSON_AddNullToObject(json, "target_sha256");
    else cJSON_AddStringToObject(json, "target_sha256", hex);
    cJSON_AddBoolToObject(json, "commit_attempted", audit.commit_attempted != 0);
    cJSON_AddBoolToObject(json, "quiet_completed", audit.quiet_completed != 0);
    cJSON_AddStringToObject(json, "commit_delivery", deliveries[(unsigned)audit.commit_delivery <= OKL_LOADER_MAYBE_SENT ? audit.commit_delivery : OKL_LOADER_MAYBE_SENT]);
    cJSON_AddBoolToObject(json, "controller_confirmed", confirmed);
    cJSON_AddBoolToObject(json, "recovery_required", recovery);
    if (target_known) cJSON_AddNumberToObject(json, "target_role", role);
    else cJSON_AddNullToObject(json, "target_role");
    cJSON_AddBoolToObject(json, "diagnostic_trial_observed", diagnostic.diagnostic_trial_observed);
    cJSON_AddBoolToObject(json, "resident_recovery_ready", resident_ready);
    if (role == OKL_ROLE_SPI_DIAGNOSTIC && target_known) {
        cJSON *detail = cJSON_AddObjectToObject(json, "diagnostic");
        cJSON_AddStringToObject(detail, "profile", "OFF1");
        cJSON_AddBoolToObject(detail, "command_attempted", diagnostic.command_attempted);
        cJSON_AddBoolToObject(detail, "command_acknowledged", diagnostic.command_acknowledged);
        cJSON_AddBoolToObject(detail, "registers_verified", diagnostic.registers_verified);
        cJSON_AddNumberToObject(detail, "generation", diagnostic.diagnostic_words[15]);
        cJSON *words = cJSON_AddArrayToObject(detail, "snapshot_words");
        for (unsigned i = 0; i < APP_DIAGNOSTIC_WORDS; ++i)
            cJSON_AddItemToArray(words, cJSON_CreateNumber(diagnostic.diagnostic_words[i]));
        if (diagnostic.diagnostic_error[0]) cJSON_AddStringToObject(detail, "error", diagnostic.diagnostic_error);
        else cJSON_AddNullToObject(detail, "error");
    } else cJSON_AddNullToObject(json, "diagnostic");
    if (error[0]) cJSON_AddStringToObject(json, "error", error);
    else cJSON_AddNullToObject(json, "error");
    return json;
}
