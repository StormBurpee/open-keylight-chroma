#include "transport_mocks.h"
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/nxp_transport.c"

static unsigned checks, cases;
#define CHECK(value) do { ++checks; if (!(value)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); exit(1); } } while (0)
enum { SCRIPT_LIMIT = 16 };
typedef struct {
    size_t size;
    unsigned request;
    uint8_t response[OKL_SPI_LIMIT];
    uint64_t duration, ready_delay;
    int ready_after;
    esp_err_t start_result, end_result;
} spi_step;
static spi_step script[SCRIPT_LIMIT];
static unsigned count, used, locked, starts, ends, delays, start_calls, wire_starts, gpio_reads;
static unsigned deny_mutex, fail_allocation;
static uint64_t time_us, ready_change;
static int ready, future_ready;
static spi_transaction_t *in_flight;
static spi_step *active_step;
static okl_nxp driver;
static const uint8_t identity[6] = {2, 1, 2, 3, 4, 5};

int64_t esp_timer_get_time(void) { return (int64_t)time_us; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return fail_allocation ? NULL : &locked; }
int xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks) {
    CHECK(handle == &locked && !locked && ticks && ticks != portMAX_DELAY);
    if (deny_mutex) { time_us += (uint64_t)ticks * 1000; return 0; }
    locked = 1; return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t handle) { CHECK(handle == &locked && locked == 1); locked = 0; }
void vTaskDelay(TickType_t ticks) { CHECK(locked && ticks == 1); ++delays; time_us += 1000; CHECK(delays < 1000); }
int gpio_get_level(unsigned pin) {
    CHECK(pin == 4 && locked);
    ++gpio_reads;
    if (time_us >= ready_change) { ready = future_ready; ready_change = UINT64_MAX; }
    return ready;
}
esp_err_t gpio_config(const gpio_config_t *config) {
    CHECK(config->pin_bit_mask == (UINT64_C(1) << 4) && config->mode == GPIO_MODE_INPUT);
    CHECK(!config->pull_up_en && !config->pull_down_en && !config->intr_type); return ESP_OK;
}
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma) {
    CHECK(host == SPI2_HOST && dma == SPI_DMA_CH_AUTO);
    CHECK(config->mosi_io_num == 12 && config->miso_io_num == 13 && config->sclk_io_num == 14);
    CHECK(config->quadwp_io_num == -1 && config->quadhd_io_num == -1 && config->max_transfer_sz == 480);
    return ESP_OK;
}
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config, spi_device_handle_t *device) {
    CHECK(host == SPI2_HOST && config->clock_speed_hz == 1000000 && config->mode == 0);
    CHECK(config->spics_io_num == 15 && config->queue_size == 1 && config->cs_ena_posttrans == 3);
    *device = &script; return ESP_OK;
}
esp_err_t spi_device_polling_start(spi_device_handle_t device, spi_transaction_t *transaction, TickType_t ticks) {
    ++start_calls;
    /* Installed Espressif IDF 5.5.5 rejects finite waits before wire setup. */
    if (ticks != portMAX_DELAY) return ESP_ERR_INVALID_ARG;
    CHECK(device == &script && locked && !in_flight && used < count);
    active_step = &script[used++]; ++starts;
    CHECK(transaction->length == active_step->size * 8 && transaction->tx_buffer && transaction->rx_buffer);
    const uint8_t *tx = transaction->tx_buffer;
    if (active_step->request) {
        okl_report report;
        CHECK(active_step->size == 97 && !memcmp(tx, identity, 6) && tx[6] == 0);
        CHECK(okl_report_decode(&report, tx + 7, 90) == OKL_OK);
        if (active_step->request == 1) CHECK(report.command_class == 0 && report.opcode == 0x87);
        else if (active_step->request == 2) CHECK(report.command_class == 0x10 && report.transaction == 0);
        else CHECK(!report.command_class && report.opcode == 4 && !report.transaction && report.arguments[0] == 1 &&
            report.size == (active_step->request == 3 ? 1 : 2) && !report.arguments[1]);
    } else for (size_t i = 0; i < active_step->size; ++i) CHECK(tx[i] == 0);
    if (active_step->start_result != ESP_OK) return active_step->start_result;
    in_flight = transaction; ++wire_starts; return ESP_OK;
}
esp_err_t spi_device_polling_end(spi_device_handle_t device, TickType_t ticks) {
    CHECK(device == &script && locked && ticks == portMAX_DELAY && in_flight);
    ++ends; time_us += active_step->duration;
    memcpy(in_flight->rx_buffer, active_step->response, active_step->size);
    in_flight = NULL;
    if (active_step->ready_after >= 0) {
        if (active_step->ready_delay) {
            future_ready = active_step->ready_after; ready_change = time_us + active_step->ready_delay;
        } else { ready = active_step->ready_after; ready_change = UINT64_MAX; }
    }
    return active_step->end_result;
}

