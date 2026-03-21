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
#include "esp_adc/adc_oneshot.h"
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
    OUTPUT_TYPE_SERVO_3WIRE,
    OUTPUT_TYPE_SERVO_5WIRE,
} output_type_t;

typedef enum {
    WS2812_MODE_RGB = 0,
    WS2812_MODE_MONO_TRIPLET,
} ws2812_mode_t;

typedef enum {
    WS2812_TRANSITION_NONE = 0,
    WS2812_TRANSITION_FADE,
    WS2812_TRANSITION_WIPE,
} ws2812_transition_style_t;

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
    bool test_active;
    int64_t test_restore_at_us;
    bool test_restore_power;
    int test_restore_level;
    uint8_t test_restore_red;
    uint8_t test_restore_green;
    uint8_t test_restore_blue;
    union {
        struct {
            int active_level;
            bool default_on;
        } relay;
        struct {
            bool inverted;
            int freq_hz;
            int level;
            int max_level_pct;
            int power_relay_gpio;
            int power_relay_active_level;
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
            ws2812_mode_t mode;
            ws2812_transition_style_t transition_style;
            int transition_ms;
            int applied_level;
            uint8_t applied_red;
            uint8_t applied_green;
            uint8_t applied_blue;
            bool transition_active;
            bool transition_use_wipe;
            int64_t transition_started_us;
            int64_t transition_duration_us;
            int start_level;
            int target_level;
            uint8_t start_red;
            uint8_t start_green;
            uint8_t start_blue;
            uint8_t target_red;
            uint8_t target_green;
            uint8_t target_blue;
            led_strip_handle_t strip;
        } ws2812;
        struct {
            int level;
            int min_us;
            int max_us;
            ledc_channel_t channel;
            ledc_timer_t timer;
        } servo_3wire;
        struct {
            int gpio_b;
            int feedback_gpio;
            int feedback_min_raw;
            int feedback_max_raw;
            int deadband_pct;
            int move_timeout_ms;
            bool reverse_direction;
            adc_channel_t adc_channel;
            int target_level;
            int current_level;
            int feedback_raw;
            int drive_state;
            bool moving;
            bool timed_out;
            int64_t drive_started_us;
        } servo_5wire;
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
    bool active;
    adc_oneshot_unit_handle_t handle;
    uint32_t configured_mask;
} adc_runtime_t;

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
    adc_runtime_t adc;
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
static ws2812_mode_t ws2812_mode_from_text(const char *mode);
static const char *ws2812_mode_to_text(ws2812_mode_t mode);
static ws2812_transition_style_t ws2812_transition_style_from_text(const char *style);
static const char *ws2812_transition_style_to_text(ws2812_transition_style_t style);
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
static esp_err_t start_output_test_locked(output_runtime_t *out, int duration_ms);
static bool process_output_test_locked(output_runtime_t *out, int64_t now_us);
static esp_err_t ensure_i2c_bus_locked(int sda_gpio, int scl_gpio, int freq_hz);
static esp_err_t ensure_ds18b20_bus_locked(const sensor_runtime_t *sensor);
static esp_err_t ensure_adc_channel_locked(int gpio, adc_channel_t *out_channel);
static esp_err_t read_sensor_locked(sensor_runtime_t *sensor);
static esp_err_t read_ds18b20_locked(void);
static esp_err_t set_pwm_power_relay_locked(output_runtime_t *out, bool on);
static bool output_supports_power_control(const output_runtime_t *out);
static bool output_supports_level_control(const output_runtime_t *out);
static esp_err_t servo_5wire_set_drive_locked(output_runtime_t *out, int logical_direction);
static int servo_5wire_feedback_to_level_locked(const output_runtime_t *out, int raw);
static esp_err_t servo_5wire_read_feedback_locked(output_runtime_t *out, int *out_raw, int *out_level);
static bool update_servo_5wire_control_locked(output_runtime_t *out, int64_t now_us);
static esp_err_t render_ws2812_frame_locked(output_runtime_t *out, int level, uint8_t red, uint8_t green, uint8_t blue, int wipe_active_segments);
static esp_err_t apply_ws2812_target_locked(output_runtime_t *out, bool allow_transition);
static void update_ws2812_transitions_locked(int64_t now_us);

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
    if (strcmp(type, "servo_3wire") == 0) {
        return OUTPUT_TYPE_SERVO_3WIRE;
    }
    if (strcmp(type, "servo_5wire") == 0) {
        return OUTPUT_TYPE_SERVO_5WIRE;
    }
    return OUTPUT_TYPE_NONE;
}

static const char *output_type_to_text(output_type_t type)
{
    switch (type) {
        case OUTPUT_TYPE_RELAY: return "relay";
        case OUTPUT_TYPE_PWM: return "pwm";
        case OUTPUT_TYPE_WS2812: return "ws2812";
        case OUTPUT_TYPE_SERVO_3WIRE: return "servo_3wire";
        case OUTPUT_TYPE_SERVO_5WIRE: return "servo_5wire";
        default: return "unknown";
    }
}

static ws2812_mode_t ws2812_mode_from_text(const char *mode)
{
    if (strcmp(mode, "mono_triplet") == 0) {
        return WS2812_MODE_MONO_TRIPLET;
    }
    return WS2812_MODE_RGB;
}

static const char *ws2812_mode_to_text(ws2812_mode_t mode)
{
    switch (mode) {
        case WS2812_MODE_MONO_TRIPLET: return "mono_triplet";
        case WS2812_MODE_RGB:
        default: return "rgb";
    }
}

static ws2812_transition_style_t ws2812_transition_style_from_text(const char *style)
{
    if (strcmp(style, "fade") == 0) {
        return WS2812_TRANSITION_FADE;
    }
    if (strcmp(style, "wipe") == 0) {
        return WS2812_TRANSITION_WIPE;
    }
    return WS2812_TRANSITION_NONE;
}

