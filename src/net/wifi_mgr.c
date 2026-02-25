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
#define STA_TO_AP_FALLBACK_MS    20000
#define WIFI_MONITOR_PERIOD_MS    1000
#define STA_SCAN_HOLD_WITH_AP_CLIENT_MS 30000

static bool s_is_ap = false;
static bool s_sta_configured = false;
static bool s_ap_fallback_on = false;
static volatile bool s_sta_has_ip = false;
static volatile int64_t s_sta_last_ok_us = 0;
static volatile int64_t s_sta_last_try_us = 0;
static volatile uint32_t s_ap_client_count = 0;
static volatile int64_t s_ap_last_client_evt_us = 0;
static TaskHandle_t s_wifi_mon_task = NULL;
static bool s_wifi_handlers_registered = false;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;

static char s_ap_ssid[33] = {0};
static wifi_config_t s_ap_cfg = {0};
static wifi_config_t s_sta_cfg = {0};

bool wifi_mgr_is_ap(void) { return s_is_ap; }
const char *wifi_mgr_get_ap_ssid(void) { return s_ap_ssid; }

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
    s_ap_cfg.ap.max_connection = 4;
    s_ap_cfg.ap.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
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

static void wifi_monitor_task(void *arg)
{
    (void)arg;

    const int64_t retry_us = (int64_t)STA_RETRY_PERIOD_MS * 1000;
    const int64_t fallback_us = (int64_t)STA_TO_AP_FALLBACK_MS * 1000;

    while (1) {
        if (!s_sta_configured) {
            vTaskDelay(pdMS_TO_TICKS(WIFI_MONITOR_PERIOD_MS));
            continue;
        }

        int64_t now_us = esp_timer_get_time();

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
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ensure_ap_dhcp_server_started();
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
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL));
        s_wifi_handlers_registered = true;
    }
    return ESP_OK;
}

static esp_err_t start_ap_only(const char *ssid, const char *pass)
{
    s_sta_configured = false;
    s_sta_has_ip = false;
    s_ap_fallback_on = false;
    s_is_ap = true;

    fill_ap_cfg(ssid, pass);
    ESP_LOGI(TAG, "Starting AP-only: %s", s_ap_ssid);
    ESP_LOGW(TAG, "STA is not configured (net.sta.ssid empty), staying in AP-only mode");

    ESP_ERROR_CHECK(ensure_netif_event_loop());
    ensure_default_wifi_netif_ap();
    ESP_ERROR_CHECK(init_common_wifi());

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

static esp_err_t start_sta_with_fallback(const char *sta_ssid, const char *sta_pass,
                                         const char *ap_ssid, const char *ap_pass)
{
    s_sta_configured = true;
    s_sta_has_ip = false;
    s_ap_fallback_on = false;
    s_is_ap = false;

    fill_sta_cfg(sta_ssid, sta_pass);
    fill_ap_cfg(ap_ssid, ap_pass);

    ESP_LOGI(TAG, "Starting STA with AP fallback; STA SSID: %s", sta_ssid);

    ESP_ERROR_CHECK(ensure_netif_event_loop());
    ensure_default_wifi_netif_sta();
    ensure_default_wifi_netif_ap(); // keep AP netif ready for fallback
    ESP_ERROR_CHECK(init_common_wifi());

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

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
