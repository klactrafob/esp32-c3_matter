#include "modules.h"
#include <string.h>
#include "esp_log.h"

#include "mod_relay.h"
#include "mod_pwm.h"
#include "mod_ws2812.h"

static const char *TAG = "modules";

esp_err_t modules_init(void)
{
    // пока ничего
    return ESP_OK;
}

static const cJSON *get_module_cfg(const cJSON *cfg, const char *name)
{
    const cJSON *mods = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "modules");
    if (!cJSON_IsObject(mods)) return NULL;
    return cJSON_GetObjectItemCaseSensitive((cJSON*)mods, name);
}

esp_err_t modules_apply_config(const cJSON *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    const cJSON *relay = get_module_cfg(cfg, "relay");
    const cJSON *pwm   = get_module_cfg(cfg, "pwm");
    const cJSON *ws    = get_module_cfg(cfg, "ws2812");

    ESP_ERROR_CHECK(mod_relay_apply(relay));
    ESP_ERROR_CHECK(mod_pwm_apply(pwm));
    ESP_ERROR_CHECK(mod_ws2812_apply(ws));

    ESP_LOGI(TAG, "Modules applied");
    return ESP_OK;
}

cJSON *modules_build_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *mods = cJSON_AddObjectToObject(root, "modules");

    cJSON_AddItemToObject(mods, "relay",  mod_relay_status_json());
    cJSON_AddItemToObject(mods, "pwm",    mod_pwm_status_json());
    cJSON_AddItemToObject(mods, "ws2812", mod_ws2812_status_json());

    return root;
}

esp_err_t modules_action(const char *name, const cJSON *action, cJSON **out_response)
{
    if (!name || !action || !out_response) return ESP_ERR_INVALID_ARG;

    if (strcmp(name, "relay") == 0)  return mod_relay_action(action, out_response);
    if (strcmp(name, "pwm") == 0)    return mod_pwm_action(action, out_response);
    if (strcmp(name, "ws2812") == 0) return mod_ws2812_action(action, out_response);

    return ESP_ERR_NOT_FOUND;
}
