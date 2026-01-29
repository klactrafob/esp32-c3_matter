#include "mod_ws2812.h"

#include <string.h>
#include <strings.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "led_strip.h"

static const char *TAG = "mod_ws2812";

typedef enum {
    COLOR_ORDER_RGB = 0,
    COLOR_ORDER_RBG,
    COLOR_ORDER_GRB,
    COLOR_ORDER_GBR,
    COLOR_ORDER_BRG,
    COLOR_ORDER_BGR,
} color_order_t;

typedef enum {
    PWR_EFF_NONE = 0,
    PWR_EFF_FADE,
    PWR_EFF_WIPE,
} power_effect_t;

typedef struct {
    bool           enable;
    int            gpio;
    int            count;

    color_order_t  order;

    int            brightness_limit;   // 0..100
    int            transition_ms;      // default
    int            frame_ms;           // render tick

    power_effect_t power_on_effect;
    power_effect_t power_off_effect;
    int            effect_duration_ms;
} ws_cfg_t;

typedef struct {
    bool on;
    int  brightness; // 0..100
    uint8_t r, g, b;
} ws_state_t;

typedef struct {
    bool active;
    int64_t t0_us;
    int duration_ms;

    // start
    bool on0;
    float br0;
    float r0, g0, b0;

    // target
    bool on1;
    float br1;
    float r1, g1, b1;

    // power effect overlay
    power_effect_t pwr_eff;
} transition_t;

// Globals
static ws_cfg_t s_cfg = {
    .enable = false,
    .gpio = 8,
    .count = 30,
    .order = COLOR_ORDER_GRB,
    .brightness_limit = 100,
    .transition_ms = 300,
    .frame_ms = 20,
    .power_on_effect = PWR_EFF_FADE,
    .power_off_effect = PWR_EFF_FADE,
    .effect_duration_ms = 400,
};

static ws_state_t s_state = {
    .on = false,
    .brightness = 50,
    .r = 255, .g = 255, .b = 255,
};

static ws_state_t s_target = {
    .on = false,
    .brightness = 50,
    .r = 255, .g = 255, .b = 255,
};

static transition_t s_tr = {0};

static led_strip_handle_t s_strip = NULL;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_lock = NULL;

static inline uint8_t clamp_u8(int v) { if (v < 0) return 0; if (v > 255) return 255; return (uint8_t)v; }
static inline int clamp_i(int v, int lo, int hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }

static color_order_t parse_color_order(const char *s)
{
    if (!s) return COLOR_ORDER_GRB;
    if (strcasecmp(s, "RGB") == 0) return COLOR_ORDER_RGB;
    if (strcasecmp(s, "RBG") == 0) return COLOR_ORDER_RBG;
    if (strcasecmp(s, "GRB") == 0) return COLOR_ORDER_GRB;
    if (strcasecmp(s, "GBR") == 0) return COLOR_ORDER_GBR;
    if (strcasecmp(s, "BRG") == 0) return COLOR_ORDER_BRG;
    if (strcasecmp(s, "BGR") == 0) return COLOR_ORDER_BGR;
    return COLOR_ORDER_GRB;
}

static const char *color_order_to_str(color_order_t o)
{
    switch (o) {
        case COLOR_ORDER_RGB: return "RGB";
        case COLOR_ORDER_RBG: return "RBG";
        case COLOR_ORDER_GRB: return "GRB";
        case COLOR_ORDER_GBR: return "GBR";
        case COLOR_ORDER_BRG: return "BRG";
        case COLOR_ORDER_BGR: return "BGR";
        default: return "GRB";
    }
}

static power_effect_t parse_pwr_eff(const char *s)
{
    if (!s) return PWR_EFF_FADE;
    if (strcasecmp(s, "none") == 0) return PWR_EFF_NONE;
    if (strcasecmp(s, "fade") == 0) return PWR_EFF_FADE;
    if (strcasecmp(s, "wipe") == 0) return PWR_EFF_WIPE;
    return PWR_EFF_FADE;
}

static const char *pwr_eff_to_str(power_effect_t e)
{
    switch (e) {
        case PWR_EFF_NONE: return "none";
        case PWR_EFF_FADE: return "fade";
        case PWR_EFF_WIPE: return "wipe";
        default: return "fade";
    }
}

