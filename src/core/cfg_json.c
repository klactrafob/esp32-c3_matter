#include "cfg_json.h"
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "cfg_json";
static const char *NVS_NS = "cfg";
static const char *NVS_KEY = "json";

static cJSON *s_cfg = NULL;

static cJSON *make_default_cfg(void)
{
    // Базовый дефолт: AP/STA + modules (relay/pwm/ws2812)
    cJSON *root = cJSON_CreateObject();

    cJSON *net = cJSON_AddObjectToObject(root, "net");
    cJSON_AddStringToObject(net, "hostname", "esp32-c3");

    cJSON *ap = cJSON_AddObjectToObject(net, "ap");
    cJSON_AddStringToObject(ap, "ssid", "ESP32-SETUP");
    cJSON_AddStringToObject(ap, "pass", "12345678");

    cJSON *sta = cJSON_AddObjectToObject(net, "sta");
    cJSON_AddStringToObject(sta, "ssid", "");
    cJSON_AddStringToObject(sta, "pass", "");

    cJSON *mods = cJSON_AddObjectToObject(root, "modules");

    cJSON *relay = cJSON_AddObjectToObject(mods, "relay");
    cJSON_AddBoolToObject(relay, "enable", false);
    cJSON_AddNumberToObject(relay, "gpio", 4);
    cJSON_AddNumberToObject(relay, "active_level", 1);
    cJSON_AddBoolToObject(relay, "default_on", false);

    cJSON *pwm = cJSON_AddObjectToObject(mods, "pwm");
    cJSON_AddBoolToObject(pwm, "enable", false);
    cJSON_AddNumberToObject(pwm, "gpio", 5);
    cJSON_AddNumberToObject(pwm, "freq", 20000);
    cJSON_AddNumberToObject(pwm, "res_bits", 10);
    cJSON_AddNumberToObject(pwm, "duty", 0); // 0..100

    cJSON *ws = cJSON_AddObjectToObject(mods, "ws2812");
    cJSON_AddBoolToObject(ws, "enable", false);
    cJSON_AddNumberToObject(ws, "gpio", 8);
    cJSON_AddNumberToObject(ws, "count", 30);
    cJSON_AddNumberToObject(ws, "brightness", 50); // 0..100
    cJSON_AddStringToObject(ws, "effect", "solid");

    return root;
}

static esp_err_t nvs_write_string(const char *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, NVS_KEY, s);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_read_string(char **out)
{
    *out = NULL;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = 0;
    err = nvs_get_str(h, NVS_KEY, NULL, &len);
    if (err != ESP_OK) { nvs_close(h); return err; }

    char *buf = (char*)calloc(1, len);
    if (!buf) { nvs_close(h); return ESP_ERR_NO_MEM; }

    err = nvs_get_str(h, NVS_KEY, buf, &len);
    nvs_close(h);

    if (err != ESP_OK) { free(buf); return err; }

    *out = buf;
    return ESP_OK;
}

const cJSON *cfg_json_get(void)
{
    return s_cfg;
}

esp_err_t cfg_json_load_or_default(void)
{
    char *json = NULL;
    esp_err_t err = nvs_read_string(&json);

    if (err == ESP_OK && json) {
        cJSON *parsed = cJSON_Parse(json);
        free(json);

        if (parsed) {
            if (s_cfg) cJSON_Delete(s_cfg);
            s_cfg = parsed;
            ESP_LOGI(TAG, "Loaded config from NVS");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Config in NVS is invalid JSON, using default");
    } else {
        ESP_LOGW(TAG, "No config in NVS, using default");
    }

    if (s_cfg) cJSON_Delete(s_cfg);
    s_cfg = make_default_cfg();

    char *out = cJSON_PrintUnformatted(s_cfg);
    if (!out) return ESP_ERR_NO_MEM;

    err = nvs_write_string(out);
    free(out);

    ESP_LOGI(TAG, "Saved default config to NVS");
    return err;
}

esp_err_t cfg_json_set_and_save(const cJSON *new_cfg)
{
    if (!new_cfg) return ESP_ERR_INVALID_ARG;

    cJSON *dup = cJSON_Duplicate((cJSON*)new_cfg, 1);
    if (!dup) return ESP_ERR_NO_MEM;

    char *out = cJSON_PrintUnformatted(dup);
    if (!out) { cJSON_Delete(dup); return ESP_ERR_NO_MEM; }

    esp_err_t err = nvs_write_string(out);
    free(out);

    if (err == ESP_OK) {
        if (s_cfg) cJSON_Delete(s_cfg);
        s_cfg = dup;
        ESP_LOGI(TAG, "Saved config to NVS");
        return ESP_OK;
    }

    cJSON_Delete(dup);
    return err;
}

esp_err_t cfg_json_reset_to_default(void)
{
    cJSON *def = make_default_cfg();
    if (!def) return ESP_ERR_NO_MEM;

    esp_err_t err = cfg_json_set_and_save(def);
    cJSON_Delete(def);
    return err;
}

esp_err_t cfg_json_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    return err;
}
