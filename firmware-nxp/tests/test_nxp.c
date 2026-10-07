#include "nxp_app.h"
#include "okl_nxp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static const uint8_t tag[6] = {2, 1, 2, 3, 4, 5};
static uint8_t xor_report(const uint8_t *p) { uint8_t x = 0; unsigned i; for (i = 2; i < 88; ++i) x ^= p[i]; return x; }
static void packet(uint8_t out[97], uint8_t cls, uint8_t op, const uint8_t *args, uint8_t size) {
    memset(out, 0, 97); memcpy(out, tag, 6); out[8] = 7;
    out[12] = size; out[13] = cls; out[14] = op;
    if (size) memcpy(out + 15, args, size);
    out[95] = xor_report(out + 7);
}
static uint8_t command(nxp_state *s, uint8_t cls, uint8_t op, const uint8_t *args, uint8_t size) {
    uint8_t q[97], r[97]; packet(q, cls, op, args, size);
    CHECK(nxp_process(s, q, 97, r, 97) == NXP_OK);
    CHECK(r[8] == 7 && r[13] == cls && r[14] == op && r[95] == xor_report(r + 7));
    CHECK(nxp_state_valid(s)); return r[7];
}
static void test_packets(void) {
    nxp_state s, before; uint8_t q[97], r[97], good[97]; unsigned i;
    nxp_state_init(&s); CHECK(nxp_state_valid(&s)); CHECK(!s.rgb_brightness && !s.white_brightness);
    packet(q, 0, 0x87, NULL, 0); memcpy(good, q, 97); before = s;
    CHECK(q[95] == 0x87); CHECK(nxp_process(&s, q, 97, r, 97) == NXP_OK);
    CHECK(r[7] == 2 && r[12] == 4 && !r[15] && r[16] == 1 && !r[17] && !r[18]);
    for (i = 0; i < 200; ++i) if (i != 97) {
        memset(r, 0xa5, 97); CHECK(nxp_process(&s, q, i, r, 97) == NXP_BAD_PACKET);
        CHECK(r[0] == 0xa5 && !memcmp(&s, &before, sizeof(s)));
    }
    for (i = 9; i < 97; ++i) {
        memcpy(q, good, 97); q[i] ^= 1;
        CHECK(nxp_process(&s, q, 97, r, 97) == NXP_BAD_PACKET);
        CHECK(!memcmp(&s, &before, sizeof(s)));
    }
    memcpy(q, good, 97); q[6] = 1; CHECK(nxp_process(&s, q, 97, r, 97) == NXP_BAD_PACKET);
    memcpy(q, good, 97); q[0] |= 1; CHECK(nxp_process(&s, q, 97, r, 97) == NXP_BAD_PACKET);
    memcpy(q, good, 97); memset(q, 0, 6); CHECK(nxp_process(&s, q, 97, r, 97) == NXP_BAD_PACKET);
    memcpy(q, good, 97); CHECK(nxp_process(&s, q, 97, q, 97) == NXP_OK && q[7] == 2);
    CHECK(command(&s, 0x10, 1, NULL, 0) == 5); CHECK(command(&s, 0, 4, NULL, 0) == 3);
}
static void test_parameters(void) {
    nxp_state s, before; uint8_t a[80] = {0}; unsigned i;
    nxp_state_init(&s); a[1] = 32;
    for (i = 0; i <= 65535; ++i) {
        a[2] = (uint8_t)(i >> 8); a[3] = (uint8_t)i; before = s;
        CHECK(command(&s, 3, 1, a, 4) == (i >= 3000 && i <= 7000 ? 2 : 3));
        if (i < 3000 || i > 7000) CHECK(!memcmp(&s, &before, sizeof(s)));
        else CHECK(s.temperature_k == i);
    }
    nxp_state_init(&s); memset(a, 0, sizeof(a)); a[2] = 1; a[5] = 1;
    a[6] = 10; a[7] = 20; a[8] = 30;
    CHECK(command(&s, 15, 2, a, 12) == 2 && s.rgb[2] == 30);
    before = s; a[0] = 1; CHECK(command(&s, 15, 2, a, 12) == 3 && !memcmp(&s, &before, sizeof(s)));
    a[0] = 0; a[5] = 255; CHECK(command(&s, 15, 2, a, 12) == 3);
    memset(a, 0, sizeof(a)); a[1] = 32; a[3] = 1;
    for (i = 0; i < 256; ++i) { a[2] = (uint8_t)i; CHECK(command(&s, 3, 3, a, 4) == (i <= 38 ? 2 : 3)); }
    memset(a, 0, sizeof(a)); a[5] = 1; a[6] = 2; a[7] = 3;
    CHECK(command(&s, 15, 3, a, 9) == 3); /* A frame cannot silently select custom mode. */
    memset(a, 0, sizeof(a)); a[2] = 8; CHECK(command(&s, 15, 2, a, 12) == 2);
    memset(a, 0, sizeof(a)); a[5] = 1; a[6] = 2; a[7] = 3;
    CHECK(command(&s, 15, 3, a, 9) == 2 && s.rgb[0] == 1 && s.rgb[2] == 3 && !s.white_brightness);
    a[8] = 1; CHECK(command(&s, 15, 3, a, 9) == 3);
    s.effect = 0; s.white_brightness = 255; memset(a, 0, sizeof(a)); a[2] = 8;
    CHECK(command(&s, 15, 2, a, 12) == 3); /* Must lower white before enabling RGB. */
}
static void test_render(void) {
    nxp_state s; nxp_pwm_frame frame; unsigned t, b, c;
    nxp_state_init(&s); s.effect = 1;
    for (b = 0; b < 256; ++b) for (c = 0; c < 256; ++c) {
        s.rgb[0] = (uint8_t)c; s.rgb[1] = (uint8_t)(255 - c); s.rgb[2] = 255; s.rgb_brightness = (uint8_t)b;
        nxp_render(&s, 1, &frame);
        CHECK(frame.red_match == 25500u - c * b * 100u / 255u);
        CHECK(frame.green_match == 25500u - (255u - c) * b * 100u / 255u);
        CHECK(frame.blue_match == 25500u - b * 100u);
        nxp_render(&s, 0, &frame); CHECK(frame.red_match == 25500 && frame.cool_match == 255);
    }
    s.effect = 0;
    for (t = 3000; t <= 7000; ++t) for (b = 0; b <= 255; b += 17) {
        unsigned warm, cool; s.temperature_k = (uint16_t)t; s.white_brightness = (uint8_t)b;
        nxp_render(&s, 1, &frame);
        cool = t < 5200 ? (255u * (t - 3000) / 2200u) * b / 255u : b;
        warm = t >= 5200 ? (255u * (7000 - t) / 1800u) * b / 255u : b;
        CHECK(frame.cool_match == 255 - cool && frame.warm_match == 255 - warm);
        CHECK(frame.red_match == 25500 && frame.green_match == 25500 && frame.blue_match == 25500);
    }
    s.temperature_k = 7001; nxp_render(&s, 1, &frame); CHECK(frame.cool_match == 255 && frame.warm_match == 255);
}

