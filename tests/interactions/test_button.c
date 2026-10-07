#include "interaction_mocks.h"
#include "app.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/button.c"

app_context app;
static unsigned checks, locked, configs, creates, activations, paired, singles, cleared;
static unsigned cursor_seen[32];
static uint64_t now, stop_at, paired_at;
static int config_result, task_result, scenario;
static jmp_buf done;
static void (*entry)(void *);
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"button line %u: %s\n",__LINE__,#x); exit(1); } } while(0)
void app_lock(void) { CHECK(!locked); locked=1; }
void app_unlock(void) { CHECK(locked); locked=0; }
uint64_t app_now_ms(void) { return now; }
void app_pair_window(void) { ++paired; paired_at=now; }
esp_err_t app_clear_clients(void) { ++cleared; return ESP_OK; }
int app_submit(const kl_patch *p,const char *actor,uint32_t expected,bool has) {
    CHECK(!locked && p->fields==KL_POWER && !strcmp(actor,"button") && !expected && !has); ++singles; return 202;
}
int app_activate_scene(unsigned index,const char *actor,uint32_t expected,bool has) {
    CHECK(!locked && !strcmp(actor,"button") && !expected && !has && activations<32);
    cursor_seen[activations++]=index;
    if(index==0) return 404;
    if(activations==2) return scenario==1 ? 503 : 423;
    return 202;
}
esp_err_t gpio_config(const gpio_config_t *c) {
    ++configs; CHECK(c->pin_bit_mask==(1ULL<<34) && c->mode==GPIO_MODE_INPUT && c->pull_up_en==GPIO_PULLUP_DISABLE && c->pull_down_en==GPIO_PULLDOWN_DISABLE && c->intr_type==GPIO_INTR_DISABLE); return config_result;
}
int xTaskCreate(void (*task)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out) {
    ++creates; CHECK(configs==1 && task==button_task && !strcmp(name,"button") && stack==3072 && !arg && priority==5 && !out); entry=task; return task_result;
}
int gpio_get_level(unsigned pin) {
    CHECK(pin==34);
    if(scenario==2) return now<10500 ? 0 : 1;
    if(scenario==3 || scenario==4) return now>=100 && now<(scenario==3?3500u:3100u) ? 0 : 1;
    for(unsigned base=0;base<=2000;base+=1000)
        if((now>=base+100 && now<base+180)||(now>=base+260 && now<base+340)) return 0;
    return 1;
}
void vTaskDelay(unsigned ticks) { CHECK(ticks==5 && !locked); now+=ticks; if(now>=stop_at) longjmp(done,1); }
static void reset(void) {
    memset(&app,0,sizeof(app)); configs=creates=activations=paired=singles=cleared=0;
    now=paired_at=0; stop_at=2700; config_result=ESP_OK; task_result=pdPASS; entry=NULL;
}
int main(void) {
    reset(); config_result=ESP_FAIL; CHECK(app_button_start()==ESP_FAIL && configs==1 && creates==0 && !entry);
    reset(); task_result=0; CHECK(app_button_start()==ESP_ERR_NO_MEM && configs==1 && creates==1);
    for(scenario=0;scenario<2;++scenario) {
        reset(); CHECK(app_button_start()==ESP_OK); if(!setjmp(done)) entry(NULL);
        const unsigned expected[]={0,1,0,1,2};
        CHECK(activations==5 && !memcmp(cursor_seen,expected,sizeof(expected)) && !singles && !paired && !cleared);
    }
    scenario=2;reset();stop_at=11000;CHECK(app_button_start()==ESP_OK);if(!setjmp(done))entry(NULL);
    CHECK(cleared==1 && !activations && !singles && !paired);
    for(volatile unsigned blocked=0;blocked<5;++blocked) {
        scenario=3;reset();stop_at=3700;
        app.controller_ready=app.controller_connected=true;app.output_revision=17;
        if(blocked==1)app.desired.recording_lock=true;
        if(blocked==2)app.updating=true;
        if(blocked==3)app.controller_ready=false;
        if(blocked==4)app.controller_connected=false;
        CHECK(app_button_start()==ESP_OK);if(!setjmp(done))entry(NULL);
        CHECK(paired==1 && paired_at==3130 && !singles && !activations && !cleared);
        CHECK(app.pairing_feedback_generation==(blocked?0u:1u) && app.output_revision==17);
        if(!blocked)CHECK(app.pairing_feedback_ms==paired_at && app.pairing_feedback_revision==17);
    }
    scenario=4;reset();stop_at=3400;app.controller_ready=app.controller_connected=true;
    CHECK(app_button_start()==ESP_OK);if(!setjmp(done))entry(NULL);
    CHECK(!paired && !app.pairing_feedback_generation); /* Releasing before the stable 3s threshold is not acknowledged. */
    printf("%u actual button gesture/startup checks passed\n",checks);return 0;
}
