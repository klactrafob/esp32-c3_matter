#include "net/mqtt_mgr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

#include "modules/relay/mod_relay.h"

typedef struct {
    bool enabled;
    bool discovery;
    bool retain;
    int port;
    char host[96];
    char uri[128];
    char username[64];
    char password[64];
    char client_id[64];
    char topic_prefix[96];
    char discovery_prefix[64];
    char device_name[64];
} mqtt_cfg_t;

static const char *TAG = "mqtt_mgr";

static mqtt_cfg_t s_cfg;
static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
static bool s_last_state_valid = false;
static bool s_last_state_on = false;

static char s_node_id[40] = {0};
static char s_unique_id[48] = {0};
static char s_topic_command[128] = {0};
static char s_topic_state[128] = {0};
static char s_topic_availability[128] = {0};
static char s_topic_discovery[192] = {0};

static bool str_ieq(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }

    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return (*a == 0 && *b == 0);
}

static const cJSON *jobj(const cJSON *o, const char *key)
{
    if (!cJSON_IsObject((cJSON *)o)) {
        return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive((cJSON *)o, key);
}

static const char *jstr(const cJSON *o, const char *key, const char *def)
{
    const cJSON *it = jobj(o, key);
    if (cJSON_IsString(it) && it->valuestring) {
        return it->valuestring;
    }
    return def;
}

static bool jbool(const cJSON *o, const char *key, bool def)
{
    const cJSON *it = jobj(o, key);
    if (cJSON_IsBool(it)) {
        return cJSON_IsTrue(it);
    }
    return def;
}

static int jint(const cJSON *o, const char *key, int def)
{
    const cJSON *it = jobj(o, key);
    if (cJSON_IsNumber(it)) {
        return it->valueint;
    }
    return def;
}

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    snprintf(dst, dst_len, "%s", src ? src : "");
}

static void build_node_id(void)
{
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(s_node_id, sizeof(s_node_id), "esp32c3-%02X%02X%02X", mac[3], mac[4], mac[5]);
    } else {
        snprintf(s_node_id, sizeof(s_node_id), "esp32c3-unknown");
    }
}

