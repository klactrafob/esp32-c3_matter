#include "web_server.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"

#include "cfg.h"
#include "wifi_mgr.h"
#include "dns_server.h"
#include "app_config.h"

static const char *TAG = "web";
static cfg_t *s_cfg = NULL;
static httpd_handle_t s_server = NULL;

static const char *INDEX_HTML =
"<!doctype html><html><head><meta charset='utf-8'/>"
"<meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>ESP32 Setup</title></head><body>"
"<h3>Wi-Fi настройка</h3>"
"<form method='POST' action='/api/wifi'>"
"SSID:<br><input name='ssid' maxlength='32'><br>"
"PASS:<br><input name='pass' maxlength='64' type='password'><br><br>"
"<button type='submit'>Сохранить</button>"
"</form>"
"<p><a href='/api/status'>status</a></p>"
"</body></html>";

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

// Декодирует application/x-www-form-urlencoded: + и %xx
static void url_decode_inplace(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '+') {
            *w++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            int hi = hexval(s[1]);
            int lo = hexval(s[2]);
            *w++ = (char)((hi << 4) | lo);
            s += 3;
        } else {
            *w++ = *s++;
        }
    }
    *w = 0;
}

// Captive portal: для любых неизвестных URI — редирект на "/"
static esp_err_t handle_captive_redirect(httpd_req_t *req)
{
#if APP_CAPTIVE_PORTAL_ENABLE
    if (wifi_mgr_is_ap()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
#endif
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{"
             "\"hostname\":\"%s\","
             "\"sta_ssid\":\"%s\","
             "\"ap_ssid\":\"%s\","
             "\"mode\":\"%s\""
             "}",
             s_cfg->hostname,
             s_cfg->sta_ssid,
             s_cfg->ap_ssid,
             wifi_mgr_is_ap() ? "AP" : "STA");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "hostname", s_cfg->hostname);
    cJSON_AddStringToObject(root, "sta_ssid", s_cfg->sta_ssid);
    cJSON_AddStringToObject(root, "ap_ssid", s_cfg->ap_ssid);
    cJSON_AddStringToObject(root, "mode", wifi_mgr_is_ap() ? "AP" : "STA");

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);

    free(out);
    return ESP_OK;
}

// POST /api/config принимает JSON: { "hostname":"...", "sta_ssid":"...", "sta_pass":"..." }
static esp_err_t handle_post_config(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }

    char *body = (char*)calloc(1, total + 1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { free(body); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); }
        got += r;
    }
    body[total] = 0;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json parse");

    const cJSON *hostname = cJSON_GetObjectItemCaseSensitive(root, "hostname");
    const cJSON *sta_ssid = cJSON_GetObjectItemCaseSensitive(root, "sta_ssid");
    const cJSON *sta_pass = cJSON_GetObjectItemCaseSensitive(root, "sta_pass");

    if (cJSON_IsString(hostname) && hostname->valuestring) {
        memset(s_cfg->hostname, 0, sizeof(s_cfg->hostname));
        strncpy(s_cfg->hostname, hostname->valuestring, sizeof(s_cfg->hostname)-1);
    }
    if (cJSON_IsString(sta_ssid) && sta_ssid->valuestring) {
        memset(s_cfg->sta_ssid, 0, sizeof(s_cfg->sta_ssid));
        strncpy(s_cfg->sta_ssid, sta_ssid->valuestring, sizeof(s_cfg->sta_ssid)-1);
    }
    if (cJSON_IsString(sta_pass) && sta_pass->valuestring) {
        memset(s_cfg->sta_pass, 0, sizeof(s_cfg->sta_pass));
        strncpy(s_cfg->sta_pass, sta_pass->valuestring, sizeof(s_cfg->sta_pass)-1);
    }

    cJSON_Delete(root);

    esp_err_t err = cfg_save(s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cfg_save failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    esp_restart();
    return ESP_OK;
}

// form POST: ssid=...&pass=...
static esp_err_t handle_wifi_post(httpd_req_t *req)
{
    char body[512] = {0};
    int len = httpd_req_recv(req, body, sizeof(body)-1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    body[len] = 0;

    char *ssid = strstr(body, "ssid=");
    char *pass = strstr(body, "pass=");
    if (!ssid || !pass) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");

    ssid += 5;
    char *amp = strchr(ssid, '&');
    if (amp) *amp = 0;

    pass += 5;

    url_decode_inplace(ssid);
    url_decode_inplace(pass);

    memset(s_cfg->sta_ssid, 0, sizeof(s_cfg->sta_ssid));
    memset(s_cfg->sta_pass, 0, sizeof(s_cfg->sta_pass));
    strncpy(s_cfg->sta_ssid, ssid, sizeof(s_cfg->sta_ssid)-1);
    strncpy(s_cfg->sta_pass, pass, sizeof(s_cfg->sta_pass)-1);

    esp_err_t err = cfg_save(s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cfg_save failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "OK, rebooting...");
    esp_restart();
    return ESP_OK;
}

// POST /api/factory_reset
static esp_err_t handle_factory_reset(httpd_req_t *req)
{
    (void)req;
    cfg_factory_reset();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    esp_restart();
    return ESP_OK;
}

esp_err_t web_server_start(cfg_t *cfg)
{
    s_cfg = cfg;

#if APP_CAPTIVE_PORTAL_ENABLE
    if (wifi_mgr_is_ap()) {
        dns_server_start();
    }
#endif

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting httpd on port %d", conf.server_port);
    esp_err_t err = httpd_start(&s_server, &conf);
    if (err != ESP_OK) return err;

    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=handle_root};
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t status = {.uri="/api/status", .method=HTTP_GET, .handler=handle_status};
    httpd_register_uri_handler(s_server, &status);

    httpd_uri_t wifi_post = {.uri="/api/wifi", .method=HTTP_POST, .handler=handle_wifi_post};
    httpd_register_uri_handler(s_server, &wifi_post);

    httpd_uri_t get_cfg = {.uri="/api/config", .method=HTTP_GET, .handler=handle_get_config};
    httpd_register_uri_handler(s_server, &get_cfg);

    httpd_uri_t post_cfg = {.uri="/api/config", .method=HTTP_POST, .handler=handle_post_config};
    httpd_register_uri_handler(s_server, &post_cfg);

    httpd_uri_t fr = {.uri="/api/factory_reset", .method=HTTP_POST, .handler=handle_factory_reset};
    httpd_register_uri_handler(s_server, &fr);

    // Captive redirect: любой другой GET -> на "/"
    httpd_uri_t any = {.uri="/*", .method=HTTP_GET, .handler=handle_captive_redirect};
    httpd_register_uri_handler(s_server, &any);

    return ESP_OK;
}
