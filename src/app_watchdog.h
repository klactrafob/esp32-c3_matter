#pragma once

#include "esp_err.h"

esp_err_t app_watchdog_ensure_init(void);
void app_watchdog_register_current_task(const char *tag);
void app_watchdog_reset_current_task(const char *tag);
void app_watchdog_unregister_current_task(const char *tag);