static esp_err_t build_topics(void)
{
    const char *prefix = s_cfg.topic_prefix[0] ? s_cfg.topic_prefix : s_node_id;
    const char *disc_prefix = s_cfg.discovery_prefix[0] ? s_cfg.discovery_prefix : "homeassistant";

    int n = snprintf(s_topic_command, sizeof(s_topic_command), "%s/relay/set", prefix);
    if (n < 0 || n >= (int)sizeof(s_topic_command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    n = snprintf(s_topic_state, sizeof(s_topic_state), "%s/relay/state", prefix);
    if (n < 0 || n >= (int)sizeof(s_topic_state)) {
        return ESP_ERR_INVALID_SIZE;
    }
    n = snprintf(s_topic_availability, sizeof(s_topic_availability), "%s/status", prefix);
    if (n < 0 || n >= (int)sizeof(s_topic_availability)) {
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(s_unique_id, sizeof(s_unique_id), "%s_relay", s_node_id);
    n = snprintf(s_topic_discovery, sizeof(s_topic_discovery), "%s/switch/%s/relay/config",
                 disc_prefix, s_node_id);
    if (n < 0 || n >= (int)sizeof(s_topic_discovery)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t parse_cfg(const cJSON *cfg, mqtt_cfg_t *out)
{
    if (!cfg || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    const cJSON *mq = jobj(cfg, "mqtt");
    if (!cJSON_IsObject((cJSON *)mq)) {
        out->enabled = false;
        return ESP_OK;
    }

    out->enabled = jbool(mq, "enable", true);
    out->discovery = jbool(mq, "discovery", true);
    out->retain = jbool(mq, "retain", true);
    out->port = jint(mq, "port", 1883);
    if (out->port <= 0 || out->port > 65535) {
        out->port = 1883;
    }

    copy_str(out->host, sizeof(out->host), jstr(mq, "host", ""));
    copy_str(out->username, sizeof(out->username), jstr(mq, "user", ""));
    copy_str(out->password, sizeof(out->password), jstr(mq, "pass", ""));
    copy_str(out->client_id, sizeof(out->client_id), jstr(mq, "client_id", ""));
    copy_str(out->topic_prefix, sizeof(out->topic_prefix), jstr(mq, "topic_prefix", ""));
    copy_str(out->discovery_prefix, sizeof(out->discovery_prefix), jstr(mq, "discovery_prefix", "homeassistant"));
    copy_str(out->device_name, sizeof(out->device_name), jstr(mq, "device_name", "ESP32 C3 Relay"));

    if (strstr(out->host, "://")) {
        copy_str(out->uri, sizeof(out->uri), out->host);
    } else if (out->host[0] != 0) {
        snprintf(out->uri, sizeof(out->uri), "mqtt://%s:%d", out->host, out->port);
    } else {
        out->uri[0] = 0;
    }

    return ESP_OK;
}

static esp_err_t publish_raw(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t publish_relay_state(bool on, bool force)
{
    if (!force && s_last_state_valid && s_last_state_on == on) {
        return ESP_OK;
    }

    esp_err_t err = publish_raw(s_topic_state, on ? "ON" : "OFF", 1, s_cfg.retain);
    if (err == ESP_OK) {
        s_last_state_valid = true;
        s_last_state_on = on;
    }
    return err;
}

static esp_err_t publish_discovery(void)
{
    if (!s_cfg.discovery) {
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "name", "Relay");
    cJSON_AddStringToObject(root, "uniq_id", s_unique_id);
    cJSON_AddStringToObject(root, "obj_id", "relay");
    cJSON_AddStringToObject(root, "cmd_t", s_topic_command);
    cJSON_AddStringToObject(root, "stat_t", s_topic_state);
    cJSON_AddStringToObject(root, "avty_t", s_topic_availability);
    cJSON_AddStringToObject(root, "pl_on", "ON");
    cJSON_AddStringToObject(root, "pl_off", "OFF");
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");
    cJSON_AddBoolToObject(root, "ret", s_cfg.retain);
    cJSON_AddNumberToObject(root, "qos", 1);

    cJSON *dev = cJSON_AddObjectToObject(root, "dev");
    cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_node_id));
    cJSON_AddStringToObject(dev, "name", s_cfg.device_name[0] ? s_cfg.device_name : "ESP32 C3 Relay");
    cJSON_AddStringToObject(dev, "mdl", "ESP32-C3 MQTT Relay");
    cJSON_AddStringToObject(dev, "sw", "mqtt-relay-1ch");
    cJSON_AddStringToObject(dev, "mf", "Custom");

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = publish_raw(s_topic_discovery, payload, 1, true);
    free(payload);
    return err;
}

static esp_err_t apply_command_payload(const char *data, int len)
{
    if (!data || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char raw[24] = {0};
    int n = (len < (int)sizeof(raw) - 1) ? len : (int)sizeof(raw) - 1;
    memcpy(raw, data, (size_t)n);
    raw[n] = 0;

    char *start = raw;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }

    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        --end;
    }
    *end = 0;

    bool desired = false;
    bool has_desired = true;

    if (str_ieq(start, "ON") || str_ieq(start, "1") || str_ieq(start, "TRUE")) {
        desired = true;
    } else if (str_ieq(start, "OFF") || str_ieq(start, "0") || str_ieq(start, "FALSE")) {
        desired = false;
    } else if (str_ieq(start, "TOGGLE")) {
        bool current = false;
        if (mod_relay_get_state(&current) != ESP_OK) {
            ESP_LOGW(TAG, "toggle rejected: relay module disabled");
            return ESP_ERR_INVALID_STATE;
        }
        desired = !current;
    } else {
        has_desired = false;
    }

    if (!has_desired) {
        ESP_LOGW(TAG, "unsupported payload on %s: '%s'", s_topic_command, start);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = mod_relay_set_state(desired);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "relay set failed from MQTT: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "relay <- MQTT: %s", desired ? "ON" : "OFF");
    return publish_relay_state(desired, true);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (!event) {
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            s_connected = true;
            ESP_LOGI(TAG, "connected to broker");

            int sub_id = esp_mqtt_client_subscribe(s_client, s_topic_command, 1);
            ESP_LOGI(TAG, "subscribed cmd topic: %s (msg_id=%d)", s_topic_command, sub_id);

            if (publish_discovery() != ESP_OK) {
                ESP_LOGW(TAG, "discovery publish failed");
            }
            if (publish_raw(s_topic_availability, "online", 1, true) != ESP_OK) {
                ESP_LOGW(TAG, "availability publish failed");
            }

            bool relay_on = false;
            if (mod_relay_get_state(&relay_on) != ESP_OK) {
                relay_on = false;
            }
            if (publish_relay_state(relay_on, true) != ESP_OK) {
                ESP_LOGW(TAG, "initial relay state publish failed");
            }
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            s_last_state_valid = false;
            ESP_LOGW(TAG, "disconnected from broker");
            break;
        case MQTT_EVENT_DATA:
            if (event->current_data_offset != 0) {
                break;
            }
            if (event->topic && event->topic_len == (int)strlen(s_topic_command) &&
                strncmp(event->topic, s_topic_command, (size_t)event->topic_len) == 0) {
                (void)apply_command_payload(event->data, event->data_len);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "mqtt transport error");
            break;
        default:
            break;
    }
}

static esp_err_t stop_client(void)
{
    if (!s_client) {
        s_connected = false;
        s_last_state_valid = false;
        return ESP_OK;
    }

    if (s_connected) {
        (void)publish_raw(s_topic_availability, "offline", 1, true);
    }

    esp_err_t err = esp_mqtt_client_stop(s_client);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_mqtt_client_stop failed: %s", esp_err_to_name(err));
    }

    err = esp_mqtt_client_destroy(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_mqtt_client_destroy failed: %s", esp_err_to_name(err));
    }

    s_client = NULL;
    s_connected = false;
    s_last_state_valid = false;
    return ESP_OK;
}

esp_err_t mqtt_mgr_start_from_cfg(const cJSON *cfg)
{
    mqtt_cfg_t new_cfg;
    esp_err_t err = parse_cfg(cfg, &new_cfg);
    if (err != ESP_OK) {
        return err;
    }

    build_node_id();
    s_cfg = new_cfg;

    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "MQTT disabled in config");
        return stop_client();
    }

    if (s_cfg.uri[0] == 0) {
        ESP_LOGW(TAG, "MQTT enabled but host is empty (cfg.mqtt.host)");
        return stop_client();
    }

    if (s_cfg.topic_prefix[0] == 0) {
        copy_str(s_cfg.topic_prefix, sizeof(s_cfg.topic_prefix), s_node_id);
    }
    if (s_cfg.client_id[0] == 0) {
        copy_str(s_cfg.client_id, sizeof(s_cfg.client_id), s_node_id);
    }

    err = build_topics();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "starting MQTT client uri=%s topic_prefix=%s", s_cfg.uri, s_cfg.topic_prefix);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_cfg.uri,
        .credentials.username = s_cfg.username[0] ? s_cfg.username : NULL,
        .credentials.client_id = s_cfg.client_id,
        .credentials.authentication.password = s_cfg.password[0] ? s_cfg.password : NULL,
        .session.keepalive = 60,
        .session.last_will.topic = s_topic_availability,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 0,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .network.reconnect_timeout_ms = 5000,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        return ESP_FAIL;
    }

    err = esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        stop_client();
        return err;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        stop_client();
        return err;
    }

    return ESP_OK;
}

esp_err_t mqtt_mgr_restart_from_cfg(const cJSON *cfg)
{
    esp_err_t err = stop_client();
    if (err != ESP_OK) {
        return err;
    }
    return mqtt_mgr_start_from_cfg(cfg);
}

esp_err_t mqtt_mgr_notify_relay_state(bool on)
{
    if (!s_cfg.enabled) {
        return ESP_OK;
    }
    return publish_relay_state(on, false);
}

bool mqtt_mgr_is_connected(void)
{
    return s_connected;
}