static const char *ws2812_transition_style_to_text(ws2812_transition_style_t style)
{
    switch (style) {
        case WS2812_TRANSITION_FADE: return "fade";
        case WS2812_TRANSITION_WIPE: return "wipe";
        case WS2812_TRANSITION_NONE:
        default: return "none";
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

static bool output_supports_power_control(const output_runtime_t *out)
{
    if (!out) {
        return false;
    }

    return out->type == OUTPUT_TYPE_RELAY ||
           out->type == OUTPUT_TYPE_PWM ||
           out->type == OUTPUT_TYPE_WS2812;
}

static bool output_supports_level_control(const output_runtime_t *out)
{
    if (!out) {
        return false;
    }

    return out->type == OUTPUT_TYPE_PWM ||
           out->type == OUTPUT_TYPE_WS2812 ||
           out->type == OUTPUT_TYPE_SERVO_3WIRE ||
           out->type == OUTPUT_TYPE_SERVO_5WIRE;
}

static esp_err_t set_pwm_power_relay_locked(output_runtime_t *out, bool on)
{
    if (!out || out->type != OUTPUT_TYPE_PWM || out->cfg.pwm.power_relay_gpio < 0) {
        return ESP_OK;
    }

    int level = on ? out->cfg.pwm.power_relay_active_level : (1 - out->cfg.pwm.power_relay_active_level);
    return gpio_set_level(out->cfg.pwm.power_relay_gpio, level);
}

static esp_err_t ensure_adc_channel_locked(int gpio, adc_channel_t *out_channel)
{
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    esp_err_t err;

    if (adc_oneshot_io_to_channel(gpio, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!s_runtime.adc.handle) {
        adc_oneshot_unit_init_cfg_t init_cfg = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&init_cfg, &s_runtime.adc.handle),
                            TAG, "adc init failed");
        s_runtime.adc.active = true;
        s_runtime.adc.configured_mask = 0;
    }

    if ((s_runtime.adc.configured_mask & (1U << channel)) == 0U) {
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_oneshot_config_channel(s_runtime.adc.handle, channel, &chan_cfg);
        if (err != ESP_OK) {
            return err;
        }
        s_runtime.adc.configured_mask |= (1U << channel);
    }

    if (out_channel) {
        *out_channel = channel;
    }
    return ESP_OK;
}

static esp_err_t servo_5wire_set_drive_locked(output_runtime_t *out, int logical_direction)
{
    int drive = logical_direction;
    int level_a = 0;
    int level_b = 0;

    if (!out || out->type != OUTPUT_TYPE_SERVO_5WIRE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (drive > 0) {
        drive = 1;
    } else if (drive < 0) {
        drive = -1;
    } else {
        drive = 0;
    }

    if (out->cfg.servo_5wire.reverse_direction) {
        drive = -drive;
    }

    if (drive > 0) {
        level_a = 1;
        level_b = 0;
    } else if (drive < 0) {
        level_a = 0;
        level_b = 1;
    }

    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)out->gpio, level_a), TAG, "servo a drive failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)out->cfg.servo_5wire.gpio_b, level_b), TAG, "servo b drive failed");
    out->cfg.servo_5wire.drive_state = logical_direction > 0 ? 1 : (logical_direction < 0 ? -1 : 0);
    return ESP_OK;
}

static int servo_5wire_feedback_to_level_locked(const output_runtime_t *out, int raw)
{
    int min_raw;
    int max_raw;
    int pct;

    if (!out || out->type != OUTPUT_TYPE_SERVO_5WIRE) {
        return 0;
    }

    min_raw = out->cfg.servo_5wire.feedback_min_raw;
    max_raw = out->cfg.servo_5wire.feedback_max_raw;
    if (min_raw == max_raw) {
        return 0;
    }

    if (min_raw < max_raw) {
        pct = (int)(((int64_t)(raw - min_raw) * 100LL) / (int64_t)(max_raw - min_raw));
    } else {
        pct = (int)(((int64_t)(min_raw - raw) * 100LL) / (int64_t)(min_raw - max_raw));
    }

    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    return pct;
}

static esp_err_t servo_5wire_read_feedback_locked(output_runtime_t *out, int *out_raw, int *out_level)
{
    int sum = 0;
    int raw = 0;

    if (!out || out->type != OUTPUT_TYPE_SERVO_5WIRE || !s_runtime.adc.handle) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < 4; ++i) {
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_runtime.adc.handle, out->cfg.servo_5wire.adc_channel, &raw),
                            TAG, "servo feedback read failed");
        sum += raw;
    }

    raw = sum / 4;
    out->cfg.servo_5wire.feedback_raw = raw;
    out->cfg.servo_5wire.current_level = servo_5wire_feedback_to_level_locked(out, raw);

    if (out_raw) {
        *out_raw = raw;
    }
    if (out_level) {
        *out_level = out->cfg.servo_5wire.current_level;
    }
    return ESP_OK;
}

