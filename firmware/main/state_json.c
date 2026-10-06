#include "app.h"

cJSON *app_state_json(void) {
    app_lock();
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "revision", app.revision);
    cJSON_AddItemToObject(json, "desired", kl_json_light(&app.desired));
    cJSON *reported = kl_json_light(&app.reported);
    cJSON_AddBoolToObject(reported, "valid", app.reported_valid);
    cJSON *fields = cJSON_AddArrayToObject(reported, "confirmed_fields");
    if (app.reported_valid) {
        const char *names[] = {"power", "mode", "brightness", "temperature_k", "rgb", "effect"};
        const uint32_t masks[] = {KL_POWER, KL_MODE, KL_BRIGHTNESS, KL_TEMPERATURE, KL_RGB, KL_EFFECT};
        for (unsigned i = 0; i < 6; i++) if (app.reported_fields & masks[i])
            cJSON_AddItemToArray(fields, cJSON_CreateString(names[i]));
    }
    cJSON_AddItemToObject(json, "reported", reported);
    cJSON *operation = cJSON_AddObjectToObject(json, "operation");
    cJSON_AddStringToObject(operation, "status", app.operation);
    if (app.error[0]) cJSON_AddStringToObject(operation, "error", app.error); else cJSON_AddNullToObject(operation, "error");
    cJSON_AddStringToObject(json, "last_actor", app.actor);
    app_unlock();
    return json;
}
