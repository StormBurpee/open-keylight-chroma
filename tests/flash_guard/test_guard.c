/* Compile the real guard; deterministic scheduler hooks model ownership,
 * blocked admission and delayed task wakeup without any device operations. */
#include "guard_mocks.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "../../firmware/main/flash_guard.c"

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static struct { unsigned owner; } mutex;
static uint64_t now, take_advance;
static unsigned task, creates, takes, gives, fail_create, fail_take, active_spi, active_flash;
static TickType_t last_ticks;
static void (*while_waiting)(void);

int64_t esp_timer_get_time(void) { return (int64_t)now; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    ++creates; return fail_create ? NULL : &mutex;
}
int xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks) {
    CHECK(handle == &mutex && ticks > 0); ++takes; last_ticks = ticks;
    CHECK((uint64_t)ticks * MOCK_TICK_MS <= 5000u);
    if (while_waiting) { void (*hook)(void) = while_waiting; while_waiting = NULL; hook(); }
    now += take_advance;
    if (fail_take || mutex.owner) return 0;
    mutex.owner = task; return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t handle) {
    CHECK(handle == &mutex && mutex.owner == task); ++gives; mutex.owner = 0;
}
static void reset(void) {
    guard = NULL; mutex.owner = 0; now = 100000; task = 1;
    creates = takes = gives = fail_create = fail_take = active_spi = active_flash = 0;
    take_advance = 0; last_ticks = 0; while_waiting = NULL;
}
static void initialized(void) { reset(); CHECK(app_flash_guard_init() == ESP_OK); CHECK(creates == 1); }

static void lifecycle(void) {
    reset(); CHECK(app_flash_guard_enter(now + 1) == ESP_ERR_INVALID_STATE && takes == 0);
    fail_create = 1; CHECK(app_flash_guard_init() == ESP_ERR_NO_MEM && !guard);
    fail_create = 0; CHECK(app_flash_guard_init() == ESP_OK && creates == 2);
    CHECK(app_flash_guard_enter(now + 1000) == ESP_OK && mutex.owner == 1);
    CHECK(app_flash_guard_init() == ESP_OK && creates == 2 && mutex.owner == 1);
    app_flash_guard_leave(); CHECK(!mutex.owner && gives == 1);
    CHECK(app_flash_guard_enter(now) == ESP_ERR_TIMEOUT && takes == 1);
    CHECK(app_flash_guard_enter(now - 1) == ESP_ERR_TIMEOUT && takes == 1);
}

static void admission_deadlines(void) {
    static const uint64_t intervals[] = {1, 999, 1000, 1001, 9999, 10000, 10001, 4999999, 5000000, 5000001, UINT64_C(100000000)};
    for (unsigned i = 0; i < sizeof(intervals) / sizeof(intervals[0]); ++i) {
        uint64_t span = intervals[i], capped = span < APP_FLASH_GUARD_WAIT_US ? span : APP_FLASH_GUARD_WAIT_US;
        for (unsigned outcome = 0; outcome < 4; ++outcome) {
            initialized(); uint64_t deadline = now + span;
            if (outcome == 0) take_advance = capped - 1;
            if (outcome == 1) take_advance = capped;
            if (outcome == 2) take_advance = capped + 1;
            if (outcome == 3) fail_take = 1;
            esp_err_t result = app_flash_guard_enter(deadline);
            if (!outcome) { CHECK(result == ESP_OK && mutex.owner == task); app_flash_guard_leave(); }
            else {
                CHECK(result == ESP_ERR_TIMEOUT && !mutex.owner);
                CHECK(gives == (outcome == 3 ? 0u : 1u));
            }
            CHECK(last_ticks > 0 && takes == 1);
        }
    }
    initialized(); CHECK(app_flash_guard_deadline() == now + APP_FLASH_GUARD_WAIT_US);
    now = (uint64_t)INT64_MAX; CHECK(app_flash_guard_deadline() == now + APP_FLASH_GUARD_WAIT_US);
    /* Arithmetic overflow defense, independent of the real signed timer's lifetime. */
    now = UINT64_MAX - 1; CHECK(app_flash_guard_deadline() == UINT64_MAX);
}

static void finish_spi_reply(void) {
    CHECK(task == 2 && mutex.owner == 1 && active_spi && !active_flash);
    /* A waiting flash writer cannot begin during READY/length/body progress. */
    for (unsigned phase = 0; phase < 3; ++phase) {
        now += 1000; CHECK(mutex.owner == 1 && active_spi && !active_flash);
    }
    task = 1; active_spi = 0; app_flash_guard_leave(); task = 2;
}

static void exclusion_lifetimes(void) {
    initialized(); CHECK(app_flash_guard_enter(app_flash_guard_deadline()) == ESP_OK); active_spi = 1;
    task = 2; while_waiting = finish_spi_reply;
    CHECK(app_flash_guard_enter(app_flash_guard_deadline()) == ESP_OK && mutex.owner == 2);
    active_flash = 1; CHECK(!active_spi);
    task = 3; CHECK(app_flash_guard_enter(now + 1000) == ESP_ERR_TIMEOUT);
    CHECK(mutex.owner == 2 && active_flash && !active_spi);
    task = 2; active_flash = 0; app_flash_guard_leave();
    task = 3; CHECK(app_flash_guard_enter(app_flash_guard_deadline()) == ESP_OK); active_spi = 1;
    CHECK(!active_flash); active_spi = 0; app_flash_guard_leave();
    CHECK(!mutex.owner && gives == 3);
}

int main(void) {
    lifecycle(); admission_deadlines(); exclusion_lifetimes();
    printf("%u actual flash-guard assertions passed (tick=%ums); no device I/O.\n", checks, (unsigned)MOCK_TICK_MS);
    return 0;
}
