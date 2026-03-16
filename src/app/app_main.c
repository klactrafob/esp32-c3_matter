#include "esp_log.h"
#include "nvs_flash.h"

#include "net/wifi_mgr.h"
#include "net/web_server.h"
#include "net/mqtt_mgr.h"
#include "drivers/reset_btn.h"

#include "core/cfg_json.h"
#include "core/modules.h"

#include "device_state.h"
#include "app_watchdog.h"
#include "app_loop.h"
#include "app_config.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "Boot...");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(cfg_json_load_or_default());
    ESP_ERROR_CHECK(cfg_json_force_relay_gpio12_profile());
    ESP_ERROR_CHECK(app_watchdog_ensure_init());
    device_state_init();
    ESP_ERROR_CHECK(modules_init());
    ESP_ERROR_CHECK(modules_apply_config(cfg_json_get()));

    ESP_ERROR_CHECK(reset_btn_start());
    ESP_ERROR_CHECK(wifi_mgr_start_from_cfg(cfg_json_get()));
    ESP_ERROR_CHECK(web_server_start());
    esp_err_t mqtt_err = mqtt_mgr_start_from_cfg(cfg_json_get());
    if (mqtt_err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(mqtt_err));
    }
    ESP_ERROR_CHECK(app_loop_start());

    ESP_LOGI(TAG, "System started");
}
