#include "okl_loader.h"
#include <string.h>

enum { BANK_BASE = 0x2000, BANK_END = 0x8fff, VECTOR_BYTES = 192 };
static const uint8_t package_magic[8] = {'O', 'K', 'L', 'C', 'N', 'X', 'P', 0};
static const uint8_t loader_information[80] = {3, 24, 1, 2, 0, 0, 0, 2, 93};

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
static void put_be32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}
static uint64_t after(uint64_t now, uint64_t duration) {
    return now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
}
int okl_loader_information_valid(const uint8_t bytes[OKL_REPORT_BYTES]) {
    okl_report report;
    return bytes && okl_report_decode(&report, bytes, OKL_REPORT_BYTES) == OKL_OK &&
        report.status == 2 && !report.transaction && report.command_class == 0x10 &&
        report.opcode == 0x80 && report.size == 80 && !memcmp(report.arguments, loader_information, 80);
}

okl_loader_result okl_loader_prepare(okl_loader_image *out, const uint8_t *package,
                                    size_t size, const okl_loader_ops *ops) {
    okl_loader_image image;
    const uint8_t *bank;
    unsigned i;
    if (!out || !package || !ops || !ops->sha256 || size != OKL_LOADER_PACKAGE_BYTES)
        return OKL_LOADER_INVALID;
    if (memcmp(package, package_magic, sizeof(package_magic)) ||
        package[8] || package[9] != 1 || package[10] || package[11] != OKL_LOADER_HEADER_BYTES ||
        be32(package + 12) != OKL_LOADER_BANK_BYTES || be32(package + 16) != OKL_LOADER_PART_ID ||
        package[20] != 1 || package[21] ||
        (package[22] != OKL_ROLE_LIGHTING && package[22] != OKL_ROLE_SPI_DIAGNOSTIC) || package[23] ||
        be32(package + 60) || !be32(package + 24)) return OKL_LOADER_INVALID;
    bank = package + OKL_LOADER_HEADER_BYTES;
    /* The public application layout reserves 192 vector bytes and uses the
     * conservative 4KiB main SRAM region, leaving all vectors populated. */
    if (le32(bank) != UINT32_C(0x10001000)) return OKL_LOADER_INVALID;
    for (i = 4; i < VECTOR_BYTES; i += 4) {
        uint32_t vector = le32(bank + i);
        if (!(vector & 1u) || vector < BANK_BASE + VECTOR_BYTES + 1u || vector > BANK_END)
            return OKL_LOADER_INVALID;
    }
    memset(&image, 0, sizeof(image));
    if (ops->sha256(ops->user, bank, OKL_LOADER_BANK_BYTES, image.bank_sha256)) return OKL_LOADER_IO;
    if (memcmp(image.bank_sha256, package + 28, 32)) return OKL_LOADER_VERIFY;
    image.package = package; image.size = size; image.role = package[22];
    memcpy(image.version.component, package + 24, 4);
    *out = image;
    return OKL_LOADER_OK;
}

