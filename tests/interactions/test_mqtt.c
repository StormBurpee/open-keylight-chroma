#include "interaction_mocks.h"
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *race_print(const cJSON *);
#define cJSON_PrintUnformatted race_print
#include "../../firmware/main/mqtt.c"
#undef cJSON_PrintUnformatted

app_context app;
static unsigned checks, locked, queued, subscriptions, commands;
static bool blocked, enqueue_failure, race, in_mqtt, queue_failure;
static unsigned events_queued, notifications;
static struct { char topic[128], body[1024]; } messages[128];
static void (*event_handler)(void *, esp_event_base_t, int32_t, void *);
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"MQTT line %u: %s\n",__LINE__,#x); exit(1); } } while(0)
void app_lock(void) { CHECK(!locked); locked=1; }
void app_unlock(void) { CHECK(locked); locked=0; }
bool app_controller_update_blocked(void) { CHECK(locked); return blocked; }
void app_event_locked(const char *a,const char *e,const char *d) { CHECK(locked && a && e && d); }
int app_submit(const kl_patch *p,const char *a,uint32_t e,bool has) { CHECK(p && !strcmp(a,"homeassistant") && !e && !has); ++commands; return 202; }
static char *race_print(const cJSON *json) {
    CHECK(!locked);
    if(race) { race=false; app_lock(); app.controller_ready=false; app_unlock(); app_mqtt_availability(); }
    return cJSON_PrintUnformatted(json);
}
const esp_app_desc_t *esp_app_get_description(void) { static const esp_app_desc_t desc={"test"}; return &desc; }
int esp_crt_bundle_attach(void *p) { (void)p; return ESP_OK; }
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *c) {
    CHECK(!strcmp(c->session.last_will.msg,"offline") && c->session.last_will.retain && c->session.last_will.qos==1);
    return (void *)1;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t c,const char *topic,const char *body,int n,int qos,bool retain,bool store) {
    CHECK(in_mqtt);
    CHECK(c==(void *)1 && n==0 && qos==1 && retain && store);
    if(!strcmp(topic,availability_topic) || !strcmp(topic,state_topic)) CHECK(locked);
    if(enqueue_failure) return -1;
    CHECK(queued<128); snprintf(messages[queued].topic,128,"%s",topic); snprintf(messages[queued++].body,1024,"%s",body); return (int)queued;
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c,const char *topic,int qos) { CHECK(c && topic && qos==1); ++subscriptions; return 1; }
int esp_mqtt_client_register_event(esp_mqtt_client_handle_t c,int id,void (*cb)(void *,esp_event_base_t,int32_t,void *),void *arg) {
    CHECK(c && id==ESP_EVENT_ANY_ID && !arg); event_handler=cb; return ESP_OK;
}
int esp_mqtt_client_start(esp_mqtt_client_handle_t c) { CHECK(c); return ESP_OK; }
int esp_mqtt_dispatch_custom_event(esp_mqtt_client_handle_t c,esp_mqtt_event_t *event) {
    CHECK(locked && c && event); ++notifications;
    if(queue_failure) return ESP_FAIL;
    ++events_queued;return ESP_OK;
}
static void dispatch(int id,void *event) {
    CHECK(!locked && !in_mqtt);in_mqtt=true;event_handler(NULL,NULL,id,event);in_mqtt=false;
}
static void drain(void) {
    unsigned limit=10;while(events_queued) { CHECK(limit--);--events_queued;dispatch(MQTT_USER_EVENT,NULL); }
}
static void availability(void) { app_mqtt_availability();drain(); }
static void publish(void) { app_mqtt_publish();drain(); }
static unsigned count(const char *topic,const char *body) {
    unsigned n=0; for(unsigned i=0;i<queued;++i) if(!strcmp(messages[i].topic,topic) && (!body || !strcmp(messages[i].body,body))) ++n; return n;
}
static void setup(void) {
    memset(&app,0,sizeof(app)); strcpy(app.id,"keylight-test"); strcpy(app.ip,"192.0.2.1");
    strcpy(app.config.name,"Test"); strcpy(app.config.mqtt_uri,"mqtt://localhost"); app.config.mqtt_enabled=true;
    client=NULL; availability_known=false;pending_publications=0;notification_queued=false;
    events_queued=notifications=0;in_mqtt=queue_failure=false; queued=subscriptions=commands=0; blocked=enqueue_failure=race=false;
    app.reported=kl_state_default(); app.reported.power=true; app.reported_valid=true;
    app.reported_fields=KL_OUTPUT_FIELDS; strcpy(app.operation,"idle");
    app_mqtt_availability(); CHECK(!queued); app_mqtt_start(); CHECK(client && event_handler);
}
static void connect(void) { dispatch(MQTT_EVENT_CONNECTED,NULL);drain(); }
static void birth(void) {
    esp_mqtt_event_t event={.topic="homeassistant/status",.topic_len=20,.data="online",.data_len=6,.total_data_len=6};
    dispatch(MQTT_EVENT_DATA,&event);drain();
}
int main(void) {
    setup(); connect(); CHECK(count(availability_topic,"offline")==1 && !count(availability_topic,"online") && !count(state_topic,NULL));
    CHECK(subscriptions==2); app.controller_ready=app.controller_connected=true;
    publish(); CHECK(count(availability_topic,"online")==1 && count(state_topic,NULL)==1);
    availability(); CHECK(count(availability_topic,NULL)==2);
    app.desired.recording_lock=true; availability(); CHECK(count(availability_topic,NULL)==2);
    app.updating=true; publish(); CHECK(count(availability_topic,"offline")==2 && count(state_topic,NULL)==1);
    app.updating=false; availability(); CHECK(count(availability_topic,"online")==2);
    blocked=true; publish(); CHECK(count(availability_topic,"offline")==3 && count(state_topic,NULL)==1);
    blocked=false; app.controller_ready=false; availability(); CHECK(count(availability_topic,NULL)==5);
    app.controller_ready=true; app.controller_connected=false; availability(); CHECK(count(availability_topic,NULL)==5);
    app.controller_connected=true; availability(); CHECK(count(availability_topic,"online")==3);
    dispatch(MQTT_EVENT_DISCONNECTED,NULL); app.updating=true; publish(); CHECK(count(availability_topic,NULL)==6);
    connect(); CHECK(count(availability_topic,"offline")==4); birth(); CHECK(count(availability_topic,"offline")==5);
    app.updating=false; enqueue_failure=true; availability(); CHECK(count(availability_topic,"online")==3);
    enqueue_failure=false; availability(); CHECK(count(availability_topic,"online")==4);
    birth(); CHECK(count(availability_topic,"online")==5);
    unsigned states=count(state_topic,NULL), online=count(availability_topic,"online");
    race=true; publish(); CHECK(!race && count(state_topic,NULL)==states && count(availability_topic,"online")==online);
    CHECK(!strcmp(messages[queued-1].body,"offline"));
    app.controller_ready=true; app.output_revision++; publish(); CHECK(count(state_topic,NULL)==states);
    setup();app.controller_ready=app.controller_connected=true;connect();
    unsigned before=notifications;app.updating=true;
    app_mqtt_availability();app_mqtt_publish();app_mqtt_availability();
    CHECK(notifications==before+1 && events_queued==1);drain();CHECK(count(availability_topic,"offline")==1);
    app.updating=false;queue_failure=true;app_mqtt_availability();
    CHECK(!events_queued && !notification_queued && pending_publications);
    /* A full IDF event queue already contains an event; its callback flushes the pending flags. */
    queue_failure=false;dispatch(MQTT_USER_EVENT,NULL);CHECK(count(availability_topic,"online")==2 && !pending_publications);
    printf("%u actual MQTT lifecycle/publication checks passed\n",checks); return 0;
}
