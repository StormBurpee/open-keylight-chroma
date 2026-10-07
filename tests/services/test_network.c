#define NETWORK_TEST
#include "network_mocks.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/network.c"

static unsigned assertions;
#define CHECK(x) do { ++assertions; if (!(x)) { fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
app_context app;
static unsigned locked, mode_calls, config_calls, connect_calls, task_calls, handlers, cycles, cycle_limit;
static uint64_t now_ms;
static int mode, fail_mode_call, config_failure, interface_failure, task_failure;
static unsigned flash_held, flash_enter_calls, flash_leave_calls, wifi_init_calls, wifi_start_calls, ps_calls, fail_flash_call;
static esp_err_t flash_failure, init_failure, start_failure;
static wifi_config_t last_ap, last_sta;
static esp_netif_t ap_interface, sta_interface;
static jmp_buf stop_loop;
uint64_t app_now_ms(void) { return now_ms; }
void app_lock(void) { CHECK(!locked); locked = 1; }
void app_unlock(void) { CHECK(locked); locked = 0; }
void app_event_locked(const char *actor, const char *event, const char *detail) { CHECK(locked && actor && event && detail); }
esp_netif_t *esp_netif_create_default_wifi_ap(void) { return interface_failure ? NULL : &ap_interface; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &sta_interface; }
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_netif_set_hostname(esp_netif_t *interface, const char *name) { CHECK(interface == &sta_interface && name); return ESP_OK; }
esp_err_t esp_wifi_init(const wifi_init_config_t *config) { CHECK(config && flash_held && !locked); ++wifi_init_calls; return init_failure; }
esp_err_t esp_wifi_set_mode(int requested) {
    ++mode_calls;
    if ((int)mode_calls == fail_mode_call) return ESP_FAIL;
    mode = requested; return ESP_OK;
}
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config) {
    ++config_calls; CHECK(interface == WIFI_IF_AP || interface == WIFI_IF_STA);
    if (config_failure) return ESP_FAIL;
    if (interface == WIFI_IF_AP) last_ap = *config; else last_sta = *config;
    return ESP_OK;
}
esp_err_t esp_wifi_set_storage(int value) { CHECK(value == WIFI_STORAGE_RAM); return ESP_OK; }
uint64_t app_flash_guard_deadline(void) { return now_ms * 1000 + APP_FLASH_GUARD_WAIT_US; }
esp_err_t app_flash_guard_enter(uint64_t deadline) {
    CHECK(!locked && !flash_held && deadline == app_flash_guard_deadline()); ++flash_enter_calls;
    if (flash_enter_calls == fail_flash_call) return flash_failure;
    flash_held = 1; return ESP_OK;
}
void app_flash_guard_leave(void) { CHECK(flash_held && !locked); flash_held = 0; ++flash_leave_calls; }
esp_err_t esp_wifi_start(void) {
    CHECK(flash_held && !locked); ++wifi_start_calls;
    /* The real first PHY calibration/NVS save completes before this returns. */
    now_ms += 250; return start_failure;
}
esp_err_t esp_wifi_set_ps(int value) { CHECK(!flash_held && value == WIFI_PS_NONE); ++ps_calls; return ESP_OK; }
esp_err_t esp_wifi_connect(void) { ++connect_calls; return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record) { record->rssi = -50; return app.network_connected ? ESP_OK : ESP_FAIL; }
esp_err_t esp_event_handler_register(esp_event_base_t base, int event, void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg) {
    CHECK(base == WIFI_EVENT || base == IP_EVENT); CHECK(event == ESP_EVENT_ANY_ID || event == IP_EVENT_STA_GOT_IP);
    CHECK(handler == network_event && !arg); ++handlers; return ESP_OK;
}
esp_err_t mdns_init(void) { return ESP_OK; }
esp_err_t mdns_hostname_set(const char *name) { CHECK(name); return ESP_OK; }
esp_err_t mdns_instance_name_set(const char *name) { CHECK(name); return ESP_OK; }
esp_err_t mdns_service_add(const char *instance, const char *service, const char *protocol, unsigned port, const mdns_txt_item_t *items, unsigned count) {
    CHECK(!instance && service && !strcmp(protocol, "_tcp") && port == 80 && items && count == 3); return ESP_OK;
}
void vTaskDelay(unsigned ticks) {
    CHECK(ticks == 5000);
    if (cycles == cycle_limit) longjmp(stop_loop, 1);
    ++cycles; now_ms += ticks;
}
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *out) {
    CHECK(entry == reconnect_task && !strcmp(name, "network") && stack == 3072 && !arg && priority == 3 && !out);
    ++task_calls; return task_failure ? 0 : pdPASS;
}
static void tick(unsigned count) {
    cycles = 0; cycle_limit = count;
    if (!setjmp(stop_loop)) reconnect_task(NULL);
    CHECK(cycles == count && !locked);
}
static void reset(bool configured) {
    CHECK(!locked && !flash_held); memset(&app, 0, sizeof(app)); memset(&last_ap, 0, sizeof(last_ap)); memset(&last_sta, 0, sizeof(last_sta));
    if (configured) { snprintf(app.config.ssid, sizeof(app.config.ssid), "private-test-ssid"); snprintf(app.config.password, sizeof(app.config.password), "private-test-password"); }
    snprintf(app.hostname, sizeof(app.hostname), "keylight-test"); app.mac[3] = 0xaa; app.mac[4] = 0xbb; app.mac[5] = 0xcc;
    now_ms = 100; mode_calls = config_calls = connect_calls = task_calls = handlers = cycles = cycle_limit = 0;
    mode = fail_mode_call = config_failure = interface_failure = task_failure = 0;
    flash_enter_calls = flash_leave_calls = wifi_init_calls = wifi_start_calls = ps_calls = fail_flash_call = 0;
    flash_failure = init_failure = start_failure = ESP_OK;
    setup_interface = NULL; setup_active = false; setup_until_ms = 0; atomic_store(&recovery_requested, false);
}
int main(void) {
    reset(true); app_config original = app.config; CHECK(app_network_start() == ESP_OK && mode == WIFI_MODE_STA && !setup_active);
    CHECK(!memcmp(last_sta.sta.ssid, app.config.ssid, strlen(app.config.ssid)) && handlers == 2 && task_calls == 1);
    CHECK(flash_enter_calls == 2 && flash_leave_calls == 2 && wifi_init_calls == 1 && wifi_start_calls == 1 && !flash_held);
    tick(2); CHECK(connect_calls == 2 && !setup_active && !memcmp(&original, &app.config, sizeof(original)));
    app_network_recovery_request(); app.pair_until_ms = now_ms + 180000; tick(1);
    CHECK(setup_active && mode == WIFI_MODE_APSTA && setup_until_ms == now_ms + 180000);
    CHECK(!strcmp((char *)last_ap.ap.ssid, "Keylight-Setup-AABBCC") && last_ap.ap.max_connection == 1 && last_ap.ap.authmode == WIFI_AUTH_OPEN);
    /* Pairing consumes its window; owner still has the AP lifetime to change Wi-Fi. */
    app.pair_until_ms = 0; tick(1); CHECK(setup_active && mode == WIFI_MODE_APSTA);
    now_ms = setup_until_ms - 5001; tick(1); CHECK(setup_active);
    tick(1); CHECK(!setup_active && mode == WIFI_MODE_STA && !memcmp(&original, &app.config, sizeof(original)));
    reset(true); app.network_connected = true; app_network_recovery_request(); tick(1); CHECK(!setup_active && !mode_calls && !connect_calls && app.rssi == -50);
    reset(false); CHECK(app_network_start() == ESP_OK && setup_active && mode == WIFI_MODE_AP && !strcmp(app.ip, "192.168.4.1"));
    tick(40); CHECK(setup_active && !connect_calls); /* Unconfigured first-boot setup remains available. */
    for (unsigned fault = 0; fault < 4; ++fault) {
        reset(true); interface_failure = fault == 0; fail_mode_call = fault == 1 ? 1 : fault == 3 ? 2 : 0; config_failure = fault >= 2;
        CHECK(start_setup_ap(true) != ESP_OK);
        if (fault < 3) CHECK(!setup_active);
        else {
            CHECK(setup_active && mode == WIFI_MODE_APSTA); now_ms = setup_until_ms; tick(1); CHECK(!setup_active && mode == WIFI_MODE_STA);
        }
    }
    reset(true); CHECK(start_setup_ap(true) == ESP_OK); now_ms = setup_until_ms; fail_mode_call = (int)mode_calls + 1;
    tick(1); CHECK(setup_active); tick(1); CHECK(!setup_active);
    reset(true); task_failure = 1; CHECK(app_network_start() == ESP_ERR_NO_MEM);
    reset(true); flash_failure = ESP_ERR_TIMEOUT; fail_flash_call = 1; CHECK(app_network_start() == ESP_ERR_TIMEOUT);
    CHECK(flash_enter_calls == 1 && !flash_leave_calls && !wifi_start_calls && !ps_calls && !task_calls);
    CHECK(!wifi_init_calls);
    reset(true); flash_failure = ESP_ERR_TIMEOUT; fail_flash_call = 2; CHECK(app_network_start() == ESP_ERR_TIMEOUT);
    CHECK(flash_enter_calls == 2 && flash_leave_calls == 1 && wifi_init_calls == 1 && !wifi_start_calls && !ps_calls && !task_calls);
    reset(true); init_failure = ESP_FAIL; CHECK(app_network_start() == ESP_FAIL);
    CHECK(flash_enter_calls == 1 && flash_leave_calls == 1 && wifi_init_calls == 1 && !wifi_start_calls && !ps_calls && !task_calls);
    reset(false); start_failure = ESP_FAIL; CHECK(app_network_start() == ESP_FAIL);
    CHECK(flash_enter_calls == 2 && flash_leave_calls == 2 && wifi_start_calls == 1 && !flash_held && !ps_calls && !task_calls);
    reset(true); original = app.config; app.network_connected = true;
    wifi_event_sta_disconnected_t disconnected = {201}; network_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
    CHECK(!app.network_connected && !connect_calls && !memcmp(&original, &app.config, sizeof(original)));
    ip_event_got_ip_t address = {.ip_info.ip.byte = {192, 0, 2, 33}}; network_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &address);
    CHECK(app.network_connected && !strcmp(app.ip, "192.0.2.33"));
    network_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL); CHECK(connect_calls == 1);
    printf("PASS %u assertions against actual network.c\n", assertions); return 0;
}