static bool update_servo_5wire_control_locked(output_runtime_t *out, int64_t now_us)
{
    int previous_level;
    bool previous_moving;
    bool previous_timeout;
    int current_level = 0;
    int error = 0;
    int direction = 0;
    bool changed = false;

    if (!out || out->type != OUTPUT_TYPE_SERVO_5WIRE || !out->enabled || !out->supported) {
        return false;
    }

    previous_level = out->cfg.servo_5wire.current_level;
    previous_moving = out->cfg.servo_5wire.moving;
    previous_timeout = out->cfg.servo_5wire.timed_out;

    if (servo_5wire_read_feedback_locked(out, NULL, &current_level) != ESP_OK) {
        (void)servo_5wire_set_drive_locked(out, 0);
        out->cfg.servo_5wire.moving = false;
        return previous_moving;
    }

    error = out->cfg.servo_5wire.target_level - current_level;
    if (error > out->cfg.servo_5wire.deadband_pct) {
        direction = 1;
    } else if (error < -out->cfg.servo_5wire.deadband_pct) {
        direction = -1;
    }

    if (direction == 0) {
        (void)servo_5wire_set_drive_locked(out, 0);
        out->cfg.servo_5wire.moving = false;
        out->cfg.servo_5wire.timed_out = false;
        out->cfg.servo_5wire.drive_started_us = 0;
    } else if (out->cfg.servo_5wire.timed_out) {
        (void)servo_5wire_set_drive_locked(out, 0);
        out->cfg.servo_5wire.moving = false;
    } else {
        if (!out->cfg.servo_5wire.moving || out->cfg.servo_5wire.drive_state != direction) {
            out->cfg.servo_5wire.drive_started_us = now_us;
            out->cfg.servo_5wire.timed_out = false;
        }

        if (out->cfg.servo_5wire.drive_started_us > 0 &&
            (now_us - out->cfg.servo_5wire.drive_started_us) >= ((int64_t)out->cfg.servo_5wire.move_timeout_ms * 1000LL)) {
            (void)servo_5wire_set_drive_locked(out, 0);
            out->cfg.servo_5wire.moving = false;
            out->cfg.servo_5wire.timed_out = true;
            out->cfg.servo_5wire.drive_started_us = 0;
        } else {
            (void)servo_5wire_set_drive_locked(out, direction);
            out->cfg.servo_5wire.moving = true;
            out->cfg.servo_5wire.timed_out = false;
        }
    }

    changed = previous_level != out->cfg.servo_5wire.current_level ||
              previous_moving != out->cfg.servo_5wire.moving ||
              previous_timeout != out->cfg.servo_5wire.timed_out;
    out->power = out->cfg.servo_5wire.moving;
    return changed;
}

static bool ws2812_color_order_valid(const char *order)
{
    static const char *const k_valid_orders[] = {
        "RGB", "RBG", "GRB", "GBR", "BRG", "BGR",
    };

    if (!order || order[0] == 0) {
        return false;
    }

    for (size_t i = 0; i < sizeof(k_valid_orders) / sizeof(k_valid_orders[0]); ++i) {
        if (strcmp(order, k_valid_orders[i]) == 0) {
            return true;
        }
    }

    return false;
}

static uint8_t ws2812_channel_value(char channel, uint8_t red, uint8_t green, uint8_t blue)
{
    switch (channel) {
        case 'R': return red;
        case 'G': return green;
        case 'B': return blue;
        default: return 0;
    }
}

static esp_err_t ws2812_set_pixel_ordered(output_runtime_t *out, int index, uint8_t red, uint8_t green, uint8_t blue)
{
    const char *order = ws2812_color_order_valid(out->cfg.ws2812.color_order) ? out->cfg.ws2812.color_order : "GRB";
    uint8_t wire_0 = ws2812_channel_value(order[0], red, green, blue);
    uint8_t wire_1 = ws2812_channel_value(order[1], red, green, blue);
    uint8_t wire_2 = ws2812_channel_value(order[2], red, green, blue);

    return led_strip_set_pixel(out->cfg.ws2812.strip, (uint32_t)index, wire_1, wire_0, wire_2);
}

static esp_err_t render_ws2812_frame_locked(output_runtime_t *out, int level, uint8_t red, uint8_t green, uint8_t blue, int wipe_active_segments)
{
    if (!out || out->type != OUTPUT_TYPE_WS2812 || !out->cfg.ws2812.strip) {
        return ESP_ERR_INVALID_STATE;
    }

    if (level < 0) {
        level = 0;
    }
    if (level > 100) {
        level = 100;
    }

    if (level <= 0) {
        ESP_ERROR_CHECK(led_strip_clear(out->cfg.ws2812.strip));
        return ESP_OK;
    }

    if (out->cfg.ws2812.mode == WS2812_MODE_MONO_TRIPLET) {
        int total_segments = out->cfg.ws2812.pixel_count * 3;
        int active_segments = wipe_active_segments;
        uint8_t mono = (uint8_t)(((uint32_t)255U * (uint32_t)level) / 100U);
        const char *order = ws2812_color_order_valid(out->cfg.ws2812.color_order) ? out->cfg.ws2812.color_order : "GRB";

        if (active_segments < 0 || active_segments > total_segments) {
            active_segments = total_segments;
        }

        for (int pixel = 0; pixel < out->cfg.ws2812.pixel_count; ++pixel) {
            uint8_t pixel_r = 0;
            uint8_t pixel_g = 0;
            uint8_t pixel_b = 0;

            for (int slot = 0; slot < 3; ++slot) {
                int seg_index = (pixel * 3) + slot;
                uint8_t channel_level = (seg_index < active_segments) ? mono : 0;
                switch (order[slot]) {
                    case 'R':
                        pixel_r = channel_level;
                        break;
                    case 'G':
                        pixel_g = channel_level;
                        break;
                    case 'B':
                        pixel_b = channel_level;
                        break;
                    default:
                        break;
                }
            }

            ESP_ERROR_CHECK(ws2812_set_pixel_ordered(out, pixel, pixel_r, pixel_g, pixel_b));
        }
    } else {
        uint8_t scaled_r = (uint8_t)(((uint32_t)red * (uint32_t)level) / 100U);
        uint8_t scaled_g = (uint8_t)(((uint32_t)green * (uint32_t)level) / 100U);
        uint8_t scaled_b = (uint8_t)(((uint32_t)blue * (uint32_t)level) / 100U);

        for (int i = 0; i < out->cfg.ws2812.pixel_count; ++i) {
            ESP_ERROR_CHECK(ws2812_set_pixel_ordered(out, i, scaled_r, scaled_g, scaled_b));
        }
    }

    return led_strip_refresh(out->cfg.ws2812.strip);
}

