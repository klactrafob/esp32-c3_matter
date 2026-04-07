#include "net/web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "core/cfg_json.h"
#include "core/modules.h"
#include "core/system_log.h"
#include "net/dns_server.h"
#include "net/mqtt_mgr.h"
#include "net/web_ui.h"
#include "net/wifi_mgr.h"

static const char *TAG = "web";
static httpd_handle_t s_server = NULL;
static const size_t OTA_RECV_CHUNK = 4096;

typedef struct {
    cJSON *cfg_dup;
} apply_ctx_t;

static void apply_cfg_task(void *arg);

static const cJSON *jobj(const cJSON *obj, const char *key)
{
    if (!cJSON_IsObject((cJSON *)obj)) {
        return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
}

static const char *jstr(const cJSON *obj, const char *key, const char *def)
{
    const cJSON *item = jobj(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return def;
}

static bool jbool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *item = jobj(obj, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return def;
}

static bool captive_active(void)
{
#if APP_CAPTIVE_PORTAL_ENABLE
    return wifi_mgr_is_ap();
#else
    return false;
#endif
}

static const cJSON *get_web_auth_cfg(void)
{
    const cJSON *web = jobj(cfg_json_get(), "web");
    return jobj(web, "auth");
}

static bool is_auth_enabled(void)
{
    const cJSON *auth = get_web_auth_cfg();
    const char *password = jstr(auth, "password", "");
    return jbool(auth, "enable", false) && password[0] != 0;
}

static esp_err_t require_auth(httpd_req_t *req)
{
    char provided[96] = {0};
    const cJSON *auth = get_web_auth_cfg();
    const char *expected = jstr(auth, "password", "");

    if (!is_auth_enabled()) {
        return ESP_OK;
    }

    size_t len = httpd_req_get_hdr_value_len(req, "X-Auth-Token");
    if (len == 0 || len >= sizeof(provided)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, "auth required", HTTPD_RESP_USE_STRLEN);
    }

    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", provided, sizeof(provided)) != ESP_OK ||
        strcmp(provided, expected) != 0) {
        system_log_write("web", "warn", "Rejected API request with invalid auth token");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, "auth required", HTTPD_RESP_USE_STRLEN);
    }

    return ESP_OK;
}

static const char *reset_reason_text(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "other_wdt";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

static esp_err_t schedule_apply_from_current_cfg(void)
{
    apply_ctx_t *ctx = calloc(1, sizeof(apply_ctx_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    ctx->cfg_dup = cJSON_Duplicate((cJSON *)cfg_json_get(), 1);
    if (!ctx->cfg_dup) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(apply_cfg_task, "apply_cfg", 4096, ctx, 4, NULL) != pdPASS) {
        cJSON_Delete(ctx->cfg_dup);
        free(ctx);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t json_send(httpd_req_t *req, cJSON *obj, int status_code)
{
    char *out = cJSON_PrintUnformatted(obj);
    if (!out) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    if (status_code != 200) {
        char st[32];
        snprintf(st, sizeof(st), "%d", status_code);
        httpd_resp_set_status(req, st);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t read_body_json(httpd_req_t *req, cJSON **out_json)
{
    *out_json = NULL;

    int total = req->content_len;
    if (total <= 0 || total > 32768) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = calloc(1, (size_t)total + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        got += r;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_json = root;
    return ESP_OK;
}

static esp_err_t captive_redirect_to_root(httpd_req_t *req)
{
    if (captive_active()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

static esp_err_t handle_generate_204(httpd_req_t *req)
{
    if (captive_active()) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

static void apply_cfg_task(void *arg)
{
    apply_ctx_t *task_ctx = (apply_ctx_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(250));

    esp_err_t werr = wifi_mgr_restart_from_cfg(task_ctx->cfg_dup);
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "wifi reconfigure failed: %s", esp_err_to_name(werr));
    }

    esp_err_t merr = mqtt_mgr_restart_from_cfg(task_ctx->cfg_dup);
    if (merr != ESP_OK) {
        ESP_LOGE(TAG, "mqtt reconfigure failed: %s", esp_err_to_name(merr));
    }

    cJSON_Delete(task_ctx->cfg_dup);
    free(task_ctx);
    vTaskDelete(NULL);
}

static void factory_reset_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(250));

    esp_err_t err = cfg_json_factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(err));
    }

    err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_restore failed during factory reset: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void ota_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, WEB_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    const cJSON *cfg = cfg_json_get();
    cJSON *dup = cJSON_Duplicate((cJSON *)cfg, 1);
    if (!dup) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    esp_err_t r = json_send(req, dup, 200);
    cJSON_Delete(dup);
    return r;
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    cJSON *root = NULL;
    esp_err_t err = read_body_json(req, &root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    err = cfg_json_set_and_save(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, cfg_json_last_error());
    }
    system_log_write("web", "info", "Configuration saved");

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    cJSON_AddStringToObject(ok, "note", "saved");
    esp_err_t r = json_send(req, ok, 200);
    cJSON_Delete(ok);
    return r;
}

static esp_err_t handle_apply(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    esp_err_t err = modules_apply_config(cfg_json_get());
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "apply failed");
    }
    err = schedule_apply_from_current_cfg();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "apply task failed");
    }
    system_log_write("web", "info", "Runtime apply scheduled");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "note", "apply scheduled");
    esp_err_t r = json_send(req, resp, 200);
    cJSON_Delete(resp);
    return r;
}

