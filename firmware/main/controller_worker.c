#include "controller_worker.h"
#include "nxp_transport.h"
#include "output_policy.h"
#include "controller_diagnostic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    okl_nxp *driver;
    const app_controller_job *job;
    uint64_t deadline;
    bool leased;
    app_controller_worker_outcome outcome;
} update_context;

static uint64_t clock_us(void *user) {
    update_context *c = user;
    return c->driver->transport.now_us(c->driver->transport.user);
}
static uint64_t bounded(update_context *c, uint64_t duration) {
    uint64_t now = clock_us(c);
    return c->deadline - (now < c->deadline ? now : c->deadline) < duration ? c->deadline : now + duration;
}
static void trace_wire(update_context *c) {
    if (!c->driver->transport.now_us) return;
    app_nxp_transport_diagnostic snapshot;
    if (app_nxp_transport_snapshot(c->driver, &snapshot, clock_us(c) + 150000) != OKL_OK) return;
    c->outcome.transport_snapshot = true;
    c->outcome.transport_phase = snapshot.phase;
    c->outcome.ready = snapshot.ready;
    c->outcome.raw_reply_received = snapshot.has_report != 0;
    c->outcome.reply_kind = snapshot.reply_kind;
    memcpy(c->outcome.routing_tag, snapshot.routing_tag, 6);
    if (!snapshot.has_report) return;
    uint8_t sha[32];
    if (!mbedtls_sha256(snapshot.report, 90, sha, 0)) {
        for (unsigned i = 0; i < 32; ++i) snprintf(c->outcome.reply_sha256 + i * 2, 3, "%02x", sha[i]);
        c->outcome.reply_status = snapshot.report[0]; c->outcome.reply_size = snapshot.report[5];
        c->outcome.reply_class = snapshot.report[6]; c->outcome.reply_opcode = snapshot.report[7];
    }
}
static void trace(update_context *c, const char *stage, okl_result result, const okl_reply *reply) {
    snprintf(c->outcome.stage, sizeof(c->outcome.stage), "%s", stage);
    c->outcome.transport_result = result;
    c->outcome.reply_received = reply && reply->received;
    c->outcome.reply_status = c->outcome.reply_class = c->outcome.reply_opcode = c->outcome.reply_size = 0;
    c->outcome.reply_sha256[0] = 0;
    c->outcome.transport_snapshot = c->outcome.raw_reply_received = false;
    c->outcome.transport_phase = c->outcome.ready = c->outcome.reply_kind = 0;
    memset(c->outcome.routing_tag, 0, sizeof(c->outcome.routing_tag));
    if (reply && reply->received) {
        c->outcome.reply_status = reply->report.status;
        c->outcome.reply_class = reply->report.command_class;
        c->outcome.reply_opcode = reply->report.opcode;
        c->outcome.reply_size = reply->report.size;
    }
    /* Cache at the failing operation, before Abort or lease cleanup can
     * replace the transport's last body. This helper clocks no SPI bytes. */
    trace_wire(c);
}
static int digest(void *user, const uint8_t *data, size_t size, uint8_t out[32]) {
    (void)user; return mbedtls_sha256(data, size, out, 0);
}
static okl_result query(update_context *c, okl_command command, okl_reply *reply) {
    okl_request request; okl_result result = okl_request_get(&request, command);
    memset(reply, 0, sizeof(*reply));
    if (result == OKL_OK) result = okl_nxp_execute(c->driver, &request, reply, bounded(c, 150000));
    trace(c, command == OKL_GET_FIRMWARE ? "identity.version" : command == OKL_GET_PART_ID ?
        "identity.part" : "identity.status", result, reply);
    return result;
}
static bool qualified(const okl_controller_status *s) {
    return s->abi_major == 1 && !s->abi_minor && s->role == OKL_ROLE_LIGHTING &&
        s->capabilities == (OKL_CAP_RECOVERY_READY | OKL_CAP_LIGHTING_READY) &&
        s->part_id == OKL_LOADER_PART_ID && !s->boot_requested;
}
static int original_identity(update_context *c, okl_controller_status *status) {
    okl_reply reply; uint32_t part;
    bool identity = qualified(status);
    if (c->job->image.role == OKL_ROLE_SPI_DIAGNOSTIC)
        identity = status->abi_major == 1 && !status->abi_minor &&
            status->role == OKL_ROLE_SPI_DIAGNOSTIC && status->capabilities == OKL_CAP_RECOVERY_READY &&
            status->part_id == OKL_LOADER_PART_ID && !status->boot_requested && !status->trial_confirmed;
    if (!identity || query(c, OKL_GET_PART_ID, &reply) != OKL_OK ||
        okl_reply_decode_part_id(&part, &reply) != OKL_OK || part != status->part_id) return -1;
    return 0;
}
static int begin(void *user, uint64_t deadline) {
    update_context *c = user; c->deadline = deadline;
    c->leased = app_nxp_loader_acquire(c->driver, c->job->id, deadline) == OKL_OK;
    return c->leased ? 0 : -1;
}
static void end(void *user) {
    update_context *c = user; app_nxp_loader_release(c->driver, c->job->id);
    /* Release propagates any non-idle physical phase to needs_recovery. The
     * sole worker still owns execution; no other SPI caller can intervene. */
    c->outcome.synchronized = c->leased && !c->driver->needs_recovery;
    c->leased = false;
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
static bool unsupported_reply(okl_result result, const okl_reply *reply, uint8_t opcode) {
    return result == OKL_REMOTE && reply->received && !reply->acknowledged &&
        reply->report.status == 5 && !reply->report.command_class &&
        reply->report.opcode == opcode && !reply->report.size;
}
static bool legacy_owner_denied(okl_result result, const okl_reply *reply) {
    return result == OKL_OWNER_DENIED && reply->received && !reply->acknowledged &&
        reply->report.status == 8 && !reply->report.command_class &&
        reply->report.opcode == 0xfc && !reply->report.size;
}
static int reject_unsupported(update_context *c) {
    if (c->outcome.entry == APP_CONTROLLER_ENTRY_UNPROVEN && !c->driver->needs_recovery)
        c->outcome.entry = APP_CONTROLLER_READ_ONLY_UNSUPPORTED;
    return -1;
}
static int enter(void *user, okl_loader_source source, uint64_t deadline) {
    update_context *c = user; c->deadline = deadline;
    okl_reply reply; okl_firmware_version version; okl_controller_status status;
    const uint8_t legacy[4] = {1, 3, 0, 0};
    bool claimed = false;
    if (source == OKL_LOADER_FROM_FRESH_RESIDENT) {
        /* This capability is created only by this boot's completed OFF1 job;
         * the transport consumes it once. Neither HTTP nor NVS can forge it. */
        return c->job->resident_proof_job_id &&
            app_nxp_loader_use_resident(c->driver, c->job->id, c->job->resident_proof_job_id,
                deadline) == OKL_OK ? 0 : -1;
    }
    if (source != OKL_LOADER_FROM_ORIGINAL && source != OKL_LOADER_FROM_LEGACY_1_3) return -1;
    okl_result result = query(c, OKL_GET_FIRMWARE, &reply);
    if (unsupported_reply(result, &reply, 0x87)) return reject_unsupported(c);
    if (result != OKL_OK || okl_reply_decode_firmware(&version, &reply) != OKL_OK) return -1;
    if (source == OKL_LOADER_FROM_LEGACY_1_3 && memcmp(version.component, legacy, 4))
        return reject_unsupported(c);
    result = query(c, OKL_GET_CONTROLLER_STATUS, &reply);
    if (source == OKL_LOADER_FROM_LEGACY_1_3 && legacy_owner_denied(result, &reply)) {
        /* Exact legacy version is known before this ownership exception.
         * Unknown FC is owner-gated by that application. Claim once, then
         * require the same unsupported-FC and exact-part proof as usual.
         * Claim is a mutation even if its reply is lost: this path can never
         * use the read-only journal-rejection shortcut afterward. */
        c->outcome.entry = APP_CONTROLLER_MUTATION_ATTEMPTED;
        result = okl_nxp_claim(c->driver, (const uint8_t *)"Open Keylight", 13, bounded(c, 600000));
        trace(c, "entry.claim", result, NULL);
        if (result != OKL_OK) return -1;
        claimed = true;
        result = query(c, OKL_GET_CONTROLLER_STATUS, &reply);
    }
    if (source == OKL_LOADER_FROM_ORIGINAL) {
        if (unsupported_reply(result, &reply, 0xfc)) return reject_unsupported(c);
        if (result != OKL_OK || okl_reply_decode_controller_status(&status, &reply) != OKL_OK) return -1;
        if (!qualified(&status)) return reject_unsupported(c);
    } else {
        if (!unsupported_reply(result, &reply, 0xfc)) {
            /* A valid original identity disproves the admitted legacy source;
             * malformed or merely missing status remains unproven. */
            if (result == OKL_OK && okl_reply_decode_controller_status(&status, &reply) == OKL_OK)
                return reject_unsupported(c);
            return -1;
        }
    }
    uint32_t part;
    result = query(c, OKL_GET_PART_ID, &reply);
    if (unsupported_reply(result, &reply, 0xfe)) return reject_unsupported(c);
    if (result != OKL_OK || okl_reply_decode_part_id(&part, &reply) != OKL_OK) return -1;
    if (part != OKL_LOADER_PART_ID || (source == OKL_LOADER_FROM_ORIGINAL && part != status.part_id))
        return reject_unsupported(c);
    c->outcome.entry = APP_CONTROLLER_MUTATION_ATTEMPTED;
    if (!claimed) {
        result = okl_nxp_claim(c->driver, (const uint8_t *)"Open Keylight", 13, bounded(c, 600000));
        trace(c, "entry.claim", result, NULL);
        if (result != OKL_OK) return -1;
    }
    /* The durable ENTERING journal exists before either Off write. Verify
     * native Off settings, never replay the previous scene. These getters do
     * not measure pin/optical darkness or legacy fade completion. */
    okl_light_state state;
    result = okl_nxp_read_state(c->driver, &state, bounded(c, 800000));
    trace(c, "entry.state", result, NULL);
    if (result != OKL_OK) return -1;
    uint8_t effect = state.effect; kl_state off = kl_state_default(); off.power = false;
    result = kl_output_prepare(&off, false, NULL, &effect, output, c);
    trace(c, "entry.off", result, NULL);
    if (result != OKL_OK) return -1;
    result = okl_nxp_read_state(c->driver, &state, bounded(c, 800000));
    if (result == OKL_OK && (state.effect || state.white_brightness)) result = OKL_VERIFY;
    trace(c, "entry.off_readback", result, NULL);
    if (result != OKL_OK) return -1;
    if (source == OKL_LOADER_FROM_ORIGINAL) {
        if (query(c, OKL_GET_CONTROLLER_STATUS, &reply) != OKL_OK ||
            okl_reply_decode_controller_status(&status, &reply) != OKL_OK || !qualified(&status)) return -1;
    }
    okl_loader_delivery delivery;
    result = app_nxp_loader_enter(c->driver, c->job->id, source, &delivery, bounded(c, 2000000));
    trace(c, "entry.send", result, NULL);
    /* Even a failed invocation may have reset the peer. Stay silent once;
     * neither a missing ACK nor READY-high permits retrying the mutation. */
    uint64_t quiet_end = clock_us(c) + OKL_LOADER_QUIET_US;
    while (clock_us(c) < quiet_end) wait_until(c, quiet_end);
    if (result != OKL_OK) return -1;
    if (delivery != OKL_LOADER_SENT_COMPLETE) {
        trace(c, "entry.delivery", OKL_PROTOCOL, NULL); return -1;
    }
    result = app_nxp_loader_reset_boundary(c->driver, c->job->id, 0x84, delivery, deadline);
    trace(c, "entry.reset_boundary", result, NULL);
    return result == OKL_OK ? 0 : -1;
}
static int exchange(void *user, const uint8_t request[90], uint8_t response[90],
                    okl_loader_delivery *delivery, uint64_t deadline) {
    update_context *c = user; c->deadline = deadline;
    okl_result result = app_nxp_loader_exchange(c->driver, c->job->id, request, response, delivery, bounded(c, 2000000));
    trace(c, request[7] == 0x80 ? "loader.information" : request[7] == 1 ? "loader.start" :
        request[7] == 2 ? "loader.program" : "loader.readback", result, NULL);
    return result == OKL_OK ? 0 : -1;
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

static int diagnostic_request(update_context *c, okl_command command,
                              const uint8_t *args, size_t size, okl_reply *reply) {
    okl_request request;
    return okl_request_build(&request, command, args, size) == OKL_OK &&
        okl_nxp_execute(c->driver, &request, reply, bounded(c, 150000)) == OKL_OK ? 0 : -1;
}
static int diagnostic_page(update_context *c, uint8_t page, uint32_t *words) {
    okl_reply reply; uint8_t bytes[64];
    if (diagnostic_request(c, OKL_GET_DIAGNOSTIC_PAGE, &page, 1, &reply) ||
        okl_reply_decode_diagnostic_page(bytes, page, &reply) != OKL_OK) return -1;
    const uint8_t *p = bytes;
    for (unsigned i = 0; i < 16; ++i)
        words[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
            (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    return 0;
}
static void diagnostic(update_context *c, const okl_loader_audit *audit) {
    app_controller_worker_outcome *out = &c->outcome;
    out->diagnostic_trial_observed = true;
    const uint8_t expected_version[4] = {0, 1, 0, 0};
    const char *error = "Diagnostic profile was not verified; no bench command sent";
    uint32_t initial[16], final_header[16]; okl_reply reply; okl_diagnostic_profile profile;
    if (memcmp(c->job->image.version.component, expected_version, 4) ||
        audit->observation.controller.uptime_ms >= 26000 ||
        begin(c, clock_us(c) + UINT64_C(45000000))) goto failed;
    if (okl_nxp_claim(c->driver, (const uint8_t *)"Open Keylight", 13, bounded(c, 600000)) != OKL_OK ||
        diagnostic_request(c, OKL_GET_DIAGNOSTIC_PROFILE, NULL, 0, &reply) ||
        okl_reply_decode_diagnostic_profile(&profile, &reply) != OKL_OK || profile.requested ||
        diagnostic_page(c, 0, initial) || !app_diagnostic_initial(initial)) goto failed;
    out->profile_verified = true;
    out->command_attempted = true;
    error = "OFF1 acknowledgement was not verified; command was not retried";
    if (diagnostic_request(c, OKL_RUN_DIAGNOSTIC_OFF, (const uint8_t *)"OFF1", 4, &reply) ||
        okl_reply_check_diagnostic_off(&reply) != OKL_OK) goto failed;
    out->command_acknowledged = true;
    uint64_t settle = clock_us(c) + UINT64_C(550000);
    while (clock_us(c) < settle) wait_until(c, settle);
    error = "OFF1 register snapshots were not verified";
    for (uint8_t page = 0; page < APP_DIAGNOSTIC_PAGES; ++page)
        if (diagnostic_page(c, page, out->diagnostic_words + page * 16)) goto failed;
    if (diagnostic_page(c, 0, final_header) || memcmp(final_header, out->diagnostic_words, sizeof(final_header)) ||
        !app_diagnostic_registers(out->diagnostic_words, initial[15])) goto failed;
    out->registers_verified = true;
    /* A known-complete End reset and the exact compiled OFF1 recovery profile
     * preceded this wait. Do not send anything until its immutable 30s expiry
     * plus the proven 3s guard has passed, even if our uptime estimate was low.
     * No loader writes/reads have occurred since that reset. Info80 below is
     * therefore not a claim that arbitrary resident RAM is fresh. */
    uint64_t quiet_end = clock_us(c) + UINT64_C(33000000);
    while (clock_us(c) < quiet_end) wait_until(c, quiet_end);
    uint8_t request[90], response[90], args[80] = {0};
    okl_loader_delivery delivery;
    error = "Diagnostic returned no qualified resident loader; explicit recovery required";
    if (okl_report_encode(request, 0, 0x10, 0x80, args, 80) != OKL_OK ||
        app_nxp_loader_exchange(c->driver, c->job->id, request, response, &delivery, bounded(c, 2000000)) != OKL_OK ||
        delivery != OKL_LOADER_SENT_COMPLETE || !okl_loader_information_valid(response) ||
        app_nxp_loader_preserve_resident(c->driver, c->job->id, bounded(c, 150000)) != OKL_OK) goto failed;
    out->resident_proof_job_id = c->job->id;
    error = NULL;
failed:
    if (error) snprintf(out->diagnostic_error, sizeof(out->diagnostic_error), "%s", error);
    if (c->leased) end(c);
}
okl_loader_result app_controller_worker_run(okl_nxp *driver, const app_controller_job *job,
                                          okl_loader_audit *audit, app_controller_worker_outcome *outcome) {
    if (outcome) memset(outcome, 0, sizeof(*outcome));
    if (!driver || !job || !audit || !outcome) return OKL_LOADER_INVALID;
    update_context context = {.driver = driver, .job = job};
    okl_loader_ops ops = {.user = &context, .sha256 = digest, .now_us = clock_us,
        .begin = begin, .end = end, .persist = persist, .enter_loader = enter,
        .exchange = exchange, .send_only = send_only, .wait_until = wait_until,
        .cancelled = cancelled, .reset_boundary = reset_boundary, .observe_readonly = observe, .progress = progress};
    okl_loader_result result = okl_loader_run(&job->image, job->source, &ops,
        clock_us(&context) + UINT64_C(120000000), audit);
    if (result == OKL_LOADER_OK && job->image.role == OKL_ROLE_SPI_DIAGNOSTIC)
        diagnostic(&context, audit);
    *outcome = context.outcome;
    if (outcome->entry == APP_CONTROLLER_READ_ONLY_UNSUPPORTED && outcome->synchronized &&
        !audit->persistence_failed && result == OKL_LOADER_IO) audit->result = result = OKL_LOADER_INVALID;
    return result;
}

void app_controller_worker_recover(okl_nxp *driver, const app_controller_job *job,
                                   const uint8_t identity[6], app_controller_worker_outcome *outcome) {
    if (!outcome) return;
    memset(outcome, 0, sizeof(*outcome));
    if (!driver || !job || !job->recovery_only || !identity || driver->transport.now_us ||
        driver->transport.transfer || driver->transport.user) {
        snprintf(outcome->stage, sizeof(outcome->stage), "recovery.pristine");
        snprintf(outcome->diagnostic_error, sizeof(outcome->diagnostic_error), "Recovery requires pristine SPI after physical power cycle");
        outcome->transport_result = OKL_NEEDS_RECOVERY; return;
    }
    update_context c = {.driver=driver, .job=job};
    okl_result result = app_nxp_transport_init(driver, identity) == ESP_OK ? OKL_OK : OKL_IO;
    trace(&c, "recovery.initialize", result, NULL);
    if (result != OKL_OK) goto finish;
    c.deadline = clock_us(&c) + UINT64_C(40000000);
    if (begin(&c, c.deadline)) { result=OKL_NEEDS_RECOVERY; trace(&c,"recovery.lease",result,NULL); goto finish; }
    uint64_t quiet = clock_us(&c) + UINT64_C(33000000);
    while (clock_us(&c) < quiet) wait_until(&c, quiet);
    uint8_t request[90], response[90], args[80]={0}; okl_loader_delivery delivery;
    (void)okl_report_encode(request,0,0x10,0x80,args,80);
    result=app_nxp_loader_exchange(driver,job->id,request,response,&delivery,bounded(&c,2000000));
    trace(&c,"recovery.information",result,NULL);
    if (result != OKL_OK) goto finish;
    if (delivery != OKL_LOADER_SENT_COMPLETE) {
        result=OKL_PROTOCOL; trace(&c,"recovery.information_delivery",result,NULL); goto finish;
    }
    if (okl_loader_information_valid(response)) {
        result=app_nxp_loader_preserve_resident(driver,job->id,bounded(&c,150000));
        trace(&c,"recovery.resident",result,NULL);
        if (result==OKL_OK) c.outcome.resident_proof_job_id=job->id;
        goto finish;
    }
    okl_report report;
    if (!job->allow_legacy_reconcile || okl_report_decode(&report,response,90)!=OKL_OK ||
        report.transaction || report.command_class!=0x10 || report.opcode!=0x80 || report.size ||
        (report.status!=5 && report.status!=8)) {
        result=OKL_PROTOCOL; trace(&c,"recovery.classify",result,NULL); goto finish;
    }
    okl_reply reply; okl_firmware_version version; uint32_t part;
    const uint8_t legacy[4]={1,3,0,0};
    result=query(&c,OKL_GET_FIRMWARE,&reply);
    if (result!=OKL_OK) goto finish;
    if (okl_reply_decode_firmware(&version,&reply)!=OKL_OK || memcmp(version.component,legacy,4)) {
        result=OKL_VERIFY; trace(&c,"recovery.version",result,&reply); goto finish;
    }
    c.outcome.entry=APP_CONTROLLER_MUTATION_ATTEMPTED;
    result=okl_nxp_claim(driver,(const uint8_t *)"Open Keylight",13,bounded(&c,600000));
    trace(&c,"recovery.claim",result,NULL); if(result!=OKL_OK)goto finish;
    result=query(&c,OKL_GET_CONTROLLER_STATUS,&reply);
    if(!unsupported_reply(result,&reply,0xfc)) {
        if (result==OKL_OK) { result=OKL_VERIFY; trace(&c,"recovery.status",result,&reply); }
        goto finish;
    }
    result=query(&c,OKL_GET_PART_ID,&reply);
    if(result!=OKL_OK)goto finish;
    if(okl_reply_decode_part_id(&part,&reply)!=OKL_OK || part!=OKL_LOADER_PART_ID) {
        result=OKL_VERIFY; trace(&c,"recovery.part",result,&reply); goto finish;
    }
    okl_light_state state;
    result=okl_nxp_read_state(driver,&state,bounded(&c,800000));
    trace(&c,"recovery.state",result,NULL);if(result!=OKL_OK)goto finish;
    uint8_t effect=state.effect;kl_state off=kl_state_default();off.power=false;
    result=kl_output_prepare(&off,false,NULL,&effect,output,&c);
    trace(&c,"recovery.off",result,NULL);if(result!=OKL_OK)goto finish;
    result=okl_nxp_read_state(driver,&state,bounded(&c,800000));
    if(result==OKL_OK && (state.effect || state.white_brightness))result=OKL_VERIFY;
    trace(&c,"recovery.off_readback",result,NULL);if(result!=OKL_OK)goto finish;
    result=okl_nxp_release(driver,bounded(&c,600000));
    trace(&c,"recovery.release",result,NULL);
    if(result==OKL_OK)c.outcome.legacy_reconciled=true;
finish:
    if(c.leased)end(&c);
    if(!c.outcome.resident_proof_job_id && !c.outcome.legacy_reconciled)
        snprintf(c.outcome.diagnostic_error,sizeof(c.outcome.diagnostic_error),"Recovery failed at %.31s (%d); no retry",c.outcome.stage,c.outcome.transport_result);
    *outcome=c.outcome;
}