static esp_err_t apply_ws2812_target_locked(output_runtime_t *out, bool allow_transition)
{
    int target_level;
    bool wants_wipe = false;

    if (!out || out->type != OUTPUT_TYPE_WS2812) {
        return ESP_ERR_INVALID_ARG;
    }

    target_level = out->power ? out->cfg.ws2812.level : 0;
    wants_wipe = (out->cfg.ws2812.mode == WS2812_MODE_MONO_TRIPLET) &&
                 (out->cfg.ws2812.transition_style == WS2812_TRANSITION_WIPE) &&
                 ((out->cfg.ws2812.applied_level == 0 && target_level > 0) ||
                  (out->cfg.ws2812.applied_level > 0 && target_level == 0));

    if (out->cfg.ws2812.applied_level == target_level &&
        out->cfg.ws2812.applied_red == out->cfg.ws2812.red &&
        out->cfg.ws2812.applied_green == out->cfg.ws2812.green &&
        out->cfg.ws2812.applied_blue == out->cfg.ws2812.blue) {
        out->cfg.ws2812.transition_active = false;
        out->cfg.ws2812.transition_use_wipe = false;
        return render_ws2812_frame_locked(out, out->cfg.ws2812.applied_level,
                                          out->cfg.ws2812.applied_red,
                                          out->cfg.ws2812.applied_green,
                                          out->cfg.ws2812.applied_blue, -1);
    }

    if (!allow_transition || out->cfg.ws2812.transition_style == WS2812_TRANSITION_NONE ||
        out->cfg.ws2812.transition_ms <= 0) {
        out->cfg.ws2812.transition_active = false;
        out->cfg.ws2812.transition_use_wipe = false;
        out->cfg.ws2812.applied_level = target_level;
        out->cfg.ws2812.applied_red = out->cfg.ws2812.red;
        out->cfg.ws2812.applied_green = out->cfg.ws2812.green;
        out->cfg.ws2812.applied_blue = out->cfg.ws2812.blue;
        return render_ws2812_frame_locked(out, out->cfg.ws2812.applied_level,
                                          out->cfg.ws2812.applied_red,
                                          out->cfg.ws2812.applied_green,
                                          out->cfg.ws2812.applied_blue, -1);
    }

    out->cfg.ws2812.transition_active = true;
    out->cfg.ws2812.transition_use_wipe = wants_wipe;
    out->cfg.ws2812.transition_started_us = esp_timer_get_time();
    out->cfg.ws2812.transition_duration_us = (int64_t)out->cfg.ws2812.transition_ms * 1000LL;
    out->cfg.ws2812.start_level = out->cfg.ws2812.applied_level;
    out->cfg.ws2812.target_level = target_level;
    out->cfg.ws2812.start_red = out->cfg.ws2812.applied_red;
    out->cfg.ws2812.start_green = out->cfg.ws2812.applied_green;
    out->cfg.ws2812.start_blue = out->cfg.ws2812.applied_blue;
    out->cfg.ws2812.target_red = out->cfg.ws2812.red;
    out->cfg.ws2812.target_green = out->cfg.ws2812.green;
    out->cfg.ws2812.target_blue = out->cfg.ws2812.blue;
    return ESP_OK;
}

