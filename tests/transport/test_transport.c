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
        CHECK(okl_report_decode(&report, tx + 7, 90) == OKL_OK && report.command_class == 0 && report.opcode == 0x87);
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
int main(void) {
    test_normal_and_zero(); test_zero_wait_preserves_boundary(); test_zero_length_crosses_deadline();
    test_startup_zero(); test_zero_completion_does_not_clock(); test_retained_stale_body();
    test_stock_delayed_reply(); test_expired_original_remains_ambiguous(); test_bad_length_never_drains_or_clears();
    test_deadline_after_consumed_body(); test_deadline_after_consumed_request(); test_retry_length_after_start_rejected();
    test_end_error_keeps_unknown_phase(); test_start_error_has_not_clocked_wire();
    test_idf_rejects_finite_polling_start(); test_deadline_and_mutex();
    printf("native transport: %u checks across %u cases passed\n", checks, cases); return 0;
}
