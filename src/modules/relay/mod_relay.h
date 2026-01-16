#pragma once
#include "esp_err.h"
#include "cJSON.h"

esp_err_t mod_relay_apply(const cJSON *cfg);
cJSON *mod_relay_status_json(void);
esp_err_t mod_relay_action(const cJSON *action, cJSON **out_response);
