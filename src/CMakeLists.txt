idf_component_register(
  SRCS
    "app/app_main.c"

    "core/cfg_json.c"
    "core/modules.c"

    "net/wifi_mgr.c"
    "net/dns_server.c"
    "net/web_server.c"

    "drivers/reset_btn.c"

    "modules/relay/mod_relay.c"
    "modules/pwm/mod_pwm.c"
    "modules/ws2812/mod_ws2812.c"

    "matter_mgr.c"
    "device_state.c"

  INCLUDE_DIRS
    "."

  REQUIRES
    nvs_flash
    esp_wifi
    esp_event
    esp_netif
    esp_http_server
    freertos
    driver
    lwip
    json
)
