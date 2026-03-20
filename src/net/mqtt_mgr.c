#include "net/mqtt_mgr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "mqtt_client.h"

#include "core/modules.h"

static const char *TAG = "mqtt_mgr";

#define MQTT_MAX_ENTITIES 24

typedef enum {
    ENTITY_KIND_NONE = 0,
    ENTITY_KIND_OUTPUT,
    ENTITY_KIND_INPUT,
    ENTITY_KIND_SENSOR,
} entity_kind_t;

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
    char node_id[40];
} mqtt_cfg_t;

typedef struct {
    bool used;
    entity_kind_t kind;
    bool supports_command;
    char id[24];
    char name[48];
    char type[16];
    char role[24];
    char component[24];
    char output_mode[24];
    char source_id[40];
    char metric[24];
    char unique_id[64];
    char command_topic[160];
    char state_topic[160];
    char config_topic[192];
} mqtt_entity_t;

static mqtt_cfg_t s_cfg = {0};
static mqtt_entity_t s_entities[MQTT_MAX_ENTITIES] = {0};
static int s_entity_count = 0;
static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
static char s_availability_topic[160] = {0};

static const cJSON *jobj(const cJSON *obj, const char *key);
static const char *jstr(const cJSON *obj, const char *key, const char *def);
static bool jbool(const cJSON *obj, const char *key, bool def);
static int jint(const cJSON *obj, const char *key, int def);
static void modules_runtime_changed_cb(void *ctx);
static esp_err_t publish_all_states(void);
static esp_err_t apply_number_command(const mqtt_entity_t *entity, const char *data, int len);

static const cJSON *jobj(const cJSON *obj, const char *key)
{
    if (!cJSON_IsObject((cJSON *)obj)) {
        return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
}

static const char *jstr(const cJSON *obj, const char *key, const char *def)
{
    const cJSON *it = jobj(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        return it->valuestring;
    }
    return def;
}

static bool jbool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *it = jobj(obj, key);
    if (cJSON_IsBool(it)) {
        return cJSON_IsTrue(it);
    }
    return def;
}

static int jint(const cJSON *obj, const char *key, int def)
{
    const cJSON *it = jobj(obj, key);
    if (cJSON_IsNumber(it)) {
        return it->valueint;
    }
    return def;
}

static void copy_str(char *dst, size_t len, const char *src)
{
    snprintf(dst, len, "%s", src ? src : "");
}

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
    return *a == 0 && *b == 0;
}

