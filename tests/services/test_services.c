#include "service_mocks.h"
#include <stdio.h>
#include <string.h>
#include "../../firmware/main/scene_store.c"
#include "../../firmware/main/storage.c"
#include "../../firmware/main/http_server.c"

static unsigned assertions;
#define CHECK(x) do { ++assertions; if (!(x)) { fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
app_context app;
typedef struct { char key[32]; unsigned char data[4096]; size_t size; } blob;
static blob stored[16], pending;
static unsigned stored_count, locked, writes, commits, closes, random_counter, recovery_requests;
static unsigned issued_routes, submit_calls, settings_calls, update_calls, confirm_calls, session_closes;
static unsigned controller_update_calls, controller_status_calls;
static unsigned request_reads, response_code, server_handlers;
static uint64_t now_ms, receive_delay;
static size_t body_used, chunk;
static int failure, sha_failure, recv_failure;
static const char *read_failure_key;
static bool commit_persists_on_failure;
static bool namespace_absent;
static const char *header_host, *header_origin, *header_auth, *header_type, *body;
static char response_body[16384];
static bool connection_close;
static esp_app_desc_t descriptor = {.version = "host-test"};

uint64_t app_now_ms(void) { return now_ms; }
void app_lock(void) { CHECK(!locked); locked = 1; }
void app_unlock(void) { CHECK(locked); locked = 0; }
void app_event_locked(const char *actor, const char *event, const char *detail) { CHECK(locked && actor && event && detail); }
void app_network_recovery_request(void) { CHECK(!locked); ++recovery_requests; }
esp_err_t nvs_flash_init(void) { return failure == 5 ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out) {
    CHECK(!strcmp(name, "openkeylight") || (!strcmp(name, "nvskvinfo0") && mode == NVS_READONLY));
    *out = !strcmp(name, "openkeylight") ? 1 : 2;
    if (namespace_absent && !stored_count && mode == NVS_READONLY && *out == 1) return ESP_ERR_NVS_NOT_FOUND;
    return failure == 1 ? ESP_FAIL : ESP_OK;
}
static blob *find_blob(const char *key) {
    for (unsigned i = 0; i < stored_count; ++i) if (!strcmp(stored[i].key, key)) return &stored[i];
    return NULL;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    CHECK(handle == 1); blob *entry = find_blob(key);
    if (read_failure_key && !strcmp(key, read_failure_key)) return ESP_FAIL;
    if (!entry) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *size = entry->size; return ESP_OK; }
    if (*size < entry->size) return ESP_FAIL;
    memcpy(out, entry->data, entry->size); *size = entry->size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) {
    CHECK(handle == 1 && size <= sizeof(pending.data)); ++writes;
    if (failure == 2) return ESP_FAIL;
    snprintf(pending.key, sizeof(pending.key), "%s", key); memcpy(pending.data, data, size); pending.size = size;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out) {
    blob *entry = find_blob(key); if (entry && entry->size != 1) return ESP_FAIL;
    size_t size = 1; return nvs_get_blob(handle, key, out, &size);
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    return nvs_set_blob(handle, key, &value, sizeof(value));
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *size) {
    CHECK(handle == 2); const char *value = !strcmp(key, "w_ssid") ? "private-test-network" : "private-test-password";
    if (failure == 4 || *size < strlen(value) + 1) return ESP_FAIL;
    memcpy(out, value, strlen(value) + 1); *size = strlen(value) + 1; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    CHECK(handle == 1); ++commits; if (failure == 3 && !commit_persists_on_failure) return ESP_FAIL;
    blob *entry = find_blob(pending.key);
    if (!entry) { CHECK(stored_count < 16); entry = &stored[stored_count++]; }
    *entry = pending; return failure == 3 ? ESP_FAIL : ESP_OK;
}
void nvs_close(nvs_handle_t handle) { CHECK(handle == 1 || handle == 2); ++closes; }
void esp_fill_random(void *out, size_t size) {
    ++random_counter;
    for (size_t i = 0; i < size; ++i) ((unsigned char *)out)[i] = (unsigned char)(random_counter + i);
}
int mbedtls_sha256(const unsigned char *value, size_t size, unsigned char *out, int sha224) {
    CHECK(!sha224);
    if (sha_failure) return -1;
    /* Deterministic digest stand-in: crypto itself belongs to IDF tests. */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) h = (h ^ value[i]) * 16777619u;
    for (unsigned i = 0; i < 32; ++i) { h = h * 1664525u + 1013904223u; out[i] = (uint8_t)(h >> 24); }
    return 0;
}
const esp_app_desc_t *esp_app_get_description(void) { return &descriptor; }
unsigned esp_get_free_heap_size(void) { return 100000; }
unsigned esp_reset_reason(void) { return 1; }
bool app_trial_pending(void) { return false; }
esp_err_t app_trial_confirm(void) { ++confirm_calls; return ESP_OK; }
cJSON *app_state_json(void) { return cJSON_CreateObject(); }
int app_submit(const kl_patch *patch, const char *actor, uint32_t revision, bool expected) {
    (void)revision; (void)expected; CHECK(patch && actor); ++submit_calls; return 202;
}
int app_activate_scene(unsigned index, const char *actor, uint32_t expected, bool has_expected) {
    (void)index; (void)actor; (void)expected; (void)has_expected; return 202;
}
esp_err_t http_settings(httpd_req_t *request) { ++settings_calls; return http_json(request, 200, cJSON_CreateObject()); }
esp_err_t http_scenes(httpd_req_t *request) { return http_json(request, 200, cJSON_CreateObject()); }
esp_err_t http_update(httpd_req_t *request) { ++update_calls; return http_json(request, 202, cJSON_CreateObject()); }
esp_err_t http_controller_update(httpd_req_t *request) { ++controller_update_calls; return http_json(request, 202, cJSON_CreateObject()); }
cJSON *app_controller_update_json(void) { ++controller_status_calls; return cJSON_CreateObject(); }
static const char *header(const char *name) {
    if (!strcmp(name, "Host")) return header_host;
    if (!strcmp(name, "Origin")) return header_origin;
    if (!strcmp(name, "Authorization")) return header_auth;
    if (!strcmp(name, "Content-Type")) return header_type;
    return NULL;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *name, char *value, size_t size) {
    (void)request; const char *source = header(name);
    if (!source || strlen(source) >= size) return ESP_FAIL;
    snprintf(value, size, "%s", source); return ESP_OK;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *request, const char *name) { (void)request; const char *value = header(name); return value ? strlen(value) : 0; }
