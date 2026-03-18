#include "core/modules.h"

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aht20.h"
#include "bme280.h"
#include "ds18b20.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "led_strip.h"
#include "onewire_bus.h"
#include "sht3x.h"

#include "app_watchdog.h"

static const char *TAG = "modules";

#define MODULES_MAX_OUTPUTS 8
#define MODULES_MAX_INPUTS 8
#define MODULES_MAX_BUTTONS 8
#define MODULES_MAX_SENSORS 4
#define MODULES_MAX_DS18B20 8
#define MODULES_POLL_PERIOD_MS 50
#define MODULES_SENSOR_TASK_PERIOD_MS 200
#define MODULES_DEFAULT_I2C_PORT I2C_NUM_0

typedef enum {
    OUTPUT_TYPE_NONE = 0,
    OUTPUT_TYPE_RELAY,
    OUTPUT_TYPE_PWM,
    OUTPUT_TYPE_WS2812,
} output_type_t;

typedef enum {
    ACTION_NONE = 0,
    ACTION_TOGGLE_OUTPUT,
    ACTION_SET_OUTPUT,
    ACTION_MASTER_ON,
    ACTION_MASTER_OFF,
    ACTION_MASTER_TOGGLE,
    ACTION_DIM_STEP_UP,
    ACTION_DIM_STEP_DOWN,
} button_action_type_t;

typedef struct {
    button_action_type_t type;
    char target[24];
    bool value;
    int step;
} button_action_t;

typedef struct {
    bool used;
    bool enabled;
    output_type_t type;
    char id[24];
    char name[40];
    int gpio;
    bool power;
    bool supported;
    union {
        struct {
            int active_level;
            bool default_on;
        } relay;
        struct {
            bool inverted;
            int freq_hz;
            int level;
            ledc_channel_t channel;
            ledc_timer_t timer;
        } pwm;
        struct {
            int pixel_count;
            char color_order[8];
            bool default_power_on;
            int level;
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            led_strip_handle_t strip;
        } ws2812;
    } cfg;
} output_runtime_t;

typedef struct {
    bool used;
    bool enabled;
    char id[24];
    char name[40];
    char role[24];
    int gpio;
    bool inverted;
    gpio_pull_mode_t pull_mode;
    bool state;
} input_runtime_t;

typedef struct {
    bool used;
    bool enabled;
    char id[24];
    char name[40];
    int gpio;
    bool inverted;
    gpio_pull_mode_t pull_mode;
    int long_press_ms;
    bool last_pressed;
    bool long_sent;
    int64_t pressed_since_us;
    button_action_t short_action;
    button_action_t long_action;
} button_runtime_t;

typedef struct {
    bool used;
    bool enabled;
    char id[24];
    char name[40];
    char type[24];
    bool supported;
    bool data_valid;
    int gpio;
    int sda_gpio;
    int scl_gpio;
    int address;
    int poll_interval_sec;
    int64_t next_poll_us;
    float temperature_c;
    float humidity_pct;
    float pressure_hpa;
    union {
        aht20_dev_handle_t aht20;
        sht3x_handle_t sht3x;
        bme280_handle_t bme280;
    } dev;
} sensor_runtime_t;

typedef struct {
    bool active;
    int sda_gpio;
    int scl_gpio;
    int freq_hz;
    i2c_bus_handle_t bus;
} i2c_runtime_t;

typedef struct {
    bool active;
    int gpio;
    onewire_bus_handle_t bus;
    ds18b20_device_handle_t devices[MODULES_MAX_DS18B20];
    onewire_device_address_t addresses[MODULES_MAX_DS18B20];
    float temperatures[MODULES_MAX_DS18B20];
    bool valid[MODULES_MAX_DS18B20];
    int device_count;
    int poll_interval_sec;
    int64_t next_poll_us;
    char sensor_id[24];
    char sensor_name[40];
} ds18b20_bus_runtime_t;

typedef struct {
    output_runtime_t outputs[MODULES_MAX_OUTPUTS];
    int output_count;
    input_runtime_t inputs[MODULES_MAX_INPUTS];
    int input_count;
    button_runtime_t buttons[MODULES_MAX_BUTTONS];
    int button_count;
    sensor_runtime_t sensors[MODULES_MAX_SENSORS];
    int sensor_count;
    i2c_runtime_t i2c;
    ds18b20_bus_runtime_t ds18b20;
} modules_runtime_t;

static modules_runtime_t s_runtime = {0};
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_poll_task = NULL;
static TaskHandle_t s_sensor_task = NULL;
static modules_runtime_callback_t s_runtime_cb = NULL;
static void *s_runtime_cb_ctx = NULL;

static const cJSON *jobj(const cJSON *obj, const char *key);
static const char *jstr(const cJSON *obj, const char *key, const char *def);
static bool jbool(const cJSON *obj, const char *key, bool def);
static int jint(const cJSON *obj, const char *key, int def);
static gpio_pull_mode_t pull_mode_from_text(const char *pull);
static output_type_t output_type_from_text(const char *type);
static const char *output_type_to_text(output_type_t type);
static button_action_type_t button_action_type_from_text(const char *type);
static const char *button_action_type_to_text(button_action_type_t type);
static void notify_runtime_changed(void);
static esp_err_t output_apply_physical_state(output_runtime_t *out);
static esp_err_t set_output_power_locked(output_runtime_t *out, bool on);
static esp_err_t set_output_level_locked(output_runtime_t *out, int level);
static esp_err_t set_master_output_locked(bool on);
static bool is_any_output_on_locked(void);
static output_runtime_t *find_output_locked(const char *id);
static input_runtime_t *find_input_locked(const char *id);
static cJSON *build_output_status_json(const output_runtime_t *out);
static cJSON *build_input_status_json(const input_runtime_t *in);
static cJSON *build_button_status_json(const button_runtime_t *btn);
static cJSON *build_sensor_status_json(const sensor_runtime_t *sensor);
static esp_err_t ensure_i2c_bus_locked(int sda_gpio, int scl_gpio, int freq_hz);
static esp_err_t ensure_ds18b20_bus_locked(const sensor_runtime_t *sensor);
static esp_err_t read_sensor_locked(sensor_runtime_t *sensor);
static esp_err_t read_ds18b20_locked(void);

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