static esp_err_t publish_raw(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

static mqtt_entity_t *find_entity_by_topic(const char *topic, int topic_len)
{
    for (int i = 0; i < s_entity_count; ++i) {
        mqtt_entity_t *entity = &s_entities[i];
        if (!entity->used || !entity->supports_command) {
            continue;
        }
        if ((int)strlen(entity->command_topic) == topic_len &&
            strncmp(entity->command_topic, topic, (size_t)topic_len) == 0) {
            return entity;
        }
    }
    return NULL;
}

static void reset_entities(void)
{
    memset(s_entities, 0, sizeof(s_entities));
    s_entity_count = 0;
}

static bool add_entity(entity_kind_t kind, const char *component, const char *id,
                       const char *name, const char *type, const char *role,
                       const char *source_id, const char *metric, bool supports_command,
                       const char *output_mode)
{
    if (s_entity_count >= MQTT_MAX_ENTITIES) {
        return false;
    }

    mqtt_entity_t *entity = &s_entities[s_entity_count++];
    entity->used = true;
    entity->kind = kind;
    entity->supports_command = supports_command;
    copy_str(entity->id, sizeof(entity->id), id);
    copy_str(entity->name, sizeof(entity->name), name);
    copy_str(entity->type, sizeof(entity->type), type);
    copy_str(entity->role, sizeof(entity->role), role);
    copy_str(entity->component, sizeof(entity->component), component);
    copy_str(entity->output_mode, sizeof(entity->output_mode), output_mode ? output_mode : "");
    copy_str(entity->source_id, sizeof(entity->source_id), source_id ? source_id : id);
    copy_str(entity->metric, sizeof(entity->metric), metric);

    snprintf(entity->unique_id, sizeof(entity->unique_id), "%s_%s", s_cfg.node_id, id);
    snprintf(entity->state_topic, sizeof(entity->state_topic), "%s/%s/state", s_cfg.topic_prefix, id);
    if (supports_command) {
        snprintf(entity->command_topic, sizeof(entity->command_topic), "%s/%s/set", s_cfg.topic_prefix, id);
    }
    snprintf(entity->config_topic, sizeof(entity->config_topic), "%s/%s/%s/%s/config",
             s_cfg.discovery_prefix, component, s_cfg.node_id, id);
    return true;
}

static esp_err_t parse_cfg(const cJSON *cfg, mqtt_cfg_t *out)
{
    if (!cfg || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    const cJSON *device = jobj(cfg, "device");
    const cJSON *conn = jobj(cfg, "connectivity");
    const cJSON *mqtt = jobj(conn, "mqtt");

    copy_str(out->node_id, sizeof(out->node_id), jstr(device, "node_id", "esp32c3-unknown"));
    copy_str(out->device_name, sizeof(out->device_name), jstr(device, "name", "ESP32 C3 MQTT Device"));

    out->enabled = jbool(mqtt, "enable", false);
    out->discovery = jbool(mqtt, "discovery", true);
    out->retain = jbool(mqtt, "retain", true);
    out->port = jint(mqtt, "port", 1883);
    if (out->port <= 0 || out->port > 65535) {
        out->port = 1883;
    }

    copy_str(out->host, sizeof(out->host), jstr(mqtt, "host", ""));
    copy_str(out->username, sizeof(out->username), jstr(mqtt, "user", ""));
    copy_str(out->password, sizeof(out->password), jstr(mqtt, "pass", ""));
    copy_str(out->client_id, sizeof(out->client_id), jstr(mqtt, "client_id", out->node_id));
    copy_str(out->topic_prefix, sizeof(out->topic_prefix), jstr(mqtt, "topic_prefix", out->node_id));
    copy_str(out->discovery_prefix, sizeof(out->discovery_prefix), jstr(mqtt, "discovery_prefix", "homeassistant"));

    if (strstr(out->host, "://")) {
        copy_str(out->uri, sizeof(out->uri), out->host);
    } else if (out->host[0] != 0) {
        snprintf(out->uri, sizeof(out->uri), "mqtt://%s:%d", out->host, out->port);
    }

    snprintf(s_availability_topic, sizeof(s_availability_topic), "%s/status", out->topic_prefix);
    return ESP_OK;
}

static void build_entities_from_status(const cJSON *status)
{
    reset_entities();

    const cJSON *outputs = jobj(status, "outputs");
    const cJSON *inputs = jobj(status, "inputs");
    const cJSON *sensors = jobj(status, "sensors");

    if (cJSON_IsArray((cJSON *)outputs)) {
        int count = cJSON_GetArraySize((cJSON *)outputs);
        for (int i = 0; i < count; ++i) {
            const cJSON *item = cJSON_GetArrayItem((cJSON *)outputs, i);
            if (!jbool(item, "enabled", true)) {
                continue;
            }
            const char *type = jstr(item, "type", "");
            const char *component = "light";
            if (strcmp(type, "relay") == 0) {
                component = "switch";
            } else if (strcmp(type, "servo_3wire") == 0 || strcmp(type, "servo_5wire") == 0) {
                component = "number";
            }
            add_entity(ENTITY_KIND_OUTPUT, component, jstr(item, "id", ""),
                       jstr(item, "name", ""), type, "", jstr(item, "id", ""), "", true,
                       jstr(item, "mode", ""));
        }
    }

    if (cJSON_IsArray((cJSON *)inputs)) {
        int count = cJSON_GetArraySize((cJSON *)inputs);
        for (int i = 0; i < count; ++i) {
            const cJSON *item = cJSON_GetArrayItem((cJSON *)inputs, i);
            if (!jbool(item, "enabled", true)) {
                continue;
            }
            add_entity(ENTITY_KIND_INPUT, "binary_sensor", jstr(item, "id", ""),
                       jstr(item, "name", ""), "digital", jstr(item, "role", ""),
                       jstr(item, "id", ""), "", false, "");
        }
    }

    if (cJSON_IsArray((cJSON *)sensors)) {
        int count = cJSON_GetArraySize((cJSON *)sensors);
        for (int i = 0; i < count; ++i) {
            const cJSON *item = cJSON_GetArrayItem((cJSON *)sensors, i);
            if (!jbool(item, "enabled", true) || !jbool(item, "supported", false)) {
                continue;
            }

            const char *type = jstr(item, "type", "");
            const char *sid = jstr(item, "id", "");
            const char *name = jstr(item, "name", sid);

            if (strcmp(type, "aht20") == 0 || strcmp(type, "sht3x") == 0 || strcmp(type, "bme280") == 0) {
                char id_temp[40] = {0};
                char id_hum[40] = {0};
                char name_temp[64] = {0};
                char name_hum[64] = {0};
                snprintf(id_temp, sizeof(id_temp), "%s_temperature", sid);
                snprintf(id_hum, sizeof(id_hum), "%s_humidity", sid);
                snprintf(name_temp, sizeof(name_temp), "%s Temperature", name);
                snprintf(name_hum, sizeof(name_hum), "%s Humidity", name);
                add_entity(ENTITY_KIND_SENSOR, "sensor", id_temp, name_temp, type, "temperature",
                           sid, "temperature_c", false, "");
                add_entity(ENTITY_KIND_SENSOR, "sensor", id_hum, name_hum, type, "humidity",
                           sid, "humidity_pct", false, "");
                if (strcmp(type, "bme280") == 0) {
                    char id_pressure[40] = {0};
                    char name_pressure[64] = {0};
                    snprintf(id_pressure, sizeof(id_pressure), "%s_pressure", sid);
                    snprintf(name_pressure, sizeof(name_pressure), "%s Pressure", name);
                    add_entity(ENTITY_KIND_SENSOR, "sensor", id_pressure, name_pressure, type, "pressure",
                               sid, "pressure_hpa", false, "");
                }
            } else if (strcmp(type, "ds18b20_bus") == 0) {
                const cJSON *devices = jobj(item, "devices");
                if (!cJSON_IsArray((cJSON *)devices)) {
                    continue;
                }
                cJSON *dev = NULL;
                cJSON_ArrayForEach(dev, (cJSON *)devices) {
                    char entity_name[72] = {0};
                    snprintf(entity_name, sizeof(entity_name), "%s Temperature", jstr(dev, "name", "DS18B20"));
                    add_entity(ENTITY_KIND_SENSOR, "sensor", jstr(dev, "id", ""),
                               entity_name, "ds18b20", "temperature",
                               jstr(dev, "id", ""), "temperature_c", false, "");
                }
            }
        }
    }
}

static cJSON *build_device_obj(void)
{
    cJSON *dev = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_cfg.node_id));
    cJSON_AddStringToObject(dev, "name", s_cfg.device_name);
    cJSON_AddStringToObject(dev, "mdl", "ESP32-C3 MQTT Controller");
    cJSON_AddStringToObject(dev, "sw", "mqtt-configurator-v1");
    cJSON_AddStringToObject(dev, "mf", "Custom");
    return dev;
}

