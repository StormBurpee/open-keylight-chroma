#include "nxp_app.h"

enum { STATUS_OK = 2, STATUS_PARAMETER = 3, STATUS_UNSUPPORTED = 5, STATUS_OWNER = 8 };

static void zero(uint8_t *p, size_t n) { while (n--) *p++ = 0; }
static void copy(uint8_t *to, const uint8_t *from, size_t n) { while (n--) *to++ = *from++; }
static int equal(const uint8_t *a, const uint8_t *b, size_t n) {
    while (n--) if (*a++ != *b++) return 0;
    return 1;
}
static int zeros(const uint8_t *a, size_t n) {
    while (n--) if (*a++) return 0;
    return 1;
}
static uint8_t checksum(const uint8_t *report) {
    uint8_t value = 0; size_t i;
    for (i = 2; i < 88; ++i) value ^= report[i];
    return value;
}
static void write_be32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24); out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8); out[3] = (uint8_t)value;
}
/* Fixed 32 iterations avoids compiler runtime division dependencies on M0.
 * The bounded rendering numerators are <= 6,502,500 and divisors <= 2200. */
static uint32_t divide(uint32_t numerator, uint32_t denominator) {
    uint32_t result = 0, remainder = 0; unsigned i;
    for (i = 32; i; --i) {
        remainder = (remainder << 1) | ((numerator >> (i - 1)) & 1u);
        if (remainder >= denominator) { remainder -= denominator; result |= UINT32_C(1) << (i - 1); }
    }
    return result;
}

void nxp_state_init(nxp_state *s) {
    if (!s) return;
    zero((uint8_t *)s, sizeof(*s));
    s->temperature_k = 5200;
}

int nxp_state_valid(const nxp_state *s) {
    return s && s->claimed <= 1 && s->name_size <= NXP_NAME_MAX && s->boot_requested <= 1 && s->trial_confirmed <= 1 &&
        s->image_role <= NXP_ROLE_LIGHTING &&
        !(s->capabilities & ~(uint32_t)(NXP_CAP_RECOVERY_READY | NXP_CAP_LIGHTING_READY)) &&
        (!s->capabilities || s->part_id) &&
        (s->image_role != NXP_ROLE_SPI_TRIAL || s->capabilities == NXP_CAP_RECOVERY_READY) &&
        (s->image_role != NXP_ROLE_LIGHTING || s->capabilities == (NXP_CAP_RECOVERY_READY | NXP_CAP_LIGHTING_READY)) &&
        (!(s->capabilities & NXP_CAP_LIGHTING_READY) || s->image_role == NXP_ROLE_LIGHTING) &&
        (s->effect == 0 || s->effect == 1 || s->effect == 8) &&
        (!s->effect || s->white_brightness <= 38) &&
        s->temperature_k >= 3000 && s->temperature_k <= 7000;
}
int nxp_state_platform(nxp_state *s, int spi_trial, int recovery_ready,
                       int lighting_ready, uint32_t reset_cause) {
    if (!nxp_state_valid(s) || (spi_trial != 0 && spi_trial != 1) ||
        (recovery_ready != 0 && recovery_ready != 1) || (lighting_ready != 0 && lighting_ready != 1) ||
        (recovery_ready && !s->part_id) || (lighting_ready && (!recovery_ready || spi_trial))) return 0;
    s->capabilities = (recovery_ready ? NXP_CAP_RECOVERY_READY : 0u) |
        (lighting_ready ? NXP_CAP_LIGHTING_READY : 0u);
    s->image_role = lighting_ready ? NXP_ROLE_LIGHTING :
        (spi_trial && recovery_ready ? NXP_ROLE_SPI_TRIAL : NXP_ROLE_UNQUALIFIED);
    s->reset_cause = reset_cause;
    return 1;
}

static int owner_exempt(uint8_t cls, uint8_t op) {
    return cls == 0 && (op == 0x87 || op == 0x84 || op == 0xc9 || op == 0x49 || op == 0xfe || op == 0xfc);
}

int nxp_trial_expired(const nxp_state *s, uint32_t now_ms) {
    return s && !s->trial_confirmed && (uint32_t)(now_ms - s->trial_started_ms) >= 30000u;
}