int httpd_req_recv(httpd_req_t *request, char *out, size_t size) {
    (void)request; ++request_reads; now_ms += receive_delay;
    if (recv_failure) return recv_failure == 1 ? -1 : 0;
    if (size > chunk) size = chunk;
    memcpy(out, body + body_used, size); body_used += size; return (int)size;
}
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status) { (void)request; response_code = (unsigned)atoi(status); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type) { (void)request; CHECK(type); return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value) {
    (void)request; CHECK(name && value); if (!strcmp(name, "Connection")) connection_close = !strcmp(value, "close"); return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *request, const char *value, int size) {
    (void)request; CHECK(value); if (size == HTTPD_RESP_USE_STRLEN) size = (int)strlen(value);
    CHECK(size >= 0 && (size_t)size < sizeof(response_body)); memcpy(response_body, value, (size_t)size); response_body[size] = 0;
    return ESP_OK;
}
int httpd_req_to_sockfd(httpd_req_t *request) { CHECK(request); return 4; }
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int socket) { (void)handle; CHECK(socket == 4 && connection_close); ++session_closes; return ESP_OK; }
esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *config) { CHECK(config->max_uri_handlers == 5); *out = (void *)1; return ESP_OK; }
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri) { CHECK(handle && !strcmp(uri->uri, "/*") && uri->handler == route); ++server_handlers; return ESP_OK; }