static void map_order(color_order_t o, uint8_t r, uint8_t g, uint8_t b, uint8_t *o0, uint8_t *o1, uint8_t *o2)
{
    // output order: first, second, third byte for strip_set_pixel (it expects RGB arguments)
    // we will reorder by swapping channels before passing to set_pixel.
    // That is: pass (r',g',b') such that strip expects RGB but we emulate order mapping.
    switch (o) {
        case COLOR_ORDER_RGB: *o0=r; *o1=g; *o2=b; break;
        case COLOR_ORDER_RBG: *o0=r; *o1=b; *o2=g; break;
        case COLOR_ORDER_GRB: *o0=g; *o1=r; *o2=b; break;
        case COLOR_ORDER_GBR: *o0=g; *o1=b; *o2=r; break;
        case COLOR_ORDER_BRG: *o0=b; *o1=r; *o2=g; break;
        case COLOR_ORDER_BGR: *o0=b; *o1=g; *o2=r; break;
        default:             *o0=g; *o1=r; *o2=b; break;
    }
}

static esp_err_t strip_deinit_locked(void)
{
    if (s_strip) {
        // best-effort turn off
        for (int i = 0; i < s_cfg.count; i++) {
            led_strip_set_pixel(s_strip, i, 0, 0, 0);
        }
        led_strip_refresh(s_strip);
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    return ESP_OK;
}

static esp_err_t strip_init_locked(void)
{
    // remove old
    strip_deinit_locked();

    led_strip_config_t strip_config = {
        .strip_gpio_num = s_cfg.gpio,
        .max_leds = s_cfg.count,
        // In ESP-IDF's led_strip, GRB is the common WS2812 wire order. We always use GRB at the driver level
        // and remap the (r,g,b) arguments so the transmitted byte sequence matches the user-selected order.
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = 0,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .mem_block_symbols = 0,
        .flags.with_dma = 0,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }

    // clear
    for (int i = 0; i < s_cfg.count; i++) {
        led_strip_set_pixel(s_strip, i, 0, 0, 0);
    }
    led_strip_refresh(s_strip);
    return ESP_OK;
}

static void start_transition_locked(int transition_ms, power_effect_t pwr_eff)
{
    if (transition_ms < 0) transition_ms = 0;

    s_tr.active = (transition_ms > 0) || (pwr_eff != PWR_EFF_NONE);
    s_tr.t0_us = esp_timer_get_time();
    s_tr.duration_ms = transition_ms;

    s_tr.on0 = s_state.on;
    s_tr.br0 = s_state.brightness / 100.0f;
    s_tr.r0  = (float)s_state.r;
    s_tr.g0  = (float)s_state.g;
    s_tr.b0  = (float)s_state.b;

    s_tr.on1 = s_target.on;
    s_tr.br1 = s_target.brightness / 100.0f;
    s_tr.r1  = (float)s_target.r;
    s_tr.g1  = (float)s_target.g;
    s_tr.b1  = (float)s_target.b;

    s_tr.pwr_eff = pwr_eff;

    if (!s_tr.active) {
        // immediate
        s_state = s_target;
    }
}

static void apply_action_locked(const cJSON *action)
{
    // Update target
    const cJSON *on = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "on");
    const cJSON *br = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "brightness");
    const cJSON *rgb = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "rgb");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "r");
    const cJSON *g = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "g");
    const cJSON *b = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "b");
    const cJSON *tr_ms = cJSON_GetObjectItemCaseSensitive((cJSON*)action, "transition_ms");

    bool any = false;

    bool new_on = s_target.on;
    if (cJSON_IsBool(on)) { new_on = cJSON_IsTrue(on); any = true; }

    if (cJSON_IsNumber(br)) {
        s_target.brightness = clamp_i(br->valueint, 0, 100);
        any = true;
    }

    if (cJSON_IsArray(rgb) && cJSON_GetArraySize(rgb) >= 3) {
        const cJSON *jr = cJSON_GetArrayItem(rgb, 0);
        const cJSON *jg = cJSON_GetArrayItem(rgb, 1);
        const cJSON *jb = cJSON_GetArrayItem(rgb, 2);
        if (cJSON_IsNumber(jr) && cJSON_IsNumber(jg) && cJSON_IsNumber(jb)) {
            s_target.r = clamp_u8(jr->valueint);
            s_target.g = clamp_u8(jg->valueint);
            s_target.b = clamp_u8(jb->valueint);
            any = true;
        }
    } else if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b)) {
        s_target.r = clamp_u8(r->valueint);
        s_target.g = clamp_u8(g->valueint);
        s_target.b = clamp_u8(b->valueint);
        any = true;
    }

    if (!any) return;

    // decide transition time
    int tms = s_cfg.transition_ms;
    if (cJSON_IsNumber(tr_ms)) tms = clamp_i(tr_ms->valueint, 0, 60000);

    // on/off effects override transition
    power_effect_t eff = PWR_EFF_NONE;
    if (new_on != s_target.on) {
        // should not happen, but keep consistent
    }
    // determine pwr effect only if changing on state relative to current state
    if (new_on != s_state.on) {
        eff = new_on ? s_cfg.power_on_effect : s_cfg.power_off_effect;
        if (s_cfg.effect_duration_ms > 0) tms = s_cfg.effect_duration_ms;
    }

    s_target.on = new_on;
    start_transition_locked(tms, eff);
}

