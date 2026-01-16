#include "mod_pwm.h"
#include <string.h>
#include "esp_log.h"
#include "driver/ledc.h"

static const char *TAG = "mod_pwm";

static bool s_enabled = false;
static int  s_gpio = -1;
static int  s_freq = 20000;
static int  s_res_bits = 10;
static int  s_duty = 0; // 0..100

static ledc_timer_t s_timer = LEDC_TIMER_0;
static ledc_channel_t s_channel = LEDC_CHANNEL_0;
static ledc_mode_t s_speed_mode = LEDC_LOW_SPEED_MODE;

static uint32_t duty_max(void)
{
    return (1UL << s_res_bits) - 1;
}

static esp_err_t pwm_apply_hw(void)
{
    ledc_timer_config_t t = {
        .speed_mode = s_speed_mode,
        .timer_num = s_timer,
        .duty_resolution = (ledc_timer_bit_t)s_res_bits,
        .freq_hz = s_freq,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t ch = {
        .gpio_num = s_gpio,
        .speed_mode = s_speed_mode,
        .channel = s_channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = s_timer,
        .duty = 0,
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));

    // set duty
    uint32_t d = (duty_max() * (uint32_t)s_duty) / 100UL;
    ESP_ERROR_CHECK(ledc_set_duty(s_speed_mode, s_channel, d));
    ESP_ERROR_CHECK(ledc_update_duty(s_speed_mode, s_channel));

    return ESP_OK;
}

esp_err_t mod_pwm_apply(const cJSON *cfg)
{
    bool enable = false;
    int gpio = 5;
    int freq = 20000;
    int res_bits = 10;
    int duty = 0;

    if (cJSON_IsObject(cfg)) {
        const cJSON *en = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "enable");
        const cJSON *gp = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "gpio");
        const cJSON *fr = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "freq");
        const cJSON *rb = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "res_bits");
        const cJSON *du = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "duty");

        if (cJSON_IsBool(en)) enable = cJSON_IsTrue(en);
        if (cJSON_IsNumber(gp)) gpio = gp->valueint;
        if (cJSON_IsNumber(fr)) freq = fr->valueint;
        if (cJSON_IsNumber(rb)) res_bits = rb->valueint;
        if (cJSON_IsNumber(du)) duty = du->valueint;
    }

    if (duty < 0) duty = 0;
    if (duty > 100) duty = 100;
    if (freq < 1) freq = 1;
    if (res_bits < 1) res_bits = 1;
    if (res_bits > 14) res_bits = 14; // LEDC limits vary; 14 safe-ish for many cases

    if (!enable) {
        if (s_enabled) {
            ledc_stop(s_speed_mode, s_channel, 0);
            ESP_LOGI(TAG, "disabled");
        }
        s_enabled = false;
        s_gpio = -1;
        s_duty = 0;
        return ESP_OK;
    }

    s_enabled = true;
    s_gpio = gpio;
    s_freq = freq;
    s_res_bits = res_bits;
    s_duty = duty;

    ESP_LOGI(TAG, "enabled gpio=%d freq=%d res=%d duty=%d", s_gpio, s_freq, s_res_bits, s_duty);
    return pwm_apply_hw();
}

cJSON *mod_pwm_status_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", s_enabled);
    cJSON_AddNumberToObject(o, "gpio", s_gpio);
    cJSON_AddNumberToObject(o, "freq", s_freq);
    cJSON_AddNumberToObject(o, "res_bits", s_res_bits);
    cJSON_AddNumberToObject(o, "duty", s_duty);
    return o;
}

// action: { "duty": 0..100 } или { "freq": 20000 } (применит с текущими)
esp_err_t mod_pwm_action(const cJSON *action, cJSON **out_response)
{
    if (!s_enabled) return ESP_ERR_INVALID_STATE;

    const cJSON *du = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "duty");
    const cJSON *fr = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "freq");

    bool changed = false;

    if (cJSON_IsNumber(du)) {
        int duty = du->valueint;
        if (duty < 0) duty = 0;
        if (duty > 100) duty = 100;
        s_duty = duty;
        changed = true;
    }
    if (cJSON_IsNumber(fr)) {
        int freq = fr->valueint;
        if (freq < 1) freq = 1;
        s_freq = freq;
        changed = true;
    }

    if (!changed) return ESP_ERR_INVALID_ARG;

    ESP_ERROR_CHECK(pwm_apply_hw());
    *out_response = mod_pwm_status_json();
    return ESP_OK;
}
