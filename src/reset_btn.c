#include "reset_btn.h"
#include "app_config.h"
#include "cfg.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "reset_btn";

static int read_btn_level(void)
{
    return gpio_get_level(APP_RESET_BTN_GPIO);
}

static bool is_pressed(void)
{
    return read_btn_level() == APP_RESET_BTN_ACTIVE_LEVEL;
}

static void reset_btn_task(void *arg)
{
    (void)arg;

    int64_t pressed_since = -1;

    while (1) {
        bool pressed = is_pressed();

        if (pressed) {
            if (pressed_since < 0) {
                // debounce
                vTaskDelay(pdMS_TO_TICKS(APP_RESET_DEBOUNCE_MS));
                if (is_pressed()) {
                    pressed_since = esp_timer_get_time(); // us
                    ESP_LOGW(TAG, "Reset button pressed...");
                }
            } else {
                int64_t now = esp_timer_get_time();
                int64_t held_ms = (now - pressed_since) / 1000;
                if (held_ms >= APP_RESET_HOLD_MS) {
                    ESP_LOGE(TAG, "Factory reset! (held %lld ms)", (long long)held_ms);
                    cfg_factory_reset();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            }
        } else {
            pressed_since = -1;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t reset_btn_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << APP_RESET_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (APP_RESET_BTN_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (APP_RESET_BTN_ACTIVE_LEVEL == 1) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    xTaskCreate(reset_btn_task, "reset_btn", 3072, NULL, 5, NULL);
    return ESP_OK;
}