static void render_frame_locked(void)
{
    if (!s_strip || !s_cfg.enable) return;

    // derive current values
    bool on = s_state.on;
    float br = s_state.brightness / 100.0f;
    float limit = s_cfg.brightness_limit / 100.0f;
    if (limit < 0) limit = 0;
    if (limit > 1) limit = 1;
    float eff_br = br * limit;

    uint8_t rr=0, gg=0, bb=0;

    if (on && eff_br > 0.0001f) {
        rr = clamp_u8((int)lroundf(s_state.r * eff_br));
        gg = clamp_u8((int)lroundf(s_state.g * eff_br));
        bb = clamp_u8((int)lroundf(s_state.b * eff_br));
    }

    if (s_tr.active && s_tr.pwr_eff == PWR_EFF_WIPE) {
        // Wipe: use progress to light first k pixels
        int64_t now = esp_timer_get_time();
        float t = 1.0f;
        if (s_tr.duration_ms > 0) {
            t = (float)((now - s_tr.t0_us) / 1000.0f) / (float)s_tr.duration_ms;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
        }
        int k = (int)lroundf(t * (float)s_cfg.count);
        if (!s_tr.on1) {
            // turning off: reverse
            k = s_cfg.count - k;
        }
        if (k < 0) k = 0;
        if (k > s_cfg.count) k = s_cfg.count;

        uint8_t o0,o1,o2;
        map_order(s_cfg.order, rr, gg, bb, &o0,&o1,&o2);

        // led_strip is configured for GRB; it transmits bytes in order (G,R,B).
        // Here o0..o2 represent the desired transmitted order, so we pass (R=o1, G=o0, B=o2).
        for (int i = 0; i < s_cfg.count; i++) {
            if (i < k) led_strip_set_pixel(s_strip, i, o1, o0, o2);
            else       led_strip_set_pixel(s_strip, i, 0, 0, 0);
        }
        led_strip_refresh(s_strip);
        return;
    }

    uint8_t o0,o1,o2;
    map_order(s_cfg.order, rr, gg, bb, &o0,&o1,&o2);

    for (int i = 0; i < s_cfg.count; i++) {
        led_strip_set_pixel(s_strip, i, o1, o0, o2);
    }
    led_strip_refresh(s_strip);
}

static void update_transition_locked(void)
{
    if (!s_tr.active) return;

    int64_t now = esp_timer_get_time();
    float t = 1.0f;
    if (s_tr.duration_ms > 0) {
        t = (float)((now - s_tr.t0_us) / 1000.0f) / (float)s_tr.duration_ms;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
    }

    // for power effects, treat wipe separately in render; but state interpolation still used
    if (s_tr.pwr_eff == PWR_EFF_FADE) {
        // fade: force brightness ramp; keep color moving too
        float br = s_tr.br0 + (s_tr.br1 - s_tr.br0) * t;
        s_state.brightness = clamp_i((int)lroundf(br * 100.0f), 0, 100);
    } else {
        float br = s_tr.br0 + (s_tr.br1 - s_tr.br0) * t;
        s_state.brightness = clamp_i((int)lroundf(br * 100.0f), 0, 100);
    }

    s_state.r = clamp_u8((int)lroundf(s_tr.r0 + (s_tr.r1 - s_tr.r0) * t));
    s_state.g = clamp_u8((int)lroundf(s_tr.g0 + (s_tr.g1 - s_tr.g0) * t));
    s_state.b = clamp_u8((int)lroundf(s_tr.b0 + (s_tr.b1 - s_tr.b0) * t));

    // on flag: switch at start for turning on, at end for turning off to avoid sudden black with fade
    if (s_tr.on1) {
        s_state.on = true;
    } else {
        // turning off
        if (t >= 0.999f) s_state.on = false;
    }

    if (t >= 0.999f) {
        s_state = s_target;
        s_tr.active = false;
        s_tr.pwr_eff = PWR_EFF_NONE;
    }
}

