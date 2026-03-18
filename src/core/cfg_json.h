#pragma once

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

const cJSON *cfg_json_get(void);
const char *cfg_json_last_error(void);

esp_err_t cfg_json_load_or_default(void);
esp_err_t cfg_json_set_and_save(const cJSON *new_cfg);
esp_err_t cfg_json_reset_to_default(void);

// Compatibility no-op kept so older call sites don't force a legacy profile anymore.
esp_err_t cfg_json_force_relay_gpio12_profile(void);

// Clear only Wi-Fi and MQTT connection settings, keep device/peripheral config.
esp_err_t cfg_json_clear_connectivity(void);

// Full factory reset: erase config namespace.
esp_err_t cfg_json_factory_reset(void);

#ifdef __cplusplus
}
#endif
