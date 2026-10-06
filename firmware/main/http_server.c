#include "http_internal.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "keylight_policy.h"
#include "web_assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *status_line(int status) {
    switch (status) {
    case 200: return "200 OK"; case 201: return "201 Created"; case 202: return "202 Accepted";
    case 400: return "400 Bad Request"; case 401: return "401 Unauthorized"; case 403: return "403 Forbidden";
    case 404: return "404 Not Found"; case 405: return "405 Method Not Allowed"; case 409: return "409 Conflict";
    case 408: return "408 Request Timeout";
    case 413: return "413 Content Too Large"; case 423: return "423 Locked"; case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
    }
}

esp_err_t http_json(httpd_req_t *request, int status, cJSON *json) {
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    httpd_resp_set_status(request, status_line(text ? status : 503));
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    /* No unread upload body may become another request on this connection. */
    httpd_resp_set_hdr(request, "Connection", "close");
    esp_err_t result = httpd_resp_send(request, text ? text : "{\"error\":\"Response allocation failed\"}", HTTPD_RESP_USE_STRLEN);
    free(text);
    httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
    return result;
}
esp_err_t http_error(httpd_req_t *request, int status, const char *message) {
    cJSON *json = cJSON_CreateObject(); cJSON_AddStringToObject(json, "error", message);
    return http_json(request, status, json);
}

cJSON *http_read_json(httpd_req_t *request) {
    if (!request->content_len || request->content_len > 2048) return NULL;
    char type[64];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof(type)) != ESP_OK
        || strncmp(type, "application/json", 16) || (type[16] && type[16] != ';')) return NULL;
    size_t length = request->content_len;
    char *body = malloc(length + 1);
    if (!body) return NULL;
    size_t used = 0;
    uint64_t deadline = app_now_ms() + 10000;
    while (used < length) {
        if (app_now_ms() >= deadline) { free(body); return NULL; }
        int received = httpd_req_recv(request, body + used, length - used);
        if (received <= 0 || app_now_ms() >= deadline) { free(body); return NULL; }
        used += received;
    }
    body[length] = 0;
    cJSON *json = NULL;
    if (!memchr(body, 0, length) && !strstr(body, "\\u0000")) json = cJSON_ParseWithLengthOpts(body, length + 1, NULL, true);
    free(body); return json;
}

static bool allowed_host(httpd_req_t *request) {
    char host[80], name[80], ip[16], hostname[40];
    if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) return false;
    char *port = strchr(host, ':');
    if (port) { if (strcmp(port, ":80")) return false; *port = 0; }
    app_lock(); snprintf(ip, sizeof(ip), "%s", app.ip); snprintf(hostname, sizeof(hostname), "%s", app.hostname); app_unlock();
    snprintf(name, sizeof(name), "%s.local", hostname);
    return !strcmp(host, ip) || !strcmp(host, hostname) || !strcmp(host, name) || !strcmp(host, "192.168.4.1");
}
static bool allowed_origin(httpd_req_t *request) {
    if (!httpd_req_get_hdr_value_len(request, "Origin")) return true;
    char origin[100], host[80], expected[100];
    if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK
        || httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) return false;
    snprintf(expected, sizeof(expected), "http://%s", host);
    return !strcmp(origin, expected);
}
static bool authorized(httpd_req_t *request) {
    char header[80];
    return httpd_req_get_hdr_value_str(request, "Authorization", header, sizeof(header)) == ESP_OK
        && !strncmp(header, "Bearer ", 7) && app_token_valid(header + 7);
}

