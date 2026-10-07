#include "nxp_transport.h"
#include "flash_guard.h"
#include "update_indicator.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <string.h>

enum { PIN_READY = 4, PIN_MOSI = 12, PIN_MISO = 13, PIN_CLOCK = 14, PIN_SELECT = 15 };
enum { BUS_IDLE = APP_NXP_BUS_IDLE, LENGTH_PENDING = APP_NXP_LENGTH_PENDING,
    BODY_PENDING = APP_NXP_BODY_PENDING, ZERO_COMPLETE = APP_NXP_ZERO_COMPLETE,
    BUS_UNKNOWN = APP_NXP_BUS_UNKNOWN, RESET_PENDING = APP_NXP_RESET_PENDING };
typedef struct {
    spi_device_handle_t spi;
    SemaphoreHandle_t mutex;
    unsigned phase;
    size_t pending_length;
    uint32_t loader_lease;
    unsigned loader_known;
    uint32_t resident_proof_job_id;
    bool resident_information_seen;
    uint8_t reset_opcode;
    uint64_t reset_completed_us;
    okl_loader_delivery last_delivery;
    unsigned has_report;
    uint8_t last_body[97];
    bool flash_held;
} nxp_bus;
static nxp_bus bus;

static uint64_t now_us(void *unused) { (void)unused; return esp_timer_get_time(); }
static uint64_t wire_deadline(uint64_t deadline) {
    /* Published without app.mutex: an OTA may finish while admission waits.
     * Its reboot cutoff cannot be extended by a stale RECEIVING snapshot. */
    uint64_t reboot = app_update_reboot_deadline_us();
    return reboot && reboot < deadline ? reboot : deadline;
}
static TickType_t ticks_left(uint64_t deadline) {
    uint64_t now = esp_timer_get_time();
    return now >= deadline ? 0 : pdMS_TO_TICKS((deadline - now + 999) / 1000);
}
static okl_result lock_bus(void *context, uint64_t deadline) {
    nxp_bus *b = context;
    TickType_t ticks = ticks_left(deadline);
    return ticks && xSemaphoreTake(b->mutex, ticks) == pdTRUE ? OKL_OK : OKL_TIMEOUT;
}
static void unlock_bus(void *context) {
    nxp_bus *b = context;
    if (b->flash_held) { b->flash_held = false; app_flash_guard_leave(); }
    xSemaphoreGive(b->mutex);
}

static okl_result lock_bus_admitted(void *context, uint64_t *deadline, uint64_t hard_deadline) {
    nxp_bus *b = context;
    *deadline = wire_deadline(*deadline);
    if (hard_deadline && *deadline > hard_deadline) *deadline = hard_deadline;
    okl_result result = lock_bus(context, *deadline);
    if (result != OKL_OK) return result;
    /* This order is acyclic: flash writers never take the bus mutex; no SPI
     * callback acquires app.mutex while either lock is held. Only a new idle
     * exchange may exclude flash admission from its response-time budget. */
    bool idle = b->phase == BUS_IDLE && gpio_get_level(PIN_READY) == 1;
    uint64_t before = esp_timer_get_time();
    uint64_t admission = idle ? app_flash_guard_deadline() : *deadline;
    admission = wire_deadline(admission);
    if (hard_deadline && admission > hard_deadline) admission = hard_deadline;
    esp_err_t entered = app_flash_guard_enter(admission);
    if (entered != ESP_OK) {
        unlock_bus(context);
        return entered == ESP_ERR_TIMEOUT ? OKL_TIMEOUT : OKL_IO;
    }
    b->flash_held = true;
    uint64_t after = esp_timer_get_time();
    if (after < before || after - before > OKL_MAX_ADMISSION_WAIT_US ||
        (idle && *deadline > UINT64_MAX - (after - before))) {
        unlock_bus(context); return OKL_TIMEOUT;
    }
    if (idle) *deadline += after - before;
    if (hard_deadline && *deadline > hard_deadline) *deadline = hard_deadline;
    *deadline = wire_deadline(*deadline);
    if (!ticks_left(*deadline)) { unlock_bus(context); return OKL_TIMEOUT; }
    return OKL_OK;
}

static okl_result wait_level(int level, uint64_t deadline) {
    while (gpio_get_level(PIN_READY) != level) {
        if (!ticks_left(wire_deadline(deadline))) return OKL_TIMEOUT;
        vTaskDelay(1);
    }
    return OKL_OK;
}
static okl_result arm_ready(void *context, uint64_t deadline) {
    nxp_bus *b = context;
    if (b->phase != BUS_IDLE) return OKL_NEEDS_RECOVERY;
    return wait_level(1, deadline);
}
static okl_result wait_ready(void *unused, uint64_t deadline) { (void)unused; return wait_level(0, deadline); }

