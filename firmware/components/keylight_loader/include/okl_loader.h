#ifndef OKL_LOADER_H
#define OKL_LOADER_H

#include "okl_nxp.h"
#include <stddef.h>
#include <stdint.h>

enum { OKL_LOADER_BANK_BYTES = 28672, OKL_LOADER_HEADER_BYTES = 64,
       OKL_LOADER_PACKAGE_BYTES = 28736, OKL_LOADER_BLOCK_BYTES = 64,
       OKL_LOADER_BLOCKS = 448 };
#define OKL_LOADER_PART_ID UINT32_C(0x0000bc40)
#define OKL_LOADER_QUIET_US UINT64_C(3000000)

typedef enum {
    OKL_LOADER_OK, OKL_LOADER_INVALID, OKL_LOADER_BUSY, OKL_LOADER_CANCELLED,
    OKL_LOADER_TIMEOUT, OKL_LOADER_IO, OKL_LOADER_PROTOCOL, OKL_LOADER_VERIFY,
    OKL_LOADER_PERSIST, OKL_LOADER_UNRESOLVED
} okl_loader_result;

typedef enum {
    OKL_LOADER_NOT_SENT, OKL_LOADER_SENT_COMPLETE, OKL_LOADER_MAYBE_SENT
} okl_loader_delivery;

typedef enum {
    OKL_LOADER_FROM_ORIGINAL, OKL_LOADER_FROM_LEGACY_1_3,
    OKL_LOADER_FROM_FRESH_RESIDENT
} okl_loader_source;

typedef enum {
    OKL_LOADER_IDLE, OKL_LOADER_VALIDATED, OKL_LOADER_ENTERING,
    OKL_LOADER_READY, OKL_LOADER_ERASING, OKL_LOADER_PROGRAMMING,
    OKL_LOADER_VERIFYING, OKL_LOADER_COMMITTING, OKL_LOADER_QUIET,
    OKL_LOADER_OBSERVING, OKL_LOADER_APPLICATION_SEEN,
    OKL_LOADER_PRECOMMIT_FAILED, OKL_LOADER_COMMIT_UNRESOLVED
} okl_loader_phase;

typedef enum {
    OKL_LOADER_OBSERVATION_UNKNOWN, OKL_LOADER_OBSERVATION_RESIDENT,
    OKL_LOADER_OBSERVATION_APPLICATION
} okl_loader_observation_kind;

typedef struct {
    okl_loader_observation_kind kind;
    okl_firmware_version version;
    okl_controller_status controller;
    uint8_t dark_state_verified;
} okl_loader_observation;

typedef struct {
    const uint8_t *package; /* Caller keeps the entire package immutable through run(). */
    size_t size;
    uint8_t bank_sha256[32];
    okl_firmware_version version;
} okl_loader_image;

typedef struct {
    okl_loader_phase phase;
    okl_loader_result result;
    okl_loader_source source;
    uint8_t bank_sha256[32];
    uint16_t program_blocks_acked, readback_blocks_verified;
    uint8_t loader_verified, erase_attempted, complete_bank_verified;
    uint8_t commit_attempted, abort_attempted, quiet_completed;
    uint8_t reset_boundary_established, persistence_failed, cancelled_after_commit;
    okl_loader_delivery commit_delivery, abort_delivery;
    uint64_t quiet_started_us, quiet_finished_us;
    okl_loader_observation observation;
} okl_loader_audit;

typedef struct {
    void *user;
    int (*sha256)(void *user, const uint8_t *data, size_t size, uint8_t out[32]);
    uint64_t (*now_us)(void *user);
    /* Logical worker admission lease, not the driver's nonrecursive SPI mutex.
     * No animation, button, health or other controller job may run until end. */
    int (*begin)(void *user, uint64_t deadline_us);
    void (*end)(void *user);
    /* Persist intent before the next mutating phase. Failure blocks mutation.
     * On restart, recorded intent is diagnostic evidence, never a resume job. */
    int (*persist)(void *user, const okl_loader_audit *audit);
    /* Typed source validation + entry, under the lease. Original: qualified
     * FC/FE, owned dark state and one 00/04. Legacy: exact reviewed 1.3 entry.
     * Fresh resident: independently established fresh loader RAM state.
     * Never infer a source from timeout; never auto-retry entry mutations. */
    int (*enter_loader)(void *user, okl_loader_source source, uint64_t deadline_us);
    /* Exactly one report exchange. Adapter supplies stable SPI tag, framing
     * and phase tracking; response is the complete 90-byte report. A successful
     * transport result is not an ACK: core validates class/op/transaction/CRC.
     * delivery is populated even on error; unknown send outcomes are MAYBE. */
    int (*exchange)(void *user, const uint8_t request[90], uint8_t response[90],
                    okl_loader_delivery *delivery, uint64_t deadline_us);
    /* End/Abort have no reply. Do not run generic READY/recovery afterwards. */
    int (*send_only)(void *user, const uint8_t request[90],
                     okl_loader_delivery *delivery, uint64_t deadline_us);
    /* Must keep the lease and issue no SPI until now_us >= deadline_us.
     * It may return early; core repeats it. Cancellation cannot skip quiet. */
    void (*wait_until)(void *user, uint64_t deadline_us);
    int (*cancelled)(void *user);
    /* Explicit reset boundary after quiet. COMPLETE known no-reply operation
     * may establish a new phase using the reviewed transport contract.
     * MAYBE_SENT/DMA-unknown must stay quarantined; READY-high alone is not proof. */
    int (*reset_boundary)(void *user, uint8_t opcode, okl_loader_delivery delivery,
                          uint64_t deadline_us);
    /* Only read-only probes, after the reset boundary is established.
     * Never confirm FD, claim ownership, enter/reset, replay output or restore. */
    int (*observe_readonly)(void *user, okl_loader_observation *observation,
                            uint64_t deadline_us);
    void (*progress)(void *user, const okl_loader_audit *audit); /* Optional; no SPI. */
} okl_loader_ops;

/* Format1: 64-byte header followed by one exact bank. Header fields are BE:
 * 0..7="OKLCNXP\0",8:u16 format1,10:u16 header64,12:u32 bank28672,
 * 16:u32 partbc40,20:ABImajor1,21:minor0,22:role2,23:flags0,
 * 24..27:firmware version,28..59:bank SHA256,60..63:zero.
 * The package declares an original image; a digest is integrity,
 * not a signature or proof that the binary is electrically qualified. */
okl_loader_result okl_loader_prepare(okl_loader_image *out, const uint8_t *package,
                                    size_t size, const okl_loader_ops *ops);
/* Exact information reply qualification shared with a native SPI adapter. */
int okl_loader_information_valid(const uint8_t report[OKL_REPORT_BYTES]);
/* Revalidates the immutable package before I/O; deadline covers entry/program/
 * verify. Quiet is always additional and classification gets at most 10s.
 * OK means a matching original application was freshly observed dark; it does
 * NOT mean its trial was confirmed. Worker must perform its typed lifecycle
 * verification before FD and before enabling output. Any other result keeps
 * output disabled; no automatic replay/restore/resume is supported. */
okl_loader_result okl_loader_run(const okl_loader_image *image, okl_loader_source source,
                                const okl_loader_ops *ops, uint64_t deadline_us,
                                okl_loader_audit *audit);

#endif