static cJSON *device_json(void) {
    cJSON *json = cJSON_CreateObject();
    app_lock();
    cJSON_AddStringToObject(json, "id", app.id); cJSON_AddStringToObject(json, "name", app.config.name);
    cJSON_AddStringToObject(json, "model", "Open Keylight Chroma");
    cJSON_AddStringToObject(json, "firmware", esp_app_get_description()->version);
    cJSON_AddNumberToObject(json, "api_version", 1); cJSON_AddNumberToObject(json, "uptime_ms", app_now_ms());
    cJSON_AddNumberToObject(json, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(json, "reset_reason", esp_reset_reason());
    cJSON_AddBoolToObject(json, "pairing_open", app_now_ms() < app.pair_until_ms);
    cJSON_AddBoolToObject(json, "trial_pending", app_trial_pending());
    cJSON *network = cJSON_AddObjectToObject(json, "network");
    cJSON_AddBoolToObject(network, "connected", app.network_connected);
    cJSON_AddNumberToObject(network, "rssi", app.rssi); cJSON_AddStringToObject(network, "ip", app.ip);
    cJSON *controller = cJSON_AddObjectToObject(json, "controller");
    cJSON_AddBoolToObject(controller, "connected", app.controller_connected);
    cJSON_AddStringToObject(controller, "version", app.controller_version);
    app_unlock();
    cJSON *capabilities = cJSON_AddObjectToObject(json, "capabilities");
    const char *names[] = {"white", "color", "transitions", "effects", "scenes", "settings", "ota"};
    for (unsigned i = 0; i < 7; i++) cJSON_AddBoolToObject(capabilities, names[i], true);
    cJSON_AddBoolToObject(capabilities, "white_transitions", false);
    const char *effects[] = {"none", "aurora", "breathe"};
    cJSON_AddItemToObject(capabilities, "effect_names", cJSON_CreateStringArray(effects, 3));
    return json;
}

static cJSON *history_json(void) {
    cJSON *json = cJSON_CreateObject(), *entries = cJSON_AddArrayToObject(json, "entries");
    app_lock();
    unsigned count = app.history_sequence < KL_HISTORY ? app.history_sequence : KL_HISTORY;
    for (unsigned i = 0; i < count; i++) {
        app_event *e = &app.history[(app.history_sequence - i - 1) % KL_HISTORY];
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "sequence", e->sequence); cJSON_AddNumberToObject(entry, "uptime_ms", e->uptime_ms);
        cJSON_AddStringToObject(entry, "actor", e->actor); cJSON_AddStringToObject(entry, "event", e->event);
        cJSON_AddStringToObject(entry, "detail", e->detail); cJSON_AddItemToArray(entries, entry);
    }
    app_unlock(); return json;
}