static uint8_t dispatch(nxp_state *s, const uint8_t tag[6], const uint8_t *q, uint8_t *r, uint32_t now_ms) {
    const uint8_t *a = q + 8; uint8_t *b = r + 8;
    uint8_t n = q[5], cls = q[6], op = q[7];
    if (s->claimed && !equal(s->owner, tag, 6) && !owner_exempt(cls, op)) return STATUS_OWNER;
    if (cls == 0) {
        if (op == 0xfc) {
            if (n) return STATUS_PARAMETER;
            b[0] = 'O'; b[1] = 'K'; b[2] = 'L'; b[3] = 'C'; b[4] = 1;
            b[6] = s->image_role; b[7] = (uint8_t)(s->trial_confirmed | (s->boot_requested << 1));
            write_be32(b + 8, s->capabilities); write_be32(b + 12, s->part_id);
            write_be32(b + 16, now_ms); write_be32(b + 20, s->reset_cause);
            r[5] = 24; return STATUS_OK;
        }
        if (op == 0x87 && !n) { r[5] = 4; b[1] = 1; return STATUS_OK; }
        if (op == 0x84 && !n) { r[5] = 1; b[0] = s->boot_requested; return STATUS_OK; }
        if (op == 0xfe && !n) {
            if (!s->part_id) return 4;
            b[0] = (uint8_t)(s->part_id >> 24); b[1] = (uint8_t)(s->part_id >> 16);
            b[2] = (uint8_t)(s->part_id >> 8); b[3] = (uint8_t)s->part_id; r[5] = 4; return STATUS_OK;
        }
        if (op == 0xfd) {
            if (n != 4 || a[0] != 'O' || a[1] != 'K' || a[2] != 'L' || a[3] != 'C') return STATUS_PARAMETER;
            if (!s->claimed || !equal(s->owner, tag, 6)) return STATUS_OWNER;
            if (nxp_trial_expired(s, now_ms)) return STATUS_PARAMETER;
            s->trial_confirmed = 1; r[5] = 1; b[0] = 1; return STATUS_OK;
        }
        if (op == 4) {
            if (n != 1 || a[0] != 1) return STATUS_PARAMETER;
            if (!s->claimed || !equal(s->owner, tag, 6)) return STATUS_OWNER;
            s->boot_requested = 1; r[5] = 1; b[0] = 1; return STATUS_OK;
        }
        if (op == 0xc9 && !n) {
            b[0] = s->claimed; copy(b + 1, s->owner, 6);
            r[5] = 7;
            if (s->claimed) { b[7] = s->name_size; copy(b + 8, s->name, s->name_size); r[5] = (uint8_t)(8 + s->name_size); }
            return STATUS_OK;
        }
        if (op == 0x49) {
            if ((n != 72 && n != 80) || a[0] > 1 || a[7] > NXP_NAME_MAX ||
                !zeros(a + 8 + a[7], NXP_NAME_MAX - a[7]) || (n == 80 && !zeros(a + 72, 8))) return STATUS_PARAMETER;
            /* Claims are explicit. Releases may only come from our current
             * owner, tightening the old protocol's unconditional release. */
            if (!a[0] && s->claimed && !equal(s->owner, tag, 6)) return STATUS_OWNER;
            s->claimed = a[0]; zero(s->owner, 6); zero(s->name, NXP_NAME_MAX); s->name_size = 0;
            if (s->claimed) { copy(s->owner, tag, 6); s->name_size = a[7]; copy(s->name, a + 8, s->name_size); }
            copy(b, a, n); r[5] = n; return STATUS_OK;
        }
        return STATUS_UNSUPPORTED;
    }
    if (cls == 15) {
        if (op == 0x82) {
            if (n != 2 || !zeros(a, 2)) return STATUS_PARAMETER;
            b[2] = s->effect; r[5] = 6;
            if (s->effect == 1) { b[5] = 1; copy(b + 6, s->rgb, 3); r[5] = 9; }
            return STATUS_OK;
        }
        if (op == 0x84) {
            if (n != 2 || !zeros(a, 2)) return STATUS_PARAMETER;
            b[2] = s->rgb_brightness; r[5] = 3; return STATUS_OK;
        }
        if (op == 2) {
            if (n != 12 || a[0] || a[1] || a[3] || a[4] ||
                (a[2] != 0 && a[2] != 1 && a[2] != 8) ||
                a[5] != (a[2] == 1 ? 1 : 0) || !zeros(a + 6 + 3 * a[5], 6 - 3 * a[5])) return STATUS_PARAMETER;
            if (a[2] && s->white_brightness > 38) return STATUS_PARAMETER;
            s->effect = a[2];
            if (s->effect == 1) copy(s->rgb, a + 6, 3);
        } else if (op == 4) {
            if (n != 3 || a[0] || a[1]) return STATUS_PARAMETER;
            s->rgb_brightness = a[2];
        } else if (op == 3) {
            if (n != 9 || !zeros(a, 5) || a[8] || s->effect != 8) return STATUS_PARAMETER;
            copy(s->rgb, a + 5, 3); s->white_brightness = 0;
        } else return STATUS_UNSUPPORTED;
    } else if (cls == 3) {
        if (op == 0x81) {
            if (n != 2 || a[0] || a[1] != 32) return STATUS_PARAMETER;
            b[1] = 32; b[2] = (uint8_t)(s->temperature_k >> 8); b[3] = (uint8_t)s->temperature_k; r[5] = 5; return STATUS_OK;
        }
        if (op == 0x83) {
            if (n != 3 || a[0] || a[1] != 32 || a[2]) return STATUS_PARAMETER;
            b[1] = 32; b[2] = s->white_brightness; b[3] = s->effect; r[5] = 4; return STATUS_OK;
        }
        if (op == 1) {
            uint16_t temperature;
            if (n != 4 || a[0] || a[1] != 32) return STATUS_PARAMETER;
            temperature = (uint16_t)((uint16_t)a[2] * 256u + a[3]);
            if (temperature < 3000 || temperature > 7000) return STATUS_PARAMETER;
            s->temperature_k = temperature;
        } else if (op == 3) {
            if (n != 4 || a[0] || a[1] != 32 || a[3] != s->effect || (s->effect && a[2] > 38)) return STATUS_PARAMETER;
            s->white_brightness = a[2];
        } else return STATUS_UNSUPPORTED;
    } else return STATUS_UNSUPPORTED;
    copy(b, a, n); r[5] = n;
    return STATUS_OK;
}