static void request_reset(const char *data) {
    header_host = "192.0.2.1"; header_origin = NULL; header_auth = NULL; header_type = "application/json";
    body = data; body_used = 0; chunk = 2048; receive_delay = 0; recv_failure = 0;
    response_code = request_reads = session_closes = 0; connection_close = false; response_body[0] = 0;
}
static void reset(void) {
    CHECK(!locked); memset(&app, 0, sizeof(app)); memset(stored, 0, sizeof(stored)); memset(&pending, 0, sizeof(pending));
    snprintf(app.ip, sizeof(app.ip), "192.0.2.1"); snprintf(app.hostname, sizeof(app.hostname), "keylight-test");
    now_ms = 100; stored_count = writes = commits = closes = random_counter = recovery_requests = 0;
    issued_routes = submit_calls = settings_calls = update_calls = confirm_calls = server_handlers = 0;
    failure = sha_failure = 0; read_failure_key = NULL; commit_persists_on_failure = namespace_absent = false; scene_store_ready = false;
    request_reset("{}");
}
static void pair(char token[65], const char *label) { app_pair_window(); CHECK(app_issue_token(label, token) == ESP_OK); CHECK(app_token_valid(token)); }
static void put_fixture(const char *key, const void *data, size_t size) {
    blob *entry = find_blob(key);
    if (!entry) { CHECK(stored_count < 16); entry = &stored[stored_count++]; }
    CHECK(size <= sizeof(entry->data)); memset(entry, 0, sizeof(*entry));
    snprintf(entry->key, sizeof(entry->key), "%s", key); memcpy(entry->data, data, size); entry->size = size;
}
static app_scene custom_scene(const char *name) {
    app_scene scene = {.used = true, .state = kl_state_default()};
    snprintf(scene.name, sizeof(scene.name), "%s", name); scene.state.brightness = 17;
    return scene;
}
static unsigned scene_count(void) {
    unsigned count = 0; for (unsigned i = 0; i < KL_SCENES; ++i) count += app.scenes[i].used; return count;
}
static void reboot_storage(void) {
    memset(&app, 0, sizeof(app)); scene_store_ready = false; memset(&pending, 0, sizeof(pending));
}
static void scene_tests(void) {
    static const char *names[] = {"Focus", "Blue hour", "Ember", "Afterglow"};
    static const uint8_t brightness[] = {80,72,60,55};
    static const kl_rgb rgb[] = {{36,92,255},{36,92,255},{255,112,38},{178,138,255}};
    reset(); app.desired = kl_state_default(); app.desired.recording_lock = true; app.revision = 53;
    kl_state desired = app.desired;
    CHECK(app_storage_init() == ESP_OK && writes == 1 && commits == 1 && scene_count() == 4);
    CHECK(!memcmp(&desired, &app.desired, sizeof(desired)) && app.revision == 53 && !app.output_revision);
    for (unsigned i = 0; i < 4; ++i) {
        const app_scene *s = &app.scenes[i];
        CHECK(s->used && !strcmp(s->name, names[i]) && s->state.brightness == brightness[i]);
        CHECK(s->state.power && s->state.mode == (i ? KL_COLOR : KL_WHITE) && s->state.temperature_k == 4200);
        CHECK(!memcmp(&s->state.rgb, &rgb[i], sizeof(rgb[i])) && s->state.transition_ms == 1200);
        CHECK(s->state.effect == KL_EFFECT_NONE && !s->state.recording_lock);
    }
    app_scene initial[KL_SCENES]; memcpy(initial, app.scenes, sizeof(initial));
    reboot_storage(); CHECK(app_storage_init() == ESP_OK && writes == 1 && !memcmp(initial, app.scenes, sizeof(initial)));
    /* Editing and deletion persist the marker, so deleted defaults stay gone. */
    app_scene edited = custom_scene("My Focus"), empty = {0};
    CHECK(app_scene_save(0, &edited) == ESP_OK && app_scene_save(1, &empty) == ESP_OK);
    unsigned before = writes; reboot_storage(); CHECK(app_storage_init() == ESP_OK && writes == before);
    CHECK(scene_count() == 3 && !strcmp(app.scenes[0].name, "My Focus") && !app.scenes[1].used);
    for (unsigned i = 0; i < KL_SCENES; ++i) CHECK(app_scene_save(i, &empty) == ESP_OK);
    before = writes; reboot_storage(); CHECK(app_storage_init() == ESP_OK && !scene_count() && writes == before);

    /* Legacy IDs and values, including a deleted legacy slot, are reserved.
     * A user-modified lower-case default prevents a duplicate named default. */
    reset(); app_scene custom = custom_scene("Portrait"), focus = custom_scene("focus");
    put_fixture("scene0_v1", &custom, sizeof(custom)); put_fixture("scene1_v1", &focus, sizeof(focus));
    put_fixture("scene5_v1", &empty, sizeof(empty));
    app_config config = {.name = "Studio", .ssid = "Saved WiFi", .password = "Saved password"};
    clients_record clients = {.count = 1, .labels = {"Trusted"}, .hashes = {{1,2,3,4}}};
    put_fixture("config_v1", &config, sizeof(config)); put_fixture("clients_v1", &clients, sizeof(clients));
    CHECK(app_storage_init() == ESP_OK && writes == 1 && scene_count() == 5);
    CHECK(!memcmp(&app.scenes[0], &custom, sizeof(custom)) && !memcmp(&app.scenes[1], &focus, sizeof(focus)));
    for (unsigned i = 2; i <= 4; ++i) CHECK(!strcmp(app.scenes[i].name, names[i - 1]));
    CHECK(!app.scenes[5].used && !memcmp(find_blob("scene0_v1")->data, &custom, sizeof(custom)));
    CHECK(!memcmp(find_blob("config_v1")->data, &config, sizeof(config))
          && !memcmp(find_blob("clients_v1")->data, &clients, sizeof(clients)));
    CHECK(!memcmp(&app.config, &config, sizeof(config)) && app.token_count == 1);
    /* A full collection is still marked seeded. A later deletion does not
     * opportunistically fill the vacancy with a missing default. */
    reset();
    for (unsigned i = 0; i < KL_SCENES; ++i) {
        char key[12], name[16]; snprintf(key, sizeof(key), "scene%u_v1", i); snprintf(name, sizeof(name), "User %u", i);
        app_scene scene = custom_scene(name); put_fixture(key, &scene, sizeof(scene));
    }
    CHECK(app_storage_init() == ESP_OK && scene_count() == 8 && writes == 1);
    CHECK(app_scene_save(3, &empty) == ESP_OK); before = writes; reboot_storage();
    CHECK(app_storage_init() == ESP_OK && scene_count() == 7 && !app.scenes[3].used && writes == before);

    /* Failed first migration publishes no defaults. After reset the one blob
     * is either absent or complete, never a scene/marker mismatch. */
    for (int fail = 1; fail <= 3; ++fail) for (unsigned durable = 0; durable < 2; ++durable) {
        reset(); failure = fail; commit_persists_on_failure = durable != 0;
        CHECK(app_storage_init() != ESP_OK && !scene_count() && !scene_store_ready);
        CHECK(app_scene_save(0, &custom) == ESP_ERR_INVALID_STATE);
        failure = 0; reboot_storage(); CHECK(app_storage_init() == ESP_OK && scene_count() == 4);
        CHECK(find_blob("scenes_v2") && find_blob("scenes_v2")->size == SCENE_RECORD_BYTES);
    }
    reset(); put_fixture("scene0_v1", &custom, sizeof(custom)); failure = 2;
    CHECK(app_storage_init() != ESP_OK && scene_count() == 1 && !scene_store_ready);
    CHECK(!memcmp(&app.scenes[0], &custom, sizeof(custom)) && !find_blob("scenes_v2"));
    failure = 0; reboot_storage(); CHECK(app_storage_init() == ESP_OK && scene_count() == 5);
    /* Failed edits preserve RAM and forbid a second edit until a fresh load
     * resolves whether the first write reached durable storage. */
    for (int fail = 1; fail <= 3; ++fail) for (unsigned durable = 0; durable < 2; ++durable) {
        reset(); CHECK(app_storage_init() == ESP_OK); app_scene prior = app.scenes[0];
        failure = fail; commit_persists_on_failure = durable != 0;
        CHECK(app_scene_save(0, &custom) != ESP_OK && !memcmp(&app.scenes[0], &prior, sizeof(prior)));
        before = writes; CHECK(app_scene_save(1, &edited) == ESP_ERR_INVALID_STATE && writes == before);
        failure = 0; reboot_storage(); CHECK(app_storage_init() == ESP_OK);
        CHECK(!strcmp(app.scenes[0].name, fail == 3 && durable ? "Portrait" : "Focus"));
        CHECK(!strcmp(app.scenes[1].name, "Blue hour"));
    }
    /* Corrupt/unreadable canonical records cannot silently fall back to
     * legacy defaults, erase deleted slots, or admit a destructive edit. */
    static const unsigned bad_offsets[] = {0,4,5,6,7,15,16,16+33,16+34,16+35,16+36,16+38,16+43,16+44,16+45,16+46,16+47,16+4*48+1};
    for (unsigned kind = 0; kind < sizeof(bad_offsets)/sizeof(bad_offsets[0]) + 2; ++kind) {
        reset(); CHECK(app_storage_init() == ESP_OK); blob *entry = find_blob("scenes_v2");
        if (kind < sizeof(bad_offsets)/sizeof(bad_offsets[0])) entry->data[bad_offsets[kind]] = 255;
        else if (kind == sizeof(bad_offsets)/sizeof(bad_offsets[0])) --entry->size;
        else read_failure_key = "scenes_v2";
        before = writes; reboot_storage(); CHECK(app_storage_init() != ESP_OK && writes == before && !scene_count());
        CHECK(app_scene_save(0, &custom) == ESP_ERR_INVALID_STATE && writes == before);
    }
    for (unsigned kind = 0; kind < 6; ++kind) {
        reset(); app_scene invalid = custom_scene("Saved");
        if (kind == 0) memset(invalid.name, 'x', sizeof(invalid.name));
        if (kind == 1) invalid.state.brightness = 101;
        put_fixture("scene6_v1", &invalid, sizeof(invalid)); blob *entry = find_blob("scene6_v1");
        if (kind == 2) --entry->size;
        if (kind == 3) entry->data[offsetof(app_scene, used)] = 2;
        if (kind == 4) entry->data[offsetof(app_scene, state) + offsetof(kl_state, power)] = 2;
        if (kind == 5) read_failure_key = "scene6_v1";
        CHECK(app_storage_init() != ESP_OK && !writes && !scene_count());
    }
    reset(); CHECK(app_storage_init() == ESP_OK); before = writes;
    custom.state.brightness = 101;
    CHECK(app_scene_save(0, &custom) == ESP_ERR_INVALID_ARG && app_scene_save(8, &edited) == ESP_ERR_INVALID_ARG);
    CHECK(app_scene_save(0, NULL) == ESP_ERR_INVALID_ARG && writes == before && scene_store_ready);
    reset(); namespace_absent = true;
    CHECK(app_storage_init() == ESP_OK && scene_count() == 4 && writes == 1 && app.config.ssid[0]);
}
static void encoding_storage_tests(void) {
    for (unsigned kind = 0; kind < 6; ++kind) {
        reset(); app.output_encoding = KL_OUTPUT_LINEAR;
        uint8_t stored_value = kind == 2 ? 1 : kind == 3 ? 2 : 0;
        if (kind) put_fixture("out_encoding", &stored_value, sizeof(stored_value));
        if (kind == 4) read_failure_key = "out_encoding";
        if (kind == 5) find_blob("out_encoding")->size = 0;
        CHECK(app_storage_init() == ESP_OK && app.output_encoding == (kind == 2 ? KL_OUTPUT_LINEAR : KL_OUTPUT_SRGB));
        CHECK(writes == 1 && !strcmp(pending.key, "scenes_v2")); /* No startup repair write. */
    }
    reset(); CHECK(app_storage_init() == ESP_OK);
    app_config config = {.name = "Desk", .ssid = "Private SSID", .password = "Private password"};
    clients_record clients = {.count = 1, .labels = {"Owner"}, .hashes = {{7,8,9}}};
    put_fixture("config_v1", &config, sizeof(config)); put_fixture("clients_v1", &clients, sizeof(clients));
    CHECK(app_output_encoding_save(KL_OUTPUT_LINEAR) == ESP_OK && app.output_encoding == KL_OUTPUT_SRGB);
    CHECK(find_blob("out_encoding")->size == 1 && find_blob("out_encoding")->data[0] == 1);
    unsigned before = writes;
    CHECK(app_output_encoding_save((kl_output_encoding)-1) == ESP_ERR_INVALID_ARG);
    CHECK(app_output_encoding_save((kl_output_encoding)2) == ESP_ERR_INVALID_ARG && writes == before);
    for (int fail = 1; fail <= 3; ++fail) {
        failure = fail; unsigned prior_closes = closes;
        CHECK(app_output_encoding_save(KL_OUTPUT_SRGB) != ESP_OK && app.output_encoding == KL_OUTPUT_SRGB);
        CHECK(closes == prior_closes + (fail == 1 ? 0 : 1) && find_blob("out_encoding")->data[0] == 1);
        CHECK(!memcmp(find_blob("config_v1")->data, &config, sizeof(config))
              && !memcmp(find_blob("clients_v1")->data, &clients, sizeof(clients)));
    }
    failure = 0; reboot_storage();
    CHECK(app_storage_init() == ESP_OK && app.output_encoding == KL_OUTPUT_LINEAR
          && !memcmp(&app.config, &config, sizeof(config)) && app.token_count == 1);
}
static void storage_tests(void) {
    char tokens[4][65], token[65], id[17];
    reset(); CHECK(app_storage_init() == ESP_OK && writes == 1 && !app.token_count); CHECK(app.config.ssid[0] && app.config.password[0]);
    app_config saved = app.config;
    for (unsigned i = 0; i < 4; ++i) { pair(tokens[i], "Test client"); CHECK(!app.pair_until_ms && app.token_count == i + 1); }
    app_pair_window(); unsigned prior_writes = writes; CHECK(app_issue_token("Fifth", token) == ESP_ERR_INVALID_STATE && writes == prior_writes);
    for (unsigned i = 0; i < 4; ++i) CHECK(app_token_valid(tokens[i]));
    cJSON *list = app_clients_json(); char *text = cJSON_PrintUnformatted(list); CHECK(text);
    for (unsigned i = 0; i < 4; ++i) CHECK(!strstr(text, tokens[i]));
    free(text); cJSON_Delete(list);
    client_id(app.token_hashes[1], id); CHECK(app_revoke_client(id) == 200 && app.token_count == 3);
    CHECK(!app_token_valid(tokens[1]) && app_token_valid(tokens[0]) && app_token_valid(tokens[2]) && app_token_valid(tokens[3]));
    CHECK(app_revoke_client(id) == 404 && app_revoke_client("bad") == 404);
    pair(token, "Replacement"); CHECK(app.token_count == 4);
    memset(app.token_hashes, 0, sizeof(app.token_hashes)); app.token_count = 0;
    CHECK(app_storage_init() == ESP_OK && app.token_count == 4 && app_token_valid(token));
    CHECK(app_clear_clients() == ESP_OK && !app.token_count && app.pair_until_ms > now_ms);
    CHECK(!app_token_valid(token) && !memcmp(&saved, &app.config, sizeof(saved)));
    app.token_count = 99; CHECK(app_storage_init() == ESP_OK && app.token_count == 0);
    for (int fail = 1; fail <= 4; ++fail) {
        reset(); pair(tokens[0], "Existing"); app_pair_window(); failure = fail <= 3 ? fail : 0; sha_failure = fail == 4;
        memset(token, 0x55, sizeof(token)); CHECK(app_issue_token("New", token) != ESP_OK);
        CHECK(app.token_count == 1 && app.pair_until_ms > now_ms);
        for (unsigned i = 0; i < sizeof(token); ++i) CHECK(!token[i]);
        sha_failure = 0; failure = 0; CHECK(app_token_valid(tokens[0]));
        if (fail <= 3) {
            failure = fail; client_id(app.token_hashes[0], id);
            CHECK(app_revoke_client(id) == 503 && app.token_count == 1 && app_token_valid(tokens[0]));
            CHECK(app_clear_clients() != ESP_OK && app.token_count == 1);
        }
    }
    reset(); app_pair_window(); now_ms = app.pair_until_ms; CHECK(app_issue_token("Expired", token) == ESP_ERR_INVALID_STATE && !writes);
    CHECK(!app_token_valid(NULL) && !app_token_valid("short"));
    const char *bad[] = {"", "has\nnewline", "123456789012345678901234567890123", NULL};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) { app_pair_window(); CHECK(app_issue_token(bad[i], token) == ESP_ERR_INVALID_ARG && !writes); }
    for (unsigned kind = 0; kind < 4; ++kind) {
        reset(); pair(token, "Valid"); blob *entry = find_blob("clients_v1"); CHECK(entry);
        clients_record corrupt; memcpy(&corrupt, entry->data, sizeof(corrupt));
        if (!kind) corrupt.count = 5;
        if (kind == 1) memset(corrupt.labels[0], 'x', 33);
        if (kind == 2) corrupt.labels[0][0] = '\n';
        memcpy(entry->data, &corrupt, sizeof(corrupt)); if (kind == 3) --entry->size;
        memset(&app, 0, sizeof(app)); CHECK(app_storage_init() == ESP_OK && !app.token_count && !app_token_valid(token));
    }
    reset(); failure = 5; CHECK(app_storage_init() != ESP_OK && !writes);
}
static void http_tests(void) {
    char token[65], auth[80]; httpd_req_t request = {.handle = (void *)1, .uri = "/api/v1/pair", .method = HTTP_POST};
    reset(); app_pair_window(); request_reset("{\"label\":\"Browser\"}"); request.content_len = strlen(body);
    CHECK(route(&request) == ESP_OK && response_code == 201 && app.token_count == 1 && connection_close && session_closes == 1);
    cJSON *reply = cJSON_Parse(response_body); CHECK(reply); const cJSON *value = cJSON_GetObjectItemCaseSensitive(reply, "token");
    CHECK(cJSON_IsString(value) && strlen(value->valuestring) == 64); memcpy(token, value->valuestring, 65); cJSON_Delete(reply);
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    const char *write_routes[] = {"/api/v1/state", "/api/v1/settings", "/api/v1/scenes/1", "/api/v1/confirm", "/api/v1/update", "/api/v1/controller/update", "/api/v1/clients/0123456789abcdef"};
    for (unsigned i = 0; i < sizeof(write_routes)/sizeof(write_routes[0]); ++i) {
        request_reset("{}"); request.uri = write_routes[i]; request.method = HTTP_POST; request.content_len = 2;
        CHECK(route(&request) == ESP_OK && response_code == 401 && !request_reads);
    }
    request_reset("{}"); request.uri = "/api/v1/clients"; request.method = HTTP_GET;
    CHECK(route(&request) == ESP_OK && response_code == 401);
    header_auth = auth; CHECK(route(&request) == ESP_OK && response_code == 200 && !strstr(response_body, token));
    const char *bad_hosts[] = {"attacker.invalid", "192.0.2.1:81", "keylight-test.local.attacker", "192.0.2.1@evil", NULL};
    for (unsigned i = 0; i < sizeof(bad_hosts)/sizeof(bad_hosts[0]); ++i) {
        request_reset("{}"); header_host = bad_hosts[i]; header_auth = auth;
        CHECK(route(&request) == ESP_OK && response_code == 403);
    }
    const char *good_hosts[] = {"192.0.2.1", "192.0.2.1:80", "keylight-test", "keylight-test.local", "192.168.4.1"};
    for (unsigned i = 0; i < sizeof(good_hosts)/sizeof(good_hosts[0]); ++i) {
        request_reset("{}"); header_host = good_hosts[i]; header_auth = auth;
        CHECK(route(&request) == ESP_OK && response_code == 200);
    }
    const char *bad_origins[] = {"null", "https://192.0.2.1", "http://evil", "http://192.0.2.1.evil", "http://192.0.2.1/"};
    for (unsigned i = 0; i < sizeof(bad_origins)/sizeof(bad_origins[0]); ++i) {
        request_reset("{}"); header_origin = bad_origins[i]; header_auth = auth;
        CHECK(route(&request) == ESP_OK && response_code == 403);
    }
    request_reset("{}"); header_auth = auth; header_origin = "http://192.0.2.1";
    CHECK(route(&request) == ESP_OK && response_code == 200);
    request.uri = "/api/v1/confirm"; request.method = HTTP_POST;
    CHECK(route(&request) == ESP_OK && response_code == 200 && confirm_calls == 1);
    CHECK(!controller_update_calls && !controller_status_calls);
    request_reset(""); request.uri = "/api/v1/controller/update"; request.method = HTTP_GET;
    CHECK(route(&request) == ESP_OK && response_code == 200 && controller_status_calls == 1 && !controller_update_calls);
    request.method = HTTP_POST;
    CHECK(route(&request) == ESP_OK && response_code == 401 && !controller_update_calls && !request_reads);
    header_auth = auth; header_origin = "http://evil";
    CHECK(route(&request) == ESP_OK && response_code == 403 && !controller_update_calls);
    header_origin = "http://192.0.2.1";
    CHECK(route(&request) == ESP_OK && response_code == 202 && controller_update_calls == 1);
    request.method = HTTP_DELETE;
    CHECK(route(&request) == ESP_OK && response_code == 405 && controller_update_calls == 1);
    request_reset("{\"power\":true}"); header_auth = auth; request.uri = "/api/v1/state"; request.method = HTTP_PATCH; request.content_len = strlen(body);
    CHECK(route(&request) == ESP_OK && response_code == 202 && submit_calls == 1);
    const char *bad_json[] = {"{", "{}{}", "{\"label\":\"a\",\"label\":\"b\"}", "{\"label\":\"\\u0000evil\"}", "[]", "{\"label\":1}", "{\"label\":\"a\",\"extra\":0}"};
    for (unsigned i = 0; i < sizeof(bad_json)/sizeof(bad_json[0]); ++i) {
        request_reset(bad_json[i]); request.uri = "/api/v1/pair"; request.method = HTTP_POST; request.content_len = strlen(body); app_pair_window();
        unsigned before = writes; CHECK(route(&request) == ESP_OK && response_code == 400 && writes == before);
    }
    for (size_t fragment = 1; fragment <= 20; ++fragment) {
        request_reset("{\"label\":\"Client\"}"); request.content_len = strlen(body); chunk = fragment;
        cJSON *parsed = http_read_json(&request); CHECK(parsed && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(parsed, "label"))); cJSON_Delete(parsed);
    }
    request_reset("{}"); request.content_len = 2; receive_delay = 10000; CHECK(!http_read_json(&request));
    request_reset("{}"); request.content_len = 2; chunk = 1; receive_delay = 5000; CHECK(!http_read_json(&request));
    for (unsigned error = 1; error <= 2; ++error) { request_reset("{}"); request.content_len = 2; recv_failure = (int)error; CHECK(!http_read_json(&request)); }
    request_reset("{}"); request.content_len = 0; CHECK(!http_read_json(&request) && !request_reads);
    request.content_len = 2049; CHECK(!http_read_json(&request) && !request_reads);
    request.content_len = 2; header_type = "text/plain"; CHECK(!http_read_json(&request));
    CHECK(app_http_start() == ESP_OK && server_handlers == 5);
}
static void controller_json_tests(void) {
    memset(&app,0,sizeof(app));
    for (unsigned i=0;i<32;++i) descriptor.app_elf_sha256[i]=(uint8_t)(i*8+7);
    cJSON *document=device_json(), *controller=cJSON_GetObjectItemCaseSensitive(document,"controller");
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(document,"firmware_elf_sha256")->valuestring,
        "070f171f272f373f474f575f676f777f878f979fa7afb7bfc7cfd7dfe7eff7ff"));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(controller,"ready")));
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(controller,"backend")->valuestring,"unknown"));
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(controller,"status")->valuestring,"starting"));
    cJSON_Delete(document);
    app.controller_connected=true;app.controller_ready=false;app.controller_part_id=0xbc40;
    app.controller_last_health_ms=1234;app.controller_trial_confirmed=true;
    snprintf(app.controller_backend,sizeof(app.controller_backend),"original");
    snprintf(app.controller_status,sizeof(app.controller_status),"diagnostic");
    document=device_json();controller=cJSON_GetObjectItemCaseSensitive(document,"controller");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(controller,"connected")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(controller,"ready")));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(controller,"trial_confirmed")));
    CHECK(cJSON_GetObjectItemCaseSensitive(controller,"part_id")->valueint==0xbc40);
    CHECK(cJSON_GetObjectItemCaseSensitive(controller,"last_health_ms")->valueint==1234);
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(controller,"status")->valuestring,"diagnostic"));
    cJSON_Delete(document);
}
int main(void) { storage_tests(); scene_tests(); encoding_storage_tests(); http_tests(); controller_json_tests(); printf("PASS %u assertions against actual storage.c/scene_store.c/http_server.c\n", assertions); return 0; }