static esp_err_t handle_factory_reset(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    cJSON *root = NULL;
    esp_err_t err = read_body_json(req, &root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    bool accepted = cJSON_IsString(confirm) && confirm->valuestring &&
                    strcmp(confirm->valuestring, "ERASE") == 0;
    cJSON_Delete(root);
    if (!accepted) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "confirmation required");
    }

    if (xTaskCreate(factory_reset_task, "factory_reset", 4096, NULL, 4, NULL) != pdPASS) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reset task failed");
    }
    system_log_write("web", "warn", "Factory reset scheduled");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "note", "factory reset scheduled");
    esp_err_t r = json_send(req, resp, 200);
    cJSON_Delete(resp);
    return r;
}

static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty firmware body");
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
    }

    if ((size_t)req->content_len > update_partition->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware too large");
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, (size_t)req->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
    }

    char *buf = malloc(OTA_RECV_CHUNK);
    if (!buf) {
        (void)esp_ota_abort(ota_handle);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    int remaining = req->content_len;

    while (remaining > 0) {
        const int want = remaining > (int)OTA_RECV_CHUNK ? (int)OTA_RECV_CHUNK : remaining;
        int received = httpd_req_recv(req, buf, want);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            free(buf);
            (void)esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "ota recv failed after %d bytes", req->content_len - remaining);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota receive failed");
        }

        err = esp_ota_write(ota_handle, buf, (size_t)received);
        if (err != ESP_OK) {
            free(buf);
            (void)esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
        }

        remaining -= received;
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        (void)esp_ota_abort(ota_handle);
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid firmware image");
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota activate failed");
    }
    system_log_writef("web", "info", "OTA uploaded (%d bytes)", req->content_len);

    if (xTaskCreate(ota_restart_task, "ota_restart", 3072, NULL, 4, NULL) != pdPASS) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "restart task failed");
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "note", "firmware uploaded, reboot scheduled");
    err = json_send(req, resp, 200);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_get_modules(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    cJSON *st = modules_build_status_json();
    if (!st) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    esp_err_t r = json_send(req, st, 200);
    cJSON_Delete(st);
    return r;
}