static esp_err_t route(httpd_req_t *request) {
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    if (!allowed_host(request) || !allowed_origin(request)) return http_error(request, 403, "Unrecognized host or browser origin");
    bool api = !strncmp(request->uri, "/api/", 5);
    if (!strncmp(request->uri, "/api/v1/clients", 15) && !authorized(request))
        return http_error(request, 401, "Provide a paired client access token");
    if (api && request->method != HTTP_GET && strcmp(request->uri, "/api/v1/pair") && !authorized(request))
        return http_error(request, 401, "Pair this client or provide its access token");
    if (request->method == HTTP_GET) {
        if (!strcmp(request->uri, "/api/v1/device")) return http_json(request, 200, device_json());
        if (!strcmp(request->uri, "/api/v1/state")) return http_json(request, 200, app_state_json());
        if (!strcmp(request->uri, "/api/v1/history")) return http_json(request, 200, history_json());
    }
    if (!strcmp(request->uri, "/api/v1/state") && request->method == HTTP_PATCH) {
        cJSON *json = http_read_json(request); kl_patch patch; uint32_t expected; bool has_expected;
        bool valid = kl_json_parse_patch(json, &patch, &expected, &has_expected); cJSON_Delete(json);
        if (!valid) return http_error(request, 400, "Invalid state patch; no changes applied");
        char actor[17] = "api", supplied[17];
        if (httpd_req_get_hdr_value_str(request, "X-Keylight-Actor", supplied, sizeof(supplied)) == ESP_OK
            && (!strcmp(supplied, "dashboard") || !strcmp(supplied, "streamdeck") || !strcmp(supplied, "automation")))
            snprintf(actor, sizeof(actor), "%s", supplied);
        int status = app_submit(&patch, actor, expected, has_expected);
        if (status == 202) return http_json(request, status, app_state_json());
        return http_error(request, status, status == 423 ? "Recording Lock is on; unlock in a separate command" :
            status == 409 ? "State changed; refresh before trying again" : "Command rejected");
    }
    if (!strcmp(request->uri, "/api/v1/pair") && request->method == HTTP_POST) {
        cJSON *json = http_read_json(request);
        cJSON *label = cJSON_GetObjectItemCaseSensitive(json, "label");
        bool valid = cJSON_IsObject(json) && cJSON_GetArraySize(json) == 1 && cJSON_IsString(label)
            && kl_client_label_valid(label->valuestring);
        if (!valid) { cJSON_Delete(json); return http_error(request, 400, "Provide a client label up to 32 bytes"); }
        char token[65];
        esp_err_t issued = app_issue_token(label->valuestring, token);
        cJSON_Delete(json);
        if (issued != ESP_OK) return http_error(request, 403, "Hold the physical button for three seconds to open pairing; four clients maximum");
        cJSON *response = cJSON_CreateObject(); cJSON_AddStringToObject(response, "token", token);
        memset(token, 0, sizeof(token)); return http_json(request, 201, response);
    }
    if (!strcmp(request->uri, "/api/v1/clients") && request->method == HTTP_GET)
        return http_json(request, 200, app_clients_json());
    if (!strcmp(request->uri, "/api/v1/pairing") && request->method == HTTP_POST) {
        app_pair_window();
        cJSON *response = cJSON_CreateObject(); cJSON_AddBoolToObject(response, "pairing_open", true);
        cJSON_AddNumberToObject(response, "duration_ms", 180000);
        return http_json(request, 200, response);
    }
    if (!strncmp(request->uri, "/api/v1/clients/", 16) && request->method == HTTP_DELETE) {
        int status = app_revoke_client(request->uri + 16);
        if (status != 200) return http_error(request, status, "Client could not be revoked");
        cJSON *response = cJSON_CreateObject(); cJSON_AddBoolToObject(response, "revoked", true);
        return http_json(request, 200, response);
    }
    if (!strcmp(request->uri, "/api/v1/confirm") && request->method == HTTP_POST) {
        if (app_trial_confirm() != ESP_OK) return http_error(request, 503, "Trial confirmation could not be persisted");
        cJSON *json = cJSON_CreateObject(); cJSON_AddBoolToObject(json, "confirmed", true); return http_json(request, 200, json);
    }
    if (!strcmp(request->uri, "/api/v1/settings")) return http_settings(request);
    if (!strncmp(request->uri, "/api/v1/scenes", 14)) return http_scenes(request);
    if (!strcmp(request->uri, "/api/v1/update") && request->method == HTTP_POST) return http_update(request);
    if (api) return http_error(request, 404, "Unknown API route");
    if (request->method != HTTP_GET) return http_error(request, 405, "Method not allowed");
    for (unsigned i = 0; i < WEB_ASSET_COUNT; i++) {
        const web_asset *asset = &web_assets[i];
        if (!strcmp(request->uri, asset->path) || (!strcmp(request->uri, "/") && !strcmp(asset->path, "/index.html"))) {
            httpd_resp_set_type(request, asset->content_type);
            httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
            httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
            httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
            return httpd_resp_send(request, (const char *)asset->data, asset->size);
        }
    }
    return http_error(request, 404, "Page not found");
}

esp_err_t app_http_start(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; config.max_uri_handlers = 5; config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 5; config.send_wait_timeout = 5; config.lru_purge_enable = true;
    esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK) return result;
    const httpd_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PATCH, HTTP_PUT, HTTP_DELETE};
    for (unsigned i = 0; i < 5; i++) {
        httpd_uri_t uri = {.uri = "/*", .method = methods[i], .handler = route};
        result = httpd_register_uri_handler(server, &uri);
        if (result != ESP_OK) return result;
    }
    return ESP_OK;
}
