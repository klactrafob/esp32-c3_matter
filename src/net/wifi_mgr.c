#include "net/wifi_mgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "net/dns_server.h"
#include "app_config.h"

static const char *TAG = "wifi";

#define STA_RETRY_PERIOD_MS       5000
#define STA_TO_AP_FALLBACK_MS     8000
#define WIFI_MONITOR_PERIOD_MS    1000
#define STA_SCAN_HOLD_WITH_AP_CLIENT_MS 30000

static bool s_is_ap = false;
static bool s_sta_configured = false;
static bool s_ap_fallback_on = false;
static bool s_ap_always_on = false;
static volatile bool s_sta_has_ip = false;
static volatile int64_t s_sta_last_ok_us = 0;
static volatile int64_t s_sta_last_try_us = 0;
static volatile uint32_t s_ap_client_count = 0;
static volatile int64_t s_ap_last_client_evt_us = 0;
static TaskHandle_t s_wifi_mon_task = NULL;
static bool s_wifi_handlers_registered = false;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static volatile bool s_ap_restore_pending = false;
static volatile bool s_ap_restore_enabled = true;

static char s_ap_ssid[33] = {0};
static wifi_config_t s_ap_cfg = {0};
static wifi_config_t s_sta_cfg = {0};

bool wifi_mgr_is_ap(void) { return s_is_ap; }
const char *wifi_mgr_get_ap_ssid(void) { return s_ap_ssid; }
bool wifi_mgr_sta_configured(void) { return s_sta_configured; }
bool wifi_mgr_sta_has_ip(void) { return s_sta_has_ip; }
void wifi_mgr_set_ap_restore_enabled(bool enabled)
{
    s_ap_restore_enabled = enabled;
    if (!enabled) {
        s_ap_restore_pending = false;
    }
    ESP_LOGI(TAG, "AP auto-restore: %s", enabled ? "enabled" : "disabled");
}

static const cJSON *jobj(const cJSON *o, const char *k)
{
    if (!cJSON_IsObject((cJSON*)o)) return NULL;
    return cJSON_GetObjectItemCaseSensitive((cJSON*)o, k);
}

static const char *jstr(const cJSON *o, const char *k, const char *def)
{
    const cJSON *it = jobj(o, k);
    if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    return def;
}

static bool jhas_sta(const cJSON *cfg)
{
    const cJSON *net = jobj(cfg, "net");
    const cJSON *sta = jobj(net, "sta");
    const char *ssid = jstr(sta, "ssid", "");
    return ssid && ssid[0] != 0;
}

// Build AP SSID with unique chip ID suffix from factory MAC (eFuse).
static void build_ap_ssid_with_chip_id(const char *base_ssid, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;

    const char *base = (base_ssid && base_ssid[0]) ? base_ssid : APP_AP_SSID_DEFAULT;

    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        snprintf(out, out_len, "%s", base);
        return;
    }

    char suffix[8] = {0};
    snprintf(suffix, sizeof(suffix), "-%02X%02X%02X", mac[3], mac[4], mac[5]);

    size_t suffix_len = strlen(suffix);
    size_t max_base_len = (out_len > suffix_len + 1) ? (out_len - suffix_len - 1) : 0;
    snprintf(out, out_len, "%.*s%s", (int)max_base_len, base, suffix);
}

