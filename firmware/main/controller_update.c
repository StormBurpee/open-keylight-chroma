#include "http_internal.h"
#include "okl_loader.h"
#include "mbedtls/sha256.h"
#include "lwip/sockets.h"
#include <stdlib.h>
#include <string.h>

#define CONTROLLER_UPLOAD_MS UINT64_C(30000)
#define RECEIVE_SLICE_MS UINT64_C(5000)

static bool controller_digest(httpd_req_t *request, uint8_t digest[32]) {
    char header[65], content_type[64];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, "application/octet-stream") ||
        httpd_req_get_hdr_value_str(request, "X-SHA256", header, sizeof(header)) != ESP_OK || strlen(header) != 64)
        return false;
    for (unsigned i = 0; i < 32; i++) {
        unsigned byte = 0;
        for (unsigned j = 0; j < 2; j++) {
            char c = header[i * 2 + j];
            unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 16;
            if (digit > 15) return false;
            byte = byte * 16 + digit;
        }
        digest[i] = (uint8_t)byte;
    }
    return true;
}

esp_err_t http_controller_update(httpd_req_t *request) {
    uint8_t expected[32], digest[32];
    uint8_t role = OKL_ROLE_LIGHTING;
    char mode[32];
    size_t mode_size = httpd_req_get_hdr_value_len(request, "X-Controller-Mode");
    if (mode_size) {
        if (mode_size >= sizeof(mode) ||
            httpd_req_get_hdr_value_str(request, "X-Controller-Mode", mode, sizeof(mode)) != ESP_OK ||
            strcmp(mode, "diagnostic-off"))
            return http_error(request, 400, "Controller mode must be diagnostic-off, or omitted for production");
        role = OKL_ROLE_SPI_DIAGNOSTIC;
    }
    if (!controller_digest(request, expected))
        return http_error(request, 400, "Use application/octet-stream and a lowercase X-SHA256 package digest");
    if (request->content_len != OKL_LOADER_PACKAGE_BYTES)
        return http_error(request, 413, "Upload one complete 28,736-byte controller package");
    if (app_trial_pending())
        return http_error(request, 409, "Confirm the ESP application trial before updating its controller");

    int socket = httpd_req_to_sockfd(request);
    struct timeval old_timeout;
    socklen_t timeout_size = sizeof(old_timeout);
    if (socket < 0 || getsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &old_timeout, &timeout_size) != 0)
        return http_error(request, 503, "Cannot bound the upload receive time");
    uint32_t job_id = 0;
    int status = app_controller_update_begin_role(&job_id, role);
    if (status != 200) return http_error(request, status, "Controller update is unavailable or another update is active");

    uint8_t *package = malloc(OKL_LOADER_PACKAGE_BYTES);
    esp_err_t result = package ? ESP_OK : ESP_ERR_NO_MEM;
    uint64_t started = app_now_ms();
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0 && result == ESP_OK) result = ESP_FAIL;
    size_t received = 0;
    while (result == ESP_OK && received < OKL_LOADER_PACKAGE_BYTES) {
        uint64_t elapsed = app_now_ms() - started;
        if (elapsed >= CONTROLLER_UPLOAD_MS) { result = ESP_ERR_TIMEOUT; break; }
        uint64_t remaining = CONTROLLER_UPLOAD_MS - elapsed;
        if (remaining > RECEIVE_SLICE_MS) remaining = RECEIVE_SLICE_MS;
        struct timeval timeout = {.tv_sec = (long)(remaining / 1000), .tv_usec = (long)(remaining % 1000) * 1000};
        if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) { result = ESP_FAIL; break; }
        size_t size = OKL_LOADER_PACKAGE_BYTES - received;
        if (size > 2048) size = 2048;
        int length = httpd_req_recv(request, (char *)package + received, size);
        if (length <= 0 || (size_t)length > size || app_now_ms() - started >= CONTROLLER_UPLOAD_MS) {
            result = ESP_ERR_TIMEOUT; break;
        }
        if (mbedtls_sha256_update(&sha, package + received, (size_t)length) != 0) { result = ESP_FAIL; break; }
        received += (size_t)length;
    }
    if (result == ESP_OK && mbedtls_sha256_finish(&sha, digest) != 0) result = ESP_FAIL;
    mbedtls_sha256_free(&sha);
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &old_timeout, sizeof(old_timeout)) != 0 && result == ESP_OK)
        result = ESP_FAIL;
    if (result == ESP_OK && app_now_ms() - started >= CONTROLLER_UPLOAD_MS) result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK && memcmp(digest, expected, sizeof(digest))) result = ESP_ERR_INVALID_CRC;

    if (result == ESP_OK) {
        /* Admission revalidates the complete package before transferring its
         * ownership to the worker. Receiving bytes never touches the SPI bus. */
        status = app_controller_update_submit(job_id, package, received);
        if (status == 202) {
            cJSON *json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "accepted", true);
            cJSON_AddNumberToObject(json, "job_id", job_id);
            return http_json(request, 202, json);
        }
    } else status = result == ESP_ERR_TIMEOUT ? 408 : result == ESP_ERR_NO_MEM ? 503 : 400;
    free(package);
    app_controller_update_cancel_upload(job_id);
    return http_error(request, status, "Controller package was not admitted; no controller update was started");
}