static okl_result transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t size, uint64_t deadline) {
    nxp_bus *b = context;
    deadline = wire_deadline(deadline);
    b->resident_proof_job_id = 0;
    b->resident_information_seen = false;
    b->last_delivery = OKL_LOADER_NOT_SENT;
    if (b->phase == BUS_IDLE) {
        b->has_report = 0;
        memset(b->last_body, 0, sizeof(b->last_body));
    }
    if (b->phase == ZERO_COMPLETE || b->phase == BUS_UNKNOWN || b->phase == RESET_PENDING) return OKL_NEEDS_RECOVERY;
    if (!size || size > OKL_SPI_LIMIT || !ticks_left(deadline)) return OKL_TIMEOUT;
    spi_transaction_t transaction = {.length = size * 8, .tx_buffer = tx, .rx_buffer = rx};
    /* IDF 5.5 only accepts portMAX_DELAY here. This host has one device and
     * every caller holds our deadline-bounded mutex; no other SPI user may
     * acquire it. Check the application deadline before and after wire I/O. */
    esp_err_t result = spi_device_polling_start(b->spi, &transaction, portMAX_DELAY);
    if (result != ESP_OK) return result == ESP_ERR_TIMEOUT ? OKL_TIMEOUT : OKL_IO;
    b->last_delivery = OKL_LOADER_MAYBE_SENT;
    /* At 1MHz the longest wire payload is 3.84ms. Scheduling or hardware
     * failure can exceed that; never return while DMA owns these buffers. */
    result = spi_device_polling_end(b->spi, portMAX_DELAY);
    if (result != ESP_OK) {
        /* IDF can report a DMA error after completing the transaction. The
         * received bytes cannot establish which peer phase was consumed. */
        b->phase = BUS_UNKNOWN;
        return OKL_IO;
    }
    b->last_delivery = OKL_LOADER_SENT_COMPLETE;
    if (b->phase == BODY_PENDING && size == sizeof(b->last_body)) {
        memcpy(b->last_body, rx, sizeof(b->last_body));
        b->has_report = 1;
    }
    if (b->phase == BUS_IDLE) b->phase = LENGTH_PENDING;
    else if (b->phase == LENGTH_PENDING) {
        b->pending_length = (size_t)rx[0] * 256 + rx[1];
        b->phase = b->pending_length ? BODY_PENDING : ZERO_COMPLETE;
    } else { b->phase = BUS_IDLE; b->pending_length = 0; }
    return ticks_left(wire_deadline(deadline)) ? OKL_OK : OKL_TIMEOUT;
}

static okl_result recover(void *context, uint64_t deadline) {
    nxp_bus *b = context;
    uint8_t tx[OKL_SPI_LIMIT] = {0}, rx[OKL_SPI_LIMIT];
    if (b->phase == BUS_UNKNOWN || b->phase == RESET_PENDING) return OKL_NEEDS_RECOVERY;
    /* A response can predate ESP startup. READY-low means its length is pending. */
    if (b->phase == BUS_IDLE && gpio_get_level(PIN_READY) == 0) b->phase = LENGTH_PENDING;
    if (b->phase == LENGTH_PENDING) {
        /* READY-high alone cannot distinguish an expired original-controller
         * response from a stock response still being prepared. Keep the phase. */
        okl_result result = wait_level(0, deadline);
        if (result != OKL_OK) return result;
        result = transfer(context, tx, rx, 2, deadline);
        if (result != OKL_OK) return result;
    }
    if (b->phase == ZERO_COMPLETE) {
        /* The length was consumed and there is no body to drain. Wait for the
         * peer to finish; retain this state on timeout instead of re-clocking
         * the length or declaring an unobserved idle state. */
        okl_result result = wait_level(1, deadline);
        if (result == OKL_OK) b->phase = BUS_IDLE;
        return result;
    }
    if (b->phase == BODY_PENDING) {
        if (b->pending_length < 7 || b->pending_length > OKL_SPI_LIMIT) return OKL_PROTOCOL;
        okl_result result = transfer(context, tx, rx, b->pending_length, deadline);
        if (result != OKL_OK) return result;
    }
    return wait_level(1, deadline);
}

static int loader_driver(const okl_nxp *driver) {
    return driver && driver->transport.user == &bus && driver->transport.transfer == transfer;
}