static void update_ws2812_transitions_locked(int64_t now_us)
{
    for (int i = 0; i < s_runtime.output_count; ++i) {
        output_runtime_t *out = &s_runtime.outputs[i];
        int64_t elapsed_us;
        int level;
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        if (!out->used || !out->enabled || out->type != OUTPUT_TYPE_WS2812 || !out->cfg.ws2812.transition_active) {
            continue;
        }

        elapsed_us = now_us - out->cfg.ws2812.transition_started_us;
        if (elapsed_us < 0) {
            elapsed_us = 0;
        }

        if (elapsed_us >= out->cfg.ws2812.transition_duration_us) {
            out->cfg.ws2812.transition_active = false;
            out->cfg.ws2812.transition_use_wipe = false;
            out->cfg.ws2812.applied_level = out->cfg.ws2812.target_level;
            out->cfg.ws2812.applied_red = out->cfg.ws2812.target_red;
            out->cfg.ws2812.applied_green = out->cfg.ws2812.target_green;
            out->cfg.ws2812.applied_blue = out->cfg.ws2812.target_blue;
            (void)render_ws2812_frame_locked(out, out->cfg.ws2812.applied_level,
                                             out->cfg.ws2812.applied_red,
                                             out->cfg.ws2812.applied_green,
                                             out->cfg.ws2812.applied_blue, -1);
            continue;
        }

        if (out->cfg.ws2812.transition_use_wipe) {
            int total_segments = out->cfg.ws2812.pixel_count * 3;
            int active_segments;
            bool turning_on = (out->cfg.ws2812.start_level == 0 && out->cfg.ws2812.target_level > 0);

            if (turning_on) {
                active_segments = (int)((elapsed_us * total_segments + out->cfg.ws2812.transition_duration_us - 1) /
                                        out->cfg.ws2812.transition_duration_us);
            } else {
                active_segments = total_segments -
                                  (int)((elapsed_us * total_segments + out->cfg.ws2812.transition_duration_us - 1) /
                                        out->cfg.ws2812.transition_duration_us);
            }

            if (active_segments < 0) {
                active_segments = 0;
            }
            if (active_segments > total_segments) {
                active_segments = total_segments;
            }

            out->cfg.ws2812.applied_level = turning_on ? out->cfg.ws2812.target_level : out->cfg.ws2812.start_level;
            out->cfg.ws2812.applied_red = out->cfg.ws2812.target_red;
            out->cfg.ws2812.applied_green = out->cfg.ws2812.target_green;
            out->cfg.ws2812.applied_blue = out->cfg.ws2812.target_blue;
            (void)render_ws2812_frame_locked(out, out->cfg.ws2812.applied_level,
                                             out->cfg.ws2812.applied_red,
                                             out->cfg.ws2812.applied_green,
                                             out->cfg.ws2812.applied_blue,
                                             active_segments);
            continue;
        }

        level = out->cfg.ws2812.start_level +
                (int)(((int64_t)(out->cfg.ws2812.target_level - out->cfg.ws2812.start_level) * elapsed_us) /
                      out->cfg.ws2812.transition_duration_us);
        red = (uint8_t)(out->cfg.ws2812.start_red +
                        (int)(((int64_t)(out->cfg.ws2812.target_red - out->cfg.ws2812.start_red) * elapsed_us) /
                              out->cfg.ws2812.transition_duration_us));
        green = (uint8_t)(out->cfg.ws2812.start_green +
                          (int)(((int64_t)(out->cfg.ws2812.target_green - out->cfg.ws2812.start_green) * elapsed_us) /
                                out->cfg.ws2812.transition_duration_us));
        blue = (uint8_t)(out->cfg.ws2812.start_blue +
                         (int)(((int64_t)(out->cfg.ws2812.target_blue - out->cfg.ws2812.start_blue) * elapsed_us) /
                               out->cfg.ws2812.transition_duration_us));

        out->cfg.ws2812.applied_level = level;
        out->cfg.ws2812.applied_red = red;
        out->cfg.ws2812.applied_green = green;
        out->cfg.ws2812.applied_blue = blue;
        (void)render_ws2812_frame_locked(out, level, red, green, blue, -1);
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
            int requested = out->power ? level : 0;
            int limited = 0;
            bool supply_on = false;

            if (requested < 0) {
                requested = 0;
            }
            if (requested > 100) {
                requested = 100;
            }

            limited = (requested * out->cfg.pwm.max_level_pct) / 100;
            if (limited < 0) {
                limited = 0;
            }
            if (limited > 100) {
                limited = 100;
            }

            supply_on = out->power && requested > 0;
            if (supply_on) {
                esp_err_t err = set_pwm_power_relay_locked(out, true);
                if (err != ESP_OK) {
                    return err;
                }
            }

            if (out->cfg.pwm.inverted) {
                limited = 100 - limited;
            }
            duty = (uint32_t)((limited * (int)max_duty) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, out->cfg.pwm.channel, duty));
            esp_err_t err = ledc_update_duty(LEDC_LOW_SPEED_MODE, out->cfg.pwm.channel);
            if (err != ESP_OK) {
                return err;
            }

            if (!supply_on) {
                err = set_pwm_power_relay_locked(out, false);
                if (err != ESP_OK) {
                    return err;
                }
            }
            return ESP_OK;
        }
        case OUTPUT_TYPE_WS2812:
            return apply_ws2812_target_locked(out, true);
        case OUTPUT_TYPE_SERVO_3WIRE: {
            int level = out->cfg.servo_3wire.level;
            int pulse_us;
            uint32_t duty;
            const uint32_t max_duty = (1U << LEDC_TIMER_13_BIT) - 1U;

            if (level < 0) {
                level = 0;
            }
            if (level > 100) {
                level = 100;
            }

            pulse_us = out->cfg.servo_3wire.min_us +
                       (int)(((int64_t)(out->cfg.servo_3wire.max_us - out->cfg.servo_3wire.min_us) * level) / 100LL);
            duty = (uint32_t)(((int64_t)pulse_us * (int64_t)max_duty) / 20000LL);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, out->cfg.servo_3wire.channel, duty));
            return ledc_update_duty(LEDC_LOW_SPEED_MODE, out->cfg.servo_3wire.channel);
        }
        case OUTPUT_TYPE_SERVO_5WIRE:
            return servo_5wire_set_drive_locked(out, 0);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t set_output_power_locked(output_runtime_t *out, bool on)
{
    if (!out || !out->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!output_supports_power_control(out)) {
        return ESP_ERR_INVALID_ARG;
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
    } else if (out->type == OUTPUT_TYPE_SERVO_3WIRE) {
        out->cfg.servo_3wire.level = level;
        out->power = true;
    } else if (out->type == OUTPUT_TYPE_SERVO_5WIRE) {
        out->cfg.servo_5wire.target_level = level;
        out->cfg.servo_5wire.timed_out = false;
        out->power = out->cfg.servo_5wire.moving;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (out->type == OUTPUT_TYPE_SERVO_5WIRE) {
        (void)update_servo_5wire_control_locked(out, esp_timer_get_time());
        return ESP_OK;
    }

    return output_apply_physical_state(out);
}

static esp_err_t start_output_test_locked(output_runtime_t *out, int duration_ms)
{
    int test_level = 100;

    if (!out || !out->used || !out->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!out->supported) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (duration_ms < 100) {
        duration_ms = 100;
    }
    if (duration_ms > 10000) {
        duration_ms = 10000;
    }

    if (!out->test_active) {
        out->test_restore_power = out->power;

        switch (out->type) {
            case OUTPUT_TYPE_PWM:
                out->test_restore_level = out->cfg.pwm.level;
                break;
            case OUTPUT_TYPE_WS2812:
                out->test_restore_level = out->cfg.ws2812.level;
                out->test_restore_red = out->cfg.ws2812.red;
                out->test_restore_green = out->cfg.ws2812.green;
                out->test_restore_blue = out->cfg.ws2812.blue;
                break;
            case OUTPUT_TYPE_SERVO_3WIRE:
                out->test_restore_level = out->cfg.servo_3wire.level;
                break;
            case OUTPUT_TYPE_SERVO_5WIRE:
                out->test_restore_level = out->cfg.servo_5wire.target_level;
                break;
            default:
                break;
        }
    }

    out->test_active = true;
    out->test_restore_at_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000LL);

    switch (out->type) {
        case OUTPUT_TYPE_RELAY:
            out->power = true;
            return output_apply_physical_state(out);
        case OUTPUT_TYPE_PWM:
            out->cfg.pwm.level = 100;
            out->power = true;
            return output_apply_physical_state(out);
        case OUTPUT_TYPE_WS2812:
            out->cfg.ws2812.transition_active = false;
            out->cfg.ws2812.level = 100;
            out->cfg.ws2812.red = 255;
            out->cfg.ws2812.green = 255;
            out->cfg.ws2812.blue = 255;
            out->power = true;
            return apply_ws2812_target_locked(out, false);
        case OUTPUT_TYPE_SERVO_3WIRE:
            test_level = out->cfg.servo_3wire.level > 50 ? 0 : 100;
            out->cfg.servo_3wire.level = test_level;
            out->power = true;
            return output_apply_physical_state(out);
        case OUTPUT_TYPE_SERVO_5WIRE:
            test_level = out->cfg.servo_5wire.current_level > 50 ? 0 : 100;
            out->cfg.servo_5wire.target_level = test_level;
            out->cfg.servo_5wire.timed_out = false;
            (void)update_servo_5wire_control_locked(out, esp_timer_get_time());
            return ESP_OK;
        default:
            out->test_active = false;
            out->test_restore_at_us = 0;
            return ESP_ERR_NOT_SUPPORTED;
    }
}

