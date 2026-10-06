#include "keylight_json.h"
#include <math.h>
#include <string.h>

static bool integer(const cJSON *value, int min, int max) {
    return cJSON_IsNumber(value) && isfinite(value->valuedouble)
        && value->valuedouble >= min && value->valuedouble <= max
        && value->valuedouble == value->valueint;
}

static bool unique_keys(const cJSON *object) {
    for (const cJSON *a = object->child; a; a = a->next)
        for (const cJSON *b = a->next; b; b = b->next)
            if (!a->string || !b->string || !strcmp(a->string, b->string)) return false;
    return true;
}

bool kl_json_parse_revision(const cJSON *json, uint32_t *expected, bool *has_expected) {
    *expected = 0; *has_expected = false;
    if (!cJSON_IsObject(json) || cJSON_GetArraySize(json) > 1) return false;
    const cJSON *value = json->child;
    if (!value) return true;
    if (strcmp(value->string, "expected_revision") || !cJSON_IsNumber(value)
        || !isfinite(value->valuedouble) || value->valuedouble < 0
        || value->valuedouble > UINT32_MAX || floor(value->valuedouble) != value->valuedouble) return false;
    *has_expected = true; *expected = (uint32_t)value->valuedouble;
    return true;
}

bool kl_json_parse_patch(const cJSON *json, kl_patch *patch, uint32_t *expected, bool *has_expected) {
    if (!cJSON_IsObject(json) || !unique_keys(json)) return false;
    *patch = (kl_patch){0}; *has_expected = false; *expected = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, json) {
        const char *key = item->string;
        if (!strcmp(key, "power") || !strcmp(key, "recording_lock")) {
            if (!cJSON_IsBool(item)) return false;
            if (!strcmp(key, "power")) { patch->fields |= KL_POWER; patch->value.power = cJSON_IsTrue(item); }
            else { patch->fields |= KL_LOCK; patch->value.recording_lock = cJSON_IsTrue(item); }
        } else if (!strcmp(key, "mode")) {
            if (!cJSON_IsString(item)) return false;
            if (!strcmp(item->valuestring, "white")) patch->value.mode = KL_WHITE;
            else if (!strcmp(item->valuestring, "color")) patch->value.mode = KL_COLOR;
            else return false;
            patch->fields |= KL_MODE;
        } else if (!strcmp(key, "effect")) {
            if (!cJSON_IsString(item)) return false;
            if (!strcmp(item->valuestring, "none")) patch->value.effect = KL_EFFECT_NONE;
            else if (!strcmp(item->valuestring, "aurora")) patch->value.effect = KL_EFFECT_AURORA;
            else if (!strcmp(item->valuestring, "breathe")) patch->value.effect = KL_EFFECT_BREATHE;
            else return false;
            patch->fields |= KL_EFFECT;
        } else if (!strcmp(key, "brightness")) {
            if (!integer(item, 0, 100)) return false;
            patch->fields |= KL_BRIGHTNESS; patch->value.brightness = item->valueint;
        } else if (!strcmp(key, "temperature_k")) {
            if (!integer(item, 3000, 7000)) return false;
            patch->fields |= KL_TEMPERATURE; patch->value.temperature_k = item->valueint;
        } else if (!strcmp(key, "transition_ms")) {
            if (!integer(item, 0, 10000)) return false;
            patch->fields |= KL_TRANSITION; patch->value.transition_ms = item->valueint;
        } else if (!strcmp(key, "expected_revision")) {
            if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < 0
                || item->valuedouble > UINT32_MAX || floor(item->valuedouble) != item->valuedouble) return false;
            *has_expected = true; *expected = (uint32_t)item->valuedouble;
        } else if (!strcmp(key, "rgb")) {
            if (!cJSON_IsObject(item) || !unique_keys(item) || cJSON_GetArraySize(item) != 3) return false;
            const cJSON *r = cJSON_GetObjectItemCaseSensitive(item, "r");
            const cJSON *g = cJSON_GetObjectItemCaseSensitive(item, "g");
            const cJSON *b = cJSON_GetObjectItemCaseSensitive(item, "b");
            if (!integer(r, 0, 255) || !integer(g, 0, 255) || !integer(b, 0, 255)) return false;
            patch->fields |= KL_RGB;
            patch->value.rgb = (kl_rgb){r->valueint, g->valueint, b->valueint};
        } else return false;
    }
    return patch->fields != 0;
}

cJSON *kl_json_light(const kl_state *s) {
    static const char *effects[] = {"none", "aurora", "breathe"};
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "power", s->power);
    cJSON_AddStringToObject(json, "mode", s->mode == KL_WHITE ? "white" : "color");
    cJSON_AddNumberToObject(json, "brightness", s->brightness);
    cJSON_AddNumberToObject(json, "temperature_k", s->temperature_k);
    cJSON *rgb = cJSON_AddObjectToObject(json, "rgb");
    cJSON_AddNumberToObject(rgb, "r", s->rgb.r); cJSON_AddNumberToObject(rgb, "g", s->rgb.g); cJSON_AddNumberToObject(rgb, "b", s->rgb.b);
    cJSON_AddNumberToObject(json, "transition_ms", s->transition_ms);
    cJSON_AddStringToObject(json, "effect", effects[s->effect]);
    cJSON_AddBoolToObject(json, "recording_lock", s->recording_lock);
    return json;
}
