#include "app_watchdog.h"

#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "app_wdt";

esp_err_t app_watchdog_ensure_init(void)
{
    esp_err_t err = esp_task_wdt_status(NULL);
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_task_wdt_config_t cfg = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U,
        .idle_core_mask = 0,
        .trigger_panic =
#if defined(CONFIG_ESP_TASK_WDT_PANIC) && CONFIG_ESP_TASK_WDT_PANIC
        true,
#else
        false,
#endif
    };

#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    cfg.idle_core_mask |= 1U << 0;
#endif
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    cfg.idle_core_mask |= 1U << 1;
#endif

    err = esp_task_wdt_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

void app_watchdog_register_current_task(const char *tag)
{
    esp_err_t err = app_watchdog_ensure_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] TWDT init failed: %s", tag, esp_err_to_name(err));
        return;
    }

    err = esp_task_wdt_status(NULL);
    if (err == ESP_OK) {
        return;
    }
    if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "[%s] TWDT status failed: %s", tag, esp_err_to_name(err));
        return;
    }

    err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] TWDT subscribe failed: %s", tag, esp_err_to_name(err));
    }
}

void app_watchdog_reset_current_task(const char *tag)
{
    esp_err_t err = esp_task_wdt_reset();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] TWDT reset failed: %s", tag, esp_err_to_name(err));
    }
}

void app_watchdog_unregister_current_task(const char *tag)
{
    esp_err_t err = esp_task_wdt_status(NULL);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] TWDT status before unsubscribe failed: %s", tag, esp_err_to_name(err));
        return;
    }

    err = esp_task_wdt_delete(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] TWDT unsubscribe failed: %s", tag, esp_err_to_name(err));
    }
}