okl_result app_nxp_transport_snapshot(okl_nxp *driver, app_nxp_transport_diagnostic *out,
                                       uint64_t deadline) {
    if (!loader_driver(driver) || !out) return OKL_INVALID;
    if (!ticks_left(deadline)) return OKL_TIMEOUT;
    okl_result result = lock_bus(&bus, deadline);
    if (result != OKL_OK) return result;
    memset(out, 0, sizeof(*out));
    out->phase = (app_nxp_transport_phase)bus.phase;
    out->ready = gpio_get_level(PIN_READY) != 0;
    out->last_delivery = bus.last_delivery;
    out->has_report = bus.has_report;
    if (out->has_report) {
        memcpy(out->routing_tag, bus.last_body, 6);
        out->reply_kind = bus.last_body[6];
        memcpy(out->report, bus.last_body + 7, 90);
    }
    unlock_bus(&bus);
    return OKL_OK;
}

okl_result app_nxp_loader_acquire(okl_nxp *driver, uint32_t lease_id, uint64_t deadline) {
    if (!loader_driver(driver) || !lease_id) return OKL_INVALID;
    okl_result result = lock_bus(&bus, deadline);
    if (result != OKL_OK) return result;
    if (bus.loader_lease) result = OKL_BUSY;
    else if (bus.phase != BUS_IDLE || driver->needs_recovery) result = OKL_NEEDS_RECOVERY;
    else { bus.loader_lease = lease_id; bus.loader_known = 0; bus.reset_opcode = 0; }
    unlock_bus(&bus); return result;
}

void app_nxp_loader_release(okl_nxp *driver, uint32_t lease_id) {
    /* Called by the sole SPI worker after every in-flight callback returned.
     * Preserve an unresolved reset/DMA phase for later explicit recovery. */
    if (!loader_driver(driver) || !lease_id || bus.loader_lease != lease_id) return;
    bus.loader_lease = 0; bus.loader_known = 0; bus.reset_opcode = 0;
    if (bus.phase != BUS_IDLE || driver->needs_recovery) {
        bus.resident_proof_job_id = 0;
        bus.resident_information_seen = false;
        driver->needs_recovery = 1;
    }
}

okl_result app_nxp_loader_preserve_resident(okl_nxp *driver, uint32_t lease_id, uint64_t deadline) {
    if (!loader_driver(driver) || !lease_id || bus.loader_lease != lease_id) return OKL_INVALID;
    okl_result result = lock_bus(&bus, deadline);
    if (result != OKL_OK) return result;
    if (bus.phase != BUS_IDLE || driver->needs_recovery || !bus.loader_known ||
        !bus.resident_information_seen || gpio_get_level(PIN_READY) != 1) result = OKL_NEEDS_RECOVERY;
    else {
        bus.resident_proof_job_id = lease_id;
        bus.resident_information_seen = false;
    }
    unlock_bus(&bus); return result;
}

okl_result app_nxp_loader_use_resident(okl_nxp *driver, uint32_t lease_id,
                                      uint32_t proof_job_id, uint64_t deadline) {
    if (!loader_driver(driver) || !lease_id || !proof_job_id || lease_id == proof_job_id ||
        bus.loader_lease != lease_id) return OKL_INVALID;
    okl_result result = lock_bus(&bus, deadline);
    if (result != OKL_OK) return result;
    if (bus.resident_proof_job_id != proof_job_id) result = OKL_NEEDS_RECOVERY;
    else {
        bus.resident_proof_job_id = 0;
        if (bus.phase != BUS_IDLE || driver->needs_recovery || gpio_get_level(PIN_READY) != 1)
            result = OKL_NEEDS_RECOVERY;
        else bus.loader_known = 1;
    }
    unlock_bus(&bus); return result;
}

