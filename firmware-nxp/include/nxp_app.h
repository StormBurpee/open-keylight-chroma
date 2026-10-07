#ifndef NXP_APP_H
#define NXP_APP_H

#include <stddef.h>
#include <stdint.h>

enum { NXP_REPORT_SIZE = 90, NXP_SPI_SIZE = 97, NXP_CONNECTION_SIZE = 9, NXP_NAME_MAX = 64 };
enum { NXP_ROLE_UNQUALIFIED = 0, NXP_ROLE_SPI_TRIAL = 1, NXP_ROLE_LIGHTING = 2 };
enum { NXP_CAP_RECOVERY_READY = 1, NXP_CAP_LIGHTING_READY = 2 };
enum { NXP_OFF_RECORD_BYTES = 896, NXP_OFF_RECORD_PAGES = 14 };
typedef enum { NXP_OK = 0, NXP_INVALID, NXP_BAD_PACKET, NXP_EXPIRED, NXP_NO_REPLY } nxp_result;

/* Original application state. All commands are RAM-only. */
typedef struct {
    uint8_t owner[6], claimed, name_size, name[NXP_NAME_MAX], connection_count;
    uint8_t effect, rgb[3], custom_rgb[3], rgb_brightness, white_brightness, boot_requested, trial_confirmed, image_role;
    uint8_t off_profile, off_requested, off_owner[6];
    const volatile uint8_t *off_snapshot;
    uint16_t temperature_k;
    uint32_t revision, part_id, trial_started_ms, capabilities, reset_cause;
} nxp_state;

typedef struct {
    uint16_t red_match, green_match, blue_match;
    uint16_t cool_match, warm_match;
} nxp_pwm_frame;

/* Dark initial state. Unknown effect modes and persistence are not implemented;
 * supported effect IDs are off(0), Static(1), custom(8). Owned00/04 value1
 * requests resident recovery, deferred until its entire reply is consumed. */
void nxp_state_init(nxp_state *state);
int nxp_state_valid(const nxp_state *state);
/* Publish actual completed platform setup, never assumed compile-time support.
 * Boolean inputs must be 0/1. Lighting requires recovery and excludes an SPI
 * trial. Invalid input leaves state untouched. reset_cause is a sticky hardware
 * snapshot, not a unique boot identifier. Wire getter 00/FC is documented below. */
int nxp_state_platform(nxp_state *state, int spi_trial, int recovery_ready,
                       int lighting_ready, uint32_t reset_cause);
/* Fixed off-only diagnostic profile. Record storage is caller-owned for the
 * entire state lifetime; updates share the parser's interrupt exclusion.
 * OFF1 is single-use: 0 unused,1 awaiting full ACK,2 consumed ACK,3 spent.
 * No pointer/address is accepted over the wire; F1 selects one of14 pages. */
int nxp_state_off_trial(nxp_state *state, const volatile uint8_t record[NXP_OFF_RECORD_BYTES]);
/* Read-only, owner-exempt 00/FC, no arguments: 24-byte ABI 1.0, big-endian words.
 * "OKLC", major 1, minor 0, role, flags (confirmed=1, boot-requested=2),
 * capabilities32, observed part32, uptime_ms32, sticky reset_cause32.
 * Controllers must never automatically confirm role 1 diagnostic images. */
/* A kind-0 report request produces a kind-0 reply. A nine-byte kind-11
 * connection event claims an unowned first connection or releases its matching
 * owner. Ownership changes produce a kind-4 notification; other valid events
 * return NXP_NO_REPLY without writing the response buffer; the link still
 * acknowledges them with READY and a zero-length response. Connection events
 * never change lighting or confirm a trial. Invalid framing/checksum
 * produces no reply and leaves state/output untouched. Valid unsupported
 * commands receive status 5; bad parameters status 3; ownership denial 8. */
nxp_result nxp_process(nxp_state *state, const uint8_t *request, size_t size,
                       uint8_t *reply, size_t capacity);
nxp_result nxp_process_at(nxp_state *state, const uint8_t *request, size_t size,
                          uint8_t *reply, size_t capacity, uint32_t now_ms);
/* Owner-confirm00/FD requires a ready lighting role and four bytes "OKLC"
 * before this 30s deadline. Diagnostic images cannot be confirmed.
 * A stable SPI identity is authorization policy, not cryptographic identity. */
int nxp_trial_expired(const nxp_state *state, uint32_t now_ms);
/* Pure bounded transfer function. No GPIO/register writes. False qualification
 * or invalid state always produces dark compare values. */
void nxp_render(const nxp_state *state, int qualified, nxp_pwm_frame *frame);

typedef enum { NXP_LINK_REQUEST, NXP_LINK_LENGTH, NXP_LINK_BODY } nxp_link_phase;
typedef struct {
    nxp_state *state;
    nxp_link_phase phase;
    uint32_t prepared_ms;
    uint8_t response[NXP_SPI_SIZE];
    uint8_t recovery_ready, response_size;
} nxp_link;

/* Portable completed-CS transaction model. Platform FIFO/ISR integration must
 * separately qualify the chip-select/ready electrical timing before use. */
void nxp_link_init(nxp_link *link, nxp_state *state);
/* Discards an incomplete reply and cancels any unacknowledged recovery request. */
void nxp_link_cancel(nxp_link *link);
int nxp_link_ready(const nxp_link *link);
nxp_result nxp_link_expire(nxp_link *link, uint32_t now_ms);
nxp_result nxp_link_transaction(nxp_link *link, const uint8_t *tx, size_t size,
                                uint8_t *rx, size_t capacity, uint32_t now_ms);

#endif
