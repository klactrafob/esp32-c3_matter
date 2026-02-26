#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mod_relay_apply(const cJSON *cfg);
cJSON *mod_relay_status_json(void);
esp_err_t mod_relay_action(const cJSON *action, cJSON **out_response);
esp_err_t mod_relay_set_state(bool on);
esp_err_t mod_relay_get_state(bool *on);

#ifdef __cplusplus
}
#endif
