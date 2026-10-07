#include "config_mocks.h"
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/main/config_api.c"

static unsigned checks;
#define CHECK(v) do { ++checks;if(!(v)){fprintf(stderr,"FAIL %u: %s\n",__LINE__,#v);exit(1);} } while(0)
app_context app;
static unsigned locked,saves,config_saves,publications,events,restarts;
static int status;
static bool save_fails,upload_race;
static kl_output_encoding persisted;
static const char *body;
static cJSON *response;
void app_lock(void){CHECK(!locked);locked=1;}
void app_unlock(void){CHECK(locked);locked=0;}
void app_event_locked(const char *actor,const char *event,const char *detail){CHECK(locked && actor && event && detail);++events;}
void app_mqtt_publish(void){CHECK(!locked);++publications;}
esp_err_t app_output_encoding_save(kl_output_encoding value){
    CHECK(locked && (value==KL_OUTPUT_SRGB || value==KL_OUTPUT_LINEAR));++saves;
    if(save_fails)return ESP_FAIL;
    persisted=value;return ESP_OK;
}
esp_err_t app_config_save(const app_config *config){CHECK(config);++config_saves;return ESP_OK;}
esp_err_t app_scene_save(unsigned id,const app_scene *scene){CHECK(id<KL_SCENES && scene);return ESP_OK;}
int app_activate_scene(unsigned id,const char *actor,uint32_t revision,bool expected){(void)id;(void)actor;(void)revision;(void)expected;return 202;}
cJSON *app_state_json(void){return cJSON_CreateObject();}
cJSON *http_read_json(httpd_req_t *request){CHECK(request && !locked);if(upload_race)app.updating=true;return cJSON_Parse(body);}
esp_err_t http_json(httpd_req_t *request,int code,cJSON *json){CHECK(request && !locked);cJSON_Delete(response);response=json;status=code;return ESP_OK;}
esp_err_t http_error(httpd_req_t *request,int code,const char *message){cJSON *json=cJSON_CreateObject();cJSON_AddStringToObject(json,"error",message);return http_json(request,code,json);}
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *handle){
    CHECK(fn && name && stack && !arg && priority && !handle);++restarts;return 1;
}
void vTaskDelay(unsigned ticks){CHECK(ticks==1500);}
void esp_restart(void){CHECK(false);}

static void reset(void){
    cJSON_Delete(response);response=NULL;memset(&app,0,sizeof(app));
    app.desired=kl_state_default();app.controller_ready=true;app.revision=41;app.output_revision=7;
    app.reported_valid=app.rgb_confirmed=true;app.reported_fields=KL_ALL_FIELDS;
    snprintf(app.config.name,sizeof(app.config.name),"Lamp");snprintf(app.config.role,sizeof(app.config.role),"key");
    snprintf(app.config.ssid,sizeof(app.config.ssid),"unchanged-network");
    snprintf(app.config.password,sizeof(app.config.password),"unchanged-password");
    locked=saves=config_saves=publications=events=restarts=0;status=0;save_fails=upload_race=false;
    persisted=KL_OUTPUT_SRGB;body="{}";
}
static void patch(const char *json){body=json;httpd_req_t request={.method=HTTP_PATCH,.uri="/api/v1/settings",.content_len=strlen(json)};CHECK(http_settings(&request)==ESP_OK && !locked);}
static const char *encoding(void){const cJSON *v=cJSON_GetObjectItemCaseSensitive(response,"output_encoding");CHECK(cJSON_IsString(v));return v->valuestring;}
static void getters_and_changes(void){
    reset();httpd_req_t request={.method=HTTP_GET,.uri="/api/v1/settings"};
    CHECK(http_settings(&request)==ESP_OK && status==200 && !strcmp(encoding(),"srgb") && !saves);
    app_config saved=app.config;patch("{\"output_encoding\":\"linear\"}");
    CHECK(status==200 && saves==1 && !config_saves && !restarts && publications==1 && events==1);
    CHECK(app.output_encoding==KL_OUTPUT_LINEAR && persisted==KL_OUTPUT_LINEAR && !strcmp(encoding(),"linear"));
    CHECK(app.revision==42 && app.output_revision==8 && !strcmp(app.operation,"pending") && !app.error[0]);
    CHECK(!app.reported_valid && !app.rgb_confirmed && !app.reported_fields);
    CHECK(!memcmp(&saved,&app.config,sizeof(saved)));
    patch("{\"output_encoding\":\"linear\"}");
    CHECK(status==200 && saves==1 && events==1 && app.revision==42 && app.output_revision==8);
    patch("{\"output_encoding\":\"srgb\"}");
    CHECK(status==200 && saves==2 && app.output_encoding==KL_OUTPUT_SRGB && app.revision==43 && app.output_revision==9);
    CHECK(!memcmp(&saved,&app.config,sizeof(saved)));
}
static void input_and_failure_boundaries(void){
    const char *invalid[]={"{}","[]","null","{","{\"output_encoding\":true}","{\"output_encoding\":0}",
        "{\"output_encoding\":null}","{\"output_encoding\":\"sRGB\"}","{\"output_encoding\":\"\"}",
        "{\"output_encoding\":\"linear \"}","{\"output_encoding\":\"linear\",\"name\":\"Other\"}",
        "{\"output_encoding\":\"srgb\",\"output_encoding\":\"linear\"}",
        "{\"output_encoding\":\"srgb\",\"extra\":0}"};
    for(unsigned n=0;n<sizeof(invalid)/sizeof(*invalid);++n){reset();patch(invalid[n]);CHECK(status==400 && !saves && !config_saves && app.revision==41 && app.output_revision==7);}
    for(unsigned fault=0;fault<5;++fault){
        reset();
        if(fault==0)app.updating=true;
        if(fault==1)app.desired.recording_lock=true;
        if(fault==2)app.controller_ready=false;
        if(fault==3)save_fails=true;
        if(fault==4)upload_race=true;
        patch("{\"output_encoding\":\"linear\"}");
        CHECK(status==(fault==0||fault==4?409:fault==1?423:503));
        CHECK(saves==(fault==3) && !config_saves && !publications && !events && !restarts);
        CHECK(app.output_encoding==KL_OUTPUT_SRGB && persisted==KL_OUTPUT_SRGB && app.revision==41 && app.output_revision==7 && app.reported_valid);
    }
    reset();app.desired.recording_lock=true;app.controller_ready=false;
    patch("{\"output_encoding\":\"srgb\"}");CHECK(status==200 && !saves && app.revision==41);
    reset();patch("{\"name\":\"Existing API\"}");CHECK(status==200 && config_saves==1 && !saves && !strcmp(app.config.name,"Existing API"));
}
int main(void){getters_and_changes();input_and_failure_boundaries();cJSON_Delete(response);printf("PASS %u actual config API assertions\n",checks);return 0;}
