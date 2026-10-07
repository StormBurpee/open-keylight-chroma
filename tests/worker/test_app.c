#include "app_mocks.h"
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/app.c"

static unsigned checks, locked, sequence, worker_order, network_order, http_order, trial_order, journal_order, pairing, buttons, mqtt;
static bool worker_failure;
#define CHECK(value) do { ++checks; if (!(value)) { fprintf(stderr,"FAIL app line %u: %s\n",__LINE__,#value); exit(1); } } while (0)
void mock_log(const char *tag,const char *format,...) { CHECK(tag && format); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &locked; }
int xSemaphoreTake(SemaphoreHandle_t handle,unsigned ticks) { CHECK(handle==&locked && !locked && ticks==portMAX_DELAY);locked=1;return pdTRUE; }
void xSemaphoreGive(SemaphoreHandle_t handle) { CHECK(handle==&locked && locked);locked=0; }
int64_t esp_timer_get_time(void) { return 500000; }
void esp_read_mac(uint8_t *out,unsigned type) { CHECK(type==ESP_MAC_WIFI_STA);memcpy(out,"ABCDEF",6); }
const char *esp_err_to_name(esp_err_t result) { (void)result;return "injected"; }
esp_err_t app_storage_init(void) { return ESP_OK; }
esp_err_t app_controller_update_init(void) { journal_order=++sequence;return ESP_OK; }
void app_trial_start(void) { trial_order=++sequence; }
void app_pair_window(void) { ++pairing; }
esp_err_t app_worker_start(void) { worker_order=++sequence;return worker_failure?ESP_FAIL:ESP_OK; }
esp_err_t app_network_start(void) { network_order=++sequence;return ESP_OK; }
esp_err_t app_http_start(void) { http_order=++sequence;return ESP_OK; }
void app_button_start(void) { ++buttons; }
void app_mqtt_start(void) { ++mqtt; }
static void reset(void) {
    memset(&app,0,sizeof(app));locked=sequence=worker_order=network_order=http_order=trial_order=pairing=buttons=mqtt=0;
    worker_failure=false;app.mutex=&locked;app.desired=kl_state_default();
}
int main(void) {
    for(unsigned failure=0;failure<2;++failure) {
        reset();worker_failure=failure!=0;app_main();
        CHECK(journal_order<trial_order && trial_order<worker_order && worker_order<network_order && network_order<http_order);
        CHECK(http_order && pairing==1 && buttons==1 && mqtt==1 && !locked);
        if(failure)CHECK(!strcmp(app.controller_status,"fault") && !app.controller_ready);
    }
    reset();kl_state original=app.desired;
    const uint32_t fields[]={KL_POWER,KL_MODE,KL_BRIGHTNESS,KL_TEMPERATURE,KL_RGB,KL_EFFECT};
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i) {
        kl_patch patch={.fields=fields[i],.value=original};
        CHECK(app_submit(&patch,"test",0,false)==503);
        CHECK(!memcmp(&app.desired,&original,sizeof(original)) && !app.revision && !app.output_revision && !app.history_sequence);
    }
    kl_patch patch={.fields=KL_LOCK,.value=original};patch.value.recording_lock=true;
    CHECK(app_submit(&patch,"test",0,false)==202 && app.desired.recording_lock && app.revision==1 && !app.output_revision);
    patch.value.recording_lock=false;CHECK(app_submit(&patch,"test",1,true)==202);
    app.controller_ready=true;patch.fields=KL_POWER;patch.value.power=true;
    CHECK(app_submit(&patch,"test",0,true)==409 && !app.output_revision);
    CHECK(app_submit(&patch,"test",2,true)==202 && app.output_revision==1 && app.desired.power);
    app.controller_ready=false;patch.value.power=false;
    CHECK(app_submit(&patch,"test",3,true)==503 && app.desired.power); /* No false Off acknowledgement. */
    printf("%u application readiness/startup assertions passed\n",checks);return 0;
}
