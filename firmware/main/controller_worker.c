#include "controller_worker.h"
#include "nxp_transport.h"
#include "output_policy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <string.h>

typedef struct { okl_nxp *driver; const app_controller_job *job; uint64_t deadline; } update_context;

static uint64_t clock_us(void *user) {
    update_context *c = user;
    return c->driver->transport.now_us(c->driver->transport.user);
}
static uint64_t bounded(update_context *c, uint64_t duration) {
    uint64_t now = clock_us(c);
    return c->deadline - (now < c->deadline ? now : c->deadline) < duration ? c->deadline : now + duration;
}
static int digest(void *user, const uint8_t *data, size_t size, uint8_t out[32]) {
    (void)user; return mbedtls_sha256(data, size, out, 0);
}
static okl_result query(update_context *c, okl_command command, okl_reply *reply) {
    okl_request request; okl_result result = okl_request_get(&request, command);
    return result == OKL_OK ? okl_nxp_execute(c->driver, &request, reply, bounded(c, 150000)) : result;
}
static bool qualified(const okl_controller_status *s) {
    return s->abi_major == 1 && !s->abi_minor && s->role == OKL_ROLE_LIGHTING &&
        s->capabilities == (OKL_CAP_RECOVERY_READY | OKL_CAP_LIGHTING_READY) &&
        s->part_id == OKL_LOADER_PART_ID && !s->boot_requested;
}
static int original_identity(update_context *c, okl_controller_status *status) {
    okl_reply reply; uint32_t part;
    if (!qualified(status) || query(c, OKL_GET_PART_ID, &reply) != OKL_OK ||
        okl_reply_decode_part_id(&part, &reply) != OKL_OK || part != status->part_id) return -1;
    return 0;
}
static int begin(void *user, uint64_t deadline) {
    update_context *c = user; c->deadline = deadline;
    return app_nxp_loader_acquire(c->driver, c->job->id, deadline) == OKL_OK ? 0 : -1;
}
static void end(void *user) {
    update_context *c = user; app_nxp_loader_release(c->driver, c->job->id);
}
static int persist(void *user, const okl_loader_audit *audit) {
    update_context *c = user; return app_controller_update_persist(c->job->id, audit);
}
static void progress(void *user, const okl_loader_audit *audit) {
    update_context *c = user; app_controller_update_progress(c->job->id, audit);
}
static void wait_until(void *user, uint64_t deadline) {
    uint64_t now = clock_us(user);
    if (now < deadline) {
        uint64_t ms = (deadline - now + 999) / 1000;
        vTaskDelay(pdMS_TO_TICKS(ms > 10 ? 10 : ms));
    }
}
static int cancelled(void *user) { (void)user; return 0; } /* No post-admission cancel endpoint. */
static okl_result output(void *user, const okl_request *request) {
    update_context *c = user; okl_reply reply;
    return okl_nxp_execute(c->driver, request, &reply, bounded(c, 150000));
}
static int enter(void *user, okl_loader_source source, uint64_t deadline) {
    update_context *c = user; c->deadline = deadline;
    okl_reply reply; okl_firmware_version version; okl_controller_status status;
    const uint8_t legacy[4] = {1, 3, 0, 0};
    if (source != OKL_LOADER_FROM_ORIGINAL && source != OKL_LOADER_FROM_LEGACY_1_3) return -1;
    if (query(c, OKL_GET_FIRMWARE, &reply) != OKL_OK || okl_reply_decode_firmware(&version, &reply) != OKL_OK) return -1;
    okl_result result = query(c, OKL_GET_CONTROLLER_STATUS, &reply);
    if (source == OKL_LOADER_FROM_ORIGINAL) {
        if (result != OKL_OK || okl_reply_decode_controller_status(&status, &reply) != OKL_OK ||
            original_identity(c, &status)) return -1;
    } else {
        uint32_t part;
        if (memcmp(version.component, legacy, 4) || result != OKL_REMOTE || !reply.received ||
            reply.report.status != 5 || reply.report.command_class || reply.report.opcode != 0xfc || reply.report.size ||
            query(c, OKL_GET_PART_ID, &reply) != OKL_OK || okl_reply_decode_part_id(&part, &reply) != OKL_OK ||
            part != OKL_LOADER_PART_ID) return -1;
    }
    if (okl_nxp_claim(c->driver, (const uint8_t *)"Open Keylight", 13, bounded(c, 600000)) != OKL_OK) return -1;
    /* The durable ENTERING journal exists before either Off write. Verify
     * native Off settings, never replay the previous scene. These getters do
     * not measure pin/optical darkness or legacy fade completion. */
    okl_light_state state;
    if (okl_nxp_read_state(c->driver, &state, bounded(c, 800000)) != OKL_OK) return -1;
    uint8_t effect = state.effect; kl_state off = kl_state_default(); off.power = false;
    if (kl_output_prepare(&off, false, NULL, &effect, output, c) != OKL_OK ||
        okl_nxp_read_state(c->driver, &state, bounded(c, 800000)) != OKL_OK ||
        state.effect || state.white_brightness) return -1;
    if (source == OKL_LOADER_FROM_ORIGINAL) {
        if (query(c, OKL_GET_CONTROLLER_STATUS, &reply) != OKL_OK ||
            okl_reply_decode_controller_status(&status, &reply) != OKL_OK || !qualified(&status)) return -1;
    }
    okl_loader_delivery delivery;
    result = app_nxp_loader_enter(c->driver, c->job->id, source, &delivery, bounded(c, 2000000));
    /* Even a failed invocation may have reset the peer. Stay silent once;
     * neither a missing ACK nor READY-high permits retrying the mutation. */
    uint64_t quiet_end = clock_us(c) + OKL_LOADER_QUIET_US;
    while (clock_us(c) < quiet_end) wait_until(c, quiet_end);
    if (result != OKL_OK || delivery != OKL_LOADER_SENT_COMPLETE) return -1;
    return app_nxp_loader_reset_boundary(c->driver, c->job->id, 0x84, delivery, deadline) == OKL_OK ? 0 : -1;
}
static int exchange(void *user, const uint8_t request[90], uint8_t response[90],
                    okl_loader_delivery *delivery, uint64_t deadline) {
    update_context *c = user; c->deadline = deadline;
    return app_nxp_loader_exchange(c->driver, c->job->id, request, response, delivery, bounded(c, 2000000)) == OKL_OK ? 0 : -1;
}
static int send_only(void *user, const uint8_t request[90], okl_loader_delivery *delivery, uint64_t deadline) {
    update_context *c = user; c->deadline = deadline;
    return app_nxp_loader_send_only(c->driver, c->job->id, request, delivery, bounded(c, 2000000)) == OKL_OK ? 0 : -1;
}
static int reset_boundary(void *user, uint8_t opcode, okl_loader_delivery delivery, uint64_t deadline) {
    update_context *c = user;
    return app_nxp_loader_reset_boundary(c->driver, c->job->id, opcode, delivery, deadline) == OKL_OK ? 0 : -1;
}
static int observe(void *user, okl_loader_observation *observation, uint64_t deadline) {
    update_context *c = user; c->deadline = deadline;
    okl_loader_observation value = {0}; okl_reply reply; okl_light_state state;
    if (query(c, OKL_GET_FIRMWARE, &reply) != OKL_OK || okl_reply_decode_firmware(&value.version, &reply) != OKL_OK ||
        query(c, OKL_GET_CONTROLLER_STATUS, &reply) != OKL_OK ||
        okl_reply_decode_controller_status(&value.controller, &reply) != OKL_OK || original_identity(c, &value.controller) ||
        okl_nxp_read_state(c->driver, &state, bounded(c, 800000)) != OKL_OK) return -1;
    value.kind = OKL_LOADER_OBSERVATION_APPLICATION;
    /* This is a native-state predicate, not a physical output measurement. */
    value.dark_state_verified = state.effect == 0 && state.white_brightness == 0;
    *observation = value; return 0;
}
okl_loader_result app_controller_worker_run(okl_nxp *driver, const app_controller_job *job, okl_loader_audit *audit) {
    if (!driver || !job || !audit) return OKL_LOADER_INVALID;
    update_context context = {.driver = driver, .job = job};
    okl_loader_ops ops = {.user = &context, .sha256 = digest, .now_us = clock_us,
        .begin = begin, .end = end, .persist = persist, .enter_loader = enter,
        .exchange = exchange, .send_only = send_only, .wait_until = wait_until,
        .cancelled = cancelled, .reset_boundary = reset_boundary, .observe_readonly = observe, .progress = progress};
    return okl_loader_run(&job->image, job->source, &ops, clock_us(&context) + UINT64_C(120000000), audit);
}