static void reset(void) {
    ++cases; memset(&bus, 0, sizeof(bus)); memset(&driver, 0, sizeof(driver)); memset(script, 0, sizeof(script));
    count = used = locked = starts = ends = delays = deny_mutex = fail_allocation = 0;
    start_calls = wire_starts = gpio_reads = 0;
    time_us = 0; ready = 1; ready_change = UINT64_MAX; in_flight = NULL; active_step = NULL;
    CHECK(app_nxp_transport_init(&driver, identity) == ESP_OK);
}
static spi_step *step(size_t size, unsigned request, int level) {
    CHECK(count < SCRIPT_LIMIT); spi_step *s = &script[count++];
    s->size = size; s->request = request; s->ready_after = level; s->duration = 100; return s;
}
static void length_step(unsigned length, int level) {
    spi_step *s = step(2, 0, level); s->response[0] = (uint8_t)(length >> 8); s->response[1] = (uint8_t)length;
}
static void valid_getter(void) {
    step(97, 1, 0); length_step(97, 0);
    spi_step *s = step(97, 0, 1); memcpy(s->response, identity, 6);
    const uint8_t version[4] = {0, 1, 0, 0};
    CHECK(okl_report_encode(s->response + 7, (uint8_t)(driver.transaction + 1), 0, 0x87, version, 4) == OKL_OK);
    s->response[7] = 2;
}
static okl_result execute(uint64_t timeout) {
    okl_request request; okl_reply reply;
    CHECK(okl_request_get(&request, OKL_GET_FIRMWARE) == OKL_OK);
    okl_result result = okl_nxp_execute(&driver, &request, &reply, time_us + timeout);
    CHECK(!locked && !in_flight);
    if (result == OKL_OK) {
        okl_firmware_version version;
        CHECK(reply.sent && reply.received && reply.acknowledged);
        CHECK(okl_reply_decode_firmware(&version, &reply) == OKL_OK && version.component[1] == 1);
    }
    return result;
}
static okl_result recovery(uint64_t timeout) {
    okl_result result = okl_nxp_recover(&driver, time_us + timeout);
    CHECK(!locked && !in_flight); return result;
}
static void consumed(void) {
    CHECK(used == count && starts == count && !in_flight && !locked);
    CHECK(wire_starts == ends);
}
static void next_getter_works(void) { valid_getter(); CHECK(execute(10000) == OKL_OK && !driver.needs_recovery); consumed(); }

