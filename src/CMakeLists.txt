idf_component_register(
  SRCS
    "app_main.c"
    "wifi_mgr.c"
    "dns_server.c"
    "web_server.c"
    "reset_btn.c"

    "cfg_json.c"
    "modules.c"

    "mod_relay.c"
    "mod_pwm.c"
    "mod_ws2812.c"

    "matter_mgr.c"
    "device_state.c"
  INCLUDE_DIRS "."
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