typedef struct { nxp_state state; nxp_link link; uint64_t now; int locked; unsigned requests; } fixture;
static uint64_t now_us(void *user) { return ((fixture *)user)->now; }
static okl_result lock(void *user, uint64_t deadline) { fixture *f = user; CHECK(deadline > f->now); if (f->locked) return OKL_BUSY; f->locked = 1; return OKL_OK; }
static void unlock(void *user) { fixture *f = user; CHECK(f->locked); f->locked = 0; }
static okl_result arm(void *user, uint64_t deadline) { fixture *f = user; CHECK(f->locked && deadline > f->now); return nxp_link_ready(&f->link) ? OKL_IO : OKL_OK; }
static okl_result wait_ready(void *user, uint64_t deadline) { fixture *f = user; CHECK(f->locked && deadline > f->now); return nxp_link_ready(&f->link) ? OKL_OK : OKL_TIMEOUT; }
static okl_result transfer(void *user, const uint8_t *tx, uint8_t *rx, size_t size, uint64_t deadline) {
    fixture *f = user; CHECK(f->locked && deadline > f->now); if (f->link.phase == NXP_LINK_REQUEST) ++f->requests;
    return nxp_link_transaction(&f->link, tx, size, rx, size, (uint32_t)(f->now / 1000)) == NXP_OK ? OKL_OK : OKL_IO;
}
static void test_driver_integration(void) {
    fixture f; okl_transport transport; okl_nxp driver; okl_owner owner; okl_light_state state;
    okl_request request; okl_reply reply; uint8_t rgb[3] = {5, 70, 255}, args[72] = {0}, q[97], r[97];
    memset(&f, 0, sizeof(f)); nxp_state_init(&f.state); nxp_link_init(&f.link, &f.state);
    memset(&transport, 0, sizeof(transport)); transport.user = &f; transport.now_us = now_us;
    transport.lock = lock; transport.unlock = unlock; transport.arm_ready = arm; transport.wait_ready = wait_ready; transport.transfer = transfer;
    CHECK(okl_nxp_init(&driver, &transport, tag) == OKL_OK);
    CHECK(okl_nxp_get_owner(&driver, &owner, 150000) == OKL_OK && !owner.claimed);
    CHECK(okl_nxp_claim(&driver, (const uint8_t *)"Open Keylight Chroma", 20, 150000) == OKL_OK);
    CHECK(okl_nxp_get_owner(&driver, &owner, 150000) == OKL_OK && owner.name_size == 20 && owner.claimed);
    CHECK(okl_request_custom(&request) == OKL_OK && okl_nxp_execute(&driver, &request, &reply, 150000) == OKL_OK);
    CHECK(okl_request_frame(&request, rgb) == OKL_OK && okl_nxp_execute(&driver, &request, &reply, 150000) == OKL_OK);
    CHECK(okl_request_color_brightness(&request, 128) == OKL_OK && okl_nxp_execute(&driver, &request, &reply, 150000) == OKL_OK);
    CHECK(okl_nxp_read_state(&driver, &state, 150000) == OKL_OK && state.effect == 8 && state.color_brightness == 128 && state.color_count == 0);
    CHECK(!memcmp(f.state.rgb, rgb, 3)); /* Internal simulation, not a wire framebuffer getter. */
    packet(q, 0, 0x49, args, 72); q[0] = 4;
    CHECK(nxp_process(&f.state, q, 97, r, 97) == NXP_OK && r[7] == 8 && f.state.claimed);
    packet(q, 15, 0x84, args, 2); q[0] = 4; CHECK(nxp_process(&f.state, q, 97, r, 97) == NXP_OK && r[7] == 8);
    CHECK(okl_request_static(&request, rgb) == OKL_OK && okl_nxp_execute(&driver, &request, &reply, 150000) == OKL_OK);
    CHECK(okl_nxp_read_state(&driver, &state, 150000) == OKL_OK && state.effect == 1 && state.color_count == 1 && state.colors[2] == 255);
    CHECK(okl_nxp_release(&driver, 150000) == OKL_OK && !f.state.claimed && !f.locked);
    {
        uint8_t registration[80] = {0}; registration[0] = 1;
        packet(q, 0, 0x49, registration, 80);
        CHECK(nxp_process(&f.state, q, 97, r, 97) == NXP_OK && r[7] == 2 && f.state.claimed);
        registration[79] = 1; packet(q, 0, 0x49, registration, 80);
        CHECK(nxp_process(&f.state, q, 97, r, 97) == NXP_OK && r[7] == 3);
    }
}
static void test_lifecycle_integration(void) {
    fixture f; okl_transport transport; okl_nxp driver; okl_request request;
    okl_reply reply, good; okl_controller_status status, before; uint32_t part;
    unsigned role, i;
    memset(&f, 0, sizeof(f)); nxp_state_init(&f.state); nxp_link_init(&f.link, &f.state);
    f.state.part_id = 0x0000bc40; f.now = 12345000;
    memset(&transport, 0, sizeof(transport)); transport.user = &f; transport.now_us = now_us;
    transport.lock = lock; transport.unlock = unlock; transport.arm_ready = arm;
    transport.wait_ready = wait_ready; transport.transfer = transfer;
    CHECK(okl_nxp_init(&driver, &transport, tag) == OKL_OK);
    for (role = NXP_ROLE_SPI_TRIAL; role <= NXP_ROLE_LIGHTING; ++role) {
        CHECK(nxp_state_platform(&f.state, role == NXP_ROLE_SPI_TRIAL, 1, role == NXP_ROLE_LIGHTING, 0x80000014));
        f.state.claimed = 1; memset(f.state.owner, 4, 6); /* Discovery is owner-exempt. */
        CHECK(okl_request_get(&request, OKL_GET_CONTROLLER_STATUS) == OKL_OK);
        CHECK(okl_nxp_execute(&driver, &request, &reply, f.now + 150000) == OKL_OK);
        CHECK(okl_reply_decode_controller_status(&status, &reply) == OKL_OK);
        CHECK(status.abi_major == 1 && status.abi_minor == 0 && status.role == role);
        CHECK(status.capabilities == (role == NXP_ROLE_SPI_TRIAL ? 1u : 3u));
        CHECK(status.part_id == 0x0000bc40 && status.uptime_ms == 12345 && status.reset_cause == 0x80000014);
        CHECK(!status.trial_confirmed && !status.boot_requested && f.state.owner[0] == 4);
        CHECK(okl_request_get(&request, OKL_GET_PART_ID) == OKL_OK);
        CHECK(okl_nxp_execute(&driver, &request, &reply, f.now + 150000) == OKL_OK);
        CHECK(okl_reply_decode_part_id(&part, &reply) == OKL_OK && part == status.part_id);
    }
    CHECK(okl_request_confirm_controller(&request) == OKL_OK);
    CHECK(okl_nxp_execute(&driver, &request, &reply, f.now + 150000) == OKL_OWNER_DENIED);
    CHECK(!f.state.trial_confirmed && !driver.needs_recovery);
    f.state.claimed = 0; memset(f.state.owner, 0, sizeof(f.state.owner));
    CHECK(okl_nxp_claim(&driver, (const uint8_t *)"Open Keylight Chroma", 20, f.now + 150000) == OKL_OK);
    CHECK(okl_nxp_execute(&driver, &request, &reply, f.now + 150000) == OKL_OK);
    CHECK(okl_reply_check_controller_confirmation(&reply) == OKL_OK);
    CHECK(okl_request_get(&request, OKL_GET_CONTROLLER_STATUS) == OKL_OK);
    CHECK(okl_nxp_execute(&driver, &request, &reply, f.now + 150000) == OKL_OK);
    CHECK(okl_reply_decode_controller_status(&status, &reply) == OKL_OK && status.trial_confirmed);
    CHECK(status.role == OKL_ROLE_LIGHTING && !status.boot_requested);
    good = reply; before = status;
    for (i = 0; i <= OKL_ARGUMENT_BYTES; ++i) if (i != OKL_CONTROLLER_STATUS_BYTES) {
        reply = good; reply.report.size = (uint8_t)i;
        CHECK(okl_reply_decode_controller_status(&status, &reply) == OKL_PROTOCOL);
        CHECK(!memcmp(&status, &before, sizeof(status)));
    }
    for (i = 0; i < 9; ++i) {
        reply = good;
        if (i < 4) reply.report.arguments[i] ^= 1;
        else if (i == 4) reply.report.arguments[4] = 2;
        else if (i == 5) reply.report.arguments[5] = 1;
        else if (i == 6) reply.report.arguments[6] = 3;
        else if (i == 7) reply.report.arguments[7] = 4;
        else reply.report.arguments[11] |= 4;
        CHECK(okl_reply_decode_controller_status(&status, &reply) == OKL_PROTOCOL);
        CHECK(!memcmp(&status, &before, sizeof(status)));
    }
    CHECK(okl_nxp_release(&driver, f.now + 150000) == OKL_OK && !f.locked);
}
static void test_handshake(void) {
    nxp_state state; nxp_link link; uint8_t q[97], r[97], dummy[97] = {0}; unsigned i;
    nxp_state_init(&state); nxp_link_init(&link, &state); packet(q, 0, 0x87, NULL, 0);
    CHECK(nxp_link_transaction(&link, q, 97, r, 97, UINT32_MAX - 50) == NXP_OK && nxp_link_ready(&link));
    CHECK(nxp_link_transaction(&link, dummy, 2, r, 97, 10) == NXP_OK && r[0] == 0 && r[1] == 97);
    CHECK(nxp_link_transaction(&link, dummy, 97, r, 97, 20) == NXP_OK && r[7] == 2 && !nxp_link_ready(&link));
    for (i = 0; i < 100; ++i) if (i != 2) {
        CHECK(nxp_link_transaction(&link, q, 97, r, 97, 0) == NXP_OK);
        CHECK(nxp_link_transaction(&link, dummy, i, r, 97, 1) == (i > 97 ? NXP_INVALID : NXP_BAD_PACKET));
        if (i > 97) CHECK(nxp_link_expire(&link, 100) == NXP_EXPIRED);
        CHECK(!nxp_link_ready(&link));
    }
    CHECK(nxp_link_transaction(&link, q, 97, r, 97, 0) == NXP_OK);
    CHECK(nxp_link_expire(&link, 99) == NXP_OK && nxp_link_expire(&link, 100) == NXP_EXPIRED);
    CHECK(nxp_link_transaction(&link, dummy, 2, r, 97, 101) == NXP_BAD_PACKET);
    /* Recovery is owned and acknowledged before the platform may reset. */
    dummy[0] = 1; packet(q, 0, 4, dummy, 1);
    CHECK(nxp_process(&state, q, 97, r, 97) == NXP_OK && r[7] == 8 && !state.boot_requested);
    state.claimed = 1; memcpy(state.owner, tag, 6); memset(dummy, 0, 97);
    CHECK(nxp_link_transaction(&link, q, 97, r, 97, 110) == NXP_OK && state.boot_requested && !link.recovery_ready);
    CHECK(nxp_link_transaction(&link, dummy, 2, r, 97, 111) == NXP_OK && !link.recovery_ready);
    CHECK(nxp_link_expire(&link, 210) == NXP_EXPIRED && !state.boot_requested && !link.recovery_ready);
    CHECK(nxp_link_transaction(&link, q, 97, r, 97, 220) == NXP_OK);
    CHECK(nxp_link_transaction(&link, dummy, 2, r, 97, 221) == NXP_OK);
    CHECK(nxp_link_transaction(&link, dummy, 97, r, 97, 222) == NXP_OK && r[7] == 2 && link.recovery_ready);
    nxp_link_cancel(&link); CHECK(!link.recovery_ready && !state.boot_requested);
}
static void test_trial(void) {
    nxp_state s, before; nxp_link link; uint8_t q[97], r[97], dummy[97] = {0};
    const uint8_t confirm[4] = {'O', 'K', 'L', 'C'};
    nxp_state_init(&s); nxp_link_init(&link, &s); packet(q, 0, 0xfd, confirm, 4);
    CHECK(!nxp_trial_expired(&s, 29999) && nxp_trial_expired(&s, 30000));
    CHECK(nxp_process_at(&s, q, 97, r, 97, 1) == NXP_OK && r[7] == 8 && !s.trial_confirmed);
    s.claimed = 1; memcpy(s.owner, tag, 6); before = s;
    q[0] = 4; CHECK(nxp_process_at(&s, q, 97, r, 97, 1) == NXP_OK && r[7] == 8 && !memcmp(&s, &before, sizeof(s)));
    q[0] = 2; q[15] = 'N'; q[95] = xor_report(q + 7);
    CHECK(nxp_process_at(&s, q, 97, r, 97, 1) == NXP_OK && r[7] == 3 && !s.trial_confirmed);
    packet(q, 0, 0xfd, confirm, 4);
    CHECK(nxp_process_at(&s, q, 97, r, 97, 30000) == NXP_OK && r[7] == 3 && !s.trial_confirmed);
    CHECK(nxp_process_at(&s, q, 97, r, 97, 29999) == NXP_OK && r[7] == 2 && s.trial_confirmed && r[15] == 1);
    CHECK(!nxp_trial_expired(&s, UINT32_MAX));
    s.trial_confirmed = 0; s.trial_started_ms = UINT32_MAX - 999;
    CHECK(!nxp_trial_expired(&s, 28999) && nxp_trial_expired(&s, 29000));
    s.trial_started_ms = 0;
    CHECK(nxp_link_transaction(&link, q, 97, r, 97, 30001) == NXP_OK && !s.trial_confirmed);
    CHECK(nxp_link_transaction(&link, dummy, 2, r, 97, 30002) == NXP_OK);
    CHECK(nxp_link_transaction(&link, dummy, 97, r, 97, 30003) == NXP_OK && r[7] == 3);
    packet(q, 0, 0xfe, NULL, 0); CHECK(nxp_process(&s, q, 97, r, 97) == NXP_OK && r[7] == 4);
    s.part_id = 0x0001bc40; CHECK(nxp_process(&s, q, 97, r, 97) == NXP_OK && r[7] == 2);
    CHECK(r[12] == 4 && !r[15] && r[16] == 1 && r[17] == 0xbc && r[18] == 0x40);
}
static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void test_platform_status(void) {
    nxp_state s, before; uint8_t q[97], r[97], bad = 0; unsigned role, flags, i;
    const uint32_t times[] = {0, 29999, 30000, UINT32_MAX};
    nxp_state_init(&s); packet(q, 0, 0xfc, NULL, 0); before = s;
    CHECK(nxp_process_at(&s, q, 97, r, 97, 1) == NXP_OK && r[7] == 2 && r[12] == 24);
    CHECK(!memcmp(r + 15, "OKLC\x01\x00\x00\x00", 8));
    CHECK(read_be32(r + 23) == 0 && read_be32(r + 27) == 0 && read_be32(r + 31) == 1);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    CHECK(!nxp_state_platform(&s, 1, 1, 0, 0) && !memcmp(&s, &before, sizeof(s)));
    s.part_id = 0x0000bc40;
    for (role = 0; role < 3; ++role) for (flags = 0; flags < 4; ++flags) {
        CHECK(nxp_state_platform(&s, role == 1, role != 0, role == 2, UINT32_MAX));
        s.trial_confirmed = (uint8_t)(flags & 1); s.boot_requested = (uint8_t)(flags >> 1);
        s.claimed = 1; memset(s.owner, 4, 6); /* FC is owner-exempt. */
        before = s;
        for (i = 0; i < sizeof(times) / sizeof(times[0]); ++i) {
            CHECK(nxp_process_at(&s, q, 97, r, 97, times[i]) == NXP_OK && r[7] == 2);
            CHECK(r[8] == 7 && r[12] == 24 && r[21] == role && r[22] == flags && r[95] == xor_report(r + 7));
            CHECK(read_be32(r + 23) == (role == 2 ? 3u : role == 1 ? 1u : 0u));
            CHECK(read_be32(r + 27) == 0x0000bc40 && read_be32(r + 31) == times[i] && read_be32(r + 35) == UINT32_MAX);
            CHECK(!memcmp(&s, &before, sizeof(s)) && !memcmp(r + 39, (uint8_t[56]){0}, 56));
        }
    }
    before = s;
    CHECK(!nxp_state_platform(&s, 1, 1, 1, 0) && !memcmp(&s, &before, sizeof(s)));
    CHECK(!nxp_state_platform(&s, 0, 0, 1, 0) && !memcmp(&s, &before, sizeof(s)));
    CHECK(!nxp_state_platform(&s, 2, 1, 0, 0) && !memcmp(&s, &before, sizeof(s)));
    CHECK(!nxp_state_platform(&s, 0, -1, 0, 0) && !memcmp(&s, &before, sizeof(s)));
    packet(q, 0, 0xfc, &bad, 1);
    CHECK(nxp_process_at(&s, q, 97, r, 97, 123) == NXP_OK && r[7] == 3 && r[12] == 0);
    CHECK(!memcmp(&s, &before, sizeof(s)));
    for (i = 4; i < 32; ++i) { s.capabilities = i; CHECK(!nxp_state_valid(&s)); }
    s = before; s.image_role = 3; CHECK(!nxp_state_valid(&s));
    s = before; s.image_role = NXP_ROLE_SPI_TRIAL; CHECK(!nxp_state_valid(&s)); /* Cannot advertise lighting. */
    s = before; s.capabilities = NXP_CAP_RECOVERY_READY; CHECK(!nxp_state_valid(&s)); /* Lighting role needs runtime PWM. */
}
static void test_connections(void) {
    nxp_state s, before; nxp_link link; uint8_t q[97] = {0}, r[97], dummy[97] = {0}; unsigned i;
    memcpy(q, tag, 6); q[6] = 11; q[7] = q[8] = 1;
    nxp_state_init(&s); nxp_link_init(&link, &s);
    s.effect = 1; s.rgb[0] = 18; s.rgb[1] = 42; s.rgb[2] = 240; s.rgb_brightness = 100;
    CHECK(nxp_link_transaction(&link, q, 9, r, 97, 10) == NXP_OK && s.claimed && nxp_link_ready(&link));
    CHECK(!memcmp(s.owner, tag, 6) && s.connection_count == 1 && !s.trial_confirmed);
    CHECK(s.effect == 1 && s.rgb[1] == 42 && s.rgb_brightness == 100 && !s.boot_requested);
    CHECK(nxp_link_transaction(&link, dummy, 2, r, 97, 11) == NXP_OK && r[1] == 97);
    CHECK(nxp_link_transaction(&link, dummy, 97, r, 97, 12) == NXP_OK);
    CHECK(!memcmp(r, dummy, 6) && r[6] == 4 && r[7] == 2 && r[12] == 80 && r[14] == 0x49 && r[15] == 1);
    CHECK(!memcmp(r + 16, tag, 6) && r[95] == xor_report(r + 7) && !nxp_link_ready(&link));
    before = s; memset(r, 0xa5, 97);
    CHECK(nxp_process(&s, q, 9, r, 97) == NXP_NO_REPLY && !memcmp(&s, &before, sizeof(s)) && r[0] == 0xa5);
    q[0] = 4; q[8] = 2;
    CHECK(nxp_link_transaction(&link, q, 9, r, 97, 20) == NXP_OK && nxp_link_ready(&link) && !memcmp(s.owner, tag, 6));
    CHECK(nxp_link_transaction(&link, dummy, 2, r, 97, 21) == NXP_OK && !r[0] && !r[1] && !nxp_link_ready(&link));
    q[7] = 0; q[8] = 1;
    CHECK(nxp_process(&s, q, 9, r, 97) == NXP_NO_REPLY && s.claimed && !memcmp(s.owner, tag, 6));
    q[0] = tag[0]; q[8] = 0;
    CHECK(nxp_process(&s, q, 9, r, 97) == NXP_OK && !s.claimed && !s.connection_count && r[15] == 0);
    CHECK(!memcmp(r + 16, tag, 6) && r[95] == xor_report(r + 7));
    CHECK(s.rgb[0] == 18 && s.rgb_brightness == 100 && s.effect == 1 && !s.trial_confirmed);
    /* An explicit owner is never displaced by a first-connection event. */
    s.claimed = 1; memset(s.owner, 4, 6); q[7] = q[8] = 1;
    CHECK(nxp_process(&s, q, 9, r, 97) == NXP_NO_REPLY && s.owner[0] == 4);
    before = s;
    for (i = 2; i <= 255; ++i) {
        q[7] = (uint8_t)i; CHECK(nxp_process(&s, q, 9, r, 97) == NXP_BAD_PACKET);
        CHECK(!memcmp(&s, &before, sizeof(s)));
    }
    q[7] = 1; q[8] = 0; CHECK(nxp_process(&s, q, 9, r, 97) == NXP_BAD_PACKET);
    q[8] = 1;
    for (i = 0; i < 97; ++i) if (i != 9) {
        CHECK(nxp_process(&s, q, i, r, 97) == NXP_BAD_PACKET && !memcmp(&s, &before, sizeof(s)));
    }
    q[0] |= 1; CHECK(nxp_process(&s, q, 9, r, 97) == NXP_BAD_PACKET);
    memset(q, 0, 6); CHECK(nxp_process(&s, q, 9, r, 97) == NXP_BAD_PACKET);
    memcpy(q, tag, 6); q[6] = 10; CHECK(nxp_process(&s, q, 9, r, 97) == NXP_BAD_PACKET);
    /* Every reported count is bounded by its byte; only an unowned 0->1 claims. */
    for (i = 1; i <= 255; ++i) {
        nxp_state_init(&s); q[6] = 11; q[8] = (uint8_t)i;
        CHECK(nxp_process(&s, q, 9, r, 97) == (i == 1 ? NXP_OK : NXP_NO_REPLY));
        CHECK(s.claimed == (i == 1) && s.connection_count == i);
    }
}
int main(void) {
    test_packets(); test_parameters(); test_render(); test_driver_integration(); test_lifecycle_integration(); test_handshake(); test_trial(); test_platform_status(); test_connections();
    printf("%u checks passed; original NXP protocol, arithmetic and ESP-driver integration; no device I/O.\n", checks);
    return 0;
}
