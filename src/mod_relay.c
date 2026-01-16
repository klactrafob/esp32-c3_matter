#include "mod_relay.h"
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "mod_relay";

static bool s_enabled = false;
static int  s_gpio = -1;
static int  s_active_level = 1;
static bool s_state = false;

static void relay_write(bool on)
{
    if (s_gpio < 0) return;
    int lvl = on ? s_active_level : (1 - s_active_level);
    gpio_set_level(s_gpio, lvl);
    s_state = on;
}

esp_err_t mod_relay_apply(const cJSON *cfg)
{
    bool enable = false;
    int gpio = 4;
    int active_level = 1;
    bool default_on = false;

    if (cJSON_IsObject(cfg)) {
        const cJSON *en = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "enable");
        const cJSON *gp = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "gpio");
        const cJSON *al = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "active_level");
        const cJSON *df = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "default_on");

        if (cJSON_IsBool(en)) enable = cJSON_IsTrue(en);
        if (cJSON_IsNumber(gp)) gpio = gp->valueint;
        if (cJSON_IsNumber(al)) active_level = (al->valueint ? 1 : 0);
        if (cJSON_IsBool(df)) default_on = cJSON_IsTrue(df);
    }

    if (!enable) {
        if (s_enabled) {
            ESP_LOGI(TAG, "disabled");
        }
        s_enabled = false;
        s_gpio = -1;
        s_state = false;
        return ESP_OK;
    }

    // init gpio
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    s_enabled = true;
    s_gpio = gpio;
    s_active_level = active_level;

    relay_write(default_on);

    ESP_LOGI(TAG, "enabled gpio=%d active=%d default_on=%d", s_gpio, s_active_level, (int)default_on);
    return ESP_OK;
}

cJSON *mod_relay_status_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", s_enabled);
    cJSON_AddNumberToObject(o, "gpio", s_gpio);
    cJSON_AddNumberToObject(o, "active_level", s_active_level);
    cJSON_AddBoolToObject(o, "state", s_state);
    return o;
}

// action: { "set": true } или { "toggle": true }
esp_err_t mod_relay_action(const cJSON *action, cJSON **out_response)
{
    if (!s_enabled) return ESP_ERR_INVALID_STATE;

    const cJSON *set = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "set");
    const cJSON *tgl = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "toggle");

    if (cJSON_IsBool(set)) {
        relay_write(cJSON_IsTrue(set));
    } else if (cJSON_IsBool(tgl) && cJSON_IsTrue(tgl)) {
        relay_write(!s_state);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    *out_response = mod_relay_status_json();
    return ESP_OK;
}
