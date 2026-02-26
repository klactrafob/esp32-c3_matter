#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

esp_err_t wifi_mgr_start_from_cfg(const cJSON *cfg);
esp_err_t wifi_mgr_restart_from_cfg(const cJSON *cfg);
bool wifi_mgr_is_ap(void);
const char *wifi_mgr_get_ap_ssid(void);
