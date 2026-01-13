#include "web_server.h"
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cfg.h"

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
             "\"ap_ssid\":\"%s\""
             "}",
             s_cfg->hostname,
             s_cfg->sta_ssid,
             s_cfg->ap_ssid);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static void url_decode_inplace(char *s)
{
    for (; *s; s++) if (*s == '+') *s = ' ';
    // %xx декодирование добавим позже, пока MVP
}

static esp_err_t handle_wifi_post(httpd_req_t *req)
{
    char body[256] = {0};
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

esp_err_t web_server_start(cfg_t *cfg)
{
    s_cfg = cfg;

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

    return ESP_OK;
}
