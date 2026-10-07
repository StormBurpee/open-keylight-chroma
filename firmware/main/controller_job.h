#ifndef KEYLIGHT_CONTROLLER_JOB_H
#define KEYLIGHT_CONTROLLER_JOB_H

#include "okl_loader.h"
#include "cJSON.h"
#include "esp_err.h"
#include "controller_profile.h"
#include <stdbool.h>

typedef struct {
    uint32_t id;
    const uint8_t *package;
    okl_loader_image image;
    okl_loader_source source;
    uint32_t resident_proof_job_id;
    bool recovery_only, allow_legacy_reconcile;
    uint8_t diagnostic_profile;
} app_controller_job;

/* Volatile evidence produced only by the sole worker's update adapter. A
 * read-only rejection requires fully correlated replies and known idle after
 * releasing the lease. Set MUTATION_ATTEMPTED before invoking ownership claim,
 * even if that call later fails; no later observation may downgrade it. */
typedef enum {
    APP_CONTROLLER_ENTRY_UNPROVEN,
    APP_CONTROLLER_READ_ONLY_UNSUPPORTED,
    APP_CONTROLLER_MUTATION_ATTEMPTED
} app_controller_entry_outcome;
typedef struct {
    app_controller_entry_outcome entry;
    bool synchronized;
    bool diagnostic_trial_observed;
    bool profile_verified, command_attempted, command_acknowledged, registers_verified;
    uint32_t resident_proof_job_id;
    uint32_t diagnostic_words[256];
    uint8_t diagnostic_profile;
    char diagnostic_error[81];
    bool legacy_reconciled;
    char stage[32];
    int transport_result;
    bool reply_received;
    uint8_t reply_status, reply_class, reply_opcode, reply_size;
    char reply_sha256[65];
    bool transport_snapshot, raw_reply_received;
    unsigned transport_phase, ready, reply_kind;
    uint8_t routing_tag[6];
} app_controller_worker_outcome;

/* Call once after NVS initialization, before starting the controller worker.
 * A pending, malformed or unreadable journal blocks automatic bootstrap;
 * it is evidence of an interrupted job, never an instruction to resume it. */
esp_err_t app_controller_update_init(void);
bool app_controller_update_blocked(void); /* Safe while app.mutex is held. */
int app_controller_update_begin(uint32_t *job_id);
/* Explicit bench admission; role must be diagnostic1 or production2. */
int app_controller_update_begin_role(uint32_t *job_id, uint8_t role);
int app_controller_update_begin_mode(uint32_t *job_id, uint8_t diagnostic_profile);
int app_controller_recovery_begin(uint32_t expected_job_id, bool power_cycle_acknowledged);
bool app_controller_recovery_finish(uint32_t id, const app_controller_worker_outcome *outcome);
/* 202 transfers ownership; every other result leaves ownership with caller. */
int app_controller_update_submit(uint32_t id, uint8_t *package, size_t size);
void app_controller_update_cancel_upload(uint32_t id);
cJSON *app_controller_update_json(void);

/* Worker-only: take does not transfer allocation ownership. The manager frees
 * the package exactly once on finish. Source must be freshly requalified by
 * the worker before entry; cached admission is not a device measurement. */
bool app_controller_update_take(app_controller_job *out);
void app_controller_update_progress(uint32_t id, const okl_loader_audit *audit);
void app_controller_update_record_outcome(uint32_t id, const app_controller_worker_outcome *outcome);
int app_controller_update_persist(uint32_t id, const okl_loader_audit *audit);
/* True only for verified, typed-confirmed success AND durable journal clear.
 * False keeps normal bootstrap/output gated, even if confirmation succeeded. */
bool app_controller_update_finish(uint32_t id, const okl_loader_audit *audit,
                                  okl_loader_result result, bool confirmed,
                                  const char *error);
/* Role1 never confirms or clears its journal. Returns no output readiness;
 * any fresh-resident capability is volatile and consumed by a new job. */
void app_controller_update_diagnostic_finish(uint32_t id, const okl_loader_audit *audit,
                                           okl_loader_result result,
                                           const app_controller_worker_outcome *outcome);
/* Separate failed-job path, never successful installation. True means a
 * synchronized, read-only incompatibility was durably cleared. Readiness stays
 * closed until fresh normal bootstrap; no interrupted journal can use this. */
bool app_controller_update_reject(uint32_t id, const okl_loader_audit *audit,
                                  okl_loader_result result,
                                  const app_controller_worker_outcome *outcome);

#endif
