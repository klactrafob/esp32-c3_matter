#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char sta_ssid[33];
    char sta_pass[65];

    char ap_ssid[33];
    char ap_pass[65];

    char hostname[33];
} cfg_t;

void cfg_set_defaults(cfg_t *cfg);

esp_err_t cfg_load_or_default(cfg_t *cfg);
esp_err_t cfg_save(const cfg_t *cfg);
esp_err_t cfg_factory_reset(void);

bool cfg_has_sta_credentials(const cfg_t *cfg);
