#include "app.h"
#include "driver/gpio.h"
#include "okl_button.h"
#include "freertos/task.h"

static void button_task(void *unused) {
    (void)unused;
    gpio_config_t config = {.pin_bit_mask = 1ULL << 34, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&config);
    okl_button button; okl_button_init(&button, gpio_get_level(34) == 0, app_now_ms());
    bool recovery_held = gpio_get_level(34) == 0;
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
            if (events & OKL_BUTTON_SETUP) app_pair_window();
            if (events & OKL_BUTTON_SINGLE) {
                app_lock(); bool power = app.desired.power; app_unlock();
                kl_patch patch = {.fields = KL_POWER, .value.power = !power};
                app_submit(&patch, "button", 0, false);
            }
            if (events & OKL_BUTTON_DOUBLE) {
                for (unsigned i = 0; i < KL_SCENES; i++) {
                    unsigned index = (scene_index + i) % KL_SCENES;
                    int result = app_activate_scene(index, "button", 0, false);
                    if (result != 404) { scene_index = (index + 1) % KL_SCENES; break; }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
void app_button_start(void) { xTaskCreate(button_task, "button", 3072, NULL, 5, NULL); }