static nxp_result connection_event(nxp_state *s, const uint8_t *request, uint8_t *reply) {
    uint8_t response[NXP_SPI_SIZE], *r = response + 7;
    uint8_t connected = request[7], count = request[8];
    int changed = 0;
    if (connected > 1 || (connected && !count)) return NXP_BAD_PACKET;
    if (connected && count == 1 && !s->connection_count && !s->claimed) {
        s->claimed = 1; copy(s->owner, request, 6); changed = 1;
    } else if (!connected && s->claimed && equal(s->owner, request, 6)) {
        s->claimed = 0; zero(s->owner, 6); changed = 1;
    }
    s->connection_count = count;
    if (!changed) return NXP_NO_REPLY;
    zero(s->name, NXP_NAME_MAX); s->name_size = 0; ++s->revision;
    zero(response, sizeof(response)); response[6] = 4;
    r[0] = STATUS_OK; r[5] = 80; r[7] = 0x49; r[8] = s->claimed;
    copy(r + 9, request, 6); r[88] = checksum(r);
    copy(reply, response, sizeof(response)); return NXP_OK;
}

nxp_result nxp_process(nxp_state *s, const uint8_t *request, size_t size, uint8_t *reply, size_t capacity) {
    return nxp_process_at(s, request, size, reply, capacity, 0);
}
nxp_result nxp_process_at(nxp_state *s, const uint8_t *request, size_t size, uint8_t *reply, size_t capacity, uint32_t now_ms) {
    uint8_t response[NXP_SPI_SIZE]; const uint8_t *q; uint8_t *r; nxp_state next;
    if (!s || !request || !reply || capacity < NXP_SPI_SIZE) return NXP_INVALID;
    if (!nxp_state_valid(s)) return NXP_INVALID;
    if (size != NXP_SPI_SIZE && size != NXP_CONNECTION_SIZE) return NXP_BAD_PACKET;
    if (zeros(request, 6) || (request[0] & 1)) return NXP_BAD_PACKET;
    if (size == NXP_CONNECTION_SIZE) {
        if (request[6] != 11) return NXP_BAD_PACKET;
        return connection_event(s, request, reply);
    }
    q = request + 7;
    if (request[6] || zeros(request, 6) || (request[0] & 1) || q[0] || !zeros(q + 2, 3) ||
        q[5] > 80 || q[89] || checksum(q) != q[88] || !zeros(q + 8 + q[5], 80 - q[5])) return NXP_BAD_PACKET;
    zero(response, sizeof(response)); copy(response, request, 6);
    r = response + 7; r[1] = q[1]; r[6] = q[6]; r[7] = q[7]; next = *s;
    r[0] = dispatch(&next, request, q, r, now_ms);
    if (r[0] == STATUS_OK) {
        /* Revision wraps by defined unsigned arithmetic. It counts accepted
         * mutation requests, not physical output or optical frames. */
        if (!(q[7] & 0x80) || (q[6] == 0 && q[7] == 0xfd)) ++next.revision;
        *s = next;
    }
    r[88] = checksum(r); copy(reply, response, sizeof(response)); return NXP_OK;
}

