#ifndef OKL_NXP_H
#define OKL_NXP_H

#include <stddef.h>
#include <stdint.h>

/* Original implementation of a documented device interface. No vendor code. */
enum { OKL_REPORT_BYTES = 90, OKL_ARGUMENT_BYTES = 80, OKL_SPI_LIMIT = 480 };
#define OKL_DEFAULT_TIMEOUT_US UINT64_C(150000)

typedef enum {
    OKL_OK = 0, OKL_INVALID, OKL_BUSY, OKL_TIMEOUT, OKL_IO,
    OKL_PROTOCOL, OKL_REMOTE, OKL_OWNER_DENIED, OKL_NOT_OWNER,
    OKL_VERIFY, OKL_NEEDS_RECOVERY
} okl_result;

typedef enum {
    OKL_GET_FIRMWARE, OKL_GET_DEVICE_MODE, OKL_GET_OWNER,
    OKL_GET_EFFECT, OKL_GET_COLOR_BRIGHTNESS, OKL_GET_WHITE_BRIGHTNESS,
    OKL_GET_TEMPERATURE, OKL_SET_OWNER, OKL_SET_EFFECT,
    OKL_SET_COLOR_BRIGHTNESS, OKL_SET_WHITE_BRIGHTNESS,
    OKL_SET_TEMPERATURE, OKL_SET_FRAME, OKL_GET_PART_ID,
    OKL_GET_CONTROLLER_STATUS, OKL_CONFIRM_CONTROLLER,
    OKL_GET_DIAGNOSTIC_PROFILE, OKL_GET_DIAGNOSTIC_PAGE,
    OKL_RUN_DIAGNOSTIC_OFF, OKL_COMMAND_COUNT
} okl_command;

typedef struct {
    okl_command command;
    uint8_t size;
    uint8_t arguments[OKL_ARGUMENT_BYTES];
} okl_request;

typedef struct {
    uint8_t status, transaction, command_class, opcode, size;
    uint8_t arguments[OKL_ARGUMENT_BYTES];
} okl_report;

typedef struct {
    uint8_t sent;         /* Request transfer began; failure can be ambiguous. */
    uint8_t received;     /* A complete correlated report passed validation. */
    uint8_t acknowledged;/* Report status2; not state or optical verification. */
    uint8_t bridge_kind;
    okl_report report;
} okl_reply;

typedef struct {
    void *user;
    uint64_t (*now_us)(void *user);
    /* One mutex must cover every user of this physical SPI link. */
    okl_result (*lock)(void *user, uint64_t deadline_us);
    void (*unlock)(void *user);
    /* Establish idle ready-high and arm the falling-edge/level notification
     * BEFORE sending. Do not clear an edge after the request transfer. */
    okl_result (*arm_ready)(void *user, uint64_t deadline_us);
    okl_result (*wait_ready)(void *user, uint64_t deadline_us);
    /* Each call is a separate CS assertion; tx/rx are nonnull, size exact.
     * Callbacks use an absolute deadline and a monotonic clock. Do not begin
     * a transfer after expiry. Once started, finish DMA before returning so
     * these buffers remain valid; report OKL_TIMEOUT if completion is late.
     * The deadline cannot safely cancel in-flight DMA or bound a hardware hang. */
    okl_result (*transfer)(void *user, const uint8_t *tx, uint8_t *rx,
                           size_t size, uint64_t deadline_us);
    /* Optional, explicitly invoked recovery. Must establish a known idle
     * peer and discard stale replies. It must not silently flash/reset state.
     * Simply clearing a software event is NOT sufficient recovery. */
    okl_result (*recover)(void *user, uint64_t deadline_us);
} okl_transport;

typedef struct {
    okl_transport transport;
    uint8_t identity[6];   /* Fixed per initialized driver; normally ESP MAC. */
    uint8_t transaction;
    uint8_t needs_recovery;
} okl_nxp;

typedef struct {
    uint8_t claimed, identity[6], name_size, name[64];
} okl_owner;

typedef struct {
    uint8_t effect, flags, speed, color_count, colors[6];
    uint8_t color_brightness, white_brightness;
    uint16_t temperature_kelvin;
} okl_light_state;

typedef struct { uint8_t component[4]; } okl_firmware_version;

enum { OKL_CONTROLLER_STATUS_BYTES = 24, OKL_CONTROLLER_ABI_MAJOR = 1, OKL_CONTROLLER_ABI_MINOR = 0 };
enum { OKL_ROLE_UNQUALIFIED, OKL_ROLE_SPI_DIAGNOSTIC, OKL_ROLE_LIGHTING };
enum { OKL_CAP_RECOVERY_READY = 1, OKL_CAP_LIGHTING_READY = 2 };
typedef struct {
    uint8_t abi_major, abi_minor, role, trial_confirmed, boot_requested;
    uint32_t capabilities, part_id, uptime_ms, reset_cause;
} okl_controller_status;

