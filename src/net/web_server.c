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
"<title>ESP32-C3 Setup</title>"
"<style>"
" :root{--bg:#f5f7ef;--ink:#152015;--card:#fffdf8;--muted:#5d6a5f;--line:#d9e2d0;}"
"*{box-sizing:border-box}"
"body{margin:0;color:var(--ink);font-family:'Bahnschrift','Trebuchet MS','Verdana',sans-serif;"
"background:radial-gradient(1200px 500px at -10% -20%,#d8f3dc 0%,transparent 70%),"
"radial-gradient(900px 500px at 110% 0%,#ffe8a3 0%,transparent 60%),var(--bg);}"
".wrap{max-width:900px;margin:0 auto;padding:24px 16px 36px}"
".hero{background:linear-gradient(135deg,#ffffff,#f6fbf2);border:1px solid var(--line);"
"border-radius:20px;padding:22px;box-shadow:0 10px 30px rgba(21,32,21,.08);animation:rise .45s ease-out both}"
" .top{display:flex;justify-content:space-between;align-items:center;gap:10px}"
"h1{margin:0;font-size:clamp(26px,4.2vw,38px);letter-spacing:.5px}"
".sub{margin:10px 0 0;color:var(--muted);font-size:15px}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;margin-top:14px}"
".card{display:block;text-decoration:none;background:var(--card);border:1px solid var(--line);"
"border-radius:14px;padding:14px;transition:.2s transform,.2s box-shadow,.2s border-color;color:var(--ink)}"
".card:hover{transform:translateY(-2px);box-shadow:0 8px 16px rgba(17,24,39,.08);border-color:#b9c9ae}"
".card b{display:block;font-size:16px}"
".card span{display:block;margin-top:6px;color:var(--muted);font-size:13px}"
".chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}"
".chip{padding:6px 10px;border-radius:999px;border:1px solid var(--line);background:#fff;font-size:12px;color:#2e4331}"
".chip.accent{background:linear-gradient(90deg,#1c7c54,#2a9d8f);color:#fff;border:0}"
".panel{margin-top:16px;background:#fff;border:1px solid var(--line);border-radius:14px;padding:14px}"
".panel h3{margin:0 0 8px}"
".hint{color:var(--muted);font-size:13px;margin:0 0 10px}"
".row{display:flex;gap:10px;flex-wrap:wrap}"
".col{flex:1 1 220px}"
"label{display:block;font-size:13px;font-weight:700;margin:8px 0 4px}"
"input,select,button{font:inherit}"
"input,select{width:100%;padding:9px;border:1px solid var(--line);border-radius:10px;background:#fff}"
"button{padding:9px 12px;border:1px solid #9fb89a;border-radius:10px;background:#eef7eb;cursor:pointer}"
"button:hover{background:#e4f2df}"
".btns{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}"
".ok{color:#1f7a1f;font-weight:700}"
".warn{color:#8a5a00;font-weight:700}"
".footer{margin-top:14px;color:var(--muted);font-size:13px}"
"@keyframes rise{from{opacity:0;transform:translateY(10px)}to{opacity:1;transform:none}}"
"</style></head><body>"
"<div class='wrap'>"
" <section class='hero'>"
"  <div class='top'>"
"   <h1 id='title'>ESP32-C3 Setup</h1>"
"   <select id='lang' style='width:auto'>"
"    <option value='en'>English</option>"
"    <option value='ru'>Русский</option>"
"   </select>"
"  </div>"
"  <p class='sub' id='sub'></p>"
"  <div class='chips'>"
"   <span class='chip accent' id='chip_mode'>Mode: loading...</span>"
"   <span class='chip' id='chip_ap'>AP SSID: ...</span>"
"   <span class='chip' id='chip_sta'>STA SSID: ...</span>"
"  </div>"
"  <div class='grid'>"
"   <a class='card' href='/ws2812'><b id='c_ws_t'></b><span id='c_ws_d'></span></a>"
"   <a class='card' href='/api/config'><b id='c_cfg_t'></b><span id='c_cfg_d'></span></a>"
"   <a class='card' href='/api/modules'><b id='c_mod_t'></b><span id='c_mod_d'></span></a>"
"   <a class='card' href='javascript:applyNow()'><b id='c_apply_t'></b><span id='c_apply_d'></span></a>"
"  </div>"
"  <section class='panel'>"
"   <h3 id='net_title'></h3>"
"   <p class='hint' id='net_hint'></p>"
"   <div class='row'>"
"    <div class='col'><label id='lbl_host' for='host'></label><input id='host' maxlength='63'/></div>"
"   </div>"
"   <div class='row'>"
"    <div class='col'><label id='lbl_ap_ssid' for='ap_ssid'></label><input id='ap_ssid' maxlength='32'/></div>"
"    <div class='col'><label id='lbl_ap_pass' for='ap_pass'></label><input id='ap_pass' maxlength='64'/></div>"
"   </div>"
"   <div class='row'>"
"    <div class='col'><label id='lbl_sta_ssid' for='sta_ssid'></label><input id='sta_ssid' maxlength='32'/></div>"
"    <div class='col'><label id='lbl_sta_pass' for='sta_pass'></label><input id='sta_pass' maxlength='64'/></div>"
"   </div>"
"   <div class='btns'>"
"    <button id='btn_save' onclick='saveNetwork()'></button>"
"    <button id='btn_clear_sta' onclick='clearSta()'></button>"
"    <button id='btn_apply' onclick='applyNow()'></button>"
"   </div>"
"   <p id='net_msg' class='hint'></p>"
"  </section>"
"  <p class='footer' id='footer'></p>"
" </section>"
"</div>"
"<script>"
"const I18N={"
" en:{"
"  sub:'Configure network, modules, and LED behavior from one place.',"
"  ws_t:'WS2812 Settings',ws_d:'LED strip config and live action',"
"  cfg_t:'Config JSON',cfg_d:'Raw project configuration',"
"  mod_t:'Modules Status',mod_d:'Current state and telemetry',"
"  ap_t:'Apply Config',ap_d:'POST /api/apply',"
"  net_t:'Network Settings',"
"  net_h:'Set AP/STA credentials here. To use router connection, fill STA SSID and password.',"
"  host:'Hostname',ap_ssid:'AP SSID (base)',ap_pass:'AP Password',"
"  sta_ssid:'STA SSID (router)',sta_pass:'STA Password (router)',"
"  save:'Save Network',clear_sta:'Clear STA',apply:'Apply',"
"  foot:'Tip: open this page via captive portal or device IP.',"
"  mode_sta:'Mode: Station',mode_ap:'Mode: Access Point',mode_un:'Mode: unavailable',"
"  chip_ap:'AP SSID: ',chip_sta:'STA SSID: ',not_set:'not set',"
"  save_ok:'Saved and applied.',save_fail:'Save failed: ',apply_status:'Apply status: ',"
"  matter:'Matter in this firmware is currently a stub.'"
" },"
" ru:{"
"  sub:'Настройка сети, модулей и светодиодов в одном месте.',"
"  ws_t:'Настройки WS2812',ws_d:'Параметры ленты и действия в реальном времени',"
"  cfg_t:'Конфиг JSON',cfg_d:'Сырой конфиг проекта',"
"  mod_t:'Статус модулей',mod_d:'Текущее состояние и телеметрия',"
"  ap_t:'Применить конфиг',ap_d:'POST /api/apply',"
"  net_t:'Настройки сети',"
"  net_h:'Задайте AP/STA параметры. Для подключения к роутеру заполните STA SSID и пароль.',"
"  host:'Имя хоста',ap_ssid:'SSID точки доступа (база)',ap_pass:'Пароль точки доступа',"
"  sta_ssid:'STA SSID (роутер)',sta_pass:'STA пароль (роутер)',"
"  save:'Сохранить сеть',clear_sta:'Очистить STA',apply:'Применить',"
"  foot:'Подсказка: открывайте страницу через captive portal или IP устройства.',"
"  mode_sta:'Режим: Станция',mode_ap:'Режим: Точка доступа',mode_un:'Режим: недоступен',"
"  chip_ap:'AP SSID: ',chip_sta:'STA SSID: ',not_set:'не задан',"
"  save_ok:'Сохранено и применено.',save_fail:'Ошибка сохранения: ',apply_status:'Статус применения: ',"
"  matter:'Matter в этой прошивке пока заглушка.'"
" }"
"};"
"let lang='en';"
"function tt(k){return (I18N[lang]&&I18N[lang][k])||I18N.en[k]||k;}"
"function setText(id,k){const e=document.getElementById(id); if(e)e.textContent=tt(k);}"
"function applyLang(){"
" document.title='ESP32-C3 Setup';"
" setText('sub','sub');"
" setText('c_ws_t','ws_t'); setText('c_ws_d','ws_d');"
" setText('c_cfg_t','cfg_t'); setText('c_cfg_d','cfg_d');"
" setText('c_mod_t','mod_t'); setText('c_mod_d','mod_d');"
" setText('c_apply_t','ap_t'); setText('c_apply_d','ap_d');"
" setText('net_title','net_t'); setText('net_hint','net_h');"
" setText('lbl_host','host'); setText('lbl_ap_ssid','ap_ssid');"
" setText('lbl_ap_pass','ap_pass'); setText('lbl_sta_ssid','sta_ssid'); setText('lbl_sta_pass','sta_pass');"
" setText('btn_save','save'); setText('btn_clear_sta','clear_sta'); setText('btn_apply','apply');"
" setText('footer','foot');"
"}"
"function msg(t,ok){const e=document.getElementById('net_msg'); e.textContent=t; e.className=ok?'ok':'warn';}"
"let _cfg=null; let _rt=null;"
"async function boot(){"
" try{"
"  lang=localStorage.getItem('ui_lang')||((navigator.language||'en').startsWith('ru')?'ru':'en');"
"  document.getElementById('lang').value=lang;"
"  applyLang();"
"  _cfg=await (await fetch('/api/config')).json();"
"  _rt=await (await fetch('/api/runtime')).json();"
"  const net=_cfg.net||{}; const ap=net.ap||{}; const sta=net.sta||{};"
"  document.getElementById('host').value=net.hostname||'esp32-c3';"
"  document.getElementById('ap_ssid').value=ap.ssid||'ESP32-SETUP';"
"  document.getElementById('ap_pass').value=ap.pass||'12345678';"
"  document.getElementById('sta_ssid').value=sta.ssid||'';"
"  document.getElementById('sta_pass').value=sta.pass||'';"
"  const apSsid=_rt.ap_ssid||(ap.ssid||'ESP32-SETUP');"
"  const staSsid=sta.ssid||tt('not_set');"
"  document.getElementById('chip_ap').textContent=tt('chip_ap')+apSsid;"
"  document.getElementById('chip_sta').textContent=tt('chip_sta')+staSsid;"
"  document.getElementById('chip_mode').textContent=(_rt.mode==='sta')?tt('mode_sta'):tt('mode_ap');"
" }catch(e){"
"  document.getElementById('chip_mode').textContent=tt('mode_un');"
" }"
"}"
"document.getElementById('lang').addEventListener('change',function(){"
" lang=this.value; localStorage.setItem('ui_lang',lang); applyLang(); boot();"
"});"
"async function saveNetwork(){"
" try{"
"  const cfg=await (await fetch('/api/config')).json();"
"  cfg.net=cfg.net||{}; cfg.net.ap=cfg.net.ap||{}; cfg.net.sta=cfg.net.sta||{};"
"  cfg.net.hostname=(document.getElementById('host').value||'esp32-c3').trim();"
"  cfg.net.ap.ssid=(document.getElementById('ap_ssid').value||'ESP32-SETUP').trim();"
"  cfg.net.ap.pass=(document.getElementById('ap_pass').value||'12345678').trim();"
"  cfg.net.sta.ssid=(document.getElementById('sta_ssid').value||'').trim();"
"  cfg.net.sta.pass=(document.getElementById('sta_pass').value||'').trim();"
"  const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});"
"  if(!r.ok){msg(tt('save_fail')+r.status,false); return;}"
"  const a=await fetch('/api/apply',{method:'POST'});"
"  if(!a.ok){msg(tt('apply_status')+a.status,false); return;}"
"  msg(tt('save_ok'),true);"
"  boot();"
" }catch(e){msg(String(e),false);}"
"}"
"function clearSta(){"
" document.getElementById('sta_ssid').value='';"
" document.getElementById('sta_pass').value='';"
" saveNetwork();"
"}"
"async function applyNow(){"
" const r=await fetch('/api/apply',{method:'POST'});"
" alert(tt('apply_status')+r.status);"
"}"
"boot();"
"</script></body></html>";

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

static esp_err_t handle_get_runtime(httpd_req_t *req)
{
    cJSON *rt = cJSON_CreateObject();
    if (!rt) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");

    if (wifi_mgr_is_ap()) {
        cJSON_AddStringToObject(rt, "mode", "ap");
        cJSON_AddStringToObject(rt, "ap_ssid", wifi_mgr_get_ap_ssid());
    } else {
        cJSON_AddStringToObject(rt, "mode", "sta");
        cJSON_AddStringToObject(rt, "ap_ssid", "");
    }

    esp_err_t r = json_send(req, rt, 200);
    cJSON_Delete(rt);
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
    conf.max_uri_handlers = 16;

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

    httpd_uri_t runtime = {.uri="/api/runtime", .method=HTTP_GET, .handler=handle_get_runtime};
    httpd_register_uri_handler(s_server, &runtime);

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
