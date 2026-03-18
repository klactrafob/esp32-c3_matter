#include "drivers/reset_btn.h"
#include "app_config.h"
#include "app_watchdog.h"

#include "core/cfg_json.h"
#include "core/modules.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
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

static void do_connectivity_reset(void)
{
    ESP_LOGW(TAG, "CONNECTIVITY RESET: Wi-Fi + MQTT");

    esp_err_t err = cfg_json_clear_connectivity();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cfg_json_clear_connectivity: %s", esp_err_to_name(err));
    }

    err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_restore: %s", esp_err_to_name(err));
    }

    // Avoid reset loop while button is still held.
    ESP_LOGW(TAG, "Waiting for button release...");
    while (is_pressed()) {
        app_watchdog_reset_current_task(TAG);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void handle_short_press(void)
{
    bool any_on = modules_is_any_output_on();
    bool target_on = !any_on;

    esp_err_t err = modules_set_master_output(target_on);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Short press ignored: no active output modules");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Short press output toggle failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Short press -> outputs %s", target_on ? "ON" : "OFF");
}

static void reset_btn_task(void *arg)
{
    (void)arg;
    int64_t pressed_since_us = -1;
    app_watchdog_register_current_task(TAG);

    while (1) {
        if (is_pressed()) {
            if (pressed_since_us < 0) {
                vTaskDelay(pdMS_TO_TICKS(APP_RESET_DEBOUNCE_MS));
                if (is_pressed()) {
                    pressed_since_us = esp_timer_get_time();
                    ESP_LOGW(TAG, "Reset button pressed...");
                }
            } else {
                int64_t held_ms = (esp_timer_get_time() - pressed_since_us) / 1000;
                if (held_ms >= APP_RESET_HOLD_MS) {
                    ESP_LOGW(TAG, "Connectivity reset! (held %lld ms)", (long long)held_ms);
                    do_connectivity_reset();
                }
            }
        } else {
            if (pressed_since_us >= 0) {
                int64_t held_ms = (esp_timer_get_time() - pressed_since_us) / 1000;
                if (held_ms >= APP_RESET_DEBOUNCE_MS && held_ms < APP_RESET_HOLD_MS) {
                    handle_short_press();
                }
            }
            pressed_since_us = -1;
        }

        app_watchdog_reset_current_task(TAG);
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
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    xTaskCreate(reset_btn_task, "reset_btn", 3072, NULL, 5, NULL);
    return ESP_OK;
}
