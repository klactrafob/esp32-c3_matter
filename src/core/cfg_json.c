#include "cfg_json.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

static const char *TAG = "cfg_json";
static const char *NVS_NS = "cfg";
static const char *NVS_KEY = "json";
static const char *MQTT_DISCOVERY_PREFIX_DEFAULT = "homeassistant";
static const char *MQTT_DEVICE_NAME_DEFAULT = "ESP32 C3 Relay";
static const char *MQTT_LEGACY_TOPIC_PREFIX = "esp32-c3/relay1";

static cJSON *s_cfg = NULL;

static void build_board_node_id(char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }

    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(dst, dst_len, "esp32c3-%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(dst, dst_len, "esp32c3-unknown");
    }
}

static cJSON *make_default_cfg(void)
{
    // Base default: AP/STA + MQTT + single relay module.
    cJSON *root = cJSON_CreateObject();
    char node_id[40] = {0};
    build_board_node_id(node_id, sizeof(node_id));

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
    cJSON_AddStringToObject(mqtt, "server_ip", "");
    cJSON_AddNumberToObject(mqtt, "port", 1883);
    cJSON_AddNumberToObject(mqtt, "server_port", 1883);
    cJSON_AddStringToObject(mqtt, "user", "");
    cJSON_AddStringToObject(mqtt, "login", "");
    cJSON_AddStringToObject(mqtt, "pass", "");
    cJSON_AddStringToObject(mqtt, "password", "");
    cJSON_AddStringToObject(mqtt, "client_id", node_id);
    cJSON_AddStringToObject(mqtt, "topic_prefix", node_id);
    cJSON_AddStringToObject(mqtt, "discovery_prefix", MQTT_DISCOVERY_PREFIX_DEFAULT);
    cJSON_AddStringToObject(mqtt, "device_name", MQTT_DEVICE_NAME_DEFAULT);
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

static bool upsert_string(cJSON *obj, const char *key, const char *value)
{
    const char *val = value ? value : "";
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring && strcmp(it->valuestring, val) == 0) {
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddStringToObject(obj, key, val);
    return true;
}

static const char *jstr_nonnull(cJSON *obj, const char *key)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        return it->valuestring;
    }
    return "";
}

static int jint_or_default(cJSON *obj, const char *key, int def)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) {
        return it->valueint;
    }
    return def;
}

static bool normalize_mqtt_server_aliases(cJSON *mqtt)
{
    bool changed = false;

    const char *host = jstr_nonnull(mqtt, "host");
    const char *server_ip = jstr_nonnull(mqtt, "server_ip");
    if (host[0] != 0) {
        changed |= upsert_string(mqtt, "server_ip", host);
    } else if (server_ip[0] != 0) {
        changed |= upsert_string(mqtt, "host", server_ip);
    }

    const char *user = jstr_nonnull(mqtt, "user");
    const char *login = jstr_nonnull(mqtt, "login");
    if (user[0] != 0) {
        changed |= upsert_string(mqtt, "login", user);
    } else if (login[0] != 0) {
        changed |= upsert_string(mqtt, "user", login);
    }

    const char *pass = jstr_nonnull(mqtt, "pass");
    const char *password = jstr_nonnull(mqtt, "password");
    if (pass[0] != 0) {
        changed |= upsert_string(mqtt, "password", pass);
    } else if (password[0] != 0) {
        changed |= upsert_string(mqtt, "pass", password);
    }

    int port = jint_or_default(mqtt, "port", 0);
    int server_port = jint_or_default(mqtt, "server_port", 0);
    if (port > 0 && port <= 65535) {
        changed |= upsert_number(mqtt, "server_port", port);
    } else if (server_port > 0 && server_port <= 65535) {
        changed |= upsert_number(mqtt, "port", server_port);
        changed |= upsert_number(mqtt, "server_port", server_port);
    } else {
        changed |= upsert_number(mqtt, "port", 1883);
        changed |= upsert_number(mqtt, "server_port", 1883);
    }

    return changed;
}

static bool normalize_mqtt_identity(cJSON *mqtt)
{
    char node_id[40] = {0};
    build_board_node_id(node_id, sizeof(node_id));

    bool changed = false;
    cJSON *topic_prefix = cJSON_GetObjectItemCaseSensitive(mqtt, "topic_prefix");
    if (!cJSON_IsString(topic_prefix) || !topic_prefix->valuestring || topic_prefix->valuestring[0] == 0 ||
        strcmp(topic_prefix->valuestring, MQTT_LEGACY_TOPIC_PREFIX) == 0) {
        changed |= upsert_string(mqtt, "topic_prefix", node_id);
    }

    cJSON *client_id = cJSON_GetObjectItemCaseSensitive(mqtt, "client_id");
    if (!cJSON_IsString(client_id) || !client_id->valuestring || client_id->valuestring[0] == 0) {
        changed |= upsert_string(mqtt, "client_id", node_id);
    }

    return changed;
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
    changed |= ensure_string(mqtt, "server_ip", "");
    changed |= ensure_number(mqtt, "port", 1883);
    changed |= ensure_number(mqtt, "server_port", 1883);
    changed |= ensure_string(mqtt, "user", "");
    changed |= ensure_string(mqtt, "login", "");
    changed |= ensure_string(mqtt, "pass", "");
    changed |= ensure_string(mqtt, "password", "");
    changed |= ensure_string(mqtt, "client_id", "");
    changed |= ensure_string(mqtt, "topic_prefix", "");
    changed |= ensure_string(mqtt, "discovery_prefix", MQTT_DISCOVERY_PREFIX_DEFAULT);
    changed |= ensure_string(mqtt, "device_name", MQTT_DEVICE_NAME_DEFAULT);
    changed |= ensure_bool(mqtt, "discovery", true);
    changed |= ensure_bool(mqtt, "retain", true);
    changed |= normalize_mqtt_server_aliases(mqtt);
    changed |= normalize_mqtt_identity(mqtt);

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
