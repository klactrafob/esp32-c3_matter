#include "web_server.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"

#include "cfg_json.h"
#include "modules.h"
#include "wifi_mgr.h"
#include "dns_server.h"
#include "app_config.h"

static const char *TAG = "web";
static httpd_handle_t s_server = NULL;

static const char *INDEX_HTML =
"<!doctype html><html><head><meta charset='utf-8'/>"
"<meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>ESP32 Setup</title></head><body>"
"<h3>ESP32-C3 Setup</h3>"
"<p>API:</p>"
"<ul>"
"<li><a href='/api/config'>/api/config</a></li>"
"<li><a href='/api/modules'>/api/modules</a></li>"
"</ul>"
"</body></html>";

static bool captive_active(void)
{
#if APP_CAPTIVE_PORTAL_ENABLE
    return wifi_mgr_is_ap();
#else
    return false;
#endif
}

static esp_err_t json_send(httpd_req_t *req, cJSON *obj, int status_code)
{
    char *out = cJSON_PrintUnformatted(obj);
    if (!out) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");

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
    if (total <= 0 || total > 8192) return ESP_ERR_INVALID_SIZE;

    char *buf = (char*)calloc(1, total + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) { free(buf); return ESP_FAIL; }
        got += r;
    }
    buf[total] = 0;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_ERR_INVALID_ARG;

    *out_json = root;
    return ESP_OK;
}

// Captive redirect to root
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

// Android probe
static esp_err_t handle_generate_204(httpd_req_t *req)
{
    if (captive_active()) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    const cJSON *cfg = cfg_json_get();
    cJSON *dup = cJSON_Duplicate((cJSON*)cfg, 1);
    if (!dup) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    esp_err_t r = json_send(req, dup, 200);
    cJSON_Delete(dup);
    return r;
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    cJSON *root = NULL;
    esp_err_t err = read_body_json(req, &root);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");

    err = cfg_json_set_and_save(root);
    cJSON_Delete(root);

    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    cJSON_AddStringToObject(ok, "note", "saved (call /api/apply to apply now)");
    esp_err_t r = json_send(req, ok, 200);
    cJSON_Delete(ok);
    return r;
}

static esp_err_t handle_apply(httpd_req_t *req)
{
    (void)req;
    esp_err_t err = modules_apply_config(cfg_json_get());
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "apply failed");

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    cJSON_AddStringToObject(ok, "note", "modules applied");
    esp_err_t r = json_send(req, ok, 200);
    cJSON_Delete(ok);
    return r;
}

static esp_err_t handle_get_modules(httpd_req_t *req)
{
    cJSON *st = modules_build_status_json();
    if (!st) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    esp_err_t r = json_send(req, st, 200);
    cJSON_Delete(st);
    return r;
}

// POST /api/modules/<name>/action  with JSON body
static esp_err_t handle_module_action(httpd_req_t *req)
{
    // uri example: /api/modules/relay/action
    const char *uri = req->uri;
    const char *p = strstr(uri, "/api/modules/");
    if (!p) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri");
    p += strlen("/api/modules/");
    const char *slash = strchr(p, '/');
    if (!slash) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri");

    char name[32] = {0};
    size_t n = (size_t)(slash - p);
    if (n == 0 || n >= sizeof(name)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    memcpy(name, p, n);
    name[n] = 0;

    cJSON *action = NULL;
    esp_err_t err = read_body_json(req, &action);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");

    cJSON *resp = NULL;
    err = modules_action(name, action, &resp);
    cJSON_Delete(action);

    if (err == ESP_ERR_NOT_FOUND) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown module");
    if (err == ESP_ERR_INVALID_STATE) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "module disabled");
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad action");

    esp_err_t r = json_send(req, resp, 200);
    cJSON_Delete(resp);
    return r;
}

esp_err_t web_server_start(void)
{
#if APP_CAPTIVE_PORTAL_ENABLE
    if (wifi_mgr_is_ap()) dns_server_start();
#endif

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting httpd on port %d", conf.server_port);
    esp_err_t err = httpd_start(&s_server, &conf);
    if (err != ESP_OK) return err;

    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=handle_root};
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t get_cfg = {.uri="/api/config", .method=HTTP_GET, .handler=handle_get_config};
    httpd_register_uri_handler(s_server, &get_cfg);

    httpd_uri_t post_cfg = {.uri="/api/config", .method=HTTP_POST, .handler=handle_post_config};
    httpd_register_uri_handler(s_server, &post_cfg);

    httpd_uri_t apply = {.uri="/api/apply", .method=HTTP_POST, .handler=handle_apply};
    httpd_register_uri_handler(s_server, &apply);

    httpd_uri_t mods = {.uri="/api/modules", .method=HTTP_GET, .handler=handle_get_modules};
    httpd_register_uri_handler(s_server, &mods);

    httpd_uri_t act = {.uri="/api/modules/*/action", .method=HTTP_POST, .handler=handle_module_action};
    httpd_register_uri_handler(s_server, &act);

    // Captive portal OS probes + wildcard redirect
    httpd_uri_t u204 = {.uri="/generate_204", .method=HTTP_GET, .handler=handle_generate_204};
    httpd_register_uri_handler(s_server, &u204);

    httpd_uri_t uios = {.uri="/hotspot-detect.html", .method=HTTP_GET, .handler=captive_redirect_to_root};
    httpd_register_uri_handler(s_server, &uios);

    httpd_uri_t uct = {.uri="/connecttest.txt", .method=HTTP_GET, .handler=captive_redirect_to_root};
    httpd_register_uri_handler(s_server, &uct);

    httpd_uri_t uncsi = {.uri="/ncsi.txt", .method=HTTP_GET, .handler=captive_redirect_to_root};
    httpd_register_uri_handler(s_server, &uncsi);

    httpd_uri_t any = {.uri="/*", .method=HTTP_GET, .handler=captive_redirect_to_root};
    httpd_register_uri_handler(s_server, &any);

    return ESP_OK;
}
