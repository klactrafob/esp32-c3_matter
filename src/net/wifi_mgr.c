#include "net/wifi_mgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_watchdog.h"
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
#include "core/system_log.h"

static const char *TAG = "wifi";

#define STA_RETRY_PERIOD_MS    60000
#define WIFI_MONITOR_PERIOD_MS 1000

static bool s_is_ap = false;
static bool s_sta_configured = false;
static bool s_ap_always_on = false;
static volatile bool s_sta_has_ip = false;
static volatile int s_sta_rssi = 0;
static volatile int64_t s_sta_last_try_us = 0;
static TaskHandle_t s_wifi_mon_task = NULL;
static bool s_wifi_handlers_registered = false;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static volatile bool s_ap_restore_pending = false;
static cJSON *s_scan_cache = NULL;

static char s_ap_ssid[33] = {0};
static wifi_config_t s_ap_cfg = {0};
static wifi_config_t s_sta_cfg = {0};

static esp_err_t ensure_netif_event_loop(void);
static void ensure_default_wifi_netif_ap(void);
static void ensure_default_wifi_netif_sta(void);
static esp_err_t init_common_wifi(void);
static esp_err_t start_ap_only(const char *ssid, const char *pass, const char *device_name);
static esp_err_t start_sta_only(const char *sta_ssid, const char *sta_pass, const char *device_name);

bool wifi_mgr_is_ap(void) { return s_is_ap; }
const char *wifi_mgr_get_ap_ssid(void) { return s_ap_ssid; }
bool wifi_mgr_sta_configured(void) { return s_sta_configured; }
bool wifi_mgr_sta_has_ip(void) { return s_sta_has_ip; }
int wifi_mgr_get_sta_rssi(void) { return s_sta_rssi; }

static void replace_scan_cache(const cJSON *arr)
{
    cJSON *dup = arr ? cJSON_Duplicate((cJSON *)arr, 1) : NULL;
    if (arr && !dup) {
        return;
    }
    if (s_scan_cache) {
        cJSON_Delete(s_scan_cache);
    }
    s_scan_cache = dup;
}

esp_err_t wifi_mgr_get_cached_scan_networks(cJSON **out_networks)
{
    if (!out_networks) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_networks = NULL;
    if (!s_scan_cache) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *dup = cJSON_Duplicate(s_scan_cache, 1);
    if (!dup) {
        return ESP_ERR_NO_MEM;
    }

    *out_networks = dup;
    return ESP_OK;
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
    const cJSON *net = jobj(cfg, "connectivity");
    if (!cJSON_IsObject((cJSON *)net)) {
        net = jobj(cfg, "net");
    }
    const cJSON *sta = jobj(net, "sta");
    const char *ssid = jstr(sta, "ssid", "");
    return ssid && ssid[0] != 0;
}

static void build_hostname_from_device_name(const char *device_name, char *out, size_t out_len)
{
    size_t wr = 0;
    bool last_was_dash = false;

    if (!out || out_len == 0) {
        return;
    }

    if (device_name) {
        for (size_t i = 0; device_name[i] != 0 && wr + 1 < out_len; ++i) {
            unsigned char ch = (unsigned char)device_name[i];
            if (ch >= 'A' && ch <= 'Z') {
                out[wr++] = (char)(ch - 'A' + 'a');
                last_was_dash = false;
            } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
                out[wr++] = (char)ch;
                last_was_dash = false;
            } else if ((ch == ' ' || ch == '-' || ch == '_') && wr > 0 && !last_was_dash) {
                out[wr++] = '-';
                last_was_dash = true;
            }
        }
    }

    while (wr > 0 && out[wr - 1] == '-') {
        --wr;
    }
    out[wr] = 0;

    if (wr == 0) {
        snprintf(out, out_len, "%s", APP_HOSTNAME_DEFAULT);
        return;
    }

    if (out[0] >= '0' && out[0] <= '9') {
        char prefixed[33] = {0};
        if (snprintf(prefixed, sizeof(prefixed), "esp-%s", out) >= (int)sizeof(prefixed)) {
            snprintf(out, out_len, "%s", APP_HOSTNAME_DEFAULT);
            return;
        }
        snprintf(out, out_len, "%s", prefixed);
    }
}

