#include "wifi_mgr.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

static const char *TAG = "wifi";

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected, retry...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static esp_err_t start_ap(const cfg_t *cfg)
{
    ESP_LOGI(TAG, "Starting AP: %s", cfg->ap_ssid);

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t ap = {0};
    strncpy((char*)ap.ap.ssid, cfg->ap_ssid, sizeof(ap.ap.ssid)-1);
    strncpy((char*)ap.ap.password, cfg->ap_pass, sizeof(ap.ap.password)-1);
    ap.ap.ssid_len = strlen(cfg->ap_ssid);
    ap.ap.max_connection = 4;
    ap.ap.authmode = (cfg->ap_pass[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

static esp_err_t start_sta(const cfg_t *cfg)
{
    ESP_LOGI(TAG, "Starting STA, SSID: %s", cfg->sta_ssid);

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t sta = {0};
    strncpy((char*)sta.sta.ssid, cfg->sta_ssid, sizeof(sta.sta.ssid)-1);
    strncpy((char*)sta.sta.password, cfg->sta_pass, sizeof(sta.sta.password)-1);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

esp_err_t wifi_mgr_start(const cfg_t *cfg)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (cfg_has_sta_credentials(cfg)) {
        return start_sta(cfg);
    }
    return start_ap(cfg);
}
