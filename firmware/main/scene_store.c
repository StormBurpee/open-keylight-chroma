#include "scene_store.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

/* One NVS blob is the commit unit: collection and seed marker cannot diverge.
 * Explicit bytes keep its format independent of C padding, enum and bool ABI. */
enum { SCENE_HEADER_BYTES = 16, SCENE_ENTRY_BYTES = 48,
       SCENE_RECORD_BYTES = SCENE_HEADER_BYTES + KL_SCENES * SCENE_ENTRY_BYTES };
static const char *scene_key = "scenes_v2";
static bool scene_store_ready;

static bool scene_valid(const app_scene *scene) {
    if (!scene->used) return true;
    const char *end = memchr(scene->name, 0, sizeof(scene->name));
    if (!end || end == scene->name || !kl_state_valid(&scene->state)) return false;
    for (const char *p = scene->name; p < end; ++p)
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127) return false;
    return true;
}

static void put16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8);
}
static uint16_t get16(const uint8_t *in) { return (uint16_t)(in[0] | (uint16_t)in[1] << 8); }

static void encode_scenes(const app_scene scenes[KL_SCENES], uint8_t out[SCENE_RECORD_BYTES]) {
    memset(out, 0, SCENE_RECORD_BYTES);
    memcpy(out, "OKSC", 4); out[4] = 2; out[5] = KL_SCENES; out[6] = 1;
    for (unsigned i = 0; i < KL_SCENES; ++i) {
        const app_scene *scene = &scenes[i];
        if (!scene->used) continue;
        uint8_t *p = out + SCENE_HEADER_BYTES + i * SCENE_ENTRY_BYTES;
        const kl_state *s = &scene->state;
        p[0] = 1; memcpy(p + 1, scene->name, strlen(scene->name));
        p[34] = s->power; p[35] = (uint8_t)s->mode; p[36] = s->brightness;
        put16(p + 37, s->temperature_k);
        p[39] = s->rgb.r; p[40] = s->rgb.g; p[41] = s->rgb.b;
        put16(p + 42, s->transition_ms); p[44] = (uint8_t)s->effect; p[45] = s->recording_lock;
    }
}

static bool decode_scenes(const uint8_t in[SCENE_RECORD_BYTES], app_scene scenes[KL_SCENES]) {
    static const uint8_t header[SCENE_HEADER_BYTES] = {'O','K','S','C',2,KL_SCENES,1};
    if (memcmp(in, header, sizeof(header))) return false;
    memset(scenes, 0, sizeof(app_scene) * KL_SCENES);
    for (unsigned i = 0; i < KL_SCENES; ++i) {
        const uint8_t *p = in + SCENE_HEADER_BYTES + i * SCENE_ENTRY_BYTES;
        if (!p[0]) {
            for (unsigned j = 1; j < SCENE_ENTRY_BYTES; ++j) if (p[j]) return false;
            continue;
        }
        if (p[0] != 1 || p[34] > 1 || p[35] > KL_COLOR || p[44] > KL_EFFECT_BREATHE
            || p[45] > 1 || p[46] || p[47]) return false;
        app_scene *scene = &scenes[i]; scene->used = true;
        memcpy(scene->name, p + 1, 33);
        scene->state = (kl_state){.power = p[34] != 0, .mode = (kl_mode)p[35],
            .brightness = p[36], .temperature_k = get16(p + 37), .rgb = {p[39],p[40],p[41]},
            .transition_ms = get16(p + 42), .effect = (kl_effect)p[44], .recording_lock = p[45] != 0};
        if (!scene_valid(scene)) return false;
        /* Require the same canonical representation that our encoder writes. */
        for (size_t j = strlen(scene->name) + 1; j < 33; ++j) if (p[1 + j]) return false;
    }
    return true;
}