static esp_err_t publish_discovery_entity(const mqtt_entity_t *entity)
{
    if (!s_cfg.discovery || !entity || !entity->used) {
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", entity->name);
    cJSON_AddStringToObject(root, "object_id", entity->id);
    cJSON_AddStringToObject(root, "unique_id", entity->unique_id);
    cJSON_AddStringToObject(root, "availability_topic", s_availability_topic);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");
    cJSON_AddItemToObject(root, "device", build_device_obj());

    if (entity->kind == ENTITY_KIND_OUTPUT && strcmp(entity->type, "relay") == 0) {
        cJSON_AddStringToObject(root, "command_topic", entity->command_topic);
        cJSON_AddStringToObject(root, "state_topic", entity->state_topic);
        cJSON_AddStringToObject(root, "payload_on", "ON");
        cJSON_AddStringToObject(root, "payload_off", "OFF");
        cJSON_AddBoolToObject(root, "retain", s_cfg.retain);
    } else if (entity->kind == ENTITY_KIND_OUTPUT &&
               (strcmp(entity->type, "servo_3wire") == 0 || strcmp(entity->type, "servo_5wire") == 0)) {
        cJSON_AddStringToObject(root, "command_topic", entity->command_topic);
        cJSON_AddStringToObject(root, "state_topic", entity->state_topic);
        cJSON_AddNumberToObject(root, "min", 0);
        cJSON_AddNumberToObject(root, "max", 100);
        cJSON_AddNumberToObject(root, "step", 1);
        cJSON_AddStringToObject(root, "mode", "slider");
        cJSON_AddStringToObject(root, "unit_of_measurement", "%");
    } else if (entity->kind == ENTITY_KIND_OUTPUT) {
        cJSON_AddStringToObject(root, "schema", "json");
        cJSON_AddStringToObject(root, "command_topic", entity->command_topic);
        cJSON_AddStringToObject(root, "state_topic", entity->state_topic);
        cJSON_AddBoolToObject(root, "brightness", true);
        cJSON_AddNumberToObject(root, "brightness_scale", 255);
        if (strcmp(entity->type, "ws2812") == 0 && strcmp(entity->output_mode, "mono_triplet") != 0) {
            cJSON *modes = cJSON_AddArrayToObject(root, "supported_color_modes");
            cJSON_AddItemToArray(modes, cJSON_CreateString("rgb"));
        } else {
            cJSON *modes = cJSON_AddArrayToObject(root, "supported_color_modes");
            cJSON_AddItemToArray(modes, cJSON_CreateString("brightness"));
        }
    } else if (entity->kind == ENTITY_KIND_INPUT) {
        cJSON_AddStringToObject(root, "state_topic", entity->state_topic);
        cJSON_AddStringToObject(root, "payload_on", "ON");
        cJSON_AddStringToObject(root, "payload_off", "OFF");
        if (strcmp(entity->role, "motion") == 0) {
            cJSON_AddStringToObject(root, "device_class", "motion");
        } else if (strcmp(entity->role, "presence") == 0) {
            cJSON_AddStringToObject(root, "device_class", "presence");
        } else if (strcmp(entity->role, "contact") == 0 || strcmp(entity->role, "limit") == 0) {
            cJSON_AddStringToObject(root, "device_class", "door");
        }
    } else if (entity->kind == ENTITY_KIND_SENSOR) {
        cJSON_AddStringToObject(root, "state_topic", entity->state_topic);
        cJSON_AddStringToObject(root, "state_class", "measurement");
        if (strcmp(entity->role, "temperature") == 0) {
            cJSON_AddStringToObject(root, "device_class", "temperature");
            cJSON_AddStringToObject(root, "unit_of_measurement", "C");
        } else if (strcmp(entity->role, "humidity") == 0) {
            cJSON_AddStringToObject(root, "device_class", "humidity");
            cJSON_AddStringToObject(root, "unit_of_measurement", "%");
        } else if (strcmp(entity->role, "pressure") == 0) {
            cJSON_AddStringToObject(root, "device_class", "pressure");
            cJSON_AddStringToObject(root, "unit_of_measurement", "hPa");
        }
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = publish_raw(entity->config_topic, payload, 1, true);
    free(payload);
    return err;
}

static const cJSON *find_status_item(const cJSON *arr, const char *id)
{
    if (!cJSON_IsArray((cJSON *)arr)) {
        return NULL;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, (cJSON *)arr) {
        const cJSON *it_id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (cJSON_IsString(it_id) && it_id->valuestring && strcmp(it_id->valuestring, id) == 0) {
            return item;
        }
    }
    return NULL;
}

static const cJSON *find_sensor_status_item(const cJSON *arr, const mqtt_entity_t *entity)
{
    if (!cJSON_IsArray((cJSON *)arr) || !entity) {
        return NULL;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, (cJSON *)arr) {
        const cJSON *it_id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (cJSON_IsString(it_id) && it_id->valuestring &&
            strcmp(it_id->valuestring, entity->source_id) == 0) {
            return item;
        }

        const cJSON *devices = cJSON_GetObjectItemCaseSensitive(item, "devices");
        if (cJSON_IsArray((cJSON *)devices)) {
            cJSON *dev = NULL;
            cJSON_ArrayForEach(dev, (cJSON *)devices) {
                const cJSON *dev_id = cJSON_GetObjectItemCaseSensitive(dev, "id");
                if (cJSON_IsString(dev_id) && dev_id->valuestring &&
                    strcmp(dev_id->valuestring, entity->source_id) == 0) {
                    return dev;
                }
            }
        }
    }

    return NULL;
}

static esp_err_t publish_entity_state_from_status(const mqtt_entity_t *entity, const cJSON *status)
{
    if (!entity || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    char payload[256] = {0};
    if (entity->kind == ENTITY_KIND_OUTPUT && strcmp(entity->type, "relay") == 0) {
        bool power = jbool(status, "power", false);
        snprintf(payload, sizeof(payload), "%s", power ? "ON" : "OFF");
    } else if (entity->kind == ENTITY_KIND_OUTPUT &&
               (strcmp(entity->type, "servo_3wire") == 0 || strcmp(entity->type, "servo_5wire") == 0)) {
        int level = jint(status, "level", 0);
        snprintf(payload, sizeof(payload), "%d", level);
    } else if (entity->kind == ENTITY_KIND_OUTPUT && strcmp(entity->type, "pwm") == 0) {
        bool power = jbool(status, "power", false);
        int level = jint(status, "level", 0);
        int brightness = (level * 255) / 100;
        snprintf(payload, sizeof(payload), "{\"state\":\"%s\",\"brightness\":%d}",
                 power ? "ON" : "OFF", brightness);
    } else if (entity->kind == ENTITY_KIND_OUTPUT && strcmp(entity->type, "ws2812") == 0) {
        bool power = jbool(status, "power", false);
        int level = jint(status, "level", 0);
        int brightness = (level * 255) / 100;
        if (strcmp(entity->output_mode, "mono_triplet") == 0) {
            snprintf(payload, sizeof(payload), "{\"state\":\"%s\",\"brightness\":%d}",
                     power ? "ON" : "OFF", brightness);
        } else {
            const cJSON *color = jobj(status, "color");
            snprintf(payload, sizeof(payload),
                     "{\"state\":\"%s\",\"brightness\":%d,\"color\":{\"r\":%d,\"g\":%d,\"b\":%d}}",
                     power ? "ON" : "OFF", brightness,
                     jint(color, "r", 255), jint(color, "g", 255), jint(color, "b", 255));
        }
    } else if (entity->kind == ENTITY_KIND_INPUT) {
        bool state = jbool(status, "state", false);
        snprintf(payload, sizeof(payload), "%s", state ? "ON" : "OFF");
    } else if (entity->kind == ENTITY_KIND_SENSOR) {
        const cJSON *metric = cJSON_GetObjectItemCaseSensitive((cJSON *)status, entity->metric);
        if (!cJSON_IsNumber(metric)) {
            return ESP_ERR_NOT_FOUND;
        }
        snprintf(payload, sizeof(payload), "%.2f", metric->valuedouble);
    } else {
        return ESP_OK;
    }

    return publish_raw(entity->state_topic, payload, 1, s_cfg.retain);
}

static esp_err_t publish_all_states(void)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *status = modules_build_status_json();
    if (!status) {
        return ESP_ERR_NO_MEM;
    }

    const cJSON *outputs = jobj(status, "outputs");
    const cJSON *inputs = jobj(status, "inputs");
    const cJSON *sensors = jobj(status, "sensors");

    for (int i = 0; i < s_entity_count; ++i) {
        const mqtt_entity_t *entity = &s_entities[i];
        const cJSON *item = NULL;
        if (entity->kind == ENTITY_KIND_OUTPUT) {
            item = find_status_item(outputs, entity->id);
        } else if (entity->kind == ENTITY_KIND_INPUT) {
            item = find_status_item(inputs, entity->id);
        } else if (entity->kind == ENTITY_KIND_SENSOR) {
            item = find_sensor_status_item(sensors, entity);
        }
        if (item) {
            (void)publish_entity_state_from_status(entity, item);
        }
    }

    cJSON_Delete(status);
    return ESP_OK;
}

static esp_err_t apply_light_command(const mqtt_entity_t *entity, const char *data, int len)
{
    char buf[256] = {0};
    int copy_len = (len < (int)sizeof(buf) - 1) ? len : (int)sizeof(buf) - 1;
    memcpy(buf, data, (size_t)copy_len);

    cJSON *root = NULL;
    cJSON *action = cJSON_CreateObject();
    if (!action) {
        return ESP_ERR_NO_MEM;
    }

    if (buf[0] == '{') {
        root = cJSON_Parse(buf);
        if (!root) {
            cJSON_Delete(action);
            return ESP_ERR_INVALID_ARG;
        }

        const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(state) && state->valuestring) {
            if (str_ieq(state->valuestring, "ON")) {
                cJSON_AddBoolToObject(action, "set", true);
            } else if (str_ieq(state->valuestring, "OFF")) {
                cJSON_AddBoolToObject(action, "set", false);
            }
        }

        const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(root, "brightness");
        if (cJSON_IsNumber(brightness)) {
            int level = (brightness->valueint * 100) / 255;
            cJSON_DeleteItemFromObjectCaseSensitive(action, "set");
            cJSON_AddNumberToObject(action, "set_level", level);
        }

        const cJSON *color = cJSON_GetObjectItemCaseSensitive(root, "color");
        if (cJSON_IsObject(color)) {
            cJSON_AddItemReferenceToObject(action, "color", (cJSON *)color);
        }
    } else {
        if (str_ieq(buf, "ON")) {
            cJSON_AddBoolToObject(action, "set", true);
        } else if (str_ieq(buf, "OFF")) {
            cJSON_AddBoolToObject(action, "set", false);
        } else if (str_ieq(buf, "TOGGLE")) {
            cJSON_AddBoolToObject(action, "toggle", true);
        }
    }

    cJSON *resp = NULL;
    esp_err_t err = modules_action(entity->id, action, &resp);
    cJSON_Delete(resp);
    cJSON_Delete(action);
    cJSON_Delete(root);
    return err;
}

static esp_err_t apply_number_command(const mqtt_entity_t *entity, const char *data, int len)
{
    char raw[32] = {0};
    char *end = NULL;
    long value;
    cJSON *action;
    cJSON *resp = NULL;
    esp_err_t err;

    if (!entity || !data || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len >= (int)sizeof(raw)) {
        len = (int)sizeof(raw) - 1;
    }
    memcpy(raw, data, (size_t)len);
    raw[len] = 0;

    while (*raw == ' ' || *raw == '\t' || *raw == '\r' || *raw == '\n') {
        memmove(raw, raw + 1, strlen(raw));
    }
    for (int i = (int)strlen(raw) - 1; i >= 0; --i) {
        if (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\r' || raw[i] == '\n') {
            raw[i] = 0;
        } else {
            break;
        }
    }

    value = strtol(raw, &end, 10);
    if (end == raw || (end && *end != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    action = cJSON_CreateObject();
    if (!action) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(action, "set_level", (int)value);
    err = modules_action(entity->id, action, &resp);
    cJSON_Delete(resp);
    cJSON_Delete(action);
    return err;
}

static esp_err_t apply_relay_command(const mqtt_entity_t *entity, const char *data, int len)
{
    char raw[24] = {0};
    int copy_len = (len < (int)sizeof(raw) - 1) ? len : (int)sizeof(raw) - 1;
    memcpy(raw, data, (size_t)copy_len);

    cJSON *action = cJSON_CreateObject();
    if (!action) {
        return ESP_ERR_NO_MEM;
    }

    if (str_ieq(raw, "ON") || str_ieq(raw, "1") || str_ieq(raw, "TRUE")) {
        cJSON_AddBoolToObject(action, "set", true);
    } else if (str_ieq(raw, "OFF") || str_ieq(raw, "0") || str_ieq(raw, "FALSE")) {
        cJSON_AddBoolToObject(action, "set", false);
    } else if (str_ieq(raw, "TOGGLE")) {
        cJSON_AddBoolToObject(action, "toggle", true);
    } else {
        cJSON_Delete(action);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *resp = NULL;
    esp_err_t err = modules_action(entity->id, action, &resp);
    cJSON_Delete(resp);
    cJSON_Delete(action);
    return err;
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
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "connected to broker");
            for (int i = 0; i < s_entity_count; ++i) {
                if (s_entities[i].supports_command) {
                    (void)esp_mqtt_client_subscribe(s_client, s_entities[i].command_topic, 1);
                }
                (void)publish_discovery_entity(&s_entities[i]);
            }
            (void)publish_raw(s_availability_topic, "online", 1, true);
            (void)publish_all_states();
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "disconnected from broker");
            break;
        case MQTT_EVENT_DATA: {
            if (event->current_data_offset != 0) {
                break;
            }
            mqtt_entity_t *entity = find_entity_by_topic(event->topic, event->topic_len);
            if (!entity) {
                break;
            }
            esp_err_t err = ESP_OK;
            if (strcmp(entity->component, "switch") == 0) {
                err = apply_relay_command(entity, event->data, event->data_len);
            } else if (strcmp(entity->component, "number") == 0) {
                err = apply_number_command(entity, event->data, event->data_len);
            } else {
                err = apply_light_command(entity, event->data, event->data_len);
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "MQTT command for %s failed: %s", entity->id, esp_err_to_name(err));
            }
            break;
        }
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
        return ESP_OK;
    }

    if (s_connected) {
        (void)publish_raw(s_availability_topic, "offline", 1, true);
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
    return ESP_OK;
}

static void modules_runtime_changed_cb(void *ctx)
{
    (void)ctx;
    (void)mqtt_mgr_notify_runtime_changed();
}

esp_err_t mqtt_mgr_notify_runtime_changed(void)
{
    if (!s_cfg.enabled || !s_connected) {
        return ESP_OK;
    }
    return publish_all_states();
}

esp_err_t mqtt_mgr_start_from_cfg(const cJSON *cfg)
{
    mqtt_cfg_t next_cfg;
    esp_err_t err = parse_cfg(cfg, &next_cfg);
    if (err != ESP_OK) {
        return err;
    }

    s_cfg = next_cfg;
    cJSON *status = modules_build_status_json();
    if (!status) {
        return ESP_ERR_NO_MEM;
    }
    build_entities_from_status(status);
    cJSON_Delete(status);
    modules_set_runtime_callback(modules_runtime_changed_cb, NULL);

    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "MQTT disabled in config");
        return stop_client();
    }

    if (s_cfg.uri[0] == 0) {
        ESP_LOGW(TAG, "MQTT enabled but host is empty");
        return stop_client();
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_cfg.uri,
        .credentials.username = s_cfg.username[0] ? s_cfg.username : NULL,
        .credentials.authentication.password = s_cfg.password[0] ? s_cfg.password : NULL,
        .credentials.client_id = s_cfg.client_id,
        .session.keepalive = 60,
        .session.last_will.topic = s_availability_topic,
        .session.last_will.msg = "offline",
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

bool mqtt_mgr_is_connected(void)
{
    return s_connected;
}
