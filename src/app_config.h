#pragma once

#ifndef APP_ENABLE_MATTER
#define APP_ENABLE_MATTER 0
#endif

#define APP_AP_SSID_DEFAULT   "ESP32-SETUP"
#define APP_AP_PASS_DEFAULT   "12345678"
#define APP_HOSTNAME_DEFAULT  "esp32-c3"

// ===== Factory reset button =====
#define APP_RESET_BTN_GPIO          9        // выбери свободный GPIO на своей плате
#define APP_RESET_BTN_ACTIVE_LEVEL  0        // 0 если кнопка на GND (pull-up), 1 если на 3.3V (pull-down)
#define APP_RESET_HOLD_MS           6000     // удержание 6 секунд
#define APP_RESET_DEBOUNCE_MS       40

// ===== Captive portal =====
#define APP_CAPTIVE_PORTAL_ENABLE   1