static gpio_pull_mode_t pull_mode_from_text(const char *pull)
{
    if (strcmp(pull, "down") == 0) {
        return GPIO_PULLDOWN_ONLY;
    }
    if (strcmp(pull, "none") == 0) {
        return GPIO_FLOATING;
    }
    return GPIO_PULLUP_ONLY;
}

static output_type_t output_type_from_text(const char *type)
{
    if (strcmp(type, "relay") == 0) {
        return OUTPUT_TYPE_RELAY;
    }
    if (strcmp(type, "pwm") == 0) {
        return OUTPUT_TYPE_PWM;
    }
    if (strcmp(type, "ws2812") == 0) {
        return OUTPUT_TYPE_WS2812;
    }
    return OUTPUT_TYPE_NONE;
}

static const char *output_type_to_text(output_type_t type)
{
    switch (type) {
        case OUTPUT_TYPE_RELAY: return "relay";
        case OUTPUT_TYPE_PWM: return "pwm";
        case OUTPUT_TYPE_WS2812: return "ws2812";
        default: return "unknown";
    }
}

static button_action_type_t button_action_type_from_text(const char *type)
{
    if (strcmp(type, "toggle_output") == 0) {
        return ACTION_TOGGLE_OUTPUT;
    }
    if (strcmp(type, "set_output") == 0) {
        return ACTION_SET_OUTPUT;
    }
    if (strcmp(type, "master_on") == 0) {
        return ACTION_MASTER_ON;
    }
    if (strcmp(type, "master_off") == 0) {
        return ACTION_MASTER_OFF;
    }
    if (strcmp(type, "master_toggle") == 0) {
        return ACTION_MASTER_TOGGLE;
    }
    if (strcmp(type, "dim_step_up") == 0) {
        return ACTION_DIM_STEP_UP;
    }
    if (strcmp(type, "dim_step_down") == 0) {
        return ACTION_DIM_STEP_DOWN;
    }
    return ACTION_NONE;
}

static const char *button_action_type_to_text(button_action_type_t type)
{
    switch (type) {
        case ACTION_TOGGLE_OUTPUT: return "toggle_output";
        case ACTION_SET_OUTPUT: return "set_output";
        case ACTION_MASTER_ON: return "master_on";
        case ACTION_MASTER_OFF: return "master_off";
        case ACTION_MASTER_TOGGLE: return "master_toggle";
        case ACTION_DIM_STEP_UP: return "dim_step_up";
        case ACTION_DIM_STEP_DOWN: return "dim_step_down";
        default: return "none";
    }
}

static void notify_runtime_changed(void)
{
    if (s_runtime_cb) {
        s_runtime_cb(s_runtime_cb_ctx);
    }
}

static output_runtime_t *find_output_locked(const char *id)
{
    if (!id || !id[0]) {
        return NULL;
    }
    for (int i = 0; i < s_runtime.output_count; ++i) {
        if (s_runtime.outputs[i].used && strcmp(s_runtime.outputs[i].id, id) == 0) {
            return &s_runtime.outputs[i];
        }
    }
    return NULL;
}

static input_runtime_t *find_input_locked(const char *id)
{
    if (!id || !id[0]) {
        return NULL;
    }
    for (int i = 0; i < s_runtime.input_count; ++i) {
        if (s_runtime.inputs[i].used && strcmp(s_runtime.inputs[i].id, id) == 0) {
            return &s_runtime.inputs[i];
        }
    }
    return NULL;
}