static void fill_ap_cfg(const char *ssid, const char *pass)
{
    memset(&s_ap_cfg, 0, sizeof(s_ap_cfg));
    memset(s_ap_ssid, 0, sizeof(s_ap_ssid));

    build_ap_ssid_with_chip_id(ssid, s_ap_ssid, sizeof(s_ap_ssid));
    strncpy((char*)s_ap_cfg.ap.ssid, s_ap_ssid, sizeof(s_ap_cfg.ap.ssid) - 1);
    strncpy((char*)s_ap_cfg.ap.password, pass ? pass : "", sizeof(s_ap_cfg.ap.password) - 1);
    s_ap_cfg.ap.ssid_len = strlen((const char*)s_ap_cfg.ap.ssid);
    s_ap_cfg.ap.channel = 1;
    s_ap_cfg.ap.ssid_hidden = 0;
    s_ap_cfg.ap.max_connection = 4;
    s_ap_cfg.ap.beacon_interval = 100;

    size_t pass_len = strlen((const char *)s_ap_cfg.ap.password);
    if (pass_len == 0) {
        s_ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    } else if (pass_len < 8 || pass_len > 63) {
        ESP_LOGW(TAG, "Invalid AP password length (%u), switching AP to OPEN",
                 (unsigned)pass_len);
        memset((char *)s_ap_cfg.ap.password, 0, sizeof(s_ap_cfg.ap.password));
        s_ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        s_ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
}

static void fill_sta_cfg(const char *ssid, const char *pass)
{
    memset(&s_sta_cfg, 0, sizeof(s_sta_cfg));
    strncpy((char*)s_sta_cfg.sta.ssid, ssid ? ssid : "", sizeof(s_sta_cfg.sta.ssid) - 1);
    strncpy((char*)s_sta_cfg.sta.password, pass ? pass : "", sizeof(s_sta_cfg.sta.password) - 1);
    s_sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
}

static void enable_ap_fallback(void)
{
    if (s_ap_fallback_on) return;

    ESP_LOGW(TAG, "STA unavailable, switching to AP fallback: %s", s_ap_ssid);

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return;
    if (esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg) != ESP_OK) return;

    s_ap_fallback_on = true;
    s_is_ap = true;
#if APP_CAPTIVE_PORTAL_ENABLE
    dns_server_start();
#endif
}

static void disable_ap_fallback(void)
{
    if (s_ap_always_on) return;
    if (!s_ap_fallback_on) return;

    ESP_LOGI(TAG, "STA restored, disabling AP fallback");

#if APP_CAPTIVE_PORTAL_ENABLE
    dns_server_stop();
#endif
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_fallback_on = false;
    s_is_ap = false;
}

static void ensure_ap_dhcp_server_started(void)
{
    if (!s_ap_netif) return;
    esp_err_t err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "dhcps start failed: %s", esp_err_to_name(err));
    }
}

static void ensure_ap_remains_enabled(void)
{
    if (!s_ap_restore_enabled) return;
    if (!s_ap_always_on) return;

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) return;

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        s_ap_restore_pending = false;
        return;
    }

    ESP_LOGW(TAG, "AP disabled externally (mode=%d), restoring APSTA", (int)mode);

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "restore AP mode failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "restore AP config failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "restore AP start failed: %s", esp_err_to_name(err));
        return;
    }

    if (s_sta_configured) {
        (void)esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg);
        (void)esp_wifi_connect();
    }

    s_ap_fallback_on = true;
    s_is_ap = true;
    ensure_ap_dhcp_server_started();
#if APP_CAPTIVE_PORTAL_ENABLE
    dns_server_start();
#endif
    s_ap_restore_pending = false;
}

static bool sta_target_available(void)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        return true;
    }

    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        return false;
    }

    uint16_t rec_count = ap_count;
    wifi_ap_record_t *records = (wifi_ap_record_t *)calloc(rec_count, sizeof(wifi_ap_record_t));
    if (!records) return false;

    bool found = false;
    if (esp_wifi_scan_get_ap_records(&rec_count, records) == ESP_OK) {
        const char *target = (const char *)s_sta_cfg.sta.ssid;
        for (uint16_t i = 0; i < rec_count; ++i) {
            if (strcmp((const char *)records[i].ssid, target) == 0) {
                found = true;
                break;
            }
        }
    }

    free(records);
    return found;
}

static int compare_ap_record_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_record_t *ra = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *rb = (const wifi_ap_record_t *)b;
    return ((int)rb->rssi - (int)ra->rssi);
}

