#include "matter_mgr.h"
#include "esp_log.h"

static const char *TAG = "matter_mgr";

esp_err_t matter_mgr_start(void *unused)
{
    (void)unused;
    ESP_LOGW(TAG, "Matter is not implemented yet (stub).");
    return ESP_OK;
}