static int valid_ops(const okl_loader_ops *o) {
    return o && o->sha256 && o->now_us && o->begin && o->end && o->persist &&
        o->enter_loader && o->exchange && o->send_only && o->wait_until && o->cancelled &&
        o->reset_boundary && o->observe_readonly;
}
static void progress(const okl_loader_ops *o, const okl_loader_audit *a) {
    if (o->progress) o->progress(o->user, a);
}
static okl_loader_result save(const okl_loader_ops *o, okl_loader_audit *a, okl_loader_phase phase) {
    a->phase = phase;
    progress(o, a);
    if (o->persist(o->user, a)) { a->persistence_failed = 1; return OKL_LOADER_PERSIST; }
    return OKL_LOADER_OK;
}
static okl_loader_result budget(const okl_loader_ops *o, uint64_t deadline) {
    if (o->now_us(o->user) >= deadline) return OKL_LOADER_TIMEOUT;
    return o->cancelled(o->user) ? OKL_LOADER_CANCELLED : OKL_LOADER_OK;
}
static okl_loader_result exchange(const okl_loader_ops *o, uint8_t opcode,
                                  const uint8_t *args, size_t size, okl_report *out,
                                  uint64_t deadline) {
    uint8_t request[90], response[90];
    okl_loader_delivery delivery = OKL_LOADER_MAYBE_SENT;
    okl_loader_result result = budget(o, deadline);
    if (result != OKL_LOADER_OK) return result;
    if (okl_report_encode(request, 0, 0x10, opcode, args, size) != OKL_OK) return OKL_LOADER_INVALID;
    memset(response, 0, sizeof(response));
    if (o->exchange(o->user, request, response, &delivery, deadline)) return OKL_LOADER_IO;
    if (delivery != OKL_LOADER_SENT_COMPLETE) return OKL_LOADER_IO;
    if (o->now_us(o->user) >= deadline) return OKL_LOADER_TIMEOUT;
    if (okl_report_decode(out, response, sizeof(response)) != OKL_OK || out->status != 2 ||
        out->transaction || out->command_class != 0x10 || out->opcode != opcode)
        return OKL_LOADER_PROTOCOL;
    return OKL_LOADER_OK;
}
static void quiet(const okl_loader_ops *o, okl_loader_audit *a) {
    uint64_t deadline;
    a->quiet_started_us = o->now_us(o->user);
    deadline = after(a->quiet_started_us, OKL_LOADER_QUIET_US);
    (void)save(o, a, OKL_LOADER_QUIET);
    while (o->now_us(o->user) < deadline) o->wait_until(o->user, deadline);
    a->quiet_finished_us = o->now_us(o->user);
    a->quiet_completed = (uint8_t)(a->quiet_finished_us - a->quiet_started_us >= OKL_LOADER_QUIET_US);
}
static void abort_precommit(const okl_loader_ops *o, okl_loader_audit *a) {
    uint8_t request[90];
    int sent;
    a->abort_attempted = 1;
    a->abort_delivery = OKL_LOADER_MAYBE_SENT;
    (void)okl_report_encode(request, 0, 0x10, 4, NULL, 0);
    sent = o->send_only(o->user, request, &a->abort_delivery, after(o->now_us(o->user), UINT64_C(2000000)));
    quiet(o, a);
    if (!sent && a->quiet_completed && a->abort_delivery == OKL_LOADER_SENT_COMPLETE &&
        !o->reset_boundary(o->user, 4, a->abort_delivery, after(o->now_us(o->user), UINT64_C(2000000))))
        a->reset_boundary_established = 1;
}
static int matching_application(const okl_loader_image *image, const okl_loader_observation *s) {
    return s->kind == OKL_LOADER_OBSERVATION_APPLICATION && s->dark_state_verified == 1 &&
        !memcmp(image->version.component, s->version.component, 4) &&
        s->controller.abi_major == 1 && !s->controller.abi_minor &&
        s->controller.role == image->role &&
        s->controller.capabilities == (image->role == OKL_ROLE_LIGHTING ?
            (OKL_CAP_RECOVERY_READY | OKL_CAP_LIGHTING_READY) : OKL_CAP_RECOVERY_READY) &&
        s->controller.part_id == OKL_LOADER_PART_ID && !s->controller.boot_requested &&
        !s->controller.trial_confirmed;
}

