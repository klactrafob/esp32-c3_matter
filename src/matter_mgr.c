#include "matter_mgr.h"
#include "esp_log.h"
#include "app_config.h"

static const char *TAG = "matter";

esp_err_t matter_mgr_start(const cfg_t *cfg)
{
    (void)cfg;

#if APP_ENABLE_MATTER
    ESP_LOGI(TAG, "Matter ENABLED (not implemented yet)");
#else
    ESP_LOGW(TAG, "Matter disabled (APP_ENABLE_MATTER=0). Using stub.");
#endif
    return ESP_OK;
}
