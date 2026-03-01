idf_component_register(
  SRCS
    "app/app_main.c"
    "app_loop.c"

    "core/cfg_json.c"
    "core/modules.c"

    "net/wifi_mgr.c"
    "net/dns_server.c"
    "net/web_server.c"

    "drivers/reset_btn.c"

    "modules/relay/mod_relay.c"
    "modules/pwm/mod_pwm.c"
    "modules/ws2812/mod_ws2812.c"

    "matter_mgr.cpp"
    "device_state.c"

  INCLUDE_DIRS
    "."

  REQUIRES
    nvs_flash
    esp_wifi
    esp_event
    esp_netif
    esp_timer
    esp_http_server
    freertos
    driver
    lwip
    json
)

# Keep esp_matter build stable across toolchain updates without patching managed_components.
if(TARGET __idf_espressif__esp_matter)
  target_include_directories(
    __idf_espressif__esp_matter
    BEFORE PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/third_party_overrides"
  )
endif()