void nxp_render(const nxp_state *s, int qualified, nxp_pwm_frame *out) {
    uint32_t warm, cool; uint8_t brightness;
    if (!out) return;
    out->red_match = out->green_match = out->blue_match = 25500;
    out->cool_match = out->warm_match = 255;
    if (!qualified || !nxp_state_valid(s)) return;
    if (s->effect) {
        out->red_match = (uint16_t)(25500u - divide((uint32_t)s->rgb[0] * s->rgb_brightness * 100u, 255));
        out->green_match = (uint16_t)(25500u - divide((uint32_t)s->rgb[1] * s->rgb_brightness * 100u, 255));
        out->blue_match = (uint16_t)(25500u - divide((uint32_t)s->rgb[2] * s->rgb_brightness * 100u, 255));
    }
    brightness = s->white_brightness;
    if (s->temperature_k < 5200) {
        warm = brightness;
        cool = divide(divide(255u * (s->temperature_k - 3000u), 2200u) * brightness, 255u);
    } else {
        cool = brightness;
        warm = divide(divide(255u * (7000u - s->temperature_k), 1800u) * brightness, 255u);
    }
    out->cool_match = (uint16_t)(255u - cool); out->warm_match = (uint16_t)(255u - warm);
}

void nxp_link_init(nxp_link *link, nxp_state *state) {
    if (!link) return;
    zero((uint8_t *)link, sizeof(*link)); link->state = state;
}
void nxp_link_cancel(nxp_link *link) {
    if (!link) return;
    link->phase = NXP_LINK_REQUEST; link->recovery_ready = 0; link->response_size = 0;
    if (link->state) link->state->boot_requested = 0;
}
int nxp_link_ready(const nxp_link *link) { return link && link->phase != NXP_LINK_REQUEST; }
nxp_result nxp_link_expire(nxp_link *link, uint32_t now_ms) {
    if (!link) return NXP_INVALID;
    if (link->phase != NXP_LINK_REQUEST && (uint32_t)(now_ms - link->prepared_ms) >= 100u) {
        nxp_link_cancel(link); return NXP_EXPIRED;
    }
    return NXP_OK;
}
nxp_result nxp_link_transaction(nxp_link *link, const uint8_t *tx, size_t size,
                                uint8_t *rx, size_t capacity, uint32_t now_ms) {
    nxp_result result;
    if (!link || !tx || !rx || capacity < size || size > NXP_SPI_SIZE) return NXP_INVALID;
    if (nxp_link_expire(link, now_ms) == NXP_EXPIRED) return NXP_EXPIRED;
    if (link->phase == NXP_LINK_REQUEST) {
        result = nxp_process_at(link->state, tx, size, link->response, sizeof(link->response), now_ms);
        if (result != NXP_OK && result != NXP_NO_REPLY) return result;
        link->response_size = result == NXP_OK ? NXP_SPI_SIZE : 0;
        link->prepared_ms = now_ms; link->phase = NXP_LINK_LENGTH; zero(rx, size); return NXP_OK;
    }
    if (!zeros(tx, size) || size != (link->phase == NXP_LINK_LENGTH ? 2u : NXP_SPI_SIZE)) {
        nxp_link_cancel(link); return NXP_BAD_PACKET;
    }
    if (link->phase == NXP_LINK_LENGTH) {
        rx[0] = 0; rx[1] = link->response_size;
        link->phase = link->response_size ? NXP_LINK_BODY : NXP_LINK_REQUEST;
    }
    else {
        copy(rx, link->response, NXP_SPI_SIZE); link->phase = NXP_LINK_REQUEST;
        link->recovery_ready = link->state ? link->state->boot_requested : 0;
    }
    return NXP_OK;
}
