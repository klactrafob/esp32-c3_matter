idf_component_register(
  SRCS
    "app_watchdog.c"
    "app/app_main.c"
    "app_loop.c"

    "core/cfg_json.c"
    "core/modules.c"
    "core/system_log.c"

    "net/wifi_mgr.c"
    "net/dns_server.c"
    "net/web_server.c"
    "net/mqtt_mgr.c"

    "drivers/reset_btn.c"

    "modules/relay/mod_relay.c"

    "device_state.c"

  INCLUDE_DIRS
    "."

  REQUIRES
    esp_system
    nvs_flash
    esp_wifi
    esp_event
    esp_netif
    esp_timer
    esp_http_server
    app_update
    freertos
    driver
    lwip
    json
    mqtt
    esp_adc
    espressif__aht20
    espressif__bme280
    espressif__ds18b20
    espressif__i2c_bus
    espressif__led_strip
    espressif__onewire_bus
    espressif__sht3x
)
