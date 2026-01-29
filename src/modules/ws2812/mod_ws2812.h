#pragma once
#include "esp_err.h"
#include "cJSON.h"

/**
 * WS2812 module
 *
 * Config path in main config JSON:
 *   modules.ws2812
 *
 * Supported config keys:
 *   enable (bool)
 *   gpio (int)
 *   count (int)              - number of LEDs
 *   color_order (string)     - "RGB","RBG","GRB","GBR","BRG","BGR"
 *   brightness_limit (int)   - 0..100 (%)
 *   transition_ms (int)      - default transition time for changes
 *   frame_ms (int)           - render/update period
 *   power_on_effect (string) - "none" | "fade" | "wipe"
 *   power_off_effect (string)- "none" | "fade" | "wipe"
 *   effect_duration_ms (int) - duration for power on/off effects
 *
 * Actions (/api/modules/ws2812/action):
 *   { "on": true/false }
 *   { "brightness": 0..100 }
 *   { "rgb": [r,g,b] } or { "r":..,"g":..,"b":.. }
 *   optional: { "transition_ms": <override> }
 */
esp_err_t mod_ws2812_apply(const cJSON *cfg);
cJSON *mod_ws2812_status_json(void);
esp_err_t mod_ws2812_action(const cJSON *action, cJSON **out_response);