static bool process_output_test_locked(output_runtime_t *out, int64_t now_us)
{
    esp_err_t err = ESP_OK;

    if (!out || !out->test_active) {
        return false;
    }
    if (now_us < out->test_restore_at_us) {
        return false;
    }

    out->test_active = false;
    out->test_restore_at_us = 0;

    switch (out->type) {
        case OUTPUT_TYPE_RELAY:
            out->power = out->test_restore_power;
            err = output_apply_physical_state(out);
            break;
        case OUTPUT_TYPE_PWM:
            out->cfg.pwm.level = out->test_restore_level;
            out->power = out->test_restore_power;
            err = output_apply_physical_state(out);
            break;
        case OUTPUT_TYPE_WS2812:
            out->cfg.ws2812.transition_active = false;
            out->cfg.ws2812.level = out->test_restore_level;
            out->cfg.ws2812.red = out->test_restore_red;
            out->cfg.ws2812.green = out->test_restore_green;
            out->cfg.ws2812.blue = out->test_restore_blue;
            out->power = out->test_restore_power;
            err = apply_ws2812_target_locked(out, false);
            break;
        case OUTPUT_TYPE_SERVO_3WIRE:
            out->cfg.servo_3wire.level = out->test_restore_level;
            out->power = true;
            err = output_apply_physical_state(out);
            break;
        case OUTPUT_TYPE_SERVO_5WIRE:
            out->cfg.servo_5wire.target_level = out->test_restore_level;
            out->cfg.servo_5wire.timed_out = false;
            (void)update_servo_5wire_control_locked(out, now_us);
            err = ESP_OK;
            break;
        default:
            err = ESP_ERR_NOT_SUPPORTED;
            break;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "output test restore failed for %s: %s", out->id, esp_err_to_name(err));
    }

    return true;
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
            if (out->cfg.pwm.power_relay_gpio >= 0) {
                gpio_reset_pin((gpio_num_t)out->cfg.pwm.power_relay_gpio);
            }
        }
        if (out->type == OUTPUT_TYPE_SERVO_3WIRE) {
            (void)ledc_stop(LEDC_LOW_SPEED_MODE, out->cfg.servo_3wire.channel, 0);
        }
        if (out->type == OUTPUT_TYPE_SERVO_5WIRE && out->cfg.servo_5wire.gpio_b >= 0) {
            (void)servo_5wire_set_drive_locked(out, 0);
            gpio_reset_pin((gpio_num_t)out->cfg.servo_5wire.gpio_b);
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
    if (s_runtime.adc.handle) {
        (void)adc_oneshot_del_unit(s_runtime.adc.handle);
        s_runtime.adc.handle = NULL;
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
        out->cfg.pwm.max_level_pct = jint(item, "max_level_pct", 100);
        if (out->cfg.pwm.max_level_pct < 1) {
            out->cfg.pwm.max_level_pct = 1;
        }
        if (out->cfg.pwm.max_level_pct > 100) {
            out->cfg.pwm.max_level_pct = 100;
        }
        out->cfg.pwm.power_relay_gpio = jint(item, "power_relay_gpio", -1);
        out->cfg.pwm.power_relay_active_level = jint(item, "power_relay_active_level", 1) ? 1 : 0;
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

        if (out->cfg.pwm.power_relay_gpio >= 0) {
            gpio_config_t relay_io = {
                .pin_bit_mask = 1ULL << out->cfg.pwm.power_relay_gpio,
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            ESP_ERROR_CHECK(gpio_config(&relay_io));
            ESP_ERROR_CHECK(set_pwm_power_relay_locked(out, false));
        }
        return output_apply_physical_state(out);
    }

    if (out->type == OUTPUT_TYPE_SERVO_3WIRE) {
        out->cfg.servo_3wire.level = jint(item, "default_level", 0);
        out->cfg.servo_3wire.min_us = jint(item, "min_us", 500);
        out->cfg.servo_3wire.max_us = jint(item, "max_us", 2500);
        if (out->cfg.servo_3wire.level < 0) {
            out->cfg.servo_3wire.level = 0;
        }
        if (out->cfg.servo_3wire.level > 100) {
            out->cfg.servo_3wire.level = 100;
        }
        if (out->cfg.servo_3wire.min_us < 400) {
            out->cfg.servo_3wire.min_us = 400;
        }
        if (out->cfg.servo_3wire.max_us > 2600) {
            out->cfg.servo_3wire.max_us = 2600;
        }
        if (out->cfg.servo_3wire.max_us <= out->cfg.servo_3wire.min_us) {
            out->cfg.servo_3wire.max_us = out->cfg.servo_3wire.min_us + 100;
        }

        out->cfg.servo_3wire.channel = (ledc_channel_t)index;
        out->cfg.servo_3wire.timer = (ledc_timer_t)(index % 4);
        out->power = true;

        ledc_timer_config_t timer_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_num = out->cfg.servo_3wire.timer,
            .duty_resolution = LEDC_TIMER_13_BIT,
            .freq_hz = 50,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

        ledc_channel_config_t chan_cfg = {
            .gpio_num = out->gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = out->cfg.servo_3wire.channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = out->cfg.servo_3wire.timer,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&chan_cfg));
        return output_apply_physical_state(out);
    }

    if (out->type == OUTPUT_TYPE_SERVO_5WIRE) {
        out->cfg.servo_5wire.gpio_b = jint(item, "gpio_b", -1);
        out->cfg.servo_5wire.feedback_gpio = jint(item, "feedback_gpio", -1);
        out->cfg.servo_5wire.feedback_min_raw = jint(item, "feedback_min_raw", 300);
        out->cfg.servo_5wire.feedback_max_raw = jint(item, "feedback_max_raw", 3700);
        out->cfg.servo_5wire.deadband_pct = jint(item, "deadband_pct", 2);
        out->cfg.servo_5wire.move_timeout_ms = jint(item, "move_timeout_ms", 15000);
        out->cfg.servo_5wire.reverse_direction = jbool(item, "reverse_direction", false);
        out->cfg.servo_5wire.target_level = jint(item, "default_level", 0);
        out->cfg.servo_5wire.feedback_raw = 0;
        out->cfg.servo_5wire.current_level = 0;
        out->cfg.servo_5wire.drive_state = 0;
        out->cfg.servo_5wire.moving = false;
        out->cfg.servo_5wire.timed_out = false;
        if (out->cfg.servo_5wire.target_level < 0) {
            out->cfg.servo_5wire.target_level = 0;
        }
        if (out->cfg.servo_5wire.target_level > 100) {
            out->cfg.servo_5wire.target_level = 100;
        }
        if (out->cfg.servo_5wire.deadband_pct < 1) {
            out->cfg.servo_5wire.deadband_pct = 1;
        }
        if (out->cfg.servo_5wire.deadband_pct > 20) {
            out->cfg.servo_5wire.deadband_pct = 20;
        }
        if (out->cfg.servo_5wire.move_timeout_ms < 1000) {
            out->cfg.servo_5wire.move_timeout_ms = 1000;
        }
        if (out->cfg.servo_5wire.move_timeout_ms > 60000) {
            out->cfg.servo_5wire.move_timeout_ms = 60000;
        }

        gpio_config_t io_b = {
            .pin_bit_mask = 1ULL << out->cfg.servo_5wire.gpio_b,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io_b));
        ESP_ERROR_CHECK(ensure_adc_channel_locked(out->cfg.servo_5wire.feedback_gpio,
                                                 &out->cfg.servo_5wire.adc_channel));
        out->supported = true;
        out->power = false;
        ESP_ERROR_CHECK(servo_5wire_set_drive_locked(out, 0));
        (void)update_servo_5wire_control_locked(out, esp_timer_get_time());
        return ESP_OK;
    }

    if (out->type == OUTPUT_TYPE_WS2812) {
        out->cfg.ws2812.pixel_count = jint(item, "pixel_count", 1);
        snprintf(out->cfg.ws2812.color_order, sizeof(out->cfg.ws2812.color_order), "%s",
                 jstr(item, "color_order", "GRB"));
        if (!ws2812_color_order_valid(out->cfg.ws2812.color_order)) {
            snprintf(out->cfg.ws2812.color_order, sizeof(out->cfg.ws2812.color_order), "%s", "GRB");
        }
        out->cfg.ws2812.default_power_on = jbool(item, "default_power_on", false);
        out->cfg.ws2812.mode = ws2812_mode_from_text(jstr(item, "mode", "rgb"));
        out->cfg.ws2812.transition_style = ws2812_transition_style_from_text(jstr(item, "transition_style", "none"));
        out->cfg.ws2812.transition_ms = jint(item, "transition_ms", 300);
        if (out->cfg.ws2812.transition_ms < 0) {
            out->cfg.ws2812.transition_ms = 0;
        }
        if (out->cfg.ws2812.transition_ms > 5000) {
            out->cfg.ws2812.transition_ms = 5000;
        }
        out->cfg.ws2812.level = out->cfg.ws2812.default_power_on ? 100 : 0;
        out->cfg.ws2812.red = 255;
        out->cfg.ws2812.green = 255;
        out->cfg.ws2812.blue = 255;
        out->cfg.ws2812.applied_level = out->cfg.ws2812.level;
        out->cfg.ws2812.applied_red = out->cfg.ws2812.red;
        out->cfg.ws2812.applied_green = out->cfg.ws2812.green;
        out->cfg.ws2812.applied_blue = out->cfg.ws2812.blue;
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
        return render_ws2812_frame_locked(out, out->cfg.ws2812.applied_level,
                                          out->cfg.ws2812.applied_red,
                                          out->cfg.ws2812.applied_green,
                                          out->cfg.ws2812.applied_blue, -1);
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
            if (!output_supports_level_control(out)) {
                return ESP_ERR_INVALID_ARG;
            }
            int level = 0;
            if (out->type == OUTPUT_TYPE_PWM) {
                level = out->cfg.pwm.level;
            } else if (out->type == OUTPUT_TYPE_WS2812) {
                level = out->cfg.ws2812.level;
            } else if (out->type == OUTPUT_TYPE_SERVO_3WIRE) {
                level = out->cfg.servo_3wire.level;
            } else if (out->type == OUTPUT_TYPE_SERVO_5WIRE) {
                level = out->cfg.servo_5wire.target_level;
            }
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
        int64_t now_us = esp_timer_get_time();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        update_ws2812_transitions_locked(now_us);
        for (int i = 0; i < s_runtime.output_count; ++i) {
            output_runtime_t *out = &s_runtime.outputs[i];
            if (!out->used || !out->enabled || !out->test_active) {
                continue;
            }
            if (process_output_test_locked(out, now_us)) {
                changed = true;
            }
        }
        for (int i = 0; i < s_runtime.output_count; ++i) {
            output_runtime_t *out = &s_runtime.outputs[i];
            if (!out->used || !out->enabled || out->type != OUTPUT_TYPE_SERVO_5WIRE) {
                continue;
            }
            if (update_servo_5wire_control_locked(out, now_us)) {
                changed = true;
            }
        }
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
        if (!out->used || !out->enabled || !output_supports_power_control(out)) {
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
        if (out->used && out->enabled && output_supports_power_control(out) && out->power) {
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
    cJSON_AddBoolToObject(obj, "test_active", out->test_active);

    if (out->type == OUTPUT_TYPE_RELAY) {
        cJSON_AddNumberToObject(obj, "active_level", out->cfg.relay.active_level);
    } else if (out->type == OUTPUT_TYPE_PWM) {
        cJSON_AddNumberToObject(obj, "level", out->cfg.pwm.level);
        cJSON_AddNumberToObject(obj, "freq_hz", out->cfg.pwm.freq_hz);
        cJSON_AddNumberToObject(obj, "max_level_pct", out->cfg.pwm.max_level_pct);
        cJSON_AddBoolToObject(obj, "inverted", out->cfg.pwm.inverted);
        if (out->cfg.pwm.power_relay_gpio >= 0) {
            cJSON_AddNumberToObject(obj, "power_relay_gpio", out->cfg.pwm.power_relay_gpio);
            cJSON_AddNumberToObject(obj, "power_relay_active_level", out->cfg.pwm.power_relay_active_level);
            cJSON_AddBoolToObject(obj, "power_relay_on", out->power && out->cfg.pwm.level > 0);
        }
    } else if (out->type == OUTPUT_TYPE_WS2812) {
        cJSON_AddNumberToObject(obj, "level", out->cfg.ws2812.level);
        cJSON_AddNumberToObject(obj, "pixel_count", out->cfg.ws2812.pixel_count);
        cJSON_AddStringToObject(obj, "mode", ws2812_mode_to_text(out->cfg.ws2812.mode));
        cJSON_AddStringToObject(obj, "color_order", out->cfg.ws2812.color_order);
        cJSON_AddStringToObject(obj, "transition_style", ws2812_transition_style_to_text(out->cfg.ws2812.transition_style));
        cJSON_AddNumberToObject(obj, "transition_ms", out->cfg.ws2812.transition_ms);
        if (out->cfg.ws2812.mode == WS2812_MODE_RGB) {
            cJSON *color = cJSON_AddObjectToObject(obj, "color");
            cJSON_AddNumberToObject(color, "r", out->cfg.ws2812.red);
            cJSON_AddNumberToObject(color, "g", out->cfg.ws2812.green);
            cJSON_AddNumberToObject(color, "b", out->cfg.ws2812.blue);
        }
    } else if (out->type == OUTPUT_TYPE_SERVO_3WIRE) {
        cJSON_AddNumberToObject(obj, "level", out->cfg.servo_3wire.level);
        cJSON_AddNumberToObject(obj, "min_us", out->cfg.servo_3wire.min_us);
        cJSON_AddNumberToObject(obj, "max_us", out->cfg.servo_3wire.max_us);
    } else if (out->type == OUTPUT_TYPE_SERVO_5WIRE) {
        cJSON_AddNumberToObject(obj, "level", out->cfg.servo_5wire.current_level);
        cJSON_AddNumberToObject(obj, "target_level", out->cfg.servo_5wire.target_level);
        cJSON_AddNumberToObject(obj, "gpio_b", out->cfg.servo_5wire.gpio_b);
        cJSON_AddNumberToObject(obj, "feedback_gpio", out->cfg.servo_5wire.feedback_gpio);
        cJSON_AddNumberToObject(obj, "feedback_raw", out->cfg.servo_5wire.feedback_raw);
        cJSON_AddNumberToObject(obj, "feedback_min_raw", out->cfg.servo_5wire.feedback_min_raw);
        cJSON_AddNumberToObject(obj, "feedback_max_raw", out->cfg.servo_5wire.feedback_max_raw);
        cJSON_AddNumberToObject(obj, "deadband_pct", out->cfg.servo_5wire.deadband_pct);
        cJSON_AddNumberToObject(obj, "move_timeout_ms", out->cfg.servo_5wire.move_timeout_ms);
        cJSON_AddBoolToObject(obj, "reverse_direction", out->cfg.servo_5wire.reverse_direction);
        cJSON_AddBoolToObject(obj, "moving", out->cfg.servo_5wire.moving);
        cJSON_AddBoolToObject(obj, "timed_out", out->cfg.servo_5wire.timed_out);
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
    cJSON_AddBoolToObject(obj, "pressed", btn->last_pressed);
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
            char dev_id[48] = {0};
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
            if (jbool(action, "test", false)) {
                err = start_output_test_locked(out, jint(action, "duration_ms", 1200));
            } else if (jbool(action, "toggle", false)) {
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
                    if (out->cfg.ws2812.mode == WS2812_MODE_RGB) {
                        out->cfg.ws2812.red = (uint8_t)jint(color, "r", out->cfg.ws2812.red);
                        out->cfg.ws2812.green = (uint8_t)jint(color, "g", out->cfg.ws2812.green);
                        out->cfg.ws2812.blue = (uint8_t)jint(color, "b", out->cfg.ws2812.blue);
                    }
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
