#include "cfg_json.h"

#include <stdarg.h>
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

#define CFG_SCHEMA_VERSION 2
#define CFG_MAX_OUTPUTS 8
#define CFG_MAX_INPUTS_AND_BUTTONS 8
#define CFG_MAX_SENSORS 4

static const int s_allowed_gpios[] = {0, 1, 3, 4, 5, 6, 7, 10};

static const char *BOARD_PROFILE = "esp32-c3-devkitm-1";
static const char *MQTT_DISCOVERY_PREFIX_DEFAULT = "homeassistant";
static const char *DEVICE_NAME_DEFAULT = "ESP32 C3 MQTT Device";
static const char *HOSTNAME_DEFAULT = "esp32-c3";
static const char *AP_SSID_DEFAULT = "ESP32-SETUP";

static cJSON *s_cfg = NULL;
static char s_last_error[192] = "";

typedef struct {
    bool used;
    char group[16];
    char owner[40];
} gpio_reservation_t;

typedef struct {
    gpio_reservation_t pins[32];
    char ids[32][24];
    int id_count;
    bool i2c_bus_seen;
    int i2c_sda;
    int i2c_scl;
    int i2c_freq;
    bool onewire_seen;
    int onewire_gpio;
} cfg_validation_t;

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
}

const char *cfg_json_last_error(void)
{
    return s_last_error[0] ? s_last_error : "unknown config error";
}

static void clear_error(void)
{
    s_last_error[0] = 0;
}

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