okl_result app_nxp_loader_enter(okl_nxp *driver, uint32_t lease_id,
                               okl_loader_source source, okl_loader_delivery *delivery, uint64_t deadline) {
    uint8_t tx[97] = {0}, rx[97], args[2] = {1, 0}; okl_report reply;
    if (delivery) *delivery = OKL_LOADER_NOT_SENT;
    if (!loader_driver(driver) || !delivery || !lease_id || bus.loader_lease != lease_id ||
        (source != OKL_LOADER_FROM_ORIGINAL && source != OKL_LOADER_FROM_LEGACY_1_3)) return OKL_INVALID;
    okl_result result = lock_bus_admitted(&bus, &deadline, 0);
    if (result != OKL_OK) return result;
    result = arm_ready(&bus, deadline);
    if (result != OKL_OK) goto finished_entry;
    memcpy(tx, driver->identity, 6);
    (void)okl_report_encode(tx + 7, 0, 0, 4, args, source == OKL_LOADER_FROM_ORIGINAL ? 1 : 2);
    result = transfer(&bus, tx, rx, 97, deadline); *delivery = bus.last_delivery;
    if (source == OKL_LOADER_FROM_LEGACY_1_3) {
        /* This exact legacy command resets synchronously; there is no ACK. */
        if (*delivery == OKL_LOADER_SENT_COMPLETE) goto reset_armed;
        goto finished_entry;
    }
    if (result != OKL_OK) goto finished_entry;
    result = wait_ready(&bus, deadline);
    if (result != OKL_OK) goto finished_entry;
    memset(tx, 0, sizeof(tx)); result = transfer(&bus, tx, rx, 2, deadline);
    if (result != OKL_OK) goto finished_entry;
    if (rx[0] || rx[1] != 97) { result = OKL_PROTOCOL; goto finished_entry; }
    result = transfer(&bus, tx, rx, 97, deadline);
    if (result != OKL_OK) goto finished_entry;
    if (rx[6] || memcmp(rx, driver->identity, 6) || okl_report_decode(&reply, rx + 7, 90) != OKL_OK ||
        reply.status != 2 || reply.transaction || reply.command_class || reply.opcode != 4 ||
        reply.size != 1 || reply.arguments[0] != 1) { result = OKL_PROTOCOL; goto finished_entry; }
reset_armed:
    bus.phase = RESET_PENDING; bus.reset_opcode = 0x84;
    bus.reset_completed_us = esp_timer_get_time(); bus.loader_known = 0;
finished_entry:
    if (result != OKL_OK || bus.phase == RESET_PENDING) driver->needs_recovery = 1;
    unlock_bus(&bus); return result;
}

static okl_result loader_request(okl_nxp *driver, uint32_t lease_id, const uint8_t request[90],
                                okl_loader_delivery *delivery, okl_report *report, int reset) {
    if (delivery) *delivery = OKL_LOADER_NOT_SENT;
    if (!loader_driver(driver) || !delivery || !request || !lease_id || bus.loader_lease != lease_id) return OKL_INVALID;
    if (okl_report_decode(report, request, 90) != OKL_OK || report->status || report->transaction || report->command_class != 0x10)
        return OKL_INVALID;
    if (reset) {
        if ((report->opcode != 4 && report->opcode != 5) || report->size || !bus.loader_known) return OKL_INVALID;
    } else if (report->opcode != 0x80 && report->opcode != 1 && report->opcode != 2 && report->opcode != 0x83)
        return OKL_INVALID;
    return OKL_OK;
}

okl_result app_nxp_loader_exchange(okl_nxp *driver, uint32_t lease_id, const uint8_t request[90],
                                  uint8_t response[90], okl_loader_delivery *delivery, uint64_t deadline) {
    okl_report report; uint8_t tx[97] = {0}, rx[97];
    okl_result result = loader_request(driver, lease_id, request, delivery, &report, 0);
    if (result != OKL_OK || !response) return result == OKL_OK ? OKL_INVALID : result;
    result = lock_bus_admitted(&bus, &deadline, 0);
    if (result != OKL_OK) return result;
    result = arm_ready(&bus, deadline);
    if (result != OKL_OK) goto finished;
    memcpy(tx, driver->identity, 6); memcpy(tx + 7, request, 90);
    result = transfer(&bus, tx, rx, 97, deadline); *delivery = bus.last_delivery;
    if (result != OKL_OK) goto finished;
    result = wait_ready(&bus, deadline);
    if (result != OKL_OK) goto finished;
    memset(tx, 0, sizeof(tx)); result = transfer(&bus, tx, rx, 2, deadline);
    if (result != OKL_OK) goto finished;
    if (rx[0] || rx[1] != 97) { result = OKL_PROTOCOL; goto finished; }
    result = transfer(&bus, tx, rx, 97, deadline);
    if (result != OKL_OK) goto finished;
    /* The resident loader returns kind0 with an all-zero routing tag; unlike
     * application replies, it does not echo the requester's identity. Accept
     * that exact envelope only on this lease-scoped loader path. The report
     * still passes the caller's command/correlation validation, and only the
     * exact Info signature below can qualify a loader for reset operations. */
    if ((rx[6] == 0 && memcmp(rx, driver->identity, 6) && memcmp(rx, (const uint8_t[6]){0}, 6)) ||
        (rx[6] == 4 && memcmp(rx, (const uint8_t[6]){0}, 6)) || (rx[6] != 0 && rx[6] != 4)) {
        result = OKL_PROTOCOL; goto finished;
    }
    memcpy(response, rx + 7, 90);
    if (report.opcode == 0x80 && report.size == 80) {
        unsigned zero = 1;
        for (unsigned i = 0; i < 80; ++i) if (report.arguments[i]) zero = 0;
        if (zero && okl_loader_information_valid(response)) {
            bus.loader_known = 1;
            bus.resident_information_seen = true;
        }
    }
finished:
    if (result != OKL_OK) driver->needs_recovery = 1;
    unlock_bus(&bus); return result;
}