okl_loader_result okl_loader_run(const okl_loader_image *image, okl_loader_source source,
                                const okl_loader_ops *o, uint64_t deadline,
                                okl_loader_audit *a) {
    okl_loader_image validated;
    okl_loader_result result;
    okl_report response;
    uint8_t args[80] = {0}, digest[32], request[90];
    const uint8_t *bank;
    unsigned block;
    int send_result;
    uint64_t observe_deadline;
    if (!a) return OKL_LOADER_INVALID;
    memset(a, 0, sizeof(*a)); a->source = source; a->result = OKL_LOADER_INVALID;
    if (!image || !valid_ops(o) || (unsigned)source > OKL_LOADER_FROM_FRESH_RESIDENT) return a->result;
    result = okl_loader_prepare(&validated, image->package, image->size, o);
    if (result != OKL_LOADER_OK) { a->result = result; return result; }
    if (memcmp(image->bank_sha256, validated.bank_sha256, 32) ||
        memcmp(image->version.component, validated.version.component, 4) || image->role != validated.role) return a->result;
    memcpy(a->bank_sha256, validated.bank_sha256, 32);
    bank = validated.package + OKL_LOADER_HEADER_BYTES;
    result = budget(o, deadline);
    if (result != OKL_LOADER_OK) { a->result = result; return result; }
    if (o->begin(o->user, deadline)) { a->result = OKL_LOADER_BUSY; return a->result; }
    result = save(o, a, OKL_LOADER_VALIDATED);
    if (result != OKL_LOADER_OK) goto failed;
    result = budget(o, deadline);
    if (result != OKL_LOADER_OK) goto failed;
    result = save(o, a, OKL_LOADER_ENTERING);
    if (result != OKL_LOADER_OK) goto failed;
    result = budget(o, deadline);
    if (result != OKL_LOADER_OK) goto failed;
    if (o->enter_loader(o->user, source, deadline)) { result = OKL_LOADER_IO; goto failed; }
    result = exchange(o, 0x80, args, 80, &response, deadline);
    if (result != OKL_LOADER_OK) goto failed;
    if (response.size != 80 || memcmp(response.arguments, loader_information, 80)) {
        result = OKL_LOADER_PROTOCOL; goto failed;
    }
    a->loader_verified = 1;
    result = save(o, a, OKL_LOADER_READY);
    if (result != OKL_LOADER_OK) goto failed;
    a->erase_attempted = 1;
    result = save(o, a, OKL_LOADER_ERASING);
    if (result != OKL_LOADER_OK) goto failed;
    put_be32(args, BANK_BASE); put_be32(args + 4, BANK_END);
    result = exchange(o, 1, args, 8, &response, deadline);
    if (result != OKL_LOADER_OK) goto failed;
    if (response.size != 8 || memcmp(response.arguments, args, 8)) { result = OKL_LOADER_PROTOCOL; goto failed; }
    result = save(o, a, OKL_LOADER_PROGRAMMING);
    if (result != OKL_LOADER_OK) goto failed;
    for (block = 0; block < OKL_LOADER_BLOCKS; ++block) {
        args[0] = OKL_LOADER_BLOCK_BYTES; put_be32(args + 1, BANK_BASE + block * OKL_LOADER_BLOCK_BYTES);
        memcpy(args + 5, bank + block * OKL_LOADER_BLOCK_BYTES, OKL_LOADER_BLOCK_BYTES);
        result = exchange(o, 2, args, 69, &response, deadline);
        if (result != OKL_LOADER_OK) goto failed;
        if (response.size != 69 || memcmp(response.arguments, args, 69)) { result = OKL_LOADER_PROTOCOL; goto failed; }
        ++a->program_blocks_acked; progress(o, a);
    }
    result = save(o, a, OKL_LOADER_VERIFYING);
    if (result != OKL_LOADER_OK) goto failed;
    memset(args + 5, 0, 64);
    for (block = 0; block < OKL_LOADER_BLOCKS; ++block) {
        put_be32(args + 1, BANK_BASE + block * OKL_LOADER_BLOCK_BYTES);
        result = exchange(o, 0x83, args, 69, &response, deadline);
        if (result != OKL_LOADER_OK) goto failed;
        if (response.size != 69 || memcmp(response.arguments, args, 5) ||
            memcmp(response.arguments + 5, bank + block * OKL_LOADER_BLOCK_BYTES, 64)) {
            result = OKL_LOADER_VERIFY; goto failed;
        }
        ++a->readback_blocks_verified; progress(o, a);
    }
    if (o->sha256(o->user, bank, OKL_LOADER_BANK_BYTES, digest) || memcmp(digest, validated.bank_sha256, 32)) {
        result = OKL_LOADER_VERIFY; goto failed;
    }
    a->complete_bank_verified = 1;
    result = budget(o, deadline);
    if (result != OKL_LOADER_OK) goto failed;
    a->commit_attempted = 1;
    result = save(o, a, OKL_LOADER_COMMITTING);
    if (result != OKL_LOADER_OK) { a->commit_attempted = 0; goto failed; }
    result = budget(o, deadline);
    if (result != OKL_LOADER_OK) { a->commit_attempted = 0; goto failed; }
    a->commit_delivery = OKL_LOADER_MAYBE_SENT;
    (void)okl_report_encode(request, 0, 0x10, 5, NULL, 0);
    send_result = o->send_only(o->user, request, &a->commit_delivery, deadline);
    quiet(o, a); /* No branch, cancellation or send error can skip this. */
    a->cancelled_after_commit = (uint8_t)(o->cancelled(o->user) != 0);
    result = OKL_LOADER_UNRESOLVED;
    if (send_result || !a->quiet_completed || a->commit_delivery != OKL_LOADER_SENT_COMPLETE) goto finished;
    observe_deadline = after(o->now_us(o->user), UINT64_C(10000000));
    if (o->reset_boundary(o->user, 5, a->commit_delivery, observe_deadline)) goto finished;
    a->reset_boundary_established = 1;
    (void)save(o, a, OKL_LOADER_OBSERVING);
    if (o->observe_readonly(o->user, &a->observation, observe_deadline) || o->now_us(o->user) >= observe_deadline)
        goto finished;
    if (o->cancelled(o->user)) a->cancelled_after_commit = 1;
    if (matching_application(&validated, &a->observation)) {
        result = a->cancelled_after_commit ? OKL_LOADER_CANCELLED : OKL_LOADER_OK;
        a->phase = OKL_LOADER_APPLICATION_SEEN;
    }
    goto finished;
failed:
    if (a->loader_verified) abort_precommit(o, a);
    a->phase = OKL_LOADER_PRECOMMIT_FAILED;
finished:
    if (a->commit_attempted && a->phase != OKL_LOADER_APPLICATION_SEEN) a->phase = OKL_LOADER_COMMIT_UNRESOLVED;
    if (a->persistence_failed && result == OKL_LOADER_OK) result = OKL_LOADER_PERSIST;
    a->result = result;
    if (save(o, a, a->phase) != OKL_LOADER_OK && result == OKL_LOADER_OK) a->result = result = OKL_LOADER_PERSIST;
    o->end(o->user);
    return result;
}