static const char *auth_mode_to_text(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN: return "open";
        case WIFI_AUTH_WEP: return "wep";
        case WIFI_AUTH_WPA_PSK: return "wpa";
        case WIFI_AUTH_WPA2_PSK: return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
#if defined(WIFI_AUTH_WPA3_PSK)
        case WIFI_AUTH_WPA3_PSK: return "wpa3";
#endif
#if defined(WIFI_AUTH_WPA2_WPA3_PSK)
        case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
#endif
#if defined(WIFI_AUTH_WAPI_PSK)
        case WIFI_AUTH_WAPI_PSK: return "wapi";
#endif
#if defined(WIFI_AUTH_OWE)
        case WIFI_AUTH_OWE: return "owe";
#endif
#if defined(WIFI_AUTH_WPA3_ENT_192)
        case WIFI_AUTH_WPA3_ENT_192: return "wpa3-ent";
#endif
#if defined(WIFI_AUTH_WPA3_EXT_PSK)
        case WIFI_AUTH_WPA3_EXT_PSK: return "wpa3-ext";
#endif
#if defined(WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE)
        case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return "wpa3-ext-mixed";
#endif
        default: return "unknown";
    }
}

static bool networks_array_has_ssid(const cJSON *arr, const char *ssid)
{
    if (!cJSON_IsArray((cJSON *)arr) || !ssid || !ssid[0]) return false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, (cJSON *)arr) {
        cJSON *s = cJSON_GetObjectItemCaseSensitive(item, "ssid");
        if (cJSON_IsString(s) && s->valuestring && strcmp(s->valuestring, ssid) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t wifi_mgr_scan_networks(cJSON **out_networks)
{
    if (!out_networks) return ESP_ERR_INVALID_ARG;
    *out_networks = NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return ESP_ERR_NO_MEM;

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        cJSON_Delete(arr);
        return err;
    }

    bool switched_ap_to_apsta = false;
    if (mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            cJSON_Delete(arr);
            return err;
        }
        err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
        if (err != ESP_OK) {
            (void)esp_wifi_set_mode(WIFI_MODE_AP);
            cJSON_Delete(arr);
            return err;
        }
        switched_ap_to_apsta = true;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err == ESP_ERR_WIFI_STATE) {
        (void)esp_wifi_scan_stop();
        err = esp_wifi_scan_start(&scan_cfg, true);
    }
    if (err != ESP_OK) {
        if (switched_ap_to_apsta) {
            (void)esp_wifi_set_mode(WIFI_MODE_AP);
            (void)esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
        }
        cJSON_Delete(arr);
        return err;
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        if (switched_ap_to_apsta) {
            (void)esp_wifi_set_mode(WIFI_MODE_AP);
            (void)esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
        }
        cJSON_Delete(arr);
        return err;
    }

    if (ap_count > 0) {
        uint16_t rec_count = ap_count;
        wifi_ap_record_t *records = (wifi_ap_record_t *)calloc(rec_count, sizeof(wifi_ap_record_t));
        if (!records) {
            if (switched_ap_to_apsta) {
                (void)esp_wifi_set_mode(WIFI_MODE_AP);
                (void)esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
            }
            cJSON_Delete(arr);
            return ESP_ERR_NO_MEM;
        }

        err = esp_wifi_scan_get_ap_records(&rec_count, records);
        if (err == ESP_OK && rec_count > 0) {
            qsort(records, rec_count, sizeof(wifi_ap_record_t), compare_ap_record_rssi_desc);
            for (uint16_t i = 0; i < rec_count; ++i) {
                const char *ssid = (const char *)records[i].ssid;
                if (!ssid || !ssid[0]) continue;
                if (networks_array_has_ssid(arr, ssid)) continue;

                cJSON *it = cJSON_CreateObject();
                if (!it) continue;
                cJSON_AddStringToObject(it, "ssid", ssid);
                cJSON_AddNumberToObject(it, "rssi", records[i].rssi);
                cJSON_AddNumberToObject(it, "channel", records[i].primary);
                cJSON_AddStringToObject(it, "auth", auth_mode_to_text(records[i].authmode));
                cJSON_AddItemToArray(arr, it);
            }
        }

        free(records);
    }

    if (switched_ap_to_apsta) {
        esp_err_t restore_err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (restore_err == ESP_OK) {
            restore_err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
        }
        if (restore_err != ESP_OK) {
            cJSON_Delete(arr);
            return restore_err;
        }
    }

    *out_networks = arr;
    return ESP_OK;
}

