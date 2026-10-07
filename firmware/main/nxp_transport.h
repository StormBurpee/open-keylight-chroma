#ifndef KEYLIGHT_NXP_TRANSPORT_H
#define KEYLIGHT_NXP_TRANSPORT_H
#include "okl_nxp.h"
#include "okl_loader.h"
#include "esp_err.h"
esp_err_t app_nxp_transport_init(okl_nxp *driver, const uint8_t mac[6]);
/* Only the SPI worker may hold this logical updater lease. Ordinary worker
 * dispatch/health must remain suspended; individual calls still take the bus
 * mutex. The lease does not clear any pre-existing ambiguous wire state. */
okl_result app_nxp_loader_acquire(okl_nxp *driver, uint32_t lease_id, uint64_t deadline_us);
void app_nxp_loader_release(okl_nxp *driver, uint32_t lease_id);
/* Caller must first prove a fresh diagnostic recovery reset, uninterrupted
 * silence through its deadline, and exact resident Info under this lease.
 * This stores only that caller's volatile evidence, not a generic reset proof.
 * Any attempted SPI transfer invalidates it. It cannot clear an unknown phase
 * and never survives ESP startup. A new explicitly admitted job consumes it. */
okl_result app_nxp_loader_preserve_resident(okl_nxp *driver, uint32_t lease_id, uint64_t deadline_us);
okl_result app_nxp_loader_use_resident(okl_nxp *driver, uint32_t lease_id,
    uint32_t proof_job_id, uint64_t deadline_us);
/* Caller first freshly qualifies the exact source and ownership. Legacy1.3
 * resets without a reply; original resets only after its ACK body is consumed.
 * Both arm the typed 0x84 entry boundary, then require the same quiet guard. */
okl_result app_nxp_loader_enter(okl_nxp *driver, uint32_t lease_id,
    okl_loader_source source, okl_loader_delivery *delivery, uint64_t deadline_us);
okl_result app_nxp_loader_exchange(okl_nxp *driver, uint32_t lease_id,
    const uint8_t request[90], uint8_t response[90], okl_loader_delivery *delivery, uint64_t deadline_us);
okl_result app_nxp_loader_send_only(okl_nxp *driver, uint32_t lease_id,
    const uint8_t request[90], okl_loader_delivery *delivery, uint64_t deadline_us);
/* One-shot expected reset boundary; only a fully sent known-loader End/Abort
 * may arm it. Three seconds of silence and READY-high are additionally needed.
 * An unknown DMA outcome can never be cleared by this API. */
okl_result app_nxp_loader_reset_boundary(okl_nxp *driver, uint32_t lease_id,
    uint8_t opcode, okl_loader_delivery delivery, uint64_t deadline_us);
#endif
