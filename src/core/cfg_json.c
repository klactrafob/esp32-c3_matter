#include "cfg_json.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "cfg_json";
static const char *NVS_NS = "cfg";
static const char *NVS_KEY = "json";

static cJSON *s_cfg = NULL;

static cJSON *make_default_cfg(void)
{
    // Base default: AP/STA + MQTT + single relay module.
    cJSON *root = cJSON_CreateObject();

    cJSON *net = cJSON_AddObjectToObject(root, "net");
    cJSON_AddStringToObject(net, "hostname", "esp32-c3");

    cJSON *ap = cJSON_AddObjectToObject(net, "ap");
    cJSON_AddStringToObject(ap, "ssid", "ESP32-SETUP");
    cJSON_AddStringToObject(ap, "pass", "");

    cJSON *sta = cJSON_AddObjectToObject(net, "sta");
    cJSON_AddStringToObject(sta, "ssid", "");
    cJSON_AddStringToObject(sta, "pass", "");

    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enable", true);
    cJSON_AddStringToObject(mqtt, "host", "");
    cJSON_AddNumberToObject(mqtt, "port", 1883);
    cJSON_AddStringToObject(mqtt, "user", "");
    cJSON_AddStringToObject(mqtt, "pass", "");
    cJSON_AddStringToObject(mqtt, "client_id", "");
    cJSON_AddStringToObject(mqtt, "topic_prefix", "esp32-c3/relay1");
    cJSON_AddStringToObject(mqtt, "discovery_prefix", "homeassistant");
    cJSON_AddStringToObject(mqtt, "device_name", "ESP32 C3 Relay");
    cJSON_AddBoolToObject(mqtt, "discovery", true);
    cJSON_AddBoolToObject(mqtt, "retain", true);

    cJSON *mods = cJSON_AddObjectToObject(root, "modules");
    cJSON *relay = cJSON_AddObjectToObject(mods, "relay");
    cJSON_AddBoolToObject(relay, "enable", true);
    cJSON_AddNumberToObject(relay, "gpio", 12);
    cJSON_AddNumberToObject(relay, "active_level", 1);
    cJSON_AddBoolToObject(relay, "default_on", false);

    return root;
}

static esp_err_t nvs_write_string(const char *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, NVS_KEY, s);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_read_string(char **out)
{
    *out = NULL;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = 0;
    err = nvs_get_str(h, NVS_KEY, NULL, &len);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    char *buf = (char *)calloc(1, len);
    if (!buf) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(h, NVS_KEY, buf, &len);
    nvs_close(h);

    if (err != ESP_OK) {
        free(buf);
        return err;
    }

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
            if (s_cfg) {
                cJSON_Delete(s_cfg);
            }
            s_cfg = parsed;
            ESP_LOGI(TAG, "Loaded config from NVS");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Config in NVS is invalid JSON, using default");
    } else {
        ESP_LOGW(TAG, "No config in NVS, using default");
    }

    if (s_cfg) {
        cJSON_Delete(s_cfg);
    }
    s_cfg = make_default_cfg();

    char *out = cJSON_PrintUnformatted(s_cfg);
    if (!out) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_write_string(out);
    free(out);

    ESP_LOGI(TAG, "Saved default config to NVS");
    return err;
}

esp_err_t cfg_json_set_and_save(const cJSON *new_cfg)
{
    if (!new_cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *dup = cJSON_Duplicate((cJSON *)new_cfg, 1);
    if (!dup) {
        return ESP_ERR_NO_MEM;
    }

    char *out = cJSON_PrintUnformatted(dup);
    if (!out) {
        cJSON_Delete(dup);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_write_string(out);
    free(out);

    if (err == ESP_OK) {
        if (s_cfg) {
            cJSON_Delete(s_cfg);
        }
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
    if (!def) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = cfg_json_set_and_save(def);
    cJSON_Delete(def);
    return err;
}

static bool upsert_bool(cJSON *obj, const char *key, bool value)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(it) && cJSON_IsTrue(it) == value) {
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddBoolToObject(obj, key, value);
    return true;
}

static bool upsert_number(cJSON *obj, const char *key, int value)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it) && it->valueint == value) {
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddNumberToObject(obj, key, value);
    return true;
}

static bool ensure_bool(cJSON *obj, const char *key, bool value)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(it)) {
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddBoolToObject(obj, key, value);
    return true;
}

static bool ensure_number(cJSON *obj, const char *key, int value)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) {
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddNumberToObject(obj, key, value);
    return true;
}

static bool ensure_string(cJSON *obj, const char *key, const char *value)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddStringToObject(obj, key, value ? value : "");
    return true;
}

static bool delete_if_present(cJSON *obj, const char *key)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) {
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    return true;
}

esp_err_t cfg_json_force_relay_gpio12_profile(void)
{
    if (!cJSON_IsObject(s_cfg)) {
        return ESP_ERR_INVALID_STATE;
    }

    bool changed = false;

    cJSON *mods = cJSON_GetObjectItemCaseSensitive(s_cfg, "modules");
    if (!cJSON_IsObject(mods)) {
        cJSON_DeleteItemFromObjectCaseSensitive(s_cfg, "modules");
        mods = cJSON_AddObjectToObject(s_cfg, "modules");
        if (!mods) {
            return ESP_ERR_NO_MEM;
        }
        changed = true;
    }

    cJSON *relay = cJSON_GetObjectItemCaseSensitive(mods, "relay");
    if (!cJSON_IsObject(relay)) {
        cJSON_DeleteItemFromObjectCaseSensitive(mods, "relay");
        relay = cJSON_AddObjectToObject(mods, "relay");
        if (!relay) {
            return ESP_ERR_NO_MEM;
        }
        changed = true;
    }

    changed |= upsert_bool(relay, "enable", true);
    changed |= upsert_number(relay, "gpio", 12);
    changed |= upsert_number(relay, "active_level", 1);
    changed |= upsert_bool(relay, "default_on", false);

    // Cleanup leftovers from older multi-module firmware.
    changed |= delete_if_present(mods, "pwm");
    changed |= delete_if_present(mods, "ws2812");

    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(s_cfg, "mqtt");
    if (!cJSON_IsObject(mqtt)) {
        cJSON_DeleteItemFromObjectCaseSensitive(s_cfg, "mqtt");
        mqtt = cJSON_AddObjectToObject(s_cfg, "mqtt");
        if (!mqtt) {
            return ESP_ERR_NO_MEM;
        }
        changed = true;
    }

    changed |= ensure_bool(mqtt, "enable", true);
    changed |= ensure_string(mqtt, "host", "");
    changed |= ensure_number(mqtt, "port", 1883);
    changed |= ensure_string(mqtt, "user", "");
    changed |= ensure_string(mqtt, "pass", "");
    changed |= ensure_string(mqtt, "client_id", "");
    changed |= ensure_string(mqtt, "topic_prefix", "esp32-c3/relay1");
    changed |= ensure_string(mqtt, "discovery_prefix", "homeassistant");
    changed |= ensure_string(mqtt, "device_name", "ESP32 C3 Relay");
    changed |= ensure_bool(mqtt, "discovery", true);
    changed |= ensure_bool(mqtt, "retain", true);

    if (!changed) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Applying fixed device profile: relay on GPIO12");
    return cfg_json_set_and_save(s_cfg);
}

esp_err_t cfg_json_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
