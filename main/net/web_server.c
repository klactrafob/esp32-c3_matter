#include "net/web_server.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"

#include "core/cfg_json.h"
#include "core/modules.h"
#include "net/wifi_mgr.h"
#include "net/dns_server.h"
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
"<li><a href='/api/modules'>/api/modules</a></li><li><a href='/ws2812'>WS2812 settings</a></li>"
"</ul>"
"</body></html>";

static const char *WS2812_HTML =
"<!doctype html><html><head><meta charset='utf-8'/>"
"<meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>WS2812 Settings</title>"
"<style>"
"body{font-family:system-ui,Arial,sans-serif;margin:16px;max-width:720px}"
"label{display:block;margin-top:10px;font-weight:600}"
"input,select{width:100%;padding:8px;margin-top:4px}"
".row{display:flex;gap:12px}"
".row>div{flex:1}"
"button{padding:10px 14px;margin-top:14px}"
"pre{background:#f4f4f4;padding:10px;overflow:auto}"
"</style></head><body>"
"<h3>WS2812 (LED Strip) settings</h3>"
"<p>Изменения сохраняются в конфиг, затем применяется /api/apply.</p>"

"<div class='row'>"
" <div><label>Enable</label><select id='en'><option value='false'>false</option><option value='true'>true</option></select></div>"
" <div><label>GPIO</label><input id='gpio' type='number' min='0' max='48'/></div>"
"</div>"

"<div class='row'>"
" <div><label>LED count</label><input id='count' type='number' min='1' max='1024'/></div>"
" <div><label>Color order</label>"
"  <select id='order'>"
"   <option>RGB</option><option>RBG</option><option>GRB</option><option>GBR</option><option>BRG</option><option>BGR</option>"
"  </select>"
" </div>"
"</div>"

"<label>Brightness limit (%)</label><input id='blim' type='number' min='1' max='100'/>"
"<label>Transition time (ms)</label><input id='tr' type='number' min='0' max='5000'/>"
"<label>Frame period (ms)</label><input id='frame' type='number' min='10' max='100'/>"

"<div class='row'>"
" <div><label>Power ON effect</label><select id='pon'><option>none</option><option>fade</option><option>wipe</option></select></div>"
" <div><label>Power OFF effect</label><select id='poff'><option>none</option><option>fade</option><option>wipe</option></select></div>"
"</div>"
"<label>Effect duration (ms)</label><input id='edur' type='number' min='0' max='5000'/>"

"<hr/>"
"<h4>Quick action</h4>"
"<div class='row'>"
" <div><label>On</label><select id='act_on'><option value='false'>false</option><option value='true'>true</option></select></div>"
" <div><label>Brightness (0..100)</label><input id='act_br' type='number' min='0' max='100'/></div>"
"</div>"
"<div class='row'>"
" <div><label>R</label><input id='act_r' type='number' min='0' max='255'/></div>"
" <div><label>G</label><input id='act_g' type='number' min='0' max='255'/></div>"
" <div><label>B</label><input id='act_b' type='number' min='0' max='255'/></div>"
"</div>"

"<button onclick='saveCfg()'>Save config</button> "
"<button onclick='applyNow()'>Apply config</button> "
"<button onclick='sendAction()'>Send action</button>"

"<p id='msg'></p>"
"<pre id='dump'></pre>"

"<script>"
"const $=id=>document.getElementById(id);"
"function msg(t){$('msg').textContent=t;}"
"async function load(){"
"  const cfg=await (await fetch('/api/config')).json();"
"  const ws=((cfg.modules||{}).ws2812)||{};"
"  $('en').value=String(!!ws.enable);"
"  $('gpio').value=(ws.gpio===undefined?8:ws.gpio);"
"  $('count').value=(ws.count===undefined?30:ws.count);"
"  $('order').value=(ws.color_order||'GRB');"
"  $('blim').value=(ws.brightness_limit===undefined?100:ws.brightness_limit);"
"  $('tr').value=(ws.transition_ms===undefined?300:ws.transition_ms);"
"  $('frame').value=(ws.frame_ms===undefined?20:ws.frame_ms);"
"  $('pon').value=(ws.power_on_effect||'fade');"
"  $('poff').value=(ws.power_off_effect||'fade');"
"  $('edur').value=(ws.effect_duration_ms===undefined?400:ws.effect_duration_ms);"
"  $('dump').textContent=JSON.stringify(ws,null,2);"
"}"
"function clamp(v,min,max){v=Number(v);if(isNaN(v))return min;return v<min?min:(v>max?max:v);}"
"async function saveCfg(){"
"  const cfg=await (await fetch('/api/config')).json();"
"  cfg.modules=cfg.modules||{}; cfg.modules.ws2812=cfg.modules.ws2812||{};"
"  const ws=cfg.modules.ws2812;"
"  ws.enable=$('en').value==='true';"
"  ws.gpio=parseInt($('gpio').value||'8',10);"
"  ws.count=clamp(parseInt($('count').value||'30',10),1,1024);"
"  ws.color_order=$('order').value;"
"  ws.brightness_limit=clamp(parseInt($('blim').value||'100',10),1,100);"
"  ws.transition_ms=clamp(parseInt($('tr').value||'300',10),0,5000);"
"  ws.frame_ms=clamp(parseInt($('frame').value||'20',10),10,100);"
"  ws.power_on_effect=$('pon').value;"
"  ws.power_off_effect=$('poff').value;"
"  ws.effect_duration_ms=clamp(parseInt($('edur').value||'400',10),0,5000);"
"  const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});"
"  if(!r.ok){msg('Save failed: '+r.status);return;}"
"  const a=await fetch('/api/apply',{method:'POST'});"
"  msg('Saved+applied: cfg='+r.status+' apply='+a.status);"
"  load();"
"}"
"async function applyNow(){"
"  const r=await fetch('/api/apply',{method:'POST'});"
"  msg('Apply: '+r.status);"
"}"
"async function sendAction(){"
"  const a={"
"    on:$('act_on').value==='true',"
"    brightness:parseInt($('act_br').value||'50',10),"
"    r:parseInt($('act_r').value||'255',10),"
"    g:parseInt($('act_g').value||'255',10),"
"    b:parseInt($('act_b').value||'255',10)"
"  };"
"  const r=await fetch('/api/modules/ws2812/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(a)});"
"  const j=await r.json().catch(()=>({}));"
"  msg('Action: '+r.status);"
"  $('dump').textContent=JSON.stringify(j,null,2);"
"}"
"load();"
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

static esp_err_t handle_ws2812(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, WS2812_HTML, HTTPD_RESP_USE_STRLEN);
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

    httpd_uri_t ws = {.uri="/ws2812", .method=HTTP_GET, .handler=handle_ws2812};
    httpd_register_uri_handler(s_server, &ws);

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
