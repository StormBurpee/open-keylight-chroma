#include "okl_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct {
    uint8_t package[OKL_LOADER_PACKAGE_BYTES], original[OKL_LOADER_BANK_BYTES];
    uint8_t flash[OKL_LOADER_BANK_BYTES], pending[4096];
    int pending_sector, leased, begins, ends, enters, boundaries, observations;
    unsigned exchanges, programmed, verified, flushes, commits, aborts, quiet_calls;
    unsigned fail_exchange, corrupt_exchange, corruption;
    int begin_fail, entry_fail, send_fail, send_not_complete, boundary_fail, observe_fail;
    int cancel, cancel_on_commit_persist, cancel_in_quiet, change_after_readback;
    int invalid_observation, fail_persist_phase, sha_fail, early_wait, cancel_in_observe;
    uint64_t now, last_reset_send, latency, observe_latency;
    okl_loader_delivery reset_delivery;
    okl_loader_phase persisted_phase;
} model;
static model m;

static void put_be32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16); p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put_le32(uint8_t *p, uint32_t n) {
    p[3] = (uint8_t)(n >> 24); p[2] = (uint8_t)(n >> 16); p[1] = (uint8_t)(n >> 8); p[0] = (uint8_t)n;
}
static void checksum(uint8_t *r) {
    unsigned i; r[88] = 0; for (i = 2; i < 88; ++i) r[88] ^= r[i];
}
static void init(void) {
    unsigned i;
    memset(&m, 0, sizeof(m)); m.pending_sector = -1; m.fail_persist_phase = -1; m.latency = 10;
    memcpy(m.package, "OKLCNXP", 8); m.package[9] = 1; m.package[11] = 64;
    put_be32(m.package + 12, OKL_LOADER_BANK_BYTES); put_be32(m.package + 16, OKL_LOADER_PART_ID);
    m.package[20] = 1; m.package[22] = 2; m.package[25] = 1;
    for (i = 0; i < 32; ++i) m.package[28 + i] = (uint8_t)i;
    memset(m.original, 0x5a, sizeof(m.original)); put_le32(m.original, 0x10001000);
    for (i = 4; i < 192; i += 4) put_le32(m.original + i, 0x20c1);
    memcpy(m.package + 64, m.original, sizeof(m.original));
    memset(m.flash, 0xa5, sizeof(m.flash));
}
/* Equality oracle for the injected SHA primitive, not a substitute SHA256.
 * It distinguishes every tested byte mutation; production supplies SHA256. */
