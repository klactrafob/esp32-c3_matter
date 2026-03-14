#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_mgr_start_from_cfg(const cJSON *cfg);
esp_err_t mqtt_mgr_restart_from_cfg(const cJSON *cfg);
esp_err_t mqtt_mgr_notify_relay_state(bool on);
bool mqtt_mgr_is_connected(void);

#ifdef __cplusplus
}
#endif
