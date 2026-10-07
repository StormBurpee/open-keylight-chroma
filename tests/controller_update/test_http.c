#include "http_mocks.h"
#include "okl_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

static struct {
    const char *type, *digest;
    bool pending, allocation_fail, get_timeout_fail, sha_start_fail, sha_finish_fail, wrong_digest;
    int begin_status, submit_status, response_result, socket;
    unsigned begins, cancels, submits, reads, sets, restores, allocations, frees, sha_frees;
    unsigned read_fail, read_overflow, sha_update_fail, set_fail;
    uint64_t now, read_delay, finish_delay;
    uint8_t *owned;
    int status;
    cJSON response;
} mock;

static void *test_malloc(size_t size) {
    CHECK(size == OKL_LOADER_PACKAGE_BYTES);
    ++mock.allocations;
    return mock.allocation_fail ? NULL : malloc(size);
}
static void test_free(void *value) { if (value) ++mock.frees; free(value); }
#define malloc test_malloc
#define free test_free
#include "../../firmware/main/controller_update.c"
#undef malloc
#undef free

uint64_t app_now_ms(void) { return mock.now; }
bool app_trial_pending(void) { return mock.pending; }
int app_controller_update_begin(uint32_t *id) { ++mock.begins; *id = 73; return mock.begin_status; }
int app_controller_update_submit(uint32_t id, uint8_t *package, size_t size) {
    ++mock.submits;
    CHECK(id == 73 && size == OKL_LOADER_PACKAGE_BYTES && mock.restores == 1);
    for (size_t i = 0; i < size; i++) CHECK(package[i] == 0x5a);
    if (mock.submit_status == 202) mock.owned = package;
    return mock.submit_status;
}
void app_controller_update_cancel_upload(uint32_t id) { CHECK(id == 73 && !mock.owned); ++mock.cancels; }
int httpd_req_to_sockfd(httpd_req_t *request) { (void)request; return mock.socket; }
int getsockopt(int socket, int level, int option, void *value, socklen_t *size) {
    CHECK(socket == 4 && level == SOL_SOCKET && option == SO_RCVTIMEO && *size == sizeof(struct timeval));
    *(struct timeval *)value = (struct timeval){7, 19};
    return mock.get_timeout_fail ? -1 : 0;
}
int setsockopt(int socket, int level, int option, const void *value, socklen_t size) {
    const struct timeval *timeout = value;
    CHECK(socket == 4 && level == SOL_SOCKET && option == SO_RCVTIMEO && size == sizeof(*timeout));
    ++mock.sets;
    if (timeout->tv_sec == 7 && timeout->tv_usec == 19) ++mock.restores;
    else CHECK(timeout->tv_sec * 1000 + timeout->tv_usec / 1000 > 0 && timeout->tv_sec <= 5);
    return mock.set_fail == mock.sets ? -1 : 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *name, char *out, size_t size) {
    (void)request;
    const char *source = !strcmp(name, "Content-Type") ? mock.type : mock.digest;
    if (!source || strlen(source) >= size) return ESP_FAIL;
    memcpy(out, source, strlen(source) + 1);
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *request, char *buffer, size_t size) {
    (void)request;
    ++mock.reads; mock.now += mock.read_delay;
    if (mock.reads == mock.read_fail) return -1;
    if (mock.reads == mock.read_overflow) return (int)size + 1;
    memset(buffer, 0x5a, size);
    return (int)size;
}
esp_err_t http_error(httpd_req_t *request, int status, const char *message) {
    (void)request; CHECK(message && *message); mock.status = status; return ESP_OK;
}
esp_err_t http_json(httpd_req_t *request, int status, cJSON *json) {
    (void)request; mock.status = status; mock.response = *json; return mock.response_result;
}
cJSON *cJSON_CreateObject(void) { static cJSON json; memset(&json, 0, sizeof(json)); return &json; }
void cJSON_AddBoolToObject(cJSON *json, const char *name, bool value) { CHECK(!strcmp(name, "accepted")); json->accepted = value; }
void cJSON_AddNumberToObject(cJSON *json, const char *name, double value) { CHECK(!strcmp(name, "job_id")); json->job_id = value; }
void mbedtls_sha256_init(mbedtls_sha256_context *sha) { sha->bytes = 0; }
int mbedtls_sha256_starts(mbedtls_sha256_context *sha, int is224) { CHECK(!sha->bytes && !is224); return mock.sha_start_fail; }
int mbedtls_sha256_update(mbedtls_sha256_context *sha, const unsigned char *data, size_t size) {
    CHECK(data && size <= 2048); sha->bytes += size;
    return mock.reads == mock.sha_update_fail;
}
int mbedtls_sha256_finish(mbedtls_sha256_context *sha, unsigned char *digest) {
    CHECK(sha->bytes == OKL_LOADER_PACKAGE_BYTES);
    mock.now += mock.finish_delay;
    memset(digest, mock.wrong_digest ? 0xbb : 0xaa, 32);
    return mock.sha_finish_fail;
}
void mbedtls_sha256_free(mbedtls_sha256_context *sha) { (void)sha; ++mock.sha_frees; }