static esp_err_t handle_get_runtime(httpd_req_t *req)
{
    cJSON *rt = cJSON_CreateObject();
    if (!rt) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();

    cJSON_AddStringToObject(rt, "mode", wifi_mgr_is_ap() ? "ap" : "sta");
    cJSON_AddStringToObject(rt, "ap_ssid", wifi_mgr_is_ap() ? wifi_mgr_get_ap_ssid() : "");
    cJSON_AddBoolToObject(rt, "mqtt_connected", mqtt_mgr_is_connected());
    cJSON_AddBoolToObject(rt, "sta_configured", wifi_mgr_sta_configured());
    cJSON_AddBoolToObject(rt, "sta_has_ip", wifi_mgr_sta_has_ip());
    cJSON_AddBoolToObject(rt, "auth_required", is_auth_enabled());
    cJSON_AddStringToObject(rt, "fw_build_date", app_desc ? app_desc->date : "");
    cJSON_AddStringToObject(rt, "fw_build_time", app_desc ? app_desc->time : "");

    esp_err_t r = json_send(req, rt, 200);
    cJSON_Delete(rt);
    return r;
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    cJSON *networks = NULL;
    bool cached = false;

    // When the UI is opened via the device AP, prefer the pre-scan cache.
    // Trying to perform a live scan first may disrupt the very request that
    // asked for the network list.
    if (wifi_mgr_is_ap()) {
        esp_err_t cache_err = wifi_mgr_get_cached_scan_networks(&networks);
        if (cache_err == ESP_OK) {
            cached = true;
        }
    }

    if (!networks) {
        esp_err_t err = wifi_mgr_scan_networks(&networks);
        if (err != ESP_OK) {
            err = wifi_mgr_get_cached_scan_networks(&networks);
            if (err != ESP_OK) {
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
            }
            cached = true;
        }
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "cached", cached);
    cJSON_AddItemToObject(resp, "networks", networks);
    esp_err_t r = json_send(req, resp, 200);
    cJSON_Delete(resp);
    return r;
}

static esp_err_t handle_get_backup(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    cJSON *dup = cJSON_Duplicate((cJSON *)cfg_json_get(), 1);
    char *payload;
    if (!dup) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    payload = cJSON_PrintUnformatted(dup);
    cJSON_Delete(dup);
    if (!payload) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"esp32-config-backup.json\"");
    esp_err_t err = httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    free(payload);
    return err;
}

static esp_err_t handle_post_restore(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    cJSON *root = NULL;
    esp_err_t err = read_body_json(req, &root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    err = cfg_json_set_and_save(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, cfg_json_last_error());
    }

    err = modules_apply_config(cfg_json_get());
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "apply failed");
    }
    err = schedule_apply_from_current_cfg();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "apply task failed");
    }
    system_log_write("web", "info", "Configuration restored from backup");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "note", "backup restored and apply scheduled");
    err = json_send(req, resp, 200);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_get_system(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    cJSON *root = cJSON_CreateObject();
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000LL));
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_text(esp_reset_reason()));
    cJSON_AddBoolToObject(root, "mqtt_connected", mqtt_mgr_is_connected());
    cJSON_AddBoolToObject(root, "sta_has_ip", wifi_mgr_sta_has_ip());
    cJSON_AddBoolToObject(root, "sta_configured", wifi_mgr_sta_configured());
    cJSON_AddBoolToObject(root, "auth_enabled", is_auth_enabled());
    cJSON_AddStringToObject(root, "mode", wifi_mgr_is_ap() ? "ap" : "sta");
    cJSON_AddStringToObject(root, "ap_ssid", wifi_mgr_get_ap_ssid());
    cJSON_AddNumberToObject(root, "sta_rssi", wifi_mgr_get_sta_rssi());
    cJSON_AddStringToObject(root, "fw_build_date", app_desc ? app_desc->date : "");
    cJSON_AddStringToObject(root, "fw_build_time", app_desc ? app_desc->time : "");
    esp_err_t err = json_send(req, root, 200);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_get_events(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *events = system_log_build_json(32);
    if (!root || !events) {
        cJSON_Delete(root);
        cJSON_Delete(events);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddItemToObject(root, "events", events);
    esp_err_t err = json_send(req, root, 200);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_module_action(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    static const char *prefix = "/api/modules/";
    const char *uri = req->uri;
    size_t prefix_len = strlen(prefix);
    if (strncmp(uri, prefix, prefix_len) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri");
    }

    const char *p = uri + prefix_len;
    const char *slash = strchr(p, '/');
    if (!slash) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri");
    }

    if (strcmp(slash, "/action") != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown action path");
    }

    char id[32] = {0};
    size_t n = (size_t)(slash - p);
    if (n == 0 || n >= sizeof(id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
    }
    memcpy(id, p, n);
    id[n] = 0;

    cJSON *action = NULL;
    esp_err_t err = read_body_json(req, &action);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    cJSON *resp = NULL;
    err = modules_action(id, action, &resp);
    cJSON_Delete(action);

    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown module");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "module disabled");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad action");
    }

    esp_err_t r = json_send(req, resp, 200);
    cJSON_Delete(resp);
    return r;
}