static void ws_task(void *arg)
{
    (void)arg;

    while (1) {
        if (!s_lock) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);

        int frame_ms = s_cfg.frame_ms;
        if (frame_ms < 10) frame_ms = 10;
        if (frame_ms > 200) frame_ms = 200;

        update_transition_locked();
        render_frame_locked();

        xSemaphoreGive(s_lock);

        vTaskDelay(pdMS_TO_TICKS(frame_ms));
    }
}

static void ensure_task_locked(void)
{
    if (s_task) return;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xTaskCreate(ws_task, "ws2812", 4096, NULL, 5, &s_task);
}

static void stop_task_locked(void)
{
    if (s_task) {
        TaskHandle_t t = s_task;
        s_task = NULL;
        vTaskDelete(t);
    }
}

static esp_err_t parse_cfg(const cJSON *cfg, ws_cfg_t *out)
{
    *out = s_cfg;

    if (!cJSON_IsObject(cfg)) return ESP_OK;

    const cJSON *en = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "enable");
    const cJSON *gp = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "gpio");
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "count");
    const cJSON *co = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "color_order");
    const cJSON *bl = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "brightness_limit");
    const cJSON *tr = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "transition_ms");
    const cJSON *fm = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "frame_ms");
    const cJSON *pon = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "power_on_effect");
    const cJSON *poff = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "power_off_effect");
    const cJSON *ed = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "effect_duration_ms");

    if (cJSON_IsBool(en)) out->enable = cJSON_IsTrue(en);
    if (cJSON_IsNumber(gp)) out->gpio = gp->valueint;
    if (cJSON_IsNumber(ct)) out->count = ct->valueint;
    if (cJSON_IsString(co) && co->valuestring) out->order = parse_color_order(co->valuestring);
    if (cJSON_IsNumber(bl)) out->brightness_limit = bl->valueint;
    if (cJSON_IsNumber(tr)) out->transition_ms = tr->valueint;
    if (cJSON_IsNumber(fm)) out->frame_ms = fm->valueint;
    if (cJSON_IsString(pon) && pon->valuestring) out->power_on_effect = parse_pwr_eff(pon->valuestring);
    if (cJSON_IsString(poff) && poff->valuestring) out->power_off_effect = parse_pwr_eff(poff->valuestring);
    if (cJSON_IsNumber(ed)) out->effect_duration_ms = ed->valueint;

    out->count = clamp_i(out->count, 1, 2048);
    out->brightness_limit = clamp_i(out->brightness_limit, 0, 100);
    out->transition_ms = clamp_i(out->transition_ms, 0, 60000);
    out->frame_ms = clamp_i(out->frame_ms, 10, 200);
    out->effect_duration_ms = clamp_i(out->effect_duration_ms, 0, 60000);

    return ESP_OK;
}

