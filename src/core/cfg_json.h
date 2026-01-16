#pragma once
#include "esp_err.h"
#include "cJSON.h"

// Возвращает "живой" указатель на текущий конфиг (не освобождать!)
const cJSON *cfg_json_get(void);

// Перечитать из NVS (или установить дефолт)
esp_err_t cfg_json_load_or_default(void);

// Сохранить новый конфиг (копирует JSON внутрь)
esp_err_t cfg_json_set_and_save(const cJSON *new_cfg);

// Применить дефолт и сохранить
esp_err_t cfg_json_reset_to_default(void);

// Factory reset: стереть namespace конфига
esp_err_t cfg_json_factory_reset(void);
