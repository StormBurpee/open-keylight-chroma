#include "app.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "mdns.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

static atomic_bool recovery_requested;
static esp_netif_t *setup_interface;
static bool setup_active;
static uint64_t setup_until_ms;

void app_network_recovery_request(void) { atomic_store(&recovery_requested, true); }

static esp_err_t start_setup_ap(bool with_station) {
    if (!setup_interface) setup_interface = esp_netif_create_default_wifi_ap();
    if (!setup_interface) return ESP_ERR_NO_MEM;
    wifi_config_t wifi = {0};
    snprintf((char *)wifi.ap.ssid, sizeof(wifi.ap.ssid), "Keylight-Setup-%02X%02X%02X", app.mac[3], app.mac[4], app.mac[5]);
    wifi.ap.ssid_len = strlen((char *)wifi.ap.ssid);
    wifi.ap.channel = 1; wifi.ap.max_connection = 1; wifi.ap.authmode = WIFI_AUTH_OPEN;
    esp_err_t result = esp_wifi_set_mode(with_station ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (result != ESP_OK) return result;
    setup_active = true;
    setup_until_ms = app_now_ms() + 180000;
    result = esp_wifi_set_config(WIFI_IF_AP, &wifi);
    if (result != ESP_OK && with_station && esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) setup_active = false;
    return result;
}

static void network_event(void *unused, esp_event_base_t base, int32_t id, void *data) {
    (void)unused;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        char detail[64]; snprintf(detail, sizeof(detail), "Wi-Fi disconnected (reason %u); output preserved", event->reason);
        app_lock(); app.network_connected = false;
        app_event_locked("network", "wifi.disconnected", detail); app_unlock();
        /* The reconnect task spaces retries; credentials and lighting are untouched. */
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        app_lock(); app.network_connected = true;
        snprintf(app.ip, sizeof(app.ip), IPSTR, IP2STR(&event->ip_info.ip));
        app_event_locked("network", "wifi.connected", "Local network available"); app_unlock();
    }
}

static void reconnect_task(void *unused) {
    (void)unused;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        app_lock(); bool connected = app.network_connected; bool configured = app.config.ssid[0] != 0;
        app_unlock();
        if (atomic_exchange(&recovery_requested, false) && !connected && configured) {
            esp_err_t result = start_setup_ap(true);
            app_lock(); app_event_locked("network", "setup.requested", result == ESP_OK ?
                "Temporary setup network available at 192.168.4.1" : "Could not start setup network"); app_unlock();
        }
        if (setup_active && configured && app_now_ms() >= setup_until_ms && esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) setup_active = false;
        if (!connected && configured) esp_wifi_connect();
        wifi_ap_record_t record;
        if (esp_wifi_sta_get_ap_info(&record) == ESP_OK) { app_lock(); app.rssi = record.rssi; app_unlock(); }
    }
}

esp_err_t app_network_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *station = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(station, app.hostname));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL));
    wifi_config_t wifi = {0};
    if (app.config.ssid[0]) {
        memcpy(wifi.sta.ssid, app.config.ssid, strlen(app.config.ssid));
        memcpy(wifi.sta.password, app.config.password, strlen(app.config.password));
        wifi.sta.pmf_cfg.capable = true;
        wifi.sta.pmf_cfg.required = false;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    } else {
        ESP_ERROR_CHECK(start_setup_ap(false));
        snprintf(app.ip, sizeof(app.ip), "192.168.4.1");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(app.hostname);
        mdns_instance_name_set(app.config.name);
        mdns_txt_item_t txt[] = {{"api", "1"}, {"path", "/"}, {"model", "keylight-chroma"}};
        mdns_service_add(NULL, "_http", "_tcp", 80, txt, 3);
        mdns_service_add(NULL, "_openkeylight", "_tcp", 80, txt, 3);
    }
    return xTaskCreate(reconnect_task, "network", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
