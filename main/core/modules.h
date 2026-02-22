#pragma once
#include "esp_err.h"
#include "cJSON.h"

esp_err_t modules_init(void);

// Применить конфиг: включить/выключить и настроить модули
esp_err_t modules_apply_config(const cJSON *cfg);

// JSON статусы модулей (caller free)
cJSON *modules_build_status_json(void);

// Действия: POST /api/modules/<name>/action
esp_err_t modules_action(const char *name, const cJSON *action, cJSON **out_response);
