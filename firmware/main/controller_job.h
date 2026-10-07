#ifndef KEYLIGHT_CONTROLLER_JOB_H
#define KEYLIGHT_CONTROLLER_JOB_H

#include "okl_loader.h"
#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    uint32_t id;
    const uint8_t *package;
    okl_loader_image image;
    okl_loader_source source;
} app_controller_job;

/* Call once after NVS initialization, before starting the controller worker.
 * A pending, malformed or unreadable journal blocks automatic bootstrap;
 * it is evidence of an interrupted job, never an instruction to resume it. */
esp_err_t app_controller_update_init(void);
bool app_controller_update_blocked(void); /* Safe while app.mutex is held. */
int app_controller_update_begin(uint32_t *job_id);
/* 202 transfers ownership; every other result leaves ownership with caller. */
int app_controller_update_submit(uint32_t id, uint8_t *package, size_t size);
void app_controller_update_cancel_upload(uint32_t id);
cJSON *app_controller_update_json(void);

/* Worker-only: take does not transfer allocation ownership. The manager frees
 * the package exactly once on finish. Source must be freshly requalified by
 * the worker before entry; cached admission is not a device measurement. */
bool app_controller_update_take(app_controller_job *out);
void app_controller_update_progress(uint32_t id, const okl_loader_audit *audit);
int app_controller_update_persist(uint32_t id, const okl_loader_audit *audit);
/* True only for verified, typed-confirmed success AND durable journal clear.
 * False keeps normal bootstrap/output gated, even if confirmation succeeded. */
bool app_controller_update_finish(uint32_t id, const okl_loader_audit *audit,
                                  okl_loader_result result, bool confirmed,
                                  const char *error);

#endif
