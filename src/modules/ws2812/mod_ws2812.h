#pragma once
#include "esp_err.h"
#include "cJSON.h"

esp_err_t mod_ws2812_apply(const cJSON *cfg);
cJSON *mod_ws2812_status_json(void);
esp_err_t mod_ws2812_action(const cJSON *action, cJSON **out_response);
