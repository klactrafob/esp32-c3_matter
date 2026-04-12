#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*modules_runtime_callback_t)(void *ctx);

esp_err_t modules_init(void);
esp_err_t modules_apply_config(const cJSON *cfg);
const char *modules_last_error(void);

cJSON *modules_build_status_json(void);
esp_err_t modules_action(const char *id, const cJSON *action, cJSON **out_response);

esp_err_t modules_set_master_output(bool on);
bool modules_is_any_output_on(void);

void modules_set_runtime_callback(modules_runtime_callback_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