okl_result app_nxp_loader_send_only(okl_nxp *driver, uint32_t lease_id, const uint8_t request[90],
                                   okl_loader_delivery *delivery, uint64_t deadline) {
    okl_report report; uint8_t tx[97] = {0}, rx[97];
    okl_result result = loader_request(driver, lease_id, request, delivery, &report, 1);
    if (result != OKL_OK) return result;
    result = lock_bus_admitted(&bus, &deadline, 0);
    if (result != OKL_OK) return result;
    result = arm_ready(&bus, deadline);
    if (result == OKL_OK) {
        memcpy(tx, driver->identity, 6); memcpy(tx + 7, request, 90);
        result = transfer(&bus, tx, rx, 97, deadline); *delivery = bus.last_delivery;
        if (*delivery == OKL_LOADER_SENT_COMPLETE) {
            bus.phase = RESET_PENDING; bus.reset_opcode = report.opcode;
            bus.reset_completed_us = esp_timer_get_time(); bus.loader_known = 0;
        }
    }
    if (result != OKL_OK || bus.phase == RESET_PENDING) driver->needs_recovery = 1;
    unlock_bus(&bus); return result;
}

okl_result app_nxp_loader_reset_boundary(okl_nxp *driver, uint32_t lease_id, uint8_t opcode,
                                        okl_loader_delivery delivery, uint64_t deadline) {
    if (!loader_driver(driver) || !lease_id || bus.loader_lease != lease_id) return OKL_INVALID;
    okl_result result = lock_bus(&bus, deadline);
    if (result != OKL_OK) return result;
    if (bus.phase != RESET_PENDING || bus.reset_opcode != opcode ||
        delivery != OKL_LOADER_SENT_COMPLETE ||
        (uint64_t)esp_timer_get_time() - bus.reset_completed_us < OKL_LOADER_QUIET_US) result = OKL_NEEDS_RECOVERY;
    else if (!ticks_left(deadline)) result = OKL_TIMEOUT;
    else if (gpio_get_level(PIN_READY) != 1) result = OKL_NEEDS_RECOVERY;
    else {
        bus.phase = BUS_IDLE; bus.pending_length = 0; bus.reset_opcode = 0;
        driver->needs_recovery = 0; result = OKL_OK;
    }
    unlock_bus(&bus); return result;
}

esp_err_t app_nxp_transport_init(okl_nxp *driver, const uint8_t mac[6]) {
    bus.has_report = 0;
    memset(bus.last_body, 0, sizeof(bus.last_body));
    bus.resident_proof_job_id = 0;
    bus.resident_information_seen = false;
    bus.mutex = xSemaphoreCreateMutex();
    if (!bus.mutex) return ESP_ERR_NO_MEM;
    gpio_config_t ready = {.pin_bit_mask = 1ULL << PIN_READY, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&ready));
    spi_bus_config_t config = {.mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLOCK, .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = OKL_SPI_LIMIT};
    esp_err_t result = spi_bus_initialize(SPI2_HOST, &config, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) return result;
    spi_device_interface_config_t device = {.clock_speed_hz = 1000000, .mode = 0,
        .spics_io_num = PIN_SELECT, .queue_size = 1, .cs_ena_posttrans = 3};
    result = spi_bus_add_device(SPI2_HOST, &device, &bus.spi);
    if (result != ESP_OK) return result;
    okl_transport transport = {.user = &bus, .now_us = now_us, .lock = lock_bus, .unlock = unlock_bus,
        .arm_ready = arm_ready, .wait_ready = wait_ready, .transfer = transfer, .recover = recover,
        .lock_with_admission = lock_bus_admitted};
    return okl_nxp_init(driver, &transport, mac) == OKL_OK ? ESP_OK : ESP_FAIL;
}