static void wifi_monitor_task(void *arg)
{
    (void)arg;

    const int64_t retry_us = (int64_t)STA_RETRY_PERIOD_MS * 1000;
    const int64_t fallback_us = (int64_t)STA_TO_AP_FALLBACK_MS * 1000;

    while (1) {
        int64_t now_us = esp_timer_get_time();
        if (s_ap_restore_enabled && (s_ap_restore_pending || s_ap_always_on)) {
            ensure_ap_remains_enabled();
        }

        if (!s_sta_configured) {
            vTaskDelay(pdMS_TO_TICKS(WIFI_MONITOR_PERIOD_MS));
            continue;
        }

        if (s_sta_has_ip) {
            s_sta_last_ok_us = now_us;
            if (s_ap_fallback_on) disable_ap_fallback();
        } else {
            bool hold_scan_for_ap_client = s_ap_fallback_on &&
                                           (s_ap_client_count > 0) &&
                                           ((now_us - s_ap_last_client_evt_us) <
                                            ((int64_t)STA_SCAN_HOLD_WITH_AP_CLIENT_MS * 1000));

            if (hold_scan_for_ap_client) {
                vTaskDelay(pdMS_TO_TICKS(WIFI_MONITOR_PERIOD_MS));
                continue;
            }

            if ((now_us - s_sta_last_try_us) >= retry_us) {
                bool can_connect = true;
                if (s_ap_fallback_on) {
                    can_connect = sta_target_available();
                    if (!can_connect) {
                        ESP_LOGI(TAG, "STA SSID not visible yet, keep AP fallback");
                    }
                }

                esp_err_t err = can_connect ? esp_wifi_connect() : ESP_FAIL;
                if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) {
                    s_sta_last_try_us = now_us;
                    ESP_LOGI(TAG, "STA reconnect attempt...");
                } else if (!can_connect) {
                    s_sta_last_try_us = now_us;
                }
            }

            if (!s_ap_fallback_on && (now_us - s_sta_last_ok_us) >= fallback_us) {
                enable_ap_fallback();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_MONITOR_PERIOD_MS));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_sta_last_try_us = esp_timer_get_time();
        if (s_sta_configured) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ensure_ap_dhcp_server_started();
        ESP_LOGI(TAG, "AP interface started");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        ESP_LOGW(TAG, "AP interface stopped");
        if (s_ap_restore_enabled && s_ap_always_on) {
            s_ap_restore_pending = true;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_client_count++;
        s_ap_last_client_evt_us = esp_timer_get_time();
        const wifi_event_ap_staconnected_t *ev = (const wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "AP client joined: " MACSTR " aid=%d", MAC2STR(ev->mac), ev->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_client_count > 0) s_ap_client_count--;
        s_ap_last_client_evt_us = esp_timer_get_time();
        const wifi_event_ap_stadisconnected_t *ev = (const wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "AP client left: " MACSTR " aid=%d", MAC2STR(ev->mac), ev->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        s_sta_has_ip = false;

        if (s_sta_configured) {
            s_sta_last_try_us = esp_timer_get_time();
            ESP_LOGW(TAG, "STA disconnected (reason=%d), retry...", ev ? ev->reason : -1);
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t*)data;
        s_sta_has_ip = true;
        s_sta_last_ok_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        s_sta_has_ip = false;
        ESP_LOGW(TAG, "STA lost IP");
    }
}

static esp_err_t ensure_netif_event_loop(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    return ESP_OK;
}

static void ensure_default_wifi_netif_ap(void)
{
    s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
}

static void ensure_default_wifi_netif_sta(void)
{
    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
}

static esp_err_t init_common_wifi(void)
{
    esp_err_t err = ensure_netif_event_loop();
    if (err != ESP_OK) return err;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (!s_wifi_handlers_registered) {
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  &wifi_event_handler, NULL, NULL);
        if (err != ESP_OK) return err;
        err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                  &wifi_event_handler, NULL, NULL);
        if (err != ESP_OK) return err;
        s_wifi_handlers_registered = true;
    }
    return ESP_OK;
}