static void test_normal_and_zero(void) {
    reset(); next_getter_works();
    reset(); step(97, 1, 0); length_step(0, 1);
    CHECK(execute(10000) == OKL_PROTOCOL && driver.needs_recovery);
    CHECK(execute(10000) == OKL_NEEDS_RECOVERY && starts == 2);
    CHECK(recovery(10000) == OKL_OK && !driver.needs_recovery && starts == 2);
    next_getter_works();
}
static void test_zero_wait_preserves_boundary(void) {
    reset(); step(97, 1, 0); length_step(0, 0);
    CHECK(execute(10000) == OKL_PROTOCOL);
    CHECK(recovery(3000) == OKL_TIMEOUT && driver.needs_recovery && starts == 2);
    CHECK(execute(10000) == OKL_NEEDS_RECOVERY && starts == 2);
    ready_change = time_us + 2000; future_ready = 1;
    CHECK(recovery(3000) == OKL_OK && starts == 2 && !driver.needs_recovery);
    next_getter_works();
}
static void test_zero_length_crosses_deadline(void) {
    reset(); step(97, 1, 0); length_step(0, 1); script[1].duration = 5000;
    CHECK(execute(2000) == OKL_TIMEOUT && driver.needs_recovery && time_us == 5100);
    CHECK(recovery(1000) == OKL_OK && starts == 2); next_getter_works();
}
static void test_startup_zero(void) {
    reset(); ready = 0; driver.needs_recovery = 1; length_step(0, 1);
    CHECK(recovery(10000) == OKL_OK && starts == 1); next_getter_works();
}
static void test_zero_completion_does_not_clock(void) {
    reset(); step(97, 1, 0); length_step(0, 0); CHECK(execute(10000) == OKL_PROTOCOL);
    uint8_t tx[2] = {0}, rx[2] = {0};
    CHECK(driver.transport.transfer(driver.transport.user, tx, rx, 2, time_us + 1000) == OKL_NEEDS_RECOVERY);
    CHECK(starts == 2 && driver.needs_recovery); consumed();
}
static void test_retained_stale_body(void) {
    const unsigned lengths[] = {7, 97, 480};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        reset(); ready = 0; driver.needs_recovery = 1;
        length_step(lengths[i], 0); step(lengths[i], 0, 1);
        CHECK(recovery(10000) == OKL_OK && starts == 2); next_getter_works();
    }
}
static void test_stock_delayed_reply(void) {
    reset(); spi_step *request = step(97, 1, 0); request->ready_delay = 5000;
    CHECK(execute(2000) == OKL_TIMEOUT && driver.needs_recovery && starts == 1);
    CHECK(execute(1000) == OKL_NEEDS_RECOVERY && starts == 1);
    length_step(97, 0); step(97, 0, 1);
    CHECK(recovery(10000) == OKL_OK && time_us >= 5100 && starts == 3);
    next_getter_works();
}
static void test_expired_original_remains_ambiguous(void) {
    reset(); step(97, 1, 1);
    CHECK(execute(2000) == OKL_TIMEOUT && driver.needs_recovery);
    /* The peer may have expired during an ESP scheduling gap. No observed
     * length and no identity-specific expiry contract establish known idle. */
    time_us += 150000;
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t before = time_us;
        CHECK(recovery(3000) == OKL_TIMEOUT && driver.needs_recovery && starts == 1);
        CHECK(time_us == before + 3000);
        CHECK(execute(1000) == OKL_NEEDS_RECOVERY && starts == 1);
    }
    consumed();
}
static void test_bad_length_never_drains_or_clears(void) {
    const unsigned lengths[] = {1, 2, 3, 4, 5, 6, 481, 65535};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        reset(); step(97, 1, 0); length_step(lengths[i], 0);
        CHECK(execute(10000) == OKL_PROTOCOL && driver.needs_recovery);
        CHECK(recovery(10000) == OKL_PROTOCOL && starts == 2);
        ready = 1;
        CHECK(recovery(10000) == OKL_PROTOCOL && driver.needs_recovery && starts == 2);
        consumed();
    }
}
static void test_deadline_after_consumed_body(void) {
    reset(); valid_getter(); script[2].duration = 3000;
    CHECK(execute(2000) == OKL_TIMEOUT && driver.needs_recovery && ends == 3);
    CHECK(recovery(1000) == OKL_OK && starts == 3); next_getter_works();
}
static void test_deadline_after_consumed_request(void) {
    reset(); step(97, 1, 0); script[0].duration = 3000;
    CHECK(execute(2000) == OKL_TIMEOUT && driver.needs_recovery && ends == 1 && time_us == 3000);
    length_step(97, 0); step(97, 0, 1);
    CHECK(recovery(1000) == OKL_OK && starts == 3 && time_us == 3200);
    next_getter_works();
}
static void test_retry_length_after_start_rejected(void) {
    reset(); step(97, 1, 0); length_step(0, 0); script[1].start_result = ESP_ERR_TIMEOUT;
    CHECK(execute(10000) == OKL_TIMEOUT && driver.needs_recovery && ends == 1);
    length_step(0, 1);
    CHECK(recovery(10000) == OKL_OK && starts == 3 && ends == 2); next_getter_works();
}
static void test_end_error_keeps_unknown_phase(void) {
    for (unsigned at = 0; at < 3; ++at) {
        reset(); valid_getter(); script[at].end_result = ESP_ERR_INVALID_STATE;
        CHECK(execute(10000) == OKL_IO && driver.needs_recovery && starts == at + 1 && ends == at + 1);
        CHECK(wire_starts == at + 1);
        CHECK(execute(10000) == OKL_NEEDS_RECOVERY && starts == at + 1);
        /* A completed transaction may have consumed length or body. Neither
         * READY level establishes the missing phase; do not read more bytes. */
        unsigned reads = gpio_reads; uint64_t before = time_us;
        for (unsigned level = 0; level <= 1; ++level) {
            ready = (int)level;
            CHECK(recovery(10000) == OKL_NEEDS_RECOVERY && driver.needs_recovery);
            CHECK(time_us == before && gpio_reads == reads && starts == at + 1);
        }
        uint8_t tx[2] = {0}, rx[2] = {0};
        CHECK(driver.transport.transfer(driver.transport.user, tx, rx, 2, time_us + 1000) == OKL_NEEDS_RECOVERY);
        CHECK(starts == at + 1);
    }
}
static void test_start_error_has_not_clocked_wire(void) {
    const esp_err_t errors[] = {ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        for (unsigned at = 0; at < 3; ++at) {
            reset(); valid_getter(); script[at].start_result = errors[i];
            okl_result expected = errors[i] == ESP_ERR_TIMEOUT ? OKL_TIMEOUT : OKL_IO;
            CHECK(execute(10000) == expected && driver.needs_recovery);
            CHECK(starts == at + 1 && wire_starts == at && ends == at);
            CHECK(execute(10000) == OKL_NEEDS_RECOVERY && starts == at + 1);
            /* Replace only the rejected operation; recovery drains an existing
             * stale reply, never resends the failed command. */
            count = used;
            if (at == 1) length_step(97, 0);
            if (at) step(97, 0, 1);
            CHECK(recovery(10000) == OKL_OK && !driver.needs_recovery);
            CHECK(wire_starts == (at ? 3u : 0u));
            next_getter_works();
        }
    }
}
static void test_idf_rejects_finite_polling_start(void) {
    reset(); uint8_t tx[2] = {0}, rx[2] = {0};
    spi_transaction_t transaction = {.length = 16, .tx_buffer = tx, .rx_buffer = rx};
    CHECK(spi_device_polling_start(&script, &transaction, 100) == ESP_ERR_INVALID_ARG);
    CHECK(start_calls == 1 && !starts && !wire_starts && !ends && !in_flight);
    next_getter_works(); CHECK(start_calls == 4);
}
static void test_deadline_and_mutex(void) {
    reset(); CHECK(execute(0) == OKL_TIMEOUT && !starts);
    CHECK(recovery(0) == OKL_TIMEOUT && !starts);
    deny_mutex = 1; CHECK(execute(1000) == OKL_TIMEOUT && !starts && !locked);
    CHECK(recovery(1000) == OKL_TIMEOUT && !starts && !locked);
    deny_mutex = 0; next_getter_works();
    reset(); fail_allocation = 1; CHECK(app_nxp_transport_init(&driver, identity) == ESP_ERR_NO_MEM && !starts);
}
static void loader_report(uint8_t out[90], uint8_t opcode, const uint8_t *args, size_t size) {
    CHECK(okl_report_encode(out, 0, 0x10, opcode, args, size) == OKL_OK);
}
static void known_loader(void) {
    CHECK(app_nxp_loader_acquire(&driver, 17, time_us + 10000) == OKL_OK);
    uint8_t request[90], response[90], args[80] = {0}; okl_loader_delivery delivery;
    loader_report(request, 0x80, args, sizeof(args));
    step(97, 2, 0); length_step(97, 0);
    /* Actual resident envelope: zero routing tag, kind0, then the report. */
    spi_step *body = step(97, 0, 1);
    const uint8_t info[] = {3,0x18,1,2,0,0,0,2,0x5d}; memcpy(args, info, sizeof(info));
    loader_report(body->response + 7, 0x80, args, sizeof(args)); body->response[7] = 2;
    CHECK(app_nxp_loader_exchange(&driver, 17, request, response, &delivery, time_us + 10000) == OKL_OK);
    CHECK(delivery == OKL_LOADER_SENT_COMPLETE && okl_loader_information_valid(response));
    CHECK(bus.loader_known && bus.phase == BUS_IDLE && !driver.needs_recovery);
}
static void test_loader_lease_and_qualification(void) {
    reset(); uint8_t request[90]; okl_loader_delivery delivery;
    loader_report(request, 5, NULL, 0);
    CHECK(app_nxp_loader_send_only(&driver, 17, request, &delivery, 10000) == OKL_INVALID);
    CHECK(delivery == OKL_LOADER_NOT_SENT && !starts);
    CHECK(app_nxp_loader_acquire(&driver, 0, 10000) == OKL_INVALID);
    CHECK(app_nxp_loader_acquire(&driver, 17, 10000) == OKL_OK);
    CHECK(app_nxp_loader_acquire(&driver, 18, 10000) != OKL_OK);
    CHECK(app_nxp_loader_send_only(&driver, 17, request, &delivery, 10000) == OKL_INVALID && !starts);
    app_nxp_loader_release(&driver, 18); CHECK(bus.loader_lease == 17);
    app_nxp_loader_release(&driver, 17); CHECK(!bus.loader_lease);
    reset(); known_loader();
    CHECK(app_nxp_loader_send_only(&driver, 18, request, &delivery, time_us + 10000) == OKL_INVALID);
    request[8] ^= 1;
    CHECK(app_nxp_loader_send_only(&driver, 17, request, &delivery, time_us + 10000) == OKL_INVALID);
    loader_report(request, 2, NULL, 0);
    CHECK(app_nxp_loader_send_only(&driver, 17, request, &delivery, time_us + 10000) == OKL_INVALID);
    CHECK(starts == 3); consumed();
}
static void test_loader_envelopes_do_not_relax_application_identity(void) {
    for (unsigned kind = 0; kind < 256; ++kind) {
        for (unsigned tag = 0; tag < 3; ++tag) {
            reset(); CHECK(app_nxp_loader_acquire(&driver, 17, 10000) == OKL_OK);
            uint8_t request[90], response[90], args[80] = {0}; okl_loader_delivery delivery;
            loader_report(request, 0x80, args, sizeof(args));
            step(97, 2, 0); length_step(97, 1); spi_step *body = step(97, 0, 1);
            if (tag) memcpy(body->response, identity, 6);
            if (tag == 2) body->response[0] ^= 1;
            body->response[6] = (uint8_t)kind;
            const uint8_t info[] = {3,0x18,1,2,0,0,0,2,0x5d}; memcpy(args, info, sizeof(info));
            loader_report(body->response + 7, 0x80, args, sizeof(args)); body->response[7] = 2;
            unsigned accepted = (kind == 0 && tag != 2) || (kind == 4 && tag == 0);
            CHECK(app_nxp_loader_exchange(&driver, 17, request, response, &delivery, 10000) ==
                (accepted ? OKL_OK : OKL_PROTOCOL));
            CHECK(bus.loader_known == accepted && driver.needs_recovery == !accepted);
            CHECK(bus.phase == BUS_IDLE && delivery == OKL_LOADER_SENT_COMPLETE);
            if (accepted) CHECK(okl_loader_information_valid(response));
            consumed();
        }
    }
    for (unsigned bad = 0; bad < 3; ++bad) {
        reset(); valid_getter();
        if (!bad) memset(script[2].response, 0, 6);
        else if (bad == 1) script[2].response[0] ^= 1;
        else script[2].response[6] = 4;
        CHECK(execute(10000) != OKL_OK && driver.needs_recovery);
        consumed();
    }
    /* Original application entry ACKs retain their exact owner-tag contract. */
    reset(); CHECK(app_nxp_loader_acquire(&driver, 17, 10000) == OKL_OK);
    step(97, 3, 0); length_step(97, 1); spi_step *body = step(97, 0, 1);
    uint8_t yes = 1; CHECK(okl_report_encode(body->response + 7, 0, 0, 4, &yes, 1) == OKL_OK);
    body->response[7] = 2; okl_loader_delivery delivery;
    CHECK(app_nxp_loader_enter(&driver, 17, OKL_LOADER_FROM_ORIGINAL, &delivery, 10000) == OKL_PROTOCOL);
    CHECK(bus.phase == BUS_IDLE && !bus.reset_opcode && driver.needs_recovery); consumed();
}
static void test_expected_reset_is_one_shot(void) {
    for (uint8_t opcode = 4; opcode <= 5; ++opcode) {
        reset(); known_loader(); uint8_t request[90]; okl_loader_delivery delivery;
        loader_report(request, opcode, NULL, 0); step(97, 2, 1);
        CHECK(app_nxp_loader_send_only(&driver, 17, request, &delivery, time_us + 10000) == OKL_OK);
        CHECK(delivery == OKL_LOADER_SENT_COMPLETE && bus.phase == RESET_PENDING && driver.needs_recovery && starts == 4);
        CHECK(recovery(10000) == OKL_NEEDS_RECOVERY && starts == 4);
        CHECK(execute(10000) == OKL_NEEDS_RECOVERY && starts == 4);
        CHECK(app_nxp_loader_reset_boundary(&driver, 17, opcode, delivery, time_us + 10000) != OKL_OK);
        time_us += OKL_LOADER_QUIET_US;
        CHECK(app_nxp_loader_reset_boundary(&driver, 18, opcode, delivery, time_us + 10000) != OKL_OK);
        CHECK(app_nxp_loader_reset_boundary(&driver, 17, (uint8_t)(9-opcode), delivery, time_us + 10000) != OKL_OK);
        CHECK(app_nxp_loader_reset_boundary(&driver, 17, opcode, OKL_LOADER_MAYBE_SENT, time_us + 10000) != OKL_OK);
        ready = 0;
        CHECK(app_nxp_loader_reset_boundary(&driver, 17, opcode, delivery, time_us + 10000) != OKL_OK);
        CHECK(bus.phase == RESET_PENDING && driver.needs_recovery && starts == 4);
        ready = 1;
        CHECK(app_nxp_loader_reset_boundary(&driver, 17, opcode, delivery, time_us + 10000) == OKL_OK);
        CHECK(bus.phase == BUS_IDLE && !driver.needs_recovery && starts == 4);
        CHECK(app_nxp_loader_reset_boundary(&driver, 17, opcode, delivery, time_us + 10000) != OKL_OK);
        app_nxp_loader_release(&driver, 17); next_getter_works();
    }
}
static void test_reset_delivery_failures_stay_distinct(void) {
    for (unsigned failure = 0; failure < 3; ++failure) {
        reset(); known_loader(); uint8_t request[90]; okl_loader_delivery delivery;
        loader_report(request, 5, NULL, 0); spi_step *send = step(97, 2, 1);
        if (failure == 0) send->start_result = ESP_ERR_TIMEOUT;
        if (failure == 1) send->end_result = ESP_ERR_INVALID_STATE;
        if (failure == 2) send->duration = 20000;
        CHECK(app_nxp_loader_send_only(&driver, 17, request, &delivery, time_us + 10000) != OKL_OK);
        CHECK(delivery == (failure == 0 ? OKL_LOADER_NOT_SENT : failure == 1 ? OKL_LOADER_MAYBE_SENT : OKL_LOADER_SENT_COMPLETE));
        time_us += OKL_LOADER_QUIET_US;
        okl_result result = app_nxp_loader_reset_boundary(&driver, 17, 5, OKL_LOADER_SENT_COMPLETE, time_us + 10000);
        CHECK((result == OKL_OK) == (failure == 2));
        if (failure == 1) {
            CHECK(bus.phase == BUS_UNKNOWN && recovery(10000) == OKL_NEEDS_RECOVERY);
            app_nxp_loader_release(&driver, 17);
            CHECK(app_nxp_loader_acquire(&driver, 18, time_us + 10000) == OKL_NEEDS_RECOVERY);
        }
        CHECK(starts == 4 && !locked && !in_flight);
    }
}
static void test_unqualified_reply_cannot_arm_reset(void) {
    reset(); known_loader(); app_nxp_loader_release(&driver, 17);
    CHECK(app_nxp_loader_acquire(&driver, 18, time_us + 10000) == OKL_OK);
    uint8_t request[90], response[90], args[80] = {0}; okl_loader_delivery delivery;
    loader_report(request, 0x80, args, sizeof(args));
    step(97, 2, 0); length_step(97, 0); spi_step *body = step(97, 0, 1);
    memcpy(body->response, identity, 6); loader_report(body->response+7, 0x80, args, sizeof(args)); body->response[7] = 2;
    CHECK(app_nxp_loader_exchange(&driver, 18, request, response, &delivery, time_us + 10000) == OKL_OK);
    CHECK(!bus.loader_known);
    loader_report(request, 5, NULL, 0);
    CHECK(app_nxp_loader_send_only(&driver, 18, request, &delivery, time_us + 10000) == OKL_INVALID);
    CHECK(starts == 6); consumed();
}
static void test_typed_entry_boundary(void) {
    for (unsigned legacy = 0; legacy < 2; ++legacy) {
        reset(); CHECK(app_nxp_loader_acquire(&driver, 17, 10000) == OKL_OK);
        step(97, legacy ? 4 : 3, legacy ? 1 : 0);
        if (!legacy) {
            length_step(97, 0); spi_step *s = step(97, 0, 1); memcpy(s->response, identity, 6);
            uint8_t yes = 1; CHECK(okl_report_encode(s->response + 7, 0, 0, 4, &yes, 1) == OKL_OK); s->response[7] = 2;
        }
        okl_loader_delivery delivery;
        CHECK(app_nxp_loader_enter(&driver, 17, legacy ? OKL_LOADER_FROM_LEGACY_1_3 : OKL_LOADER_FROM_ORIGINAL,
            &delivery, 10000) == OKL_OK);
        CHECK(delivery == OKL_LOADER_SENT_COMPLETE && bus.phase == RESET_PENDING && bus.reset_opcode == 0x84);
        CHECK(starts == (legacy ? 1u : 3u)); /* No speculative legacy ACK clocks. */
        time_us += OKL_LOADER_QUIET_US;
        CHECK(app_nxp_loader_reset_boundary(&driver, 17, 4, delivery, time_us + 10000) != OKL_OK);
        CHECK(app_nxp_loader_reset_boundary(&driver, 17, 0x84, delivery, time_us + 10000) == OKL_OK);
        CHECK(!driver.needs_recovery); consumed();
    }
    reset(); CHECK(app_nxp_loader_acquire(&driver, 17, 10000) == OKL_OK);
    okl_loader_delivery delivery;
    CHECK(app_nxp_loader_enter(&driver, 17, OKL_LOADER_FROM_FRESH_RESIDENT, &delivery, 10000) == OKL_INVALID && !starts);
    step(97, 3, 0); length_step(97, 0); spi_step *s = step(97, 0, 1); memcpy(s->response, identity, 6);
    uint8_t no = 0; CHECK(okl_report_encode(s->response + 7, 0, 0, 4, &no, 1) == OKL_OK); s->response[7] = 2;
    CHECK(app_nxp_loader_enter(&driver, 17, OKL_LOADER_FROM_ORIGINAL, &delivery, 10000) == OKL_PROTOCOL);
    time_us += OKL_LOADER_QUIET_US;
    CHECK(app_nxp_loader_reset_boundary(&driver, 17, 0x84, delivery, time_us + 10000) != OKL_OK && driver.needs_recovery);
    consumed();
}
static void test_resident_proof_is_scoped_and_consumed(void) {
    reset(); known_loader();
    CHECK(app_nxp_loader_preserve_resident(&driver, 18, time_us + 10000) == OKL_INVALID);
    CHECK(app_nxp_loader_preserve_resident(&driver, 17, time_us + 10000) == OKL_OK);
    CHECK(bus.resident_proof_job_id == 17);
    CHECK(app_nxp_loader_preserve_resident(&driver, 17, time_us + 10000) != OKL_OK);
    app_nxp_loader_release(&driver, 17);
    CHECK(bus.resident_proof_job_id == 17);
    CHECK(app_nxp_loader_acquire(&driver, 18, time_us + 10000) == OKL_OK);
    CHECK(app_nxp_loader_use_resident(&driver, 18, 19, time_us + 10000) != OKL_OK);
    CHECK(app_nxp_loader_use_resident(&driver, 18, 17, time_us + 10000) == OKL_OK);
    CHECK(bus.loader_known && !bus.resident_proof_job_id);
    CHECK(app_nxp_loader_use_resident(&driver, 18, 17, time_us + 10000) != OKL_OK);
    CHECK(starts == 3); consumed();

    reset(); CHECK(app_nxp_loader_acquire(&driver, 17, 10000) == OKL_OK);
    CHECK(app_nxp_loader_preserve_resident(&driver, 17, 10000) != OKL_OK);
    CHECK(!bus.resident_proof_job_id && !starts);
    for (unsigned failure = 0; failure < 5; ++failure) {
        reset(); known_loader();
        CHECK(app_nxp_loader_preserve_resident(&driver, 17, time_us + 10000) == OKL_OK);
        app_nxp_loader_release(&driver, 17);
        if (failure < 2) {
            valid_getter();
            if (failure == 1) script[used].start_result = ESP_ERR_TIMEOUT;
            CHECK(execute(10000) == (failure ? OKL_TIMEOUT : OKL_OK));
        } else if (failure == 2) {
            CHECK(app_nxp_transport_init(&driver, identity) == ESP_OK);
        } else {
            CHECK(app_nxp_loader_acquire(&driver, 19, time_us + 10000) == OKL_OK);
            if (failure == 3) bus.phase = BUS_UNKNOWN;
            else driver.needs_recovery = 1;
            app_nxp_loader_release(&driver, 19);
        }
        CHECK(!bus.resident_proof_job_id);
        if (!driver.needs_recovery) {
            CHECK(app_nxp_loader_acquire(&driver, 18, time_us + 10000) == OKL_OK);
            CHECK(app_nxp_loader_use_resident(&driver, 18, 17, time_us + 10000) != OKL_OK);
        }
    }
    for (unsigned failure = 0; failure < 3; ++failure) {
        reset(); known_loader();
        if (!failure) ready = 0;
        if (failure == 1) bus.phase = BUS_UNKNOWN;
        if (failure == 2) driver.needs_recovery = 1;
        CHECK(app_nxp_loader_preserve_resident(&driver, 17, time_us + 10000) != OKL_OK);
        CHECK(!bus.resident_proof_job_id && starts == 3);
    }
    reset(); known_loader();
    CHECK(app_nxp_loader_preserve_resident(&driver, 17, time_us + 10000) == OKL_OK);
    app_nxp_loader_release(&driver, 17);
    CHECK(app_nxp_loader_acquire(&driver, 18, time_us + 10000) == OKL_OK);
    ready = 0;
    CHECK(app_nxp_loader_use_resident(&driver, 18, 17, time_us + 10000) != OKL_OK);
    ready = 1;
    CHECK(app_nxp_loader_use_resident(&driver, 18, 17, time_us + 10000) != OKL_OK);
    CHECK(!bus.resident_proof_job_id && starts == 3);
}
static void test_diagnostic_snapshot_never_clocks_or_clears(void) {
    app_nxp_transport_diagnostic snapshot;
    reset(); memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK(app_nxp_transport_snapshot(&driver, &snapshot, 10000) == OKL_OK);
    CHECK(snapshot.phase == APP_NXP_BUS_IDLE && snapshot.ready && !snapshot.has_report && !starts);
    for (unsigned i = 0; i < sizeof(snapshot.report); ++i) CHECK(!snapshot.report[i]);
    CHECK(app_nxp_transport_snapshot(NULL, &snapshot, 10000) == OKL_INVALID);
    CHECK(app_nxp_transport_snapshot(&driver, NULL, 10000) == OKL_INVALID);
    CHECK(app_nxp_transport_snapshot(&driver, &snapshot, 0) == OKL_TIMEOUT && !starts);

    for (unsigned failure = 0; failure < 4; ++failure) {
        reset(); valid_getter();
        if (failure == 0) script[2].response[0] ^= 1; /* Bad envelope remains inspectable. */
        if (failure == 1) script[2].response[95] ^= 1; /* Bad report checksum. */
        if (failure == 2) script[2].duration = 20000; /* Complete body, elapsed deadline. */
        if (failure == 3) script[2].end_result = ESP_ERR_INVALID_STATE; /* Uncertain DMA is not evidence. */
        CHECK(execute(10000) != OKL_OK && driver.needs_recovery);
        unsigned before = starts, phase = bus.phase;
        CHECK(app_nxp_transport_snapshot(&driver, &snapshot, time_us + 10000) == OKL_OK);
        CHECK(starts == before && bus.phase == phase && driver.needs_recovery);
        CHECK(snapshot.phase == (app_nxp_transport_phase)phase && snapshot.ready);
        CHECK(snapshot.has_report == (failure != 3));
        CHECK(snapshot.last_delivery == (failure == 3 ? OKL_LOADER_MAYBE_SENT : OKL_LOADER_SENT_COMPLETE));
        if (snapshot.has_report) {
            CHECK(!memcmp(snapshot.report, script[2].response + 7, 90));
            CHECK(!memcmp(snapshot.routing_tag, script[2].response, 6) && snapshot.reply_kind == script[2].response[6]);
        } else for (unsigned i = 0; i < sizeof(snapshot.report); ++i) CHECK(!snapshot.report[i]);
        consumed();
    }
    reset(); known_loader();
    CHECK(app_nxp_transport_snapshot(&driver, &snapshot, time_us + 10000) == OKL_OK);
    CHECK(snapshot.has_report && !snapshot.reply_kind && okl_loader_information_valid(snapshot.report));
    for (unsigned i = 0; i < 6; ++i) CHECK(!snapshot.routing_tag[i]);
    unsigned before = starts; deny_mutex = 1;
    CHECK(app_nxp_transport_snapshot(&driver, &snapshot, time_us + 1000) == OKL_TIMEOUT && starts == before);
    deny_mutex = 0; app_nxp_loader_release(&driver, 17);
    step(97, 1, 0)->start_result = ESP_ERR_TIMEOUT;
    CHECK(execute(10000) == OKL_TIMEOUT);
    CHECK(app_nxp_transport_snapshot(&driver, &snapshot, time_us + 10000) == OKL_OK);
    CHECK(!snapshot.has_report && snapshot.last_delivery == OKL_LOADER_NOT_SENT && driver.needs_recovery);
    for (unsigned i = 0; i < sizeof(snapshot.report); ++i) CHECK(!snapshot.report[i]);
    consumed();
}
int main(void) {
    test_normal_and_zero(); test_zero_wait_preserves_boundary(); test_zero_length_crosses_deadline();
    test_startup_zero(); test_zero_completion_does_not_clock(); test_retained_stale_body();
    test_stock_delayed_reply(); test_expired_original_remains_ambiguous(); test_bad_length_never_drains_or_clears();
    test_deadline_after_consumed_body(); test_deadline_after_consumed_request(); test_retry_length_after_start_rejected();
    test_end_error_keeps_unknown_phase(); test_start_error_has_not_clocked_wire();
    test_idf_rejects_finite_polling_start(); test_deadline_and_mutex();
    test_loader_lease_and_qualification(); test_expected_reset_is_one_shot();
    test_loader_envelopes_do_not_relax_application_identity();
    test_reset_delivery_failures_stay_distinct(); test_unqualified_reply_cannot_arm_reset();
    test_typed_entry_boundary();
    test_resident_proof_is_scoped_and_consumed();
    test_diagnostic_snapshot_never_clocks_or_clears();
    printf("native transport: %u checks across %u cases passed\n", checks, cases); return 0;
}
