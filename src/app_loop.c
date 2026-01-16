#include "app_loop.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_task_wdt.h"

static const char *TAG = "app_loop";

// Настройки цикла
#define LOOP_PERIOD_MS            20      // период "основного цикла" (20мс = 50 Гц)
#define STATS_PRINT_EVERY_MS      2000    // печать статистики раз в 2 секунды

// Настройки WDT
#define APP_WDT_TIMEOUT_S         5       // таймаут WDT (сек)
#define APP_WDT_PANIC             0       // 0 = перезагрузка без panic-дампа, 1 = panic

static void loop_task(void *arg)
{
    (void)arg;

    // --- Task Watchdog init (IDF 5.x) ---
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = APP_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,          // не трогаем idle tasks
        .trigger_panic = APP_WDT_PANIC
    };

    // Инициализируем WDT один раз (если уже инициализирован — ESP_ERR_INVALID_STATE)
    esp_err_t err = esp_task_wdt_init(&wdt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_task_wdt_init failed: %s", esp_err_to_name(err));
    }

    // Регистрируем текущую задачу в WDT
    err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_task_wdt_add failed: %s", esp_err_to_name(err));
    }

    TickType_t last_wake = xTaskGetTickCount();

    int64_t stats_t0_us = esp_timer_get_time();
    uint64_t sum_work_us = 0;
    uint32_t n = 0;
    uint32_t max_work_us = 0;

    while (1) {
        int64_t t0 = esp_timer_get_time();

        // ====== ТУТ будет твоя логика "основного цикла" ======
        // Например: опрос датчиков, обслуживание state-машин, обработка очередей и т.д.
        // Сейчас пусто.
        // ======================================================

        int64_t t1 = esp_timer_get_time();
        uint32_t work_us = (uint32_t)(t1 - t0);

        sum_work_us += work_us;
        n++;
        if (work_us > max_work_us) max_work_us = work_us;

        // Сбрасываем WDT, раз цикл прошёл
        esp_task_wdt_reset();

        // Периодический лог статистики
        int64_t now_us = t1;
        int64_t elapsed_ms = (now_us - stats_t0_us) / 1000;
        if (elapsed_ms >= STATS_PRINT_EVERY_MS && n > 0) {
            uint32_t avg_work_us = (uint32_t)(sum_work_us / n);

            // "примерная загрузка" по отношению к периоду цикла:
            // load = avg_work / period
            uint32_t period_us = LOOP_PERIOD_MS * 1000;
            uint32_t load_permille = (avg_work_us * 1000UL) / (period_us ? period_us : 1);
            // load_permille=1000 => 100%

            ESP_LOGI(TAG,
                     "loop: avg=%u us, max=%u us, period=%u us, approx_load=%u.%u%%",
                     avg_work_us, max_work_us, period_us,
                     load_permille / 10, load_permille % 10);

            // reset stats
            stats_t0_us = now_us;
            sum_work_us = 0;
            n = 0;
            max_work_us = 0;
        }

        // Ждём до следующего тика периода
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LOOP_PERIOD_MS));
    }
}

esp_err_t app_loop_start(void)
{
    BaseType_t ok = xTaskCreate(loop_task, "app_loop", 4096, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