static esp_err_t output_apply_physical_state(output_runtime_t *out)
{
    if (!out || !out->enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (out->type) {
        case OUTPUT_TYPE_RELAY: {
            int level = out->power ? out->cfg.relay.active_level : (1 - out->cfg.relay.active_level);
            return gpio_set_level(out->gpio, level);
        }
        case OUTPUT_TYPE_PWM: {
            int level = out->cfg.pwm.level;
            uint32_t duty = 0;
            const uint32_t max_duty = (1U << LEDC_TIMER_13_BIT) - 1U;
            int effective = out->power ? level : 0;
            if (effective < 0) {
                effective = 0;
            }
            if (effective > 100) {
                effective = 100;
            }
            if (out->cfg.pwm.inverted) {
                effective = 100 - effective;
            }
            duty = (uint32_t)((effective * (int)max_duty) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, out->cfg.pwm.channel, duty));
            return ledc_update_duty(LEDC_LOW_SPEED_MODE, out->cfg.pwm.channel);
        }
        case OUTPUT_TYPE_WS2812:
            if (!out->cfg.ws2812.strip) {
                return ESP_ERR_INVALID_STATE;
            }
            if (!out->power || out->cfg.ws2812.level <= 0) {
                ESP_ERROR_CHECK(led_strip_clear(out->cfg.ws2812.strip));
                return ESP_OK;
            }
            for (int i = 0; i < out->cfg.ws2812.pixel_count; ++i) {
                uint32_t red = ((uint32_t)out->cfg.ws2812.red * (uint32_t)out->cfg.ws2812.level) / 100U;
                uint32_t green = ((uint32_t)out->cfg.ws2812.green * (uint32_t)out->cfg.ws2812.level) / 100U;
                uint32_t blue = ((uint32_t)out->cfg.ws2812.blue * (uint32_t)out->cfg.ws2812.level) / 100U;
                ESP_ERROR_CHECK(led_strip_set_pixel(out->cfg.ws2812.strip, (uint32_t)i, red, green, blue));
            }
            return led_strip_refresh(out->cfg.ws2812.strip);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t set_output_power_locked(output_runtime_t *out, bool on)
{
    if (!out || !out->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    out->power = on;
    return output_apply_physical_state(out);
}

static esp_err_t set_output_level_locked(output_runtime_t *out, int level)
{
    if (!out || !out->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (level < 0) {
        level = 0;
    }
    if (level > 100) {
        level = 100;
    }

    if (out->type == OUTPUT_TYPE_PWM) {
        out->cfg.pwm.level = level;
        out->power = (level > 0);
    } else if (out->type == OUTPUT_TYPE_WS2812) {
        out->cfg.ws2812.level = level;
        out->power = (level > 0);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return output_apply_physical_state(out);
}

static void clear_runtime_locked(void)
{
    for (int i = 0; i < s_runtime.output_count; ++i) {
        output_runtime_t *out = &s_runtime.outputs[i];
        if (!out->used) {
            continue;
        }
        if (out->type == OUTPUT_TYPE_WS2812 && out->cfg.ws2812.strip) {
            (void)led_strip_clear(out->cfg.ws2812.strip);
            (void)led_strip_del(out->cfg.ws2812.strip);
            out->cfg.ws2812.strip = NULL;
        }
        if (out->type == OUTPUT_TYPE_PWM) {
            (void)ledc_stop(LEDC_LOW_SPEED_MODE, out->cfg.pwm.channel, 0);
        }
        if (out->gpio >= 0) {
            gpio_reset_pin((gpio_num_t)out->gpio);
        }
    }

    for (int i = 0; i < s_runtime.input_count; ++i) {
        if (s_runtime.inputs[i].used && s_runtime.inputs[i].gpio >= 0) {
            gpio_reset_pin((gpio_num_t)s_runtime.inputs[i].gpio);
        }
    }
    for (int i = 0; i < s_runtime.button_count; ++i) {
        if (s_runtime.buttons[i].used && s_runtime.buttons[i].gpio >= 0) {
            gpio_reset_pin((gpio_num_t)s_runtime.buttons[i].gpio);
        }
    }

    for (int i = 0; i < s_runtime.sensor_count; ++i) {
        sensor_runtime_t *sensor = &s_runtime.sensors[i];
        if (!sensor->used) {
            continue;
        }
        if (strcmp(sensor->type, "aht20") == 0 && sensor->dev.aht20) {
            (void)aht20_del_sensor(sensor->dev.aht20);
        } else if (strcmp(sensor->type, "sht3x") == 0 && sensor->dev.sht3x) {
            (void)sht3x_delete(&sensor->dev.sht3x);
        } else if (strcmp(sensor->type, "bme280") == 0 && sensor->dev.bme280) {
            (void)bme280_delete(&sensor->dev.bme280);
        }
    }

    for (int i = 0; i < s_runtime.ds18b20.device_count; ++i) {
        if (s_runtime.ds18b20.devices[i]) {
            (void)ds18b20_del_device(s_runtime.ds18b20.devices[i]);
            s_runtime.ds18b20.devices[i] = NULL;
        }
    }
    if (s_runtime.ds18b20.bus) {
        (void)onewire_bus_del(s_runtime.ds18b20.bus);
        s_runtime.ds18b20.bus = NULL;
    }
    if (s_runtime.i2c.bus) {
        (void)i2c_bus_delete(&s_runtime.i2c.bus);
    }

    memset(&s_runtime, 0, sizeof(s_runtime));
}

static esp_err_t configure_output(output_runtime_t *out, const cJSON *item, int index)
{
    memset(out, 0, sizeof(*out));
    out->used = true;
    out->enabled = jbool(item, "enabled", true);
    out->type = output_type_from_text(jstr(item, "type", "relay"));
    out->gpio = jint(item, "gpio", -1);
    out->supported = true;
    snprintf(out->id, sizeof(out->id), "%s", jstr(item, "id", ""));
    snprintf(out->name, sizeof(out->name), "%s", jstr(item, "name", out->id));

    if (!out->enabled) {
        return ESP_OK;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << out->gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    if (out->type == OUTPUT_TYPE_RELAY) {
        out->cfg.relay.active_level = jint(item, "active_level", 1) ? 1 : 0;
        out->cfg.relay.default_on = jbool(item, "default_on", false);
        out->power = out->cfg.relay.default_on;
        return output_apply_physical_state(out);
    }

    if (out->type == OUTPUT_TYPE_PWM) {
        out->cfg.pwm.freq_hz = jint(item, "freq_hz", 1000);
        out->cfg.pwm.inverted = jbool(item, "inverted", false);
        out->cfg.pwm.level = jint(item, "default_level", 0);
        out->cfg.pwm.channel = (ledc_channel_t)index;
        out->cfg.pwm.timer = (ledc_timer_t)(index % 4);
        out->power = out->cfg.pwm.level > 0;

        ledc_timer_config_t timer_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_num = out->cfg.pwm.timer,
            .duty_resolution = LEDC_TIMER_13_BIT,
            .freq_hz = out->cfg.pwm.freq_hz,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

        ledc_channel_config_t chan_cfg = {
            .gpio_num = out->gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = out->cfg.pwm.channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = out->cfg.pwm.timer,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&chan_cfg));
        return output_apply_physical_state(out);
    }

    if (out->type == OUTPUT_TYPE_WS2812) {
        out->cfg.ws2812.pixel_count = jint(item, "pixel_count", 1);
        snprintf(out->cfg.ws2812.color_order, sizeof(out->cfg.ws2812.color_order), "%s",
                 jstr(item, "color_order", "GRB"));
        out->cfg.ws2812.default_power_on = jbool(item, "default_power_on", false);
        out->cfg.ws2812.level = out->cfg.ws2812.default_power_on ? 100 : 0;
        out->cfg.ws2812.red = 255;
        out->cfg.ws2812.green = 255;
        out->cfg.ws2812.blue = 255;
        out->power = out->cfg.ws2812.default_power_on;
        led_strip_config_t strip_cfg = {
            .strip_gpio_num = out->gpio,
            .max_leds = (uint32_t)out->cfg.ws2812.pixel_count,
            .led_pixel_format = LED_PIXEL_FORMAT_GRB,
            .led_model = LED_MODEL_WS2812,
            .flags = {
                .invert_out = false,
            },
        };
        if (strcmp(out->cfg.ws2812.color_order, "GRB") != 0) {
            ESP_LOGW(TAG, "WS2812 %s requested unsupported color order '%s', using GRB",
                     out->id, out->cfg.ws2812.color_order);
            snprintf(out->cfg.ws2812.color_order, sizeof(out->cfg.ws2812.color_order), "%s", "GRB");
        }
        led_strip_rmt_config_t rmt_cfg = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
            .mem_block_symbols = 64,
            .flags = {
                .with_dma = false,
            },
        };
        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &out->cfg.ws2812.strip));
        out->supported = true;
        return output_apply_physical_state(out);
    }

    return ESP_ERR_INVALID_ARG;
}

static esp_err_t configure_input(input_runtime_t *in, const cJSON *item)
{
    memset(in, 0, sizeof(*in));
    in->used = true;
    in->enabled = jbool(item, "enabled", true);
    in->gpio = jint(item, "gpio", -1);
    in->inverted = jbool(item, "inverted", false);
    in->pull_mode = pull_mode_from_text(jstr(item, "pull", "up"));
    snprintf(in->id, sizeof(in->id), "%s", jstr(item, "id", ""));
    snprintf(in->name, sizeof(in->name), "%s", jstr(item, "name", in->id));
    snprintf(in->role, sizeof(in->role), "%s", jstr(item, "role", "generic_binary"));

    if (!in->enabled) {
        return ESP_OK;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << in->gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (in->pull_mode == GPIO_PULLUP_ONLY) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (in->pull_mode == GPIO_PULLDOWN_ONLY) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    in->state = gpio_get_level((gpio_num_t)in->gpio) != 0;
    if (in->inverted) {
        in->state = !in->state;
    }
    return ESP_OK;
}

static void parse_button_action(button_action_t *dst, const cJSON *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->type = button_action_type_from_text(jstr(src, "type", "none"));
    snprintf(dst->target, sizeof(dst->target), "%s", jstr(src, "target", ""));
    dst->value = jbool(src, "value", false);
    dst->step = jint(src, "step", 10);
    if (dst->step < 1) {
        dst->step = 1;
    }
    if (dst->step > 100) {
        dst->step = 100;
    }
}

static esp_err_t configure_button(button_runtime_t *btn, const cJSON *item)
{
    memset(btn, 0, sizeof(*btn));
    btn->used = true;
    btn->enabled = jbool(item, "enabled", true);
    btn->gpio = jint(item, "gpio", -1);
    btn->inverted = jbool(item, "inverted", false);
    btn->pull_mode = pull_mode_from_text(jstr(item, "pull", "up"));
    btn->long_press_ms = jint(item, "long_press_ms", 1000);
    snprintf(btn->id, sizeof(btn->id), "%s", jstr(item, "id", ""));
    snprintf(btn->name, sizeof(btn->name), "%s", jstr(item, "name", btn->id));

    const cJSON *actions = jobj(item, "actions");
    parse_button_action(&btn->short_action, jobj(actions, "short"));
    parse_button_action(&btn->long_action, jobj(actions, "long"));

    if (!btn->enabled) {
        return ESP_OK;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << btn->gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (btn->pull_mode == GPIO_PULLUP_ONLY) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (btn->pull_mode == GPIO_PULLDOWN_ONLY) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    return ESP_OK;
}

static esp_err_t ensure_i2c_bus_locked(int sda_gpio, int scl_gpio, int freq_hz)
{
    if (s_runtime.i2c.bus) {
        if (s_runtime.i2c.sda_gpio == sda_gpio &&
            s_runtime.i2c.scl_gpio == scl_gpio &&
            s_runtime.i2c.freq_hz == freq_hz) {
            return ESP_OK;
        }
        return ESP_ERR_INVALID_STATE;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = (uint32_t)freq_hz,
    };
    s_runtime.i2c.bus = i2c_bus_create(MODULES_DEFAULT_I2C_PORT, &conf);
    if (!s_runtime.i2c.bus) {
        return ESP_FAIL;
    }

    s_runtime.i2c.active = true;
    s_runtime.i2c.sda_gpio = sda_gpio;
    s_runtime.i2c.scl_gpio = scl_gpio;
    s_runtime.i2c.freq_hz = freq_hz;
    return ESP_OK;
}

static esp_err_t ensure_ds18b20_bus_locked(const sensor_runtime_t *sensor)
{
    if (s_runtime.ds18b20.bus) {
        return ESP_OK;
    }

    onewire_bus_config_t bus_config = {
        .bus_gpio_num = sensor->gpio,
        .flags = {
            .en_pull_up = true,
        },
    };
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10,
    };
    ESP_RETURN_ON_ERROR(onewire_new_bus_rmt(&bus_config, &rmt_config, &s_runtime.ds18b20.bus),
                        TAG, "create 1-wire bus failed");

    s_runtime.ds18b20.active = true;
    s_runtime.ds18b20.gpio = sensor->gpio;
    s_runtime.ds18b20.poll_interval_sec = sensor->poll_interval_sec;
    s_runtime.ds18b20.next_poll_us = 0;
    snprintf(s_runtime.ds18b20.sensor_id, sizeof(s_runtime.ds18b20.sensor_id), "%s", sensor->id);
    snprintf(s_runtime.ds18b20.sensor_name, sizeof(s_runtime.ds18b20.sensor_name), "%s", sensor->name);

    onewire_device_iter_handle_t iter = NULL;
    ESP_RETURN_ON_ERROR(onewire_new_device_iter(s_runtime.ds18b20.bus, &iter), TAG, "create 1-wire iterator failed");

    onewire_device_t dev = {0};
    int count = 0;
    while (count < MODULES_MAX_DS18B20 && onewire_device_iter_get_next(iter, &dev) == ESP_OK) {
        ds18b20_config_t ds_cfg = {};
        if (ds18b20_new_device_from_enumeration(&dev, &ds_cfg, &s_runtime.ds18b20.devices[count]) == ESP_OK) {
            (void)ds18b20_set_resolution(s_runtime.ds18b20.devices[count], DS18B20_RESOLUTION_12B);
            s_runtime.ds18b20.addresses[count] = dev.address;
            s_runtime.ds18b20.valid[count] = false;
            count++;
        }
    }
    (void)onewire_del_device_iter(iter);
    s_runtime.ds18b20.device_count = count;
    ESP_LOGI(TAG, "Discovered %d DS18B20 device(s) on GPIO%d", count, sensor->gpio);
    return ESP_OK;
}

static esp_err_t configure_sensor(sensor_runtime_t *sensor, const cJSON *item)
{
    memset(sensor, 0, sizeof(*sensor));
    sensor->used = true;
    sensor->enabled = jbool(item, "enabled", true);
    sensor->supported = false;
    snprintf(sensor->id, sizeof(sensor->id), "%s", jstr(item, "id", ""));
    snprintf(sensor->name, sizeof(sensor->name), "%s", jstr(item, "name", sensor->id));
    snprintf(sensor->type, sizeof(sensor->type), "%s", jstr(item, "type", ""));
    sensor->gpio = jint(item, "gpio", -1);
    sensor->sda_gpio = jint(item, "sda_gpio", -1);
    sensor->scl_gpio = jint(item, "scl_gpio", -1);
    sensor->address = jint(item, "address", 0);
    sensor->poll_interval_sec = jint(item, "poll_interval_sec", 30);
    sensor->next_poll_us = 0;
    if (sensor->poll_interval_sec < 2) {
        sensor->poll_interval_sec = 2;
    }

    if (!sensor->enabled) {
        return ESP_OK;
    }

    if (strcmp(sensor->type, "ds18b20_bus") == 0) {
        sensor->supported = true;
        return ensure_ds18b20_bus_locked(sensor);
    }

    ESP_RETURN_ON_ERROR(ensure_i2c_bus_locked(sensor->sda_gpio, sensor->scl_gpio,
                                              jint(item, "freq_hz", 100000)),
                        TAG, "i2c bus init failed");

    if (strcmp(sensor->type, "aht20") == 0) {
        aht20_i2c_config_t conf = {
            .bus_inst = s_runtime.i2c.bus,
            .i2c_addr = (uint8_t)sensor->address,
        };
        ESP_RETURN_ON_ERROR(aht20_new_sensor(&conf, &sensor->dev.aht20), TAG, "aht20 init failed");
        sensor->supported = true;
        return ESP_OK;
    }
    if (strcmp(sensor->type, "sht3x") == 0) {
        sensor->dev.sht3x = sht3x_create(s_runtime.i2c.bus, (uint8_t)sensor->address);
        if (!sensor->dev.sht3x) {
            return ESP_FAIL;
        }
        ESP_RETURN_ON_ERROR(sht3x_soft_reset(sensor->dev.sht3x), TAG, "sht3x reset failed");
        sensor->supported = true;
        return ESP_OK;
    }
    if (strcmp(sensor->type, "bme280") == 0) {
        sensor->dev.bme280 = bme280_create(s_runtime.i2c.bus, (uint8_t)sensor->address);
        if (!sensor->dev.bme280) {
            return ESP_FAIL;
        }
        ESP_RETURN_ON_ERROR(bme280_default_init(sensor->dev.bme280), TAG, "bme280 init failed");
        sensor->supported = true;
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static bool button_is_pressed(const button_runtime_t *btn)
{
    bool pressed = gpio_get_level((gpio_num_t)btn->gpio) != 0;
    if (btn->inverted) {
        pressed = !pressed;
    }
    return pressed;
}

static esp_err_t execute_button_action_locked(const button_action_t *action)
{
    if (!action) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (action->type) {
        case ACTION_MASTER_ON:
            return set_master_output_locked(true);
        case ACTION_MASTER_OFF:
            return set_master_output_locked(false);
        case ACTION_MASTER_TOGGLE:
            return set_master_output_locked(!is_any_output_on_locked());
        case ACTION_TOGGLE_OUTPUT: {
            output_runtime_t *out = find_output_locked(action->target);
            if (!out) {
                return ESP_ERR_NOT_FOUND;
            }
            return set_output_power_locked(out, !out->power);
        }
        case ACTION_SET_OUTPUT: {
            output_runtime_t *out = find_output_locked(action->target);
            if (!out) {
                return ESP_ERR_NOT_FOUND;
            }
            return set_output_power_locked(out, action->value);
        }
        case ACTION_DIM_STEP_UP:
        case ACTION_DIM_STEP_DOWN: {
            output_runtime_t *out = find_output_locked(action->target);
            if (!out) {
                return ESP_ERR_NOT_FOUND;
            }
            if (out->type != OUTPUT_TYPE_PWM && out->type != OUTPUT_TYPE_WS2812) {
                return ESP_ERR_INVALID_ARG;
            }
            int level = (out->type == OUTPUT_TYPE_PWM) ? out->cfg.pwm.level : out->cfg.ws2812.level;
            level += (action->type == ACTION_DIM_STEP_UP) ? action->step : -action->step;
            return set_output_level_locked(out, level);
        }
        case ACTION_NONE:
            return ESP_OK;
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t read_sensor_locked(sensor_runtime_t *sensor)
{
    if (!sensor || !sensor->enabled || !sensor->supported) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    if (strcmp(sensor->type, "aht20") == 0) {
        uint32_t t_raw = 0;
        uint32_t h_raw = 0;
        err = aht20_read_temperature_humidity(sensor->dev.aht20, &t_raw, &sensor->temperature_c, &h_raw, &sensor->humidity_pct);
        sensor->data_valid = (err == ESP_OK);
        return err;
    }
    if (strcmp(sensor->type, "sht3x") == 0) {
        err = sht3x_get_single_shot(sensor->dev.sht3x, &sensor->temperature_c, &sensor->humidity_pct);
        sensor->data_valid = (err == ESP_OK);
        return err;
    }
    if (strcmp(sensor->type, "bme280") == 0) {
        err = bme280_read_temperature(sensor->dev.bme280, &sensor->temperature_c);
        if (err == ESP_OK) {
            err = bme280_read_humidity(sensor->dev.bme280, &sensor->humidity_pct);
        }
        if (err == ESP_OK) {
            err = bme280_read_pressure(sensor->dev.bme280, &sensor->pressure_hpa);
            if (err == ESP_OK) {
                sensor->pressure_hpa /= 100.0f;
            }
        }
        sensor->data_valid = (err == ESP_OK);
        return err;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t read_ds18b20_locked(void)
{
    if (!s_runtime.ds18b20.active || !s_runtime.ds18b20.bus) {
        return ESP_ERR_INVALID_STATE;
    }

    app_watchdog_reset_current_task("modules_sensor");
    ESP_RETURN_ON_ERROR(ds18b20_trigger_temperature_conversion_for_all(s_runtime.ds18b20.bus),
                        TAG, "ds18 conversion failed");

    for (int i = 0; i < s_runtime.ds18b20.device_count; ++i) {
        app_watchdog_reset_current_task("modules_sensor");
        esp_err_t err = ds18b20_get_temperature(s_runtime.ds18b20.devices[i], &s_runtime.ds18b20.temperatures[i]);
        s_runtime.ds18b20.valid[i] = (err == ESP_OK);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "DS18B20[%016" PRIX64 "] read failed: %s",
                     (uint64_t)s_runtime.ds18b20.addresses[i], esp_err_to_name(err));
        }
    }

    return ESP_OK;
}

static void modules_poll_task(void *arg)
{
    (void)arg;
    app_watchdog_register_current_task("modules_poll");

    while (1) {
        bool changed = false;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_runtime.input_count; ++i) {
            input_runtime_t *in = &s_runtime.inputs[i];
            if (!in->used || !in->enabled) {
                continue;
            }
            bool state = gpio_get_level((gpio_num_t)in->gpio) != 0;
            if (in->inverted) {
                state = !state;
            }
            if (state != in->state) {
                in->state = state;
                changed = true;
            }
        }

        for (int i = 0; i < s_runtime.button_count; ++i) {
            button_runtime_t *btn = &s_runtime.buttons[i];
            if (!btn->used || !btn->enabled) {
                continue;
            }
            bool pressed = button_is_pressed(btn);
            int64_t now_us = esp_timer_get_time();

            if (pressed && !btn->last_pressed) {
                btn->pressed_since_us = now_us;
                btn->long_sent = false;
            } else if (!pressed && btn->last_pressed) {
                int held_ms = (btn->pressed_since_us > 0) ? (int)((now_us - btn->pressed_since_us) / 1000) : 0;
                if (!btn->long_sent && held_ms >= 40) {
                    if (execute_button_action_locked(&btn->short_action) == ESP_OK) {
                        changed = true;
                    }
                }
                btn->pressed_since_us = 0;
            } else if (pressed && !btn->long_sent && btn->pressed_since_us > 0) {
                int held_ms = (int)((now_us - btn->pressed_since_us) / 1000);
                if (held_ms >= btn->long_press_ms) {
                    btn->long_sent = true;
                    if (execute_button_action_locked(&btn->long_action) == ESP_OK) {
                        changed = true;
                    }
                }
            }

            btn->last_pressed = pressed;
        }
        xSemaphoreGive(s_lock);

        if (changed) {
            notify_runtime_changed();
        }

        app_watchdog_reset_current_task("modules_poll");
        vTaskDelay(pdMS_TO_TICKS(MODULES_POLL_PERIOD_MS));
    }
}

static void modules_sensor_task(void *arg)
{
    (void)arg;
    app_watchdog_register_current_task("modules_sensor");

    while (1) {
        bool changed = false;
        int64_t now_us = esp_timer_get_time();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_runtime.ds18b20.active &&
            (s_runtime.ds18b20.next_poll_us == 0 || now_us >= s_runtime.ds18b20.next_poll_us)) {
            if (read_ds18b20_locked() == ESP_OK) {
                changed = true;
            }
            s_runtime.ds18b20.next_poll_us = esp_timer_get_time() +
                                             ((int64_t)s_runtime.ds18b20.poll_interval_sec * 1000000LL);
        }

        for (int i = 0; i < s_runtime.sensor_count; ++i) {
            sensor_runtime_t *sensor = &s_runtime.sensors[i];
            if (!sensor->used || !sensor->enabled || !sensor->supported) {
                continue;
            }
            if (strcmp(sensor->type, "ds18b20_bus") == 0) {
                continue;
            }
            if (sensor->next_poll_us != 0 && now_us < sensor->next_poll_us) {
                continue;
            }
            if (read_sensor_locked(sensor) == ESP_OK) {
                changed = true;
            }
            sensor->next_poll_us = esp_timer_get_time() + ((int64_t)sensor->poll_interval_sec * 1000000LL);
            app_watchdog_reset_current_task("modules_sensor");
        }
        xSemaphoreGive(s_lock);

        if (changed) {
            notify_runtime_changed();
        }

        app_watchdog_reset_current_task("modules_sensor");
        vTaskDelay(pdMS_TO_TICKS(MODULES_SENSOR_TASK_PERIOD_MS));
    }
}

static esp_err_t set_master_output_locked(bool on)
{
    bool any = false;
    for (int i = 0; i < s_runtime.output_count; ++i) {
        output_runtime_t *out = &s_runtime.outputs[i];
        if (!out->used || !out->enabled) {
            continue;
        }
        if (set_output_power_locked(out, on) == ESP_OK) {
            any = true;
        }
    }
    return any ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool is_any_output_on_locked(void)
{
    for (int i = 0; i < s_runtime.output_count; ++i) {
        const output_runtime_t *out = &s_runtime.outputs[i];
        if (out->used && out->enabled && out->power) {
            return true;
        }
    }
    return false;
}

static cJSON *build_output_status_json(const output_runtime_t *out)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", out->id);
    cJSON_AddStringToObject(obj, "name", out->name);
    cJSON_AddStringToObject(obj, "type", output_type_to_text(out->type));
    cJSON_AddBoolToObject(obj, "enabled", out->enabled);
    cJSON_AddBoolToObject(obj, "supported", out->supported);
    cJSON_AddNumberToObject(obj, "gpio", out->gpio);
    cJSON_AddBoolToObject(obj, "power", out->power);

    if (out->type == OUTPUT_TYPE_RELAY) {
        cJSON_AddNumberToObject(obj, "active_level", out->cfg.relay.active_level);
    } else if (out->type == OUTPUT_TYPE_PWM) {
        cJSON_AddNumberToObject(obj, "level", out->cfg.pwm.level);
        cJSON_AddNumberToObject(obj, "freq_hz", out->cfg.pwm.freq_hz);
        cJSON_AddBoolToObject(obj, "inverted", out->cfg.pwm.inverted);
    } else if (out->type == OUTPUT_TYPE_WS2812) {
        cJSON_AddNumberToObject(obj, "level", out->cfg.ws2812.level);
        cJSON_AddNumberToObject(obj, "pixel_count", out->cfg.ws2812.pixel_count);
        cJSON_AddStringToObject(obj, "color_order", out->cfg.ws2812.color_order);
        cJSON *color = cJSON_AddObjectToObject(obj, "color");
        cJSON_AddNumberToObject(color, "r", out->cfg.ws2812.red);
        cJSON_AddNumberToObject(color, "g", out->cfg.ws2812.green);
        cJSON_AddNumberToObject(color, "b", out->cfg.ws2812.blue);
    }
    return obj;
}

static cJSON *build_input_status_json(const input_runtime_t *in)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", in->id);
    cJSON_AddStringToObject(obj, "name", in->name);
    cJSON_AddStringToObject(obj, "role", in->role);
    cJSON_AddBoolToObject(obj, "enabled", in->enabled);
    cJSON_AddNumberToObject(obj, "gpio", in->gpio);
    cJSON_AddBoolToObject(obj, "state", in->state);
    return obj;
}

static cJSON *build_button_status_json(const button_runtime_t *btn)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", btn->id);
    cJSON_AddStringToObject(obj, "name", btn->name);
    cJSON_AddBoolToObject(obj, "enabled", btn->enabled);
    cJSON_AddNumberToObject(obj, "gpio", btn->gpio);
    cJSON_AddNumberToObject(obj, "long_press_ms", btn->long_press_ms);
    cJSON *actions = cJSON_AddObjectToObject(obj, "actions");
    cJSON *short_a = cJSON_AddObjectToObject(actions, "short");
    cJSON_AddStringToObject(short_a, "type", button_action_type_to_text(btn->short_action.type));
    cJSON_AddStringToObject(short_a, "target", btn->short_action.target);
    cJSON *long_a = cJSON_AddObjectToObject(actions, "long");
    cJSON_AddStringToObject(long_a, "type", button_action_type_to_text(btn->long_action.type));
    cJSON_AddStringToObject(long_a, "target", btn->long_action.target);
    return obj;
}

static cJSON *build_sensor_status_json(const sensor_runtime_t *sensor)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", sensor->id);
    cJSON_AddStringToObject(obj, "name", sensor->name);
    cJSON_AddStringToObject(obj, "type", sensor->type);
    cJSON_AddBoolToObject(obj, "enabled", sensor->enabled);
    cJSON_AddBoolToObject(obj, "supported", sensor->supported);
    cJSON_AddStringToObject(obj, "status",
                            sensor->supported ? (sensor->data_valid || strcmp(sensor->type, "ds18b20_bus") == 0 ? "ready" : "waiting_data")
                                              : "driver_unavailable");
    if (sensor->gpio >= 0) {
        cJSON_AddNumberToObject(obj, "gpio", sensor->gpio);
    }
    if (sensor->sda_gpio >= 0) {
        cJSON_AddNumberToObject(obj, "sda_gpio", sensor->sda_gpio);
        cJSON_AddNumberToObject(obj, "scl_gpio", sensor->scl_gpio);
        cJSON_AddNumberToObject(obj, "address", sensor->address);
    }
    if (sensor->data_valid) {
        if (strcmp(sensor->type, "aht20") == 0 || strcmp(sensor->type, "sht3x") == 0 || strcmp(sensor->type, "bme280") == 0) {
            cJSON_AddNumberToObject(obj, "temperature_c", sensor->temperature_c);
            cJSON_AddNumberToObject(obj, "humidity_pct", sensor->humidity_pct);
        }
        if (strcmp(sensor->type, "bme280") == 0) {
            cJSON_AddNumberToObject(obj, "pressure_hpa", sensor->pressure_hpa);
        }
    }
    if (strcmp(sensor->type, "ds18b20_bus") == 0) {
        cJSON *devices = cJSON_AddArrayToObject(obj, "devices");
        for (int i = 0; i < s_runtime.ds18b20.device_count; ++i) {
            cJSON *dev = cJSON_CreateObject();
            char dev_id[40] = {0};
            char dev_name[64] = {0};
            snprintf(dev_id, sizeof(dev_id), "%s_%016" PRIX64, sensor->id, (uint64_t)s_runtime.ds18b20.addresses[i]);
            snprintf(dev_name, sizeof(dev_name), "%s %d", sensor->name, i + 1);
            cJSON_AddStringToObject(dev, "id", dev_id);
            cJSON_AddStringToObject(dev, "name", dev_name);
            cJSON_AddStringToObject(dev, "metric", "temperature_c");
            cJSON_AddStringToObject(dev, "address", dev_id + strlen(sensor->id) + 1);
            cJSON_AddBoolToObject(dev, "valid", s_runtime.ds18b20.valid[i]);
            if (s_runtime.ds18b20.valid[i]) {
                cJSON_AddNumberToObject(dev, "temperature_c", s_runtime.ds18b20.temperatures[i]);
            }
            cJSON_AddItemToArray(devices, dev);
        }
    }
    return obj;
}

esp_err_t modules_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_poll_task) {
        if (xTaskCreate(modules_poll_task, "modules_poll", 4096, NULL, 4, &s_poll_task) != pdPASS) {
            return ESP_FAIL;
        }
    }

    if (!s_sensor_task) {
        if (xTaskCreate(modules_sensor_task, "modules_sensor", 6144, NULL, 4, &s_sensor_task) != pdPASS) {
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t modules_apply_config(const cJSON *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    clear_runtime_locked();

    const cJSON *outputs = jobj(cfg, "outputs");
    const cJSON *inputs = jobj(cfg, "inputs");
    const cJSON *buttons = jobj(cfg, "buttons");
    const cJSON *sensors = jobj(cfg, "sensors");

    if (cJSON_IsArray((cJSON *)outputs)) {
        s_runtime.output_count = cJSON_GetArraySize((cJSON *)outputs);
        if (s_runtime.output_count > MODULES_MAX_OUTPUTS) {
            s_runtime.output_count = MODULES_MAX_OUTPUTS;
        }
        for (int i = 0; i < s_runtime.output_count; ++i) {
            ESP_ERROR_CHECK(configure_output(&s_runtime.outputs[i], cJSON_GetArrayItem((cJSON *)outputs, i), i));
        }
    }

    if (cJSON_IsArray((cJSON *)inputs)) {
        s_runtime.input_count = cJSON_GetArraySize((cJSON *)inputs);
        if (s_runtime.input_count > MODULES_MAX_INPUTS) {
            s_runtime.input_count = MODULES_MAX_INPUTS;
        }
        for (int i = 0; i < s_runtime.input_count; ++i) {
            ESP_ERROR_CHECK(configure_input(&s_runtime.inputs[i], cJSON_GetArrayItem((cJSON *)inputs, i)));
        }
    }

    if (cJSON_IsArray((cJSON *)buttons)) {
        s_runtime.button_count = cJSON_GetArraySize((cJSON *)buttons);
        if (s_runtime.button_count > MODULES_MAX_BUTTONS) {
            s_runtime.button_count = MODULES_MAX_BUTTONS;
        }
        for (int i = 0; i < s_runtime.button_count; ++i) {
            ESP_ERROR_CHECK(configure_button(&s_runtime.buttons[i], cJSON_GetArrayItem((cJSON *)buttons, i)));
        }
    }

    if (cJSON_IsArray((cJSON *)sensors)) {
        s_runtime.sensor_count = cJSON_GetArraySize((cJSON *)sensors);
        if (s_runtime.sensor_count > MODULES_MAX_SENSORS) {
            s_runtime.sensor_count = MODULES_MAX_SENSORS;
        }
        for (int i = 0; i < s_runtime.sensor_count; ++i) {
            ESP_ERROR_CHECK(configure_sensor(&s_runtime.sensors[i], cJSON_GetArrayItem((cJSON *)sensors, i)));
        }
    }

    xSemaphoreGive(s_lock);
    notify_runtime_changed();
    ESP_LOGI(TAG, "Applied runtime config: outputs=%d inputs=%d buttons=%d sensors=%d",
             s_runtime.output_count, s_runtime.input_count, s_runtime.button_count, s_runtime.sensor_count);
    return ESP_OK;
}

cJSON *modules_build_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *outputs = cJSON_AddArrayToObject(root, "outputs");
    cJSON *inputs = cJSON_AddArrayToObject(root, "inputs");
    cJSON *buttons = cJSON_AddArrayToObject(root, "buttons");
    cJSON *sensors = cJSON_AddArrayToObject(root, "sensors");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_runtime.output_count; ++i) {
        cJSON_AddItemToArray(outputs, build_output_status_json(&s_runtime.outputs[i]));
    }
    for (int i = 0; i < s_runtime.input_count; ++i) {
        cJSON_AddItemToArray(inputs, build_input_status_json(&s_runtime.inputs[i]));
    }
    for (int i = 0; i < s_runtime.button_count; ++i) {
        cJSON_AddItemToArray(buttons, build_button_status_json(&s_runtime.buttons[i]));
    }
    for (int i = 0; i < s_runtime.sensor_count; ++i) {
        cJSON_AddItemToArray(sensors, build_sensor_status_json(&s_runtime.sensors[i]));
    }
    cJSON_AddBoolToObject(root, "any_output_on", is_any_output_on_locked());
    xSemaphoreGive(s_lock);

    return root;
}

esp_err_t modules_action(const char *id, const cJSON *action, cJSON **out_response)
{
    if (!id || !action || !out_response) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    cJSON *resp = NULL;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (strcmp(id, "master") == 0) {
        const char *cmd = jstr(action, "command", "");
        if (strcmp(cmd, "on") == 0) {
            err = set_master_output_locked(true);
        } else if (strcmp(cmd, "off") == 0) {
            err = set_master_output_locked(false);
        } else if (strcmp(cmd, "toggle") == 0) {
            err = set_master_output_locked(!is_any_output_on_locked());
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
        if (err == ESP_OK) {
            resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "any_output_on", is_any_output_on_locked());
        }
    } else {
        output_runtime_t *out = find_output_locked(id);
        if (out) {
            if (jbool(action, "toggle", false)) {
                err = set_output_power_locked(out, !out->power);
            } else {
                const cJSON *set = jobj(action, "set");
                const cJSON *set_level = jobj(action, "set_level");
                const cJSON *color = jobj(action, "color");
                if (cJSON_IsBool(set)) {
                    err = set_output_power_locked(out, cJSON_IsTrue(set));
                } else if (cJSON_IsNumber(set_level)) {
                    err = set_output_level_locked(out, set_level->valueint);
                } else if (out->type == OUTPUT_TYPE_WS2812 && cJSON_IsObject((cJSON *)color)) {
                    out->cfg.ws2812.red = (uint8_t)jint(color, "r", out->cfg.ws2812.red);
                    out->cfg.ws2812.green = (uint8_t)jint(color, "g", out->cfg.ws2812.green);
                    out->cfg.ws2812.blue = (uint8_t)jint(color, "b", out->cfg.ws2812.blue);
                    err = output_apply_physical_state(out);
                } else {
                    err = ESP_ERR_INVALID_ARG;
                }
            }

            if (err == ESP_OK) {
                resp = build_output_status_json(out);
            }
        } else {
            input_runtime_t *in = find_input_locked(id);
            if (in) {
                resp = build_input_status_json(in);
                err = ESP_OK;
            }
        }
    }

    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        *out_response = resp;
        notify_runtime_changed();
        return ESP_OK;
    }

    if (resp) {
        cJSON_Delete(resp);
    }
    return err;
}

esp_err_t modules_set_master_output(bool on)
{
    esp_err_t err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = set_master_output_locked(on);
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        notify_runtime_changed();
    }
    return err;
}

bool modules_is_any_output_on(void)
{
    bool on = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    on = is_any_output_on_locked();
    xSemaphoreGive(s_lock);
    return on;
}

void modules_set_runtime_callback(modules_runtime_callback_t cb, void *ctx)
{
    s_runtime_cb = cb;
    s_runtime_cb_ctx = ctx;
}