esp_err_t mod_ws2812_apply(const cJSON *cfg)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    ws_cfg_t nc;
    parse_cfg(cfg, &nc);

    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool was_enabled = s_cfg.enable;
    bool need_reinit = (!was_enabled && nc.enable)
                    || (was_enabled && nc.enable && (nc.gpio != s_cfg.gpio || nc.count != s_cfg.count))
                    || (nc.enable && s_strip == NULL);

    s_cfg = nc;

    if (!s_cfg.enable) {
        stop_task_locked();
        strip_deinit_locked();
        s_target.on = false;
        s_state.on = false;
        s_tr.active = false;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "disabled");
        return ESP_OK;
    }

    if (need_reinit) {
        esp_err_t err = strip_init_locked();
        if (err != ESP_OK) {
            xSemaphoreGive(s_lock);
            return err;
        }
    }

    // Load initial runtime state from config if present
    if (cJSON_IsObject(cfg)) {
        const cJSON *on = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "on");
        const cJSON *br = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "brightness");
        const cJSON *rgb = cJSON_GetObjectItemCaseSensitive((cJSON*)cfg, "rgb");
        if (cJSON_IsBool(on)) {
            s_target.on = cJSON_IsTrue(on);
            s_state.on = s_target.on;
        }
        if (cJSON_IsNumber(br)) {
            s_target.brightness = clamp_i(br->valueint, 0, 100);
            s_state.brightness = s_target.brightness;
        }
        if (cJSON_IsArray(rgb) && cJSON_GetArraySize(rgb) >= 3) {
            const cJSON *jr = cJSON_GetArrayItem(rgb, 0);
            const cJSON *jg = cJSON_GetArrayItem(rgb, 1);
            const cJSON *jb = cJSON_GetArrayItem(rgb, 2);
            if (cJSON_IsNumber(jr) && cJSON_IsNumber(jg) && cJSON_IsNumber(jb)) {
                s_target.r = clamp_u8(jr->valueint);
                s_target.g = clamp_u8(jg->valueint);
                s_target.b = clamp_u8(jb->valueint);
                s_state.r = s_target.r;
                s_state.g = s_target.g;
                s_state.b = s_target.b;
            }
        }
    }
    ensure_task_locked();

    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "enabled gpio=%d count=%d order=%s br_limit=%d%% tr=%dms frame=%dms on_eff=%s off_eff=%s eff_dur=%dms",
             s_cfg.gpio, s_cfg.count, color_order_to_str(s_cfg.order), s_cfg.brightness_limit,
             s_cfg.transition_ms, s_cfg.frame_ms, pwr_eff_to_str(s_cfg.power_on_effect),
             pwr_eff_to_str(s_cfg.power_off_effect), s_cfg.effect_duration_ms);

    return ESP_OK;
}

cJSON *mod_ws2812_status_json(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", s_cfg.enable);
    cJSON_AddNumberToObject(o, "gpio", s_cfg.gpio);
    cJSON_AddNumberToObject(o, "count", s_cfg.count);
    cJSON_AddStringToObject(o, "color_order", color_order_to_str(s_cfg.order));
    cJSON_AddNumberToObject(o, "brightness_limit", s_cfg.brightness_limit);
    cJSON_AddNumberToObject(o, "transition_ms", s_cfg.transition_ms);
    cJSON_AddNumberToObject(o, "frame_ms", s_cfg.frame_ms);
    cJSON_AddStringToObject(o, "power_on_effect", pwr_eff_to_str(s_cfg.power_on_effect));
    cJSON_AddStringToObject(o, "power_off_effect", pwr_eff_to_str(s_cfg.power_off_effect));
    cJSON_AddNumberToObject(o, "effect_duration_ms", s_cfg.effect_duration_ms);

    cJSON *st = cJSON_AddObjectToObject(o, "state");
    cJSON_AddBoolToObject(st, "on", s_state.on);
    cJSON_AddNumberToObject(st, "brightness", s_state.brightness);
    cJSON *rgb = cJSON_AddArrayToObject(st, "rgb");
    cJSON_AddItemToArray(rgb, cJSON_CreateNumber(s_state.r));
    cJSON_AddItemToArray(rgb, cJSON_CreateNumber(s_state.g));
    cJSON_AddItemToArray(rgb, cJSON_CreateNumber(s_state.b));

    cJSON_AddBoolToObject(o, "transition_active", s_tr.active);

    xSemaphoreGive(s_lock);
    return o;
}

esp_err_t mod_ws2812_action(const cJSON *action, cJSON **out_response)
{
    if (!action || !out_response) return ESP_ERR_INVALID_ARG;
    if (!s_cfg.enable) return ESP_ERR_INVALID_STATE;

    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    apply_action_locked(action);
    xSemaphoreGive(s_lock);

    *out_response = mod_ws2812_status_json();
    return (*out_response) ? ESP_OK : ESP_ERR_NO_MEM;
}
