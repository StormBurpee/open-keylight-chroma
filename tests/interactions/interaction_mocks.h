#ifndef INTERACTION_MOCKS_H
#define INTERACTION_MOCKS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define pdPASS 1
#define pdMS_TO_TICKS(n) (n)
typedef const char *esp_event_base_t;
typedef void *esp_mqtt_client_handle_t;
typedef struct {
    struct { struct { const char *uri; } address; struct { int (*crt_bundle_attach)(void *); } verification; } broker;
    struct { const char *client_id, *username; struct { const char *password; } authentication; } credentials;
    struct { struct { const char *topic, *msg; unsigned qos; bool retain; } last_will; } session;
    struct { unsigned size; } buffer;
    struct { unsigned limit; } outbox;
} esp_mqtt_client_config_t;
typedef struct { size_t topic_len; int current_data_offset, total_data_len, data_len; bool retain; char *topic, *data; } esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;
enum { MQTT_EVENT_CONNECTED, MQTT_EVENT_DISCONNECTED, MQTT_EVENT_DATA, MQTT_USER_EVENT, ESP_EVENT_ANY_ID };
typedef struct { char version[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
int esp_crt_bundle_attach(void *);
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t, const char *, const char *, int, int, bool, bool);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char *, int);
int esp_mqtt_client_register_event(esp_mqtt_client_handle_t, int, void (*)(void *, esp_event_base_t, int32_t, void *), void *);
int esp_mqtt_client_start(esp_mqtt_client_handle_t);
int esp_mqtt_dispatch_custom_event(esp_mqtt_client_handle_t, esp_mqtt_event_t *);
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
enum { GPIO_MODE_INPUT, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE };
esp_err_t gpio_config(const gpio_config_t *);
int gpio_get_level(unsigned);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskDelay(unsigned);
#endif
