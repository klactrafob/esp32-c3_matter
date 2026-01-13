#include "esp_log.h"
#include "nvs_flash.h"

#include "cfg.h"
#include "wifi_mgr.h"
#include "web_server.h"
#include "device_state.h"
#include "matter_mgr.h"
#include "reset_btn.h"

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

    cfg_t cfg;
    cfg_load_or_default(&cfg);

    device_state_init();

    ESP_ERROR_CHECK(wifi_mgr_start(&cfg));
    ESP_ERROR_CHECK(web_server_start(&cfg));
    ESP_ERROR_CHECK(reset_btn_start());

    ESP_ERROR_CHECK(matter_mgr_start(&cfg)); // пока заглушка

    ESP_LOGI(TAG, "System started");
}
