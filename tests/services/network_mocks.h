#ifndef NETWORK_MOCKS_H
#define NETWORK_MOCKS_H
#include "service_mocks.h"
#define pdPASS 1
#define pdMS_TO_TICKS(value) (value)
typedef const char *esp_event_base_t;
#define WIFI_EVENT ((esp_event_base_t)1)
#define IP_EVENT ((esp_event_base_t)2)
enum { WIFI_EVENT_STA_START = 1, WIFI_EVENT_STA_DISCONNECTED, IP_EVENT_STA_GOT_IP, ESP_EVENT_ANY_ID,
       WIFI_AUTH_OPEN, WIFI_MODE_STA, WIFI_MODE_APSTA, WIFI_MODE_AP, WIFI_IF_AP, WIFI_IF_STA, WIFI_STORAGE_RAM, WIFI_PS_NONE };
typedef struct { int id; } esp_netif_t;
typedef struct { unsigned char ssid[32]; unsigned char password[64]; struct { bool capable, required; } pmf_cfg; } wifi_sta_config_t;
typedef struct { unsigned char ssid[32]; unsigned ssid_len, channel, max_connection, authmode; } wifi_ap_config_t;
typedef union { wifi_sta_config_t sta; wifi_ap_config_t ap; } wifi_config_t;
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct { unsigned reason; } wifi_event_sta_disconnected_t;
typedef struct { struct { struct { unsigned byte[4]; } ip; } ip_info; } ip_event_got_ip_t;
typedef struct { int rssi; } wifi_ap_record_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(value) (value)->byte[0], (value)->byte[1], (value)->byte[2], (value)->byte[3]
typedef struct { const char *key, *value; } mdns_txt_item_t;
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_netif_set_hostname(esp_netif_t *, const char *);
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int, const wifi_config_t *);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_set_ps(int);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
esp_err_t esp_event_handler_register(esp_event_base_t, int, void (*)(void *, esp_event_base_t, int32_t, void *), void *);
esp_err_t mdns_init(void);
esp_err_t mdns_hostname_set(const char *);
esp_err_t mdns_instance_name_set(const char *);
esp_err_t mdns_service_add(const char *, const char *, const char *, unsigned, const mdns_txt_item_t *, unsigned);
void vTaskDelay(unsigned);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
#endif
