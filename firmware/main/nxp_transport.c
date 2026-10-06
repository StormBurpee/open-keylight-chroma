#include "nxp_transport.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

enum { PIN_READY = 4, PIN_MOSI = 12, PIN_MISO = 13, PIN_CLOCK = 14, PIN_SELECT = 15 };
typedef struct {
    spi_device_handle_t spi;
    SemaphoreHandle_t mutex;
    unsigned phase;
    size_t pending_length;
} nxp_bus;
static nxp_bus bus;

static uint64_t now_us(void *unused) { (void)unused; return esp_timer_get_time(); }
static TickType_t ticks_left(uint64_t deadline) {
    uint64_t now = esp_timer_get_time();
    return now >= deadline ? 0 : pdMS_TO_TICKS((deadline - now + 999) / 1000);
}
static okl_result lock_bus(void *context, uint64_t deadline) {
    nxp_bus *b = context;
    return xSemaphoreTake(b->mutex, ticks_left(deadline)) == pdTRUE ? OKL_OK : OKL_TIMEOUT;
}
static void unlock_bus(void *context) { xSemaphoreGive(((nxp_bus *)context)->mutex); }

static okl_result wait_level(int level, uint64_t deadline) {
    while (gpio_get_level(PIN_READY) != level) {
        if (!ticks_left(deadline)) return OKL_TIMEOUT;
        vTaskDelay(1);
    }
    return OKL_OK;
}
static okl_result arm_ready(void *context, uint64_t deadline) {
    nxp_bus *b = context;
    if (b->phase) return OKL_NEEDS_RECOVERY;
    return wait_level(1, deadline);
}
static okl_result wait_ready(void *unused, uint64_t deadline) { (void)unused; return wait_level(0, deadline); }

static okl_result transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t size, uint64_t deadline) {
    nxp_bus *b = context;
    if (!size || size > OKL_SPI_LIMIT || !ticks_left(deadline)) return OKL_TIMEOUT;
    spi_transaction_t transaction = {.length = size * 8, .tx_buffer = tx, .rx_buffer = rx};
    esp_err_t result = spi_device_polling_start(b->spi, &transaction, ticks_left(deadline));
    if (result != ESP_OK) return result == ESP_ERR_TIMEOUT ? OKL_TIMEOUT : OKL_IO;
    /* A hardware transaction is at most 3.84ms. Never return live DMA buffers. */
    result = spi_device_polling_end(b->spi, portMAX_DELAY);
    if (result != ESP_OK) return OKL_IO;
    if (b->phase == 0) b->phase = 1;
    else if (b->phase == 1) { b->pending_length = (size_t)rx[0] * 256 + rx[1]; b->phase = 2; }
    else { b->phase = 0; b->pending_length = 0; }
    return ticks_left(deadline) ? OKL_OK : OKL_TIMEOUT;
}

static okl_result recover(void *context, uint64_t deadline) {
    nxp_bus *b = context;
    uint8_t tx[OKL_SPI_LIMIT] = {0}, rx[OKL_SPI_LIMIT];
    /* A response can predate ESP startup. READY-low means its length is pending. */
    if (b->phase == 0 && gpio_get_level(PIN_READY) == 0) b->phase = 1;
    if (b->phase == 1) {
        okl_result result = wait_level(0, deadline);
        if (result != OKL_OK) return result;
        result = transfer(context, tx, rx, 2, deadline);
        if (result != OKL_OK) return result;
    }
    if (b->phase == 2) {
        if (b->pending_length < 7 || b->pending_length > OKL_SPI_LIMIT) return OKL_PROTOCOL;
        okl_result result = transfer(context, tx, rx, b->pending_length, deadline);
        if (result != OKL_OK) return result;
    }
    return wait_level(1, deadline);
}

esp_err_t app_nxp_transport_init(okl_nxp *driver, const uint8_t mac[6]) {
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
        .arm_ready = arm_ready, .wait_ready = wait_ready, .transfer = transfer, .recover = recover};
    return okl_nxp_init(driver, &transport, mac) == OKL_OK ? ESP_OK : ESP_FAIL;
}
