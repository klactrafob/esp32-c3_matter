#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

esp_err_t wifi_mgr_start_from_cfg(const cJSON *cfg);
bool wifi_mgr_is_ap(void);