static esp_err_t persist_scenes(const app_scene scenes[KL_SCENES]) {
    uint8_t record[SCENE_RECORD_BYTES]; encode_scenes(scenes, record);
    nvs_handle_t handle;
    esp_err_t result = nvs_open("openkeylight", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, scene_key, record, sizeof(record));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

static bool same_default_name(const char *a, const char *b) {
    /* Preserve a user's edited version of a known default, including case. */
    while (*a && *b) {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return *a == *b;
}

static void seed_scenes(app_scene scenes[KL_SCENES], bool occupied[KL_SCENES]) {
    static const struct { const char *name; kl_mode mode; uint8_t brightness; kl_rgb rgb; } defaults[] = {
        {"Focus", KL_WHITE, 80, {36,92,255}}, {"Blue hour", KL_COLOR, 72, {36,92,255}},
        {"Ember", KL_COLOR, 60, {255,112,38}}, {"Afterglow", KL_COLOR, 55, {178,138,255}}
    };
    for (unsigned n = 0; n < sizeof(defaults) / sizeof(defaults[0]); ++n) {
        unsigned free_slot = KL_SCENES; bool found = false;
        for (unsigned i = 0; i < KL_SCENES; ++i) {
            if (scenes[i].used && same_default_name(scenes[i].name, defaults[n].name)) found = true;
            if (!occupied[i] && free_slot == KL_SCENES) free_slot = i;
        }
        if (found || free_slot == KL_SCENES) continue;
        app_scene *scene = &scenes[free_slot];
        *scene = (app_scene){.used = true, .state = {.power = true, .mode = defaults[n].mode,
            .brightness = defaults[n].brightness, .temperature_k = 4200, .rgb = defaults[n].rgb,
            .transition_ms = 1200, .effect = KL_EFFECT_NONE, .recording_lock = false}};
        snprintf(scene->name, sizeof(scene->name), "%s", defaults[n].name);
        occupied[free_slot] = true;
    }
}

esp_err_t app_scene_store_init(void) {
    scene_store_ready = false;
    app_scene scenes[KL_SCENES] = {0}; bool occupied[KL_SCENES] = {0};
    nvs_handle_t handle;
    esp_err_t result = nvs_open("openkeylight", NVS_READONLY, &handle);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) return result;
    if (result == ESP_OK) {
        uint8_t record[SCENE_RECORD_BYTES]; size_t size = 0;
        result = nvs_get_blob(handle, scene_key, NULL, &size);
        if (result != ESP_ERR_NVS_NOT_FOUND) {
            if (result == ESP_OK && size != sizeof(record)) result = ESP_ERR_INVALID_STATE;
            if (result == ESP_OK) result = nvs_get_blob(handle, scene_key, record, &size);
            if (result == ESP_OK && (size != sizeof(record) || !decode_scenes(record, scenes)))
                result = ESP_ERR_INVALID_STATE;
            nvs_close(handle);
            if (result == ESP_OK) { memcpy(app.scenes, scenes, sizeof(scenes)); scene_store_ready = true; }
            return result;
        }
        /* Missing is distinct from unreadable. Even an unused legacy record
         * reserves its ID during migration; malformed slots abort migration. */
        for (unsigned i = 0; i < KL_SCENES; ++i) {
            uint8_t legacy[sizeof(app_scene)];
            char key[12]; snprintf(key, sizeof(key), "scene%u_v1", i); size = sizeof(legacy);
            result = nvs_get_blob(handle, key, legacy, &size);
            if (result == ESP_ERR_NVS_NOT_FOUND) continue;
            if (result == ESP_OK && (size != sizeof(legacy) || legacy[offsetof(app_scene, used)] > 1
                || legacy[offsetof(app_scene, state) + offsetof(kl_state, power)] > 1
                || legacy[offsetof(app_scene, state) + offsetof(kl_state, recording_lock)] > 1))
                result = ESP_ERR_INVALID_STATE;
            if (result == ESP_OK) memcpy(&scenes[i], legacy, sizeof(scenes[i]));
            if (result != ESP_OK || !scene_valid(&scenes[i])) {
                nvs_close(handle); return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
            }
            occupied[i] = true;
        }
        nvs_close(handle);
    }
    /* Keep readable pre-migration scenes available even if adding the defaults
     * cannot be persisted. Only the new collection requires a successful write. */
    memcpy(app.scenes, scenes, sizeof(scenes));
    seed_scenes(scenes, occupied);
    result = persist_scenes(scenes);
    if (result == ESP_OK) { memcpy(app.scenes, scenes, sizeof(scenes)); scene_store_ready = true; }
    return result;
}

esp_err_t app_scene_save(unsigned index, const app_scene *scene) {
    if (index >= KL_SCENES || !scene || !scene_valid(scene)) return ESP_ERR_INVALID_ARG;
    app_lock();
    if (!scene_store_ready) { app_unlock(); return ESP_ERR_INVALID_STATE; }
    app_scene scenes[KL_SCENES]; memcpy(scenes, app.scenes, sizeof(scenes));
    scenes[index] = *scene;
    if (!scene->used) memset(&scenes[index], 0, sizeof(scenes[index]));
    esp_err_t result = persist_scenes(scenes);
    if (result == ESP_OK) memcpy(app.scenes, scenes, sizeof(scenes));
    else scene_store_ready = false; /* A failed commit may have persisted. Reload before another write. */
    app_unlock();
    return result;
}
