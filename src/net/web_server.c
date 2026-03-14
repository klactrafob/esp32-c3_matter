#include "net/web_server.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "core/cfg_json.h"
#include "core/modules.h"
#include "device_state.h"
#include "net/dns_server.h"
#include "net/mqtt_mgr.h"
#include "net/wifi_mgr.h"

static const char *TAG = "web";
static httpd_handle_t s_server = NULL;

typedef struct {
    cJSON *cfg_dup;
} apply_ctx_t;

static const char *INDEX_HTML =
"<!doctype html><html><head><meta charset='utf-8'/>"
"<meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>ESP32-C3 Relay Setup</title>"
"<style>"
"body{font-family:Arial,sans-serif;margin:16px;max-width:760px}"
"h2{margin:0 0 12px}"
".row{display:flex;gap:10px;flex-wrap:wrap}"
".col{flex:1 1 220px}"
"label{display:block;margin:10px 0 4px;font-weight:600}"
"input,select,button{font:inherit}"
"input,select{width:100%;padding:8px}"
"button{padding:9px 12px;margin-top:12px}"
".muted{color:#666;font-size:13px}"
".ok{color:#0a7c2f}"
".err{color:#b00020}"
".cards{display:flex;gap:10px;flex-wrap:wrap;margin:12px 0}"
".card{padding:8px 10px;border:1px solid #ddd;border-radius:8px;text-decoration:none;color:#111}"
"</style></head><body>"
"<h2>ESP32-C3 Relay Setup</h2>"
"<p class='muted' id='runtime'>Loading runtime...</p>"
"<div class='cards'>"
" <a class='card' href='/api/config'>/api/config</a>"
" <a class='card' href='/api/modules'>/api/modules</a>"
"</div>"
"<div class='row'>"
" <div class='col'><label for='host'>Hostname</label><input id='host'/></div>"
"</div>"
"<div class='row'>"
" <div class='col'><label for='ap_ssid'>AP SSID</label><input id='ap_ssid'/></div>"
" <div class='col'><label for='ap_pass'>AP Password</label><input id='ap_pass'/></div>"
"</div>"
"<div class='row'>"
" <div class='col'><label for='sta_list'>Wi-Fi networks</label><select id='sta_list'></select></div>"
"</div>"
"<div class='row'>"
" <div class='col'><label for='sta_ssid'>STA SSID</label><input id='sta_ssid'/></div>"
" <div class='col'><label for='sta_pass'>STA Password</label><input id='sta_pass'/></div>"
"</div>"
"<div class='row'>"
" <div class='col'><button onclick='scanWifi()'>Scan Wi-Fi</button></div>"
" <div class='col'><button onclick='saveAndApply()'>Save + Apply</button></div>"
" <div class='col'><button onclick='applyOnly()'>Apply only</button></div>"
"</div>"
"<p id='msg' class='muted'></p>"
"<script>"
"const $=id=>document.getElementById(id);"
"function setMsg(text,ok){const m=$('msg');m.textContent=text;m.className=ok?'ok':'err';}"
"function setRuntime(rt){"
" const mode=rt.mode||'unknown';"
" const ap=rt.ap_ssid||'';"
" const mqtt=(rt.mqtt_connected===true)?'connected':'disconnected';"
" $('runtime').textContent='Mode: '+mode+'; AP: '+ap+'; MQTT: '+mqtt;"
"}"
"function fillWifiList(list){"
" const s=$('sta_list');"
" s.innerHTML='';"
" const e=document.createElement('option');e.value='';e.textContent='Select network...';s.appendChild(e);"
" (Array.isArray(list)?list:[]).forEach(n=>{"
"   if(!n||!n.ssid) return;"
"   const o=document.createElement('option');"
"   o.value=String(n.ssid);"
"   o.textContent=String(n.ssid)+' '+((n.rssi===undefined)?'':n.rssi)+'dBm '+(n.auth||'');"
"   s.appendChild(o);"
" });"
" s.onchange=()=>{if(s.value)$('sta_ssid').value=s.value;};"
"}"
"async function loadCfg(){"
" const cfg=await (await fetch('/api/config')).json();"
" const rt=await (await fetch('/api/runtime')).json();"
" setRuntime(rt);"
" const net=cfg.net||{};const ap=net.ap||{};const sta=net.sta||{};"
" $('host').value=net.hostname||'esp32-c3';"
" $('ap_ssid').value=ap.ssid||'ESP32-SETUP';"
" $('ap_pass').value=(ap.pass===undefined?'':ap.pass);"
" $('sta_ssid').value=sta.ssid||'';"
" $('sta_pass').value=sta.pass||'';"
"}"
"async function scanWifi(){"
" try{"
"  setMsg('Scanning...',true);"
"  const r=await fetch('/api/wifi/scan');"
"  if(!r.ok){setMsg('Scan failed: '+r.status,false);return;}"
"  const j=await r.json();"
"  fillWifiList(j.networks||[]);"
"  setMsg('Found: '+(j.networks||[]).length,true);"
" }catch(e){setMsg(String(e),false);}"
"}"
"async function saveAndApply(){"
" try{"
"  const cfg=await (await fetch('/api/config')).json();"
"  cfg.net=cfg.net||{}; cfg.net.ap=cfg.net.ap||{}; cfg.net.sta=cfg.net.sta||{};"
"  cfg.net.hostname=($('host').value||'esp32-c3').trim();"
"  cfg.net.ap.ssid=($('ap_ssid').value||'ESP32-SETUP').trim();"
"  cfg.net.ap.pass=($('ap_pass').value||'').trim();"
"  cfg.net.sta.ssid=($('sta_ssid').value||'').trim();"
"  cfg.net.sta.pass=($('sta_pass').value||'').trim();"
"  const s=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});"
"  if(!s.ok){setMsg('Save failed: '+s.status,false);return;}"
"  const a=await fetch('/api/apply',{method:'POST'});"
"  if(!a.ok){setMsg('Apply failed: '+a.status,false);return;}"
"  setMsg('Saved and apply scheduled',true);"
"  setTimeout(loadCfg,700);"
" }catch(e){setMsg(String(e),false);}"
"}"
"async function applyOnly(){"
" try{"
"  const a=await fetch('/api/apply',{method:'POST'});"
"  if(!a.ok){setMsg('Apply failed: '+a.status,false);return;}"
"  setMsg('Apply scheduled',true);"
"  setTimeout(loadCfg,700);"
" }catch(e){setMsg(String(e),false);}"
"}"
"loadCfg();"
"</script></body></html>";

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
    if (total <= 0 || total > 8192) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = (char *)calloc(1, (size_t)total + 1);
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
    } else {
        ESP_LOGI(TAG, "wifi reconfigure applied");
    }

    esp_err_t merr = mqtt_mgr_restart_from_cfg(task_ctx->cfg_dup);
    if (merr != ESP_OK) {
        ESP_LOGE(TAG, "mqtt reconfigure failed: %s", esp_err_to_name(merr));
    } else {
        ESP_LOGI(TAG, "mqtt reconfigure applied");
    }

    cJSON_Delete(task_ctx->cfg_dup);
    free(task_ctx);
    vTaskDelete(NULL);
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
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
    cJSON *root = NULL;
    esp_err_t err = read_body_json(req, &root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    err = cfg_json_set_and_save(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    cJSON_AddStringToObject(ok, "note", "saved (call /api/apply to apply now)");
    esp_err_t r = json_send(req, ok, 200);
    cJSON_Delete(ok);
    return r;
}

static esp_err_t handle_apply(httpd_req_t *req)
{
    esp_err_t err = modules_apply_config(cfg_json_get());
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "apply failed");
    }

    apply_ctx_t *ctx = (apply_ctx_t *)calloc(1, sizeof(apply_ctx_t));
    if (!ctx) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    ctx->cfg_dup = cJSON_Duplicate((cJSON *)cfg_json_get(), 1);
    if (!ctx->cfg_dup) {
        free(ctx);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    BaseType_t ok = xTaskCreate(apply_cfg_task, "apply_cfg", 4096, ctx, 4, NULL);
    if (ok != pdPASS) {
        cJSON_Delete(ctx->cfg_dup);
        free(ctx);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "apply task failed");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "note", "modules applied, wifi+mqtt reconfigure scheduled");
    esp_err_t r = json_send(req, resp, 200);
    cJSON_Delete(resp);
    return r;
}