static int digest(void *user, const uint8_t *data, size_t size, uint8_t out[32]) {
    model *s = user; unsigned i;
    CHECK(size == OKL_LOADER_BANK_BYTES);
    if (s->sha_fail) return -1;
    for (i = 0; i < 32; ++i) out[i] = (uint8_t)i;
    if (memcmp(data, s->original, size)) out[0] ^= 0xff;
    return 0;
}
static uint64_t now(void *user) { return ((model *)user)->now; }
static int begin(void *user, uint64_t deadline) {
    model *s = user; CHECK(!s->leased && s->now < deadline); ++s->begins;
    if (s->begin_fail) return -1;
    s->leased = 1; return 0;
}
static void end(void *user) {
    model *s = user; CHECK(s->leased);
    if (s->commits || s->aborts) CHECK(s->now - s->last_reset_send >= OKL_LOADER_QUIET_US);
    s->leased = 0; ++s->ends;
}
static int persist(void *user, const okl_loader_audit *audit) {
    model *s = user; CHECK(s->leased); s->persisted_phase = audit->phase;
    if (audit->phase == OKL_LOADER_COMMITTING) {
        CHECK(audit->complete_bank_verified && audit->program_blocks_acked == 448 && audit->readback_blocks_verified == 448);
        if (s->cancel_on_commit_persist) s->cancel = 1;
    }
    return (int)audit->phase == s->fail_persist_phase ? -1 : 0;
}
static int enter(void *user, okl_loader_source source, uint64_t deadline) {
    model *s = user; CHECK(s->leased && s->now < deadline && s->persisted_phase == OKL_LOADER_ENTERING);
    CHECK(source <= OKL_LOADER_FROM_FRESH_RESIDENT); ++s->enters; return s->entry_fail;
}
static void flush(model *s) {
    if (s->pending_sector >= 0) {
        memcpy(s->flash + s->pending_sector, s->pending, 4096);
        s->pending_sector = -1; ++s->flushes;
    }
}
static int exchange(void *user, const uint8_t request[90], uint8_t response[90],
                    okl_loader_delivery *delivery, uint64_t deadline) {
    model *s = user; okl_report q; uint8_t args[80]; unsigned size; uint32_t address;
    CHECK(s->leased && s->enters == 1 && s->now < deadline && !s->commits && !s->aborts);
    CHECK(okl_report_decode(&q, request, 90) == OKL_OK && !q.transaction && !q.status && q.command_class == 0x10);
    ++s->exchanges; s->now += s->latency; *delivery = OKL_LOADER_SENT_COMPLETE;
    if (s->exchanges == s->fail_exchange) { *delivery = OKL_LOADER_MAYBE_SENT; return -1; }
    size = q.size; memcpy(args, q.arguments, size);
    if (q.opcode == 0x80) {
        CHECK(s->exchanges == 1 && size == 80 && !memcmp(args, (uint8_t[80]){0}, 80));
        memcpy(args, (uint8_t[80]){3, 24, 1, 2, 0, 0, 0, 2, 93}, 80);
    } else if (q.opcode == 1) {
        CHECK(s->persisted_phase == OKL_LOADER_ERASING && size == 8);
        CHECK(be32(args) == 0x2000 && be32(args + 4) == 0x8fff);
        memset(s->flash, 0xff, sizeof(s->flash));
    } else if (q.opcode == 2) {
        CHECK(s->persisted_phase == OKL_LOADER_PROGRAMMING && size == 69 && args[0] == 64);
        address = be32(args + 1); CHECK(address == 0x2000 + 64 * s->programmed && address <= 0x8fc0);
        CHECK(!memcmp(args + 5, s->package + 64 + address - 0x2000, 64));
        if (s->pending_sector != (int)((address - 0x2000) & ~4095u)) {
            flush(s); s->pending_sector = (int)((address - 0x2000) & ~4095u);
            memcpy(s->pending, s->flash + s->pending_sector, 4096);
        }
        memcpy(s->pending + ((address - 0x2000) & 4095u), args + 5, 64); ++s->programmed;
    } else {
        CHECK(q.opcode == 0x83 && size == 69 && args[0] == 64 && s->programmed == 448);
        CHECK(s->persisted_phase == OKL_LOADER_VERIFYING && !memcmp(args + 5, (uint8_t[64]){0}, 64));
        address = be32(args + 1); CHECK(address == 0x2000 + 64 * s->verified && address <= 0x8fc0);
        flush(s); memcpy(args + 5, s->flash + address - 0x2000, 64); ++s->verified;
        if (s->change_after_readback && s->verified == 448) s->package[64 + 256] ^= 1;
    }
    CHECK(okl_report_encode(response, 0, 0x10, q.opcode, args, size) == OKL_OK); response[0] = 2;
    if (s->exchanges == s->corrupt_exchange) {
        switch (s->corruption) {
        case 0: response[0] = 3; break;
        case 1: response[1] = 1; break;
        case 2: response[6] = 0; break;
        case 3: response[7] ^= 1; break;
        case 4: --response[5]; break;
        case 5: response[8 + size - 1] ^= 1; break;
        case 6: response[2] = 1; break;
        case 7: response[89] = 1; break;
        case 8: response[88] ^= 1; return 0;
        case 9: response[8] ^= 1; break;
        default: response[12] ^= 64; break;
        }
        checksum(response);
    }
    return 0;
}
static int send_only(void *user, const uint8_t request[90], okl_loader_delivery *delivery, uint64_t deadline) {
    model *s = user; okl_report q;
    CHECK(s->leased && s->now < deadline && okl_report_decode(&q, request, 90) == OKL_OK);
    CHECK(q.command_class == 0x10 && !q.size && !q.transaction);
    if (q.opcode == 5) {
        CHECK(!s->commits && !s->aborts && s->programmed == 448 && s->verified == 448);
        CHECK(s->flushes == 7 && s->pending_sector == -1 && !memcmp(s->flash, s->original, sizeof(s->flash)));
        CHECK(s->persisted_phase == OKL_LOADER_COMMITTING); ++s->commits;
    } else { CHECK(q.opcode == 4 && !s->commits && !s->aborts); ++s->aborts; }
    s->last_reset_send = s->now;
    *delivery = s->send_fail || s->send_not_complete ? OKL_LOADER_MAYBE_SENT : OKL_LOADER_SENT_COMPLETE;
    s->reset_delivery = *delivery;
    return s->send_fail;
}
static void wait_until(void *user, uint64_t deadline) {
    model *s = user; CHECK(s->leased && s->now < deadline && (s->commits || s->aborts));
    ++s->quiet_calls;
    if (s->cancel_in_quiet) s->cancel = 1;
    if (s->early_wait && deadline - s->now > 100000) s->now += 100000;
    else s->now = deadline;
}
static int cancelled(void *user) { return ((model *)user)->cancel; }
static int boundary(void *user, uint8_t op, okl_loader_delivery delivery, uint64_t deadline) {
    model *s = user; CHECK(s->leased && s->now < deadline && s->now - s->last_reset_send >= OKL_LOADER_QUIET_US);
    CHECK(delivery == OKL_LOADER_SENT_COMPLETE && delivery == s->reset_delivery);
    CHECK((op == 5 && s->commits == 1) || (op == 4 && s->aborts == 1));
    ++s->boundaries; return s->boundary_fail;
}
static int observe(void *user, okl_loader_observation *observation, uint64_t deadline) {
    model *s = user; CHECK(s->leased && s->commits == 1 && s->boundaries == 1 && s->now < deadline);
    ++s->observations; s->now += s->observe_latency;
    if (s->cancel_in_observe) s->cancel = 1;
    observation->kind = OKL_LOADER_OBSERVATION_APPLICATION;
    observation->dark_state_verified = 1; observation->version.component[1] = 1;
    observation->controller.abi_major = 1; observation->controller.role = s->package[22];
    observation->controller.capabilities = s->package[22] == OKL_ROLE_LIGHTING ? 3 : 1;
    observation->controller.part_id = OKL_LOADER_PART_ID;
    switch (s->invalid_observation) {
    case 1: observation->kind = OKL_LOADER_OBSERVATION_RESIDENT; break;
    case 2: observation->kind = OKL_LOADER_OBSERVATION_UNKNOWN; break;
    case 3: observation->controller.role = s->package[22] == OKL_ROLE_LIGHTING ? OKL_ROLE_SPI_DIAGNOSTIC : OKL_ROLE_LIGHTING; break;
    case 4: observation->controller.capabilities = s->package[22] == OKL_ROLE_LIGHTING ? 1 : 3; break;
    case 5: observation->controller.part_id ^= 1; break;
    case 6: observation->controller.abi_major = 2; break;
    case 7: observation->controller.abi_minor = 1; break;
    case 8: observation->controller.boot_requested = 1; break;
    case 9: observation->controller.trial_confirmed = 1; break;
    case 10: observation->version.component[2] = 1; break;
    case 11: observation->dark_state_verified = 0; break;
    default: break;
    }
    return s->observe_fail;
}
static okl_loader_ops ops(void) {
    okl_loader_ops value = {0};
    value.user = &m; value.sha256 = digest; value.now_us = now; value.begin = begin; value.end = end;
    value.persist = persist; value.enter_loader = enter; value.exchange = exchange; value.send_only = send_only;
    value.wait_until = wait_until; value.cancelled = cancelled; value.reset_boundary = boundary; value.observe_readonly = observe;
    return value;
}
static okl_loader_result run(okl_loader_audit *audit, okl_loader_source source, uint64_t deadline) {
    okl_loader_ops o = ops(); okl_loader_image image;
    CHECK(okl_loader_prepare(&image, m.package, sizeof(m.package), &o) == OKL_LOADER_OK);
    return okl_loader_run(&image, source, &o, deadline, audit);
}
static void test_admission(void) {
    okl_loader_ops o; okl_loader_image image, before; unsigned i; uint8_t saved;
    init(); o = ops(); memset(&image, 0xa5, sizeof(image)); before = image;
    for (i = 0; i < OKL_LOADER_PACKAGE_BYTES; ++i) {
        CHECK(okl_loader_prepare(&image, m.package, i, &o) == OKL_LOADER_INVALID);
        CHECK(!memcmp(&image, &before, sizeof(image)));
    }
    for (i = 0; i < 64; ++i) if (i < 24 || i >= 28) {
        saved = m.package[i]; m.package[i] ^= 0x80;
        CHECK(okl_loader_prepare(&image, m.package, sizeof(m.package), &o) != OKL_LOADER_OK);
        CHECK(!memcmp(&image, &before, sizeof(image))); m.package[i] = saved;
    }
    for (i = 0; i < 192; i += 4) {
        saved = m.package[64 + i]; m.package[64 + i] = 0;
        if (i == 0) m.package[64 + i + 1] = 0;
        CHECK(okl_loader_prepare(&image, m.package, sizeof(m.package), &o) == OKL_LOADER_INVALID);
        m.package[64 + i] = saved; if (i == 0) m.package[65] = 0x10;
    }
    CHECK(okl_loader_prepare(&image, m.package, sizeof(m.package), &o) == OKL_LOADER_OK);
    for (i = 192; i < OKL_LOADER_BANK_BYTES; i += 31) {
        m.package[64 + i] ^= 1;
        CHECK(okl_loader_prepare(&image, m.package, sizeof(m.package), &o) == OKL_LOADER_VERIFY);
        m.package[64 + i] ^= 1;
    }
    CHECK(!m.begins && !m.exchanges && !m.enters);
}
static void test_success(void) {
    unsigned source; okl_loader_audit a;
    for (source = 0; source <= OKL_LOADER_FROM_FRESH_RESIDENT; ++source) {
        init(); m.early_wait = 1;
        CHECK(run(&a, (okl_loader_source)source, 1000000) == OKL_LOADER_OK);
        CHECK(a.phase == OKL_LOADER_APPLICATION_SEEN && a.complete_bank_verified && a.commit_attempted && !a.abort_attempted);
        CHECK(a.program_blocks_acked == 448 && a.readback_blocks_verified == 448 && a.quiet_completed);
        CHECK(a.quiet_finished_us - a.quiet_started_us == OKL_LOADER_QUIET_US && m.quiet_calls == 30);
        CHECK(m.ends == 1 && !m.leased && m.commits == 1 && !m.aborts && m.observations == 1);
    }
}
static void test_explicit_diagnostic_role(void) {
    okl_loader_ops o; okl_loader_image image; okl_loader_audit audit; unsigned i;
    init(); o = ops();
    for (i = 0; i <= 255; ++i) {
        m.package[22] = (uint8_t)i;
        CHECK(okl_loader_prepare(&image, m.package, sizeof(m.package), &o) ==
            (i == 1 || i == 2 ? OKL_LOADER_OK : OKL_LOADER_INVALID));
        if (i == 1 || i == 2) CHECK(image.role == i);
    }
    init(); m.package[22] = 1;
    CHECK(run(&audit, OKL_LOADER_FROM_ORIGINAL, 1000000) == OKL_LOADER_OK);
    CHECK(audit.observation.controller.role == OKL_ROLE_SPI_DIAGNOSTIC &&
        audit.observation.controller.capabilities == OKL_CAP_RECOVERY_READY &&
        !audit.observation.controller.trial_confirmed);
    CHECK(m.commits == 1 && !m.aborts && audit.program_blocks_acked == 448 &&
        audit.readback_blocks_verified == 448 && audit.quiet_completed);
    for (i = 1; i <= 11; ++i) {
        init(); m.package[22] = 1; m.invalid_observation = (int)i;
        CHECK(run(&audit, OKL_LOADER_FROM_ORIGINAL, 1000000) == OKL_LOADER_UNRESOLVED);
        CHECK(m.commits == 1 && !m.aborts && audit.quiet_completed);
    }
    init(); m.package[22] = 1; o = ops();
    CHECK(okl_loader_prepare(&image, m.package, sizeof(m.package), &o) == OKL_LOADER_OK);
    image.role = 2;
    CHECK(okl_loader_run(&image, OKL_LOADER_FROM_ORIGINAL, &o, 1000000, &audit) == OKL_LOADER_INVALID);
    CHECK(!m.begins && !m.enters && !m.exchanges);
}
static void test_every_exchange_failure(void) {
    unsigned i; okl_loader_audit a;
    for (i = 1; i <= 898; ++i) {
        init(); m.fail_exchange = i;
        CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, 1000000) == OKL_LOADER_IO);
        CHECK(!m.commits && !a.commit_attempted && m.ends == 1 && !m.leased);
        CHECK(m.aborts == (i > 1) && a.loader_verified == (i > 1));
        CHECK(!m.observations);
    }
}
static void test_strict_reports(void) {
    const unsigned positions[] = {1, 2, 3, 226, 450, 451, 674, 898};
    unsigned i, kind; okl_loader_audit a;
    for (i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) for (kind = 0; kind < 11; ++kind) {
        init(); m.corrupt_exchange = positions[i]; m.corruption = kind;
        CHECK(run(&a, OKL_LOADER_FROM_LEGACY_1_3, 1000000) != OKL_LOADER_OK);
        CHECK(!m.commits && !a.commit_attempted && m.ends == 1 && !m.leased);
    }
}
static void test_persistence_cancellation_and_deadlines(void) {
    unsigned i; okl_loader_audit a;
    for (i = OKL_LOADER_VALIDATED; i <= OKL_LOADER_COMMITTING; ++i) {
        init(); m.fail_persist_phase = (int)i;
        CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, 1000000) == OKL_LOADER_PERSIST);
        CHECK(a.persistence_failed && !m.commits && !a.commit_attempted && m.ends == 1);
    }
    init(); m.cancel = 1; CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, 1000000) == OKL_LOADER_CANCELLED);
    CHECK(!m.begins && !m.enters);
    init(); m.cancel_on_commit_persist = 1;
    CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, 1000000) == OKL_LOADER_CANCELLED && m.aborts == 1 && !m.commits);
    init(); CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, 0) == OKL_LOADER_TIMEOUT && !m.begins);
    for (i = 1; i <= 898; i += 113) {
        init(); CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, i * 10) == OKL_LOADER_TIMEOUT);
        CHECK(!m.commits && m.ends == 1);
    }
    init(); m.change_after_readback = 1;
    CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, 1000000) == OKL_LOADER_VERIFY && !m.commits && m.aborts == 1);
    init(); m.begin_fail = 1; CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, 1000000) == OKL_LOADER_BUSY && !m.ends);
    init(); m.entry_fail = 1; CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, 1000000) == OKL_LOADER_IO && !m.aborts && !m.exchanges);
}
static void test_postcommit_never_retries(void) {
    unsigned i; okl_loader_audit a;
    for (i = 0; i < 8; ++i) {
        init();
        if (i == 0) m.send_fail = 1;
        if (i == 1) m.send_not_complete = 1;
        if (i == 2) m.boundary_fail = 1;
        if (i == 3) m.observe_fail = 1;
        if (i == 4) m.observe_latency = 10000000;
        if (i == 5) m.cancel_in_quiet = 1;
        if (i == 6) m.fail_persist_phase = OKL_LOADER_QUIET;
        if (i == 7) m.cancel_in_observe = 1;
        CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, 1000000) ==
              (i == 5 || i == 7 ? OKL_LOADER_CANCELLED : i == 6 ? OKL_LOADER_PERSIST : OKL_LOADER_UNRESOLVED));
        CHECK(a.commit_attempted && a.quiet_completed && m.commits == 1 && !m.aborts && m.ends == 1);
        if (i < 2) CHECK(!m.boundaries && !m.observations);
    }
    for (i = 1; i <= 11; ++i) {
        init(); m.invalid_observation = (int)i;
        CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, 1000000) == OKL_LOADER_UNRESOLVED);
        CHECK(a.phase == OKL_LOADER_COMMIT_UNRESOLVED && m.commits == 1 && !m.aborts);
    }
    init(); m.fail_exchange = 3; m.send_fail = 1;
    CHECK(run(&a, OKL_LOADER_FROM_ORIGINAL, 1000000) == OKL_LOADER_IO && m.aborts == 1);
    CHECK(!m.boundaries && a.quiet_completed);
}
static void test_information_helper(void) {
    uint8_t report[90], good[90]; unsigned i;
    CHECK(okl_report_encode(report, 0, 0x10, 0x80, (uint8_t[80]){3, 24, 1, 2, 0, 0, 0, 2, 93}, 80) == OKL_OK);
    report[0] = 2; memcpy(good, report, 90); CHECK(okl_loader_information_valid(report));
    for (i = 0; i < 90; ++i) {
        memcpy(report, good, 90); report[i] ^= 1;
        CHECK(!okl_loader_information_valid(report));
        if (i != 88 && i != 89) { checksum(report); CHECK(!okl_loader_information_valid(report)); }
    }
    CHECK(!okl_loader_information_valid(NULL));
}
int main(void) {
    test_admission(); test_success(); test_explicit_diagnostic_role(); test_every_exchange_failure(); test_strict_reports();
    test_persistence_cancellation_and_deadlines(); test_postcommit_never_retries(); test_information_helper();
    printf("%u checks passed; original-bank loader admission, 898 exchange failure positions, buffered staging and commit lifecycle; no device I/O.\n", checks);
    return 0;
}
