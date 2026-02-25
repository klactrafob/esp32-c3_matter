#include "app_loop.h"
#include <inttypes.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_loop";

#define LOOP_PERIOD_MS       20
#define STATS_PRINT_EVERY_MS 2000

#define APP_WDT_TIMEOUT_S    5
#define APP_WDT_PANIC        0

static void loop_task(void *arg)
{
    (void)arg;

    // Prefer attaching to an existing TWDT instance. Initialize only if needed.
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err == ESP_ERR_INVALID_STATE) {
        esp_task_wdt_config_t wdt_cfg = {
            .timeout_ms = APP_WDT_TIMEOUT_S * 1000,
            .idle_core_mask = 0,
            .trigger_panic = APP_WDT_PANIC,
        };
        err = esp_task_wdt_init(&wdt_cfg);
        if (err == ESP_OK) {
            err = esp_task_wdt_add(NULL);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWDT setup failed: %s", esp_err_to_name(err));
    }

    TickType_t last_wake = xTaskGetTickCount();

    int64_t stats_t0_us = esp_timer_get_time();
    uint64_t sum_work_us = 0;
    uint32_t n = 0;
    uint32_t max_work_us = 0;

    while (1) {
        int64_t t0 = esp_timer_get_time();

        // Main periodic application cycle goes here.

        int64_t t1 = esp_timer_get_time();
        uint32_t work_us = (uint32_t)(t1 - t0);

        sum_work_us += work_us;
        n++;
        if (work_us > max_work_us) {
            max_work_us = work_us;
        }

        if (esp_task_wdt_reset() != ESP_OK) {
            ESP_LOGW(TAG, "TWDT reset failed");
        }

        int64_t elapsed_ms = (t1 - stats_t0_us) / 1000;
        if (elapsed_ms >= STATS_PRINT_EVERY_MS && n > 0) {
            uint32_t avg_work_us = (uint32_t)(sum_work_us / n);
            uint32_t period_us = LOOP_PERIOD_MS * 1000;
            uint32_t load_permille = (avg_work_us * 1000UL) / (period_us ? period_us : 1);
            uint32_t load_pct_int = load_permille / 10U;
            uint32_t load_pct_dec = load_permille % 10U;

            ESP_LOGI(TAG,
                     "loop: avg=%" PRIu32 " us, max=%" PRIu32 " us, period=%" PRIu32 " us, approx_load=%" PRIu32 ".%" PRIu32 "%%",
                     avg_work_us, max_work_us, period_us, load_pct_int, load_pct_dec);

            stats_t0_us = t1;
            sum_work_us = 0;
            n = 0;
            max_work_us = 0;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LOOP_PERIOD_MS));
    }
}

esp_err_t app_loop_start(void)
{
    BaseType_t ok = xTaskCreate(loop_task, "app_loop", 4096, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
