#ifndef TRANSPORT_MOCKS_H
#define TRANSPORT_MOCKS_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef int esp_err_t;
typedef unsigned TickType_t;
typedef void *SemaphoreHandle_t;
typedef void *spi_device_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERROR_CHECK(value) do { if ((value) != ESP_OK) abort(); } while (0)
#define pdTRUE 1
#define pdMS_TO_TICKS(value) (value)
#define portMAX_DELAY UINT32_MAX
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define SPI2_HOST 2
#define SPI_DMA_CH_AUTO 3

typedef struct {
    uint64_t pin_bit_mask;
    unsigned mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
    size_t max_transfer_sz;
} spi_bus_config_t;
typedef struct {
    unsigned clock_speed_hz, mode;
    int spics_io_num;
    unsigned queue_size, cs_ena_posttrans;
} spi_device_interface_config_t;
typedef struct {
    size_t length;
    const void *tx_buffer;
    void *rx_buffer;
} spi_transaction_t;

int64_t esp_timer_get_time(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
void xSemaphoreGive(SemaphoreHandle_t);
void vTaskDelay(TickType_t);
int gpio_get_level(unsigned);
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t spi_bus_initialize(int, const spi_bus_config_t *, int);
esp_err_t spi_bus_add_device(int, const spi_device_interface_config_t *, spi_device_handle_t *);
esp_err_t spi_device_polling_start(spi_device_handle_t, spi_transaction_t *, TickType_t);
esp_err_t spi_device_polling_end(spi_device_handle_t, TickType_t);
#endif