static httpd_req_t request;
static void reset(void) {
    free(mock.owned);
    memset(&mock, 0, sizeof(mock));
    mock.type = "application/octet-stream";
    mock.digest = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    mock.begin_status = 200; mock.submit_status = 202; mock.socket = 4;
    request.content_len = OKL_LOADER_PACKAGE_BYTES;
}
static void reject(int status) {
    CHECK(http_controller_update(&request) == ESP_OK && mock.status == status);
    CHECK(!mock.owned && mock.submits == 0);
    if (mock.allocations) CHECK(mock.cancels == 1 && mock.sha_frees == 1 && mock.restores == 1);
}

int main(void) {
    reset();
    CHECK(http_controller_update(&request) == ESP_OK && mock.status == 202);
    CHECK(mock.response.accepted && mock.response.job_id == 73 && mock.owned);
    CHECK(mock.begins == 1 && mock.submits == 1 && !mock.cancels && !mock.frees && mock.sha_frees == 1);
    unsigned chunks = mock.reads, sets = mock.sets;
    reset(); mock.response_result = ESP_FAIL;
    CHECK(http_controller_update(&request) == ESP_FAIL && mock.owned && !mock.cancels && !mock.frees);

    const char *bad_types[] = {NULL, "text/plain", "application/octet-stream; charset=utf-8"};
    for (unsigned i = 0; i < sizeof(bad_types) / sizeof(*bad_types); i++) {
        reset(); mock.type = bad_types[i]; reject(400); CHECK(!mock.begins && !mock.reads);
    }
    const char *bad_digests[] = {NULL, "", "a", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "gaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    for (unsigned i = 0; i < sizeof(bad_digests) / sizeof(*bad_digests); i++) {
        reset(); mock.digest = bad_digests[i]; reject(400); CHECK(!mock.begins && !mock.reads);
    }
    size_t bad_sizes[] = {0, 64, 28672, 28735, 28737, SIZE_MAX};
    for (unsigned i = 0; i < sizeof(bad_sizes) / sizeof(*bad_sizes); i++) {
        reset(); request.content_len = bad_sizes[i]; reject(413); CHECK(!mock.begins);
    }
    reset(); mock.pending = true; reject(409); CHECK(!mock.begins);
    reset(); mock.socket = -1; reject(503); CHECK(!mock.begins);
    reset(); mock.get_timeout_fail = true; reject(503); CHECK(!mock.begins);
    reset(); mock.begin_status = 409; reject(409); CHECK(!mock.allocations && !mock.cancels);
    reset(); mock.begin_status = 503; reject(503); CHECK(!mock.allocations && !mock.cancels);
    reset(); mock.allocation_fail = true; reject(503); CHECK(!mock.reads && !mock.frees);
    reset(); mock.sha_start_fail = true; reject(400); CHECK(!mock.reads && mock.frees == 1);
    reset(); mock.sha_finish_fail = true; reject(400); CHECK(mock.frees == 1);
    reset(); mock.wrong_digest = true; reject(400); CHECK(mock.frees == 1);
    reset(); mock.read_delay = 2000; reject(408); CHECK(mock.reads == 15);
    reset(); mock.finish_delay = 30000; reject(408);
    for (unsigned i = 1; i <= chunks; i++) {
        reset(); mock.read_fail = i; reject(408); CHECK(mock.reads == i && mock.frees == 1);
        reset(); mock.read_overflow = i; reject(408); CHECK(mock.reads == i && mock.frees == 1);
        reset(); mock.sha_update_fail = i; reject(400); CHECK(mock.reads == i && mock.frees == 1);
    }
    for (unsigned i = 1; i <= sets; i++) { reset(); mock.set_fail = i; reject(400); CHECK(mock.frees == 1); }
    int admission_failures[] = {400, 409, 503};
    for (unsigned i = 0; i < sizeof(admission_failures) / sizeof(*admission_failures); i++) {
        reset(); mock.submit_status = admission_failures[i];
        CHECK(http_controller_update(&request) == ESP_OK && mock.status == admission_failures[i]);
        CHECK(mock.submits == 1 && mock.cancels == 1 && mock.frees == 1 && !mock.owned);
    }
    reset();
    printf("%u controller upload HTTP assertions passed\n", checks);
    return 0;
}