static esp_err_t start_ap_only(const char *ssid, const char *pass)
{
    esp_err_t err = ESP_OK;

    s_sta_configured = false;
    s_sta_has_ip = false;
    s_ap_fallback_on = false;
    s_ap_always_on = true;
    s_is_ap = true;

    fill_ap_cfg(ssid, pass);
    ESP_LOGI(TAG, "Starting AP-only: %s", s_ap_ssid);
    ESP_LOGI(TAG, "AP security: %s", (s_ap_cfg.ap.authmode == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2-PSK");
    ESP_LOGW(TAG, "STA is not configured (net.sta.ssid empty), staying in AP-only mode");

    err = ensure_netif_event_loop();
    if (err != ESP_OK) return err;
    ensure_default_wifi_netif_ap();
    err = init_common_wifi();
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;

    ESP_LOGI(TAG, "AP started: ssid=%s channel=%u auth=%d", s_ap_ssid,
             s_ap_cfg.ap.channel, s_ap_cfg.ap.authmode);

    if (s_wifi_mon_task == NULL) {
        BaseType_t ok = xTaskCreate(wifi_monitor_task, "wifi_mon", 3072, NULL, 5, &s_wifi_mon_task);
        if (ok != pdPASS) return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t start_sta_with_fallback(const char *sta_ssid, const char *sta_pass,
                                         const char *ap_ssid, const char *ap_pass)
{
    esp_err_t err = ESP_OK;

    s_sta_configured = true;
    s_sta_has_ip = false;
    s_ap_fallback_on = false;
    s_ap_always_on = false;
    s_is_ap = false;

    fill_sta_cfg(sta_ssid, sta_pass);
    fill_ap_cfg(ap_ssid, ap_pass);

    ESP_LOGI(TAG, "Starting STA with AP fallback; STA SSID: %s", sta_ssid);
    ESP_LOGI(TAG, "AP available during STA mode: %s", s_ap_ssid);

    err = ensure_netif_event_loop();
    if (err != ESP_OK) return err;
    ensure_default_wifi_netif_sta();
    ensure_default_wifi_netif_ap(); // keep AP netif ready for runtime fallback
    err = init_common_wifi();
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;

    ESP_LOGI(TAG, "STA started. AP fallback prepared: %s", s_ap_ssid);

    s_sta_last_ok_us = esp_timer_get_time();
    s_sta_last_try_us = 0;

    if (s_wifi_mon_task == NULL) {
        BaseType_t ok = xTaskCreate(wifi_monitor_task, "wifi_mon", 3072, NULL, 5, &s_wifi_mon_task);
        if (ok != pdPASS) return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t wifi_mgr_start_from_cfg(const cJSON *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    const cJSON *net = jobj(cfg, "net");
    const cJSON *ap  = jobj(net, "ap");
    const cJSON *sta = jobj(net, "sta");

    const char *ap_ssid = jstr(ap, "ssid", APP_AP_SSID_DEFAULT);
    const char *ap_pass = jstr(ap, "pass", APP_AP_PASS_DEFAULT);
    const char *st_ssid = jstr(sta, "ssid", "");
    const char *st_pass = jstr(sta, "pass", "");

    if (jhas_sta(cfg)) {
        return start_sta_with_fallback(st_ssid, st_pass, ap_ssid, ap_pass);
    }
    return start_ap_only(ap_ssid, ap_pass);
}

esp_err_t wifi_mgr_restart_from_cfg(const cJSON *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
    }

#if APP_CAPTIVE_PORTAL_ENABLE
    dns_server_stop();
#endif

    s_ap_restore_pending = false;
    s_ap_restore_enabled = true;
    s_ap_client_count = 0;
    s_ap_last_client_evt_us = esp_timer_get_time();

    vTaskDelay(pdMS_TO_TICKS(80));
    return wifi_mgr_start_from_cfg(cfg);
}