static void apply_device_hostname(esp_netif_t *netif, const char *device_name, const char *if_name)
{
    if (!netif) {
        return;
    }

    char hostname[33] = {0};
    build_hostname_from_device_name(device_name, hostname, sizeof(hostname));

    esp_err_t err = esp_netif_set_hostname(netif, hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set %s hostname to %s: %s",
                 if_name, hostname, esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "%s hostname: %s", if_name, hostname);
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
    if (!s_ap_always_on) return;

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) return;

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        s_ap_restore_pending = false;
        return;
    }

    ESP_LOGW(TAG, "AP disabled externally (mode=%d), restoring AP", (int)mode);

    err = esp_wifi_set_mode(WIFI_MODE_AP);
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

    s_is_ap = true;
    ensure_ap_dhcp_server_started();
#if APP_CAPTIVE_PORTAL_ENABLE
    dns_server_start();
#endif
    s_ap_restore_pending = false;
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
        ensure_default_wifi_netif_sta();
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

    replace_scan_cache(arr);
    *out_networks = arr;
    return ESP_OK;
}

static void preload_scan_cache_before_ap_start(void)
{
    esp_err_t err = ensure_netif_event_loop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pre-scan skipped: netif init failed: %s", esp_err_to_name(err));
        return;
    }

    ensure_default_wifi_netif_sta();
    err = init_common_wifi();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pre-scan skipped: wifi init failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pre-scan skipped: set STA mode failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "pre-scan skipped: wifi start failed: %s", esp_err_to_name(err));
        return;
    }

    cJSON *networks = NULL;
    err = wifi_mgr_scan_networks(&networks);
    if (err == ESP_OK) {
        int found = cJSON_GetArraySize(networks);
        ESP_LOGI(TAG, "Pre-scanned %d Wi-Fi network(s) before enabling AP", found);
        cJSON_Delete(networks);
    } else {
        ESP_LOGW(TAG, "pre-scan failed: %s", esp_err_to_name(err));
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "pre-scan stop failed: %s", esp_err_to_name(err));
    }
}