static const char *jstr_alias(const cJSON *obj, const char *key, const char *alias, const char *def)
{
    const char *value = jstr(obj, key, NULL);
    if (value && value[0] != 0) {
        return value;
    }
    value = jstr(obj, alias, NULL);
    if (value && value[0] != 0) {
        return value;
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

static int jint_alias(const cJSON *obj, const char *key, const char *alias, int def)
{
    const cJSON *it = jobj(obj, key);
    if (cJSON_IsNumber(it)) {
        return it->valueint;
    }
    it = jobj(obj, alias);
    if (cJSON_IsNumber(it)) {
        return it->valueint;
    }
    return def;
}

static const cJSON *get_connectivity_obj(const cJSON *src)
{
    const cJSON *conn = jobj(src, "connectivity");
    if (cJSON_IsObject((cJSON *)conn)) {
        return conn;
    }
    return jobj(src, "net");
}

static const cJSON *get_mqtt_obj(const cJSON *src)
{
    const cJSON *conn = jobj(src, "connectivity");
    const cJSON *mqtt = jobj(conn, "mqtt");
    if (cJSON_IsObject((cJSON *)mqtt)) {
        return mqtt;
    }
    mqtt = jobj(src, "mqtt");
    if (cJSON_IsObject((cJSON *)mqtt)) {
        return mqtt;
    }
    return NULL;
}

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

static void sanitize_id_copy(char *dst, size_t dst_len, const char *src, const char *prefix, int ordinal)
{
    size_t wr = 0;
    if (!dst || dst_len == 0) {
        return;
    }

    if (src) {
        for (size_t i = 0; src[i] != 0 && wr + 1 < dst_len; ++i) {
            char ch = src[i];
            bool ok = ((ch >= 'a' && ch <= 'z') ||
                       (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') ||
                       ch == '_' || ch == '-');
            if (ok) {
                dst[wr++] = ch;
            }
        }
    }

    if (wr == 0) {
        snprintf(dst, dst_len, "%s%d", prefix, ordinal);
    } else {
        dst[wr] = 0;
    }
}

static bool is_gpio_allowed(int gpio)
{
    for (size_t i = 0; i < sizeof(s_allowed_gpios) / sizeof(s_allowed_gpios[0]); ++i) {
        if (s_allowed_gpios[i] == gpio) {
            return true;
        }
    }
    return false;
}

static bool id_already_used(cfg_validation_t *ctx, const char *id)
{
    for (int i = 0; i < ctx->id_count; ++i) {
        if (strcmp(ctx->ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static bool register_id(cfg_validation_t *ctx, const char *id)
{
    if (!id || !id[0]) {
        set_error("Entity id is empty");
        return false;
    }
    if (id_already_used(ctx, id)) {
        set_error("Duplicate entity id: %s", id);
        return false;
    }
    if (ctx->id_count >= (int)(sizeof(ctx->ids) / sizeof(ctx->ids[0]))) {
        set_error("Too many entities in config");
        return false;
    }
    snprintf(ctx->ids[ctx->id_count], sizeof(ctx->ids[ctx->id_count]), "%s", id);
    ctx->id_count++;
    return true;
}

static bool reserve_gpio(cfg_validation_t *ctx, int gpio, const char *owner, const char *share_group)
{
    if (gpio < 0 || gpio >= (int)(sizeof(ctx->pins) / sizeof(ctx->pins[0]))) {
        set_error("%s uses invalid GPIO%d", owner, gpio);
        return false;
    }
    if (!is_gpio_allowed(gpio)) {
        set_error("%s uses forbidden GPIO%d", owner, gpio);
        return false;
    }

    gpio_reservation_t *slot = &ctx->pins[gpio];
    if (!slot->used) {
        slot->used = true;
        snprintf(slot->group, sizeof(slot->group), "%s", share_group ? share_group : "");
        snprintf(slot->owner, sizeof(slot->owner), "%s", owner);
        return true;
    }

    if (share_group && share_group[0] != 0 && strcmp(slot->group, share_group) == 0) {
        return true;
    }

    set_error("GPIO%d conflict between %s and %s", gpio, owner, slot->owner);
    return false;
}

static cJSON *create_empty_schema(void)
{
    cJSON *root = cJSON_CreateObject();
    char node_id[40] = {0};
    build_board_node_id(node_id, sizeof(node_id));

    cJSON_AddNumberToObject(root, "schema_version", CFG_SCHEMA_VERSION);

    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "name", DEVICE_NAME_DEFAULT);
    cJSON_AddStringToObject(device, "board_profile", BOARD_PROFILE);
    cJSON_AddStringToObject(device, "node_id", node_id);

    cJSON *connectivity = cJSON_AddObjectToObject(root, "connectivity");
    cJSON_AddStringToObject(connectivity, "hostname", HOSTNAME_DEFAULT);

    cJSON *ap = cJSON_AddObjectToObject(connectivity, "ap");
    cJSON_AddStringToObject(ap, "ssid", AP_SSID_DEFAULT);
    cJSON_AddStringToObject(ap, "pass", "");

    cJSON *sta = cJSON_AddObjectToObject(connectivity, "sta");
    cJSON_AddStringToObject(sta, "ssid", "");
    cJSON_AddStringToObject(sta, "pass", "");

    cJSON *mqtt = cJSON_AddObjectToObject(connectivity, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enable", false);
    cJSON_AddStringToObject(mqtt, "host", "");
    cJSON_AddNumberToObject(mqtt, "port", 1883);
    cJSON_AddStringToObject(mqtt, "user", "");
    cJSON_AddStringToObject(mqtt, "pass", "");
    cJSON_AddStringToObject(mqtt, "client_id", node_id);
    cJSON_AddStringToObject(mqtt, "topic_prefix", node_id);
    cJSON_AddStringToObject(mqtt, "discovery_prefix", MQTT_DISCOVERY_PREFIX_DEFAULT);
    cJSON_AddBoolToObject(mqtt, "discovery", true);
    cJSON_AddBoolToObject(mqtt, "retain", true);

    cJSON_AddArrayToObject(root, "outputs");
    cJSON_AddArrayToObject(root, "inputs");
    cJSON_AddArrayToObject(root, "buttons");
    cJSON_AddArrayToObject(root, "sensors");

    return root;
}

static bool append_action_json(cJSON *dst_parent, const char *key, const cJSON *src_action)
{
    cJSON *dst = cJSON_AddObjectToObject(dst_parent, key);
    if (!dst) {
        return false;
    }

    const char *type = jstr(src_action, "type", "none");
    if (type[0] == 0) {
        type = "none";
    }
    cJSON_AddStringToObject(dst, "type", type);

    const char *target = jstr(src_action, "target", "");
    if (target[0] != 0) {
        cJSON_AddStringToObject(dst, "target", target);
    }

    if (strcmp(type, "set_output") == 0) {
        cJSON_AddBoolToObject(dst, "value", jbool(src_action, "value", false));
    } else if (strcmp(type, "dim_step_up") == 0 || strcmp(type, "dim_step_down") == 0) {
        int step = jint(src_action, "step", 10);
        if (step < 1) {
            step = 1;
        }
        if (step > 100) {
            step = 100;
        }
        cJSON_AddNumberToObject(dst, "step", step);
    }

    return true;
}

static cJSON *normalize_config(const cJSON *src)
{
    cJSON *root = create_empty_schema();
    if (!root) {
        set_error("Out of memory while creating config");
        return NULL;
    }

    char node_id[40] = {0};
    build_board_node_id(node_id, sizeof(node_id));

    const bool src_is_v2 = jint(src, "schema_version", 0) == CFG_SCHEMA_VERSION;
    const cJSON *src_device = jobj(src, "device");
    const cJSON *src_conn = get_connectivity_obj(src);
    const cJSON *src_ap = jobj(src_conn, "ap");
    const cJSON *src_sta = jobj(src_conn, "sta");
    const cJSON *src_mqtt = get_mqtt_obj(src);

    cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    cJSON *connectivity = cJSON_GetObjectItemCaseSensitive(root, "connectivity");
    cJSON *ap = cJSON_GetObjectItemCaseSensitive(connectivity, "ap");
    cJSON *sta = cJSON_GetObjectItemCaseSensitive(connectivity, "sta");
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(connectivity, "mqtt");
    cJSON *outputs = cJSON_GetObjectItemCaseSensitive(root, "outputs");
    cJSON *inputs = cJSON_GetObjectItemCaseSensitive(root, "inputs");
    cJSON *buttons = cJSON_GetObjectItemCaseSensitive(root, "buttons");
    cJSON *sensors = cJSON_GetObjectItemCaseSensitive(root, "sensors");

    const char *device_name = jstr(src_device, "name",
                                   jstr(src_mqtt, "device_name", DEVICE_NAME_DEFAULT));
    const char *board_profile = jstr(src_device, "board_profile", BOARD_PROFILE);
    const char *saved_node_id = jstr(src_device, "node_id", node_id);
    if (!saved_node_id[0]) {
        saved_node_id = node_id;
    }

    cJSON_ReplaceItemInObject(device, "name", cJSON_CreateString(device_name));
    cJSON_ReplaceItemInObject(device, "board_profile", cJSON_CreateString(board_profile));
    cJSON_ReplaceItemInObject(device, "node_id", cJSON_CreateString(saved_node_id));

    cJSON_ReplaceItemInObject(connectivity, "hostname",
                              cJSON_CreateString(jstr(src_conn, "hostname", HOSTNAME_DEFAULT)));
    cJSON_ReplaceItemInObject(ap, "ssid", cJSON_CreateString(jstr(src_ap, "ssid", AP_SSID_DEFAULT)));
    cJSON_ReplaceItemInObject(ap, "pass", cJSON_CreateString(jstr(src_ap, "pass", "")));
    cJSON_ReplaceItemInObject(sta, "ssid", cJSON_CreateString(jstr(src_sta, "ssid", "")));
    cJSON_ReplaceItemInObject(sta, "pass", cJSON_CreateString(jstr(src_sta, "pass", "")));

    cJSON_ReplaceItemInObject(mqtt, "enable", cJSON_CreateBool(jbool(src_mqtt, "enable", false)));
    cJSON_ReplaceItemInObject(mqtt, "host",
                              cJSON_CreateString(jstr_alias(src_mqtt, "host", "server_ip", "")));
    cJSON_ReplaceItemInObject(mqtt, "port",
                              cJSON_CreateNumber(jint_alias(src_mqtt, "port", "server_port", 1883)));
    cJSON_ReplaceItemInObject(mqtt, "user",
                              cJSON_CreateString(jstr_alias(src_mqtt, "user", "login", "")));
    cJSON_ReplaceItemInObject(mqtt, "pass",
                              cJSON_CreateString(jstr_alias(src_mqtt, "pass", "password", "")));
    cJSON_ReplaceItemInObject(mqtt, "client_id",
                              cJSON_CreateString(jstr(src_mqtt, "client_id", saved_node_id)));
    cJSON_ReplaceItemInObject(mqtt, "topic_prefix",
                              cJSON_CreateString(jstr(src_mqtt, "topic_prefix", saved_node_id)));
    cJSON_ReplaceItemInObject(mqtt, "discovery_prefix",
                              cJSON_CreateString(jstr(src_mqtt, "discovery_prefix",
                                                      MQTT_DISCOVERY_PREFIX_DEFAULT)));
    cJSON_ReplaceItemInObject(mqtt, "discovery", cJSON_CreateBool(jbool(src_mqtt, "discovery", true)));
    cJSON_ReplaceItemInObject(mqtt, "retain", cJSON_CreateBool(jbool(src_mqtt, "retain", true)));

    cfg_validation_t ctx = {0};

    const cJSON *src_outputs = src_is_v2 ? jobj(src, "outputs") : NULL;
    if (cJSON_IsArray((cJSON *)src_outputs)) {
        int count = cJSON_GetArraySize((cJSON *)src_outputs);
        if (count > CFG_MAX_OUTPUTS) {
            cJSON_Delete(root);
            set_error("Too many outputs: max %d", CFG_MAX_OUTPUTS);
            return NULL;
        }

        for (int i = 0; i < count; ++i) {
            const cJSON *item = cJSON_GetArrayItem((cJSON *)src_outputs, i);
            if (!cJSON_IsObject((cJSON *)item)) {
                continue;
            }

            char id[24] = {0};
            sanitize_id_copy(id, sizeof(id), jstr(item, "id", ""), "out", i + 1);
            if (!register_id(&ctx, id)) {
                cJSON_Delete(root);
                return NULL;
            }

            const char *type = jstr(item, "type", "relay");
            if (strcmp(type, "relay") != 0 && strcmp(type, "pwm") != 0 && strcmp(type, "ws2812") != 0) {
                cJSON_Delete(root);
                set_error("Output %s uses unsupported type '%s'", id, type);
                return NULL;
            }

            int gpio = jint(item, "gpio", -1);
            char owner[40] = {0};
            snprintf(owner, sizeof(owner), "output:%s", id);
            if (!reserve_gpio(&ctx, gpio, owner, "")) {
                cJSON_Delete(root);
                return NULL;
            }

            cJSON *dst = cJSON_CreateObject();
            cJSON_AddStringToObject(dst, "id", id);
            cJSON_AddStringToObject(dst, "name", jstr(item, "name", id));
            cJSON_AddStringToObject(dst, "type", type);
            cJSON_AddBoolToObject(dst, "enabled", jbool(item, "enabled", true));
            cJSON_AddNumberToObject(dst, "gpio", gpio);

            if (strcmp(type, "relay") == 0) {
                cJSON_AddNumberToObject(dst, "active_level", jint(item, "active_level", 1) ? 1 : 0);
                cJSON_AddBoolToObject(dst, "default_on", jbool(item, "default_on", false));
            } else if (strcmp(type, "pwm") == 0) {
                int freq = jint(item, "freq_hz", 1000);
                int default_level = jint(item, "default_level", 0);
                if (freq < 100) {
                    freq = 100;
                }
                if (freq > 20000) {
                    freq = 20000;
                }
                if (default_level < 0) {
                    default_level = 0;
                }
                if (default_level > 100) {
                    default_level = 100;
                }
                cJSON_AddNumberToObject(dst, "freq_hz", freq);
                cJSON_AddBoolToObject(dst, "inverted", jbool(item, "inverted", false));
                cJSON_AddNumberToObject(dst, "default_level", default_level);
            } else {
                int pixel_count = jint(item, "pixel_count", 1);
                if (pixel_count < 1) {
                    pixel_count = 1;
                }
                if (pixel_count > 300) {
                    pixel_count = 300;
                }
                cJSON_AddNumberToObject(dst, "pixel_count", pixel_count);
                cJSON_AddStringToObject(dst, "color_order", jstr(item, "color_order", "GRB"));
                cJSON_AddBoolToObject(dst, "default_power_on", jbool(item, "default_power_on", false));
            }

            cJSON_AddItemToArray(outputs, dst);
        }
    } else {
        const cJSON *legacy_modules = jobj(src, "modules");
        const cJSON *legacy_relay = jobj(legacy_modules, "relay");
        if (cJSON_IsObject((cJSON *)legacy_relay) && jbool(legacy_relay, "enable", true)) {
            int gpio = jint(legacy_relay, "gpio", 4);
            if (!reserve_gpio(&ctx, gpio, "output:relay1", "")) {
                cJSON_Delete(root);
                return NULL;
            }
            if (!register_id(&ctx, "relay1")) {
                cJSON_Delete(root);
                return NULL;
            }
            cJSON *dst = cJSON_CreateObject();
            cJSON_AddStringToObject(dst, "id", "relay1");
            cJSON_AddStringToObject(dst, "name", "Relay 1");
            cJSON_AddStringToObject(dst, "type", "relay");
            cJSON_AddBoolToObject(dst, "enabled", true);
            cJSON_AddNumberToObject(dst, "gpio", gpio);
            cJSON_AddNumberToObject(dst, "active_level", jint(legacy_relay, "active_level", 1) ? 1 : 0);
            cJSON_AddBoolToObject(dst, "default_on", jbool(legacy_relay, "default_on", false));
            cJSON_AddItemToArray(outputs, dst);
        }
    }

    const cJSON *src_inputs = src_is_v2 ? jobj(src, "inputs") : NULL;
    int input_count = 0;
    if (cJSON_IsArray((cJSON *)src_inputs)) {
        input_count = cJSON_GetArraySize((cJSON *)src_inputs);
        if (input_count > CFG_MAX_INPUTS_AND_BUTTONS) {
            cJSON_Delete(root);
            set_error("Too many inputs: max %d", CFG_MAX_INPUTS_AND_BUTTONS);
            return NULL;
        }

        for (int i = 0; i < input_count; ++i) {
            const cJSON *item = cJSON_GetArrayItem((cJSON *)src_inputs, i);
            if (!cJSON_IsObject((cJSON *)item)) {
                continue;
            }

            char id[24] = {0};
            sanitize_id_copy(id, sizeof(id), jstr(item, "id", ""), "in", i + 1);
            if (!register_id(&ctx, id)) {
                cJSON_Delete(root);
                return NULL;
            }

            int gpio = jint(item, "gpio", -1);
            char owner[40] = {0};
            snprintf(owner, sizeof(owner), "input:%s", id);
            if (!reserve_gpio(&ctx, gpio, owner, "")) {
                cJSON_Delete(root);
                return NULL;
            }

            const char *role = jstr(item, "role", "generic_binary");
            if (strcmp(role, "motion") != 0 &&
                strcmp(role, "presence") != 0 &&
                strcmp(role, "contact") != 0 &&
                strcmp(role, "limit") != 0 &&
                strcmp(role, "generic_binary") != 0) {
                cJSON_Delete(root);
                set_error("Input %s uses unsupported role '%s'", id, role);
                return NULL;
            }

            const char *pull = jstr(item, "pull", "up");
            if (strcmp(pull, "up") != 0 && strcmp(pull, "down") != 0 && strcmp(pull, "none") != 0) {
                cJSON_Delete(root);
                set_error("Input %s uses unsupported pull '%s'", id, pull);
                return NULL;
            }

            cJSON *dst = cJSON_CreateObject();
            cJSON_AddStringToObject(dst, "id", id);
            cJSON_AddStringToObject(dst, "name", jstr(item, "name", id));
            cJSON_AddStringToObject(dst, "type", "digital");
            cJSON_AddBoolToObject(dst, "enabled", jbool(item, "enabled", true));
            cJSON_AddNumberToObject(dst, "gpio", gpio);
            cJSON_AddStringToObject(dst, "pull", pull);
            cJSON_AddBoolToObject(dst, "inverted", jbool(item, "inverted", false));
            cJSON_AddStringToObject(dst, "role", role);
            cJSON_AddItemToArray(inputs, dst);
        }
    }

    const cJSON *src_buttons = src_is_v2 ? jobj(src, "buttons") : NULL;
    int button_count = 0;
    if (cJSON_IsArray((cJSON *)src_buttons)) {
        button_count = cJSON_GetArraySize((cJSON *)src_buttons);
        if ((button_count + input_count) > CFG_MAX_INPUTS_AND_BUTTONS) {
            cJSON_Delete(root);
            set_error("Too many inputs/buttons combined: max %d", CFG_MAX_INPUTS_AND_BUTTONS);
            return NULL;
        }

        for (int i = 0; i < button_count; ++i) {
            const cJSON *item = cJSON_GetArrayItem((cJSON *)src_buttons, i);
            if (!cJSON_IsObject((cJSON *)item)) {
                continue;
            }

            char id[24] = {0};
            sanitize_id_copy(id, sizeof(id), jstr(item, "id", ""), "btn", i + 1);
            if (!register_id(&ctx, id)) {
                cJSON_Delete(root);
                return NULL;
            }

            int gpio = jint(item, "gpio", -1);
            char owner[40] = {0};
            snprintf(owner, sizeof(owner), "button:%s", id);
            if (!reserve_gpio(&ctx, gpio, owner, "")) {
                cJSON_Delete(root);
                return NULL;
            }

            const char *pull = jstr(item, "pull", "up");
            if (strcmp(pull, "up") != 0 && strcmp(pull, "down") != 0 && strcmp(pull, "none") != 0) {
                cJSON_Delete(root);
                set_error("Button %s uses unsupported pull '%s'", id, pull);
                return NULL;
            }

            cJSON *dst = cJSON_CreateObject();
            cJSON_AddStringToObject(dst, "id", id);
            cJSON_AddStringToObject(dst, "name", jstr(item, "name", id));
            cJSON_AddBoolToObject(dst, "enabled", jbool(item, "enabled", true));
            cJSON_AddNumberToObject(dst, "gpio", gpio);
            cJSON_AddStringToObject(dst, "pull", pull);
            cJSON_AddBoolToObject(dst, "inverted", jbool(item, "inverted", false));
            cJSON_AddNumberToObject(dst, "long_press_ms", jint(item, "long_press_ms", 1000));

            cJSON *actions = cJSON_AddObjectToObject(dst, "actions");
            const cJSON *src_actions = jobj(item, "actions");
            if (!append_action_json(actions, "short", jobj(src_actions, "short")) ||
                !append_action_json(actions, "long", jobj(src_actions, "long"))) {
                cJSON_Delete(root);
                set_error("Out of memory while building button actions");
                return NULL;
            }

            cJSON_AddItemToArray(buttons, dst);
        }
    }

    const cJSON *src_sensors = src_is_v2 ? jobj(src, "sensors") : NULL;
    if (cJSON_IsArray((cJSON *)src_sensors)) {
        int count = cJSON_GetArraySize((cJSON *)src_sensors);
        int ds18b20_bus_count = 0;
        if (count > CFG_MAX_SENSORS) {
            cJSON_Delete(root);
            set_error("Too many sensors: max %d", CFG_MAX_SENSORS);
            return NULL;
        }

        for (int i = 0; i < count; ++i) {
            const cJSON *item = cJSON_GetArrayItem((cJSON *)src_sensors, i);
            if (!cJSON_IsObject((cJSON *)item)) {
                continue;
            }

            char id[24] = {0};
            sanitize_id_copy(id, sizeof(id), jstr(item, "id", ""), "sensor", i + 1);
            if (!register_id(&ctx, id)) {
                cJSON_Delete(root);
                return NULL;
            }

            const char *type = jstr(item, "type", "");
            if (strcmp(type, "ds18b20_bus") != 0 &&
                strcmp(type, "aht20") != 0 &&
                strcmp(type, "sht3x") != 0 &&
                strcmp(type, "bme280") != 0) {
                cJSON_Delete(root);
                set_error("Sensor %s uses unsupported type '%s'", id, type);
                return NULL;
            }

            cJSON *dst = cJSON_CreateObject();
            cJSON_AddStringToObject(dst, "id", id);
            cJSON_AddStringToObject(dst, "name", jstr(item, "name", id));
            cJSON_AddStringToObject(dst, "type", type);
            cJSON_AddBoolToObject(dst, "enabled", jbool(item, "enabled", true));

            if (strcmp(type, "ds18b20_bus") == 0) {
                ds18b20_bus_count++;
                if (ds18b20_bus_count > 1) {
                    cJSON_Delete(root);
                    set_error("Only one ds18b20_bus sensor entry is allowed");
                    return NULL;
                }
                int gpio = jint(item, "gpio", -1);
                char owner[40] = {0};
                snprintf(owner, sizeof(owner), "sensor:%s", id);
                if (!reserve_gpio(&ctx, gpio, owner, "")) {
                    cJSON_Delete(root);
                    return NULL;
                }
                if (ctx.onewire_seen && ctx.onewire_gpio != gpio) {
                    cJSON_Delete(root);
                    set_error("DS18B20 sensors must share one OneWire bus");
                    return NULL;
                }
                ctx.onewire_seen = true;
                ctx.onewire_gpio = gpio;

                cJSON_AddNumberToObject(dst, "gpio", gpio);
                cJSON_AddNumberToObject(dst, "poll_interval_sec", jint(item, "poll_interval_sec", 30));
            } else {
                int sda = jint(item, "sda_gpio", -1);
                int scl = jint(item, "scl_gpio", -1);
                int address = jint(item, "address", strcmp(type, "aht20") == 0 ? 0x38 :
                                                 strcmp(type, "sht3x") == 0 ? 0x44 : 0x76);
                int freq = jint(item, "freq_hz", 100000);

                if (!reserve_gpio(&ctx, sda, "i2c_sda", "i2c_sda") ||
                    !reserve_gpio(&ctx, scl, "i2c_scl", "i2c_scl")) {
                    cJSON_Delete(root);
                    return NULL;
                }

                if (!ctx.i2c_bus_seen) {
                    ctx.i2c_bus_seen = true;
                    ctx.i2c_sda = sda;
                    ctx.i2c_scl = scl;
                    ctx.i2c_freq = freq;
                } else if (ctx.i2c_sda != sda || ctx.i2c_scl != scl || ctx.i2c_freq != freq) {
                    cJSON_Delete(root);
                    set_error("All I2C sensors must share the same SDA/SCL/freq bus settings");
                    return NULL;
                }

                cJSON_AddNumberToObject(dst, "sda_gpio", sda);
                cJSON_AddNumberToObject(dst, "scl_gpio", scl);
                cJSON_AddNumberToObject(dst, "address", address);
                cJSON_AddNumberToObject(dst, "freq_hz", freq);
                cJSON_AddNumberToObject(dst, "poll_interval_sec", jint(item, "poll_interval_sec", 30));
            }

            cJSON_AddItemToArray(sensors, dst);
        }
    }

    return root;
}

static esp_err_t nvs_write_string(const char *s)
{
    nvs_handle_t h = 0;
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

    nvs_handle_t h = 0;
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

    char *buf = calloc(1, len);
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

static esp_err_t save_cfg_object(cJSON *cfg)
{
    char *out = cJSON_PrintUnformatted(cfg);
    if (!out) {
        set_error("Out of memory while serializing config");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_write_string(out);
    free(out);
    if (err != ESP_OK) {
        set_error("NVS write failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_cfg) {
        cJSON_Delete(s_cfg);
    }
    s_cfg = cfg;
    clear_error();
    return ESP_OK;
}

const cJSON *cfg_json_get(void)
{
    return s_cfg;
}

esp_err_t cfg_json_load_or_default(void)
{
    clear_error();

    char *json = NULL;
    esp_err_t err = nvs_read_string(&json);
    if (err == ESP_OK && json) {
        cJSON *parsed = cJSON_Parse(json);
        free(json);

        if (parsed) {
            cJSON *normalized = normalize_config(parsed);
            cJSON_Delete(parsed);
            if (normalized) {
                ESP_LOGI(TAG, "Loaded config from NVS");
                return save_cfg_object(normalized);
            }
            ESP_LOGW(TAG, "Stored config invalid, using default: %s", cfg_json_last_error());
        } else {
            ESP_LOGW(TAG, "Stored config is invalid JSON, using default");
        }
    } else {
        ESP_LOGW(TAG, "No config in NVS, using default");
    }

    cJSON *def = create_empty_schema();
    if (!def) {
        set_error("Out of memory while creating default config");
        return ESP_ERR_NO_MEM;
    }
    return save_cfg_object(def);
}

esp_err_t cfg_json_set_and_save(const cJSON *new_cfg)
{
    if (!new_cfg) {
        set_error("Config payload is empty");
        return ESP_ERR_INVALID_ARG;
    }

    clear_error();
    cJSON *normalized = normalize_config(new_cfg);
    if (!normalized) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Saving normalized schema v%d config", CFG_SCHEMA_VERSION);
    return save_cfg_object(normalized);
}

esp_err_t cfg_json_reset_to_default(void)
{
    cJSON *def = create_empty_schema();
    if (!def) {
        set_error("Out of memory while creating default config");
        return ESP_ERR_NO_MEM;
    }
    return save_cfg_object(def);
}

esp_err_t cfg_json_force_relay_gpio12_profile(void)
{
    return ESP_OK;
}

static bool replace_string(cJSON *obj, const char *key, const char *value)
{
    return cJSON_ReplaceItemInObject(obj, key, cJSON_CreateString(value ? value : "")) != 0;
}

static bool replace_bool(cJSON *obj, const char *key, bool value)
{
    return cJSON_ReplaceItemInObject(obj, key, cJSON_CreateBool(value)) != 0;
}

static bool replace_number(cJSON *obj, const char *key, int value)
{
    return cJSON_ReplaceItemInObject(obj, key, cJSON_CreateNumber(value)) != 0;
}

esp_err_t cfg_json_clear_connectivity(void)
{
    if (!cJSON_IsObject(s_cfg)) {
        set_error("Config is not loaded");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *connectivity = cJSON_GetObjectItemCaseSensitive(s_cfg, "connectivity");
    cJSON *sta = cJSON_GetObjectItemCaseSensitive(connectivity, "sta");
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(connectivity, "mqtt");
    if (!cJSON_IsObject(connectivity) || !cJSON_IsObject(sta) || !cJSON_IsObject(mqtt)) {
        set_error("Connectivity section is missing");
        return ESP_ERR_INVALID_STATE;
    }

    if (!replace_string(sta, "ssid", "") ||
        !replace_string(sta, "pass", "") ||
        !replace_bool(mqtt, "enable", false) ||
        !replace_string(mqtt, "host", "") ||
        !replace_number(mqtt, "port", 1883) ||
        !replace_string(mqtt, "user", "") ||
        !replace_string(mqtt, "pass", "")) {
        set_error("Out of memory while clearing connectivity");
        return ESP_ERR_NO_MEM;
    }

    return cfg_json_set_and_save(s_cfg);
}

esp_err_t cfg_json_factory_reset(void)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        set_error("NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        set_error("Factory reset failed: %s", esp_err_to_name(err));
        return err;
    }

    clear_error();
    return ESP_OK;
}