esp_err_t web_server_start(void)
{
#if APP_CAPTIVE_PORTAL_ENABLE
    if (wifi_mgr_is_ap()) {
        dns_server_start();
    }
#endif

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.uri_match_fn = httpd_uri_match_wildcard;
    conf.max_uri_handlers = 20;

    esp_err_t err = httpd_start(&s_server, &conf);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handle_root};
    httpd_uri_t get_cfg = {.uri = "/api/config", .method = HTTP_GET, .handler = handle_get_config};
    httpd_uri_t post_cfg = {.uri = "/api/config", .method = HTTP_POST, .handler = handle_post_config};
    httpd_uri_t apply = {.uri = "/api/apply", .method = HTTP_POST, .handler = handle_apply};
    httpd_uri_t factory_reset = {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = handle_factory_reset};
    httpd_uri_t ota = {.uri = "/api/ota", .method = HTTP_POST, .handler = handle_ota_upload};
    httpd_uri_t mods = {.uri = "/api/modules", .method = HTTP_GET, .handler = handle_get_modules};
    httpd_uri_t runtime = {.uri = "/api/runtime", .method = HTTP_GET, .handler = handle_get_runtime};
    httpd_uri_t wifi_scan = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan};
    httpd_uri_t backup = {.uri = "/api/backup", .method = HTTP_GET, .handler = handle_get_backup};
    httpd_uri_t restore = {.uri = "/api/restore", .method = HTTP_POST, .handler = handle_post_restore};
    httpd_uri_t system = {.uri = "/api/system", .method = HTTP_GET, .handler = handle_get_system};
    httpd_uri_t events = {.uri = "/api/events", .method = HTTP_GET, .handler = handle_get_events};
    httpd_uri_t act = {.uri = "/api/modules/*", .method = HTTP_POST, .handler = handle_module_action};
    httpd_uri_t u204 = {.uri = "/generate_204", .method = HTTP_GET, .handler = handle_generate_204};
    httpd_uri_t uios = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_to_root};
    httpd_uri_t uct = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect_to_root};
    httpd_uri_t uncsi = {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect_to_root};
    httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = captive_redirect_to_root};

    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &get_cfg);
    httpd_register_uri_handler(s_server, &post_cfg);
    httpd_register_uri_handler(s_server, &apply);
    httpd_register_uri_handler(s_server, &factory_reset);
    httpd_register_uri_handler(s_server, &ota);
    httpd_register_uri_handler(s_server, &mods);
    httpd_register_uri_handler(s_server, &runtime);
    httpd_register_uri_handler(s_server, &wifi_scan);
    httpd_register_uri_handler(s_server, &backup);
    httpd_register_uri_handler(s_server, &restore);
    httpd_register_uri_handler(s_server, &system);
    httpd_register_uri_handler(s_server, &events);
    httpd_register_uri_handler(s_server, &act);
    httpd_register_uri_handler(s_server, &u204);
    httpd_register_uri_handler(s_server, &uios);
    httpd_register_uri_handler(s_server, &uct);
    httpd_register_uri_handler(s_server, &uncsi);
    httpd_register_uri_handler(s_server, &any);

    return ESP_OK;
}