static void wifi_monitor_task(void *arg)
{
    (void)arg;
    app_watchdog_register_current_task("wifi_mon");

    const int64_t retry_us = (int64_t)STA_RETRY_PERIOD_MS * 1000;

    while (1) {
        int64_t now_us = esp_timer_get_time();
        if (s_ap_restore_pending || s_ap_always_on) {
            ensure_ap_remains_enabled();
        }

        if (!s_sta_configured) {
            app_watchdog_reset_current_task("wifi_mon");
            vTaskDelay(pdMS_TO_TICKS(WIFI_MONITOR_PERIOD_MS));
            continue;
        }

        if (s_sta_has_ip) {
            s_is_ap = false;
            wifi_ap_record_t ap_info = {0};
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                s_sta_rssi = ap_info.rssi;
            }
        } else {
            s_sta_rssi = 0;
            if ((now_us - s_sta_last_try_us) >= retry_us) {
                s_sta_last_try_us = now_us;
                esp_err_t err = esp_wifi_connect();
                if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) {
                    ESP_LOGI(TAG, "STA reconnect attempt...");
                } else {
                    ESP_LOGW(TAG, "STA reconnect attempt failed: %s", esp_err_to_name(err));
                }
            }
        }

        app_watchdog_reset_current_task("wifi_mon");
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
        system_log_writef("wifi", "info", "AP started: %s", s_ap_ssid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        ESP_LOGW(TAG, "AP interface stopped");
        system_log_write("wifi", "warn", "AP interface stopped");
        if (s_ap_always_on) {
            s_ap_restore_pending = true;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        s_sta_has_ip = false;

        if (s_sta_configured) {
            s_sta_last_try_us = esp_timer_get_time();
            ESP_LOGW(TAG, "STA disconnected (reason=%d), next retry in %d sec",
                     ev ? ev->reason : -1, STA_RETRY_PERIOD_MS / 1000);
            system_log_writef("wifi", "warn", "STA disconnected, reason=%d",
                              ev ? ev->reason : -1);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t*)data;
        s_sta_has_ip = true;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        system_log_writef("wifi", "info", "STA got IP " IPSTR, IP2STR(&e->ip_info.ip));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        s_sta_has_ip = false;
        ESP_LOGW(TAG, "STA lost IP");
        system_log_write("wifi", "warn", "STA lost IP");
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

static esp_err_t start_ap_only(const char *ssid, const char *pass, const char *device_name)
{
    esp_err_t err = ESP_OK;

    s_sta_configured = false;
    s_sta_has_ip = false;
    s_ap_always_on = true;
    s_ap_restore_pending = false;
    s_is_ap = true;

    fill_ap_cfg(ssid, pass);
    ESP_LOGI(TAG, "Starting AP-only: %s", s_ap_ssid);
    ESP_LOGI(TAG, "AP security: %s", (s_ap_cfg.ap.authmode == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2-PSK");
    ESP_LOGW(TAG, "STA is not configured (net.sta.ssid empty), staying in AP-only mode");

    err = ensure_netif_event_loop();
    if (err != ESP_OK) return err;
    preload_scan_cache_before_ap_start();
    ensure_default_wifi_netif_ap();
    apply_device_hostname(s_ap_netif, device_name, "AP");
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

static esp_err_t start_sta_only(const char *sta_ssid, const char *sta_pass, const char *device_name)
{
    esp_err_t err = ESP_OK;

    s_sta_configured = true;
    s_sta_has_ip = false;
    s_ap_always_on = false;
    s_ap_restore_pending = false;
    s_is_ap = false;

    fill_sta_cfg(sta_ssid, sta_pass);

    ESP_LOGI(TAG, "Starting STA-only mode; STA SSID: %s", sta_ssid);

    err = ensure_netif_event_loop();
    if (err != ESP_OK) return err;
    ensure_default_wifi_netif_sta();
    apply_device_hostname(s_sta_netif, device_name, "STA");
    err = init_common_wifi();
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;

    ESP_LOGI(TAG, "STA started. AP fallback disabled for configured device");

    s_sta_last_try_us = esp_timer_get_time();

    if (s_wifi_mon_task == NULL) {
        BaseType_t ok = xTaskCreate(wifi_monitor_task, "wifi_mon", 3072, NULL, 5, &s_wifi_mon_task);
        if (ok != pdPASS) return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t wifi_mgr_start_from_cfg(const cJSON *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    const cJSON *net = jobj(cfg, "connectivity");
    if (!cJSON_IsObject((cJSON *)net)) {
        net = jobj(cfg, "net");
    }
    const cJSON *device = jobj(cfg, "device");
    const cJSON *sta = jobj(net, "sta");
    const char *st_ssid = jstr(sta, "ssid", "");
    const char *st_pass = jstr(sta, "pass", "");
    const char *device_name = jstr(device, "name", APP_AP_SSID_DEFAULT);

    if (jhas_sta(cfg)) {
        return start_sta_only(st_ssid, st_pass, device_name);
    }
    const cJSON *ap  = jobj(net, "ap");
    const char *ap_ssid = jstr(ap, "ssid", device_name && device_name[0] ? device_name : APP_AP_SSID_DEFAULT);
    const char *ap_pass = jstr(ap, "pass", APP_AP_PASS_DEFAULT);
    return start_ap_only(ap_ssid, ap_pass, device_name);
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

    vTaskDelay(pdMS_TO_TICKS(80));
    return wifi_mgr_start_from_cfg(cfg);
}