enum { OKL_DIAGNOSTIC_PAGES = 14, OKL_DIAGNOSTIC_PAGE_BYTES = 64 };
typedef struct { uint8_t requested; uint16_t duration_ms; } okl_diagnostic_profile;

okl_result okl_request_build(okl_request *out, okl_command command,
                              const uint8_t *arguments, size_t size);
okl_result okl_request_get(okl_request *out, okl_command command);
okl_result okl_request_static(okl_request *out, const uint8_t rgb[3]);
okl_result okl_request_custom(okl_request *out);
okl_result okl_request_frame(okl_request *out, const uint8_t rgb[3]);
okl_result okl_request_color_brightness(okl_request *out, uint8_t brightness);
okl_result okl_request_white_brightness(okl_request *out, uint8_t brightness, uint8_t current_effect);
okl_result okl_request_temperature(okl_request *out, uint16_t kelvin);
/* Explicit lifecycle action; never issue based on firmware version alone. */
okl_result okl_request_confirm_controller(okl_request *out);
/* Fixed OFF1 bench profile only; no arbitrary opcode or address access. */
okl_result okl_request_diagnostic_page(okl_request *out, uint8_t page);
okl_result okl_request_diagnostic_off(okl_request *out);
okl_result okl_report_encode(uint8_t out[OKL_REPORT_BYTES], uint8_t transaction,
                             uint8_t command_class, uint8_t opcode,
                             const uint8_t *arguments, size_t size);
okl_result okl_report_decode(okl_report *out, const uint8_t *bytes, size_t size);
/* Decode only an acknowledged, correlated getter reply; outputs are unchanged
 * on failure. Version components retain wire order, without BCD conversion. */
okl_result okl_reply_decode_firmware(okl_firmware_version *out, const okl_reply *reply);
okl_result okl_reply_decode_mode(uint8_t *out, const okl_reply *reply);
okl_result okl_reply_decode_part_id(uint32_t *out, const okl_reply *reply);
/* FC ABI1.0: OKLC, major/minor/role/flags, then four big-endian words:
 * capabilities, part ID, uptime and sticky reset cause (not a unique boot ID).
 * Rejects unsupported ABI/reserved bits; output remains unchanged on failure. */
okl_result okl_reply_decode_controller_status(okl_controller_status *out, const okl_reply *reply);
/* An ACK alone does not verify confirmation; follow with a fresh FC getter. */
okl_result okl_reply_check_controller_confirmation(const okl_reply *reply);
okl_result okl_reply_decode_diagnostic_profile(okl_diagnostic_profile *out, const okl_reply *reply);
okl_result okl_reply_decode_diagnostic_page(uint8_t out[OKL_DIAGNOSTIC_PAGE_BYTES], uint8_t page, const okl_reply *reply);
okl_result okl_reply_check_diagnostic_off(const okl_reply *reply);
okl_result okl_nxp_init(okl_nxp *driver, const okl_transport *transport,
                        const uint8_t identity[6]);
/* Saturating now+150ms. Application may instead supply a longer bounded
 * deadline for a multi-request owner/state operation. */
uint64_t okl_nxp_default_deadline(const okl_nxp *driver);
/* No retries. A timeout or invalid response after send poisons the link;
 * caller must explicitly recover before another request can be transmitted. */
okl_result okl_nxp_execute(okl_nxp *driver, const okl_request *request,
                           okl_reply *reply, uint64_t deadline_us);
okl_result okl_nxp_recover(okl_nxp *driver, uint64_t deadline_us);
okl_result okl_nxp_get_owner(okl_nxp *driver, okl_owner *owner, uint64_t deadline_us);
/* Claim is an explicit caller action, never an automatic response to denial. */
okl_result okl_nxp_claim(okl_nxp *driver, const uint8_t *name, size_t name_size,
                         uint64_t deadline_us);
/* Query and release only our owner, then verify. Protocol has no atomic
 * compare-and-release; another physical SPI initiator would be a race. */
okl_result okl_nxp_release(okl_nxp *driver, uint64_t deadline_us);
/* Four separately verified getters under one lock/deadline. These are not an
 * atomic device snapshot; output remains untouched if any getter fails. */
okl_result okl_nxp_read_state(okl_nxp *driver, okl_light_state *state,
                              uint64_t deadline_us);

#endif