static esp_err_t handle_get_modules(httpd_req_t *req)
{
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

    if (wifi_mgr_is_ap()) {
        cJSON_AddStringToObject(rt, "mode", "ap");
        cJSON_AddStringToObject(rt, "ap_ssid", wifi_mgr_get_ap_ssid());
    } else {
        cJSON_AddStringToObject(rt, "mode", "sta");
        cJSON_AddStringToObject(rt, "ap_ssid", "");
    }

    cJSON_AddBoolToObject(rt, "mqtt_connected", mqtt_mgr_is_connected());

    esp_err_t r = json_send(req, rt, 200);
    cJSON_Delete(rt);
    return r;
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    cJSON *networks = NULL;
    esp_err_t err = wifi_mgr_scan_networks(&networks);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        cJSON_Delete(networks);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddItemToObject(resp, "networks", networks);
    esp_err_t r = json_send(req, resp, 200);
    cJSON_Delete(resp);
    return r;
}

static esp_err_t handle_module_action(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *p = strstr(uri, "/api/modules/");
    if (!p) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri");
    }

    p += strlen("/api/modules/");
    const char *slash = strchr(p, '/');
    if (!slash) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri");
    }

    char name[32] = {0};
    size_t n = (size_t)(slash - p);
    if (n == 0 || n >= sizeof(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    memcpy(name, p, n);
    name[n] = 0;

    cJSON *action = NULL;
    esp_err_t err = read_body_json(req, &action);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    cJSON *resp = NULL;
    err = modules_action(name, action, &resp);
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

    if (strcmp(name, "relay") == 0) {
        device_state_t ds = device_state_get();
        (void)mqtt_mgr_notify_relay_state(ds.relay_on);
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
    conf.max_uri_handlers = 16;

    ESP_LOGI(TAG, "Starting httpd on port %d", conf.server_port);
    esp_err_t err = httpd_start(&s_server, &conf);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handle_root};
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t get_cfg = {.uri = "/api/config", .method = HTTP_GET, .handler = handle_get_config};
    httpd_register_uri_handler(s_server, &get_cfg);

    httpd_uri_t post_cfg = {.uri = "/api/config", .method = HTTP_POST, .handler = handle_post_config};
    httpd_register_uri_handler(s_server, &post_cfg);

    httpd_uri_t apply = {.uri = "/api/apply", .method = HTTP_POST, .handler = handle_apply};
    httpd_register_uri_handler(s_server, &apply);

    httpd_uri_t mods = {.uri = "/api/modules", .method = HTTP_GET, .handler = handle_get_modules};
    httpd_register_uri_handler(s_server, &mods);

    httpd_uri_t runtime = {.uri = "/api/runtime", .method = HTTP_GET, .handler = handle_get_runtime};
    httpd_register_uri_handler(s_server, &runtime);

    httpd_uri_t wifi_scan = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan};
    httpd_register_uri_handler(s_server, &wifi_scan);

    httpd_uri_t act = {.uri = "/api/modules/*/action", .method = HTTP_POST, .handler = handle_module_action};
    httpd_register_uri_handler(s_server, &act);

    httpd_uri_t u204 = {.uri = "/generate_204", .method = HTTP_GET, .handler = handle_generate_204};
    httpd_register_uri_handler(s_server, &u204);

    httpd_uri_t uios = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_to_root};
    httpd_register_uri_handler(s_server, &uios);

    httpd_uri_t uct = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect_to_root};
    httpd_register_uri_handler(s_server, &uct);

    httpd_uri_t uncsi = {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect_to_root};
    httpd_register_uri_handler(s_server, &uncsi);

    httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = captive_redirect_to_root};
    httpd_register_uri_handler(s_server, &any);

    return ESP_OK;
}
