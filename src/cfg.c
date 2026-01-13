#include "cfg.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "app_config.h"

static const char *TAG = "cfg";
static const char *NVS_NS = "cfg";

void cfg_set_defaults(cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->ap_ssid, APP_AP_SSID_DEFAULT, sizeof(cfg->ap_ssid)-1);
    strncpy(cfg->ap_pass, APP_AP_PASS_DEFAULT, sizeof(cfg->ap_pass)-1);
    strncpy(cfg->hostname, APP_HOSTNAME_DEFAULT, sizeof(cfg->hostname)-1);
}

static esp_err_t open_nvs(nvs_handle_t *out)
{
    return nvs_open(NVS_NS, NVS_READWRITE, out);
}

bool cfg_has_sta_credentials(const cfg_t *cfg)
{
    return (cfg->sta_ssid[0] != '\0');
}

esp_err_t cfg_load_or_default(cfg_t *cfg)
{
    cfg_set_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = open_nvs(&h);
    if (err != ESP_OK) return err;

    size_t len = sizeof(*cfg);
    err = nvs_get_blob(h, "blob", cfg, &len);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Config not found, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load cfg: %s", esp_err_to_name(err));
        cfg_set_defaults(cfg);
        return err;
    }
    return ESP_OK;
}

esp_err_t cfg_save(const cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = open_nvs(&h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, "blob", cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    return err;
}

esp_err_t cfg_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = open_nvs(&h);
    if (err != ESP_OK) return err;

    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    return err;
}
