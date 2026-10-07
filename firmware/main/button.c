#include "app.h"
#include "driver/gpio.h"
#include "okl_button.h"
#include "freertos/task.h"

static void button_task(void *unused) {
    (void)unused;
    bool recovery_held = gpio_get_level(34) == 0;
    okl_button button; okl_button_init(&button, recovery_held, app_now_ms());
    uint64_t recovery_started = app_now_ms();
    unsigned scene_index = 0;
    for (;;) {
        bool pressed = gpio_get_level(34) == 0;
        if (recovery_held && !pressed) recovery_held = false;
        if (recovery_held && app_now_ms() - recovery_started >= 10000) {
            /* Only an uninterrupted hold beginning at boot can revoke all clients. */
            recovery_held = false;
            app_clear_clients();
        }
        unsigned events = 0;
        if (!okl_button_sample(&button, pressed, app_now_ms(), &events)) {
            if (events & OKL_BUTTON_SETUP) {
                /* Opening pairing never waits for SPI or cosmetic feedback. */
                app_pair_window();
                app_lock();
                if (app.controller_ready && app.controller_connected && !app.updating &&
                    !app.desired.recording_lock) {
                    if (!++app.pairing_feedback_generation) ++app.pairing_feedback_generation;
                    app.pairing_feedback_ms = app_now_ms();
                    app.pairing_feedback_revision = app.output_revision;
                    app.pairing_feedback_state_revision = app.revision;
                }
                app_unlock();
            }
            if (events & OKL_BUTTON_SINGLE) {
                app_lock(); bool power = app.desired.power; app_unlock();
                kl_patch patch = {.fields = KL_POWER, .value.power = !power};
                app_submit(&patch, "button", 0, false);
            }
            if (events & OKL_BUTTON_DOUBLE) {
                for (unsigned i = 0; i < KL_SCENES; i++) {
                    unsigned index = (scene_index + i) % KL_SCENES;
                    int result = app_activate_scene(index, "button", 0, false);
                    if (result == 202) { scene_index = (index + 1) % KL_SCENES; break; }
                    if (result != 404) break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
esp_err_t app_button_start(void) {
    gpio_config_t config = {.pin_bit_mask = 1ULL << 34, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) return result;
    return xTaskCreate(button_task, "button", 3072, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
