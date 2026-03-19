#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_mgr_start_from_cfg(const cJSON *cfg);
esp_err_t wifi_mgr_restart_from_cfg(const cJSON *cfg);
esp_err_t wifi_mgr_scan_networks(cJSON **out_networks);
esp_err_t wifi_mgr_get_cached_scan_networks(cJSON **out_networks);
bool wifi_mgr_is_ap(void);
const char *wifi_mgr_get_ap_ssid(void);
bool wifi_mgr_sta_configured(void);
bool wifi_mgr_sta_has_ip(void);

#ifdef __cplusplus
}
#endif
