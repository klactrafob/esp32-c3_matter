#include "mod_ws2812.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "mod_ws2812";

static bool s_enabled = false;
static int  s_gpio = -1;
static int  s_count = 0;
static int  s_brightness = 0; // 0..100
static char s_effect[16] = "solid";

esp_err_t mod_ws2812_apply(const cJSON *cfg)
{
    bool enable = false;
    int gpio = 8;
    int count = 30;
    int brightness = 50;
    const char *effect = "solid";

    if (cJSON_IsObject(cfg)) {
        const cJSON *en = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "enable");
        const cJSON *gp = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "gpio");
        const cJSON *ct = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "count");
        const cJSON *br = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "brightness");
        const cJSON *ef = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "effect");

        if (cJSON_IsBool(en)) enable = cJSON_IsTrue(en);
        if (cJSON_IsNumber(gp)) gpio = gp->valueint;
        if (cJSON_IsNumber(ct)) count = ct->valueint;
        if (cJSON_IsNumber(br)) brightness = br->valueint;
        if (cJSON_IsString(ef) && ef->valuestring) effect = ef->valuestring;
    }

    if (count < 1) count = 1;
    if (count > 1024) count = 1024; // защита
    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;

    if (!enable) {
        if (s_enabled) ESP_LOGI(TAG, "disabled");
        s_enabled = false;
        s_gpio = -1;
        s_count = 0;
        s_brightness = 0;
        strncpy(s_effect, "solid", sizeof(s_effect)-1);
        return ESP_OK;
    }

    s_enabled = true;
    s_gpio = gpio;
    s_count = count;
    s_brightness = brightness;
    strncpy(s_effect, effect, sizeof(s_effect)-1);
    s_effect[sizeof(s_effect)-1] = 0;

    // Пока заглушка: тут позже добавим реальную RMT-реализацию WS2812
    ESP_LOGI(TAG, "enabled gpio=%d count=%d brightness=%d effect=%s (stub)",
             s_gpio, s_count, s_brightness, s_effect);

    return ESP_OK;
}

cJSON *mod_ws2812_status_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", s_enabled);
    cJSON_AddNumberToObject(o, "gpio", s_gpio);
    cJSON_AddNumberToObject(o, "count", s_count);
    cJSON_AddNumberToObject(o, "brightness", s_brightness);
    cJSON_AddStringToObject(o, "effect", s_effect);
    cJSON_AddBoolToObject(o, "stub", true);
    return o;
}

// action: { "effect":"rainbow" } или { "brightness": 80 }
esp_err_t mod_ws2812_action(const cJSON *action, cJSON **out_response)
{
    if (!s_enabled) return ESP_ERR_INVALID_STATE;

    const cJSON *ef = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "effect");
    const cJSON *br = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "brightness");

    bool changed = false;

    if (cJSON_IsString(ef) && ef->valuestring) {
        strncpy(s_effect, ef->valuestring, sizeof(s_effect)-1);
        s_effect[sizeof(s_effect)-1] = 0;
        changed = true;
    }
    if (cJSON_IsNumber(br)) {
        int b = br->valueint;
        if (b < 0) b = 0;
        if (b > 100) b = 100;
        s_brightness = b;
        changed = true;
    }

    if (!changed) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "action: brightness=%d effect=%s (stub)", s_brightness, s_effect);
    *out_response = mod_ws2812_status_json();
    return ESP_OK;
}
